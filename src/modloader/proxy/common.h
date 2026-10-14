#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

/* Whether this DLL's file name is `name` (e.g. L"dxgi.dll"). */
bool proxy_is_named(const wchar_t *name);
/* Loads the System32 DLL `name`, refusing to load this module itself. */
HMODULE proxy_load_system_dll(const wchar_t *name);

HRESULT WINAPI proxy_DllCanUnloadNow(void);
HRESULT WINAPI proxy_DllGetClassObject(REFCLSID class_id, REFIID interface_id, void **object);

#ifdef __cplusplus
}
#endif
