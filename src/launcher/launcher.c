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
#include "launcher/launch_process.h"

#include <getopt.h>
#include <shlwapi.h>
#include <stdbool.h>
#include <stdlib.h>

static const wchar_t *game_path_arg = NULL;
static const wchar_t *config_arg = NULL;
static const wchar_t *dll_arg = NULL;
static bool suspend = false;
static const ml_game_descriptor_t *launch_game = NULL;
static bool launch_game_explicit = false;
static wchar_t launcher_dir[MAX_PATH] = L"";
static HANDLE attached_console = INVALID_HANDLE_VALUE;
static bool console_attach_tried = false;

static void report_text(HANDLE output, bool console, const wchar_t *text) {
    size_t length = wcslen(text);
    while (length != 0) {
        DWORD written = 0;
        if (console) {
            const DWORD chunk = length > 8192 ? 8192 : (DWORD)length;
            if (!WriteConsoleW(output, text, chunk, &written, NULL) || written == 0) return;
            text += written;
            length -= written;
        } else {
            char utf8[4096];
            int chunk = length > 1024 ? 1024 : (int)length;
            int bytes;
            /* Keep a surrogate pair within one conversion. */
            if ((size_t)chunk < length && IS_HIGH_SURROGATE(text[chunk - 1])) chunk--;
            bytes = WideCharToMultiByte(CP_UTF8, 0, text, chunk, utf8, (int)sizeof(utf8), NULL, NULL);
            if (bytes <= 0 || !WriteFile(output, utf8, (DWORD)bytes, &written, NULL)) return;
            text += chunk;
            length -= (size_t)chunk;
        }
    }
}

/* A WIN32 launcher has no console of its own: write each message to standard
 * error when it is redirected, otherwise to the console of the parent process,
 * and show errors in a dialog so a launch from Explorer or Steam does not fail
 * silently. */
static void report_message(ml_log_level_t level, const wchar_t *component, const wchar_t *message) {
    HANDLE output = GetStdHandle(STD_ERROR_HANDLE);
    if (output == NULL || output == INVALID_HANDLE_VALUE || GetFileType(output) == FILE_TYPE_UNKNOWN) {
        if (!console_attach_tried) {
            console_attach_tried = true;
            if (AttachConsole(ATTACH_PARENT_PROCESS)) {
                attached_console = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
                                               FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                               OPEN_EXISTING, 0, NULL);
            }
        }
        output = attached_console;
    }
    if (output != INVALID_HANDLE_VALUE) {
        DWORD mode;
        /* Consoles take UTF-16 directly; files and pipes receive UTF-8. */
        const bool console = GetConsoleMode(output, &mode) != 0;
        report_text(output, console, ml_log_level_name(level));
        report_text(output, console, L": ");
        if (component != NULL && component[0] != L'\0') {
            report_text(output, console, L"[");
            report_text(output, console, component);
            report_text(output, console, L"] ");
        }
        report_text(output, console, message);
        report_text(output, console, console ? L"\n" : L"\r\n");
    }
    /* Tests that include this file replace MessageBoxW so they never block. */
    if (level >= ML_LOG_LEVEL_ERROR)
        MessageBoxW(NULL, message, L"YAFSML", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
}

/* Formats a system error message. Messages such as ERROR_BAD_EXE_FORMAT name
 * the affected file as an insert, so every insert (at most %99) is `subject`. */
static void describe_error(DWORD error, const wchar_t *subject, wchar_t *text, DWORD count) {
    DWORD_PTR inserts[99];
    DWORD length;
    for (size_t i = 0; i < sizeof(inserts) / sizeof(inserts[0]); i++)
        inserts[i] = (DWORD_PTR)(subject != NULL ? subject : L"");
    length = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ARGUMENT_ARRAY |
                            FORMAT_MESSAGE_MAX_WIDTH_MASK, NULL, error, 0, text, count,
                            (va_list *)inserts);
    while (length != 0 && (text[length - 1] == L' ' || text[length - 1] == L'.' ||
                           text[length - 1] == L'\x3002' || text[length - 1] == L'\r' ||
                           text[length - 1] == L'\n')) {
        length--;
    }
    text[length] = L'\0';
    if (length == 0) lstrcpynW(text, L"unknown error", (int)count);
}

static bool is_file(const wchar_t *path) {
    const DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

/* Returns the value of an environment variable of any length, or NULL. */
static wchar_t *read_environment(const wchar_t *name) {
    DWORD size = GetEnvironmentVariableW(name, NULL, 0);
    while (size > 1) {
        wchar_t *value = malloc(size * sizeof(*value));
        DWORD length;
        if (value == NULL) return NULL;
        length = GetEnvironmentVariableW(name, value, size);
        if (length != 0 && length < size) return value;
        free(value);
        /* The value changed between the calls; retry with the new size. */
        size = length;
    }
    return NULL;
}

static bool parse_args(const int argc, wchar_t *argv[]) {
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
            /* Empty paths keep the defaults, as before. */
            case 'p':
                game_path_arg = optarg[0] != L'\0' ? optarg : NULL;
                break;
            case 'c':
                config_arg = optarg[0] != L'\0' ? optarg : NULL;
                break;
            case 'd':
                dll_arg = optarg[0] != L'\0' ? optarg : NULL;
                break;
            case 's':
                suspend = true;
                break;
            case '?':
                if (optopt != 0 || optind < 1 || optind > argc) {
                    ML_LOG_ERROR(L"launcher", L"bad argument: -%c", optopt);
                } else {
                    ML_LOG_ERROR(L"launcher", L"bad argument: %ls", argv[optind - 1]);
                }
                return false;
            case ':':
                ML_LOG_ERROR(L"launcher", L"missing argument for: -%c", optopt);
                return false;
            default:
                break;
        }
    }
    return true;
}

static bool init_launcher_dir(void) {
    const DWORD length = GetModuleFileNameW(NULL, launcher_dir, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        launcher_dir[0] = L'\0';
        ML_LOG_ERROR(L"launcher", L"could not determine the launcher directory");
        return false;
    }
    PathRemoveFileSpecW(launcher_dir);
    return true;
}

/* Resolves -c, or an inherited YAFSML_CONFIG when -c is absent, the way the
 * DLL does: relative to the DLL directory, whose YAFSML.ini is also the
 * default. A request is exported to the DLL as an absolute path. */
static bool resolve_config(wchar_t config_path[MAX_PATH], const wchar_t *dll_path, bool *exported) {
    wchar_t dll_dir[MAX_PATH];
    wchar_t *inherited = config_arg == NULL ? read_environment(L"YAFSML_CONFIG") : NULL;
    const wchar_t *requested = config_arg != NULL ? config_arg : inherited;
    bool resolved;
    lstrcpyW(dll_dir, dll_path);
    PathRemoveFileSpecW(dll_dir);
    resolved = ml_launcher_resolve_config_path(config_path, dll_dir, requested);
    if (!resolved) {
        ML_LOG_ERROR(L"launcher", L"invalid configuration path `%ls`",
                     requested != NULL ? requested : L"YAFSML.ini");
    }
    *exported = requested != NULL;
    free(inherited);
    return resolved;
}

static bool select_config_game(const wchar_t *config_path) {
    const ml_game_descriptor_t *config_game = NULL;
    ml_launcher_game_config_result_t result = ML_LAUNCHER_GAME_CONFIG_NOT_SPECIFIED;
    wchar_t ini_path[MAX_PATH];
    wchar_t invalid_key[64];
    /* TOML is handled by the DLL and has no INI launcher game setting. */
    if (ml_launcher_config_ini_path(ini_path, config_path) &&
        lstrcmpiW(PathFindExtensionW(ini_path), L".toml") != 0 && is_file(ini_path)) {
        result = ml_launcher_game_from_ini_file(ini_path, &config_game, invalid_key,
                                                sizeof(invalid_key) / sizeof(invalid_key[0]));
    }
    if (result == ML_LAUNCHER_GAME_CONFIG_INVALID) {
        ML_LOG_ERROR(L"launcher", L"unsupported game in `%ls`: %ls", ini_path, invalid_key);
        return false;
    }
    if (result == ML_LAUNCHER_GAME_CONFIG_ERROR) {
        ML_LOG_ERROR(L"launcher", L"could not read or parse configuration `%ls`", ini_path);
        return false;
    }
    launch_game = ml_launcher_select_game(NULL, config_game);
    return true;
}

static bool resolve_modloader_dll(wchar_t dll_path[MAX_PATH]) {
    /* Relative paths are relative to the launcher. An explicit DLL that does
     * not exist is an error rather than a reason to inject YAFSML.dll. */
    const wchar_t *requested = dll_arg != NULL ? dll_arg : L"YAFSML.dll";
    if (ml_launcher_resolve_path(dll_path, launcher_dir, requested) && is_file(dll_path)) return true;
    ML_LOG_ERROR(L"launcher", L"could not find mod loader DLL `%ls`",
                 dll_path[0] != L'\0' ? dll_path : requested);
    return false;
}

/* Accepts a game executable, or a directory containing one at a known path. */
static bool find_game_executable(wchar_t game_path[MAX_PATH], const wchar_t *base_dir,
                                 const wchar_t *requested) {
    wchar_t candidate[MAX_PATH];
    if (!ml_launcher_resolve_path(candidate, base_dir, requested)) return false;
    if (is_file(candidate)) {
        lstrcpyW(game_path, candidate);
        return true;
    }
    if (!PathIsDirectoryW(candidate)) return false;
    for (size_t i = 0; i < launch_game->exe_relpath_count; i++) {
        if (PathCombineW(game_path, candidate, launch_game->exe_relpaths[i]) != NULL &&
            is_file(game_path)) {
            return true;
        }
    }
    return false;
}

static bool locate_game_executable(wchar_t game_path[MAX_PATH]) {
    wchar_t steam_folder[MAX_PATH];
    if (game_path_arg != NULL) {
        /* A relative path is relative to the working directory, and then to
         * the launcher directory. An explicit path without the game is an
         * error rather than a reason to start a game found through Steam. */
        if (find_game_executable(game_path, NULL, game_path_arg) ||
            (ml_launcher_path_is_relative(game_path_arg) &&
             find_game_executable(game_path, launcher_dir, game_path_arg))) {
            return true;
        }
        ML_LOG_ERROR(L"launcher", L"could not find %ls at `%ls`", launch_game->title, game_path_arg);
        return false;
    }
    if (find_game_executable(game_path, NULL, launcher_dir)) return true;
    if (!app_find_game_path(launch_game->steam_app_id, steam_folder)) {
        ML_LOG_ERROR(L"launcher", L"could not find %ls through Steam", launch_game->title);
        return false;
    }
    if (!find_game_executable(game_path, NULL, steam_folder)) {
        ML_LOG_ERROR(L"launcher", L"could not find %ls executable in `%ls`", launch_game->title, steam_folder);
        return false;
    }
    return true;
}

/* The game executable outranks the configuration's game=... value, but must
 * agree with an explicit --launch-target. */
static bool select_executable_game(const wchar_t *game_path) {
    const ml_game_descriptor_t *game = ml_game_by_exe_path(game_path);
    if (game == NULL || game == launch_game) return true;
    if (launch_game_explicit) {
        ML_LOG_ERROR(L"launcher", L"`%ls` is the %ls executable, but --launch-target selects %ls",
                     game_path, game->title, launch_game->title);
        return false;
    }
    launch_game = game;
    return true;
}

static void export_environment(const wchar_t *config_path, bool export_config) {
    if (export_config) SetEnvironmentVariableW(L"YAFSML_CONFIG", config_path);
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
}

static void report_start_failure(DWORD error, const wchar_t *game_path) {
    wchar_t text[512];
    if (error == ERROR_ELEVATION_REQUIRED) {
        ML_LOG_ERROR(L"launcher", L"could not start %ls: `%ls` requires administrator rights (error %lu); "
                     L"run YAFSML.exe as administrator, or turn off \"Run this program as an administrator\" "
                     L"for the game executable", launch_game->title, game_path, error);
        return;
    }
    describe_error(error, PathFindFileNameW(game_path), text, sizeof(text) / sizeof(text[0]));
    ML_LOG_ERROR(L"launcher", L"could not start %ls `%ls`: %ls (error %lu)",
                 launch_game->title, game_path, text, error);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR lpCmdLine, int nShowCmd) {
    STARTUPINFOW si = {0};
    PROCESS_INFORMATION pi = {0};
    wchar_t config_path[MAX_PATH];
    wchar_t dll_path[MAX_PATH];
    wchar_t game_path[MAX_PATH];
    wchar_t game_folder[MAX_PATH];
    wchar_t proxy_path[MAX_PATH];
    bool export_config = false;
    bool success = true;

    UNREFERENCED_PARAMETER(hInstance);
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);
    UNREFERENCED_PARAMETER(nShowCmd);
    ml_log_set_sink(report_message);
    if (!parse_args(__argc, __wargv) || !init_launcher_dir() || !resolve_modloader_dll(dll_path) ||
        !resolve_config(config_path, dll_path, &export_config)) {
        return -1;
    }
    if (!launch_game_explicit && !select_config_game(config_path)) return -1;
    if (!locate_game_executable(game_path) || !select_executable_game(game_path)) return -1;
    lstrcpyW(game_folder, game_path);
    PathRemoveFileSpecW(game_folder);
    /* With YAFSML_REMOTE_INIT set, a proxy copy loaded by the game returns from
     * DllMain without installing its forwarders, and the game crashes. */
    if (ml_launcher_find_proxy_copy(game_folder, proxy_path)) {
        ML_LOG_ERROR(L"launcher", L"a YAFSML proxy DLL is installed in the game folder: `%ls`; "
                     L"remove it to start %ls through YAFSML.exe, or start the game directly "
                     L"instead of through YAFSML.exe", proxy_path, launch_game->title);
        return -1;
    }
    export_environment(config_path, export_config);
    si.cb = sizeof(si);
    if (!ml_launcher_create_game_process(game_path, game_folder, &si, &pi)) {
        report_start_failure(GetLastError(), game_path);
        return -1;
    }
    if (!ml_launcher_inject_dll(pi.hProcess, dll_path)) {
        const DWORD error = GetLastError();
        wchar_t text[512];
        describe_error(error, PathFindFileNameW(dll_path), text, sizeof(text) / sizeof(text[0]));
        ML_LOG_ERROR(L"launcher", L"could not inject `%ls` into %ls: %ls (error %lu)",
                     dll_path, launch_game->title, text, error);
        TerminateProcess(pi.hProcess, (UINT)-1);
        success = false;
    }
    if (success && !suspend && ResumeThread(pi.hThread) == (DWORD)-1) {
        const DWORD error = GetLastError();
        ML_LOG_ERROR(L"launcher", L"could not resume %ls (error %lu)", launch_game->title, error);
        TerminateProcess(pi.hProcess, (UINT)-1);
        success = false;
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return success ? 0 : -1;
}
