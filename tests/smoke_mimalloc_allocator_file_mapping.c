#include <stdint.h>
#include <stdio.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "test_common.h"
#include "modloader/mimalloc_allocator.h"

int main(void) {
    wchar_t temporary_directory[MAX_PATH];
    wchar_t mapping_path[MAX_PATH];
    HANDLE file;
    LARGE_INTEGER file_size;
    void *arena_base;
    size_t arena_size;
    MEMORY_BASIC_INFORMATION memory;
    dl_allocator_t *allocator;
    void *ptr;
    DWORD temporary_length;
    int path_length;

    temporary_length = GetTempPathW(MAX_PATH, temporary_directory);
    EXPECT_TRUE(temporary_length != 0 && temporary_length < MAX_PATH);
    path_length = _snwprintf(mapping_path, MAX_PATH, L"%lsYAFSMLSmokeHeap_%lu.bin",
                             temporary_directory, GetCurrentProcessId());
    EXPECT_TRUE(path_length > 0 && path_length < MAX_PATH);
    DeleteFileW(mapping_path);
    SetEnvironmentVariableW(L"YAFSML_HEAP_MAPPING_FILE", mapping_path);
    SetEnvironmentVariableW(L"YAFSML_HEAP_MAPPING_NAME", NULL);

    EXPECT_TRUE(mimalloc_dl_allocator_prepare(64));
    arena_base = mimalloc_test_arena_base();
    arena_size = mimalloc_test_arena_size();
    file = mimalloc_test_mapping_file_handle();
    EXPECT_NOT_NULL(arena_base);
    EXPECT_EQ(arena_size, 64u * 1024u * 1024u);
    EXPECT_TRUE(mimalloc_test_arena_is_mapped());
    EXPECT_NOT_NULL(mimalloc_test_mapping_handle());
    EXPECT_TRUE(file != INVALID_HANDLE_VALUE);
    EXPECT_TRUE(GetFileSizeEx(file, &file_size));
    EXPECT_EQ(file_size.QuadPart, (LONGLONG)arena_size);
    EXPECT_TRUE(VirtualQuery(arena_base, &memory, sizeof(memory)) == sizeof(memory));
    EXPECT_EQ(memory.AllocationBase, arena_base);
    EXPECT_EQ(memory.Type, MEM_MAPPED);

    allocator = mimalloc_dl_allocator();
    ptr = allocator->vtable->allocate(allocator, 32);
    EXPECT_NOT_NULL(ptr);
    EXPECT_TRUE((uintptr_t)ptr >= (uintptr_t)arena_base);
    EXPECT_TRUE((uintptr_t)ptr < (uintptr_t)arena_base + arena_size);
    allocator->vtable->free(allocator, ptr);

    EXPECT_TRUE(DeleteFileW(mapping_path));
    printf("smoke_mimalloc_allocator_file_mapping: all tests passed\n");
    return 0;
}
