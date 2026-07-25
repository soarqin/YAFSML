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
static volatile LONG before_main_reached;
static void *entrypoint_hook_target;

static void load_extdlls_after_runtime(ml_lifecycle_phase_t phase, void *userp) {
    (void)phase;
    (void)userp;
    extdlls_load_all();
}

static bool hook_game_entrypoint(void);

static void before_game_main(void) {
    if (entrypoint_hook_target != NULL) MH_DisableHook(entrypoint_hook_target);
    if (InterlockedCompareExchange(&before_main_reached, 1, 0) != 0) return;
    ml_lifecycle_advance(ML_LIFECYCLE_PHASE_BEFORE_MAIN);
    extdlls_load_early();
}

static bool modloader_init(void) {
    wchar_t remote_init[2];
    bool remote_init_mode;
    if (InterlockedCompareExchange(&modloader_initialized, 1, 0) != 0) return false;
    remote_init_mode = GetEnvironmentVariableW(L"YAFSML_REMOTE_INIT", remote_init, 2) != 0;
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
    if (!remote_init_mode && orig_entrypoint == NULL && !hook_game_entrypoint()) {
        ML_LOG_WARN(L"modloader", L"could not install game entrypoint hook; before-main work is disabled");
        return false;
    }
    if (remote_init_mode) before_game_main();
    if (ml_game_context_get() != NULL &&
        ml_game_context_get()->runtime_ready_trigger == ML_RUNTIME_READY_STEAM_API_INIT) {
        if (!ml_lifecycle_on_phase(ML_LIFECYCLE_PHASE_AFTER_RUNTIME_INIT,
                                   load_extdlls_after_runtime, NULL)) {
            ML_LOG_WARN(L"extdll", L"could not schedule external DLL loading after SteamAPI_Init");
        }
    } else {
        /* Preserve loading for unsupported game contexts without a runtime trigger. */
        extdlls_load_all();
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
    if (InterlockedCompareExchange(&modloader_initialized, 0, 0) == 0) {
        (void)modloader_init();
    }
    before_game_main();
    return orig_entrypoint();
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
