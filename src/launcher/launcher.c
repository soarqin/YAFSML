/*
 * Copyright (C) 2024, Soar Qin<soarchin@gmail.com>

 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#include "log.h"

#include "steam/app.h"
#include "game/game.h"
#include "launcher/launch_config.h"

#include <getopt.h>
#include <shlwapi.h>
#include <psapi.h>
#include <stdbool.h>
#include <stdint.h>

static wchar_t full_game_path[MAX_PATH] = L"";
static wchar_t full_config_path[MAX_PATH] = L"";
static wchar_t full_modengine_dll[MAX_PATH] = L"";
static bool suspend = false;
static const ml_game_descriptor_t *launch_game = NULL;
static bool launch_game_explicit = false;

bool parse_args(const int argc, wchar_t *argv[]) {
    const struct option options[] = {
        { L"launch-target", required_argument, NULL, 't' },
        { L"game-path", required_argument, NULL, 'p' },
        { L"config", required_argument, NULL, 'c' },
        { L"modloader-dll", required_argument, NULL, 'd' },
        { L"suspend", no_argument, NULL, 's' },
        { L"modengine-dll", required_argument, NULL, 'd' },
        { NULL, 0, NULL, 0 },
    };
    int opt;
    while ((opt = getopt_long(argc, argv, L":t:p:c:d:s", options, NULL)) != -1) {
        switch (opt) {
            case 't':
                launch_game = ml_game_by_key(optarg);
                if (launch_game == NULL) {
                    ML_LOG_ERROR(L"launcher", L"unsupported launch target: %ls", optarg);
                    return false;
                }
                launch_game_explicit = true;
                break;
            case 'p':
                lstrcpynW(full_game_path, optarg, MAX_PATH);
                break;
            case 'c':
                lstrcpynW(full_config_path, optarg, MAX_PATH);
                break;
            case 'd':
                lstrcpynW(full_modengine_dll, optarg, MAX_PATH);
                break;
            case 's':
                suspend = true;
                break;
            case '?':
                ML_LOG_ERROR(L"launcher", L"bad argument: %c", optopt);
                return false;
            case ':':
                ML_LOG_ERROR(L"launcher", L"missing argument for: %c", optopt);
                return false;
            default:
                break;
        }
    }
    return true;
}

static bool locate_game_executable(wchar_t *game_path) {
    wchar_t temp[MAX_PATH];
    if (launch_game == NULL) return false;
    if (PathFileExistsW(game_path) && !PathIsDirectoryW(game_path)) return true;
    if (StrChrW(game_path, L':') == NULL && game_path[0] != L'\\' && game_path[0] != L'/') {
        GetModuleFileNameW(NULL, temp, MAX_PATH);
        PathRemoveFileSpecW(temp);
        PathAppendW(temp, game_path);
        lstrcpyW(game_path, temp);
        if (PathFileExistsW(game_path) && !PathIsDirectoryW(game_path)) {
            return true;
        }
    }
    for (size_t i = 0; i < launch_game->exe_relpath_count; i++) {
        lstrcpyW(temp, game_path);
        PathAppendW(temp, launch_game->exe_relpaths[i]);
        if (PathFileExistsW(temp) && !PathIsDirectoryW(temp)) {
            lstrcpyW(game_path, temp);
            return true;
        }
    }
    return false;
}

static bool check_current_folder_for_game_path(wchar_t *game_path) {
    GetModuleFileNameW(NULL, game_path, MAX_PATH);
    PathRemoveFileSpecW(game_path);
    return locate_game_executable(game_path);
}

static HMODULE find_remote_module(HANDLE process, const wchar_t *module_name) {
    HMODULE remote_modules[256];
    DWORD remote_module_size = 0;
    const DWORD module_bytes = sizeof(remote_modules);
    if (!EnumProcessModules(process, remote_modules, module_bytes, &remote_module_size)) {
        return NULL;
    }
    for (DWORD i = 0; i < remote_module_size / sizeof(remote_modules[0]); i++) {
        wchar_t remote_module_name[MAX_PATH];
        if (GetModuleBaseNameW(process, remote_modules[i], remote_module_name, MAX_PATH) != 0 &&
            lstrcmpiW(remote_module_name, module_name) == 0) {
            return remote_modules[i];
        }
    }
    return NULL;
}

static bool inject_dll(HANDLE process, const wchar_t *dll_path) {
    const SIZE_T path_size = (lstrlenW(dll_path) + 1) * sizeof(wchar_t);
    LPVOID remote_path = VirtualAllocEx(process, NULL, path_size,
                                        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (remote_path == NULL) return false;

    SIZE_T written = 0;
    if (!WriteProcessMemory(process, remote_path, dll_path, path_size, &written) ||
        written != path_size) {
        VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
        return false;
    }

    const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    const LPTHREAD_START_ROUTINE load_library = kernel32 != NULL
        ? (LPTHREAD_START_ROUTINE)GetProcAddress(kernel32, "LoadLibraryW") : NULL;
    if (load_library == NULL) {
        VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
        return false;
    }

    const HANDLE thread = CreateRemoteThread(
        process, NULL, 0, load_library, remote_path, 0, NULL);
    if (thread == NULL) {
        VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
        return false;
    }

    const DWORD wait_result = WaitForSingleObject(thread, INFINITE);
    DWORD exit_code = 0;
    const bool success = wait_result == WAIT_OBJECT_0 &&
                         GetExitCodeThread(thread, &exit_code) && exit_code != 0;
    CloseHandle(thread);
    VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
    if (!success) return false;

    const HMODULE remote_module = find_remote_module(process, PathFindFileNameW(dll_path));
    const HMODULE local_module = LoadLibraryExW(dll_path, NULL, DONT_RESOLVE_DLL_REFERENCES);
    const FARPROC local_init = local_module != NULL
        ? GetProcAddress(local_module, "YAFSMLInit") : NULL;
    if (remote_module == NULL || local_init == NULL) {
        if (local_module != NULL) FreeLibrary(local_module);
        return false;
    }
    const uintptr_t init_rva = (uintptr_t)local_init - (uintptr_t)local_module;
    const LPTHREAD_START_ROUTINE remote_init =
        (LPTHREAD_START_ROUTINE)((uintptr_t)remote_module + init_rva);
    const HANDLE init_thread = CreateRemoteThread(process, NULL, 0, remote_init, NULL, 0, NULL);
    FreeLibrary(local_module);
    if (init_thread == NULL) return false;
    const DWORD init_wait_result = WaitForSingleObject(init_thread, INFINITE);
    DWORD init_exit_code = 0;
    const bool init_success = init_wait_result == WAIT_OBJECT_0 &&
                              GetExitCodeThread(init_thread, &init_exit_code) &&
                              init_exit_code != 0;
    CloseHandle(init_thread);
    return init_success;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR lpCmdLine, int nShowCmd) {
    STARTUPINFOW si = {0};
    PROCESS_INFORMATION pi = {0};
    wchar_t dll_path[MAX_PATH];
    wchar_t game_folder[MAX_PATH];
    wchar_t launcher_dir[MAX_PATH];
    wchar_t config_path[MAX_PATH];

    if (!parse_args(__argc, __wargv)) return -1;
    if (!launch_game_explicit) {
        const ml_game_descriptor_t *config_game = NULL;
        wchar_t invalid_key[64];
        ml_launcher_game_config_result_t config_result;
        if (GetModuleFileNameW(hInstance, launcher_dir, MAX_PATH) == 0) return -1;
        PathRemoveFileSpecW(launcher_dir);
        if (!ml_launcher_resolve_config_path(config_path, launcher_dir,
                                              full_config_path[0] != L'\0' ? full_config_path : NULL)) {
            return -1;
        }
        /* TOML is handled by the DLL and has no INI launcher game setting. */
        config_result = lstrcmpiW(PathFindExtensionW(config_path), L".toml") != 0 &&
                        PathFileExistsW(config_path) && !PathIsDirectoryW(config_path)
            ? ml_launcher_game_from_ini_file(config_path, &config_game,
                                             invalid_key, sizeof(invalid_key) / sizeof(invalid_key[0]))
            : ML_LAUNCHER_GAME_CONFIG_NOT_SPECIFIED;
        if (config_result == ML_LAUNCHER_GAME_CONFIG_INVALID) {
            ML_LOG_ERROR(L"launcher", L"unsupported game in `%ls`: %ls", config_path, invalid_key);
            return -1;
        }
        if (config_result == ML_LAUNCHER_GAME_CONFIG_ERROR) {
            ML_LOG_ERROR(L"launcher", L"could not read or parse configuration `%ls`", config_path);
            return -1;
        }
        launch_game = ml_launcher_select_game(NULL, config_game);
    }
    if (full_modengine_dll[0] == L'\0' || !PathFileExistsW(full_modengine_dll) || PathIsDirectoryW(full_modengine_dll)) {
        GetModuleFileNameW(hInstance, dll_path, MAX_PATH);
        PathRemoveFileSpecW(dll_path);
        PathAppendW(dll_path, L"YAFSML.dll");
    } else {
        if (StrChrW(full_modengine_dll, L':') == NULL && full_modengine_dll[0] != L'\\' && full_modengine_dll[0] != L'/') {
            GetModuleFileNameW(hInstance, dll_path, MAX_PATH);
            PathRemoveFileSpecW(dll_path);
            PathAppendW(dll_path, full_modengine_dll);
        } else {
            lstrcpynW(dll_path, full_modengine_dll, MAX_PATH);
        }
    }
    if (!PathFileExistsW(dll_path) || PathIsDirectoryW(dll_path)) {
        ML_LOG_ERROR(L"launcher", L"could not find mod loader DLL `%ls`", dll_path);
        return -1;
    }

    if ((full_game_path[0] == L'\0' && !check_current_folder_for_game_path(full_game_path)) || (full_game_path[0] != L'\0' && !locate_game_executable(full_game_path))) {
        if (!app_find_game_path(launch_game->steam_app_id, game_folder)) {
            ML_LOG_ERROR(L"launcher", L"could not find %ls through Steam", launch_game->title);
            return -1;
        }
        lstrcpyW(full_game_path, game_folder);
        if (!locate_game_executable(full_game_path)) {
            ML_LOG_ERROR(L"launcher", L"could not find %ls executable in `%ls`", launch_game->title, game_folder);
            return -1;
        }
    }
    lstrcpyW(game_folder, full_game_path);
    PathRemoveFileSpecW(game_folder);
    if (full_config_path[0] != L'\0') SetEnvironmentVariableW(L"YAFSML_CONFIG", full_config_path);
    {
        /* set SteamAppId here, to make sure the game can be launched w/o EAC */
        wchar_t app_id_str[16];
        _snwprintf(app_id_str, 16, L"%u", launch_game->steam_app_id);
        app_id_str[15] = L'\0';
        SetEnvironmentVariableW(L"SteamAppId", app_id_str);
        SetEnvironmentVariableW(L"SteamGameId", app_id_str);
        SetEnvironmentVariableW(L"SteamOverlayGameId", app_id_str);
    }
    {
        wchar_t game_key[32];
        MultiByteToWideChar(CP_ACP, 0, launch_game->key, -1, game_key, 32);
        game_key[31] = L'\0';
        SetEnvironmentVariableW(L"YAFSML_GAME", game_key);
    }
    SetEnvironmentVariableW(L"YAFSML_REMOTE_INIT", L"1");
    si.cb = sizeof(si);
    BOOL success = CreateProcessW(full_game_path, NULL, NULL, NULL, FALSE,
                                  CREATE_SUSPENDED, NULL, game_folder, &si, &pi);
    if (success && !inject_dll(pi.hProcess, dll_path)) {
        ML_LOG_ERROR(L"launcher", L"could not inject `%ls` into %ls", dll_path, launch_game->title);
        TerminateProcess(pi.hProcess, (UINT)-1);
        success = FALSE;
    }
    if (success && !suspend) {
        if (ResumeThread(pi.hThread) == (DWORD)-1) {
            ML_LOG_ERROR(L"launcher", L"could not resume %ls", launch_game->title);
            TerminateProcess(pi.hProcess, (UINT)-1);
            success = FALSE;
        }
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return success ? 0 : -1;
}
