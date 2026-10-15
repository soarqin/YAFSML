/*
 * Copyright (C) 2024, Soar Qin<soarchin@gmail.com>

 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#include "mod.h"

#include "config.h"
#include "log.h"

#include "allocator.h"
#include "common/path.h"

#include "vfs.h"
#include "zip_package.h"

#include <shlwapi.h>
#include <stdint.h>

static wchar_t *game_folder;

static int mod_count = 0;

void mods_init(void) {
    game_folder = ml_path_module_filename_alloc(NULL);
    if (game_folder != NULL) ml_path_remove_filename(game_folder);

    vfs_init();
}

void mods_uninit(void) {
    mod_count = 0;
    vfs_uninit();
    ml_mem_free(game_folder);
    game_folder = NULL;
}

void mods_add(const char *name, const wchar_t *path) {
    wchar_t *base_path;
    wchar_t *disk_path;
    wchar_t *package_path;
    const wchar_t *separator;
    size_t length;
    size_t suffix_length;
    DWORD error;
    if (path == NULL || path[0] == L'\0') {
        ML_LOG_ERROR(L"mod", L"empty mod path for %hs", name);
        return;
    }
    separator = zip_package_root_separator(path);
    length = separator == NULL ? wcslen(path) : (size_t)(separator - path);
    if (length == 0 || length >= ML_PATH_MAX_CCH) {
        ML_LOG_ERROR(L"mod", L"invalid mod disk path for %hs", name);
        return;
    }
    disk_path = ml_path_buffer(length + 1);
    if (disk_path == NULL) return;
    memcpy(disk_path, path, length * sizeof(*disk_path));
    disk_path[length] = L'\0';
    base_path = config_full_path_alloc(disk_path);
    error = GetLastError();
    ml_mem_free(disk_path);
    if (base_path == NULL) {
        ML_LOG_ERROR(L"mod", L"cannot resolve mod %hs from `%ls` (error %lu)", name, path, error);
        return;
    }
    if (GetFileAttributesW(base_path) == INVALID_FILE_ATTRIBUTES) {
        ML_LOG_ERROR(L"mod", L"cannot find mod %hs at `%ls` (error %lu)", name, base_path, GetLastError());
        ml_mem_free(base_path);
        return;
    }
    /* Canonicalize only the disk path; / and .. in the suffix belong to the ZIP. */
    length = wcslen(base_path);
    suffix_length = separator == NULL ? 0 : wcslen(separator);
    if (suffix_length > SIZE_MAX / sizeof(*package_path) - length - 1) {
        ml_mem_free(base_path);
        return;
    }
    package_path = ml_mem_alloc(0, (length + suffix_length + 1) * sizeof(*package_path));
    if (package_path == NULL) {
        ml_mem_free(base_path);
        return;
    }
    memcpy(package_path, base_path, length * sizeof(*package_path));
    ml_mem_free(base_path);
    if (suffix_length != 0) memcpy(package_path + length, separator, suffix_length * sizeof(*package_path));
    package_path[length + suffix_length] = L'\0';
    if (!vfs_add_package(package_path)) {
        ML_LOG_ERROR(L"mod", L"cannot load mod %hs from `%ls`", name, package_path);
        ml_mem_free(package_path);
        return;
    }
    mod_count++;
    ML_LOG_INFO(L"mod", L"added mod %hs from `%ls`", name, package_path);
    ml_mem_free(package_path);
}

int mods_count(void) {
    return mod_count;
}

const wchar_t *mods_file_search(const wchar_t *path) {
    return mod_count <= 0 ? NULL : vfs_lookup(path);
}

const wchar_t *mods_file_search_prefixed_domain(const wchar_t *path, int domain) {
    return mod_count <= 0 ? NULL : vfs_lookup_prefixed_domain(path, game_folder, (vfs_lookup_domain_t)domain);
}

const wchar_t *mods_file_virtual_to_uid_prefixed(const wchar_t *path) {
    return mod_count > 0 ? vfs_virtual_to_uid_prefixed(path, game_folder) : NULL;
}

const wchar_t *mods_file_route_read(const wchar_t *path, DWORD desired_access, DWORD creation_disposition) {
    const wchar_t *uid_path;
    if (mod_count <= 0 || path == NULL) return NULL;
    uid_path = vfs_uid_to_path(path);
    if (uid_path != NULL) return uid_path;
    return vfs_route_read_path_prefixed(path, game_folder, desired_access, creation_disposition);
}
