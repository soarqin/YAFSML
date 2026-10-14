/*
 * Copyright (C) 2026, Soar Qin<soarchin@gmail.com>
 *
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

typedef struct ml_msvc2015_vector_s {
    void *allocator;
    void *first;
    void *last;
    void *end;
} ml_msvc2015_vector_t;

typedef struct ml_msvc2015_string_s {
    void *allocator;
    union { unsigned char inline_storage[16]; void *large; } storage;
    size_t length;
    size_t capacity;
    uint8_t encoding;
} ml_msvc2015_string_t;

_Static_assert(offsetof(ml_msvc2015_vector_t, first) == 8, "MSVC 2015 vector first offset");
_Static_assert(offsetof(ml_msvc2015_string_t, allocator) == 0, "MSVC 2015 string allocator offset");
_Static_assert(offsetof(ml_msvc2015_string_t, encoding) == 40, "MSVC 2015 string encoding offset");

#ifdef __cplusplus
}
#endif
