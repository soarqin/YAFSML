/*
 * C11 implementation of dearxan v0.5.7 functionality.
 *
 * Original project copyright (c) 2025 William Tremblay.
 * Licensed under the MIT license; see THIRD_PARTY_NOTICES.md.
 */

#pragma once

/* Internal helpers shared by the dearxan translation units. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Grow `*items` to hold at least `count` elements of `item_size` bytes. The
 * capacity starts at 8 and doubles; on failure the array is left untouched. */
bool dearxan_reserve_array(void **items, size_t *capacity, size_t count,
                           size_t item_size);

/* SplitMix64 finalizer used to hash addresses and memory block indices. */
static inline uint64_t dearxan_hash_u64(uint64_t value) {
    value ^= value >> 30;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27;
    value *= UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

/* Copy `size` bytes over code or read-only data, then restore the previous page
 * protection and flush the instruction cache. Fails only when the bytes could not
 * be written; a failed protection restore leaves them writable, which is harmless
 * once the image sections are already RWX. */
bool dearxan_write_protected(void *target, const void *source, size_t size);

/* Allocate `size` bytes of RWX memory that every address in [`low`, `high`] can
 * reach with a rel32 displacement, searching free regions in ascending order. */
void *dearxan_allocate_near(uintptr_t low, uintptr_t high, size_t size);

#ifdef __cplusplus
}
#endif
