/*
 * Copyright (C) 2026, Soar Qin<soarchin@gmail.com>
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#pragma once

#include <stddef.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zip_package_s zip_package_t;

/* The drive colon belongs to the disk path; the next colon starts the ZIP root. */
static inline const wchar_t *zip_package_root_separator(const wchar_t *path) {
    if (path == NULL || path[0] == L'\0') return NULL;
    return wcschr(path + (path[1] == L':' ? 2 : 0), L':');
}

zip_package_t *zip_package_open(const wchar_t *path, const wchar_t *root);
void zip_package_close(zip_package_t *package);
size_t zip_package_count(const zip_package_t *package);
const wchar_t *zip_package_relative_path(const zip_package_t *package, size_t file);
const wchar_t *zip_package_cache_path(const zip_package_t *package, size_t file);
const wchar_t *zip_package_materialize(zip_package_t *package, size_t file);

#ifdef __cplusplus
}
#endif
