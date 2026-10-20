/*
 * Copyright (C) 2026, Soar Qin<soarchin@gmail.com>
 * Licensed under the MIT license; see LICENSE.
 */

#include "launch_process.h"
#include "log.h"

#define ML_PATH_USE_CRT_ALLOCATOR
#include "common/path.h"

#include <psapi.h>
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
    /* The game inherits these spellings as its image path, argv[0] and current
     * directory, which holds MAX_PATH - 2 characters plus a separator. */
    wchar_t *application = ml_path_plain_alloc(executable, MAX_PATH);
    wchar_t *directory = working_directory == NULL ? NULL : ml_path_plain_alloc(working_directory, MAX_PATH - 1);
    wchar_t *command_line = NULL;
    wine_locale_guard_t guard;
    BOOL success = FALSE;
    DWORD error = ERROR_NOT_ENOUGH_MEMORY;
    if (application != NULL && (working_directory == NULL || directory != NULL)) {
        const size_t length = wcslen(application);
        /* Without a command line the game sees the unquoted path, which splits
         * into several arguments when it contains spaces. */
        command_line = malloc((length + 3) * sizeof(*command_line));
        if (command_line != NULL) {
            command_line[0] = L'"';
            memcpy(command_line + 1, application, length * sizeof(*command_line));
            command_line[length + 1] = L'"';
            command_line[length + 2] = L'\0';
            begin_wine_locale(&guard);
            success = CreateProcessW(application, command_line, NULL, NULL, FALSE,
                                     CREATE_SUSPENDED, NULL, directory, startup, process);
            error = GetLastError();
            end_wine_locale(&guard);
        }
    }
    free(command_line);
    free(directory);
    free(application);
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
    wchar_t *requested;
    if (module_path == NULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    requested = ml_path_resolve_alloc(NULL, module_path);
    if (requested == NULL) return NULL;
    for (;;) {
        HMODULE *grown = realloc(modules, capacity);
        if (grown == NULL) {
            free(modules);
            free(requested);
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return NULL;
        }
        modules = grown;
        if (!EnumProcessModules(process, modules, capacity, &needed)) {
            const DWORD error = GetLastError();
            free(modules);
            free(requested);
            SetLastError(error);
            return NULL;
        }
        if (needed <= capacity) break;
        /* Retry with the reported size; modules can load in the meantime. */
        capacity = needed;
    }
    for (DWORD i = 0; i < needed / sizeof(HMODULE) && found == NULL; i++) {
        wchar_t *name = NULL;
        DWORD name_capacity = 256;
        while (name_capacity <= ML_PATH_MAX_CCH) {
            DWORD length;
            name = ml_path_buffer(name_capacity);
            if (name == NULL) break;
            length = GetModuleFileNameExW(process, modules[i], name, name_capacity);
            if (length != 0 && length < name_capacity) break;
            free(name);
            name = NULL;
            if (length == 0) break;
            name_capacity *= 2;
        }
        /* A base name is ambiguous: `-d ...\dxgi.dll` would match System32's dxgi.dll. */
        if (name != NULL) {
            wchar_t *normalized = ml_path_resolve_alloc(NULL, name);
            if (normalized != NULL && CompareStringOrdinal(normalized, -1, requested, -1, TRUE) == CSTR_EQUAL)
                found = modules[i];
            free(normalized);
            free(name);
        }
    }
    free(modules);
    free(requested);
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

wchar_t *ml_launcher_find_proxy_copy_alloc(const wchar_t *game_folder) {
    static const wchar_t *const proxy_names[] = { L"dinput8.dll", L"dxgi.dll", L"winhttp.dll" };
    for (size_t i = 0; i < sizeof(proxy_names) / sizeof(proxy_names[0]); i++) {
        HMODULE module;
        bool exports_init;
        wchar_t *path = ml_path_resolve_alloc(game_folder, proxy_names[i]);
        if (path == NULL) continue;
        /* Map the image without running its entry point, as for injection;
         * other DLLs with these names, such as graphics wrappers, pass. */
        module = LoadLibraryExW(path, NULL, DONT_RESOLVE_DLL_REFERENCES);
        if (module == NULL) {
            free(path);
            continue;
        }
        exports_init = GetProcAddress(module, "YAFSMLInit") != NULL;
        FreeLibrary(module);
        if (exports_init) return path;
        free(path);
    }
    return NULL;
}
