/*
 * Copyright (C) 2026, Soar Qin<soarchin@gmail.com>
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#include "dlrf.h"

#include "pe.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct dlrf_raw_vector_s {
    void *fields[4];
} dlrf_raw_vector_t;

typedef struct dlrf_runtime_class_layout_s {
    void *vtable;
    void *parent;
    void *runtime_ctor;
    dlrf_raw_vector_t methods;
    const char *name;
    const wchar_t *name_wide;
} dlrf_runtime_class_layout_t;

typedef struct dlrf_method_layout_s {
    void *resolver;
    const char *name;
    const wchar_t *name_wide;
    size_t name_len;
} dlrf_method_layout_t;

typedef struct dlrf_resolver_layout_s {
    void *runtime_class;
    const char *name;
    const wchar_t *name_wide;
    dlrf_raw_vector_t invokers;
    dlrf_raw_vector_t parameter_infos;
    void *loose_parameter_info_start;
} dlrf_resolver_layout_t;

typedef struct dlrf_concrete_invoker_layout_s {
    void *vtable;
    void *address;
} dlrf_concrete_invoker_layout_t;

_Static_assert(sizeof(dlrf_raw_vector_t) == 32, "DLRF vector size");
_Static_assert(offsetof(dlrf_runtime_class_layout_t, methods) == 24, "DLRF runtime class methods offset");
_Static_assert(offsetof(dlrf_runtime_class_layout_t, name) == 56, "DLRF runtime class name offset");
_Static_assert(offsetof(dlrf_runtime_class_layout_t, name_wide) == 64, "DLRF runtime class wide name offset");
_Static_assert(sizeof(dlrf_method_layout_t) == 32, "DLRF method entry size");
_Static_assert(offsetof(dlrf_resolver_layout_t, invokers) == 24, "DLRF resolver invokers offset");
_Static_assert(offsetof(dlrf_resolver_layout_t, parameter_infos) == 56, "DLRF resolver parameter infos offset");
_Static_assert(offsetof(dlrf_resolver_layout_t, loose_parameter_info_start) == 88, "DLRF resolver loose parameter info offset");
_Static_assert(sizeof(dlrf_resolver_layout_t) == 96, "DLRF resolver size");
_Static_assert(offsetof(dlrf_concrete_invoker_layout_t, address) == 8, "DLRF invoker address offset");

static INIT_ONCE dlrf_once = INIT_ONCE_STATIC_INIT;
static const dlrf_runtime_class_entry_t *dlrf_registry;
static size_t dlrf_registry_count;

static bool range_contains(dlrf_range_t range, const void *ptr, size_t size) {
    const unsigned char *address = (const unsigned char *)ptr;
    return range.start != NULL && address >= range.start && size <= range.size &&
           (size_t)(address - range.start) <= range.size - size;
}

static bool memory_readable(const void *address, size_t size) {
    const unsigned char *current = (const unsigned char *)address;
    const unsigned char *end;

    if (address == NULL || size == 0 || (uintptr_t)address > UINTPTR_MAX - size) return false;
    end = current + size;
    while (current < end) {
        MEMORY_BASIC_INFORMATION mbi;
        size_t available;
        DWORD protect;

        if (VirtualQuery(current, &mbi, sizeof(mbi)) != sizeof(mbi) || mbi.State != MEM_COMMIT) return false;
        protect = mbi.Protect & 0xff;
        if ((mbi.Protect & PAGE_GUARD) != 0 || protect == PAGE_NOACCESS) return false;
        available = (size_t)((const unsigned char *)mbi.BaseAddress + mbi.RegionSize - current);
        if (available == 0) return false;
        if (available >= (size_t)(end - current)) return true;
        current += available;
    }
    return true;
}

/* True when value holds all of expected, terminator included, in readable memory. */
static bool string_equals(const char *value, const char *expected) {
    size_t size;

    if (value == NULL || expected == NULL) return false;
    size = strlen(expected) + 1;
    return memory_readable(value, size) && memcmp(value, expected, size) == 0;
}

typedef struct dlrf_vector_view_s {
    const unsigned char *first;
    size_t count;
} dlrf_vector_view_t;

/* vector is a local copy of the game's vector; the elements it points to are
   validated here, so callers can read them directly. */
static bool vector_view(const void *vector, dlrf_vector_abi_t abi, size_t element_size,
                        dlrf_vector_view_t *view) {
    void *fields[4];
    uintptr_t first;
    uintptr_t last;
    uintptr_t end;
    size_t bytes;

    if (view == NULL || vector == NULL || element_size == 0) return false;
    memcpy(fields, vector, sizeof(fields));
    if (abi == DLRF_VECTOR_ABI_MSVC2012) {
        first = (uintptr_t)fields[0];
        last = (uintptr_t)fields[1];
        end = (uintptr_t)fields[2];
    } else if (abi == DLRF_VECTOR_ABI_MSVC2015) {
        first = (uintptr_t)fields[1];
        last = (uintptr_t)fields[2];
        end = (uintptr_t)fields[3];
    } else {
        return false;
    }
    if (first == 0 || last == 0 || end == 0) {
        view->first = NULL;
        view->count = 0;
        return first == 0 && last == 0 && end == 0;
    }
    if (first > last || last > end || (last - first) % element_size != 0 ||
        (end - first) % element_size != 0 || (end - first) / element_size > 1024 * 1024) return false;
    bytes = (size_t)(end - first);
    if (!memory_readable((const void *)first, bytes)) return false;
    view->first = (const unsigned char *)first;
    view->count = (size_t)(last - first) / element_size;
    return true;
}

static bool read_class_name(const void *runtime_class, const char **name) {
    const char *value;

    if (name == NULL || !memory_readable(runtime_class, sizeof(dlrf_runtime_class_layout_t))) return false;
    memcpy(&value, (const unsigned char *)runtime_class + offsetof(dlrf_runtime_class_layout_t, name),
           sizeof(value));
    if (value == NULL) return false;
    *name = value;
    return true;
}

size_t dlrf_find_registry(const dlrf_runtime_class_entry_t *entries, size_t count,
                          dlrf_range_t data, dlrf_range_t rdata,
                          const dlrf_runtime_class_entry_t **registry) {
    size_t best_count = 0;
    const dlrf_runtime_class_entry_t *best = NULL;
    size_t run = 0;
    const dlrf_runtime_class_entry_t *start = NULL;
    uintptr_t previous_type_id = 0;

    if (registry != NULL) *registry = NULL;
    for (size_t i = 0; i < count; i++) {
        const dlrf_runtime_class_entry_t *entry = &entries[i];
        void *vtable = NULL;
        bool valid = entry->type_id != NULL && entry->runtime_class != NULL &&
                     (uintptr_t)entry->type_id > previous_type_id &&
                     range_contains(data, entry->type_id, 1) && *(const unsigned char *)entry->type_id == 0 &&
                     range_contains(data, entry->runtime_class, sizeof(void *));
        if (valid) {
            memcpy(&vtable, entry->runtime_class, sizeof(vtable));
            valid = range_contains(rdata, vtable, sizeof(void *));
        }
        if (valid) {
            if (run == 0) start = entry;
            run++;
            previous_type_id = (uintptr_t)entry->type_id;
            if (run > best_count) {
                best = start;
                best_count = run;
            }
        } else {
            run = 0;
            previous_type_id = 0;
        }
    }
    if (registry != NULL) *registry = best;
    return best_count;
}

static BOOL CALLBACK dlrf_init_once(PINIT_ONCE once, PVOID parameter, PVOID *context) {
    void *image = GetModuleHandleW(NULL);
    const IMAGE_SECTION_HEADER *data;
    const IMAGE_SECTION_HEADER *rdata;
    size_t data_size;
    size_t rdata_size;
    dlrf_range_t data_range;
    dlrf_range_t rdata_range;
    (void)once;
    (void)parameter;
    (void)context;

    data = pe_section_by_name(image, ".data");
    rdata = pe_section_by_name(image, ".rdata");
    data_range.start = pe_section_data(image, data, &data_size);
    rdata_range.start = pe_section_data(image, rdata, &rdata_size);
    data_range.size = data_size;
    rdata_range.size = rdata_size;
    if (data_range.start != NULL && rdata_range.start != NULL) {
        size_t count = data_size / sizeof(dlrf_runtime_class_entry_t);
        size_t found = dlrf_find_registry((const dlrf_runtime_class_entry_t *)data_range.start, count,
                                          data_range, rdata_range, &dlrf_registry);
        if (found >= 512) dlrf_registry_count = found;
        else dlrf_registry = NULL;
    }
    return TRUE;
}

const dlrf_runtime_class_entry_t *dlrf_runtime_classes(size_t *count) {
    InitOnceExecuteOnce(&dlrf_once, dlrf_init_once, NULL, NULL);
    if (count != NULL) *count = dlrf_registry_count;
    return dlrf_registry;
}

const void *dlrf_find_runtime_class(const dlrf_runtime_class_entry_t *registry, size_t count,
                                    const char *class_name) {
    if (registry == NULL || class_name == NULL || count == 0 || count > SIZE_MAX / sizeof(*registry) ||
        !memory_readable(registry, count * sizeof(*registry))) return NULL;
    for (size_t i = 0; i < count; i++) {
        const char *name;
        if (registry[i].runtime_class != NULL && read_class_name(registry[i].runtime_class, &name) &&
            string_equals(name, class_name)) return registry[i].runtime_class;
    }
    return NULL;
}

bool dlrf_find_method(const void *runtime_class, dlrf_vector_abi_t vector_abi,
                      const char *method_name, dlrf_method_info_t *info) {
    dlrf_runtime_class_layout_t class_layout;
    dlrf_vector_view_t methods;

    if (info != NULL) memset(info, 0, sizeof(*info));
    if (runtime_class == NULL || method_name == NULL || info == NULL ||
        !memory_readable(runtime_class, sizeof(class_layout))) return false;
    memcpy(&class_layout, runtime_class, sizeof(class_layout));
    if (!vector_view(&class_layout.methods, vector_abi, sizeof(dlrf_method_layout_t), &methods)) return false;
    for (size_t i = 0; i < methods.count; i++) {
        dlrf_method_layout_t method;
        const char *name;
        dlrf_resolver_layout_t resolver;
        dlrf_vector_view_t invokers;

        memcpy(&method, methods.first + i * sizeof(method), sizeof(method));
        name = method.name;
        if (name == NULL || !string_equals(name, method_name) || method.resolver == NULL ||
            !memory_readable(method.resolver, sizeof(resolver))) continue;
        memcpy(&resolver, method.resolver, sizeof(resolver));
        if (!vector_view(&resolver.invokers, vector_abi, sizeof(void *), &invokers) || invokers.count == 0) continue;
        for (size_t j = 0; j < invokers.count; j++) {
            const void *invoker;
            dlrf_concrete_invoker_layout_t concrete;
            memcpy(&invoker, invokers.first + j * sizeof(void *), sizeof(invoker));
            if (invoker == NULL || !memory_readable(invoker, sizeof(concrete))) continue;
            memcpy(&concrete, invoker, sizeof(concrete));
            if (concrete.address == NULL) continue;
            info->resolver = method.resolver;
            info->invoker = invoker;
            info->address = concrete.address;
            info->invoker_count = invokers.count;
            return true;
        }
    }
    return false;
}
