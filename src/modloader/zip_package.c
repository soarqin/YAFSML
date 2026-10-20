/*
 * Copyright (C) 2026, Soar Qin<soarchin@gmail.com>
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#include "zip_package.h"

#include "allocator.h"
#include "log.h"
#include "vfs.h"

#include <miniz.h>

#include <limits.h>
#include <stdint.h>

typedef struct zip_file_s {
    wchar_t *relative;
    mz_uint archive_index;
} zip_file_t;

struct zip_package_s {
    mz_zip_archive archive;
    HANDLE file;
    wchar_t *path;
    wchar_t *root;
    zip_file_t *files;
    size_t count;
    size_t capacity;
    SRWLOCK lock;
    volatile LONG references;
};

#define ZIP_STREAM_BUFFER_SIZE 65536

#ifdef ML_VFS_FILE_TEST
static volatile LONG64 test_read_count;
uint64_t zip_package_test_read_count(void) { return (uint64_t)ReadAcquire64(&test_read_count); }
#endif

struct zip_stream_s {
    zip_package_t *package;
    mz_zip_archive_file_stat stat;
    mz_stream inflater;
    uint64_t data_offset;
    uint64_t compressed_loaded;
    uint64_t position;
    mz_ulong crc;
    DWORD error;
    bool ended;
    unsigned char input[];
};

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
#ifdef ML_VFS_FILE_TEST
    InterlockedIncrement64(&test_read_count);
#endif
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
        package->files[package->count++] = (zip_file_t){ key, file };
    }
    if (!found_root) {
        mz_zip_set_last_error(&package->archive, MZ_ZIP_FILE_NOT_FOUND);
        return false;
    }
    return true;
failed:
    return false;
}

zip_package_t *zip_package_open(const wchar_t *path, const wchar_t *root) {
    zip_package_t *package;
    LARGE_INTEGER size;
    if (path == NULL || path[0] == L'\0') return NULL;
    package = ml_mem_alloc(LPTR, sizeof(*package));
    if (package == NULL) return NULL;
    package->file = INVALID_HANDLE_VALUE;
    package->references = 1;
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
    if (package == NULL || InterlockedDecrement(&package->references) != 0) return;
    vfs_recursion_guard_enter();
    if (package->archive.m_pState != NULL) mz_zip_reader_end(&package->archive);
    if (package->file != INVALID_HANDLE_VALUE) CloseHandle(package->file);
    for (size_t i = 0; i < package->count; i++) {
        ml_mem_free(package->files[i].relative);
    }
    vfs_recursion_guard_leave();
    ml_mem_free(package->files);
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

static unsigned int zip_u16(const unsigned char *p) {
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}

zip_stream_t *zip_stream_open(zip_package_t *package, size_t file) {
    unsigned char header[30];
    zip_stream_t *stream = NULL;
    mz_zip_archive_file_stat stat;
    DWORD error = ERROR_INVALID_DATA;
    if (package == NULL || file >= package->count) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    AcquireSRWLockExclusive(&package->lock);
    vfs_recursion_guard_enter();
    if (!mz_zip_reader_file_stat(&package->archive, package->files[file].archive_index, &stat) ||
        stat.m_uncomp_size > INT64_MAX ||
        zip_read(package->file, stat.m_local_header_ofs, header, sizeof(header)) != sizeof(header) ||
        memcmp(header, "PK\x03\x04", 4) != 0 || zip_u16(header + 8) != stat.m_method ||
        (zip_u16(header + 6) & (1 | 32 | 64)) != 0) goto failed;
    stream = ml_mem_alloc(LPTR, sizeof(*stream) + (stat.m_method == MZ_DEFLATED ? ZIP_STREAM_BUFFER_SIZE : 0));
    if (stream == NULL) { error = ERROR_NOT_ENOUGH_MEMORY; goto failed; }
    stream->stat = stat;
    stream->package = package;
    stream->data_offset = stream->stat.m_local_header_ofs + sizeof(header) +
                          zip_u16(header + 26) + zip_u16(header + 28);
    if (stream->data_offset > package->archive.m_archive_size ||
        stream->stat.m_comp_size > package->archive.m_archive_size - stream->data_offset ||
        (stream->stat.m_method == 0 && stream->stat.m_comp_size != stream->stat.m_uncomp_size)) goto failed;
    stream->inflater.zalloc = zip_alloc;
    stream->inflater.zfree = zip_free;
    if (stream->stat.m_method == MZ_DEFLATED &&
        mz_inflateInit2(&stream->inflater, -MZ_DEFAULT_WINDOW_BITS) != MZ_OK) {
        error = ERROR_NOT_ENOUGH_MEMORY;
        goto failed;
    }
    InterlockedIncrement(&package->references);
    vfs_recursion_guard_leave();
    ReleaseSRWLockExclusive(&package->lock);
    return stream;
failed:
    vfs_recursion_guard_leave();
    ReleaseSRWLockExclusive(&package->lock);
    ml_mem_free(stream);
    SetLastError(error);
    return NULL;
}

void zip_stream_close(zip_stream_t *stream) {
    if (stream == NULL) return;
    if (stream->inflater.state != NULL) mz_inflateEnd(&stream->inflater);
    zip_package_close(stream->package);
    ml_mem_free(stream);
}

uint64_t zip_stream_size(const zip_stream_t *stream) {
    return stream->stat.m_uncomp_size;
}

static bool zip_stream_error(zip_stream_t *stream, DWORD error) {
    if (stream->error == ERROR_SUCCESS) {
        ML_LOG_ERROR(L"zip", L"cannot stream `%ls` (root `%ls`, file `%hs`): error=%lu",
                     stream->package->path, stream->package->root, stream->stat.m_filename, error);
    }
    stream->error = error;
    SetLastError(error);
    return false;
}

static bool zip_stream_input(zip_stream_t *stream, uint64_t offset, void *buffer, size_t size) {
    size_t read;
    AcquireSRWLockExclusive(&stream->package->lock);
    vfs_recursion_guard_enter();
    read = zip_read(stream->package->file, stream->data_offset + offset, buffer, size);
    vfs_recursion_guard_leave();
    ReleaseSRWLockExclusive(&stream->package->lock);
    return read == size || zip_stream_error(stream, ERROR_READ_FAULT);
}

/* Always consume the end marker, even when the caller requested exactly the
 * declared size. This catches truncated streams, extra output and bad CRCs. */
static bool zip_stream_decode(zip_stream_t *stream, void *buffer, size_t size, size_t *read) {
    unsigned char extra;
    *read = 0;
    if (stream->stat.m_method == 0) {
        if (!zip_stream_input(stream, stream->position, buffer, size)) return false;
        stream->crc = mz_crc32(stream->crc, buffer, size);
        stream->position += size;
        *read = size;
        if (stream->position == stream->stat.m_uncomp_size &&
            stream->crc != stream->stat.m_crc32) return zip_stream_error(stream, ERROR_CRC);
        return true;
    }
    while (!stream->ended && (*read < size || stream->position == stream->stat.m_uncomp_size)) {
        mz_uint before_in, before_out;
        size_t produced;
        int status;
        bool at_end = stream->position == stream->stat.m_uncomp_size;
        if (stream->inflater.avail_in == 0 && stream->compressed_loaded < stream->stat.m_comp_size) {
            uint64_t remaining = stream->stat.m_comp_size - stream->compressed_loaded;
            size_t chunk = remaining < ZIP_STREAM_BUFFER_SIZE ? (size_t)remaining : ZIP_STREAM_BUFFER_SIZE;
            if (!zip_stream_input(stream, stream->compressed_loaded, stream->input, chunk)) return false;
            stream->compressed_loaded += chunk;
            stream->inflater.next_in = stream->input;
            stream->inflater.avail_in = (mz_uint)chunk;
        }
        stream->inflater.next_out = at_end ? &extra : (unsigned char *)buffer + *read;
        stream->inflater.avail_out = at_end ? 1 : size - *read > UINT_MAX ? UINT_MAX : (mz_uint)(size - *read);
        before_in = stream->inflater.avail_in;
        before_out = stream->inflater.avail_out;
        status = mz_inflate(&stream->inflater, MZ_NO_FLUSH);
        produced = before_out - stream->inflater.avail_out;
        if (at_end && produced != 0) return zip_stream_error(stream, ERROR_INVALID_DATA);
        if (produced != 0) stream->crc = mz_crc32(stream->crc, (unsigned char *)buffer + *read, produced);
        stream->position += produced;
        *read += produced;
        if (status == MZ_STREAM_END) {
            stream->ended = true;
            if (stream->position != stream->stat.m_uncomp_size ||
                stream->compressed_loaded - stream->inflater.avail_in != stream->stat.m_comp_size)
                return zip_stream_error(stream, ERROR_INVALID_DATA);
            if (stream->crc != stream->stat.m_crc32) return zip_stream_error(stream, ERROR_CRC);
        } else if ((status != MZ_OK && status != MZ_BUF_ERROR) ||
                   (produced == 0 && before_in == stream->inflater.avail_in)) {
            return zip_stream_error(stream, ERROR_INVALID_DATA);
        }
    }
    return true;
}

bool zip_stream_read(zip_stream_t *stream, void *buffer, size_t size, size_t *read) {
    *read = 0;
    if (size != 0 && buffer == NULL) { SetLastError(ERROR_INVALID_PARAMETER); return false; }
    if (stream->error != ERROR_SUCCESS) { SetLastError(stream->error); return false; }
    if (size == 0) return true;
    if (size > stream->stat.m_uncomp_size - stream->position)
        size = (size_t)(stream->stat.m_uncomp_size - stream->position);
    return zip_stream_decode(stream, buffer, size, read);
}
