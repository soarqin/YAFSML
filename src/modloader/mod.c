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

#include "vfs.h"
#include "zip_package.h"

#include <shlwapi.h>
#include <stdint.h>

static wchar_t game_folder[MAX_PATH];

static int mod_count = 0;

void mods_init(void) {
    GetModuleFileNameW(NULL, game_folder, MAX_PATH);
    PathRemoveFileSpecW(game_folder);
    PathRemoveBackslashW(game_folder);

    vfs_init();
}

void mods_uninit(void) {
    mod_count = 0;
    vfs_uninit();
}

void mods_add(const char *name, const wchar_t *path) {
    wchar_t base_path[MAX_PATH];
    wchar_t disk_path[MAX_PATH];
    wchar_t *package_path;
    const wchar_t *separator;
    size_t length;
    size_t suffix_length;
    if (path == NULL || path[0] == L'\0') {
        ML_LOG_ERROR(L"mod", L"empty mod path for %hs", name);
        return;
    }
    separator = zip_package_root_separator(path);
    length = separator == NULL ? wcslen(path) : (size_t)(separator - path);
    if (length == 0 || length >= MAX_PATH) {
        ML_LOG_ERROR(L"mod", L"mod path is too long for %hs", name);
        return;
    }
    memcpy(disk_path, path, length * sizeof(*disk_path));
    disk_path[length] = L'\0';
    /* Classify and resolve with one separator; the ZIP subpath keeps its own. */
    for (size_t i = 0; i < length; i++) {
        if (disk_path[i] == L'/') disk_path[i] = L'\\';
    }
    if (disk_path[0] == L'\\' && disk_path[1] != L'\\') {
        config_full_path(base_path, NULL);
        PathStripToRootW(base_path);
        PathAppendW(base_path, disk_path + 1);
    } else if (disk_path[1] == L':' || (disk_path[0] == L'\\' && disk_path[1] == L'\\')) {
        if (!PathCanonicalizeW(base_path, disk_path)) return;
    } else {
        config_full_path(base_path, disk_path);
    }
    if (!PathFileExistsW(base_path)) {
        ML_LOG_ERROR(L"mod", L"cannot find mod %hs at `%ls`", name, base_path);
        return;
    }
    /* Canonicalize only the disk path; / and .. in the suffix belong to the ZIP. */
    length = wcslen(base_path);
    suffix_length = separator == NULL ? 0 : wcslen(separator);
    package_path = ml_mem_alloc(0, (length + suffix_length + 1) * sizeof(*package_path));
    if (package_path == NULL) return;
    memcpy(package_path, base_path, length * sizeof(*package_path));
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
