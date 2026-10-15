/*
 * Copyright (C) 2026, Soar Qin<soarchin@gmail.com>
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#include "dl_device.h"

#include "common/allocator.h"
#include "common/path.h"

#include "modloader/dl_allocator.h"
#include "process/pe.h"

#include <xxhash.h>
#include <miniz.h>
#include <miniz_tdef.h>
#include <miniz_tinfl.h>

#include <shlwapi.h>

#include <string.h>
#include <wchar.h>

static bool pointer_in_section(void *image_base, const IMAGE_SECTION_HEADER *section, const void *pointer) {
    return pointer != NULL && pe_section_contains_va(image_base, section, pointer);
}

static bool object_in_section(void *image_base, const IMAGE_SECTION_HEADER *section,
                              const void *pointer, size_t size) {
    uintptr_t base;
    uintptr_t start;
    uintptr_t end;
    uintptr_t object;
    if (image_base == NULL || section == NULL || pointer == NULL || size == 0) return false;
    base = (uintptr_t)image_base;
    start = base + pe_section_rva_start(section);
    end = base + pe_section_rva_end(section);
    object = (uintptr_t)pointer;
    return object >= start && object <= end && size <= end - object;
}

static bool vector_fields(const ml_dl_vector_t *vector, ml_stl_abi_t abi,
                          void **allocator, void **first, void **last, void **end) {
    if (vector == NULL || allocator == NULL || first == NULL || last == NULL || end == NULL) return false;
    if (abi == ML_STL_ABI_MSVC2012) {
        *allocator = vector->msvc2012.allocator;
        *first = vector->msvc2012.first;
        *last = vector->msvc2012.last;
        *end = vector->msvc2012.end;
        return true;
    }
    if (abi == ML_STL_ABI_MSVC2015) {
        *allocator = vector->msvc2015.allocator;
        *first = vector->msvc2015.first;
        *last = vector->msvc2015.last;
        *end = vector->msvc2015.end;
        return true;
    }
    return false;
}

static void vector_set(ml_dl_vector_t *vector, ml_stl_abi_t abi,
                       void *first, void *last, void *end) {
    if (abi == ML_STL_ABI_MSVC2012) {
        vector->msvc2012.first = first;
        vector->msvc2012.last = last;
        vector->msvc2012.end = end;
    } else {
        vector->msvc2015.first = first;
        vector->msvc2015.last = last;
        vector->msvc2015.end = end;
    }
}

static bool vector_valid(const ml_dl_vector_t *vector, ml_stl_abi_t abi, void *expected_allocator) {
    uintptr_t first;
    uintptr_t last;
    uintptr_t end;
    void *allocator;
    void *first_ptr;
    void *last_ptr;
    void *end_ptr;
    if (!vector_fields(vector, abi, &allocator, &first_ptr, &last_ptr, &end_ptr) ||
        allocator != expected_allocator) return false;
    first = (uintptr_t)first_ptr;
    last = (uintptr_t)last_ptr;
    end = (uintptr_t)end_ptr;
    return (first | last | end) % sizeof(void *) == 0 && first <= last && last <= end;
}

static bool manager_valid(void *image_base, const IMAGE_SECTION_HEADER *data,
                          const IMAGE_SECTION_HEADER *rdata, const ml_dl_device_manager_t *manager,
                          ml_stl_abi_t abi) {
    void *allocator;
    void *first;
    void *last;
    void *end;
    if (manager == NULL || ((uintptr_t)manager & (sizeof(void *) - 1)) != 0 ||
        !object_in_section(image_base, data, manager, sizeof(*manager))) return false;
    if (!vector_fields(&manager->devices, abi, &allocator, &first, &last, &end)) return false;
    /* patch_mem may replace Dantelion's original .data allocator with mimalloc. */
    if (allocator == NULL || ((uintptr_t)allocator & (sizeof(void *) - 1)) != 0) return false;
    if (!vector_valid(&manager->devices, abi, allocator) || !vector_valid(&manager->spis, abi, allocator) ||
        !vector_valid(&manager->virtual_roots, abi, allocator) ||
        !vector_valid(&manager->bnd3_mounts, abi, allocator) || !vector_valid(&manager->bnd4_mounts, abi, allocator)) return false;
    /* Every candidate pointer in .data gets here; dereference only last. */
    if (manager->bnd3_spi == NULL || manager->bnd4_spi == NULL ||
        !pointer_in_section(image_base, rdata, manager->mutex_vtable) ||
        manager->disk_device == NULL || ((uintptr_t)manager->disk_device & (sizeof(void *) - 1)) != 0) return false;
    return manager->disk_device->vtable != NULL &&
           pointer_in_section(image_base, rdata, manager->disk_device->vtable);
}

ml_dl_device_manager_t *ml_dl_device_manager_find(void *image_base, ml_stl_abi_t abi) {
    const IMAGE_SECTION_HEADER *data;
    const IMAGE_SECTION_HEADER *rdata;
    uintptr_t *pointers;
    size_t size = 0;
    if (image_base == NULL) return NULL;
    data = pe_section_by_name(image_base, ".data");
    rdata = pe_section_by_name(image_base, ".rdata");
    pointers = data == NULL ? NULL : pe_section_data(image_base, data, &size);
    if (pointers == NULL || rdata == NULL) return NULL;
    for (size_t i = 0; i < size / sizeof(*pointers); i++) {
        ml_dl_device_manager_t *manager = (ml_dl_device_manager_t *)pointers[i];
        if (manager_valid(image_base, data, rdata, manager, abi)) return manager;
    }
    return NULL;
}

bool ml_dl_device_manager_lock(ml_dl_device_manager_t *manager, ml_dl_device_manager_guard_t *guard) {
    if (manager == NULL || guard == NULL) return false;
    EnterCriticalSection(&manager->critical_section);
    guard->manager = manager;
    return true;
}

void ml_dl_device_manager_unlock(ml_dl_device_manager_guard_t *guard) {
    if (guard != NULL && guard->manager != NULL) {
        LeaveCriticalSection(&guard->manager->critical_section);
        guard->manager = NULL;
    }
}

const wchar_t *ml_dl_string_data(const ml_dl_string_t *value, ml_stl_abi_t abi) {
    const wchar_t *inline_storage;
    const wchar_t *large;
    size_t length;
    size_t capacity;
    uint8_t encoding;
    if (value == NULL) return NULL;
    if (abi == ML_STL_ABI_MSVC2012) {
        inline_storage = (const wchar_t *)value->msvc2012.storage.inline_storage;
        large = value->msvc2012.storage.large;
        length = value->msvc2012.length;
        capacity = value->msvc2012.capacity;
        encoding = value->msvc2012.encoding;
    } else if (abi == ML_STL_ABI_MSVC2015) {
        inline_storage = (const wchar_t *)value->msvc2015.storage.inline_storage;
        large = value->msvc2015.storage.large;
        length = value->msvc2015.length;
        capacity = value->msvc2015.capacity;
        encoding = value->msvc2015.encoding;
    } else {
        return NULL;
    }
    if (encoding != 1 || length > capacity) return NULL;
    return capacity > 7 ? large : inline_storage;
}

size_t ml_dl_vector_count(const ml_dl_vector_t *vector, ml_stl_abi_t abi, size_t item_size) {
    uintptr_t first;
    uintptr_t last;
    void *allocator;
    void *first_ptr;
    void *last_ptr;
    void *end_ptr;
    if (vector == NULL || item_size == 0) return 0;
    if (!vector_fields(vector, abi, &allocator, &first_ptr, &last_ptr, &end_ptr)) return 0;
    first = (uintptr_t)first_ptr;
    last = (uintptr_t)last_ptr;
    return last >= first && (last - first) % item_size == 0 ? (last - first) / item_size : 0;
}

bool ml_dl_device_expand_path(ml_dl_device_manager_t *manager, ml_stl_abi_t abi,
                              const wchar_t *path, wchar_t **expanded) {
    wchar_t *current;
    if (manager == NULL || path == NULL || expanded == NULL) return false;
    *expanded = NULL;
    current = ml_mem_strdup_w(path);
    if (current == NULL) return false;
    for (size_t depth = 0; depth != 16; depth++) {
        size_t count = ml_dl_vector_count(&manager->virtual_roots, abi, sizeof(ml_dl_virtual_root_t));
        bool replaced = false;
        const wchar_t *separator = wcsstr(current, L":/");
        size_t root_len;
        if (separator == NULL) { *expanded = current; return true; }
        root_len = (size_t)(separator - current);
        for (size_t i = 0; i < count; i++) {
            void *allocator;
            void *first;
            void *last;
            void *end;
            ml_dl_virtual_root_t *root;
            const wchar_t *from;
            const wchar_t *to;
            size_t from_len;
            size_t to_len;
            size_t remainder_len;
            wchar_t *next;
            if (!vector_fields(&manager->virtual_roots, abi, &allocator, &first, &last, &end)) break;
            root = &((ml_dl_virtual_root_t *)first)[i];
            from = ml_dl_string_data(&root->root, abi);
            to = ml_dl_string_data(&root->expanded, abi);
            from_len = abi == ML_STL_ABI_MSVC2012 ? root->root.msvc2012.length : root->root.msvc2015.length;
            to_len = abi == ML_STL_ABI_MSVC2012 ? root->expanded.msvc2012.length : root->expanded.msvc2015.length;
            if (from == NULL || to == NULL || from_len == 0 || from_len != root_len ||
                memcmp(current, from, from_len * sizeof(*current)) != 0) continue;
            remainder_len = wcslen(current + from_len + 2);
            next = ml_mem_alloc(0, (to_len + remainder_len + 1) * sizeof(*next));
            if (next == NULL) { ml_mem_free(current); return false; }
            memcpy(next, to, to_len * sizeof(*next));
            memcpy(next + to_len, current + from_len + 2, (remainder_len + 1) * sizeof(*next));
            ml_mem_free(current);
            current = next;
            replaced = true;
            break;
        }
        if (!replaced) { *expanded = current; return true; }
    }
    ml_mem_free(current);
    return false;
}

bool ml_dl_string_clone_replace(const ml_dl_string_t *source, ml_stl_abi_t abi,
                                const wchar_t *path, ml_dl_string_t *replacement) {
    size_t length;
    wchar_t *buffer;
    dl_allocator_t *allocator;
    uint8_t encoding;
    if (source == NULL || path == NULL || replacement == NULL) return false;
    if (abi == ML_STL_ABI_MSVC2012) {
        encoding = source->msvc2012.encoding;
        allocator = source->msvc2012.allocator;
    } else if (abi == ML_STL_ABI_MSVC2015) {
        encoding = source->msvc2015.encoding;
        allocator = source->msvc2015.allocator;
    } else {
        return false;
    }
    if (encoding != 1) return false;
    if (allocator == NULL || allocator->vtable == NULL || allocator->vtable->allocate_aligned == NULL) return false;
    length = wcslen(path);
    memset(replacement, 0, sizeof(*replacement));
    if (abi == ML_STL_ABI_MSVC2012) {
        replacement->msvc2012.allocator = allocator;
        replacement->msvc2012.capacity = 7;
        replacement->msvc2012.encoding = 1;
        buffer = length <= 7 ? (wchar_t *)replacement->msvc2012.storage.inline_storage : NULL;
    } else {
        replacement->msvc2015.allocator = allocator;
        replacement->msvc2015.capacity = 7;
        replacement->msvc2015.encoding = 1;
        buffer = length <= 7 ? (wchar_t *)replacement->msvc2015.storage.inline_storage : NULL;
    }
    if (buffer == NULL) {
        if (length == SIZE_MAX) return false;
        buffer = allocator->vtable->allocate_aligned(allocator, (length + 1) * sizeof(*buffer), _Alignof(wchar_t));
        if (buffer == NULL) return false;
        if (abi == ML_STL_ABI_MSVC2012) {
            replacement->msvc2012.storage.large = buffer;
            replacement->msvc2012.capacity = length;
        } else {
            replacement->msvc2015.storage.large = buffer;
            replacement->msvc2015.capacity = length;
        }
    }
    memcpy(buffer, path, (length + 1) * sizeof(*buffer));
    if (abi == ML_STL_ABI_MSVC2012) replacement->msvc2012.length = length;
    else replacement->msvc2015.length = length;
    return true;
}

bool ml_dl_mount_snapshot(ml_dl_device_manager_t *manager, ml_stl_abi_t abi,
                          ml_dl_mount_snapshot_t *snapshot) {
    size_t count;
    if (manager == NULL || snapshot == NULL) return false;
    memset(snapshot, 0, sizeof(*snapshot));
    count = ml_dl_vector_count(&manager->bnd4_mounts, abi, sizeof(ml_dl_virtual_mount_t));
    if (count == 0) return true;
    snapshot->roots = ml_mem_alloc(0, count * sizeof(*snapshot->roots));
    if (snapshot->roots == NULL) return false;
    for (size_t i = 0; i < count; i++) {
        void *allocator;
        void *first;
        void *last;
        void *end;
        ml_dl_virtual_mount_t *mount;
        const wchar_t *root;
        if (!vector_fields(&manager->bnd4_mounts, abi, &allocator, &first, &last, &end)) {
            ml_dl_mount_snapshot_destroy(snapshot);
            return false;
        }
        mount = &((ml_dl_virtual_mount_t *)first)[i];
        root = ml_dl_string_data(&mount->root, abi);
        snapshot->roots[i] = root == NULL ? NULL : ml_mem_strdup_w(root);
        if (root != NULL && snapshot->roots[i] == NULL) {
            ml_dl_mount_snapshot_destroy(snapshot);
            return false;
        }
        snapshot->count = i + 1;
    }
    return true;
}

void ml_dl_mount_snapshot_destroy(ml_dl_mount_snapshot_t *snapshot) {
    if (snapshot == NULL) return;
    for (size_t i = 0; i < snapshot->count; i++) ml_mem_free(snapshot->roots[i]);
    ml_mem_free(snapshot->roots);
    memset(snapshot, 0, sizeof(*snapshot));
}

bool ml_dl_mounts_init(ml_dl_vfs_mounts_t *mounts) {
    if (mounts == NULL) return false;
    memset(mounts, 0, sizeof(*mounts));
    return true;
}

static bool snapshot_contains(const ml_dl_mount_snapshot_t *snapshot, const wchar_t *root) {
    if (snapshot == NULL || root == NULL) return false;
    for (size_t i = 0; i < snapshot->count; i++) {
        if (snapshot->roots[i] != NULL && wcscmp(snapshot->roots[i], root) == 0) return true;
    }
    return false;
}

static bool mounts_reserve(ml_dl_vfs_mounts_t *mounts, size_t capacity) {
    ml_dl_virtual_mount_t *items;
    if (mounts == NULL || capacity > SIZE_MAX / sizeof(*items)) return false;
    if (capacity <= mounts->capacity) return true;
    items = ml_mem_alloc(0, capacity * sizeof(*items));
    if (items == NULL) return false;
    memcpy(items, mounts->items, mounts->count * sizeof(*items));
    ml_mem_free(mounts->items);
    mounts->items = items;
    mounts->capacity = capacity;
    return true;
}

static bool vector_reserve(ml_dl_vector_t *vector, ml_stl_abi_t abi, size_t count, size_t item_size) {
    size_t capacity;
    size_t used;
    dl_allocator_t *allocator;
    void *items;
    if (vector == NULL || item_size == 0 || count > SIZE_MAX / item_size) return false;
    void *raw_allocator;
    void *first;
    void *last;
    void *end;
    if (!vector_fields(vector, abi, &raw_allocator, &first, &last, &end)) return false;
    capacity = first == NULL || end == NULL ? 0 :
               (size_t)((uint8_t *)end - (uint8_t *)first) / item_size;
    used = ml_dl_vector_count(vector, abi, item_size);
    if (count <= capacity) return true;
    allocator = (dl_allocator_t *)raw_allocator;
    if (allocator == NULL || allocator->vtable == NULL || allocator->vtable->allocate_aligned == NULL) return false;
    items = allocator->vtable->allocate_aligned(allocator, count * item_size, sizeof(void *));
    if (items == NULL) return false;
    if (used != 0 && first != NULL) memcpy(items, first, used * item_size);
    if (first != NULL && allocator->vtable->free != NULL) allocator->vtable->free(allocator, first);
    vector_set(vector, abi, items, (uint8_t *)items + used * item_size,
               (uint8_t *)items + count * item_size);
    return true;
}

bool ml_dl_device_extract_new(ml_dl_device_manager_t *manager, ml_stl_abi_t abi,
                              const ml_dl_mount_snapshot_t *snapshot, ml_dl_vfs_mounts_t *mounts) {
    size_t mount_count;
    size_t new_count = 0;
    if (manager == NULL || snapshot == NULL || mounts == NULL) return false;
    mount_count = ml_dl_vector_count(&manager->bnd4_mounts, abi, sizeof(ml_dl_virtual_mount_t));
    for (size_t i = 0; i < mount_count; i++) {
        void *allocator;
        void *first;
        void *last;
        void *end;
        ml_dl_virtual_mount_t *items;
        const wchar_t *root;
        if (!vector_fields(&manager->bnd4_mounts, abi, &allocator, &first, &last, &end)) return false;
        items = (ml_dl_virtual_mount_t *)first;
        root = ml_dl_string_data(&items[i].root, abi);
        if (root != NULL && !snapshot_contains(snapshot, root)) new_count++;
    }
    /* MountEbl has already grown the game-owned vectors.  Detaching mounts only
       shortens last; avoid reallocating those ABI-owned vectors here. */
    if (new_count > SIZE_MAX - mounts->count ||
        !mounts_reserve(mounts, mounts->count + new_count)) return false;
    for (size_t i = mount_count; i != 0; i--) {
        void *allocator;
        void *first;
        void *last;
        void *end;
        ml_dl_virtual_mount_t *items;
        if (!vector_fields(&manager->bnd4_mounts, abi, &allocator, &first, &last, &end)) return false;
        items = (ml_dl_virtual_mount_t *)first;
        ml_dl_virtual_mount_t mount = items[i - 1];
        const wchar_t *root = ml_dl_string_data(&mount.root, abi);
        if (root == NULL || snapshot_contains(snapshot, root)) continue;
        memmove(mounts->items + 1, mounts->items, mounts->count * sizeof(*mounts->items));
        mounts->items[0] = mount;
        mounts->count++;
        memmove(items + i - 1, items + i, (mount_count - i) * sizeof(*items));
        mount_count--;
    }
    {
        void *allocator;
        void *first;
        void *last;
        void *end;
        size_t device_count = ml_dl_vector_count(&manager->devices, abi, sizeof(ml_dl_device_t *));
        ml_dl_device_t **devices;
        if (!vector_fields(&manager->devices, abi, &allocator, &first, &last, &end)) return false;
        devices = (ml_dl_device_t **)first;
        for (size_t i = device_count; i != 0; i--) {
            bool used = false;
            for (size_t j = 0; j < mounts->count; j++) if (mounts->items[j].device == devices[i - 1]) used = true;
            if (used) memmove(devices + i - 1, devices + i, (device_count - i) * sizeof(*devices)), device_count--;
        }
        vector_set(&manager->devices, abi, devices,
                   devices == NULL ? NULL : devices + device_count, end);
    }
    {
        void *allocator;
        void *first;
        void *last;
        void *end;
        if (!vector_fields(&manager->bnd4_mounts, abi, &allocator, &first, &last, &end)) return false;
        vector_set(&manager->bnd4_mounts, abi, first,
                   first == NULL ? NULL : (uint8_t *)first + mount_count * sizeof(ml_dl_virtual_mount_t), end);
    }
    return true;
}

void ml_dl_mounts_destroy(ml_dl_vfs_mounts_t *mounts, ml_stl_abi_t abi) {
    if (mounts == NULL) return;
    for (size_t i = 0; i < mounts->count; i++) ml_dl_string_destroy(&mounts->items[i].root, abi);
    ml_mem_free(mounts->items);
    memset(mounts, 0, sizeof(*mounts));
}

void ml_dl_mounts_release(ml_dl_vfs_mounts_t *mounts) {
    if (mounts == NULL) return;
    ml_mem_free(mounts->items);
    memset(mounts, 0, sizeof(*mounts));
}

bool ml_dl_device_push_mounts_permanent(ml_dl_device_manager_t *manager, ml_stl_abi_t abi,
                                        ml_dl_vfs_mounts_t *mounts) {
    size_t device_count;
    size_t mount_count;
    size_t required_devices;
    size_t required_mounts;
    if (manager == NULL || mounts == NULL) return false;
    device_count = ml_dl_vector_count(&manager->devices, abi, sizeof(ml_dl_device_t *));
    mount_count = ml_dl_vector_count(&manager->bnd4_mounts, abi, sizeof(ml_dl_virtual_mount_t));
    if (mounts->count == 0) return true;
    if (mounts->count > SIZE_MAX - device_count || mounts->count > SIZE_MAX - mount_count) return false;
    required_devices = device_count + mounts->count;
    required_mounts = mount_count + mounts->count;
    if (!vector_reserve(&manager->devices, abi, required_devices, sizeof(ml_dl_device_t *)) ||
        !vector_reserve(&manager->bnd4_mounts, abi, required_mounts, sizeof(ml_dl_virtual_mount_t))) return false;
    void *devices_allocator;
    void *devices_first;
    void *devices_last;
    void *devices_end;
    void *mounts_allocator;
    void *mounts_first;
    void *mounts_last;
    void *mounts_end;
    if (!vector_fields(&manager->devices, abi, &devices_allocator, &devices_first, &devices_last, &devices_end) ||
        !vector_fields(&manager->bnd4_mounts, abi, &mounts_allocator, &mounts_first, &mounts_last, &mounts_end)) return false;
    for (size_t i = 0; i < mounts->count; i++) {
        ((ml_dl_device_t **)devices_first)[device_count + i] = mounts->items[i].device;
        ((ml_dl_virtual_mount_t *)mounts_first)[mount_count + i] = mounts->items[i];
    }
    vector_set(&manager->devices, abi, devices_first,
               (uint8_t *)devices_first + required_devices * sizeof(ml_dl_device_t *), devices_end);
    vector_set(&manager->bnd4_mounts, abi, mounts_first,
               (uint8_t *)mounts_first + required_mounts * sizeof(ml_dl_virtual_mount_t), mounts_end);
    ml_mem_free(mounts->items);
    memset(mounts, 0, sizeof(*mounts));
    return true;
}

void ml_dl_string_destroy(ml_dl_string_t *string, ml_stl_abi_t abi) {
    dl_allocator_t *allocator;
    void *large;
    size_t capacity;
    if (string == NULL) return;
    if (abi == ML_STL_ABI_MSVC2012) {
        allocator = string->msvc2012.allocator;
        large = string->msvc2012.storage.large;
        capacity = string->msvc2012.capacity;
    } else if (abi == ML_STL_ABI_MSVC2015) {
        allocator = string->msvc2015.allocator;
        large = string->msvc2015.storage.large;
        capacity = string->msvc2015.capacity;
    } else {
        return;
    }
    if (capacity > 7 && large != NULL) {
        dl_allocator_dealloc(allocator, large);
    }
    memset(string, 0, sizeof(*string));
}

bool ml_bhd5_header_valid(const void *data, size_t size) {
    const uint8_t *bytes = data;
    uint32_t file_size;
    uint32_t bucket_count;
    uint32_t bucket_offset;
    if (bytes == NULL || size < 24 || memcmp(bytes, "BHD5", 4) != 0) return false;
    memcpy(&file_size, bytes + 12, sizeof(file_size));
    memcpy(&bucket_count, bytes + 16, sizeof(bucket_count));
    memcpy(&bucket_offset, bytes + 20, sizeof(bucket_offset));
    return file_size == size && bucket_offset <= file_size &&
           bucket_count <= (file_size - bucket_offset) / sizeof(uint32_t);
}

bool ml_boot_boost_cache_key(const void *data, size_t size, uint64_t key[2]) {
    const uint8_t *bytes = data;
    XXH128_hash_t hash;
    if (bytes == NULL || key == NULL) return false;
    hash = XXH3_128bits_withSeed(bytes, size, 1);
    key[0] = hash.low64;
    key[1] = hash.high64;
    return true;
}

wchar_t *ml_boot_boost_cache_path(const wchar_t *directory, const uint64_t key[2]) {
    int suffix_length;
    size_t directory_length;
    bool separator;
    wchar_t *result;
    if (directory == NULL || key == NULL) return NULL;
    directory_length = wcslen(directory);
    separator = directory_length != 0 && directory[directory_length - 1] != L'\\' &&
                directory[directory_length - 1] != L'/';
    suffix_length = _scwprintf(L"%016llx%016llx.bhd.zz", (unsigned long long)key[1], (unsigned long long)key[0]);
    if (suffix_length < 0 || directory_length > SIZE_MAX - (size_t)suffix_length - (separator ? 2 : 1)) return NULL;
    result = ml_mem_alloc(0, (directory_length + (size_t)suffix_length + (separator ? 2 : 1)) * sizeof(*result));
    if (result == NULL) return NULL;
    memcpy(result, directory, directory_length * sizeof(*result));
    if (separator) result[directory_length++] = L'\\';
    _snwprintf(result + directory_length, (size_t)suffix_length + 1, L"%016llx%016llx.bhd.zz",
               (unsigned long long)key[1], (unsigned long long)key[0]);
    if (wcslen(result) >= MAX_PATH - 12 && ml_path_is_absolute(result) && wcsncmp(result, L"\\\\?\\", 4) != 0) {
        wchar_t *extended = ml_path_extended_alloc(result);
        ml_mem_free(result);
        return extended;
    }
    return result;
}

typedef struct ml_boot_boost_cache_header_s {
    uint8_t magic[8];
    uint32_t version;
    uint32_t header_size;
    uint64_t key[2];
    uint32_t uncompressed_size;
    uint32_t compressed_size;
    uint64_t checksum;
} ml_boot_boost_cache_header_t;

_Static_assert(sizeof(ml_boot_boost_cache_header_t) == 48, "BootBoost cache header size");

static const uint8_t boot_boost_cache_magic[8] = { 'Y', 'A', 'F', 'S', 'B', 'H', 'D', 0 };
static const uint32_t boot_boost_cache_version = 1;

static uint64_t boot_boost_cache_checksum(const void *data, size_t size) {
    return XXH3_128bits_withSeed(data, size, 2).low64;
}

bool ml_boot_boost_cache_load(const wchar_t *path, const uint64_t key[2], void *output, size_t output_size) {
    HANDLE file = INVALID_HANDLE_VALUE;
    DWORD read;
    ml_boot_boost_cache_header_t header;
    LARGE_INTEGER file_size;
    uint8_t *compressed = NULL;
    size_t compressed_size;
    bool valid = false;
    if (path == NULL || key == NULL || output == NULL || output_size > UINT32_MAX) return false;
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return false;
    if (!GetFileSizeEx(file, &file_size) || file_size.QuadPart < sizeof(header) || file_size.QuadPart > SIZE_MAX) goto done;
    if (!ReadFile(file, &header, sizeof(header), &read, NULL) || read != sizeof(header) ||
        memcmp(header.magic, boot_boost_cache_magic, sizeof(header.magic)) != 0 ||
        header.version != boot_boost_cache_version || header.header_size != sizeof(header) ||
        header.key[0] != key[0] || header.key[1] != key[1] ||
        header.uncompressed_size != output_size ||
        header.compressed_size != (uint64_t)file_size.QuadPart - sizeof(header)) goto done;
    compressed_size = header.compressed_size;
    if (compressed_size == 0) goto done;
    compressed = ml_mem_alloc(0, compressed_size);
    if (compressed == NULL) goto done;
    if (ReadFile(file, compressed, (DWORD)compressed_size, &read, NULL) && read == compressed_size) {
        tinfl_decompressor decompressor;
        size_t input_size = compressed_size;
        size_t uncompressed_size = output_size;
        tinfl_init(&decompressor);
        valid = tinfl_decompress(&decompressor, compressed, &input_size, output, output, &uncompressed_size,
                                 TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF) == TINFL_STATUS_DONE &&
                input_size == compressed_size && uncompressed_size == output_size &&
                boot_boost_cache_checksum(output, output_size) == header.checksum &&
                ml_bhd5_header_valid(output, output_size);
    }
    ml_mem_free(compressed);
done:
    CloseHandle(file);
    if (!valid) DeleteFileW(path);
    return valid;
}

bool ml_boot_boost_cache_store(const wchar_t *path, const uint64_t key[2], const void *data, size_t size) {
    wchar_t *temporary;
    HANDLE file = INVALID_HANDLE_VALUE;
    size_t bound;
    size_t compressed_size;
    uint8_t *compressed;
    ml_boot_boost_cache_header_t header;
    DWORD written;
    bool result = false;
    int temporary_length;
    if (path == NULL || key == NULL || data == NULL || size > UINT32_MAX || !ml_bhd5_header_valid(data, size)) return false;
    temporary_length = _scwprintf(L"%ls.%08lx.%08lx.tmp", path,
                                  GetCurrentProcessId(), GetCurrentThreadId());
    if (temporary_length < 0) return false;
    temporary = ml_mem_alloc(0, ((size_t)temporary_length + 1) * sizeof(*temporary));
    if (temporary == NULL) return false;
    _snwprintf(temporary, (size_t)temporary_length + 1, L"%ls.%08lx.%08lx.tmp", path,
               GetCurrentProcessId(), GetCurrentThreadId());
    bound = mz_compressBound((mz_ulong)size);
    compressed = ml_mem_alloc(0, bound);
    if (compressed == NULL) { ml_mem_free(temporary); return false; }
    compressed_size = tdefl_compress_mem_to_mem(compressed, bound, data, size,
        tdefl_create_comp_flags_from_zip_params(7, -MZ_DEFAULT_WINDOW_BITS, MZ_DEFAULT_STRATEGY));
    if (compressed_size == 0 || compressed_size > MAXDWORD) goto done;
    memset(&header, 0, sizeof(header));
    memcpy(header.magic, boot_boost_cache_magic, sizeof(header.magic));
    header.version = boot_boost_cache_version;
    header.header_size = sizeof(header);
    header.key[0] = key[0];
    header.key[1] = key[1];
    header.uncompressed_size = (uint32_t)size;
    header.compressed_size = (uint32_t)compressed_size;
    header.checksum = boot_boost_cache_checksum(data, size);
    file = CreateFileW(temporary, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) goto done;
    if (WriteFile(file, &header, sizeof(header), &written, NULL) && written == sizeof(header) &&
        WriteFile(file, compressed, (DWORD)compressed_size, &written, NULL) && written == (DWORD)compressed_size) {
        if (!FlushFileBuffers(file)) goto done;
        CloseHandle(file);
        file = INVALID_HANDLE_VALUE;
        result = MoveFileExW(temporary, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    }
done:
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (!result) DeleteFileW(temporary);
    ml_mem_free(compressed);
    ml_mem_free(temporary);
    return result;
}
