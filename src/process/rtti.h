/*
 * Copyright (C) 2024,2025, Soar Qin<soarchin@gmail.com>

 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns the lowest-offset vtable slot for class_name, or NULL. */
extern void *rtti_find_vtable(const char *class_name);
extern size_t rtti_vtable_count(const char *class_name);
extern void *rtti_find_vtable_at(const char *class_name, size_t index);

#ifdef ML_RTTI_TEST
/* Test hooks: the production ordered insert and lookups on a private map. */
extern void *rtti_test_map_create(void);
extern void rtti_test_map_destroy(void *map);
extern void rtti_test_insert(void *map, const char *class_name, void *vtable, uint32_t offset);
extern size_t rtti_test_vtable_count(void *map, const char *class_name);
extern void *rtti_test_find_vtable_at(void *map, const char *class_name, size_t index);
#endif

#ifdef __cplusplus
}
#endif
