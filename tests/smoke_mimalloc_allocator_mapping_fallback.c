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
    HANDLE existing_mapping;
    void *arena_base;
    size_t arena_size;
    MEMORY_BASIC_INFORMATION memory;
    dl_allocator_t *allocator;
    void *ptr;

    _snwprintf(mapping_name, sizeof(mapping_name) / sizeof(mapping_name[0]),
               L"Local\\YAFSMLSmokeHeapCollision_%lu", GetCurrentProcessId());
    existing_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                          0, 0x10000, mapping_name);
    EXPECT_TRUE(existing_mapping != NULL);
    EXPECT_TRUE(GetLastError() != ERROR_ALREADY_EXISTS);
    SetEnvironmentVariableW(L"YAFSML_HEAP_MAPPING_FILE", NULL);
    SetEnvironmentVariableW(L"YAFSML_HEAP_MAPPING_NAME", mapping_name);

    EXPECT_TRUE(mimalloc_dl_allocator_prepare(64));
    arena_base = mimalloc_test_arena_base();
    arena_size = mimalloc_test_arena_size();
    EXPECT_NOT_NULL(arena_base);
    EXPECT_EQ(arena_size, 64u * 1024u * 1024u);
    EXPECT_TRUE(!mimalloc_test_arena_is_mapped());
    EXPECT_NULL(mimalloc_test_mapping_handle());
    EXPECT_EQ(mimalloc_test_mapping_file_handle(), INVALID_HANDLE_VALUE);
    EXPECT_TRUE(VirtualQuery(arena_base, &memory, sizeof(memory)) == sizeof(memory));
    EXPECT_EQ(memory.AllocationBase, arena_base);
    EXPECT_EQ(memory.Type, MEM_PRIVATE);

    allocator = mimalloc_dl_allocator();
    ptr = allocator->vtable->allocate(allocator, 32);
    EXPECT_NOT_NULL(ptr);
    EXPECT_TRUE((uintptr_t)ptr >= (uintptr_t)arena_base);
    EXPECT_TRUE((uintptr_t)ptr < (uintptr_t)arena_base + arena_size);
    allocator->vtable->free(allocator, ptr);

    CloseHandle(existing_mapping);
    printf("smoke_mimalloc_allocator_mapping_fallback: all tests passed\n");
    return 0;
}
