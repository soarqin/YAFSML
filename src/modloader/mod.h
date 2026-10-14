/*
 * Copyright (C) 2024, Soar Qin<soarchin@gmail.com>

 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <stdbool.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

extern void mods_init(void);
extern void mods_uninit(void);
extern void mods_add(const char *name, const wchar_t *path);
extern int mods_count(void);
extern const wchar_t *mods_file_search(const wchar_t *path);
extern const wchar_t *mods_file_search_prefixed_domain(const wchar_t *path, int domain);
extern const wchar_t *mods_file_virtual_to_uid_prefixed(const wchar_t *path);
extern const wchar_t *mods_file_route_read(const wchar_t *path, DWORD desired_access, DWORD creation_disposition);

#ifdef __cplusplus
}
#endif
