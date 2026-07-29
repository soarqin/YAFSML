/*
 * Copyright (C) 2024, Soar Qin<soarchin@gmail.com>

 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#include "config.h"
#include "gamehook.h"
#include "mod.h"
#include "extdll.h"
#include "lifecycle.h"
#include "log.h"

#include "game/game.h"

#include "process/image.h"

#include "dearxan.h"

#include "patches/window_flash.h"

#include "proxy/winhttp.h"
#include "proxy/dxgi.h"
#include "proxy/dinput8.h"

#include <MinHook.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

HMODULE module_instance = NULL;
typedef int (WINAPI*entrypoint_t)(void);
static entrypoint_t orig_entrypoint = NULL;
static volatile LONG modloader_initialized;
static void *entrypoint_hook_target;
static volatile LONG before_main_result;
static ULONGLONG dearxan_neuter_start;

static void load_extdlls_after_runtime(ml_lifecycle_phase_t phase, void *userp) {
    (void)phase;
    (void)userp;
    if (InterlockedCompareExchange(&before_main_result, 0, 0) == 1) {
        extdlls_load_all();
    } else {
        ML_LOG_WARN(L"extdll", L"normal external DLL loading skipped because before-main work failed or did not run");
    }
}

static void load_extdlls_after_data_ready(ml_lifecycle_phase_t phase, void *userp) {
    (void)phase;
    (void)userp;
    if (InterlockedCompareExchange(&before_main_result, 0, 0) == 1) {
        extdlls_load_data_ready();
    } else {
        ML_LOG_WARN(L"extdll", L"data-ready external DLL loading skipped because before-main work failed or did not run");
    }
}

static bool hook_game_entrypoint(void);

static void before_game_main(bool is_arxan_detected,
                             bool is_executing_entrypoint, void *opaque) {
    (void)opaque;
    ML_LOG_INFO(L"dearxan", L"after-Arxan reached; detected=%ls blocking-entrypoint=%ls",
                is_arxan_detected ? L"true" : L"false",
                is_executing_entrypoint ? L"true" : L"false");
    ml_lifecycle_advance(ML_LIFECYCLE_PHASE_BEFORE_MAIN);
    InterlockedExchange(&before_main_result,
                        extdlls_load_early() ? 1 : -1);
}

static void after_neuter_arxan(const DearxanResult *result, void *opaque) {
    if (result == NULL) return;
    if (result->status == DearxanSuccess) {
        ML_LOG_INFO(L"dearxan", L"Arxan neutralization completed in %llu ms",
                    GetTickCount64() - dearxan_neuter_start);
    } else {
        ML_LOG_WARN(L"dearxan", L"Arxan neutralization failed: %hs",
                    result->error_msg == NULL ? "unknown error" : result->error_msg);
    }
    before_game_main(result->is_arxan_detected,
                     result->is_executing_entrypoint, opaque);
}

static bool modloader_init(void) {
    if (InterlockedCompareExchange(&modloader_initialized, 1, 0) != 0) return false;
    load_winhttp_proxy();
    load_dxgi_proxy();
    load_dinput8_proxy();
    config_init(module_instance);
    mods_init();
    config_load();
    ml_lifecycle_init();
    ml_lifecycle_advance(ML_LIFECYCLE_PHASE_PRE_ENTRY_SAFE);
    ml_window_flash_install();
    extdlls_prepare();
    gamehook_install();
    {
        const ml_game_descriptor_t *game = ml_game_context_get();
        const bool disable_arxan = config.disable_arxan ||
            (game != NULL && game->id == ML_GAME_DARK_SOULS_3);
        ML_LOG_INFO(L"dearxan", L"Arxan neutralization requested=%ls effective=%ls",
                    config.disable_arxan ? L"true" : L"false",
                    disable_arxan ? L"true" : L"false");
        if (disable_arxan) {
            dearxan_neuter_start = GetTickCount64();
            dearxan_neuter_arxan(after_neuter_arxan, NULL);
        }
        else dearxan_schedule_after_arxan(before_game_main, NULL);
    }
    if (ml_game_context_get() != NULL &&
        ml_game_context_get()->runtime_ready_trigger == ML_RUNTIME_READY_STEAM_API_INIT) {
        if (!ml_lifecycle_on_phase(ML_LIFECYCLE_PHASE_AFTER_RUNTIME_INIT,
                                   load_extdlls_after_runtime, NULL)) {
            ML_LOG_WARN(L"extdll", L"could not schedule external DLL loading after SteamAPI_Init");
        }
        if (!ml_lifecycle_on_phase(ML_LIFECYCLE_PHASE_AFTER_DATA_READY,
                                   load_extdlls_after_data_ready, NULL)) {
            ML_LOG_WARN(L"extdll", L"could not schedule external DLL loading after game data readiness");
        }
    } else {
        /* Preserve loading for unsupported game contexts without a runtime trigger. */
        extdlls_load_all();
        extdlls_load_data_ready();
    }
    return true;
}

__declspec(dllexport) DWORD WINAPI YAFSMLInit(LPVOID parameter) {
    UNREFERENCED_PARAMETER(parameter);
    if (MH_Initialize() != MH_OK) return 0;
    if (!modloader_init()) return 0;
    return 1;
}

int WINAPI new_entrypoint(void) {
    entrypoint_t entrypoint = (entrypoint_t)entrypoint_hook_target;
    MH_STATUS status = entrypoint_hook_target != NULL
        ? MH_DisableHook(entrypoint_hook_target) : MH_ERROR_NOT_CREATED;
    if (status != MH_OK && status != MH_ERROR_DISABLED) {
        ML_LOG_ERROR(L"modloader", L"could not disable bootstrap entrypoint hook");
        return orig_entrypoint();
    }
    if (InterlockedCompareExchange(&modloader_initialized, 0, 0) == 0) {
        (void)modloader_init();
    }
    /* Run the restored image entrypoint, not MinHook's trampoline: the
       dearxan scheduler patched its __security_init_cookie call in-place. */
    return entrypoint();
}

static bool hook_game_entrypoint(void) {
    void *entrypoint;
    MH_STATUS status;
    if (orig_entrypoint != NULL) return true;
    entrypoint = get_module_entrypoint(NULL);
    if (entrypoint == NULL) return false;
    status = MH_CreateHook(entrypoint, new_entrypoint, (LPVOID *)&orig_entrypoint);
    if (status != MH_OK && status != MH_ERROR_ALREADY_CREATED) return false;
    status = MH_EnableHook(entrypoint);
    if (status == MH_OK || status == MH_ERROR_ENABLED) {
        entrypoint_hook_target = entrypoint;
        return true;
    }
    return false;
}

BOOL APIENTRY DllMain(const HMODULE module, const DWORD ul_reason_for_call, LPVOID reserved) {
    UNREFERENCED_PARAMETER(reserved);

    switch (ul_reason_for_call) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(module);
            module_instance = module;
            {
                wchar_t remote_init[2];
                if (GetEnvironmentVariableW(L"YAFSML_REMOTE_INIT", remote_init, 2) != 0) break;
            }
            {
                if (MH_Initialize() != MH_OK) break;
                if (!hook_game_entrypoint()) return FALSE;
            }
            break;
        case DLL_PROCESS_DETACH:
            /* WARNING: FreeLibrary / MH_Uninitialize under loader lock is risky.
               Acceptable here because this only runs on game process exit. */
            extdlls_unload_all();
            gamehook_uninstall();
            mods_uninit();
            ml_lifecycle_uninit();
            break;
        default:
            break;
    }

    return TRUE;
}
