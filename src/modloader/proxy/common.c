#include "common.h"

#include "dinput8.h"
#include "winhttp.h"
#include "log.h"

#include <shlwapi.h>

#include <stdio.h>

static HMODULE own_module(void) {
    HMODULE module;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              (LPCWSTR)proxy_DllCanUnloadNow, &module) ? module : NULL;
}

bool proxy_is_named(const wchar_t *name) {
    HMODULE module = own_module();
    wchar_t path[MAX_PATH];
    if (module == NULL) return false;
    if (GetModuleFileNameW(module, path, MAX_PATH) == 0) return false;
    return lstrcmpiW(PathFindFileNameW(path), name) == 0;
}

HMODULE proxy_load_system_dll(const wchar_t *name) {
    wchar_t path[MAX_PATH];
    wchar_t system_directory[MAX_PATH];
    UINT length = GetSystemDirectoryW(system_directory, MAX_PATH);
    HMODULE module;
    if (length == 0 || length >= MAX_PATH ||
        _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%ls\\%ls", system_directory, name) < 0) {
        ML_LOG_ERROR(L"proxy", L"cannot resolve system directory for %ls", name);
        return NULL;
    }
    module = LoadLibraryW(path);
    if (module == NULL) {
        ML_LOG_ERROR(L"proxy", L"cannot load original %ls library", name);
        return NULL;
    }
    /* Forwarding into ourselves would loop forever in the jmp stubs. */
    if (module == own_module()) {
        FreeLibrary(module);
        ML_LOG_ERROR(L"proxy", L"`%ls` resolved to the proxy itself", path);
        return NULL;
    }
    return module;
}

HRESULT WINAPI proxy_DllCanUnloadNow(void) {
    if (proxy_is_named(L"dinput8.dll")) return dinput8_DllCanUnloadNow();
    if (proxy_is_named(L"winhttp.dll")) return winhttp_DllCanUnloadNow();
    return E_NOINTERFACE;
}

HRESULT WINAPI proxy_DllGetClassObject(REFCLSID class_id, REFIID interface_id, void **object) {
    if (proxy_is_named(L"dinput8.dll")) return dinput8_DllGetClassObject(class_id, interface_id, object);
    if (proxy_is_named(L"winhttp.dll")) return winhttp_DllGetClassObject(class_id, interface_id, object);
    return E_NOINTERFACE;
}
