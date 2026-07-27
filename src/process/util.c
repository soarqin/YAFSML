/*
 * Copyright (C) 2024, Soar Qin<soarchin@gmail.com>

 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#include "util.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <sysinfoapi.h>

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>

bool select_process_cpu_affinity_mask(const int strategy,
                                      const uint64_t masks[256],
                                      const uint64_t system_mask,
                                      uint64_t *selected_mask) {
    uint64_t all_masks = 0;
    uint64_t nonzero_masks = 0;
    uint64_t selected = 0;
    if (masks == NULL || selected_mask == NULL || strategy < 1 || strategy > 4) {
        return false;
    }
    for (int i = 0; i < 256; i++) {
        all_masks |= masks[i];
        if (i > 0) nonzero_masks |= masks[i];
    }
    switch (strategy) {
        case 1:
            selected = all_masks & ~UINT64_C(1);
            break;
        case 2:
            if (!nonzero_masks) return false;
            for (int i = 0; i < 256; i++) {
                if (masks[i] != 0) {
                    selected = masks[i];
                    break;
                }
            }
            break;
        case 3:
            if (!nonzero_masks) return false;
            for (int i = 255; i > 0; i--) {
                if (masks[i] != 0) {
                    selected = masks[i];
                    break;
                }
            }
            break;
        case 4:
            if (!nonzero_masks) return false;
            for (int i = 255; i > 0; i--) {
                if (masks[i] != 0) {
                    selected = masks[i];
                    for (int j = 0; j < 64; j++) {
                        if ((selected & (UINT64_C(1) << j)) != 0) {
                            selected &= ~(UINT64_C(1) << j);
                            break;
                        }
                    }
                    break;
                }
            }
            break;
        default:
            return false;
    }
    selected &= system_mask;
    if (selected == 0) return false;
    *selected_mask = selected;
    return true;
}

bool set_process_cpu_affinity_strategy(const int strategy,
                                       uint64_t *applied_mask,
                                       uint32_t *error_code) {
    DWORD len = 0;
    uint32_t error = ERROR_SUCCESS;
    uint64_t masks[256] = {0};
    uint64_t selected_mask;
    uint64_t process_mask;
    uint64_t system_mask;
    DWORD offset = 0;
    if (applied_mask != NULL) *applied_mask = 0;
    if (error_code != NULL) *error_code = ERROR_SUCCESS;
    if (strategy < 1 || strategy > 4) {
        error = ERROR_INVALID_PARAMETER;
        goto fail;
    }
    SetLastError(ERROR_SUCCESS);
    if (GetLogicalProcessorInformationEx(RelationProcessorCore, NULL, &len) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER || len == 0) {
        error = GetLastError();
        if (error == ERROR_SUCCESS) error = ERROR_NOT_SUPPORTED;
        goto fail;
    }
    char *data = (char *)LocalAlloc(0, len);
    if (!data) {
        error = ERROR_NOT_ENOUGH_MEMORY;
        goto fail;
    }
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore, (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*)data, &len)) {
        error = GetLastError();
        LocalFree(data);
        goto fail;
    }
    while (offset < len) {
        const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *info = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*)(data + offset);
        size_t group_mask_offset = offsetof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX,
                                            Processor.GroupMask);
        if (info->Size < group_mask_offset + sizeof(GROUP_AFFINITY) ||
            info->Size > len - offset || info->Processor.GroupCount != 1 ||
            info->Processor.GroupMask[0].Group != 0) {
            error = ERROR_NOT_SUPPORTED;
            LocalFree(data);
            goto fail;
        }
        offset += info->Size;
        const uint64_t mask = info->Processor.GroupMask[0].Mask;
        const BYTE eff = info->Processor.EfficiencyClass;
        masks[eff] |= mask;
    }
    LocalFree(data);
    if (!GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask)) {
        error = GetLastError();
        goto fail;
    }
    if (!select_process_cpu_affinity_mask(strategy, masks, system_mask,
                                          &selected_mask)) {
        error = ERROR_INVALID_PARAMETER;
        goto fail;
    }
    if (!SetProcessAffinityMask(GetCurrentProcess(), (DWORD_PTR)selected_mask)) {
        error = GetLastError();
        goto fail;
    }
    if (applied_mask != NULL) *applied_mask = selected_mask;
    return true;
fail:
    if (error_code != NULL) *error_code = error;
    return false;
}
