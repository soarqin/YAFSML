/*
 * Copyright (C) 2026, Soar Qin<soarchin@gmail.com>
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#include "hook.h"

#include "log.h"

#include <MinHook.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

ml_hook_result_t ml_hook_install(void *target, void *detour, void **original) {
    if (target == NULL) return ML_HOOK_SIGNATURE_NOT_FOUND;
    if (MH_CreateHook(target, detour, original) != MH_OK) return ML_HOOK_FAILED;
    if (MH_EnableHook(target) != MH_OK) {
        MH_RemoveHook(target);
        return ML_HOOK_FAILED;
    }
    return ML_HOOK_APPLIED;
}

bool ml_hook_remove(void *target, const wchar_t *scope) {
    MH_STATUS status;
    if (target == NULL) return true;
    status = MH_RemoveHook(target);
    if (status == MH_OK || status == MH_ERROR_NOT_CREATED) return true;
    ML_LOG_WARN(scope, L"failed to remove hook at %p: %d", target, status);
    return false;
}

bool ml_hook_disable(void *target, const wchar_t *scope) {
    MH_STATUS status;
    if (target == NULL) return true;
    status = MH_DisableHook(target);
    if (status == MH_OK || status == MH_ERROR_DISABLED || status == MH_ERROR_NOT_CREATED) return true;
    ML_LOG_WARN(scope, L"failed to disable hook at %p: %d", target, status);
    return false;
}

bool ml_hook_pin_module(void) {
    HMODULE module;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                              (LPCWSTR)(uintptr_t)ml_hook_pin_module, &module) != 0;
}

const char *ml_hook_result_name(ml_hook_result_t result) {
    switch (result) {
        case ML_HOOK_APPLIED: return "APPLIED";
        case ML_HOOK_SIGNATURE_NOT_FOUND: return "SIGNATURE_NOT_FOUND";
        case ML_HOOK_FAILED: return "HOOK_FAILED";
        default: return "UNKNOWN";
    }
}
