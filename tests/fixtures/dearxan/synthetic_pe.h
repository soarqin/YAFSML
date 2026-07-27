#ifndef DEARXAN_SYNTHETIC_PE_H
#define DEARXAN_SYNTHETIC_PE_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static IMAGE_NT_HEADERS64 *dearxan_fixture_init_pe(unsigned char *bytes,
                                                   size_t size,
                                                   uint64_t preferred_base,
                                                   uint32_t entrypoint_rva,
                                                   WORD section_count) {
    IMAGE_DOS_HEADER *dos;
    IMAGE_NT_HEADERS64 *nt;

    memset(bytes, 0, size);
    dos = (IMAGE_DOS_HEADER *)bytes;
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = 0x80;
    nt = (IMAGE_NT_HEADERS64 *)(bytes + dos->e_lfanew);
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
    nt->FileHeader.NumberOfSections = section_count;
    nt->FileHeader.SizeOfOptionalHeader = sizeof(nt->OptionalHeader);
    nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    nt->OptionalHeader.ImageBase = preferred_base;
    nt->OptionalHeader.AddressOfEntryPoint = entrypoint_rva;
    nt->OptionalHeader.SectionAlignment = 0x1000;
    nt->OptionalHeader.FileAlignment = 0x200;
    nt->OptionalHeader.SizeOfImage = (DWORD)size;
    nt->OptionalHeader.SizeOfHeaders = 0x200;
    nt->OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
    return nt;
}

static IMAGE_SECTION_HEADER *dearxan_fixture_sections(IMAGE_NT_HEADERS64 *nt) {
    return IMAGE_FIRST_SECTION(nt);
}

#endif
