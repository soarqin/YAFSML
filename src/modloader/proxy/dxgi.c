#include "dxgi.h"
#include "common.h"
#include "log.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

static HMODULE dxgi_dll;

/* Original entry points resolved at load time. Pure pass-through exports jump
 * through these via naked stubs in proxy_stubs.asm; the COM entry points keep C
 * wrappers because proxy/common.c dispatches them by host module name. */
void *dxgi_orig_ApplyCompatResolutionQuirking;
void *dxgi_orig_CompatString;
void *dxgi_orig_CompatValue;
void *dxgi_orig_CreateDXGIFactory;
void *dxgi_orig_CreateDXGIFactory1;
void *dxgi_orig_CreateDXGIFactory2;
void *dxgi_orig_DXGID3D10CreateDevice;
void *dxgi_orig_DXGID3D10CreateLayeredDevice;
void *dxgi_orig_DXGID3D10GetLayeredDeviceSize;
void *dxgi_orig_DXGID3D10RegisterLayers;
void *dxgi_orig_DXGIDeclareAdapterRemovalSupport;
void *dxgi_orig_DXGIDisableVBlankVirtualization;
void *dxgi_orig_DXGIDumpJournal;
void *dxgi_orig_DXGIGetDebugInterface1;
void *dxgi_orig_DXGIReportAdapterConfiguration;
void *dxgi_orig_PIXBeginCapture;
void *dxgi_orig_PIXEndCapture;
void *dxgi_orig_PIXGetCaptureState;
void *dxgi_orig_SetAppCompatStringPointer;
void *dxgi_orig_UpdateHMDEmulationStatus;

bool load_dxgi_proxy(void) {
    dxgi_dll = proxy_load_system_dll(L"dxgi.dll");
    if (dxgi_dll == NULL) return false;
    dxgi_orig_ApplyCompatResolutionQuirking = (void *)GetProcAddress(dxgi_dll, "ApplyCompatResolutionQuirking");
    dxgi_orig_CompatString = (void *)GetProcAddress(dxgi_dll, "CompatString");
    dxgi_orig_CompatValue = (void *)GetProcAddress(dxgi_dll, "CompatValue");
    dxgi_orig_CreateDXGIFactory = (void *)GetProcAddress(dxgi_dll, "CreateDXGIFactory");
    dxgi_orig_CreateDXGIFactory1 = (void *)GetProcAddress(dxgi_dll, "CreateDXGIFactory1");
    dxgi_orig_CreateDXGIFactory2 = (void *)GetProcAddress(dxgi_dll, "CreateDXGIFactory2");
    dxgi_orig_DXGID3D10CreateDevice = (void *)GetProcAddress(dxgi_dll, "DXGID3D10CreateDevice");
    dxgi_orig_DXGID3D10CreateLayeredDevice = (void *)GetProcAddress(dxgi_dll, "DXGID3D10CreateLayeredDevice");
    dxgi_orig_DXGID3D10GetLayeredDeviceSize = (void *)GetProcAddress(dxgi_dll, "DXGID3D10GetLayeredDeviceSize");
    dxgi_orig_DXGID3D10RegisterLayers = (void *)GetProcAddress(dxgi_dll, "DXGID3D10RegisterLayers");
    dxgi_orig_DXGIDeclareAdapterRemovalSupport = (void *)GetProcAddress(dxgi_dll, "DXGIDeclareAdapterRemovalSupport");
    dxgi_orig_DXGIDisableVBlankVirtualization = (void *)GetProcAddress(dxgi_dll, "DXGIDisableVBlankVirtualization");
    dxgi_orig_DXGIDumpJournal = (void *)GetProcAddress(dxgi_dll, "DXGIDumpJournal");
    dxgi_orig_DXGIGetDebugInterface1 = (void *)GetProcAddress(dxgi_dll, "DXGIGetDebugInterface1");
    dxgi_orig_DXGIReportAdapterConfiguration = (void *)GetProcAddress(dxgi_dll, "DXGIReportAdapterConfiguration");
    dxgi_orig_PIXBeginCapture = (void *)GetProcAddress(dxgi_dll, "PIXBeginCapture");
    dxgi_orig_PIXEndCapture = (void *)GetProcAddress(dxgi_dll, "PIXEndCapture");
    dxgi_orig_PIXGetCaptureState = (void *)GetProcAddress(dxgi_dll, "PIXGetCaptureState");
    dxgi_orig_SetAppCompatStringPointer = (void *)GetProcAddress(dxgi_dll, "SetAppCompatStringPointer");
    dxgi_orig_UpdateHMDEmulationStatus = (void *)GetProcAddress(dxgi_dll, "UpdateHMDEmulationStatus");
    return true;
}
