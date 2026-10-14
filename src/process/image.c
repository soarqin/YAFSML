/*
 * Copyright (C) 2024,2025, Soar Qin<soarchin@gmail.com>

 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#include "image.h"

#include "pe.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

void *get_module_image_base(const wchar_t *module_name, size_t *size) {
    const HMODULE hModule = GetModuleHandleW(module_name);
    const IMAGE_NT_HEADERS64 *ntHeader = pe_nt_headers(hModule);
    if (ntHeader == NULL) {
        return NULL;
    }
    *size = ntHeader->OptionalHeader.SizeOfImage;
    return hModule;
}

void *get_module_entrypoint(const wchar_t *module_name) {
    BYTE *image_base = (BYTE *)GetModuleHandleW(module_name);
    const IMAGE_NT_HEADERS64 *nt_header = pe_nt_headers(image_base);
    if (nt_header == NULL) {
        return NULL;
    }
    const DWORD entrypoint = nt_header->OptionalHeader.AddressOfEntryPoint;
    if (entrypoint == 0 || entrypoint >= nt_header->OptionalHeader.SizeOfImage) {
        return NULL;
    }
    return image_base + entrypoint;
}
