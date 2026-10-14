#include "dinput8.h"
#include "common.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static HMODULE dinput8_dll;

/* Original entry points resolved at load time. Pure pass-through exports jump
 * through these via naked stubs in proxy_stubs.asm; the COM entry points keep C
 * wrappers because proxy/common.c dispatches them by host module name. */
void *dinput8_orig_DirectInput8Create;
void *dinput8_orig_DllCanUnloadNow;
void *dinput8_orig_DllGetClassObject;
void *dinput8_orig_DllRegisterServer;
void *dinput8_orig_DllUnregisterServer;
void *dinput8_orig_GetdfDIJoystick;

HRESULT dinput8_DllCanUnloadNow(void) {
    typedef HRESULT (STDAPICALLTYPE *function_t)(void);
    return dinput8_orig_DllCanUnloadNow == NULL ? E_NOINTERFACE : ((function_t)dinput8_orig_DllCanUnloadNow)();
}

HRESULT dinput8_DllGetClassObject(REFCLSID class_id, REFIID interface_id, void **object) {
    typedef HRESULT (STDAPICALLTYPE *function_t)(REFCLSID, REFIID, void **);
    return dinput8_orig_DllGetClassObject == NULL
        ? E_NOINTERFACE
        : ((function_t)dinput8_orig_DllGetClassObject)(class_id, interface_id, object);
}

bool load_dinput8_proxy(void) {
    dinput8_dll = proxy_load_system_dll(L"dinput8.dll");
    if (dinput8_dll == NULL) return false;
    dinput8_orig_DirectInput8Create = (void *)GetProcAddress(dinput8_dll, "DirectInput8Create");
    dinput8_orig_DllCanUnloadNow = (void *)GetProcAddress(dinput8_dll, "DllCanUnloadNow");
    dinput8_orig_DllGetClassObject = (void *)GetProcAddress(dinput8_dll, "DllGetClassObject");
    dinput8_orig_DllRegisterServer = (void *)GetProcAddress(dinput8_dll, "DllRegisterServer");
    dinput8_orig_DllUnregisterServer = (void *)GetProcAddress(dinput8_dll, "DllUnregisterServer");
    dinput8_orig_GetdfDIJoystick = (void *)GetProcAddress(dinput8_dll, "GetdfDIJoystick");
    return true;
}
