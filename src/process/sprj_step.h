/*
 * Copyright (C) 2026, Soar Qin<soarchin@gmail.com>
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sprj_step_range_s {
    const uint8_t *base;
    size_t size;
} sprj_step_range_t;

typedef struct sprj_step_table_s {
    /* Step function table the constructor installed in the object. */
    void **table;
    /* table[0] and table[1]; the legacy template has no step names. */
    void *first_step;
    void *second_step;
    /* Object offsets recovered from the same constructor match. */
    size_t index_offset;
    size_t table_offset;
} sprj_step_table_t;

/* Recovers the step function table of a legacy `NS_SPRJ::Step<T>` subclass from
 * its RTTI vtable. Dark Souls III and Sekiro build these tables without the step
 * name pairs that fd4_step_find() relies on, so the only anchor is the
 * constructor:
 *
 *     lea  rax, [rip+step_table]
 *     mov  [this+table_offset], rax
 *     lea  rax, [rip+derived_vtable]
 *     mov  [this], rax
 *
 * `text` and `table_ranges` are live mapped section pointers: rip-relative
 * displacements resolve against those addresses, matching
 * ml_find_rip_relative_lea(). Every shipped build observed so far keeps the table
 * in `.data`, but the caller passes each section the table may legitimately live
 * in so that a build placing it elsewhere is not rejected outright. */
bool sprj_step_find_from_vtable(const uint8_t *text, size_t text_size,
                                const void *vtable,
                                const sprj_step_range_t *table_ranges,
                                size_t table_range_count,
                                sprj_step_table_t *out);

#ifdef __cplusplus
}
#endif
