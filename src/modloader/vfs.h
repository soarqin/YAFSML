/*
 * Copyright (C) 2026, Soar Qin<soarchin@gmail.com>
 *
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
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum vfs_lookup_domain_e {
    VFS_LOOKUP_VIRTUAL = 0,
    VFS_LOOKUP_DISK_WIDE,
    VFS_LOOKUP_DISK_ANSI,
    VFS_LOOKUP_WWISE,
    VFS_LOOKUP_DOMAIN_COUNT
} vfs_lookup_domain_t;

void vfs_init(void);
void vfs_uninit(void);
/* Accepts a directory, a ZIP path, or ZIP:path/to/root. ZIP data is streamed. */
bool vfs_add_package(const wchar_t *path);
bool vfs_register_writable_path(const wchar_t *virtual_path, const wchar_t *physical_path);
const wchar_t *vfs_lookup(const wchar_t *path);
const wchar_t *vfs_lookup_domain(const wchar_t *path, vfs_lookup_domain_t domain);
const wchar_t *vfs_lookup_prefixed_domain(const wchar_t *path, const wchar_t *game_root,
                                          vfs_lookup_domain_t domain);
bool vfs_normalize_path(const wchar_t *path, wchar_t **normalized);
size_t vfs_entry_count(void);
bool vfs_has_wwise_entries(void);
uint64_t vfs_generation(void);
void vfs_begin_lookup_reset(void);
uint64_t vfs_reset_lookup_caches(void);
const wchar_t *vfs_virtual_to_uid(const wchar_t *path);
const wchar_t *vfs_virtual_to_uid_prefixed(const wchar_t *path, const wchar_t *game_root);
const wchar_t *vfs_uid_to_path(const wchar_t *uid);
/* Lookups return a disk path for loose files and an opaque UID for ZIP files. */
bool vfs_is_zip_path(const wchar_t *path);
bool vfs_has_zip_entries(void);
typedef struct vfs_stream_s vfs_stream_t;
/* ZIP streams accept a virtual path or ZIP UID. The caller serializes access
 * to each stream; Win32 handles do this in zip_hooks.c. read_at preserves position. */
vfs_stream_t *vfs_stream_open(const wchar_t *path);
void vfs_stream_close(vfs_stream_t *stream);
uint64_t vfs_stream_size(const vfs_stream_t *stream);
uint64_t vfs_stream_identity(const vfs_stream_t *stream);
bool vfs_stream_read(vfs_stream_t *stream, void *buffer, size_t size, size_t *read);
bool vfs_stream_read_at(vfs_stream_t *stream, uint64_t offset, void *buffer, size_t size, size_t *read);
bool vfs_stream_seek(vfs_stream_t *stream, int64_t distance, DWORD origin, uint64_t *position);
const wchar_t *vfs_route_read_path(const wchar_t *path, DWORD desired_access, DWORD creation_disposition);
const wchar_t *vfs_route_read_path_prefixed(const wchar_t *path, const wchar_t *game_root,
                                            DWORD desired_access, DWORD creation_disposition);
const wchar_t *vfs_route_writable_path(const wchar_t *path);
void vfs_recursion_guard_enter(void);
void vfs_recursion_guard_leave(void);
bool vfs_recursion_guard_active(void);

#ifdef __cplusplus
}
#endif
