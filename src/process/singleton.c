/*
 * Copyright (C) 2024,2025, Soar Qin<soarchin@gmail.com>

 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#include "singleton.h"

#include "pe.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define kcalloc(N,Z)  LocalAlloc(LPTR, (N)*(Z))
#define kmalloc(Z)    LocalAlloc(0, (Z))
#define krealloc(P,Z) ((P) ? LocalReAlloc((P), (Z), LMEM_MOVEABLE) : LocalAlloc(0, (Z)))
#define kfree(P)      LocalFree(P)
#include "khash.h"

#define SINGLETON_MAX_NAME 512

#define MOV_EDX 0xba
#define CALL 0xe8
#define JNE_SHORT 0x75
#define REX_W 0x48
#define REX_WRXB 0x4f
#define REX_MASK ((uint8_t)~(REX_WRXB ^ REX_W))

static const uint8_t JNE_NEAR[] = { 0x0f, 0x85 };
static const uint8_t LEA_RCX[] = { 0x48, 0x8d, 0x0d };
static const uint8_t LEA_R8[] = { 0x4c, 0x8d, 0x05 };
static const uint8_t LEA_R9[] = { 0x4c, 0x8d, 0x0d };
static const uint8_t MOV_R9[] = { 0x4c, 0x8b, 0xc8 };

typedef const char *(__cdecl *singleton_get_name_fn_t)(const uint8_t *reflection);

typedef enum singleton_instruction_kind_e {
    SINGLETON_INS_LEA_RCX,
    SINGLETON_INS_LEA_R8,
    SINGLETON_INS_LEA_R9,
    SINGLETON_INS_MOV_R9,
    SINGLETON_INS_MOV_EDX
} singleton_instruction_kind_t;

typedef struct singleton_instruction_s {
    singleton_instruction_kind_t kind;
    uint32_t pos;
} singleton_instruction_t;

typedef enum singleton_candidate_kind_e {
    SINGLETON_CANDIDATE_NONE,
    SINGLETON_CANDIDATE_DERIVED,
    SINGLETON_CANDIDATE_FD4
} singleton_candidate_kind_t;

typedef struct singleton_candidate_s {
    singleton_candidate_kind_t kind;
    uint32_t pos;
    uint32_t cap2;
} singleton_candidate_t;

KHASH_MAP_INIT_STR(singleton_slots, void *)
KHASH_MAP_INIT_INT64(singleton_partial, void *)

static INIT_ONCE singleton_once = INIT_ONCE_STATIC_INIT;
static SRWLOCK singleton_fd4_lock = SRWLOCK_INIT;

static khash_t(singleton_slots) *singleton_derived_map = NULL;
static khash_t(singleton_slots) *singleton_fd4_map = NULL;
static khash_t(singleton_partial) *singleton_partial_map = NULL;
static singleton_get_name_fn_t singleton_get_name = NULL;
/* Guarded by singleton_fd4_lock for the finalize path; read on the fast path
 * via Interlocked so the flag and the map it publishes stay in sync. */
static volatile LONG singleton_fd4_finished = 0;

static char *singleton_strdup_a(const char *s) {
    size_t len;
    char *copy;

    if (s == NULL) {
        return NULL;
    }

    len = strlen(s);
    copy = (char *)LocalAlloc(0, len + 1);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, s, len + 1);
    return copy;
}

static bool singleton_name_char_valid(char c) {
    return (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') ||
           c == '_' || c == ':';
}

static bool singleton_read_name(const char *s, const char *end, char *out, size_t out_size) {
    size_t len = 0;

    if (s == NULL || out == NULL || out_size == 0) {
        return false;
    }

    if (end != NULL && s >= end) {
        return false;
    }

    while ((end == NULL || s + len < end) && s[len] != '\0') {
        if (len + 1 >= out_size || !singleton_name_char_valid(s[len])) {
            return false;
        }
        len++;
    }

    if (len == 0 || (end != NULL && s + len >= end)) {
        return false;
    }

    memcpy(out, s, len);
    out[len] = '\0';
    return true;
}

static bool singleton_read_unbounded_name(const char *s, char *out, size_t out_size) {
    size_t len = 0;

    if (s == NULL || out == NULL || out_size == 0) {
        return false;
    }

    while (s[len] != '\0') {
        if (len + 1 >= out_size || !singleton_name_char_valid(s[len])) {
            return false;
        }
        len++;
    }

    if (len == 0) {
        return false;
    }

    memcpy(out, s, len);
    out[len] = '\0';
    return true;
}

static void singleton_named_insert(khash_t(singleton_slots) *map, const char *name, void **slot) {
    khiter_t k;
    int ret;

    if (map == NULL || name == NULL || slot == NULL) {
        return;
    }

    k = kh_put(singleton_slots, map, name, &ret);
    if (ret <= 0) {
        return;
    }

    kh_key(map, k) = singleton_strdup_a(name);
    if (kh_key(map, k) == NULL) {
        kh_del(singleton_slots, map, k);
        return;
    }

    kh_value(map, k) = slot;
}

static void singleton_partial_insert(uintptr_t reflection, void **slot) {
    khiter_t k;
    int ret;

    if (singleton_partial_map == NULL || reflection == 0 || slot == NULL) {
        return;
    }

    k = kh_put(singleton_partial, singleton_partial_map, (khint64_t)reflection, &ret);
    if (ret <= 0) {
        return;
    }

    kh_value(singleton_partial_map, k) = slot;
}

static bool bytes_eq(const uint8_t *text, size_t text_size, uint32_t pos, const uint8_t *pat, size_t pat_size) {
    return pos <= text_size && pat_size <= text_size - pos && memcmp(text + pos, pat, pat_size) == 0;
}

static uint32_t singleton_instruction_pos(singleton_instruction_t ins) {
    return ins.pos;
}

static uint32_t singleton_instruction_next_pos(singleton_instruction_t ins) {
    switch (ins.kind) {
        case SINGLETON_INS_MOV_EDX:
            return ins.pos + 5;
        case SINGLETON_INS_LEA_RCX:
        case SINGLETON_INS_LEA_R8:
        case SINGLETON_INS_LEA_R9:
            return ins.pos + 7;
        case SINGLETON_INS_MOV_R9:
            return ins.pos + 3;
    }
    return ins.pos;
}

static void singleton_insert_instruction(singleton_instruction_t instructions[4], size_t *len,
                                         singleton_instruction_kind_t kind, uint32_t pos) {
    for (size_t i = *len; i > 0; i--) {
        instructions[i] = instructions[i - 1];
    }
    instructions[0].kind = kind;
    instructions[0].pos = pos;
    (*len)++;
}

static bool singleton_push_instruction(singleton_instruction_t instructions[4], size_t *len,
                                       singleton_instruction_kind_t kind, uint32_t pos) {
    if (*len >= 4) {
        return false;
    }
    instructions[*len].kind = kind;
    instructions[*len].pos = pos;
    (*len)++;
    return true;
}

static singleton_candidate_t singleton_candidate_at(const uint8_t *text, size_t text_size, uint32_t pos) {
    singleton_instruction_t instructions[4];
    size_t len = 1;
    uint32_t mask = 0;
    singleton_candidate_t none = { SINGLETON_CANDIDATE_NONE, 0, 0 };

    instructions[0].kind = SINGLETON_INS_MOV_EDX;
    instructions[0].pos = pos;

    while (len < 4) {
        uint32_t next_pos = singleton_instruction_next_pos(instructions[len - 1]);
        if (next_pos > text_size || 3 > text_size - next_pos) {
            return none;
        }

        if (memcmp(text + next_pos, LEA_RCX, sizeof(LEA_RCX)) == 0) {
            if (!singleton_push_instruction(instructions, &len, SINGLETON_INS_LEA_RCX, next_pos)) return none;
        } else if (memcmp(text + next_pos, LEA_R8, sizeof(LEA_R8)) == 0) {
            if (!singleton_push_instruction(instructions, &len, SINGLETON_INS_LEA_R8, next_pos)) return none;
        } else if (memcmp(text + next_pos, LEA_R9, sizeof(LEA_R9)) == 0) {
            if (!singleton_push_instruction(instructions, &len, SINGLETON_INS_LEA_R9, next_pos)) return none;
        } else if (memcmp(text + next_pos, MOV_R9, sizeof(MOV_R9)) == 0) {
            if (!singleton_push_instruction(instructions, &len, SINGLETON_INS_MOV_R9, next_pos)) return none;
        } else if (text[next_pos] == CALL) {
            break;
        } else {
            return none;
        }
    }

    while (len < 4) {
        uint32_t first_pos = singleton_instruction_pos(instructions[0]);
        uint32_t prev_pos;

        if (first_pos < 7) {
            return none;
        }
        prev_pos = first_pos - 7;
        if (prev_pos > text_size || 7 > text_size - prev_pos) {
            return none;
        }

        if (memcmp(text + prev_pos, LEA_RCX, sizeof(LEA_RCX)) == 0) {
            singleton_insert_instruction(instructions, &len, SINGLETON_INS_LEA_RCX, prev_pos);
        } else if (memcmp(text + prev_pos, LEA_R8, sizeof(LEA_R8)) == 0) {
            singleton_insert_instruction(instructions, &len, SINGLETON_INS_LEA_R8, prev_pos);
        } else if (memcmp(text + prev_pos, LEA_R9, sizeof(LEA_R9)) == 0) {
            singleton_insert_instruction(instructions, &len, SINGLETON_INS_LEA_R9, prev_pos);
        } else if (memcmp(text + prev_pos + 4, MOV_R9, sizeof(MOV_R9)) == 0) {
            singleton_insert_instruction(instructions, &len, SINGLETON_INS_MOV_R9, prev_pos + 4);
        } else {
            return none;
        }
    }

    for (size_t i = 0; i < 4; i++) {
        switch (instructions[i].kind) {
            case SINGLETON_INS_LEA_RCX:
                mask |= 1;
                break;
            case SINGLETON_INS_LEA_R8:
                mask |= 2;
                break;
            case SINGLETON_INS_LEA_R9:
                mask |= 4;
                break;
            case SINGLETON_INS_MOV_R9:
                mask |= 8;
                break;
            case SINGLETON_INS_MOV_EDX:
                break;
        }
    }

    if (mask == 7) {
        for (size_t i = 0; i < 4; i++) {
            if (instructions[i].kind == SINGLETON_INS_LEA_R9) {
                singleton_candidate_t candidate = { SINGLETON_CANDIDATE_DERIVED, instructions[0].pos, instructions[i].pos + 3 };
                return candidate;
            }
        }
    } else if (mask == 11) {
        singleton_candidate_t candidate = { SINGLETON_CANDIDATE_FD4, instructions[0].pos, 0 };
        return candidate;
    }

    return none;
}

static bool singleton_cond_jump(const uint8_t *text, size_t text_size, uint32_t pos, uint32_t *out_pos) {
    uint32_t jump_pos;

    if (out_pos == NULL) {
        return false;
    }

    if (pos >= 2) {
        jump_pos = pos - 2;
        if (jump_pos < text_size && text[jump_pos] == JNE_SHORT) {
            *out_pos = jump_pos;
            return true;
        }
    }

    if (pos >= 6) {
        jump_pos = pos - 6;
        if (bytes_eq(text, text_size, jump_pos, JNE_NEAR, sizeof(JNE_NEAR))) {
            *out_pos = jump_pos;
            return true;
        }
    }

    return false;
}

static bool singleton_decode_guarded_mov(const uint8_t *text, size_t text_size, uint32_t pos, uint32_t *out_cap1) {
    const uint8_t *test;
    uint8_t test_rex;
    uint8_t test_modrm;
    uint8_t test_rexb;
    uint8_t test_mod;
    uint8_t test_reg1;
    uint8_t test_reg2;

    if (pos < 3 || out_cap1 == NULL) {
        return false;
    }

    pos -= 3;
    if (pos > text_size || 3 > text_size - pos) {
        return false;
    }

    test = text + pos;
    test_rex = test[0];
    test_modrm = test[2];
    test_rexb = test_rex & 1;
    test_mod = test_modrm & 0xc0;
    test_reg1 = test_modrm & 7;
    test_reg2 = (test_modrm >> 3) & 7;

    if ((test_rex & REX_MASK) != REX_W || test_mod != 0xc0 || test_reg1 != test_reg2) {
        return false;
    }

    for (uint32_t pad = 0; pad <= 3; pad++) {
        uint32_t mov_pos;
        const uint8_t *mov;
        uint8_t mov_rex;
        uint8_t mov_modrm;
        uint8_t mov_rexw;
        uint8_t mov_mod;
        uint8_t mov_mem;
        uint8_t mov_reg;

        if (pos < 7 + pad) {
            continue;
        }
        mov_pos = pos - (7 + pad);
        if (mov_pos > text_size || 7 > text_size - mov_pos) {
            continue;
        }

        mov = text + mov_pos;
        mov_rex = mov[0];
        mov_modrm = mov[2];
        mov_rexw = (mov_rex >> 2) & 1;
        mov_mod = mov_modrm & 0xc0;
        mov_mem = mov_modrm & 7;
        mov_reg = (mov_modrm >> 3) & 7;

        if ((mov_rex & REX_MASK) == REX_W &&
            mov_mod == 0 &&
            mov_mem == 5 &&
            mov_rexw == test_rexb &&
            mov_reg == test_reg1) {
            *out_cap1 = mov_pos + 3;
            return true;
        }
    }

    return false;
}

static bool singleton_derived_caps(const uint8_t *text, size_t text_size, singleton_candidate_t candidate,
                                   uint32_t *out_cap1, uint32_t *out_cap2) {
    uint32_t jump_pos;

    if (candidate.kind != SINGLETON_CANDIDATE_DERIVED || out_cap1 == NULL || out_cap2 == NULL) {
        return false;
    }

    if (candidate.cap2 > text_size || 4 > text_size - candidate.cap2) {
        return false;
    }

    if (!singleton_cond_jump(text, text_size, candidate.pos, &jump_pos)) {
        return false;
    }

    if (!singleton_decode_guarded_mov(text, text_size, jump_pos, out_cap1)) {
        return false;
    }

    *out_cap2 = candidate.cap2;
    return true;
}

static bool singleton_fd4_caps(const uint8_t *text, size_t text_size, singleton_candidate_t candidate,
                               uint32_t *out_cap1, uint32_t *out_cap2, uint32_t *out_cap3) {
    if (candidate.kind != SINGLETON_CANDIDATE_FD4 || out_cap1 == NULL || out_cap2 == NULL || out_cap3 == NULL) {
        return false;
    }

    for (uint32_t pad = 0; pad <= 1; pad++) {
        uint32_t call_pos;
        uint32_t lea_pos;
        uint32_t jump_pos;

        if (candidate.pos < 5 + pad) {
            continue;
        }
        call_pos = candidate.pos - (5 + pad);
        if (call_pos >= text_size || text[call_pos] != CALL || call_pos + 5 > text_size) {
            continue;
        }

        if (call_pos < 7) {
            continue;
        }
        lea_pos = call_pos - 7;
        if (!bytes_eq(text, text_size, lea_pos, LEA_RCX, sizeof(LEA_RCX))) {
            continue;
        }

        if (!singleton_cond_jump(text, text_size, lea_pos, &jump_pos)) {
            continue;
        }

        if (!singleton_decode_guarded_mov(text, text_size, jump_pos, out_cap1)) {
            continue;
        }

        *out_cap2 = lea_pos + 3;
        *out_cap3 = call_pos + 1;
        return true;
    }

    return false;
}

static uintptr_t singleton_rip_target(const uint8_t *text, uint32_t cap) {
    int32_t disp;
    uintptr_t rip;

    memcpy(&disp, text + cap, sizeof(disp));
    rip = (uintptr_t)(text + cap + 4);
    return (uintptr_t)((intptr_t)rip + (intptr_t)disp);
}

static void singleton_record_derived(void *image_base, const uint8_t *text, size_t text_size,
                                     const IMAGE_SECTION_HEADER *data, const IMAGE_SECTION_HEADER *rdata,
                                     const char *rdata_end, singleton_candidate_t candidate) {
    uint32_t cap1;
    uint32_t cap2;
    uintptr_t slot_va;
    uintptr_t name_va;
    char name[SINGLETON_MAX_NAME];

    if (!singleton_derived_caps(text, text_size, candidate, &cap1, &cap2)) {
        return;
    }

    slot_va = singleton_rip_target(text, cap1);
    name_va = singleton_rip_target(text, cap2);

    if ((slot_va & (sizeof(void *) - 1)) != 0 ||
        !pe_section_contains_va(image_base, data, (const void *)slot_va) ||
        !pe_section_contains_va(image_base, rdata, (const void *)name_va)) {
        return;
    }

    if (!singleton_read_name((const char *)name_va, rdata_end, name, sizeof(name))) {
        return;
    }

    singleton_named_insert(singleton_derived_map, name, (void **)slot_va);
}

static void singleton_record_fd4(void *image_base, const uint8_t *text, size_t text_size,
                                 const IMAGE_SECTION_HEADER *text_sh, const IMAGE_SECTION_HEADER *data,
                                 singleton_candidate_t candidate) {
    uint32_t cap1;
    uint32_t cap2;
    uint32_t cap3;
    uintptr_t slot_va;
    uintptr_t reflection_va;
    uintptr_t get_name_va;

    if (!singleton_fd4_caps(text, text_size, candidate, &cap1, &cap2, &cap3)) {
        return;
    }

    slot_va = singleton_rip_target(text, cap1);
    reflection_va = singleton_rip_target(text, cap2);
    get_name_va = singleton_rip_target(text, cap3);

    if ((slot_va & (sizeof(void *) - 1)) != 0 ||
        !pe_section_contains_va(image_base, data, (const void *)slot_va) ||
        !pe_section_contains_va(image_base, text_sh, (const void *)get_name_va)) {
        return;
    }

    if (singleton_get_name == NULL) {
        singleton_get_name = (singleton_get_name_fn_t)get_name_va;
    }

    singleton_partial_insert(reflection_va, (void **)slot_va);
}

/* One pass over .text. singleton_candidate_at is a pure function of
 * (text, text_size, pos), so the derived and fd4 scans used to compute the same
 * result twice for every MOV_EDX; the candidate kind selects which recorder
 * consumes it. */
static void singleton_scan(void *image_base, const uint8_t *text, size_t text_size,
                           const IMAGE_SECTION_HEADER *text_sh, const IMAGE_SECTION_HEADER *data,
                           const IMAGE_SECTION_HEADER *rdata) {
    const char *rdata_end = (const char *)image_base + pe_section_rva_end(rdata);

    for (size_t i = 0; i < text_size; i++) {
        singleton_candidate_t candidate;

        if (text[i] != MOV_EDX) {
            continue;
        }

        candidate = singleton_candidate_at(text, text_size, (uint32_t)i);
        if (candidate.kind == SINGLETON_CANDIDATE_DERIVED) {
            singleton_record_derived(image_base, text, text_size, data, rdata, rdata_end, candidate);
        } else if (candidate.kind == SINGLETON_CANDIDATE_FD4) {
            singleton_record_fd4(image_base, text, text_size, text_sh, data, candidate);
        }
    }
}

static void singleton_build_for_image(void *image_base) {
    const IMAGE_SECTION_HEADER *text_sh;
    const IMAGE_SECTION_HEADER *data;
    const IMAGE_SECTION_HEADER *rdata;
    uint8_t *text;
    size_t text_size = 0;

    if (image_base == NULL) {
        return;
    }

    text_sh = pe_section_by_name(image_base, ".text");
    data = pe_section_by_name(image_base, ".data");
    rdata = pe_section_by_name(image_base, ".rdata");
    if (text_sh == NULL || data == NULL || rdata == NULL) {
        return;
    }

    text = (uint8_t *)pe_section_data(image_base, text_sh, &text_size);
    if (text == NULL || text_size == 0 || text_size > UINT32_MAX) {
        return;
    }

    singleton_scan(image_base, text, text_size, text_sh, data, rdata);
}

static BOOL CALLBACK singleton_init_once(PINIT_ONCE init_once, PVOID parameter, PVOID *context) {
    (void)init_once;
    (void)parameter;
    (void)context;

    singleton_derived_map = kh_init(singleton_slots);
    singleton_partial_map = kh_init(singleton_partial);
    singleton_fd4_map = kh_init(singleton_slots);

    if (singleton_derived_map != NULL && singleton_partial_map != NULL && singleton_fd4_map != NULL) {
        singleton_build_for_image(GetModuleHandleW(NULL));
    }

    return TRUE;
}

static bool singleton_partial_all_null(void) {
    khiter_t k;

    if (singleton_partial_map == NULL) {
        return true;
    }

    for (k = kh_begin(singleton_partial_map); k != kh_end(singleton_partial_map); ++k) {
        if (kh_exist(singleton_partial_map, k)) {
            void **slot = (void **)kh_value(singleton_partial_map, k);
            if (slot != NULL && *slot != NULL) {
                return false;
            }
        }
    }

    return true;
}

static void singleton_try_finish_fd4(void) {
    bool added = false;
    khiter_t k;

    if (InterlockedCompareExchange(&singleton_fd4_finished, 0, 0) != 0 ||
        singleton_get_name == NULL || singleton_partial_all_null()) {
        return;
    }

    AcquireSRWLockExclusive(&singleton_fd4_lock);
    if (InterlockedCompareExchange(&singleton_fd4_finished, 0, 0) != 0) {
        ReleaseSRWLockExclusive(&singleton_fd4_lock);
        return;
    }

    for (k = kh_begin(singleton_partial_map); k != kh_end(singleton_partial_map); ++k) {
        uintptr_t reflection;
        void **slot;
        const char *raw_name;
        char name[SINGLETON_MAX_NAME];

        if (!kh_exist(singleton_partial_map, k)) {
            continue;
        }

        reflection = (uintptr_t)kh_key(singleton_partial_map, k);
        slot = (void **)kh_value(singleton_partial_map, k);
        if (slot == NULL) {
            continue;
        }

        raw_name = singleton_get_name((const uint8_t *)reflection);
        if (!singleton_read_unbounded_name(raw_name, name, sizeof(name))) {
            continue;
        }

        singleton_named_insert(singleton_fd4_map, name, slot);
        added = true;
    }

    /* Only latch the flag once a name actually resolved: an early pass can run
     * before the game has published the reflection names, and the next lookup
     * must retry. Both callers of singleton_find are one-shot startup paths, so
     * the retry cost is bounded. */
    if (added) {
        InterlockedExchange(&singleton_fd4_finished, 1);
    }
    ReleaseSRWLockExclusive(&singleton_fd4_lock);
}

static void **singleton_map_get(khash_t(singleton_slots) *map, const char *name) {
    khiter_t k;

    if (map == NULL || name == NULL) {
        return NULL;
    }

    k = kh_get(singleton_slots, map, name);
    if (k == kh_end(map)) {
        return NULL;
    }

    return (void **)kh_value(map, k);
}

static void **singleton_find_slot(const char *name) {
    if (name == NULL) {
        return NULL;
    }

    if (!InitOnceExecuteOnce(&singleton_once, singleton_init_once, NULL, NULL)) {
        return NULL;
    }

    /* singleton_derived_map is built once inside InitOnce and never mutated
     * afterwards, so it needs no lock. singleton_fd4_map is populated lazily by
     * singleton_try_finish_fd4 under the exclusive lock (kh_put may rehash),
     * so read it under the shared lock. The returned slot points into the
     * game's .data and stays valid after the lock is released. */
    void **slot = singleton_map_get(singleton_derived_map, name);
    if (slot != NULL) {
        return slot;
    }

    singleton_try_finish_fd4();

    AcquireSRWLockShared(&singleton_fd4_lock);
    slot = singleton_map_get(singleton_fd4_map, name);
    ReleaseSRWLockShared(&singleton_fd4_lock);
    return slot;
}

void *singleton_find(const char *name) {
    void **slot = singleton_find_slot(name);
    return slot != NULL ? *slot : NULL;
}
