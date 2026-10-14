#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <stdbool.h>

#if defined(__cplusplus)
extern "C" {
#endif

extern bool load_winhttp_proxy(void);
extern HRESULT winhttp_DllCanUnloadNow(void);
extern HRESULT winhttp_DllGetClassObject(REFCLSID class_id, REFIID interface_id, void **object);

#if defined(__cplusplus)
}
#endif
