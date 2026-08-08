/*
 * Copyright (C) 2024-2026, Soar Qin<soarchin@gmail.com>

 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#include "mimalloc_allocator.h"

#include "game/game.h"
#include "log.h"

#include <mimalloc.h>

#include <stdint.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

static INIT_ONCE mimalloc_heap_once = INIT_ONCE_STATIC_INIT;
static mi_heap_t *mimalloc_heap;
static void *mimalloc_arena_base;
static size_t mimalloc_arena_size;
static bool mimalloc_arena_mapped;
static HANDLE mimalloc_mapping_file = INVALID_HANDLE_VALUE;
static HANDLE mimalloc_mapping = NULL;

#define HEAP_MAPPING_FILE_ENV L"YAFSML_HEAP_MAPPING_FILE"
#define HEAP_MAPPING_NAME_ENV L"YAFSML_HEAP_MAPPING_NAME"

typedef PVOID (WINAPI *map_view_of_file3_t)(HANDLE file_mapping, HANDLE process,
                                            PVOID base_address, ULONG64 offset,
                                            SIZE_T view_size, ULONG allocation_type,
                                            ULONG page_protection,
                                            void *extended_parameters,
                                            ULONG parameter_count);

typedef struct heap_memory_s {
    void *base;
    HANDLE file;
    HANDLE mapping;
    bool mapped;
} heap_memory_t;

static wchar_t *read_environment_value(const wchar_t *name, bool *defined) {
    *defined = false;
    SetLastError(ERROR_SUCCESS);
    DWORD capacity = GetEnvironmentVariableW(name, NULL, 0);
    DWORD error = GetLastError();
    if (capacity == 0) {
        if (error == ERROR_ENVVAR_NOT_FOUND) return NULL;
        *defined = true;
        ML_LOG_WARN(L"allocator", L"%ls is empty or unreadable; ignoring heap mapping request",
                    name);
        return NULL;
    }
    *defined = true;
    if (capacity > SIZE_MAX / sizeof(wchar_t)) return NULL;
    wchar_t *value = mi_malloc((size_t)capacity * sizeof(*value));
    if (value == NULL) return NULL;
    DWORD length = GetEnvironmentVariableW(name, value, capacity);
    if (length == 0 || length >= capacity) {
        mi_free(value);
        ML_LOG_WARN(L"allocator", L"could not read %ls; ignoring heap mapping request",
                    name);
        return NULL;
    }
    return value;
}

static map_view_of_file3_t resolve_map_view_of_file3(void) {
    HMODULE module = GetModuleHandleW(L"kernelbase.dll");
    map_view_of_file3_t result = module == NULL ? NULL :
        (map_view_of_file3_t)GetProcAddress(module, "MapViewOfFile3");
    if (result != NULL) return result;
    module = GetModuleHandleW(L"kernel32.dll");
    return module == NULL ? NULL :
        (map_view_of_file3_t)GetProcAddress(module, "MapViewOfFile3");
}

static void release_heap_memory(heap_memory_t *memory) {
    if (memory == NULL) return;
    if (memory->base != NULL) {
        if (memory->mapped) {
            UnmapViewOfFile(memory->base);
        } else {
            VirtualFree(memory->base, 0, MEM_RELEASE);
        }
    }
    if (memory->mapping != NULL) CloseHandle(memory->mapping);
    if (memory->file != INVALID_HANDLE_VALUE) CloseHandle(memory->file);
    *memory = (heap_memory_t){ NULL, INVALID_HANDLE_VALUE, NULL, false };
}

static heap_memory_t allocate_mapped_heap_memory(size_t size,
                                                 const wchar_t *mapping_file,
                                                 const wchar_t *mapping_name) {
    heap_memory_t memory = { NULL, INVALID_HANDLE_VALUE, NULL, true };
    map_view_of_file3_t map_view_of_file3;
    DWORD error;

    if (mapping_file != NULL) {
        memory.file = CreateFileW(mapping_file, GENERIC_READ | GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (memory.file == INVALID_HANDLE_VALUE) {
            ML_LOG_WARN(L"allocator", L"could not create heap mapping file %ls error=%lu",
                        mapping_file, GetLastError());
            return memory;
        }
    }
    SetLastError(ERROR_SUCCESS);
    memory.mapping = CreateFileMappingW(memory.file, NULL, PAGE_READWRITE,
                                        (DWORD)((uint64_t)size >> 32), (DWORD)size,
                                        mapping_name);
    error = GetLastError();
    if (memory.mapping == NULL) {
        ML_LOG_WARN(L"allocator", L"could not create heap mapping error=%lu", error);
        release_heap_memory(&memory);
        return memory;
    }
    if (error == ERROR_ALREADY_EXISTS) {
        ML_LOG_WARN(L"allocator", L"heap mapping name already exists; refusing an unknown mapping size");
        release_heap_memory(&memory);
        return memory;
    }

    map_view_of_file3 = resolve_map_view_of_file3();
    if (map_view_of_file3 == NULL) {
        ML_LOG_WARN(L"allocator", L"MapViewOfFile3 is unavailable; heap mapping disabled");
        release_heap_memory(&memory);
        return memory;
    }
    /* MEM_TOP_DOWN is undocumented for MapViewOfFile3, but me3 relies on the
       Windows and Wine implementations accepting it. */
    memory.base = map_view_of_file3(memory.mapping, GetCurrentProcess(), NULL, 0,
                                    size, MEM_TOP_DOWN, PAGE_READWRITE, NULL, 0);
    if (memory.base == NULL) {
        ML_LOG_WARN(L"allocator", L"could not map the dedicated heap at a high address error=%lu",
                    GetLastError());
        release_heap_memory(&memory);
    }
    return memory;
}

static heap_memory_t allocate_heap_memory(size_t size) {
    bool mapping_file_defined;
    bool mapping_name_defined;
    wchar_t *mapping_file = read_environment_value(HEAP_MAPPING_FILE_ENV,
                                                   &mapping_file_defined);
    wchar_t *mapping_name = read_environment_value(HEAP_MAPPING_NAME_ENV,
                                                   &mapping_name_defined);
    bool mapping_requested = mapping_file_defined || mapping_name_defined;
    heap_memory_t memory = { NULL, INVALID_HANDLE_VALUE, NULL, false };

    if (mapping_requested && (mapping_file != NULL || mapping_name != NULL)) {
        memory = allocate_mapped_heap_memory(size, mapping_file, mapping_name);
    }
    mi_free(mapping_file);
    mi_free(mapping_name);

    if (memory.base == NULL) {
        if (mapping_requested) {
            ML_LOG_WARN(L"allocator", L"heap mapping unavailable; falling back to private high-address memory");
        }
        memory = (heap_memory_t){
            VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE | MEM_TOP_DOWN,
                         PAGE_READWRITE),
            INVALID_HANDLE_VALUE,
            NULL,
            false
        };
        if (memory.base == NULL) {
            ML_LOG_WARN(L"allocator", L"could not reserve private high-address heap memory error=%lu",
                        GetLastError());
        }
    }
    return memory;
}

static BOOL CALLBACK mimalloc_heap_initialize(PINIT_ONCE init_once, PVOID parameter, PVOID *context) {
    size_t heap_size_mb = (size_t)(uintptr_t)parameter;
    size_t heap_size;
    mi_arena_id_t arena_id = NULL;
    heap_memory_t memory;
    (void)init_once;
    (void)context;

    if (heap_size_mb == 0) {
        const ml_game_descriptor_t *game_ctx = ml_game_context_get();
        heap_size_mb = 6 * 1024;
        if (game_ctx) {
            switch (game_ctx->id) {
            case ML_GAME_ELDEN_RING:
                heap_size_mb = 12 * 1024;
                break;
            case ML_GAME_SEKIRO:
            case ML_GAME_DARK_SOULS_3:
                heap_size_mb = 6 * 1024;
                break;
            default:
                break;
            }
        }
    }
    if (heap_size_mb > 32 * 1024) heap_size_mb = 32 * 1024;
    heap_size = heap_size_mb * 1024u * 1024u;

    mi_option_set(mi_option_purge_decommits, 0);
    memory = allocate_heap_memory(heap_size);
    if (memory.base == NULL) return TRUE;
    if (!mi_manage_os_memory_ex(memory.base, heap_size, true, false, false, -1,
                                true, &arena_id)) {
        ML_LOG_WARN(L"allocator", L"mimalloc refused the dedicated heap memory base=%p size=%zu",
                    memory.base, heap_size);
        release_heap_memory(&memory);
        return TRUE;
    }
    mimalloc_arena_base = memory.base;
    mimalloc_arena_size = heap_size;
    mimalloc_arena_mapped = memory.mapped;
    mimalloc_mapping_file = memory.file;
    mimalloc_mapping = memory.mapping;
    mimalloc_heap = mi_heap_new_in_arena(arena_id);
    if (mimalloc_heap != NULL) {
        ML_LOG_INFO(L"allocator", L"dedicated mimalloc arena ready base=%p size=%zu mode=%ls",
                    mimalloc_arena_base, mimalloc_arena_size,
                    mimalloc_arena_mapped ? L"mapped" : L"private");
    } else {
        ML_LOG_WARN(L"allocator", L"could not create a mimalloc heap in arena=%p",
                    mimalloc_arena_base);
    }
    return TRUE;
}

bool mimalloc_dl_allocator_prepare(size_t heap_size_mb) {
    InitOnceExecuteOnce(&mimalloc_heap_once, mimalloc_heap_initialize, (PVOID)(uintptr_t)heap_size_mb, NULL);
    return mimalloc_heap != NULL;
}

static mi_heap_t *mimalloc_dl_heap(void) {
    return mimalloc_heap;
}

static size_t round_up_size(size_t size, size_t alignment, bool *ok) {
    size_t mask = alignment - 1;
    if ((alignment & mask) != 0) {
        *ok = false;
        return 0;
    }
    if (size > SIZE_MAX - mask) {
        *ok = false;
        return 0;
    }
    *ok = true;
    return (size + mask) & ~mask;
}

static size_t normalized_alignment(size_t alignment) {
    return alignment < 16 ? 16 : alignment;
}

static void __cdecl mimalloc_noop(dl_allocator_t *self) {
    (void)self;
}

static uint32_t __cdecl mimalloc_heap_id(dl_allocator_t *self) {
    (void)self;
    return 0x401;
}

static uint32_t __cdecl mimalloc_allocator_id(dl_allocator_t *self) {
    (void)self;
    return 0xffffffffu;
}

static void *__cdecl mimalloc_capability(dl_allocator_t *self, uint32_t *out, dl_heap_direction_t heap) {
    (void)self;
    (void)heap;
    if (out != NULL) *out = 0x7b;
    return out;
}

static size_t __cdecl mimalloc_size_max(dl_allocator_t *self) {
    (void)self;
    return SIZE_MAX;
}

static size_t __cdecl mimalloc_zero(dl_allocator_t *self) {
    (void)self;
    return 0;
}

static size_t __cdecl mimalloc_block_size(dl_allocator_t *self, void *ptr) {
    (void)self;
    return ptr ? mi_usable_size(ptr) : 0;
}

static void *__cdecl mimalloc_allocate_aligned(dl_allocator_t *self, size_t size, size_t alignment) {
    mi_heap_t *heap;
    (void)self;
    bool ok = false;
    alignment = normalized_alignment(alignment);
    size = round_up_size(size, alignment, &ok);
    if (!ok) return NULL;
    heap = mimalloc_dl_heap();
    return heap != NULL ? mi_heap_malloc_aligned(heap, size, alignment) : mi_malloc_aligned(size, alignment);
}

static void *__cdecl mimalloc_allocate(dl_allocator_t *self, size_t size) {
    return mimalloc_allocate_aligned(self, size, 16);
}

static void *__cdecl mimalloc_reallocate_aligned(dl_allocator_t *self, void *ptr, size_t size, size_t alignment) {
    mi_heap_t *heap;
    (void)self;
    bool ok = false;
    alignment = normalized_alignment(alignment);
    size = round_up_size(size, alignment, &ok);
    if (!ok) return NULL;
    heap = mimalloc_dl_heap();
    return heap != NULL ? mi_heap_realloc_aligned(heap, ptr, size, alignment) : mi_realloc_aligned(ptr, size, alignment);
}

static void *__cdecl mimalloc_reallocate(dl_allocator_t *self, void *ptr, size_t size) {
    return mimalloc_reallocate_aligned(self, ptr, size, 16);
}

static void __cdecl mimalloc_free(dl_allocator_t *self, void *ptr) {
    (void)self;
    mi_free(ptr);
}

static bool __cdecl mimalloc_self_diagnose(dl_allocator_t *self) {
    (void)self;
    return false;
}

static bool __cdecl mimalloc_is_valid_block(dl_allocator_t *self, void *ptr) {
    (void)self;
    (void)ptr;
    return true;
}

static void * __cdecl mimalloc_block_of(dl_allocator_t *self, void *ptr) {
    (void)self;
    (void)ptr;
    return NULL;
}

static dl_allocator_vtable_t mimalloc_vtable = {
    mimalloc_noop,
    mimalloc_heap_id,
    mimalloc_allocator_id,
    mimalloc_capability,
    mimalloc_size_max,
    mimalloc_size_max,
    mimalloc_size_max,
    mimalloc_zero,
    mimalloc_block_size,
    mimalloc_allocate,
    mimalloc_allocate_aligned,
    mimalloc_reallocate,
    mimalloc_reallocate_aligned,
    mimalloc_free,
    mimalloc_noop,
    mimalloc_allocate,
    mimalloc_allocate_aligned,
    mimalloc_reallocate,
    mimalloc_reallocate_aligned,
    mimalloc_free,
    mimalloc_self_diagnose,
    mimalloc_is_valid_block,
    mimalloc_noop,
    mimalloc_noop,
    mimalloc_block_of,
};

static dl_allocator_t mimalloc_allocator = { &mimalloc_vtable };

dl_allocator_t *mimalloc_dl_allocator(void) {
    return &mimalloc_allocator;
}

#ifdef ML_MIMALLOC_ALLOCATOR_TEST
void *mimalloc_test_arena_base(void) {
    return mimalloc_arena_base;
}

size_t mimalloc_test_arena_size(void) {
    return mimalloc_arena_size;
}

bool mimalloc_test_arena_is_mapped(void) {
    return mimalloc_arena_mapped;
}

HANDLE mimalloc_test_mapping_handle(void) {
    return mimalloc_mapping;
}

HANDLE mimalloc_test_mapping_file_handle(void) {
    return mimalloc_mapping_file;
}
#endif
