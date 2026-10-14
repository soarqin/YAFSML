/*
 * Copyright (C) 2026, Soar Qin<soarchin@gmail.com>
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#pragma once

#include <stdbool.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ml_hook_result_e {
    ML_HOOK_APPLIED,
    ML_HOOK_SIGNATURE_NOT_FOUND,
    ML_HOOK_FAILED,
} ml_hook_result_t;

ml_hook_result_t ml_hook_install(void *target, void *detour, void **original);
const char *ml_hook_result_name(ml_hook_result_t result);
/* Remove or disable a hook. A NULL target, a hook that was never created and an
 * already disabled hook count as success; failures are logged under `scope`. */
bool ml_hook_remove(void *target, const wchar_t *scope);
bool ml_hook_disable(void *target, const wchar_t *scope);
/* Keeps the loader DLL mapped for the life of the process, so hooks that can
 * no longer be removed never point into unmapped code. */
bool ml_hook_pin_module(void);

#ifdef __cplusplus
}
#endif
