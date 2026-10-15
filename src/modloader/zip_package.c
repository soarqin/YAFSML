/*
 * Copyright (C) 2026, Soar Qin<soarchin@gmail.com>
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#include "zip_package.h"

#include "allocator.h"
#include "common/path.h"
#include "log.h"
#include "vfs.h"

#include <miniz.h>

#include <limits.h>
#include <stdint.h>

typedef struct zip_file_s {
    wchar_t *relative;
    wchar_t *cache_path;
    mz_uint archive_index;
    bool materialized;
} zip_file_t;

struct zip_package_s {
    mz_zip_archive archive;
    HANDLE file;
    wchar_t *path;
    wchar_t *root;
    wchar_t *cache_directory;
    zip_file_t *files;
    size_t count;
    size_t capacity;
    SRWLOCK lock;
};

static volatile LONG cache_sequence;

static void *zip_alloc(void *opaque, size_t items, size_t size) {
    (void)opaque;
    return size != 0 && items > SIZE_MAX / size ? NULL : ml_mem_alloc(0, items * size);
}

static void zip_free(void *opaque, void *address) {
    (void)opaque;
    ml_mem_free(address);
}

static void *zip_realloc(void *opaque, void *address, size_t items, size_t size) {
    size_t bytes;
    (void)opaque;
    if (size != 0 && items > SIZE_MAX / size) return NULL;
    bytes = items * size;
    return address == NULL ? ml_mem_alloc(0, bytes) : ml_mem_realloc(address, bytes, LMEM_MOVEABLE);
}

/* miniz uses offset-based callbacks. Access to each archive is serialized. */
static size_t zip_read(void *opaque, mz_uint64 offset, void *buffer, size_t size) {
    HANDLE file = opaque;
    LARGE_INTEGER position;
    size_t total = 0;
    if (offset > INT64_MAX) return 0;
    position.QuadPart = (LONGLONG)offset;
    if (!SetFilePointerEx(file, position, NULL, FILE_BEGIN)) return 0;
    while (total < size) {
        DWORD read;
        DWORD chunk = size - total > MAXDWORD ? MAXDWORD : (DWORD)(size - total);
        if (!ReadFile(file, (char *)buffer + total, chunk, &read, NULL) || read == 0) break;
        total += read;
    }
    return total;
}

static size_t zip_write(void *opaque, mz_uint64 offset, const void *buffer, size_t size) {
    HANDLE file = opaque;
    LARGE_INTEGER position;
    size_t total = 0;
    if (offset > INT64_MAX) return 0;
    position.QuadPart = (LONGLONG)offset;
    if (!SetFilePointerEx(file, position, NULL, FILE_BEGIN)) return 0;
    while (total < size) {
        DWORD written;
        DWORD chunk = size - total > MAXDWORD ? MAXDWORD : (DWORD)(size - total);
        if (!WriteFile(file, (const char *)buffer + total, chunk, &written, NULL) || written == 0) break;
        total += written;
    }
    return total;
}

static bool valid_virtual_name(const wchar_t *name) {
    for (; *name != L'\0'; name++) {
        if (*name < L' ' || wcschr(L":<>\"|?*", *name) != NULL) return false;
    }
    return true;
}

static wchar_t *zip_filename(mz_zip_archive *archive, mz_uint file, mz_uint16 flags) {
    mz_uint bytes = mz_zip_reader_get_filename(archive, file, NULL, 0);
    char *name;
    wchar_t *wide = NULL;
    int length;
    UINT code_page = CP_UTF8;
    DWORD conversion_flags = MB_ERR_INVALID_CHARS;
    if (bytes <= 1 || bytes - 1 > INT_MAX) return NULL;
    name = ml_mem_alloc(0, bytes);
    if (name == NULL) return NULL;
    if (mz_zip_reader_get_filename(archive, file, name, bytes) != bytes ||
        memchr(name, '\0', bytes - 1) != NULL) goto done;
    length = MultiByteToWideChar(code_page, conversion_flags, name, (int)bytes - 1, NULL, 0);
    if (length == 0 && (flags & 0x0800) == 0) {
        /* Legacy ZIP names use CP437. Some writers omit the flag on valid UTF-8. */
        code_page = 437;
        conversion_flags = 0;
        length = MultiByteToWideChar(code_page, conversion_flags, name, (int)bytes - 1, NULL, 0);
    }
    if (length == 0) goto done;
    wide = ml_mem_alloc(0, ((size_t)length + 1) * sizeof(*wide));
    if (wide != NULL) {
        if (MultiByteToWideChar(code_page, conversion_flags, name, (int)bytes - 1, wide, length) == 0) {
            ml_mem_free(wide);
            wide = NULL;
        } else {
            wide[length] = L'\0';
        }
    }
done:
    ml_mem_free(name);
    return wide;
}

static bool zip_index(zip_package_t *package, const wchar_t *root) {
    wchar_t *normalized_root;
    size_t root_length;
    bool found_root;
    if (!valid_virtual_name(root) || !vfs_normalize_path(root, &package->root)) {
        mz_zip_set_last_error(&package->archive, MZ_ZIP_INVALID_FILENAME);
        return false;
    }
    normalized_root = package->root;
    root_length = wcslen(normalized_root);
    found_root = root_length == 0;
    for (mz_uint file = 0; file < mz_zip_reader_get_num_files(&package->archive); file++) {
        mz_zip_archive_file_stat stat;
        wchar_t *name;
        wchar_t *key = NULL;
        const wchar_t *relative;
        bool valid;
        if (!mz_zip_reader_file_stat(&package->archive, file, &stat)) goto failed;
        name = zip_filename(&package->archive, file, stat.m_bit_flag);
        valid = name != NULL && name[0] != L'/' && name[0] != L'\\' &&
                valid_virtual_name(name) && vfs_normalize_path(name, &key);
        ml_mem_free(name);
        if (!valid) {
            ml_mem_free(key);
            mz_zip_set_last_error(&package->archive, MZ_ZIP_INVALID_FILENAME);
            goto failed;
        }
        /* ZIPs produced from '.' often include an explicit './' root record. */
        if (key[0] == L'\0') {
            ml_mem_free(key);
            if (stat.m_is_directory) continue;
            mz_zip_set_last_error(&package->archive, MZ_ZIP_INVALID_FILENAME);
            goto failed;
        }
        relative = key;
        if (root_length != 0) {
            if (wcscmp(key, normalized_root) == 0 && stat.m_is_directory) found_root = true;
            if (wcsncmp(key, normalized_root, root_length) != 0 || key[root_length] != L'\\') {
                ml_mem_free(key);
                continue;
            }
            found_root = true;
            relative += root_length + 1;
        }
        if (stat.m_is_directory) {
            ml_mem_free(key);
            continue;
        }
        if (!stat.m_is_supported) {
            ml_mem_free(key);
            mz_zip_set_last_error(&package->archive,
                                 stat.m_is_encrypted ? MZ_ZIP_UNSUPPORTED_ENCRYPTION : MZ_ZIP_UNSUPPORTED_METHOD);
            goto failed;
        }
        if (package->count == package->capacity) {
            size_t capacity = package->capacity == 0 ? 128 : package->capacity * 2;
            zip_file_t *files;
            if (capacity < package->capacity || capacity > SIZE_MAX / sizeof(*files)) {
                ml_mem_free(key);
                mz_zip_set_last_error(&package->archive, MZ_ZIP_ALLOC_FAILED);
                goto failed;
            }
            files = package->files == NULL ? ml_mem_alloc(0, capacity * sizeof(*files))
                : ml_mem_realloc(package->files, capacity * sizeof(*files), LMEM_MOVEABLE);
            if (files == NULL) {
                ml_mem_free(key);
                mz_zip_set_last_error(&package->archive, MZ_ZIP_ALLOC_FAILED);
                goto failed;
            }
            package->files = files;
            package->capacity = capacity;
        }
        if (relative != key) memmove(key, relative, (wcslen(relative) + 1) * sizeof(*key));
        package->files[package->count++] = (zip_file_t){ key, NULL, file, false };
    }
    if (!found_root) {
        mz_zip_set_last_error(&package->archive, MZ_ZIP_FILE_NOT_FOUND);
        return false;
    }
    return true;
failed:
    return false;
}

static wchar_t *zip_cache_directory(void) {
    DWORD capacity = GetTempPathW(0, NULL);
    wchar_t *temp;
    wchar_t *directory = NULL;
    DWORD length;
    if (capacity == 0) return NULL;
    temp = ml_mem_alloc(0, (size_t)capacity * sizeof(*temp));
    if (temp == NULL) return NULL;
    length = GetTempPathW(capacity, temp);
    if (length == 0 || length >= capacity) goto done;
    for (size_t attempt = 0; attempt < 64; attempt++) {
        unsigned long long stamp = (unsigned long long)GetTickCount64();
        LONG sequence = InterlockedIncrement(&cache_sequence);
        wchar_t name[96];
        _snwprintf_s(name, 96, _TRUNCATE, L"YAFSML-%lu-%llu-%ld", GetCurrentProcessId(), stamp, sequence);
        directory = ml_path_resolve_alloc(temp, name);
        if (directory == NULL) break;
        if (CreateDirectoryW(directory, NULL)) break;
        DWORD error = GetLastError();
        ml_mem_free(directory);
        directory = NULL;
        if (error != ERROR_ALREADY_EXISTS) break;
    }
done:
    ml_mem_free(temp);
    return directory;
}

zip_package_t *zip_package_open(const wchar_t *path, const wchar_t *root) {
    zip_package_t *package;
    LARGE_INTEGER size;
    if (path == NULL || path[0] == L'\0') return NULL;
    package = ml_mem_alloc(LPTR, sizeof(*package));
    if (package == NULL) return NULL;
    package->file = INVALID_HANDLE_VALUE;
    InitializeSRWLock(&package->lock);
    package->path = ml_mem_strdup_w(path);
    package->archive.m_pAlloc = zip_alloc;
    package->archive.m_pRealloc = zip_realloc;
    package->archive.m_pFree = zip_free;
    package->archive.m_pRead = zip_read;
    vfs_recursion_guard_enter();
    if (package->path == NULL) {
        mz_zip_set_last_error(&package->archive, MZ_ZIP_ALLOC_FAILED);
        goto failed;
    }
    package->file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, NULL);
    if (package->file == INVALID_HANDLE_VALUE || !GetFileSizeEx(package->file, &size)) {
        mz_zip_set_last_error(&package->archive, MZ_ZIP_FILE_OPEN_FAILED);
        goto failed;
    }
    package->archive.m_pIO_opaque = package->file;
    if (!mz_zip_reader_init(&package->archive, (mz_uint64)size.QuadPart, MZ_ZIP_FLAG_DO_NOT_SORT_CENTRAL_DIRECTORY) ||
        !zip_index(package, root == NULL ? L"" : root)) goto failed;
    if (package->count != 0) {
        package->cache_directory = zip_cache_directory();
        if (package->cache_directory == NULL) {
            mz_zip_set_last_error(&package->archive, MZ_ZIP_FILE_CREATE_FAILED);
            goto failed;
        }
        for (size_t i = 0; i < package->count; i++) {
            const wchar_t *extension = ml_path_extension(package->files[i].relative);
            if (wcslen(extension) > 32) extension = L".bin";
            wchar_t name[48];
            _snwprintf_s(name, 48, _TRUNCATE, L"%08x%ls", package->files[i].archive_index, extension);
            wchar_t *cache_path = ml_path_resolve_alloc(package->cache_directory, name);
            if (cache_path == NULL) {
                mz_zip_set_last_error(&package->archive, MZ_ZIP_ALLOC_FAILED);
                goto failed;
            }
            package->files[i].cache_path = cache_path;
        }
    }
    vfs_recursion_guard_leave();
    return package;
failed:
    ML_LOG_ERROR(L"zip", L"cannot index `%ls` (root `%ls`): %hs", path, root == NULL ? L"" : root,
                 mz_zip_get_error_string(mz_zip_peek_last_error(&package->archive)));
    vfs_recursion_guard_leave();
    zip_package_close(package);
    return NULL;
}

void zip_package_close(zip_package_t *package) {
    if (package == NULL) return;
    vfs_recursion_guard_enter();
    if (package->archive.m_pState != NULL) mz_zip_reader_end(&package->archive);
    if (package->file != INVALID_HANDLE_VALUE) CloseHandle(package->file);
    for (size_t i = 0; i < package->count; i++) {
        if (package->files[i].materialized) DeleteFileW(package->files[i].cache_path);
        ml_mem_free(package->files[i].relative);
        ml_mem_free(package->files[i].cache_path);
    }
    if (package->cache_directory != NULL) RemoveDirectoryW(package->cache_directory);
    vfs_recursion_guard_leave();
    ml_mem_free(package->files);
    ml_mem_free(package->cache_directory);
    ml_mem_free(package->path);
    ml_mem_free(package->root);
    ml_mem_free(package);
}

size_t zip_package_count(const zip_package_t *package) {
    return package->count;
}

const wchar_t *zip_package_relative_path(const zip_package_t *package, size_t file) {
    return package->files[file].relative;
}

const wchar_t *zip_package_cache_path(const zip_package_t *package, size_t file) {
    return package->files[file].cache_path;
}

const wchar_t *zip_package_materialize(zip_package_t *package, size_t file) {
    zip_file_t *entry = &package->files[file];
    const wchar_t *result = NULL;
    HANDLE output;
    bool success;
    AcquireSRWLockShared(&package->lock);
    if (entry->materialized) {
        ReleaseSRWLockShared(&package->lock);
        return entry->cache_path;
    }
    ReleaseSRWLockShared(&package->lock);
    AcquireSRWLockExclusive(&package->lock);
    if (entry->materialized) {
        result = entry->cache_path;
        goto done;
    }
    vfs_recursion_guard_enter();
    output = CreateFileW(entry->cache_path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, NULL);
    if (output == INVALID_HANDLE_VALUE) {
        ML_LOG_ERROR(L"zip", L"cannot create cache for `%ls` (root `%ls`, file `%ls`): error=%lu",
                     package->path, package->root, entry->relative, GetLastError());
        vfs_recursion_guard_leave();
        goto done;
    }
    success = mz_zip_reader_extract_to_callback(&package->archive, entry->archive_index, zip_write, output, 0) != MZ_FALSE;
    if (!CloseHandle(output)) {
        mz_zip_set_last_error(&package->archive, MZ_ZIP_FILE_CLOSE_FAILED);
        success = false;
    }
    if (success) {
        entry->materialized = true;
        result = entry->cache_path;
    } else {
        DeleteFileW(entry->cache_path);
        ML_LOG_ERROR(L"zip", L"cannot read `%ls` (root `%ls`, file `%ls`): %hs", package->path, package->root, entry->relative,
                     mz_zip_get_error_string(mz_zip_peek_last_error(&package->archive)));
    }
    vfs_recursion_guard_leave();
done:
    ReleaseSRWLockExclusive(&package->lock);
    return result;
}
