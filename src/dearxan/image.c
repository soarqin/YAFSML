/*
 * C11 implementation of dearxan v0.5.3 functionality.
 *
 * Original project copyright (c) 2025 William Tremblay.
 * Licensed under the MIT license; see THIRD_PARTY_NOTICES.md.
 */

#include "image.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <stdlib.h>

static bool image_headers(const unsigned char *base, size_t known_size,
                          const IMAGE_NT_HEADERS64 **headers) {
    const IMAGE_DOS_HEADER *dos;
    size_t nt_offset;
    if (base == NULL || headers == NULL ||
        (known_size != 0 && known_size < sizeof(IMAGE_DOS_HEADER))) return false;
    dos = (const IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return false;
    nt_offset = (size_t)dos->e_lfanew;
    if (known_size != 0 &&
        (nt_offset > known_size || known_size - nt_offset < sizeof(IMAGE_NT_HEADERS64))) {
        return false;
    }
    *headers = (const IMAGE_NT_HEADERS64 *)(base + nt_offset);
    return (*headers)->Signature == IMAGE_NT_SIGNATURE &&
           (*headers)->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC;
}

bool dearxan_image_from_module(void *module, dearxan_image_t *image) {
    const IMAGE_NT_HEADERS64 *nt;
    if (module == NULL || image == NULL) return false;
    if (!image_headers((const unsigned char *)module, 0, &nt) ||
        nt->OptionalHeader.SizeOfImage == 0) return false;
    image->base = module;
    image->size = nt->OptionalHeader.SizeOfImage;
    image->base_va = (uint64_t)(uintptr_t)module;
    image->preferred_base = nt->OptionalHeader.ImageBase;
    image->entrypoint_rva = nt->OptionalHeader.AddressOfEntryPoint;
    return true;
}

bool dearxan_image_set_preferred_base_from_file(dearxan_image_t *image,
                                                 const wchar_t *path) {
    HANDLE file;
    unsigned char *headers = NULL;
    DWORD read_size;
    DWORD size;
    const IMAGE_NT_HEADERS64 *nt;
    bool result = false;
    if (image == NULL || path == NULL) return false;
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE |
                       FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return false;
    size = 0x1000;
    for (;;) {
        unsigned char *next = realloc(headers, size);
        if (next == NULL) goto done;
        headers = next;
        if (SetFilePointer(file, 0, NULL, FILE_BEGIN) == INVALID_SET_FILE_POINTER &&
            GetLastError() != NO_ERROR) goto done;
        if (!ReadFile(file, headers, size, &read_size, NULL) ||
            read_size < sizeof(IMAGE_DOS_HEADER)) goto done;
        if (image_headers(headers, read_size, &nt)) break;
        {
            const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)headers;
            uint64_t needed;
            if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) goto done;
            needed = (uint64_t)(uint32_t)dos->e_lfanew + sizeof(*nt);
            if (needed <= read_size || needed > UINT32_MAX) goto done;
            size = (DWORD)needed;
        }
    }
    if (nt->OptionalHeader.ImageBase == 0) goto done;
    image->preferred_base = nt->OptionalHeader.ImageBase;
    result = true;
done:
    free(headers);
    CloseHandle(file);
    return result;
}

const unsigned char *dearxan_image_read(const dearxan_image_t *image,
                                        uint64_t va, size_t min_size,
                                        size_t *available) {
    uint64_t offset;
    size_t remaining;
    if (image == NULL || va < image->base_va) return NULL;
    offset = va - image->base_va;
    if (offset > image->size) return NULL;
    remaining = image->size - (size_t)offset;
    if (remaining < min_size) return NULL;
    if (available != NULL) *available = remaining;
    return image->base + (size_t)offset;
}

static const IMAGE_NT_HEADERS64 *image_nt_headers(const dearxan_image_t *image) {
    const IMAGE_NT_HEADERS64 *nt;
    if (image == NULL || !image_headers(image->base, image->size, &nt)) return NULL;
    return nt;
}

bool dearxan_image_for_each_section(const dearxan_image_t *image,
                                    dearxan_section_callback_t callback,
                                    void *opaque) {
    const IMAGE_NT_HEADERS64 *nt = image_nt_headers(image);
    const IMAGE_SECTION_HEADER *section;
    size_t section_table_offset;
    if (nt == NULL || callback == NULL || nt->Signature != IMAGE_NT_SIGNATURE) return false;
    section = IMAGE_FIRST_SECTION(nt);
    section_table_offset = (const unsigned char *)section - image->base;
    if (section_table_offset > image->size ||
        nt->FileHeader.NumberOfSections >
            (image->size - section_table_offset) / sizeof(*section)) return false;
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        size_t rva = section[i].VirtualAddress;
        size_t size = section[i].Misc.VirtualSize;
        if (size == 0) size = section[i].SizeOfRawData;
        if (rva >= image->size) continue;
        if (size > image->size - rva) size = image->size - rva;
        if (!callback(image->base_va + rva, image->base + rva, size, opaque)) return false;
    }
    return true;
}

bool dearxan_image_for_each_relocation64(const dearxan_image_t *image,
                                         dearxan_relocation_callback_t callback,
                                         void *opaque) {
    const IMAGE_NT_HEADERS64 *nt = image_nt_headers(image);
    const IMAGE_DATA_DIRECTORY *directory;
    size_t offset = 0;
    if (nt == NULL || callback == NULL || nt->Signature != IMAGE_NT_SIGNATURE) return false;
    directory = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if (directory->VirtualAddress == 0 || directory->Size == 0) return true;
    if (directory->VirtualAddress >= image->size ||
        directory->Size > image->size - directory->VirtualAddress) return false;
    while (offset < directory->Size) {
        const IMAGE_BASE_RELOCATION *block;
        const WORD *entries;
        size_t count;
        if (directory->Size - offset < sizeof(*block)) return false;
        block = (const IMAGE_BASE_RELOCATION *)(image->base +
                  directory->VirtualAddress + offset);
        if (block->SizeOfBlock < sizeof(*block) ||
            block->SizeOfBlock > directory->Size - offset ||
            ((block->SizeOfBlock - sizeof(*block)) % sizeof(WORD)) != 0) return false;
        entries = (const WORD *)(block + 1);
        count = (block->SizeOfBlock - sizeof(*block)) / sizeof(*entries);
        for (size_t i = 0; i < count; i++) {
            if ((entries[i] >> 12) == IMAGE_REL_BASED_DIR64 &&
                !callback(block->VirtualAddress + (entries[i] & 0x0fff), opaque)) return false;
        }
        offset += block->SizeOfBlock;
    }
    return offset == directory->Size;
}

bool dearxan_image_make_sections_rwe(const dearxan_image_t *image) {
    const IMAGE_NT_HEADERS64 *nt = image_nt_headers(image);
    const IMAGE_SECTION_HEADER *section;
    size_t table_offset;
    if (nt == NULL) return false;
    section = IMAGE_FIRST_SECTION(nt);
    table_offset = (const unsigned char *)section - image->base;
    if (table_offset > image->size ||
        nt->FileHeader.NumberOfSections >
            (image->size - table_offset) / sizeof(*section)) return false;
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        size_t rva = section[i].VirtualAddress;
        size_t size = section[i].Misc.VirtualSize;
        DWORD old_protection;
        if (size == 0) size = section[i].SizeOfRawData;
        if (size == 0 || rva >= image->size) continue;
        if (size > image->size - rva) size = image->size - rva;
        if (!VirtualProtect((void *)(image->base + rva), size,
                            PAGE_EXECUTE_READWRITE, &old_protection)) return false;
    }
    return true;
}
