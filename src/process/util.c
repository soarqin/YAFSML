/*
 * Copyright (C) 2024, Soar Qin<soarchin@gmail.com>

 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#include "util.h"
#include "cpu_sets.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

static bool fail_with_error(const uint32_t error, uint32_t *error_code) {
    if (error_code != NULL) *error_code = error;
    return false;
}

static bool read_process_cpu_sets(process_cpu_set_info_t **sets_out,
                                  size_t *set_count_out,
                                  uint32_t *error_code) {
    DWORD length = 0;
    HANDLE process = GetCurrentProcess();
    SetLastError(ERROR_SUCCESS);
    if (GetSystemCpuSetInformation(NULL, 0, &length, process, 0) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER || length == 0) {
        uint32_t error = GetLastError();
        if (error == ERROR_SUCCESS) error = ERROR_NOT_SUPPORTED;
        return fail_with_error(error, error_code);
    }

    unsigned char *data = (unsigned char *)malloc(length);
    if (data == NULL) return fail_with_error(ERROR_NOT_ENOUGH_MEMORY, error_code);

    DWORD returned_length = 0;
    if (!GetSystemCpuSetInformation((PSYSTEM_CPU_SET_INFORMATION)data,
                                    length, &returned_length, process, 0)) {
        const uint32_t error = GetLastError();
        free(data);
        return fail_with_error(error, error_code);
    }
    if (returned_length == 0) {
        free(data);
        return fail_with_error(ERROR_NOT_SUPPORTED, error_code);
    }

    size_t set_count = 0;
    DWORD offset = 0;
    while (offset < returned_length) {
        const SYSTEM_CPU_SET_INFORMATION *info =
            (const SYSTEM_CPU_SET_INFORMATION *)(data + offset);
        if (info->Size < sizeof(*info) || info->Size > returned_length - offset) {
            free(data);
            return fail_with_error(ERROR_INVALID_DATA, error_code);
        }
        if (info->Type == CpuSetInformation) set_count++;
        offset += info->Size;
    }
    if (set_count == 0 || set_count > SIZE_MAX / sizeof(process_cpu_set_info_t)) {
        free(data);
        return fail_with_error(ERROR_NOT_SUPPORTED, error_code);
    }

    process_cpu_set_info_t *sets = (process_cpu_set_info_t *)malloc(
        set_count * sizeof(process_cpu_set_info_t));
    if (sets == NULL) {
        free(data);
        return fail_with_error(ERROR_NOT_ENOUGH_MEMORY, error_code);
    }

    size_t set_index = 0;
    offset = 0;
    while (offset < returned_length) {
        const SYSTEM_CPU_SET_INFORMATION *info =
            (const SYSTEM_CPU_SET_INFORMATION *)(data + offset);
        if (info->Type == CpuSetInformation) {
            sets[set_index].id = info->CpuSet.Id;
            sets[set_index].group = info->CpuSet.Group;
            sets[set_index].logical_processor_index =
                info->CpuSet.LogicalProcessorIndex;
            sets[set_index].efficiency_class = info->CpuSet.EfficiencyClass;
            sets[set_index].allocated = info->CpuSet.Allocated != 0;
            sets[set_index].allocated_to_target_process =
                info->CpuSet.AllocatedToTargetProcess != 0;
            set_index++;
        }
        offset += info->Size;
    }
    free(data);

    *sets_out = sets;
    *set_count_out = set_index;
    return true;
}

bool set_process_cpu_affinity_strategy(const int strategy,
                                      uint32_t *applied_cpu_set_count,
                                      uint32_t *error_code) {
    if (applied_cpu_set_count != NULL) *applied_cpu_set_count = 0;
    if (error_code != NULL) *error_code = ERROR_SUCCESS;
    if (strategy < 1 || strategy > 4) {
        return fail_with_error(ERROR_INVALID_PARAMETER, error_code);
    }

    process_cpu_set_info_t *sets = NULL;
    size_t set_count = 0;
    if (!read_process_cpu_sets(&sets, &set_count, error_code)) return false;

    const size_t selected_count =
        select_process_cpu_set_ids(strategy, sets, set_count, NULL, 0);
    if (selected_count == 0 || selected_count > UINT32_MAX) {
        free(sets);
        return fail_with_error(ERROR_NOT_SUPPORTED, error_code);
    }

    if (selected_count > SIZE_MAX / sizeof(process_cpu_set_id_t)) {
        free(sets);
        return fail_with_error(ERROR_NOT_ENOUGH_MEMORY, error_code);
    }
    process_cpu_set_id_t *ids = (process_cpu_set_id_t *)malloc(
        selected_count * sizeof(process_cpu_set_id_t));
    if (ids == NULL) {
        free(sets);
        return fail_with_error(ERROR_NOT_ENOUGH_MEMORY, error_code);
    }
    if (select_process_cpu_set_ids(strategy, sets, set_count, ids,
                                   selected_count) != selected_count) {
        free(ids);
        free(sets);
        return fail_with_error(ERROR_INVALID_DATA, error_code);
    }
    free(sets);

    if (!SetProcessDefaultCpuSets(GetCurrentProcess(), (const ULONG *)ids,
                                  (ULONG)selected_count)) {
        const uint32_t error = GetLastError();
        free(ids);
        return fail_with_error(error, error_code);
    }
    free(ids);

    if (applied_cpu_set_count != NULL) {
        *applied_cpu_set_count = (uint32_t)selected_count;
    }
    return true;
}
