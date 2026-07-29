/*
 * Copyright (C) 2026, Soar Qin<soarchin@gmail.com>
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#include "sprj_step.h"

#include "rip_scan.h"

#include <string.h>

enum {
    /* `lea rax, [rip+disp32]` */
    SPRJ_LEA_SIZE = 7,
    /* `mov [reg], rax` */
    SPRJ_MOV_BASE_SIZE = 3,
    /* `mov [reg+disp8], reg` */
    SPRJ_MOV_DISP8_SIZE = 4,
    /* The constructor stores the table a few instructions before the derived
       vtable; Dark Souls III needs 21 bytes of reach. */
    SPRJ_CTOR_BACK_WINDOW = 0x40,
    SPRJ_MODRM_REG_RAX = 0,
    SPRJ_MODRM_REG_RCX = 1,
};

static bool is_lea_rax_rip(const uint8_t *code) {
    return code[0] == 0x48 && code[1] == 0x8d && code[2] == 0x05;
}

static const uint8_t *lea_rip_target(const uint8_t *code) {
    int32_t displacement;
    memcpy(&displacement, code + 3, sizeof(displacement));
    return code + SPRJ_LEA_SIZE + (intptr_t)displacement;
}

/* `mov [base], rax` with mod=00: rm 4 would need a SIB byte and rm 5 encodes the
   rip-relative form, so neither can name a base register here. */
static bool is_mov_base_rax(const uint8_t *code, uint8_t *out_base) {
    uint8_t rm;
    if (code[0] != 0x48 || code[1] != 0x89 || (code[2] & 0xf8) != 0x00) return false;
    rm = code[2] & 7;
    if (rm == 4 || rm == 5) return false;
    *out_base = rm;
    return true;
}

/* `mov [base+disp8], reg` with mod=01. */
static bool is_mov_base_disp8(const uint8_t *code, uint8_t reg, uint8_t base,
                              uint8_t *out_displacement) {
    if (code[0] != 0x48 || code[1] != 0x89 ||
        code[2] != (uint8_t)(0x40 | (reg << 3) | base)) return false;
    *out_displacement = code[3];
    return true;
}

static bool range_contains(const uint8_t *base, size_t size, const void *address,
                           size_t needed) {
    const uint8_t *pointer = (const uint8_t *)address;
    return base != NULL && pointer >= base && needed <= size &&
           (size_t)(pointer - base) <= size - needed;
}

static bool ranges_contain(const sprj_step_range_t *ranges, size_t count,
                           const void *address, size_t needed) {
    for (size_t i = 0; i < count; i++) {
        if (range_contains(ranges[i].base, ranges[i].size, address, needed)) return true;
    }
    return false;
}

static bool resolve_candidate(const uint8_t *text, size_t text_size,
                              const uint8_t *vtable_lea,
                              const sprj_step_range_t *table_ranges,
                              size_t table_range_count,
                              sprj_step_table_t *out) {
    size_t lea_offset = (size_t)(vtable_lea - text);
    size_t window;
    void **table = NULL;
    size_t table_offset = 0;
    size_t index_offset = 0;
    uint8_t base;

    if (!is_lea_rax_rip(vtable_lea)) return false;
    if (!range_contains(text, text_size, vtable_lea, SPRJ_LEA_SIZE + SPRJ_MOV_BASE_SIZE)) return false;
    if (!is_mov_base_rax(vtable_lea + SPRJ_LEA_SIZE, &base)) return false;

    window = lea_offset < SPRJ_CTOR_BACK_WINDOW ? lea_offset : SPRJ_CTOR_BACK_WINDOW;

    /* Nearest `lea rax, [rip+table]; mov [base+disp8], rax` before the vtable store. */
    for (size_t back = 1; back <= window; back++) {
        const uint8_t *code = vtable_lea - back;
        uint8_t displacement;
        if (!is_lea_rax_rip(code)) continue;
        if (!range_contains(text, text_size, code, SPRJ_LEA_SIZE + SPRJ_MOV_DISP8_SIZE)) continue;
        if (!is_mov_base_disp8(code + SPRJ_LEA_SIZE, SPRJ_MODRM_REG_RAX, base, &displacement)) continue;
        if (displacement < sizeof(void *)) continue;
        table = (void **)(void *)lea_rip_target(code);
        table_offset = displacement;
        break;
    }
    if (table == NULL) return false;

    /* Nearest `mov [base+disp8], rcx`: the constructor clearing the step index.
       Taking the offset from the same match keeps the layout self-consistent. */
    for (size_t back = 1; back <= window; back++) {
        const uint8_t *code = vtable_lea - back;
        uint8_t displacement;
        if (!range_contains(text, text_size, code, SPRJ_MOV_DISP8_SIZE)) continue;
        if (!is_mov_base_disp8(code, SPRJ_MODRM_REG_RCX, base, &displacement)) continue;
        if (displacement < sizeof(void *) || displacement >= table_offset ||
            (displacement & 3) != 0) continue;
        index_offset = displacement;
        break;
    }
    if (index_offset == 0) return false;

    if (((uintptr_t)table & (sizeof(void *) - 1)) != 0) return false;
    if (!ranges_contain(table_ranges, table_range_count, table, 2 * sizeof(void *))) return false;
    if (!range_contains(text, text_size, table[0], 1) ||
        !range_contains(text, text_size, table[1], 1)) return false;

    out->table = table;
    out->first_step = table[0];
    out->second_step = table[1];
    out->index_offset = index_offset;
    out->table_offset = table_offset;
    return true;
}

bool sprj_step_find_from_vtable(const uint8_t *text, size_t text_size,
                               const void *vtable,
                               const sprj_step_range_t *table_ranges,
                               size_t table_range_count,
                               sprj_step_table_t *out) {
    if (text == NULL || vtable == NULL || table_ranges == NULL || out == NULL) return false;
    if (text_size < SPRJ_LEA_SIZE || table_range_count == 0) return false;

    memset(out, 0, sizeof(*out));
    for (size_t offset = 0; offset + SPRJ_LEA_SIZE <= text_size; ) {
        const uint8_t *vtable_lea = ml_find_rip_relative_lea(text + offset, text_size - offset,
                                                            vtable);
        if (vtable_lea == NULL) break;
        if (resolve_candidate(text, text_size, vtable_lea, table_ranges, table_range_count,
                              out)) return true;
        offset = (size_t)(vtable_lea - text) + 1;
    }
    return false;
}
