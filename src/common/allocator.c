#include "allocator.h"

/* Only the loader DLL links this file. The tests map ml_mem_* onto LocalAlloc
   through allocator.h (ML_USE_WIN32_ALLOCATOR) or define it themselves
   (ML_USE_TEST_ALLOCATOR). */
#include <mimalloc.h>

void *ml_mem_alloc(UINT flags, size_t size) {
    return (flags & LMEM_ZEROINIT) ? mi_zalloc(size) : mi_malloc(size);
}

void *ml_mem_realloc(void *ptr, size_t size, UINT flags) {
    return (flags & LMEM_ZEROINIT) ? mi_rezalloc(ptr, size) : mi_realloc(ptr, size);
}

void ml_mem_free(void *ptr) {
    mi_free(ptr);
}

char *ml_mem_strdup_a(const char *str) {
    return mi_strdup(str);
}

wchar_t *ml_mem_strdup_w(const wchar_t *str) {
    return mi_wcsdup(str);
}
