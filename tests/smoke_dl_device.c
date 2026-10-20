#include <stdint.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "test_common.h"
#include "modloader/dl_allocator.h"
#include "modloader/patches/dl_device.h"

static size_t allocated;
static size_t freed;
static size_t permanent_freed;

typedef struct cache_store_thread_s {
    const wchar_t *path;
    const uint64_t *key;
    const uint8_t *data;
    size_t size;
    bool result;
} cache_store_thread_t;

static DWORD WINAPI cache_store_thread(void *parameter) {
    cache_store_thread_t *context = parameter;
    context->result = ml_boot_boost_cache_store(context->path, context->key, context->data, context->size);
    return 0;
}

static void *__cdecl test_allocate_aligned(dl_allocator_t *self, size_t size, size_t alignment) {
    (void)self;
    (void)alignment;
    allocated++;
    return malloc(size);
}

static void __cdecl test_free(dl_allocator_t *self, void *ptr) {
    (void)self;
    freed++;
    free(ptr);
}

static void __cdecl test_permanent_free(dl_allocator_t *self, void *ptr) {
    (void)self;
    permanent_freed++;
    free(ptr);
}

int main(void) {
    uint8_t header[64] = { 0 };
    uint8_t expected_header[24];
    const uint64_t game_key[2] = { 1, 2 };
    uint64_t first[2];
    uint64_t second[2];
    wchar_t temporary[MAX_PATH];
    uint32_t file_size = 64;
    uint32_t bucket_count = 2;
    uint32_t bucket_offset = 32;
    ml_dl_virtual_root_t roots[2] = { 0 };
    ml_dl_device_manager_t manager = { 0 };
    wchar_t *expanded = NULL;
    wchar_t *cache_path;
    dl_allocator_vtable_t allocator_vtable = { 0 };
    dl_allocator_t allocator = { &allocator_vtable };
    ml_dl_string_t source = { 0 };
    ml_dl_string_t replacement;

    allocator_vtable.allocate_aligned = test_allocate_aligned;
    allocator_vtable.free = test_free;
    source.msvc2015.allocator = &allocator;
    memcpy(source.msvc2015.storage.inline_storage, L"data:/", 14);
    source.msvc2015.length = 6;
    source.msvc2015.capacity = 7;
    source.msvc2015.encoding = 1;
    EXPECT_TRUE(ml_dl_string_clone_replace(&source, ML_STL_ABI_MSVC2015, L"short", &replacement));
    EXPECT_STREQ_W(ml_dl_string_data(&replacement, ML_STL_ABI_MSVC2015), L"short");
    EXPECT_STREQ_W(ml_dl_string_data(&source, ML_STL_ABI_MSVC2015), L"data:/");
    ml_dl_string_destroy(&replacement, ML_STL_ABI_MSVC2015);
    EXPECT_EQ(allocated, 0);
    EXPECT_EQ(freed, 0);
    EXPECT_TRUE(ml_dl_string_clone_replace(&source, ML_STL_ABI_MSVC2015, L"\\me3??123456789", &replacement));
    EXPECT_STREQ_W(ml_dl_string_data(&replacement, ML_STL_ABI_MSVC2015), L"\\me3??123456789");
    EXPECT_STREQ_W(ml_dl_string_data(&source, ML_STL_ABI_MSVC2015), L"data:/");
    EXPECT_EQ(allocated, 1);
    ml_dl_string_destroy(&replacement, ML_STL_ABI_MSVC2015);
    EXPECT_EQ(freed, 1);
    {
        ml_dl_string_t source2012 = { 0 };
        source2012.msvc2012.allocator = &allocator;
        memcpy(source2012.msvc2012.storage.inline_storage, L"data:/", 14);
        source2012.msvc2012.length = 6;
        source2012.msvc2012.capacity = 7;
        source2012.msvc2012.encoding = 1;
        EXPECT_TRUE(ml_dl_string_clone_replace(&source2012, ML_STL_ABI_MSVC2012,
                                               L"\\me3??123456789", &replacement));
        EXPECT_STREQ_W(ml_dl_string_data(&replacement, ML_STL_ABI_MSVC2012), L"\\me3??123456789");
        EXPECT_STREQ_W(ml_dl_string_data(&source2012, ML_STL_ABI_MSVC2012), L"data:/");
        EXPECT_EQ(replacement.msvc2012.allocator, &allocator);
        ml_dl_string_destroy(&replacement, ML_STL_ABI_MSVC2012);
        EXPECT_EQ(freed, 2);
    }

    memcpy(header, "BHD5", 4);
    memcpy(header + 12, &file_size, sizeof(file_size));
    memcpy(header + 16, &bucket_count, sizeof(bucket_count));
    memcpy(header + 20, &bucket_offset, sizeof(bucket_offset));
    memcpy(expected_header, header, sizeof(expected_header));
    EXPECT_TRUE(ml_bhd5_header_valid(header, sizeof(header)));
    EXPECT_TRUE(ml_boot_boost_cache_key(header, sizeof(header), game_key, "test RSA key", first));
    EXPECT_TRUE(ml_boot_boost_cache_key(header, sizeof(header), game_key, "test RSA key", second));
    EXPECT_EQ(first[0], second[0]);
    EXPECT_EQ(first[1], second[1]);
    {
        wchar_t directory[600];
        for (size_t i = 0; i < 599; i++) directory[i] = L'a';
        directory[599] = L'\0';
        const uint64_t key[2] = { UINT64_C(0x1234), UINT64_C(0x5678) };
        cache_path = ml_boot_boost_cache_path(directory, key);
        EXPECT_NOT_NULL(cache_path);
        EXPECT_EQ(wcslen(cache_path), 599 + 1 + 32 + 7);
        EXPECT_TRUE(wcsstr(cache_path, L"00000000000056780000000000001234.bhd.zz") != NULL);
        LocalFree(cache_path);
    }
    EXPECT_TRUE(GetTempPathW(MAX_PATH, temporary) != 0);
    EXPECT_TRUE(GetTempFileNameW(temporary, L"bbd", 0, temporary) != 0);
    DeleteFileW(temporary);
    EXPECT_TRUE(ml_boot_boost_cache_store(temporary, first, header, sizeof(header)));
    memset(header, 0, sizeof(header));
    EXPECT_TRUE(ml_boot_boost_cache_load(temporary, first, expected_header, header, sizeof(header)));
    DeleteFileW(temporary);
    EXPECT_TRUE(ml_boot_boost_cache_store(temporary, first, header, sizeof(header)));
    {
        HANDLE file = CreateFileW(temporary, FILE_APPEND_DATA, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        const uint8_t trailing = 0;
        DWORD written;
        EXPECT_TRUE(file != INVALID_HANDLE_VALUE);
        EXPECT_TRUE(WriteFile(file, &trailing, sizeof(trailing), &written, NULL));
        EXPECT_EQ(written, sizeof(trailing));
        CloseHandle(file);
    }
    EXPECT_TRUE(!ml_boot_boost_cache_load(temporary, first, expected_header, header, sizeof(header)));
    EXPECT_EQ(GetFileAttributesW(temporary), INVALID_FILE_ATTRIBUTES);
    EXPECT_TRUE(ml_boot_boost_cache_store(temporary, first, header, sizeof(header)));
    second[0] ^= 1;
    EXPECT_TRUE(!ml_boot_boost_cache_load(temporary, second, expected_header, header, sizeof(header)));
    second[0] ^= 1;
    EXPECT_EQ(GetFileAttributesW(temporary), INVALID_FILE_ATTRIBUTES);
    EXPECT_TRUE(ml_boot_boost_cache_store(temporary, first, header, sizeof(header)));
    {
        HANDLE file = CreateFileW(temporary, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        LARGE_INTEGER end;
        EXPECT_TRUE(file != INVALID_HANDLE_VALUE);
        EXPECT_TRUE(GetFileSizeEx(file, &end));
        EXPECT_TRUE(end.QuadPart > 1);
        end.QuadPart--;
        EXPECT_TRUE(SetFilePointerEx(file, end, NULL, FILE_BEGIN));
        EXPECT_TRUE(SetEndOfFile(file));
        CloseHandle(file);
    }
    EXPECT_TRUE(!ml_boot_boost_cache_load(temporary, first, expected_header, header, sizeof(header)));
    EXPECT_EQ(GetFileAttributesW(temporary), INVALID_FILE_ATTRIBUTES);
    EXPECT_TRUE(ml_boot_boost_cache_store(temporary, first, header, sizeof(header)));
    {
        HANDLE file = CreateFileW(temporary, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        LARGE_INTEGER payload = { 0 };
        uint8_t byte;
        DWORD transferred;
        payload.QuadPart = 48;
        EXPECT_TRUE(file != INVALID_HANDLE_VALUE);
        EXPECT_TRUE(SetFilePointerEx(file, payload, NULL, FILE_BEGIN));
        EXPECT_TRUE(ReadFile(file, &byte, sizeof(byte), &transferred, NULL));
        EXPECT_EQ(transferred, sizeof(byte));
        byte ^= 1;
        EXPECT_TRUE(SetFilePointerEx(file, payload, NULL, FILE_BEGIN));
        EXPECT_TRUE(WriteFile(file, &byte, sizeof(byte), &transferred, NULL));
        EXPECT_EQ(transferred, sizeof(byte));
        CloseHandle(file);
    }
    EXPECT_TRUE(!ml_boot_boost_cache_load(temporary, first, expected_header, header, sizeof(header)));
    EXPECT_EQ(GetFileAttributesW(temporary), INVALID_FILE_ATTRIBUTES);
    {
        wchar_t interrupted[MAX_PATH + 32];
        HANDLE file;
        DWORD written;
        _snwprintf(interrupted, sizeof(interrupted) / sizeof(interrupted[0]),
                   L"%ls.%08lx.%08lx.tmp", temporary,
                   GetCurrentProcessId(), GetCurrentThreadId());
        file = CreateFileW(interrupted, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        EXPECT_TRUE(file != INVALID_HANDLE_VALUE);
        EXPECT_TRUE(WriteFile(file, header, 8, &written, NULL));
        CloseHandle(file);
        EXPECT_TRUE(ml_boot_boost_cache_store(temporary, first, header, sizeof(header)));
        EXPECT_TRUE(ml_boot_boost_cache_load(temporary, first, expected_header, header, sizeof(header)));
        EXPECT_EQ(GetFileAttributesW(interrupted), INVALID_FILE_ATTRIBUTES);
        DeleteFileW(temporary);
    }
    {
        cache_store_thread_t contexts[2] = {
            { temporary, first, header, sizeof(header), false },
            { temporary, first, header, sizeof(header), false },
        };
        HANDLE threads[2] = {
            CreateThread(NULL, 0, cache_store_thread, &contexts[0], 0, NULL),
            CreateThread(NULL, 0, cache_store_thread, &contexts[1], 0, NULL),
        };
        EXPECT_TRUE(threads[0] != NULL && threads[1] != NULL);
        EXPECT_EQ(WaitForMultipleObjects(2, threads, TRUE, INFINITE), WAIT_OBJECT_0);
        CloseHandle(threads[0]);
        CloseHandle(threads[1]);
        EXPECT_TRUE(contexts[0].result || contexts[1].result);
        EXPECT_TRUE(ml_boot_boost_cache_load(temporary, first, expected_header, header, sizeof(header)));
        DeleteFileW(temporary);
    }
    memcpy(roots[0].root.msvc2015.storage.inline_storage, L"dataX", 12);
    roots[0].root.msvc2015.length = roots[0].root.msvc2015.capacity = 4;
    roots[0].root.msvc2015.encoding = 1;
    memcpy(roots[0].expanded.msvc2015.storage.inline_storage, L"game:/X", 16);
    roots[0].expanded.msvc2015.length = roots[0].expanded.msvc2015.capacity = 6;
    roots[0].expanded.msvc2015.encoding = 1;
    memcpy(roots[1].root.msvc2015.storage.inline_storage, L"gameX", 12);
    roots[1].root.msvc2015.length = roots[1].root.msvc2015.capacity = 4;
    roots[1].root.msvc2015.encoding = 1;
    memcpy(roots[1].expanded.msvc2015.storage.inline_storage, L"rootX", 12);
    roots[1].expanded.msvc2015.length = roots[1].expanded.msvc2015.capacity = 4;
    roots[1].expanded.msvc2015.encoding = 1;
    manager.virtual_roots.msvc2015.first = roots;
    manager.virtual_roots.msvc2015.last = roots + 2;
    EXPECT_TRUE(ml_dl_device_expand_path(&manager, ML_STL_ABI_MSVC2015, L"data:/parts/test.bin", &expanded));
    EXPECT_STREQ_W(expanded, L"rootparts/test.bin");
    LocalFree(expanded);
    EXPECT_TRUE(ml_dl_device_expand_path(&manager, ML_STL_ABI_MSVC2015, L"data:/parts:/test.bin", &expanded));
    EXPECT_STREQ_W(expanded, L"rootparts:/test.bin");
    LocalFree(expanded);
    EXPECT_TRUE(ml_dl_device_expand_path(&manager, ML_STL_ABI_MSVC2015, L"data:\\parts\\test.bin", &expanded));
    EXPECT_STREQ_W(expanded, L"data:\\parts\\test.bin");
    LocalFree(expanded);
    {
        ml_dl_virtual_root_t root2012 = { 0 };
        ml_dl_device_manager_t manager2012 = { 0 };
        memcpy(root2012.root.msvc2012.storage.inline_storage, L"dataX", 12);
        root2012.root.msvc2012.length = root2012.root.msvc2012.capacity = 4;
        root2012.root.msvc2012.encoding = 1;
        memcpy(root2012.expanded.msvc2012.storage.inline_storage, L"rootX", 12);
        root2012.expanded.msvc2012.length = root2012.expanded.msvc2012.capacity = 4;
        root2012.expanded.msvc2012.encoding = 1;
        manager2012.virtual_roots.msvc2012 = (ml_msvc2012_vector_t){ &root2012, &root2012 + 1,
                                                                    &root2012 + 1, &allocator };
        EXPECT_TRUE(ml_dl_device_expand_path(&manager2012, ML_STL_ABI_MSVC2012,
                                             L"data:/parts/test.bin", &expanded));
        EXPECT_STREQ_W(expanded, L"rootparts/test.bin");
        LocalFree(expanded);
        EXPECT_EQ(ml_dl_vector_count(&manager2012.virtual_roots, ML_STL_ABI_MSVC2012,
                                     sizeof(ml_dl_virtual_root_t)), 1);
    }
    {
        dl_allocator_vtable_t vector_allocator_vtable = { 0 };
        dl_allocator_t vector_allocator = { &vector_allocator_vtable };
        ml_dl_device_t first_device = { 0 };
        ml_dl_device_t second_device = { 0 };
        ml_dl_device_t **devices = malloc(sizeof(*devices));
        ml_dl_virtual_mount_t *manager_mounts = malloc(sizeof(*manager_mounts));
        ml_dl_vfs_mounts_t vfs_mounts = { 0 };
        vector_allocator_vtable.allocate_aligned = test_allocate_aligned;
        vector_allocator_vtable.free = test_permanent_free;
        devices[0] = &first_device;
        manager_mounts[0].device = &first_device;
        manager.devices.msvc2015 = (ml_msvc2015_vector_t){ &vector_allocator, devices, devices + 1, devices + 1 };
        manager.bnd4_mounts.msvc2015 = (ml_msvc2015_vector_t){ &vector_allocator, manager_mounts, manager_mounts + 1,
                                                               manager_mounts + 1 };
        vfs_mounts.items = LocalAlloc(0, sizeof(*vfs_mounts.items));
        vfs_mounts.items[0].device = &second_device;
        vfs_mounts.count = vfs_mounts.capacity = 1;
        EXPECT_TRUE(ml_dl_device_push_mounts_permanent(&manager, ML_STL_ABI_MSVC2015, &vfs_mounts));
        EXPECT_EQ(ml_dl_vector_count(&manager.devices, ML_STL_ABI_MSVC2015, sizeof(ml_dl_device_t *)), 2);
        EXPECT_EQ(ml_dl_vector_count(&manager.bnd4_mounts, ML_STL_ABI_MSVC2015, sizeof(ml_dl_virtual_mount_t)), 2);
        EXPECT_TRUE(((ml_dl_device_t **)manager.devices.msvc2015.first)[1] == &second_device);
        EXPECT_TRUE(((ml_dl_virtual_mount_t *)manager.bnd4_mounts.msvc2015.first)[1].device == &second_device);
        EXPECT_NULL(vfs_mounts.items);
        EXPECT_EQ(permanent_freed, 2);
        free(manager.devices.msvc2015.first);
        free(manager.bnd4_mounts.msvc2015.first);
    }
    {
        dl_allocator_vtable_t vector_allocator_vtable = { 0 };
        dl_allocator_t vector_allocator = { &vector_allocator_vtable };
        ml_dl_device_manager_t manager2012 = { 0 };
        ml_dl_device_t first_device = { 0 };
        ml_dl_device_t second_device = { 0 };
        ml_dl_device_t **devices = malloc(sizeof(*devices));
        ml_dl_virtual_mount_t *manager_mounts = malloc(sizeof(*manager_mounts));
        ml_dl_vfs_mounts_t vfs_mounts = { 0 };
        vector_allocator_vtable.allocate_aligned = test_allocate_aligned;
        vector_allocator_vtable.free = test_permanent_free;
        devices[0] = &first_device;
        manager_mounts[0].device = &first_device;
        manager2012.devices.msvc2012 = (ml_msvc2012_vector_t){
            devices, devices + 1, devices + 1, &vector_allocator
        };
        manager2012.bnd4_mounts.msvc2012 = (ml_msvc2012_vector_t){
            manager_mounts, manager_mounts + 1, manager_mounts + 1, &vector_allocator
        };
        vfs_mounts.items = LocalAlloc(0, sizeof(*vfs_mounts.items));
        vfs_mounts.items[0].device = &second_device;
        vfs_mounts.count = vfs_mounts.capacity = 1;
        EXPECT_TRUE(ml_dl_device_push_mounts_permanent(&manager2012, ML_STL_ABI_MSVC2012,
                                                        &vfs_mounts));
        EXPECT_EQ(ml_dl_vector_count(&manager2012.devices, ML_STL_ABI_MSVC2012,
                                     sizeof(ml_dl_device_t *)), 2);
        EXPECT_EQ(ml_dl_vector_count(&manager2012.bnd4_mounts, ML_STL_ABI_MSVC2012,
                                     sizeof(ml_dl_virtual_mount_t)), 2);
        EXPECT_EQ(manager2012.devices.msvc2012.allocator, &vector_allocator);
        EXPECT_EQ(manager2012.bnd4_mounts.msvc2012.allocator, &vector_allocator);
        EXPECT_TRUE(((ml_dl_device_t **)manager2012.devices.msvc2012.first)[1] == &second_device);
        EXPECT_EQ(permanent_freed, 4);
        free(manager2012.devices.msvc2012.first);
        free(manager2012.bnd4_mounts.msvc2012.first);
    }
    {
        ml_dl_device_manager_t empty_manager = { 0 };
        ml_dl_mount_snapshot_t empty_snapshot = { 0 };
        ml_dl_vfs_mounts_t empty_mounts = { 0 };
        EXPECT_TRUE(ml_dl_device_extract_new(&empty_manager, ML_STL_ABI_MSVC2012,
                                             &empty_snapshot, &empty_mounts));
        EXPECT_NULL(empty_manager.devices.msvc2012.first);
        EXPECT_NULL(empty_manager.bnd4_mounts.msvc2012.first);
    }
    {
        dl_allocator_vtable_t vector_allocator_vtable = { 0 };
        dl_allocator_t vector_allocator = { &vector_allocator_vtable };
        ml_dl_device_t device = { 0 };
        ml_dl_device_t **devices = malloc(sizeof(*devices));
        ml_dl_virtual_mount_t *manager_mounts = malloc(sizeof(*manager_mounts));
        ml_dl_vfs_mounts_t vfs_mounts = { 0 };
        vector_allocator_vtable.allocate_aligned = test_allocate_aligned;
        vector_allocator_vtable.free = test_permanent_free;
        devices[0] = &device;
        manager_mounts[0].device = &device;
        manager.devices.msvc2015 = (ml_msvc2015_vector_t){ &vector_allocator, devices, devices, devices + 1 };
        manager.bnd4_mounts.msvc2015 = (ml_msvc2015_vector_t){ &vector_allocator, manager_mounts,
                                                               manager_mounts, manager_mounts + 1 };
        vfs_mounts.items = LocalAlloc(0, sizeof(*vfs_mounts.items));
        vfs_mounts.items[0].device = &device;
        vfs_mounts.count = vfs_mounts.capacity = 1;
        EXPECT_TRUE(ml_dl_device_push_mounts_permanent(&manager, ML_STL_ABI_MSVC2015, &vfs_mounts));
        EXPECT_EQ(ml_dl_vector_count(&manager.devices, ML_STL_ABI_MSVC2015, sizeof(ml_dl_device_t *)), 1);
        EXPECT_EQ(ml_dl_vector_count(&manager.bnd4_mounts, ML_STL_ABI_MSVC2015, sizeof(ml_dl_virtual_mount_t)), 1);
        EXPECT_NULL(vfs_mounts.items);
        free(manager.devices.msvc2015.first);
        free(manager.bnd4_mounts.msvc2015.first);
    }
    header[0] = 'X';
    EXPECT_TRUE(!ml_bhd5_header_valid(header, sizeof(header)));
    printf("smoke_dl_device: all tests passed\n");
    return 0;
}
