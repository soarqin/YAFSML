/*
 * Copyright (C) 2026, Soar Qin<soarchin@gmail.com>
 * Licensed under the MIT license; see LICENSE.
 */

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Spawn suspended with the quoted executable as the command line, temporarily
 * restoring Wine's UTF-8 host locale when possible. */
BOOL ml_launcher_create_game_process(const wchar_t *executable,
                                      const wchar_t *working_directory,
                                      STARTUPINFOW *startup,
                                      PROCESS_INFORMATION *process);
/* Waits for a remote thread while the game is alive and returns its exit code.
 * Fails with ERROR_PROCESS_ABORTED when the game exits first. The caller judges
 * the exit code: a LoadLibraryW thread returns only the low 32 bits of the
 * module address. */
bool ml_launcher_wait_for_remote_thread(HANDLE process, HANDLE thread, DWORD *exit_code);
/* Finds the module loaded from the absolute `module_path`, ignoring case. */
HMODULE ml_launcher_find_remote_module(HANDLE process, const wchar_t *module_path);
/* Loads the absolute `dll_path` into the game and runs its YAFSMLInit export;
 * initialization succeeds only when YAFSMLInit returns nonzero. */
bool ml_launcher_inject_dll(HANDLE process, const wchar_t *dll_path);
/* Finds a dinput8.dll, dxgi.dll or winhttp.dll in `game_folder` that exports
 * YAFSMLInit, i.e. a YAFSML proxy copy. */
bool ml_launcher_find_proxy_copy(const wchar_t *game_folder, wchar_t path[MAX_PATH]);
/* Long-path variant. Returns an owned CRT string, or NULL when none is found. */
wchar_t *ml_launcher_find_proxy_copy_alloc(const wchar_t *game_folder);

#ifdef __cplusplus
}
#endif
