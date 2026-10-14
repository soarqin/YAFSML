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

typedef struct ml_msvc2012_vector_s {
    void *first;
    void *last;
    void *end;
    void *allocator;
} ml_msvc2012_vector_t;

typedef struct ml_msvc2012_string_s {
    union { unsigned char inline_storage[16]; void *large; } storage;
    size_t length;
    size_t capacity;
    void *allocator;
    uint8_t encoding;
} ml_msvc2012_string_t;

_Static_assert(offsetof(ml_msvc2012_vector_t, allocator) == 24, "MSVC 2012 vector allocator offset");
_Static_assert(offsetof(ml_msvc2012_string_t, allocator) == 32, "MSVC 2012 string allocator offset");
_Static_assert(offsetof(ml_msvc2012_string_t, encoding) == 40, "MSVC 2012 string encoding offset");

#ifdef __cplusplus
}
#endif
