/*
 * Copyright (C) 2024-2026, Soar Qin<soarchin@gmail.com>

 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#pragma once

#include "dl_allocator.h"

#ifdef __cplusplus
extern "C" {
#endif

bool mimalloc_dl_allocator_prepare(size_t heap_size_mb);
dl_allocator_t *mimalloc_dl_allocator(void);

#ifdef ML_MIMALLOC_ALLOCATOR_TEST
void *mimalloc_test_arena_base(void);
size_t mimalloc_test_arena_size(void);
bool mimalloc_test_arena_is_mapped(void);
void *mimalloc_test_mapping_handle(void);
void *mimalloc_test_mapping_file_handle(void);
#endif

#ifdef __cplusplus
}
#endif
