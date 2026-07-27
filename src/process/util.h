/*
 * Copyright (C) 2024, Soar Qin<soarchin@gmail.com>

 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool select_process_cpu_affinity_mask(int strategy,
                                      const uint64_t masks[256],
                                      uint64_t system_mask,
                                      uint64_t *selected_mask);
bool set_process_cpu_affinity_strategy(int strategy,
                                       uint64_t *applied_mask,
                                       uint32_t *error_code);

#ifdef __cplusplus
}
#endif
