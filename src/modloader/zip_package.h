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
#include <stdint.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zip_package_s zip_package_t;
typedef struct zip_stream_s zip_stream_t;

/* The drive colon belongs to the disk path; the next colon starts the ZIP root. */
static inline const wchar_t *zip_package_root_separator(const wchar_t *path) {
    if (path == NULL || path[0] == L'\0') return NULL;
    if ((path[0] == L'\\' || path[0] == L'/') &&
        (path[1] == L'\\' || path[1] == L'/') && path[2] == L'?' &&
        (path[3] == L'\\' || path[3] == L'/')) path += 4;
    return wcschr(path + (path[0] != L'\0' && path[1] == L':' ? 2 : 0), L':');
}

zip_package_t *zip_package_open(const wchar_t *path, const wchar_t *root);
void zip_package_close(zip_package_t *package);
size_t zip_package_count(const zip_package_t *package);
const wchar_t *zip_package_relative_path(const zip_package_t *package, size_t file);
/* Streams retain their package and use bounded buffers, independent of entry size. */
zip_stream_t *zip_stream_open(zip_package_t *package, size_t file);
void zip_stream_close(zip_stream_t *stream);
uint64_t zip_stream_size(const zip_stream_t *stream);
bool zip_stream_read_at(zip_stream_t *stream, uint64_t offset, void *buffer,
                        size_t size, size_t *read);

#ifdef __cplusplus
}
#endif
