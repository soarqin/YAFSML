/*
 * Copyright (C) 2026, Soar Qin<soarchin@gmail.com>
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#pragma once

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dlrf_runtime_class_entry_s {
    const void *type_id;
    void *runtime_class;
} dlrf_runtime_class_entry_t;

typedef struct dlrf_range_s {
    const unsigned char *start;
    size_t size;
} dlrf_range_t;

typedef enum dlrf_vector_abi_e {
    DLRF_VECTOR_ABI_MSVC2012,
    DLRF_VECTOR_ABI_MSVC2015,
} dlrf_vector_abi_t;

typedef struct dlrf_method_info_s {
    const void *resolver;
    const void *invoker;
    void *address;
    size_t invoker_count;
} dlrf_method_info_t;

const dlrf_runtime_class_entry_t *dlrf_runtime_classes(size_t *count);
size_t dlrf_find_registry(const dlrf_runtime_class_entry_t *entries, size_t count,
                          dlrf_range_t data, dlrf_range_t rdata,
                          const dlrf_runtime_class_entry_t **registry);

const void *dlrf_find_runtime_class(const dlrf_runtime_class_entry_t *registry, size_t count,
                                    const char *class_name);
bool dlrf_find_method(const void *runtime_class, dlrf_vector_abi_t vector_abi,
                      const char *method_name, dlrf_method_info_t *info);

#ifdef __cplusplus
}
#endif
