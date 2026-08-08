#include <stdint.h>
#include <stdio.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "test_common.h"
#include "modloader/mimalloc_allocator.h"

int main(void) {
    wchar_t mapping_name[96];
    void *arena_base;
    size_t arena_size;
    MEMORY_BASIC_INFORMATION memory;
    HANDLE mapping;
    void *view;
    dl_allocator_t *allocator;
    void *ptr;

    _snwprintf(mapping_name, sizeof(mapping_name) / sizeof(mapping_name[0]),
               L"Local\\YAFSMLSmokeHeap_%lu", GetCurrentProcessId());
    SetEnvironmentVariableW(L"YAFSML_HEAP_MAPPING_FILE", NULL);
    SetEnvironmentVariableW(L"YAFSML_HEAP_MAPPING_NAME", mapping_name);

    EXPECT_TRUE(mimalloc_dl_allocator_prepare(64));
    arena_base = mimalloc_test_arena_base();
    arena_size = mimalloc_test_arena_size();
    EXPECT_NOT_NULL(arena_base);
    EXPECT_EQ(arena_size, 64u * 1024u * 1024u);
    EXPECT_TRUE(mimalloc_test_arena_is_mapped());
    EXPECT_NOT_NULL(mimalloc_test_mapping_handle());
    EXPECT_EQ(mimalloc_test_mapping_file_handle(), INVALID_HANDLE_VALUE);
    EXPECT_TRUE(VirtualQuery(arena_base, &memory, sizeof(memory)) == sizeof(memory));
    EXPECT_EQ(memory.AllocationBase, arena_base);
    EXPECT_EQ(memory.Type, MEM_MAPPED);

    mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, mapping_name);
    EXPECT_TRUE(mapping != NULL);
    view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0x1000);
    EXPECT_NOT_NULL(view);
    UnmapViewOfFile(view);
    CloseHandle(mapping);

    allocator = mimalloc_dl_allocator();
    ptr = allocator->vtable->allocate(allocator, 32);
    EXPECT_NOT_NULL(ptr);
    EXPECT_TRUE((uintptr_t)ptr >= (uintptr_t)arena_base);
    EXPECT_TRUE((uintptr_t)ptr < (uintptr_t)arena_base + arena_size);
    allocator->vtable->free(allocator, ptr);

    printf("smoke_mimalloc_allocator_mapping: all tests passed\n");
    return 0;
}
