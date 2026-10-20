/* Copyright (C) 2026, Soar Qin. Licensed under the MIT license. */
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>

/* The launcher owns CRT strings; loader callers use the loader allocator.
 * Keep these helpers local to each translation unit so ownership never crosses
 * between the two allocators. */
#ifdef ML_PATH_USE_CRT_ALLOCATOR
#include <stdlib.h>
#define ML_PATH_ALLOC(bytes) malloc(bytes)
#define ML_PATH_FREE(ptr) free(ptr)
#else
#include "allocator.h"
#define ML_PATH_ALLOC(bytes) ml_mem_alloc(0, bytes)
#define ML_PATH_FREE(ptr) ml_mem_free(ptr)
#endif

#define ML_PATH_MAX_CCH 32768u

#ifdef __cplusplus
extern "C" {
#endif

static inline wchar_t *ml_path_buffer(size_t count) {
    wchar_t *result;
    if (count == 0 || count > ML_PATH_MAX_CCH) {
        SetLastError(ERROR_FILENAME_EXCED_RANGE);
        return NULL;
    }
    result = ML_PATH_ALLOC(count * sizeof(*result));
    if (result == NULL) SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return result;
}

static inline wchar_t *ml_path_copy_alloc(const wchar_t *path) {
    size_t length = wcslen(path);
    wchar_t *result = ml_path_buffer(length + 1);
    if (result != NULL) memcpy(result, path, (length + 1) * sizeof(*result));
    return result;
}

static inline bool ml_path_separator(wchar_t ch) {
    return ch == L'\\' || ch == L'/';
}

static inline const wchar_t *ml_path_filename(const wchar_t *path) {
    const wchar_t *filename = path;
    for (; *path != L'\0'; ++path) if (ml_path_separator(*path)) filename = path + 1;
    return filename;
}

static inline const wchar_t *ml_path_extension(const wchar_t *path) {
    const wchar_t *filename = ml_path_filename(path);
    const wchar_t *extension = wcsrchr(filename, L'.');
    return extension == NULL ? filename + wcslen(filename) : extension;
}

/* The DOS drive or UNC part of a disk path. Skips an extended (\\?\) or device
 * (\\.\) prefix in front of a drive or UNC\ name, and the separators that start
 * a UNC name; `unc` tells the two forms apart. Other device names, such as
 * \\?\Volume{...}, are returned unchanged. Either separator is accepted. */
static inline const wchar_t *ml_path_dos_body(const wchar_t *path, bool *unc) {
    *unc = false;
    if (!ml_path_separator(path[0]) || !ml_path_separator(path[1])) return path;
    if ((path[2] == L'?' || path[2] == L'.') && ml_path_separator(path[3])) {
        if (_wcsnicmp(path + 4, L"UNC", 3) == 0 && ml_path_separator(path[7])) {
            *unc = true;
            return path + 8;
        }
        return path[4] != L'\0' && path[5] == L':' && ml_path_separator(path[6]) ? path + 4 : path;
    }
    *unc = true;
    return path + 2;
}

/* Extended and device DOS/UNC names are converted back before resolving . and
 * ..; those navigation elements have no meaning after an extended prefix. */
static inline void ml_path_normalize_input(wchar_t *path) {
    bool unc;
    const wchar_t *body;
    size_t offset;
    for (wchar_t *p = path; *p != L'\0'; ++p) if (*p == L'/') *p = L'\\';
    body = ml_path_dos_body(path, &unc);
    offset = unc ? 2 : 0;
    if (body != path + offset) memmove(path + offset, body, (wcslen(body) + 1) * sizeof(*path));
}

static inline bool ml_path_is_absolute(const wchar_t *path) {
    size_t length = path == NULL ? 0 : wcslen(path);
    return length >= 3 && ((path[1] == L':' && ml_path_separator(path[2])) ||
                          (ml_path_separator(path[0]) && ml_path_separator(path[1])));
}

/* Includes the final separator when one is present; an UNC share itself is a
 * root, so a rooted request cannot discard its server/share components. */
static inline size_t ml_path_root_length(const wchar_t *path) {
    bool unc;
    const wchar_t *p = ml_path_dos_body(path, &unc);
    if (!unc) return p[0] != L'\0' && p[1] == L':' && ml_path_separator(p[2]) ? (size_t)(p + 3 - path) : 0;
    while (*p != L'\0' && !ml_path_separator(*p)) ++p;
    if (*p == L'\0') return 0;
    ++p;
    while (*p != L'\0' && !ml_path_separator(*p)) ++p;
    return (size_t)(p - path) + (*p != L'\0');
}

static inline void ml_path_remove_filename(wchar_t *path) {
    size_t length = wcslen(path);
    size_t root = ml_path_root_length(path);
    while (length > root && !ml_path_separator(path[length - 1])) --length;
    while (length > root && ml_path_separator(path[length - 1])) --length;
    path[length] = L'\0';
}

/* Takes ownership of a path that GetFullPathNameW normalized, possibly joined
 * with a plain relative name. A long DOS or UNC name is replaced by its extended
 * form before the directory-creation limit as well as the file limit, which
 * works inside games without a longPathAware manifest. Other paths are returned
 * as is. Returns NULL, having released `path`, if the copy cannot be made. */
static inline wchar_t *ml_path_extend(wchar_t *path) {
    const wchar_t *prefix;
    size_t length, prefix_length, skip;
    wchar_t *result;
    if (path == NULL || (length = wcslen(path)) < MAX_PATH - 12) return path;
    if (path[1] == L':' && path[2] == L'\\') {
        prefix = L"\\\\?\\";
        skip = 0;
    } else if (path[0] == L'\\' && path[1] == L'\\' && path[2] != L'?' && path[2] != L'.') {
        prefix = L"\\\\?\\UNC\\";
        skip = 2;
    } else {
        return path;
    }
    prefix_length = wcslen(prefix);
    result = ml_path_buffer(prefix_length + length - skip + 1);
    if (result != NULL) {
        memcpy(result, prefix, prefix_length * sizeof(*result));
        memcpy(result + prefix_length, path + skip, (length - skip + 1) * sizeof(*result));
    }
    ML_PATH_FREE(path);
    return result;
}

/* base is an absolute directory, or NULL to use the current directory. A
 * single leading separator uses base's root. Drive-relative names (C:foo)
 * are rejected rather than consulting a hidden per-drive current directory. */
static inline wchar_t *ml_path_resolve_alloc(const wchar_t *base, const wchar_t *requested) {
    wchar_t *name = NULL, *directory = NULL, *joined = NULL, *full = NULL, *result = NULL;
    size_t base_length = 0, name_length;
    DWORD capacity, length, error;
    if (requested == NULL || requested[0] == L'\0') requested = base;
    if (requested == NULL || requested[0] == L'\0') {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    name = ml_path_copy_alloc(requested);
    if (name == NULL) goto done;
    ml_path_normalize_input(name);
    name_length = wcslen(name);
    if (name_length >= 2 && name[1] == L':' &&
        (name_length < 3 || name[2] != L'\\')) {
        SetLastError(ERROR_BAD_PATHNAME);
        goto done;
    }
    if (base != NULL && !ml_path_is_absolute(name)) {
        directory = ml_path_copy_alloc(base);
        if (directory == NULL) goto done;
        ml_path_normalize_input(directory);
        if (!ml_path_is_absolute(directory)) {
            SetLastError(ERROR_BAD_PATHNAME);
            goto done;
        }
        base_length = name[0] == L'\\' ? ml_path_root_length(directory) : wcslen(directory);
        if (base_length == 0) {
            SetLastError(ERROR_BAD_PATHNAME);
            goto done;
        }
        while (base_length != 0 && directory[base_length - 1] == L'\\') --base_length;
        if (name[0] == L'\\') memmove(name, name + 1, name_length-- * sizeof(*name));
    }
    joined = ml_path_buffer(base_length + name_length + (base_length != 0 ? 2 : 1));
    if (joined == NULL) goto done;
    if (base_length != 0) {
        memcpy(joined, directory, base_length * sizeof(*joined));
        joined[base_length++] = L'\\';
    }
    memcpy(joined + base_length, name, (name_length + 1) * sizeof(*joined));
    capacity = GetFullPathNameW(joined, 0, NULL, NULL);
    while (capacity != 0) {
        full = ml_path_buffer(capacity);
        if (full == NULL) goto done;
        length = GetFullPathNameW(joined, capacity, full, NULL);
        if (length == 0) goto done;
        if (length < capacity) {
            result = ml_path_extend(full);
            full = NULL;
            break;
        }
        ML_PATH_FREE(full);
        full = NULL;
        capacity = length;
    }
done:
    error = GetLastError();
    ML_PATH_FREE(full);
    ML_PATH_FREE(joined);
    ML_PATH_FREE(directory);
    ML_PATH_FREE(name);
    if (result == NULL) SetLastError(error);
    return result;
}

/* Copies `path` in its DOS or UNC spelling when that has fewer than `limit`
 * characters, and as is otherwise. For paths that another process inherits,
 * such as its image path and current directory: with an extended current
 * directory, the child cannot resolve relative paths that contain "..". */
static inline wchar_t *ml_path_plain_alloc(const wchar_t *path, size_t limit) {
    wchar_t *plain = ml_path_copy_alloc(path);
    if (plain == NULL) return NULL;
    ml_path_normalize_input(plain);
    if (wcslen(plain) < limit) return plain;
    ML_PATH_FREE(plain);
    return ml_path_copy_alloc(path);
}

static inline wchar_t *ml_path_module_filename_alloc(HMODULE module) {
    DWORD capacity = 256;
    while (capacity <= ML_PATH_MAX_CCH) {
        wchar_t *path = ml_path_buffer(capacity);
        DWORD length, error;
        if (path == NULL) return NULL;
        length = GetModuleFileNameW(module, path, capacity);
        if (length != 0 && length < capacity) return path;
        error = GetLastError();
        ML_PATH_FREE(path);
        if (length == 0) {
            SetLastError(error);
            return NULL;
        }
        capacity *= 2;
    }
    SetLastError(ERROR_FILENAME_EXCED_RANGE);
    return NULL;
}

static inline wchar_t *ml_path_environment_alloc(const wchar_t *name) {
    DWORD capacity = GetEnvironmentVariableW(name, NULL, 0);
    while (capacity > 1) {
        wchar_t *value = ml_path_buffer(capacity);
        DWORD length, error;
        if (value == NULL) return NULL;
        length = GetEnvironmentVariableW(name, value, capacity);
        if (length != 0 && length < capacity) return value;
        error = GetLastError();
        ML_PATH_FREE(value);
        if (length == 0) {
            SetLastError(error);
            return NULL;
        }
        capacity = length;
    }
    return NULL;
}

static inline bool ml_path_is_directory(const wchar_t *path) {
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

#undef ML_PATH_ALLOC
#undef ML_PATH_FREE

#ifdef __cplusplus
}
#endif
