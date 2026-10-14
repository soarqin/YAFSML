/*
 * Copyright (C) 2026, Soar Qin<soarchin@gmail.com>
 * Licensed under the MIT license; see LICENSE.
 */

#include "launch_process.h"
#include "log.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef LONG (__cdecl *wine_get_unix_env_t)(const char *, char *, size_t);
typedef LONG (__cdecl *wine_set_unix_env_t)(const char *, const char *);

#define WINE_STATUS_SUCCESS ((LONG)0)
#define WINE_STATUS_BUFFER_TOO_SMALL ((LONG)0xc0000023)
#define WINE_STATUS_VARIABLE_NOT_FOUND ((LONG)0xc0000100)

typedef struct wine_locale_guard {
    wine_set_unix_env_t set_env;
    char *original_value;
    bool active;
} wine_locale_guard_t;

static void begin_wine_locale(wine_locale_guard_t *guard) {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    wine_get_unix_env_t get_env;
    wine_set_unix_env_t set_env;
    char *buffer = NULL;
    size_t capacity = 1024;
    LONG status;
    memset(guard, 0, sizeof(*guard));
    if (ntdll == NULL || GetProcAddress(ntdll, "wine_get_version") == NULL) return;
    get_env = (wine_get_unix_env_t)GetProcAddress(ntdll, "__wine_get_unix_env");
    set_env = (wine_set_unix_env_t)GetProcAddress(ntdll, "__wine_set_unix_env");
    if (get_env == NULL || set_env == NULL) {
        ML_LOG_WARN(L"launcher", L"Wine Unix environment exports unavailable; non-ANSI paths may fail to load");
        return;
    }
    for (;;) {
        char *next = realloc(buffer, capacity);
        if (next == NULL) {
            free(buffer);
            ML_LOG_WARN(L"launcher", L"could not allocate Wine locale buffer");
            return;
        }
        buffer = next;
        status = get_env("LC_ALL", buffer, capacity);
        if (status == WINE_STATUS_SUCCESS) {
            if (memchr(buffer, '\0', capacity) == NULL) {
                free(buffer);
                ML_LOG_WARN(L"launcher", L"Wine returned an unterminated locale value");
                return;
            }
            break;
        }
        if (status == WINE_STATUS_VARIABLE_NOT_FOUND) {
            free(buffer);
            buffer = NULL;
            break;
        }
        if (status != WINE_STATUS_BUFFER_TOO_SMALL || capacity > SIZE_MAX / 2) {
            free(buffer);
            ML_LOG_WARN(L"launcher", L"could not read Wine host locale (0x%08lx)", (unsigned long)status);
            return;
        }
        capacity *= 2;
    }
    /* Proton's steamclient sets Unix LC_ALL=C. Unset it only around spawning
     * the game so Wine can choose a UTF-8 host code page for non-ANSI paths. */
    status = set_env("LC_ALL", NULL);
    if (status < 0) {
        free(buffer);
        ML_LOG_WARN(L"launcher", L"could not override Wine host locale (0x%08lx)", (unsigned long)status);
        return;
    }
    guard->set_env = set_env;
    guard->original_value = buffer;
    guard->active = true;
    ML_LOG_INFO(L"launcher", L"overriding Wine host locale to support non-ANSI paths");
}

static void end_wine_locale(wine_locale_guard_t *guard) {
    if (!guard->active) return;
    LONG status = guard->set_env("LC_ALL", guard->original_value);
    if (status < 0) {
        ML_LOG_WARN(L"launcher", L"could not restore Wine host locale (0x%08lx)", (unsigned long)status);
    }
    free(guard->original_value);
}

BOOL ml_launcher_create_game_process(const wchar_t *executable,
                                      const wchar_t *working_directory,
                                      STARTUPINFOW *startup,
                                      PROCESS_INFORMATION *process) {
    wine_locale_guard_t guard;
    begin_wine_locale(&guard);
    BOOL success = CreateProcessW(executable, NULL, NULL, NULL, FALSE,
                                  CREATE_SUSPENDED, NULL, working_directory,
                                  startup, process);
    DWORD error = GetLastError();
    end_wine_locale(&guard);
    SetLastError(error);
    return success;
}

bool ml_launcher_wait_for_remote_thread(HANDLE process, HANDLE thread) {
    /* Check the process first, including when both handles are signaled. */
    const HANDLE handles[] = { process, thread };
    DWORD wait_result = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
    DWORD exit_code;
    if (wait_result == WAIT_OBJECT_0) {
        SetLastError(ERROR_PROCESS_ABORTED);
        return false;
    }
    if (wait_result != WAIT_OBJECT_0 + 1) return false;
    if (!GetExitCodeThread(thread, &exit_code)) return false;
    if (exit_code == 0) {
        SetLastError(ERROR_DLL_INIT_FAILED);
        return false;
    }
    return true;
}
