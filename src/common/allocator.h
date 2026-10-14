#ifndef ML_ALLOCATOR_H
#define ML_ALLOCATOR_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlwapi.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(ML_USE_WIN32_ALLOCATOR) && !defined(ML_USE_TEST_ALLOCATOR)
#define ml_mem_alloc LocalAlloc
#define ml_mem_realloc LocalReAlloc
#define ml_mem_free LocalFree
#define ml_mem_strdup_a StrDupA
#define ml_mem_strdup_w StrDupW
#else
void *ml_mem_alloc(UINT flags, size_t size);
void *ml_mem_realloc(void *ptr, size_t size, UINT flags);
void ml_mem_free(void *ptr);
char *ml_mem_strdup_a(const char *str);
wchar_t *ml_mem_strdup_w(const wchar_t *str);
#endif

#ifdef __cplusplus
}
#endif

#endif
