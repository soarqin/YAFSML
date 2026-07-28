/*
 * Copyright (C) 2026, Soar Qin<soarchin@gmail.com>
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#include "vfs.h"

#include "allocator.h"

#include <shlwapi.h>

#include <wctype.h>
#include <stdlib.h>

#define kcalloc(N,Z) ml_mem_alloc(LPTR, (N) * (Z))
#define kmalloc(Z) ml_mem_alloc(0, (Z))
#define krealloc(P,Z) ((P) ? ml_mem_realloc((P), (Z), LMEM_MOVEABLE) : ml_mem_alloc(0, (Z)))
#define kfree(P) ml_mem_free(P)
#include "khash.h"
#include "khash_wstr.h"

typedef struct vfs_entry_s {
    wchar_t *key;
    wchar_t *path;
    wchar_t *uid;
} vfs_entry_t;

typedef struct vfs_writable_entry_s {
    wchar_t *key;
    wchar_t *path;
} vfs_writable_entry_t;

KHASH_INIT(vfs_index, const wchar_t *, size_t, 1, kh_wstr_hash_func, kh_wstr_hash_equal)
KHASH_INIT(vfs_lookup_cache, const wchar_t *, const wchar_t *, 1, kh_wstr_hash_func, kh_wstr_hash_equal)

static khash_t(vfs_index) *index;
static khash_t(vfs_lookup_cache) *lookup_caches[VFS_LOOKUP_DOMAIN_COUNT];
static khash_t(vfs_lookup_cache) *uid_cache;
static khash_t(vfs_lookup_cache) *writable_cache;
static vfs_entry_t *entries;
static size_t entry_count;
static size_t entry_capacity;
static volatile LONG frozen;
static volatile LONG64 generation;
static volatile LONG64 generation_sequence;
static SRWLOCK lookup_cache_lock = SRWLOCK_INIT;
static SRWLOCK writable_lock = SRWLOCK_INIT;
static __declspec(thread) unsigned int vfs_recursion_depth;
static vfs_writable_entry_t *writable_entries;
static size_t writable_count;
static size_t writable_capacity;

static const wchar_t *vfs_strip_path_prefix(const wchar_t *path, const wchar_t *prefix) {
    if (path == NULL || prefix == NULL) return NULL;
    while (*prefix != L'\0') {
        bool path_separator = *path == L'\\' || *path == L'/';
        bool prefix_separator = *prefix == L'\\' || *prefix == L'/';
        if (*path == L'\0' || (path_separator != prefix_separator) ||
            (!path_separator && CompareStringOrdinal(path, 1, prefix, 1, TRUE) != CSTR_EQUAL)) return NULL;
        path++;
        prefix++;
    }
    return *path == L'\0' || *path == L'\\' || *path == L'/' ? path : NULL;
}

static wchar_t *vfs_join(const wchar_t *left, const wchar_t *right) {
    size_t left_len = wcslen(left);
    size_t right_len = wcslen(right);
    wchar_t *result = ml_mem_alloc(0, (left_len + right_len + 2) * sizeof(*result));
    if (result == NULL) return NULL;
    memcpy(result, left, left_len * sizeof(*result));
    if (left_len != 0 && left[left_len - 1] != L'\\') result[left_len++] = L'\\';
    memcpy(result + left_len, right, (right_len + 1) * sizeof(*result));
    return result;
}

bool vfs_normalize_path(const wchar_t *path, wchar_t **normalized) {
    size_t length;
    wchar_t *result;
    size_t *segment_starts;
    size_t out = 0;
    size_t segment_count = 0;
    if (normalized == NULL || path == NULL) return false;
    *normalized = NULL;
    length = wcslen(path);
    if (length == SIZE_MAX || length + 1 > SIZE_MAX / sizeof(*result) ||
        length + 1 > SIZE_MAX / sizeof(*segment_starts)) return false;
    result = ml_mem_alloc(0, (length + 1) * sizeof(*result));
    if (result == NULL) return false;
    segment_starts = ml_mem_alloc(0, (length + 1) * sizeof(*segment_starts));
    if (segment_starts == NULL) {
        ml_mem_free(result);
        return false;
    }

    while (*path == L'\\' || *path == L'/') path++;
    {
        const wchar_t *separator = wcschr(path, L':');
        const wchar_t *slash = wcspbrk(path, L"\\/");
        if (separator != NULL && (slash == NULL || separator < slash)) path = separator + 1;
        while (*path == L'\\' || *path == L'/') path++;
    }
    while (*path != L'\0') {
        const wchar_t *start = path;
        size_t part_len;
        while (*path != L'\0' && *path != L'\\' && *path != L'/') path++;
        part_len = (size_t)(path - start);
        while (*path == L'\\' || *path == L'/') path++;
        if (part_len == 0 || (part_len == 1 && start[0] == L'.')) continue;
        if (part_len == 2 && start[0] == L'.' && start[1] == L'.') {
            if (segment_count == 0) {
                ml_mem_free(segment_starts);
                ml_mem_free(result);
                return false;
            }
            out = segment_starts[--segment_count];
            continue;
        }
        segment_starts[segment_count++] = out;
        if (out != 0) result[out++] = L'\\';
        for (size_t i = 0; i < part_len; i++) result[out++] = towlower(start[i]);
    }
    result[out] = L'\0';
    ml_mem_free(segment_starts);
    *normalized = result;
    return true;
}

static bool vfs_insert(const wchar_t *relative, const wchar_t *physical) {
    wchar_t *key;
    wchar_t *path;
    khiter_t slot;
    int ret;
    if (!vfs_normalize_path(relative, &key)) return false;
    path = ml_mem_strdup_w(physical);
    if (path == NULL) {
        ml_mem_free(key);
        return false;
    }
    slot = kh_put(vfs_index, index, key, &ret);
    if (ret < 0) {
        ml_mem_free(key);
        ml_mem_free(path);
        return false;
    }
    if (ret == 0) {
        vfs_entry_t *entry = &entries[kh_value(index, slot)];
        ml_mem_free(entry->path);
        entry->path = path;
        ml_mem_free(key);
        return true;
    }
    if (entry_count == entry_capacity) {
        size_t capacity = entry_capacity == 0 ? 128 : entry_capacity * 2;
        vfs_entry_t *new_entries = entries == NULL
            ? ml_mem_alloc(LMEM_ZEROINIT, capacity * sizeof(*entries))
            : ml_mem_realloc(entries, capacity * sizeof(*entries), LMEM_MOVEABLE | LMEM_ZEROINIT);
        if (new_entries == NULL) {
            kh_del(vfs_index, index, slot);
            ml_mem_free(key);
            ml_mem_free(path);
            return false;
        }
        entries = new_entries;
        entry_capacity = capacity;
    }
    kh_key(index, slot) = key;
    kh_value(index, slot) = entry_count;
    int uid_length = _scwprintf(L"\\\\me3??%zx", entry_count);
    wchar_t *uid = uid_length < 0 ? NULL : ml_mem_alloc(0, ((size_t)uid_length + 1) * sizeof(*uid));
    if (uid == NULL) {
        kh_del(vfs_index, index, slot);
        ml_mem_free(key);
        ml_mem_free(path);
        return false;
    }
    _snwprintf(uid, (size_t)uid_length + 1, L"\\\\me3??%zx", entry_count);
    entries[entry_count] = (vfs_entry_t){ key, path, uid };
    entry_count++;
    return true;
}

static bool vfs_scan(const wchar_t *root, const wchar_t *relative) {
    wchar_t *directory = relative[0] == L'\0' ? ml_mem_strdup_w(root) : vfs_join(root, relative);
    wchar_t *pattern;
    WIN32_FIND_DATAW find;
    HANDLE handle;
    if (directory == NULL) return false;
    pattern = vfs_join(directory, L"*");
    ml_mem_free(directory);
    if (pattern == NULL) return false;
    handle = FindFirstFileW(pattern, &find);
    ml_mem_free(pattern);
    if (handle == INVALID_HANDLE_VALUE) return false;
    do {
        if (lstrcmpW(find.cFileName, L".") == 0 || lstrcmpW(find.cFileName, L"..") == 0) continue;
        wchar_t *child_relative = relative[0] == L'\0' ? ml_mem_strdup_w(find.cFileName) : vfs_join(relative, find.cFileName);
        wchar_t *child_path = child_relative == NULL ? NULL : vfs_join(root, child_relative);
        if (child_relative == NULL || child_path == NULL) {
            ml_mem_free(child_relative);
            ml_mem_free(child_path);
            FindClose(handle);
            return false;
        }
        if ((find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            if ((find.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0 && !vfs_scan(root, child_relative)) {
                ml_mem_free(child_relative);
                ml_mem_free(child_path);
                FindClose(handle);
                return false;
            }
        } else {
            if (!vfs_insert(child_relative, child_path)) {
                ml_mem_free(child_relative);
                ml_mem_free(child_path);
                FindClose(handle);
                return false;
            }
        }
        ml_mem_free(child_relative);
        ml_mem_free(child_path);
    } while (FindNextFileW(handle, &find));
    FindClose(handle);
    return true;
}

void vfs_init(void) {
    index = kh_init(vfs_index);
    for (size_t i = 0; i < VFS_LOOKUP_DOMAIN_COUNT; i++) {
        lookup_caches[i] = kh_init(vfs_lookup_cache);
    }
    uid_cache = kh_init(vfs_lookup_cache);
    writable_cache = kh_init(vfs_lookup_cache);
    entries = NULL;
    entry_count = 0;
    entry_capacity = 0;
    InterlockedExchange(&frozen, FALSE);
    InterlockedExchange64(&generation, 0);
    InterlockedExchange64(&generation_sequence, 0);
    writable_entries = NULL;
    writable_count = 0;
    writable_capacity = 0;
}

void vfs_uninit(void) {
    if (entries != NULL) {
        for (size_t i = 0; i < entry_count; i++) {
            ml_mem_free(entries[i].key);
            ml_mem_free(entries[i].path);
            ml_mem_free(entries[i].uid);
        }
        ml_mem_free(entries);
    }
    for (size_t i = 0; i < VFS_LOOKUP_DOMAIN_COUNT; i++) {
        khash_t(vfs_lookup_cache) *cache = lookup_caches[i];
        if (cache != NULL) {
            for (khiter_t slot = kh_begin(cache); slot != kh_end(cache); slot++) {
                if (kh_exist(cache, slot)) ml_mem_free((void *)kh_key(cache, slot));
            }
            kh_destroy(vfs_lookup_cache, cache);
        }
        lookup_caches[i] = NULL;
    }
    if (uid_cache != NULL) {
        for (khiter_t slot = kh_begin(uid_cache); slot != kh_end(uid_cache); slot++) {
            if (kh_exist(uid_cache, slot)) ml_mem_free((void *)kh_key(uid_cache, slot));
        }
        kh_destroy(vfs_lookup_cache, uid_cache);
        uid_cache = NULL;
    }
    if (writable_cache != NULL) {
        for (khiter_t slot = kh_begin(writable_cache); slot != kh_end(writable_cache); slot++) {
            if (kh_exist(writable_cache, slot)) ml_mem_free((void *)kh_key(writable_cache, slot));
        }
        kh_destroy(vfs_lookup_cache, writable_cache);
        writable_cache = NULL;
    }
    if (index != NULL) kh_destroy(vfs_index, index);
    for (size_t i = 0; i < writable_count; i++) {
        ml_mem_free(writable_entries[i].key);
        ml_mem_free(writable_entries[i].path);
    }
    ml_mem_free(writable_entries);
    index = NULL;
    entries = NULL;
    entry_count = 0;
    entry_capacity = 0;
    InterlockedExchange(&frozen, FALSE);
    InterlockedExchange64(&generation, 0);
    InterlockedExchange64(&generation_sequence, 0);
    writable_entries = NULL;
    writable_count = 0;
    writable_capacity = 0;
}

static void clear_writable_cache_locked(void) {
    if (writable_cache == NULL) return;
    for (khiter_t slot = kh_begin(writable_cache); slot != kh_end(writable_cache); slot++) {
        if (kh_exist(writable_cache, slot)) ml_mem_free((void *)kh_key(writable_cache, slot));
    }
    kh_clear(vfs_lookup_cache, writable_cache);
}

bool vfs_add_package(const wchar_t *path) {
    if (index == NULL || InterlockedCompareExchange(&frozen, FALSE, FALSE) || path == NULL || !PathIsDirectoryW(path)) return false;
    return vfs_scan(path, L"");
}

bool vfs_register_writable_path(const wchar_t *virtual_path, const wchar_t *physical_path) {
    wchar_t *key;
    wchar_t *path;
    if (virtual_path == NULL || physical_path == NULL || physical_path[0] == L'\0' || !vfs_normalize_path(virtual_path, &key)) return false;
    if (key[0] == L'\0') {
        ml_mem_free(key);
        return false;
    }
    path = ml_mem_strdup_w(physical_path);
    if (path == NULL) {
        ml_mem_free(key);
        return false;
    }
    AcquireSRWLockExclusive(&writable_lock);
    for (size_t i = 0; i < writable_count; i++) {
        if (wcscmp(key, writable_entries[i].key) == 0) {
            bool same = CompareStringOrdinal(path, -1, writable_entries[i].path, -1, TRUE) == CSTR_EQUAL;
            ml_mem_free(path);
            ml_mem_free(key);
            ReleaseSRWLockExclusive(&writable_lock);
            return same;
        }
    }
    if (writable_count == writable_capacity) {
        size_t capacity = writable_capacity == 0 ? 8 : writable_capacity * 2;
        vfs_writable_entry_t *new_entries = writable_entries == NULL
            ? ml_mem_alloc(LMEM_ZEROINIT, capacity * sizeof(*writable_entries))
            : ml_mem_realloc(writable_entries, capacity * sizeof(*writable_entries), LMEM_MOVEABLE | LMEM_ZEROINIT);
        if (new_entries == NULL) {
            ml_mem_free(key);
            ml_mem_free(path);
            ReleaseSRWLockExclusive(&writable_lock);
            return false;
        }
        writable_entries = new_entries;
        writable_capacity = capacity;
    }
    writable_entries[writable_count++] = (vfs_writable_entry_t){ key, path };
    clear_writable_cache_locked();
    ReleaseSRWLockExclusive(&writable_lock);
    return true;
}

bool vfs_has_writable_paths(void) {
    bool result;
    AcquireSRWLockShared(&writable_lock);
    result = writable_count != 0;
    ReleaseSRWLockShared(&writable_lock);
    return result;
}

const wchar_t *vfs_lookup(const wchar_t *path) {
    return vfs_lookup_domain(path, VFS_LOOKUP_VIRTUAL);
}

const wchar_t *vfs_lookup_domain(const wchar_t *path, vfs_lookup_domain_t domain) {
    wchar_t *key;
    khiter_t slot;
    const wchar_t *result;
    uint64_t lookup_generation;
    khash_t(vfs_lookup_cache) *cache;
    if (index == NULL || path == NULL || domain < 0 || domain >= VFS_LOOKUP_DOMAIN_COUNT) return NULL;
    cache = lookup_caches[domain];
    if (cache == NULL) return NULL;

    result = vfs_uid_to_path(path);
    if (result != NULL) return result;

    lookup_generation = vfs_generation();
    if (lookup_generation != 0) {
        AcquireSRWLockShared(&lookup_cache_lock);
        slot = kh_get(vfs_lookup_cache, cache, path);
        if (slot != kh_end(cache)) {
            result = kh_value(cache, slot);
            ReleaseSRWLockShared(&lookup_cache_lock);
            return result;
        }
        ReleaseSRWLockShared(&lookup_cache_lock);
    }

    if (!vfs_normalize_path(path, &key)) return NULL;
    slot = kh_get(vfs_index, index, key);
    ml_mem_free(key);
    result = slot == kh_end(index) ? NULL : entries[kh_value(index, slot)].path;

    if (lookup_generation == 0) return result;
    wchar_t *cache_key = ml_mem_strdup_w(path);
    if (cache_key == NULL) return result;
    AcquireSRWLockExclusive(&lookup_cache_lock);
    if (vfs_generation() != lookup_generation) {
        ReleaseSRWLockExclusive(&lookup_cache_lock);
        ml_mem_free(cache_key);
        return result;
    }
    slot = kh_get(vfs_lookup_cache, cache, cache_key);
    if (slot != kh_end(cache)) {
        result = kh_value(cache, slot);
        ml_mem_free(cache_key);
    } else {
        int ret;
        slot = kh_put(vfs_lookup_cache, cache, cache_key, &ret);
        if (ret < 0) {
            ml_mem_free(cache_key);
        } else {
            kh_value(cache, slot) = result;
        }
    }
    ReleaseSRWLockExclusive(&lookup_cache_lock);
    return result;
}

uint64_t vfs_generation(void) {
    return (uint64_t)InterlockedCompareExchange64(&generation, 0, 0);
}

static void clear_lookup_cache_locked(void) {
    for (size_t i = 0; i < VFS_LOOKUP_DOMAIN_COUNT; i++) {
        khash_t(vfs_lookup_cache) *cache = lookup_caches[i];
        if (cache == NULL) continue;
        for (khiter_t slot = kh_begin(cache); slot != kh_end(cache); slot++) {
            if (kh_exist(cache, slot)) ml_mem_free((void *)kh_key(cache, slot));
        }
        kh_clear(vfs_lookup_cache, cache);
    }
    if (uid_cache != NULL) {
        for (khiter_t slot = kh_begin(uid_cache); slot != kh_end(uid_cache); slot++) {
            if (kh_exist(uid_cache, slot)) ml_mem_free((void *)kh_key(uid_cache, slot));
        }
        kh_clear(vfs_lookup_cache, uid_cache);
    }
}

void vfs_begin_lookup_reset(void) {
    InterlockedExchange(&frozen, TRUE);
    AcquireSRWLockExclusive(&lookup_cache_lock);
    InterlockedExchange64(&generation, 0);
    clear_lookup_cache_locked();
    ReleaseSRWLockExclusive(&lookup_cache_lock);
}

uint64_t vfs_reset_lookup_caches(void) {
    uint64_t next;
    AcquireSRWLockExclusive(&lookup_cache_lock);
    InterlockedExchange(&frozen, TRUE);
    clear_lookup_cache_locked();
    next = (uint64_t)InterlockedIncrement64(&generation_sequence);
    InterlockedExchange64(&generation, (LONG64)next);
    ReleaseSRWLockExclusive(&lookup_cache_lock);
    return next;
}

const wchar_t *vfs_virtual_to_uid(const wchar_t *path) {
    wchar_t *key;
    wchar_t *cache_key;
    khiter_t slot;
    const wchar_t *result;
    uint64_t lookup_generation;
    int ret;
    if (path == NULL || index == NULL || uid_cache == NULL) return NULL;
    lookup_generation = vfs_generation();
    if (lookup_generation != 0) {
        AcquireSRWLockShared(&lookup_cache_lock);
        slot = kh_get(vfs_lookup_cache, uid_cache, path);
        if (slot != kh_end(uid_cache)) {
            result = kh_value(uid_cache, slot);
            ReleaseSRWLockShared(&lookup_cache_lock);
            return result;
        }
        ReleaseSRWLockShared(&lookup_cache_lock);
    }

    if (!vfs_normalize_path(path, &key)) return NULL;
    slot = kh_get(vfs_index, index, key);
    ml_mem_free(key);
    result = slot == kh_end(index) ? NULL : entries[kh_value(index, slot)].uid;
    if (lookup_generation == 0) return result;
    cache_key = ml_mem_strdup_w(path);
    if (cache_key == NULL) return result;
    AcquireSRWLockExclusive(&lookup_cache_lock);
    if (vfs_generation() != lookup_generation) {
        ReleaseSRWLockExclusive(&lookup_cache_lock);
        ml_mem_free(cache_key);
        return result;
    }
    slot = kh_get(vfs_lookup_cache, uid_cache, cache_key);
    if (slot != kh_end(uid_cache)) {
        result = kh_value(uid_cache, slot);
        ml_mem_free(cache_key);
    } else {
        slot = kh_put(vfs_lookup_cache, uid_cache, cache_key, &ret);
        if (ret < 0) {
            ml_mem_free(cache_key);
        } else {
            kh_value(uid_cache, slot) = result;
        }
    }
    ReleaseSRWLockExclusive(&lookup_cache_lock);
    return result;
}

const wchar_t *vfs_virtual_to_uid_prefixed(const wchar_t *path, const wchar_t *game_root) {
    const wchar_t *relative;
    if (path == NULL || game_root == NULL) return NULL;
    relative = vfs_strip_path_prefix(path, game_root);
    return relative == NULL ? NULL : vfs_virtual_to_uid(relative);
}

const wchar_t *vfs_uid_to_path(const wchar_t *uid) {
    static const wchar_t prefix[] = L"\\\\me3??";
    wchar_t *end;
    unsigned long long index_value;
    if (uid == NULL || wcsncmp(uid, prefix, (sizeof(prefix) / sizeof(prefix[0])) - 1) != 0) return NULL;
    AcquireSRWLockShared(&lookup_cache_lock);
    uid += (sizeof(prefix) / sizeof(prefix[0])) - 1;
    if (!iswxdigit(*uid)) {
        ReleaseSRWLockShared(&lookup_cache_lock);
        return NULL;
    }
    index_value = wcstoull(uid, &end, 16);
    if (end == uid || *end != L'\0' || index_value >= entry_count) {
        ReleaseSRWLockShared(&lookup_cache_lock);
        return NULL;
    }
    const wchar_t *result = entries[index_value].path;
    ReleaseSRWLockShared(&lookup_cache_lock);
    return result;
}

const wchar_t *vfs_lookup_prefixed_domain(const wchar_t *path, const wchar_t *game_root,
                                          vfs_lookup_domain_t domain) {
    const wchar_t *relative = vfs_strip_path_prefix(path, game_root);
    return relative == NULL ? NULL : vfs_lookup_domain(relative, domain);
}

size_t vfs_entry_count(void) {
    return index != NULL ? kh_size(index) : 0;
}

bool vfs_has_wwise_entries(void) {
    if (index == NULL) return false;
    for (khiter_t slot = kh_begin(index); slot != kh_end(index); slot++) {
        const wchar_t *path;
        const wchar_t *extension;
        if (!kh_exist(index, slot)) continue;
        path = kh_key(index, slot);
        if (_wcsnicmp(path, L"sd\\", 3) != 0 && _wcsnicmp(path, L"sd_dlc02\\", 9) != 0) continue;
        extension = PathFindExtensionW(path);
        if (_wcsicmp(extension, L".bnk") == 0 || _wcsicmp(extension, L".wem") == 0) return true;
    }
    return false;
}

void vfs_recursion_guard_enter(void) {
    vfs_recursion_depth++;
}

void vfs_recursion_guard_leave(void) {
    if (vfs_recursion_depth != 0) vfs_recursion_depth--;
}

bool vfs_recursion_guard_active(void) {
    return vfs_recursion_depth != 0;
}

/* Last path segment, or NULL when the caller must take the slow path.
 *
 * A path can only match a writable entry when its normalized form equals the
 * entry key, which implies the normalized last segments are equal too:
 * vfs_normalize_path lowercases per character and never rewrites the content of
 * the final segment. The exceptions are a final segment of `.` or `..` (folded
 * into the parent), a trailing separator (the final segment moves left), and a
 * colon in the final segment (a drive-relative/device prefix is stripped before
 * the final segment is parsed); all return NULL here so those paths keep using
 * the full comparison. */
static const wchar_t *vfs_last_segment(const wchar_t *path) {
    const wchar_t *segment = path;
    for (const wchar_t *cursor = path; *cursor != L'\0'; cursor++) {
        if (*cursor == L'\\' || *cursor == L'/') segment = cursor + 1;
    }
    if (segment[0] == L'\0') return NULL;
    if (segment[0] == L'.' && (segment[1] == L'\0' || (segment[1] == L'.' && segment[2] == L'\0'))) return NULL;
    if (wcschr(segment, L':') != NULL) return NULL;
    return segment;
}

/* Cheap pre-filter for the writable table. Returns false only when `path`
 * provably matches no registered key, in which case the caller can skip the
 * normalization, the allocation and the negative cache insert. Must be called
 * with `writable_lock` held.
 *
 * `writable_entries` is append-only until vfs_uninit, so a false answer is exact
 * for every entry registered before the call. A registration that lands after it
 * is genuinely concurrent, and the only producer -- ml_save_mapping_route --
 * re-queries on the same thread after registering, so it always observes its own
 * entry. */
static bool vfs_writable_segment_may_match(const wchar_t *path) {
    const wchar_t *segment = vfs_last_segment(path);
    if (segment == NULL) return true;
    for (size_t i = 0; i < writable_count; i++) {
        const wchar_t *key_segment = vfs_last_segment(writable_entries[i].key);
        if (key_segment == NULL) return true;
        if (CompareStringOrdinal(segment, -1, key_segment, -1, TRUE) == CSTR_EQUAL) return true;
    }
    return false;
}

const wchar_t *vfs_route_writable_path(const wchar_t *path) {
    wchar_t *key;
    wchar_t *cache_key;
    khiter_t slot;
    int ret;
    const wchar_t *result = NULL;
    if (path == NULL || vfs_recursion_depth != 0 || writable_cache == NULL) return NULL;
    AcquireSRWLockShared(&writable_lock);
    if (writable_count == 0) {
        ReleaseSRWLockShared(&writable_lock);
        return NULL;
    }
    slot = kh_get(vfs_lookup_cache, writable_cache, path);
    if (slot != kh_end(writable_cache)) {
        result = kh_value(writable_cache, slot);
        ReleaseSRWLockShared(&writable_lock);
        return result;
    }
    /* Keep the cache bounded to paths that can actually match: without this the
     * cache would grow one strdup'd entry per distinct path the game ever opens,
     * and every first access would serialize on the exclusive lock below. */
    if (!vfs_writable_segment_may_match(path)) {
        ReleaseSRWLockShared(&writable_lock);
        return NULL;
    }
    ReleaseSRWLockShared(&writable_lock);

    if (!vfs_normalize_path(path, &key)) return NULL;
    cache_key = ml_mem_strdup_w(path);
    if (cache_key == NULL) {
        ml_mem_free(key);
        return NULL;
    }
    AcquireSRWLockExclusive(&writable_lock);
    slot = kh_get(vfs_lookup_cache, writable_cache, cache_key);
    if (slot != kh_end(writable_cache)) {
        result = kh_value(writable_cache, slot);
        ml_mem_free(cache_key);
        ReleaseSRWLockExclusive(&writable_lock);
        ml_mem_free(key);
        return result;
    }
    for (size_t i = 0; i < writable_count; i++) {
        if (wcscmp(key, writable_entries[i].key) == 0) {
            result = writable_entries[i].path;
            break;
        }
    }
    slot = kh_put(vfs_lookup_cache, writable_cache, cache_key, &ret);
    if (ret < 0) {
        ml_mem_free(cache_key);
    } else {
        kh_value(writable_cache, slot) = result;
    }
    ReleaseSRWLockExclusive(&writable_lock);
    ml_mem_free(key);
    return result;
}

/* No "is this already a package path" guard here: the physical paths in `entries`
 * are always absolute (a validated directory root joined with a relative child),
 * while every caller reaches this function with a game-root-relative path -- the
 * public entry point is vfs_route_read_path_prefixed, and vfs_strip_path_prefix
 * only returns when the next character is `\0`, `\` or `/`. An ordinal compare
 * between the two can never be equal, so such a guard would cost an O(entries)
 * Unicode comparison per file open and never fire. Re-entry is handled by
 * vfs_recursion_depth and by the vfs_uid_to_path fast path in
 * mods_file_route_read. */
const wchar_t *vfs_route_read_path(const wchar_t *path, DWORD desired_access, DWORD creation_disposition) {
    if (path == NULL || vfs_recursion_depth != 0) return NULL;
    const wchar_t *writable = vfs_route_writable_path(path);
    if (writable != NULL) return writable;
    if ((desired_access & (GENERIC_WRITE | DELETE)) != 0 ||
        creation_disposition == CREATE_NEW || creation_disposition == CREATE_ALWAYS ||
        creation_disposition == TRUNCATE_EXISTING) return NULL;
    return vfs_lookup_domain(path, VFS_LOOKUP_DISK_WIDE);
}

const wchar_t *vfs_route_read_path_prefixed(const wchar_t *path, const wchar_t *game_root,
                                            DWORD desired_access, DWORD creation_disposition) {
    const wchar_t *relative = vfs_strip_path_prefix(path, game_root);
    return relative == NULL ? NULL : vfs_route_read_path(relative, desired_access, creation_disposition);
}
