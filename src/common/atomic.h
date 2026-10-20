/* Copyright (C) 2026, Soar Qin. Licensed under the MIT license. */
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* MinGW does not provide the Windows SDK's ReadAcquire helpers. Keep these
 * loads free of interlocked read-modify-write operations on the lookup paths. */
static inline LONG ml_atomic_load_acquire(const volatile LONG *value) {
#ifdef _MSC_VER
    return ReadAcquire(value);
#else
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
#endif
}

static inline LONG64 ml_atomic_load_acquire64(const volatile LONG64 *value) {
#ifdef _MSC_VER
    return ReadAcquire64(value);
#else
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
#endif
}

#ifdef __cplusplus
}
#endif
