/*
 * Copyright (C) 2024,2025, Soar Qin<soarchin@gmail.com>

 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#include "rtti.h"

#include "pe.h"
#include "undname.h"

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

typedef struct rtti_complete_object_locator_s {
    uint32_t signature;
    uint32_t offset;
    uint32_t cd_offset;
    uint32_t type_descriptor;
    uint32_t class_descriptor;
    uint32_t self;
} rtti_complete_object_locator_t;

typedef struct rtti_type_descriptor_s {
    uintptr_t vftable;
    uintptr_t spare;
    char name[1];
} rtti_type_descriptor_t;

typedef struct rtti_vtable_entry_s {
    void *vtable;
    uint32_t offset;
    struct rtti_vtable_entry_s *next;
} rtti_vtable_entry_t;

KHASH_MAP_INIT_STR(rtti_vtables, rtti_vtable_entry_t)

static INIT_ONCE rtti_once = INIT_ONCE_STATIC_INIT;
static khash_t(rtti_vtables) *rtti_map = NULL;

static char *local_strdup_a(const char *s) {
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

static bool read_bounded_cstr(const char *s, const char *end, char *out, size_t out_size) {
    size_t len = 0;

    if (s == NULL || end == NULL || out == NULL || out_size == 0 || s >= end) {
        return false;
    }

    while (s + len < end && s[len] != '\0') {
        len++;
    }
    if (s + len >= end || len >= out_size) {
        return false;
    }

    memcpy(out, s, len);
    out[len] = '\0';
    return true;
}

/* Keeps each class's vtables ordered by offset; the map value is the head. */
static void rtti_insert(khash_t(rtti_vtables) *map, const char *class_name, void *vtable, uint32_t offset) {
    khiter_t k;
    int ret;

    if (map == NULL || class_name == NULL || vtable == NULL) {
        return;
    }

    k = kh_put(rtti_vtables, map, class_name, &ret);
    if (ret < 0) {
        return;
    }

    if (ret == 0) {
        rtti_vtable_entry_t *entry = &kh_value(map, k);
        rtti_vtable_entry_t *next;
        for (next = entry; next != NULL; next = next->next) {
            if (next->vtable == vtable) return;
        }
        next = (rtti_vtable_entry_t *)LocalAlloc(0, sizeof(*next));
        if (next == NULL) return;
        next->vtable = vtable;
        next->offset = offset;
        next->next = NULL;
        if (offset < entry->offset) {
            /* Move the old head into the new node and take its place. */
            next->vtable = entry->vtable;
            next->offset = entry->offset;
            next->next = entry->next;
            entry->offset = offset;
            entry->vtable = vtable;
            entry->next = next;
        } else {
            while (entry->next != NULL && entry->next->offset <= offset) entry = entry->next;
            next->next = entry->next;
            entry->next = next;
        }
        return;
    }

    kh_key(map, k) = local_strdup_a(class_name);
    if (kh_key(map, k) == NULL) {
        kh_del(rtti_vtables, map, k);
        return;
    }
    kh_value(map, k).vtable = vtable;
    kh_value(map, k).offset = offset;
    kh_value(map, k).next = NULL;
}

static const rtti_vtable_entry_t *rtti_map_get(khash_t(rtti_vtables) *map, const char *class_name) {
    khiter_t k;

    if (map == NULL || class_name == NULL) {
        return NULL;
    }

    k = kh_get(rtti_vtables, map, class_name);
    return k != kh_end(map) ? &kh_value(map, k) : NULL;
}

static size_t rtti_entry_count(const rtti_vtable_entry_t *entry) {
    size_t count = 0;

    for (; entry != NULL; entry = entry->next) count++;
    return count;
}

static void *rtti_entry_at(const rtti_vtable_entry_t *entry, size_t index) {
    while (entry != NULL && index-- != 0) entry = entry->next;
    return entry != NULL ? entry->vtable : NULL;
}

static void rtti_build_for_image(void *image_base) {
    const IMAGE_SECTION_HEADER *text;
    const IMAGE_SECTION_HEADER *data;
    const IMAGE_SECTION_HEADER *rdata;
    uintptr_t *ptrs;
    size_t rdata_size = 0;
    size_t ptr_count;
    const char *data_end;

    if (image_base == NULL) {
        return;
    }

    text = pe_section_by_name(image_base, ".text");
    data = pe_section_by_name(image_base, ".data");
    rdata = pe_section_by_name(image_base, ".rdata");
    if (text == NULL || data == NULL || rdata == NULL) {
        return;
    }

    ptrs = (uintptr_t *)pe_section_data(image_base, rdata, &rdata_size);
    if (ptrs == NULL || rdata_size < sizeof(uintptr_t) * 2) {
        return;
    }

    ptr_count = rdata_size / sizeof(uintptr_t);
    data_end = (const char *)image_base + pe_section_rva_end(data);

    for (size_t i = 0; i + 1 < ptr_count; i++) {
        const uintptr_t col_va = ptrs[i];
        const uintptr_t first_fn_va = ptrs[i + 1];
        const rtti_complete_object_locator_t *col;
        const rtti_type_descriptor_t *td;
        char mangled[512];
        char demangled[512];

        if (!pe_section_contains_va(image_base, rdata, (const void *)col_va) ||
            !pe_section_contains_va(image_base, text, (const void *)first_fn_va)) {
            continue;
        }

        col = (const rtti_complete_object_locator_t *)col_va;
        if (col->signature != 1 || !pe_section_contains_rva(data, col->type_descriptor)) {
            continue;
        }

        td = (const rtti_type_descriptor_t *)((const uint8_t *)image_base + col->type_descriptor);
        if (!read_bounded_cstr(td->name, data_end, mangled, sizeof(mangled))) {
            continue;
        }

        if (!undname_class(mangled, demangled, sizeof(demangled))) {
            continue;
        }

        rtti_insert(rtti_map, demangled, &ptrs[i + 1], col->offset);
    }
}

static BOOL CALLBACK rtti_init_once(PINIT_ONCE init_once, PVOID parameter, PVOID *context) {
    (void)init_once;
    (void)parameter;
    (void)context;

    rtti_map = kh_init(rtti_vtables);
    if (rtti_map != NULL) {
        rtti_build_for_image(GetModuleHandleW(NULL));
    }

    return TRUE;
}

void *rtti_find_vtable(const char *class_name) {
    return rtti_find_vtable_at(class_name, 0);
}

size_t rtti_vtable_count(const char *class_name) {
    if (class_name == NULL) {
        return 0;
    }

    if (!InitOnceExecuteOnce(&rtti_once, rtti_init_once, NULL, NULL)) {
        return 0;
    }

    return rtti_entry_count(rtti_map_get(rtti_map, class_name));
}

void *rtti_find_vtable_at(const char *class_name, size_t index) {
    if (class_name == NULL) return NULL;
    if (!InitOnceExecuteOnce(&rtti_once, rtti_init_once, NULL, NULL)) return NULL;
    return rtti_entry_at(rtti_map_get(rtti_map, class_name), index);
}

#ifdef ML_RTTI_TEST
void *rtti_test_map_create(void) {
    return kh_init(rtti_vtables);
}

void rtti_test_map_destroy(void *map) {
    khash_t(rtti_vtables) *vtables = map;

    if (vtables == NULL) return;
    for (khiter_t k = kh_begin(vtables); k != kh_end(vtables); ++k) {
        rtti_vtable_entry_t *entry;

        if (!kh_exist(vtables, k)) continue;
        entry = kh_value(vtables, k).next;
        while (entry != NULL) {
            rtti_vtable_entry_t *next = entry->next;
            LocalFree(entry);
            entry = next;
        }
        LocalFree((void *)kh_key(vtables, k));
    }
    kh_destroy(rtti_vtables, vtables);
}

void rtti_test_insert(void *map, const char *class_name, void *vtable, uint32_t offset) {
    rtti_insert(map, class_name, vtable, offset);
}

size_t rtti_test_vtable_count(void *map, const char *class_name) {
    return rtti_entry_count(rtti_map_get(map, class_name));
}

void *rtti_test_find_vtable_at(void *map, const char *class_name, size_t index) {
    return rtti_entry_at(rtti_map_get(map, class_name), index);
}
#endif
