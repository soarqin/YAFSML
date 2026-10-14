/*
 * C11 implementation of dearxan v0.5.7 functionality.
 *
 * Original project copyright (c) 2025 William Tremblay.
 * Licensed under the MIT license; see THIRD_PARTY_NOTICES.md.
 */

#include "util.h"

#include <stdlib.h>
#include <string.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

bool dearxan_reserve_array(void **items, size_t *capacity, size_t count,
                           size_t item_size) {
    size_t next_capacity;
    void *next;
    if (count <= *capacity) return true;
    next_capacity = *capacity == 0 ? 8 : *capacity;
    while (next_capacity < count) {
        if (next_capacity > SIZE_MAX / 2) return false;
        next_capacity *= 2;
    }
    if (next_capacity > SIZE_MAX / item_size) return false;
    next = realloc(*items, next_capacity * item_size);
    if (next == NULL) return false;
    *items = next;
    *capacity = next_capacity;
    return true;
}

bool dearxan_write_protected(void *target, const void *source, size_t size) {
    DWORD old_protection;
    if (!VirtualProtect(target, size, PAGE_EXECUTE_READWRITE, &old_protection)) return false;
    memcpy(target, source, size);
    VirtualProtect(target, size, old_protection, &old_protection);
    FlushInstructionCache(GetCurrentProcess(), target, size);
    return true;
}

void *dearxan_allocate_near(uintptr_t low, uintptr_t high, size_t size) {
    SYSTEM_INFO information;
    uintptr_t minimum;
    uintptr_t maximum;
    uintptr_t address;
    GetSystemInfo(&information);
    minimum = high > INT32_MAX
        ? high - INT32_MAX
        : (uintptr_t)information.lpMinimumApplicationAddress;
    if (minimum < (uintptr_t)information.lpMinimumApplicationAddress) {
        minimum = (uintptr_t)information.lpMinimumApplicationAddress;
    }
    maximum = low <= UINTPTR_MAX - INT32_MAX
        ? low + INT32_MAX
        : (uintptr_t)information.lpMaximumApplicationAddress;
    if (maximum > (uintptr_t)information.lpMaximumApplicationAddress) {
        maximum = (uintptr_t)information.lpMaximumApplicationAddress;
    }
    address = minimum - minimum % information.dwAllocationGranularity;
    if (address < minimum) address += information.dwAllocationGranularity;
    while (address <= maximum && size <= maximum - address) {
        MEMORY_BASIC_INFORMATION region;
        uintptr_t next;
        if (VirtualQuery((void *)address, &region, sizeof(region)) == 0) break;
        if (region.State == MEM_FREE && region.RegionSize >= size) {
            void *result = VirtualAlloc((void *)address, size,
                                        MEM_COMMIT | MEM_RESERVE,
                                        PAGE_EXECUTE_READWRITE);
            if (result != NULL) return result;
        }
        if ((uintptr_t)region.BaseAddress >
            UINTPTR_MAX - region.RegionSize) break;
        next = (uintptr_t)region.BaseAddress + region.RegionSize;
        if (next > UINTPTR_MAX - information.dwAllocationGranularity + 1) break;
        next = (next + information.dwAllocationGranularity - 1) -
               (next + information.dwAllocationGranularity - 1) %
                   information.dwAllocationGranularity;
        if (next <= address) break;
        address = next;
    }
    return NULL;
}
