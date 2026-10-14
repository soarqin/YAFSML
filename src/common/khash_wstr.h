#ifndef ML_KHASH_WSTR_H
#define ML_KHASH_WSTR_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "khash.h"

#ifdef __cplusplus
extern "C" {
#endif

/* FNV-1a over the UTF-16LE bytes, in one pass. */
static inline khint_t kh_wstr_hash_func(const wchar_t *s) {
    khint_t h = 2166136261u;
    for (; *s != L'\0'; s++) {
        h ^= (khint_t)(*s & 0xff);
        h *= 16777619u;
        h ^= (khint_t)((unsigned int)*s >> 8);
        h *= 16777619u;
    }
    return h;
}

static inline int kh_wstr_hash_equal(const wchar_t *a, const wchar_t *b) {
    return wcscmp(a, b) == 0;
}

KHASH_INIT(wstr, const wchar_t*, void*, 1, kh_wstr_hash_func, kh_wstr_hash_equal)

#ifdef __cplusplus
}
#endif

#endif
