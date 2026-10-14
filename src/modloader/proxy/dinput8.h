#pragma once

#include <stdbool.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#if defined(__cplusplus)
extern "C" {
#endif

extern bool load_dinput8_proxy(void);
extern HRESULT dinput8_DllCanUnloadNow(void);
extern HRESULT dinput8_DllGetClassObject(REFCLSID class_id, REFIID interface_id, void **object);

#if defined(__cplusplus)
}
#endif
