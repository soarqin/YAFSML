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
#include <wchar.h>

/* `wem/00/` plus the terminating NUL. */
#define WWISE_WEM_BUCKET_SIZE 8

const wchar_t *wwise_strip_prefixes(const wchar_t *path);
size_t wwise_join3(wchar_t *buffer, size_t capacity, const wchar_t *prefix,
                   const wchar_t *middle, const wchar_t *path);
void wwise_wem_bucket(const wchar_t *path, wchar_t bucket[WWISE_WEM_BUCKET_SIZE]);
size_t wwise_find_open_call(const uint8_t *text, size_t size);
