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

/* Extended DOS/UNC names are converted back before resolving . and ..; those
 * navigation elements have no meaning after an extended prefix is applied. */
static inline void ml_path_normalize_input(wchar_t *path) {
    for (wchar_t *p = path; *p != L'\0'; ++p) if (*p == L'/') *p = L'\\';
    if (_wcsnicmp(path, L"\\\\?\\UNC\\", 8) == 0) {
        memmove(path + 2, path + 8, (wcslen(path + 8) + 1) * sizeof(*path));
        path[0] = path[1] = L'\\';
    } else if (wcsncmp(path, L"\\\\?\\", 4) == 0 &&
               wcslen(path + 4) >= 3 && path[5] == L':' && path[6] == L'\\') {
        memmove(path, path + 4, (wcslen(path + 4) + 1) * sizeof(*path));
    }
}

static inline bool ml_path_is_absolute(const wchar_t *path) {
    size_t length = path == NULL ? 0 : wcslen(path);
    return length >= 3 && ((path[1] == L':' && ml_path_separator(path[2])) ||
                          (ml_path_separator(path[0]) && ml_path_separator(path[1])));
}

/* Includes the final separator when one is present; an UNC share itself is a
 * root, so a rooted request cannot discard its server/share components. */
static inline size_t ml_path_root_length(const wchar_t *path) {
    const wchar_t *p;
    if (path[0] != L'\0' && path[1] == L':' && ml_path_separator(path[2])) return 3;
    if (!ml_path_separator(path[0]) || !ml_path_separator(path[1])) return 0;
    p = path + 2;
    if (wcsncmp(p, L"?\\", 2) == 0) {
        p += 2;
        if (_wcsnicmp(p, L"UNC\\", 4) == 0) p += 4;
        else if (p[0] != L'\0' && p[1] == L':' && ml_path_separator(p[2])) return (size_t)(p + 3 - path);
        else return 0;
    }
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

/* Use an extended prefix before the directory-creation limit as well as the
 * file limit. This works inside games without a longPathAware manifest. */
static inline wchar_t *ml_path_extended_alloc(const wchar_t *path) {
    size_t length = wcslen(path);
    size_t prefix;
    wchar_t *result;
    if (length < MAX_PATH - 12 || wcsncmp(path, L"\\\\?\\", 4) == 0 ||
        !ml_path_is_absolute(path)) return ml_path_copy_alloc(path);
    prefix = path[0] == L'\\' && path[1] == L'\\' ? 6 : 4;
    result = ml_path_buffer(length + prefix + 1);
    if (result == NULL) return NULL;
    if (prefix == 6) {
        memcpy(result, L"\\\\?\\UNC\\", 8 * sizeof(*result));
        memcpy(result + 8, path + 2, (length - 1) * sizeof(*result));
    } else {
        memcpy(result, L"\\\\?\\", 4 * sizeof(*result));
        memcpy(result + 4, path, (length + 1) * sizeof(*result));
    }
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
            result = ml_path_extended_alloc(full);
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
