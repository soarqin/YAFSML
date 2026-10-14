/*
 * Copyright (C) 2026, Soar Qin<soarchin@gmail.com>
 * Licensed under the MIT license; see LICENSE.
 */

#include "launch_process.h"
#include "log.h"

#include <psapi.h>
#include <shlwapi.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Initial EnumProcessModules capacity; the buffer grows when it is too small. */
#ifndef ML_LAUNCHER_INITIAL_MODULES
#define ML_LAUNCHER_INITIAL_MODULES 256
#endif

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
    const size_t length = wcslen(executable);
    /* Without a command line the game sees the unquoted path, which splits
     * into several arguments when it contains spaces. */
    wchar_t *command_line = malloc((length + 3) * sizeof(*command_line));
    wine_locale_guard_t guard;
    if (command_line == NULL) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    command_line[0] = L'"';
    memcpy(command_line + 1, executable, length * sizeof(*command_line));
    command_line[length + 1] = L'"';
    command_line[length + 2] = L'\0';
    begin_wine_locale(&guard);
    BOOL success = CreateProcessW(executable, command_line, NULL, NULL, FALSE,
                                  CREATE_SUSPENDED, NULL, working_directory,
                                  startup, process);
    DWORD error = GetLastError();
    end_wine_locale(&guard);
    free(command_line);
    SetLastError(error);
    return success;
}

bool ml_launcher_wait_for_remote_thread(HANDLE process, HANDLE thread, DWORD *exit_code) {
    /* Check the process first, including when both handles are signaled. */
    const HANDLE handles[] = { process, thread };
    DWORD wait_result = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
    DWORD code;
    if (wait_result == WAIT_OBJECT_0) {
        SetLastError(ERROR_PROCESS_ABORTED);
        return false;
    }
    if (wait_result != WAIT_OBJECT_0 + 1) return false;
    if (!GetExitCodeThread(thread, &code)) return false;
    if (exit_code != NULL) *exit_code = code;
    return true;
}

HMODULE ml_launcher_find_remote_module(HANDLE process, const wchar_t *module_path) {
    DWORD capacity = ML_LAUNCHER_INITIAL_MODULES * sizeof(HMODULE);
    DWORD needed = 0;
    HMODULE *modules = NULL;
    HMODULE found = NULL;
    if (module_path == NULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    for (;;) {
        HMODULE *grown = realloc(modules, capacity);
        if (grown == NULL) {
            free(modules);
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return NULL;
        }
        modules = grown;
        if (!EnumProcessModules(process, modules, capacity, &needed)) {
            const DWORD error = GetLastError();
            free(modules);
            SetLastError(error);
            return NULL;
        }
        if (needed <= capacity) break;
        /* Retry with the reported size; modules can load in the meantime. */
        capacity = needed;
    }
    for (DWORD i = 0; i < needed / sizeof(HMODULE) && found == NULL; i++) {
        wchar_t name[MAX_PATH + 1];
        const DWORD length = GetModuleFileNameExW(process, modules[i], name, MAX_PATH + 1);
        /* A base name is ambiguous: `-d ...\dxgi.dll` would match System32's dxgi.dll. */
        if (length != 0 && length <= MAX_PATH &&
            CompareStringOrdinal(name, (int)length, module_path, -1, TRUE) == CSTR_EQUAL) {
            found = modules[i];
        }
    }
    free(modules);
    if (found == NULL) SetLastError(ERROR_MOD_NOT_FOUND);
    return found;
}

bool ml_launcher_inject_dll(HANDLE process, const wchar_t *dll_path) {
    const SIZE_T path_size = (wcslen(dll_path) + 1) * sizeof(wchar_t);
    const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    const LPTHREAD_START_ROUTINE load_library = kernel32 != NULL
        ? (LPTHREAD_START_ROUTINE)GetProcAddress(kernel32, "LoadLibraryW") : NULL;
    LPVOID remote_path;
    HANDLE thread;
    HMODULE remote_module;
    HMODULE local_module;
    FARPROC local_init;
    SIZE_T written = 0;
    DWORD exit_code = 0;
    DWORD error;
    BOOL wrote;
    bool completed;

    if (load_library == NULL) return false;
    remote_path = VirtualAllocEx(process, NULL, path_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (remote_path == NULL) return false;
    wrote = WriteProcessMemory(process, remote_path, dll_path, path_size, &written);
    if (!wrote || written != path_size) {
        error = wrote ? ERROR_PARTIAL_COPY : GetLastError();
        VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
        SetLastError(error);
        return false;
    }
    thread = CreateRemoteThread(process, NULL, 0, load_library, remote_path, 0, NULL);
    if (thread == NULL) {
        error = GetLastError();
        VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
        SetLastError(error);
        return false;
    }
    /* LoadLibraryW's exit code is the low half of the module address, which is
     * zero for a 4 GiB-aligned base; look the module up by path instead. */
    completed = ml_launcher_wait_for_remote_thread(process, thread, NULL);
    error = GetLastError();
    CloseHandle(thread);
    VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
    if (!completed) {
        SetLastError(error);
        return false;
    }

    remote_module = ml_launcher_find_remote_module(process, dll_path);
    if (remote_module == NULL) return false;
    local_module = LoadLibraryExW(dll_path, NULL, DONT_RESOLVE_DLL_REFERENCES);
    local_init = local_module != NULL ? GetProcAddress(local_module, "YAFSMLInit") : NULL;
    if (local_init == NULL) {
        error = GetLastError();
        if (local_module != NULL) FreeLibrary(local_module);
        SetLastError(error);
        return false;
    }
    const uintptr_t init_rva = (uintptr_t)local_init - (uintptr_t)local_module;
    const LPTHREAD_START_ROUTINE remote_init =
        (LPTHREAD_START_ROUTINE)((uintptr_t)remote_module + init_rva);
    FreeLibrary(local_module);
    thread = CreateRemoteThread(process, NULL, 0, remote_init, NULL, 0, NULL);
    if (thread == NULL) return false;
    completed = ml_launcher_wait_for_remote_thread(process, thread, &exit_code);
    error = GetLastError();
    CloseHandle(thread);
    if (!completed) {
        SetLastError(error);
        return false;
    }
    if (exit_code == 0) {
        SetLastError(ERROR_DLL_INIT_FAILED);
        return false;
    }
    return true;
}

bool ml_launcher_find_proxy_copy(const wchar_t *game_folder, wchar_t path[MAX_PATH]) {
    static const wchar_t *const proxy_names[] = { L"dinput8.dll", L"dxgi.dll", L"winhttp.dll" };
    for (size_t i = 0; i < sizeof(proxy_names) / sizeof(proxy_names[0]); i++) {
        HMODULE module;
        bool exports_init;
        if (PathCombineW(path, game_folder, proxy_names[i]) == NULL) continue;
        /* Map the image without running its entry point, as for injection;
         * other DLLs with these names, such as graphics wrappers, pass. */
        module = LoadLibraryExW(path, NULL, DONT_RESOLVE_DLL_REFERENCES);
        if (module == NULL) continue;
        exports_init = GetProcAddress(module, "YAFSMLInit") != NULL;
        FreeLibrary(module);
        if (exports_init) return true;
    }
    path[0] = L'\0';
    return false;
}
