/*
 * Copyright (C) 2024, Soar Qin<soarchin@gmail.com>

 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t process_cpu_set_id_t;

typedef struct process_cpu_set_info {
    process_cpu_set_id_t id;
    uint16_t group;
    uint8_t logical_processor_index;
    uint8_t efficiency_class;
    bool allocated;
    bool allocated_to_target_process;
} process_cpu_set_info_t;

size_t select_process_cpu_set_ids(int strategy,
                                  const process_cpu_set_info_t *sets,
                                  size_t set_count,
                                  process_cpu_set_id_t *ids,
                                  size_t ids_capacity);

#ifdef __cplusplus
}
#endif
