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

/* Spawn suspended, temporarily restoring Wine's UTF-8 host locale when possible. */
BOOL ml_launcher_create_game_process(const wchar_t *executable,
                                      const wchar_t *working_directory,
                                      STARTUPINFOW *startup,
                                      PROCESS_INFORMATION *process);
/* Initialization succeeds only while the game is alive and the thread returns nonzero. */
bool ml_launcher_wait_for_remote_thread(HANDLE process, HANDLE thread);

#ifdef __cplusplus
}
#endif
