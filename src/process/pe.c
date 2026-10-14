/*
 * Copyright (C) 2024,2025, Soar Qin<soarchin@gmail.com>

 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#include "pe.h"

#include <string.h>

const IMAGE_NT_HEADERS64 *pe_nt_headers(const void *image_base) {
    if (image_base == NULL) {
        return NULL;
    }

    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)image_base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
        return NULL;
    }

    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)((const uint8_t *)image_base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return NULL;
    }

    return nt;
}

/* VirtualSize, or SizeOfRawData for linkers that leave VirtualSize zero. */
static uint32_t pe_section_size(const IMAGE_SECTION_HEADER *sh) {
    return sh->Misc.VirtualSize != 0 ? sh->Misc.VirtualSize : sh->SizeOfRawData;
}

static bool section_name_equals(const IMAGE_SECTION_HEADER *sh, const char *name) {
    size_t len;

    if (sh == NULL || name == NULL) {
        return false;
    }

    len = strlen(name);
    if (len == 0 || len > IMAGE_SIZEOF_SHORT_NAME) {
        return false;
    }

    if (memcmp(sh->Name, name, len) != 0) {
        return false;
    }

    return len == IMAGE_SIZEOF_SHORT_NAME || sh->Name[len] == '\0';
}

const IMAGE_SECTION_HEADER *pe_section_by_name(void *image_base, const char *name) {
    const IMAGE_NT_HEADERS64 *nt = pe_nt_headers(image_base);
    if (nt == NULL) {
        return NULL;
    }

    const IMAGE_SECTION_HEADER *sections = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (section_name_equals(&sections[i], name)) {
            return &sections[i];
        }
    }

    return NULL;
}

void *pe_section_data(void *image_base, const IMAGE_SECTION_HEADER *sh, size_t *out_size) {
    if (image_base == NULL || sh == NULL) {
        if (out_size != NULL) {
            *out_size = 0;
        }
        return NULL;
    }

    if (out_size != NULL) {
        *out_size = (size_t)pe_section_size(sh);
    }

    return (uint8_t *)image_base + sh->VirtualAddress;
}

uint32_t pe_section_rva_start(const IMAGE_SECTION_HEADER *sh) {
    return sh != NULL ? sh->VirtualAddress : 0;
}

uint32_t pe_section_rva_end(const IMAGE_SECTION_HEADER *sh) {
    if (sh == NULL) {
        return 0;
    }

    return sh->VirtualAddress + pe_section_size(sh);
}

bool pe_section_contains_rva(const IMAGE_SECTION_HEADER *sh, uint32_t rva) {
    const uint32_t start = pe_section_rva_start(sh);
    const uint32_t end = pe_section_rva_end(sh);
    return start <= rva && rva < end;
}

bool pe_section_contains_va(void *image_base, const IMAGE_SECTION_HEADER *sh, const void *va) {
    uintptr_t base;
    uintptr_t ptr;
    uintptr_t start;
    uintptr_t end;

    if (image_base == NULL || sh == NULL || va == NULL) {
        return false;
    }

    base = (uintptr_t)image_base;
    ptr = (uintptr_t)va;
    start = base + pe_section_rva_start(sh);
    end = base + pe_section_rva_end(sh);

    return start <= ptr && ptr < end;
}
