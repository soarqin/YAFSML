#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Runs once after the first successful SteamAPI_Init; returning false runs it
 * again after the next one. */
typedef bool (*ml_runtime_ready_callback_t)(void);

bool ml_runtime_ready_hook_install(const wchar_t *log_scope,
                                   ml_runtime_ready_callback_t callback);
bool ml_runtime_ready_hook_uninstall(const wchar_t *log_scope);

#ifdef __cplusplus
}
#endif
