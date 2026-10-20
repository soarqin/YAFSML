/*
 * Copyright (C) 2026, Soar Qin<soarchin@gmail.com>
 * Use of this source code is governed by the MIT license in the LICENSE file.
 */

#include "vfs_file.h"
#include "common/allocator.h"
#include "common/path.h"

typedef enum file_storage_e { FILE_UNPREPARED, FILE_STREAM, FILE_MEMORY, FILE_DISK } file_storage_t;

struct vfs_file_s {
    zip_stream_t *zip;
    uint64_t size;
    uint64_t identity;
    uint64_t position;
    uint64_t stream_position;
    unsigned char *memory;
    HANDLE disk;
    DWORD error;
    file_storage_t storage;
};

static size_t memory_file_limit = 32 * 1024 * 1024;
static size_t memory_total_limit = 128 * 1024 * 1024;
static volatile LONG64 memory_used;
static volatile LONG64 temporary_sequence;

static bool reserve_memory(uint64_t size) {
    LONG64 used;
    if (size > memory_file_limit || size > memory_total_limit) return false;
    do {
        used = ReadAcquire64(&memory_used);
        if ((uint64_t)used > memory_total_limit - size) return false;
    } while (InterlockedCompareExchange64(&memory_used, used + (LONG64)size, used) != used);
    return true;
}

static void release_storage(vfs_file_t *file) {
    if (file->memory != NULL) {
        ml_mem_free(file->memory);
        file->memory = NULL;
        InterlockedAdd64(&memory_used, -(LONG64)file->size);
    }
    if (file->disk != INVALID_HANDLE_VALUE) {
        vfs_recursion_guard_enter();
        CloseHandle(file->disk);
        vfs_recursion_guard_leave();
        file->disk = INVALID_HANDLE_VALUE;
    }
}

static HANDLE create_disk_cache(void) {
    DWORD capacity = GetTempPathW(0, NULL), length, error;
    wchar_t *path, *extended;
    HANDLE result = INVALID_HANDLE_VALUE;
    if (capacity == 0) return INVALID_HANDLE_VALUE;
    path = ml_path_buffer((size_t)capacity + 80);
    if (path == NULL) return INVALID_HANDLE_VALUE;
    length = GetTempPathW(capacity, path);
    if (length == 0 || length >= capacity) {
        error = length == 0 ? GetLastError() : ERROR_INSUFFICIENT_BUFFER;
        goto done;
    }
    for (unsigned int attempt = 0; attempt < 16; attempt++) {
        _snwprintf(path + length, 80, L"YAFSML-zip-%08lx-%016llx.tmp", GetCurrentProcessId(),
                   (unsigned long long)InterlockedIncrement64(&temporary_sequence));
        extended = ml_path_extended_alloc(path);
        if (extended == NULL) { error = GetLastError(); goto done; }
        vfs_recursion_guard_enter();
        result = CreateFileW(extended, GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_DELETE,
                             NULL, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE |
                             FILE_FLAG_RANDOM_ACCESS, NULL);
        error = GetLastError();
        vfs_recursion_guard_leave();
        ml_mem_free(extended);
        if (result != INVALID_HANDLE_VALUE || error != ERROR_FILE_EXISTS) break;
    }
done:
    ml_mem_free(path);
    SetLastError(error);
    return result;
}

static bool write_cache(HANDLE disk, const void *buffer, size_t size) {
    const unsigned char *data = buffer;
    DWORD written;
    while (size != 0) {
        DWORD chunk = size > MAXDWORD ? MAXDWORD : (DWORD)size;
        if (!WriteFile(disk, data, chunk, &written, NULL)) return false;
        if (written == 0) { SetLastError(ERROR_WRITE_FAULT); return false; }
        data += written;
        size -= written;
    }
    return true;
}

/* Decode raw files once, lazily. The BND4/EBL route selects FILE_STREAM at open
 * time; neither extensions nor content headers determine the storage policy. */
static bool prepare_file(vfs_file_t *file) {
    unsigned char buffer[65536];
    uint64_t offset;
    DWORD error;
    bool result = false;
    if (file->error != ERROR_SUCCESS) { SetLastError(file->error); return false; }
    if (file->storage != FILE_UNPREPARED) return true;
    if (file->size == 0) {
        file->storage = FILE_MEMORY;
    } else if (reserve_memory(file->size)) {
        file->memory = ml_mem_alloc(0, (size_t)file->size);
        if (file->memory == NULL) InterlockedAdd64(&memory_used, -(LONG64)file->size);
    }
    if (file->memory != NULL) file->storage = FILE_MEMORY;
    if (file->storage != FILE_MEMORY) {
        file->disk = create_disk_cache();
        if (file->disk == INVALID_HANDLE_VALUE) goto failed;
        file->storage = FILE_DISK;
    }
    vfs_recursion_guard_enter();
    offset = 0;
    while (offset < file->size) {
        size_t received, chunk = file->size - offset < sizeof(buffer) ? (size_t)(file->size - offset) : sizeof(buffer);
        unsigned char *target = file->memory == NULL ? buffer : file->memory + (size_t)offset;
        if (!zip_stream_read(file->zip, target, chunk, &received)) goto cache_done;
        if (received != chunk) { SetLastError(ERROR_INVALID_DATA); goto cache_done; }
        if (file->storage == FILE_DISK && !write_cache(file->disk, target, received)) goto cache_done;
        offset += received;
    }
    /* Even empty entries must consume their end marker and validate the CRC. */
    if (file->size == 0) {
        size_t received;
        if (!zip_stream_read(file->zip, buffer, 1, &received)) goto cache_done;
    }
    result = true;
cache_done:
    error = GetLastError();
    vfs_recursion_guard_leave();
    if (!result) { SetLastError(error); goto failed; }
    zip_stream_close(file->zip);
    file->zip = NULL;
    return true;
failed:
    error = GetLastError();
    release_storage(file);
    zip_stream_close(file->zip);
    file->zip = NULL;
    file->error = error;
    SetLastError(error);
    return false;
}

vfs_file_t *vfs_file_create(zip_package_t *package, size_t entry, uint64_t identity, bool streaming) {
    vfs_file_t *file = ml_mem_alloc(LPTR, sizeof(*file));
    if (file == NULL) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    file->disk = INVALID_HANDLE_VALUE;
    file->zip = zip_stream_open(package, entry);
    if (file->zip == NULL) {
        DWORD error = GetLastError();
        ml_mem_free(file);
        SetLastError(error);
        return NULL;
    }
    file->size = zip_stream_size(file->zip);
    file->identity = identity;
    if (streaming) file->storage = FILE_STREAM;
    return file;
}

void vfs_file_close(vfs_file_t *file) {
    if (file == NULL) return;
    release_storage(file);
    zip_stream_close(file->zip);
    ml_mem_free(file);
}

uint64_t vfs_file_size(const vfs_file_t *file) { return file->size; }
uint64_t vfs_file_identity(const vfs_file_t *file) { return file->identity; }

bool vfs_file_read_at(vfs_file_t *file, uint64_t offset, void *buffer, size_t size, size_t *read) {
    size_t received = 0;
    bool result = false;
    DWORD error;
    if (read != NULL) *read = 0;
    if (size != 0 && buffer == NULL) { SetLastError(ERROR_INVALID_PARAMETER); return false; }
    if (file->error != ERROR_SUCCESS) { SetLastError(file->error); return false; }
    if (size == 0) return true;
    if (!prepare_file(file)) return false;
    if (file->storage == FILE_STREAM) {
        if (offset != file->stream_position) { SetLastError(ERROR_NOT_SUPPORTED); return false; }
        result = zip_stream_read(file->zip, buffer, size, &received);
        file->stream_position += received;
    } else if (offset >= file->size) {
        return true;
    } else {
        if (size > file->size - offset) size = (size_t)(file->size - offset);
        if (file->storage == FILE_MEMORY) {
            memcpy(buffer, file->memory + (size_t)offset, size);
            received = size;
            result = true;
        } else {
            LARGE_INTEGER position;
            position.QuadPart = (LONGLONG)offset;
            vfs_recursion_guard_enter();
            result = SetFilePointerEx(file->disk, position, NULL, FILE_BEGIN) != FALSE;
            while (result && received < size) {
                DWORD chunk = size - received > MAXDWORD ? MAXDWORD : (DWORD)(size - received), part;
                result = ReadFile(file->disk, (unsigned char *)buffer + received, chunk, &part, NULL) != FALSE;
                if (!result) break;
                if (part == 0) { SetLastError(ERROR_READ_FAULT); result = false; break; }
                received += part;
            }
            error = GetLastError();
            vfs_recursion_guard_leave();
            SetLastError(error);
        }
    }
    if (read != NULL) *read = received;
    return result;
}

bool vfs_file_read(vfs_file_t *file, void *buffer, size_t size, size_t *read) {
    size_t received;
    bool result = vfs_file_read_at(file, file->position, buffer, size, &received);
    file->position += received;
    if (read != NULL) *read = received;
    return result;
}

bool vfs_file_seek(vfs_file_t *file, int64_t distance, DWORD origin, uint64_t *position) {
    uint64_t base, next;
    if (origin > FILE_END) { SetLastError(ERROR_INVALID_PARAMETER); return false; }
    base = origin == FILE_BEGIN ? 0 : origin == FILE_CURRENT ? file->position : file->size;
    if (distance < 0 && (uint64_t)(-(distance + 1)) + 1 > base) {
        SetLastError(ERROR_NEGATIVE_SEEK); return false;
    }
    if (distance > 0 && (uint64_t)distance > INT64_MAX - base) {
        SetLastError(ERROR_INVALID_PARAMETER); return false;
    }
    next = distance < 0 ? base - ((uint64_t)(-(distance + 1)) + 1) : base + (uint64_t)distance;
    if (next != file->position) {
        if (!prepare_file(file)) return false;
        if (file->storage == FILE_STREAM) { SetLastError(ERROR_NOT_SUPPORTED); return false; }
    }
    file->position = next;
    if (position != NULL) *position = next;
    return true;
}

#ifdef ML_VFS_FILE_TEST
void vfs_file_test_limits(size_t file_limit, size_t total_limit) {
    memory_file_limit = file_limit;
    memory_total_limit = total_limit;
}
uint64_t vfs_file_test_memory_used(void) { return (uint64_t)ReadAcquire64(&memory_used); }
HANDLE vfs_file_test_cache_handle(const vfs_file_t *file) { return file->disk; }
bool vfs_file_test_is_stream(const vfs_file_t *file) { return file->storage == FILE_STREAM; }
#endif
