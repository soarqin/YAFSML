/*
 * Copyright (C) 2024,2025, Soar Qin<soarchin@gmail.com>

 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#include "fd4_step.h"

#include "fd4_step_static.h"
#include "pe.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define kcalloc(N,Z)  LocalAlloc(LPTR, (N)*(Z))
#define kmalloc(Z)    LocalAlloc(0, (Z))
#define krealloc(P,Z) ((P) ? LocalReAlloc((P), (Z), LMEM_MOVEABLE) : LocalAlloc(0, (Z)))
#define kfree(P)      LocalFree(P)
#include "khash.h"
#include "khash_wstr.h"

static INIT_ONCE fd4_step_once = INIT_ONCE_STATIC_INIT;
static SRWLOCK fd4_step_lock = SRWLOCK_INIT;
static khash_t(wstr) *fd4_step_map = NULL;

static bool fd4_step_char_valid(wchar_t c) {
    return (c >= L'A' && c <= L'Z') ||
           (c >= L'a' && c <= L'z') ||
           (c >= L'0' && c <= L'9') ||
           c == L'_' || c == L':';
}

static wchar_t *local_wcsdup_n(const wchar_t *s, size_t len) {
    wchar_t *copy = (wchar_t *)LocalAlloc(0, (len + 1) * sizeof(wchar_t));
    if (copy == NULL) {
        return NULL;
    }

    memcpy(copy, s, len * sizeof(wchar_t));
    copy[len] = L'\0';
    return copy;
}

static bool fd4_step_read_name(const wchar_t *s, const wchar_t *end, size_t *out_len) {
    size_t len = 0;

    if (s == NULL || end == NULL || out_len == NULL || s >= end) {
        return false;
    }

    while (s + len < end && s[len] != L'\0') {
        if (!fd4_step_char_valid(s[len])) {
            return false;
        }
        len++;
    }

    if (len == 0 || s + len >= end) {
        return false;
    }

    *out_len = len;
    return true;
}

static void fd4_step_insert(const wchar_t *name, size_t name_len, void **slot) {
    wchar_t *key;
    khiter_t k;
    int ret;

    if (fd4_step_map == NULL || name == NULL || slot == NULL) {
        return;
    }

    k = kh_put(wstr, fd4_step_map, name, &ret);
    if (ret <= 0) {
        return;
    }

    key = local_wcsdup_n(name, name_len);
    if (key == NULL) {
        kh_del(wstr, fd4_step_map, k);
        return;
    }

    kh_key(fd4_step_map, k) = key;
    kh_value(fd4_step_map, k) = slot;
}

static void fd4_step_build_for_image(void *image_base) {
    const IMAGE_SECTION_HEADER *data;
    const IMAGE_SECTION_HEADER *rdata;
    uintptr_t *ptrs;
    size_t data_size = 0;
    size_t ptr_count;
    const wchar_t *rdata_end;

    if (image_base == NULL) {
        return;
    }

    data = pe_section_by_name(image_base, ".data");
    rdata = pe_section_by_name(image_base, ".rdata");
    if (data == NULL || rdata == NULL) {
        return;
    }

    ptrs = (uintptr_t *)pe_section_data(image_base, data, &data_size);
    if (ptrs == NULL || data_size < sizeof(uintptr_t) * 2) {
        return;
    }

    ptr_count = data_size / sizeof(uintptr_t);
    rdata_end = (const wchar_t *)((const uint8_t *)image_base + pe_section_rva_end(rdata));

    for (size_t i = 0; i + 1 < ptr_count; i++) {
        const uintptr_t name_src_va = ptrs[i + 1];
        const wchar_t *name;
        size_t name_len = 0;

        if ((name_src_va & (sizeof(uintptr_t) - 1)) != 0 ||
            !pe_section_contains_va(image_base, rdata, (const void *)name_src_va)) {
            continue;
        }

        name = (const wchar_t *)name_src_va;
        if (!fd4_step_read_name(name, rdata_end, &name_len)) {
            continue;
        }

        fd4_step_insert(name, name_len, (void **)&ptrs[i]);
    }
}

static bool fd4_step_find_static(void *image_base, const wchar_t *step_name,
                                 fd4_step_static_result_t *result) {
    const IMAGE_SECTION_HEADER *text;
    const IMAGE_SECTION_HEADER *data;
    const IMAGE_SECTION_HEADER *rdata;
    fd4_step_static_sections_t sections = { 0 };

    if (image_base == NULL || step_name == NULL || result == NULL) return false;
    text = pe_section_by_name(image_base, ".text");
    data = pe_section_by_name(image_base, ".data");
    rdata = pe_section_by_name(image_base, ".rdata");
    if (text == NULL || data == NULL || rdata == NULL) return false;

    sections.text = pe_section_data(image_base, text, &sections.text_size);
    sections.text_va = (uintptr_t)sections.text;
    sections.data_va = (uintptr_t)pe_section_data(image_base, data, &sections.data_size);
    sections.rdata = pe_section_data(image_base, rdata, &sections.rdata_size);
    sections.rdata_va = (uintptr_t)sections.rdata;
    return fd4_step_static_find(&sections, step_name, result);
}

static BOOL CALLBACK fd4_step_init_once(PINIT_ONCE init_once, PVOID parameter, PVOID *context) {
    (void)init_once;
    (void)parameter;
    (void)context;

    fd4_step_map = kh_init(wstr);
    if (fd4_step_map != NULL) {
        fd4_step_build_for_image(GetModuleHandleW(NULL));
    }

    return TRUE;
}

static void fd4_step_clear_keys(void) {
    khiter_t k;

    if (fd4_step_map == NULL) {
        return;
    }

    for (k = kh_begin(fd4_step_map); k != kh_end(fd4_step_map); ++k) {
        if (kh_exist(fd4_step_map, k)) {
            LocalFree((void *)kh_key(fd4_step_map, k));
        }
    }
    kh_clear(wstr, fd4_step_map);
}

static void fd4_step_rebuild_locked(void) {
    fd4_step_clear_keys();
    fd4_step_build_for_image(GetModuleHandleW(NULL));
}

static void fd4_step_rebuild_if_empty(void) {
    if (fd4_step_map == NULL || kh_size(fd4_step_map) != 0) {
        return;
    }

    AcquireSRWLockExclusive(&fd4_step_lock);
    if (kh_size(fd4_step_map) == 0) {
        fd4_step_rebuild_locked();
    }
    ReleaseSRWLockExclusive(&fd4_step_lock);
}

static void **fd4_step_lookup_unlocked(const wchar_t *step_name) {
    khiter_t k = kh_get(wstr, fd4_step_map, step_name);
    if (k == kh_end(fd4_step_map)) {
        return NULL;
    }
    return (void **)kh_value(fd4_step_map, k);
}

void **fd4_step_find_slot(const wchar_t *step_name) {
    void **slot;

    if (step_name == NULL) {
        return NULL;
    }

    if (!InitOnceExecuteOnce(&fd4_step_once, fd4_step_init_once, NULL, NULL) || fd4_step_map == NULL) {
        return NULL;
    }

    fd4_step_rebuild_if_empty();

    AcquireSRWLockShared(&fd4_step_lock);
    slot = fd4_step_lookup_unlocked(step_name);
    ReleaseSRWLockShared(&fd4_step_lock);

    if (slot != NULL) {
        return slot;
    }

    AcquireSRWLockExclusive(&fd4_step_lock);
    fd4_step_rebuild_locked();
    slot = fd4_step_lookup_unlocked(step_name);
    ReleaseSRWLockExclusive(&fd4_step_lock);
    if (slot == NULL) {
        fd4_step_static_result_t result;
        if (fd4_step_find_static(GetModuleHandleW(NULL), step_name, &result)) {
            slot = result.slot;
        }
    }
    return slot;
}

void *fd4_step_find(const wchar_t *step_name) {
    void **slot = fd4_step_find_slot(step_name);
    fd4_step_static_result_t result;
    if (slot != NULL && *slot != NULL) return *slot;
    return fd4_step_find_static(GetModuleHandleW(NULL), step_name, &result)
        ? result.step : NULL;
}
