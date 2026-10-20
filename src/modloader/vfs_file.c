/*
 * Copyright (C) 2026, Soar Qin<soarchin@gmail.com>
 * Use of this source code is governed by the MIT license in the LICENSE file.
 */

#include "vfs_file.h"
#include "common/allocator.h"
#include "common/path.h"

#define kcalloc(N,Z) ml_mem_alloc(LPTR, (N) * (Z))
#define kmalloc(Z) ml_mem_alloc(0, (Z))
#define krealloc(P,Z) ((P) ? ml_mem_realloc((P), (Z), LMEM_MOVEABLE) : ml_mem_alloc(0, (Z)))
#define kfree(P) ml_mem_free(P)
#include "khash.h"

typedef enum cache_state_e { CACHE_EMPTY, CACHE_MEMORY, CACHE_DISK } cache_state_t;

/* The decoded copy of one ZIP entry, shared by all of its seekable files. Once
 * they are closed it stays idle for later opens, until its budget needs the
 * room or the caches are dropped. */
typedef struct vfs_cache_s {
    uint64_t identity;
    uint64_t size;
    /* Retained until the entry is decoded, so a failed decode can be retried. */
    zip_package_t *package;
    size_t entry;
    /* Held exclusively while decoding; the data never changes afterwards. */
    SRWLOCK lock;
    volatile LONG state;
    unsigned char *memory;
    HANDLE disk;
    /* The fields below are guarded by cache_lock. */
    LONG references;
    bool idle;
    bool detached;
    struct vfs_cache_s *older, *newer;
} vfs_cache_t;

/* Idle caches of one backend, least recently released first. */
typedef struct idle_list_s {
    vfs_cache_t *oldest, *newest;
    uint64_t bytes;
} idle_list_t;

KHASH_MAP_INIT_INT64(vfs_caches, vfs_cache_t *)

struct vfs_file_s {
    zip_stream_t *zip;
    vfs_cache_t *cache;
    uint64_t size;
    uint64_t identity;
    uint64_t position;
    uint64_t stream_position;
    DWORD error;
};

#define CACHE_CHUNK_SIZE 65536

static size_t memory_file_limit = 32 * 1024 * 1024;
static size_t memory_total_limit = 128 * 1024 * 1024;
/* Temporary files in use are unlimited; idle ones keep within this total. */
static uint64_t idle_disk_limit = 1024 * 1024 * 1024;
static volatile LONG64 memory_used;
static volatile LONG64 temporary_sequence;
/* Identities are VFS entry numbers; the VFS drops the table before it
 * re-points any of them. A cache in the table that no file uses is idle. */
static SRWLOCK cache_lock = SRWLOCK_INIT;
static khash_t(vfs_caches) *caches;
static idle_list_t idle_memory, idle_disk;

static void close_disk_cache(HANDLE disk) {
    vfs_recursion_guard_enter();
    CloseHandle(disk);
    vfs_recursion_guard_leave();
}

static void cache_destroy(vfs_cache_t *cache) {
    if (cache->memory != NULL) {
        ml_mem_free(cache->memory);
        InterlockedAdd64(&memory_used, -(LONG64)cache->size);
    }
    if (cache->disk != INVALID_HANDLE_VALUE) close_disk_cache(cache->disk);
    zip_package_close(cache->package);
    ml_mem_free(cache);
}

/* Destroys caches chained through `newer`. */
static void cache_destroy_chain(vfs_cache_t *chain) {
    while (chain != NULL) {
        vfs_cache_t *next = chain->newer;
        cache_destroy(chain);
        chain = next;
    }
}

/* The helpers below run with cache_lock held exclusively. */
static idle_list_t *idle_list(const vfs_cache_t *cache) {
    return cache->memory != NULL ? &idle_memory : &idle_disk;
}

static void idle_unlink(vfs_cache_t *cache) {
    idle_list_t *list = idle_list(cache);
    if (cache->older != NULL) cache->older->newer = cache->newer;
    else list->oldest = cache->newer;
    if (cache->newer != NULL) cache->newer->older = cache->older;
    else list->newest = cache->older;
    cache->older = cache->newer = NULL;
    cache->idle = false;
    list->bytes -= cache->size;
}

static void idle_push(vfs_cache_t *cache) {
    idle_list_t *list = idle_list(cache);
    cache->older = list->newest;
    cache->newer = NULL;
    if (list->newest != NULL) list->newest->newer = cache;
    else list->oldest = cache;
    list->newest = cache;
    cache->idle = true;
    list->bytes += cache->size;
}

static void cache_forget(const vfs_cache_t *cache) {
    khiter_t slot;
    if (caches == NULL) return;
    slot = kh_get(vfs_caches, caches, cache->identity);
    if (slot != kh_end(caches)) kh_del(vfs_caches, caches, slot);
}

/* Takes an idle cache out of the table and chains it in front of `chain`, to
 * be destroyed once cache_lock is released. */
static vfs_cache_t *idle_evict(vfs_cache_t *cache, vfs_cache_t *chain) {
    idle_unlink(cache);
    cache_forget(cache);
    cache->newer = chain;
    return cache;
}

static bool try_reserve_memory(uint64_t size) {
    LONG64 used;
    do {
        used = ReadAcquire64(&memory_used);
        if ((uint64_t)used > memory_total_limit - size) return false;
    } while (InterlockedCompareExchange64(&memory_used, used + (LONG64)size, used) != used);
    return true;
}

/* Makes room by dropping idle memory caches, least recently released first,
 * but only when they can free enough space together. */
static bool reserve_memory(uint64_t size) {
    if (size > memory_file_limit || size > memory_total_limit) return false;
    while (!try_reserve_memory(size)) {
        vfs_cache_t *victim = NULL;
        uint64_t used;
        AcquireSRWLockExclusive(&cache_lock);
        used = (uint64_t)ReadAcquire64(&memory_used);
        if (idle_memory.oldest != NULL && used >= idle_memory.bytes &&
            used - idle_memory.bytes <= memory_total_limit - size)
            victim = idle_evict(idle_memory.oldest, NULL);
        ReleaseSRWLockExclusive(&cache_lock);
        if (victim == NULL) return false;
        cache_destroy(victim);
    }
    return true;
}

static HANDLE create_disk_cache(void) {
    DWORD capacity = GetTempPathW(0, NULL), length, error;
    wchar_t *directory;
    HANDLE result = INVALID_HANDLE_VALUE;
    if (capacity == 0) return INVALID_HANDLE_VALUE;
    directory = ml_path_buffer(capacity);
    if (directory == NULL) return INVALID_HANDLE_VALUE;
    length = GetTempPathW(capacity, directory);
    if (length == 0 || length >= capacity) {
        error = length == 0 ? GetLastError() : ERROR_INSUFFICIENT_BUFFER;
        goto done;
    }
    for (unsigned int attempt = 0; attempt < 16; attempt++) {
        wchar_t name[48], *path;
        _snwprintf_s(name, sizeof(name) / sizeof(name[0]), _TRUNCATE, L"YAFSML-zip-%08lx-%016llx.tmp",
                     GetCurrentProcessId(), (unsigned long long)InterlockedIncrement64(&temporary_sequence));
        path = ml_path_resolve_alloc(directory, name);
        if (path == NULL) { error = GetLastError(); goto done; }
        vfs_recursion_guard_enter();
        result = CreateFileW(path, GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_DELETE,
                             NULL, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE |
                             FILE_FLAG_RANDOM_ACCESS, NULL);
        error = GetLastError();
        vfs_recursion_guard_leave();
        ml_mem_free(path);
        if (result != INVALID_HANDLE_VALUE || error != ERROR_FILE_EXISTS) break;
    }
done:
    ml_mem_free(directory);
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

/* Decodes the whole entry with cache->lock held exclusively. The storage is set
 * up first, so the archive is only read when the data has somewhere to go. */
static bool cache_decode(vfs_cache_t *cache) {
    unsigned char *memory = NULL, *staging = NULL, empty;
    HANDLE disk = INVALID_HANDLE_VALUE;
    zip_stream_t *zip = NULL;
    size_t received;
    DWORD error;
    bool result = false;
    if (cache->size != 0 && reserve_memory(cache->size)) {
        memory = ml_mem_alloc(0, (size_t)cache->size);
        if (memory == NULL) InterlockedAdd64(&memory_used, -(LONG64)cache->size);
    }
    if (cache->size != 0 && memory == NULL) {
        disk = create_disk_cache();
        if (disk == INVALID_HANDLE_VALUE) goto done;
        /* Hooked reads run on game threads whose stack size is unknown. */
        staging = ml_mem_alloc(0, CACHE_CHUNK_SIZE);
        if (staging == NULL) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); goto done; }
    }
    zip = zip_stream_open(cache->package, cache->entry);
    if (zip == NULL) goto done;
    vfs_recursion_guard_enter();
    for (uint64_t offset = 0; offset < cache->size; offset += received) {
        size_t chunk = cache->size - offset < CACHE_CHUNK_SIZE ? (size_t)(cache->size - offset) : CACHE_CHUNK_SIZE;
        unsigned char *target = memory == NULL ? staging : memory + (size_t)offset;
        if (!zip_stream_read(zip, target, chunk, &received)) goto decoded;
        if (received != chunk) { SetLastError(ERROR_INVALID_DATA); goto decoded; }
        if (disk != INVALID_HANDLE_VALUE && !write_cache(disk, target, received)) goto decoded;
    }
    /* Even empty entries must consume their end marker and validate the CRC. */
    if (cache->size == 0 && !zip_stream_read(zip, &empty, 1, &received)) goto decoded;
    result = true;
decoded:
    error = GetLastError();
    vfs_recursion_guard_leave();
    SetLastError(error);
done:
    error = GetLastError();
    ml_mem_free(staging);
    zip_stream_close(zip);
    if (result) {
        cache->memory = memory;
        cache->disk = disk;
        /* The data no longer needs the archive. */
        zip_package_close(cache->package);
        cache->package = NULL;
        InterlockedExchange(&cache->state, disk != INVALID_HANDLE_VALUE ? CACHE_DISK : CACHE_MEMORY);
    } else {
        if (memory != NULL) {
            ml_mem_free(memory);
            InterlockedAdd64(&memory_used, -(LONG64)cache->size);
        }
        if (disk != INVALID_HANDLE_VALUE) close_disk_cache(disk);
    }
    SetLastError(error);
    return result;
}

/* Decodes the entry on its first read. A failed decode leaves the cache empty,
 * so a later read through another file tries again. */
static bool cache_prepare(vfs_cache_t *cache) {
    bool result = true;
    if (ReadAcquire(&cache->state) != CACHE_EMPTY) return true;
    AcquireSRWLockExclusive(&cache->lock);
    if (ReadAcquire(&cache->state) == CACHE_EMPTY) result = cache_decode(cache);
    ReleaseSRWLockExclusive(&cache->lock);
    return result;
}

static bool cache_read(const vfs_cache_t *cache, uint64_t offset, void *buffer, size_t size, size_t *read) {
    size_t received = 0;
    bool result = true;
    DWORD error;
    if (cache->memory != NULL) {
        memcpy(buffer, cache->memory + (size_t)offset, size);
        *read = size;
        return true;
    }
    /* Positional reads let the files sharing the temporary file read at once. */
    vfs_recursion_guard_enter();
    while (received < size) {
        OVERLAPPED overlapped = { 0 };
        uint64_t position = offset + received;
        DWORD chunk = size - received > MAXDWORD ? MAXDWORD : (DWORD)(size - received), part;
        overlapped.Offset = (DWORD)position;
        overlapped.OffsetHigh = (DWORD)(position >> 32);
        if (!ReadFile(cache->disk, (unsigned char *)buffer + received, chunk, &part, &overlapped)) {
            result = false;
            break;
        }
        if (part == 0) { SetLastError(ERROR_READ_FAULT); result = false; break; }
        received += part;
    }
    error = GetLastError();
    vfs_recursion_guard_leave();
    SetLastError(error);
    *read = received;
    return result;
}

static vfs_cache_t *cache_acquire(zip_package_t *package, size_t entry, uint64_t identity, uint64_t size) {
    vfs_cache_t *cache = NULL;
    khiter_t slot;
    int ret;
    AcquireSRWLockExclusive(&cache_lock);
    if (caches == NULL) caches = kh_init(vfs_caches);
    if (caches != NULL) {
        slot = kh_put(vfs_caches, caches, identity, &ret);
        if (ret == 0) {
            cache = kh_value(caches, slot);
            if (cache->idle) idle_unlink(cache);
            cache->references++;
        } else if (ret > 0) {
            cache = ml_mem_alloc(LPTR, sizeof(*cache));
            if (cache == NULL) {
                kh_del(vfs_caches, caches, slot);
            } else {
                cache->identity = identity;
                cache->size = size;
                cache->package = package;
                cache->entry = entry;
                InitializeSRWLock(&cache->lock);
                cache->disk = INVALID_HANDLE_VALUE;
                cache->references = 1;
                zip_package_retain(package);
                kh_value(caches, slot) = cache;
            }
        }
    }
    ReleaseSRWLockExclusive(&cache_lock);
    if (cache == NULL) SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return cache;
}

/* The last file to close leaves the data idle: memory until another entry
 * needs the room, a temporary file while the idle ones fit their limit. */
static void cache_release(vfs_cache_t *cache) {
    vfs_cache_t *unused = NULL;
    AcquireSRWLockExclusive(&cache_lock);
    if (--cache->references == 0) {
        if (!cache->detached && (cache->memory != NULL ||
                                 (cache->disk != INVALID_HANDLE_VALUE && cache->size <= idle_disk_limit))) {
            idle_push(cache);
            while (idle_disk.bytes > idle_disk_limit) unused = idle_evict(idle_disk.oldest, unused);
        } else {
            /* Undecoded, empty, too large to keep, or named by a dropped table. */
            if (!cache->detached) cache_forget(cache);
            cache->newer = NULL;
            unused = cache;
        }
    }
    ReleaseSRWLockExclusive(&cache_lock);
    cache_destroy_chain(unused);
}

void vfs_file_drop_caches(void) {
    khash_t(vfs_caches) *table;
    vfs_cache_t *unused = NULL;
    AcquireSRWLockExclusive(&cache_lock);
    table = caches;
    caches = NULL;
    if (table != NULL) {
        for (khiter_t slot = kh_begin(table); slot != kh_end(table); slot++) {
            vfs_cache_t *cache;
            if (!kh_exist(table, slot)) continue;
            cache = kh_value(table, slot);
            if (cache->idle) {
                idle_unlink(cache);
                cache->newer = unused;
                unused = cache;
            } else {
                cache->detached = true;
            }
        }
    }
    ReleaseSRWLockExclusive(&cache_lock);
    cache_destroy_chain(unused);
    if (table != NULL) kh_destroy(vfs_caches, table);
}

vfs_file_t *vfs_file_create(zip_package_t *package, size_t entry, uint64_t identity, bool streaming) {
    vfs_file_t *file = ml_mem_alloc(LPTR, sizeof(*file));
    DWORD error;
    if (file == NULL) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    file->identity = identity;
    if (streaming) {
        file->zip = zip_stream_open(package, entry);
        if (file->zip != NULL) file->size = zip_stream_size(file->zip);
    } else {
        file->size = zip_package_file_size(package, entry);
        if (file->size > INT64_MAX) SetLastError(ERROR_INVALID_DATA);
        else file->cache = cache_acquire(package, entry, identity, file->size);
    }
    if (file->zip != NULL || file->cache != NULL) return file;
    error = GetLastError();
    ml_mem_free(file);
    SetLastError(error);
    return NULL;
}

void vfs_file_close(vfs_file_t *file) {
    if (file == NULL) return;
    zip_stream_close(file->zip);
    if (file->cache != NULL) cache_release(file->cache);
    ml_mem_free(file);
}

uint64_t vfs_file_size(const vfs_file_t *file) { return file->size; }
uint64_t vfs_file_identity(const vfs_file_t *file) { return file->identity; }

bool vfs_file_read_at(vfs_file_t *file, uint64_t offset, void *buffer, size_t size, size_t *read) {
    size_t received = 0;
    bool result;
    if (read != NULL) *read = 0;
    if (size != 0 && buffer == NULL) { SetLastError(ERROR_INVALID_PARAMETER); return false; }
    if (file->error != ERROR_SUCCESS) { SetLastError(file->error); return false; }
    if (size == 0) return true;
    if (file->zip != NULL) {
        /* Like a disk file, a read past the end returns no data; a stream
         * cannot otherwise skip ahead. */
        if (offset != file->stream_position) {
            if (offset >= file->size) return true;
            SetLastError(ERROR_NOT_SUPPORTED);
            return false;
        }
        result = zip_stream_read(file->zip, buffer, size, &received);
        file->stream_position += received;
    } else {
        /* Like a stream, a file whose decode failed keeps reporting it. */
        if (!cache_prepare(file->cache)) {
            file->error = GetLastError();
            return false;
        }
        if (offset >= file->size) return true;
        if (size > file->size - offset) size = (size_t)(file->size - offset);
        result = cache_read(file->cache, offset, buffer, size, &received);
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
    /* Moving the pointer does no I/O: caches decode on the next read. */
    if (next != file->position && file->zip != NULL) {
        SetLastError(ERROR_NOT_SUPPORTED);
        return false;
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
void vfs_file_test_idle_disk_limit(uint64_t limit) { idle_disk_limit = limit; }
uint64_t vfs_file_test_memory_used(void) { return (uint64_t)ReadAcquire64(&memory_used); }
HANDLE vfs_file_test_cache_handle(const vfs_file_t *file) {
    return file->cache == NULL ? INVALID_HANDLE_VALUE : file->cache->disk;
}
bool vfs_file_test_is_stream(const vfs_file_t *file) { return file->zip != NULL; }
#endif
