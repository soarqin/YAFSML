#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test_common.h"
#include "dearxan/encryption.h"
#include "dearxan/image.h"
#include "fixtures/dearxan/synthetic_pe.h"

typedef struct callback_state {
    size_t count;
    uint64_t values[4];
    size_t sizes[4];
    bool stop;
} callback_state_t;

static bool section_callback(uint64_t va, const unsigned char *bytes,
                             size_t size, void *opaque) {
    callback_state_t *state = opaque;
    if (bytes == NULL) return false;
    state->values[state->count] = va;
    state->sizes[state->count] = size;
    state->count++;
    return !state->stop;
}

static bool relocation_callback(uint32_t rva, void *opaque) {
    callback_state_t *state = opaque;
    state->values[state->count++] = rva;
    return !state->stop;
}

static int test_image_and_sections(void) {
    unsigned char bytes[0x1000];
    const uint64_t preferred_base = UINT64_C(0x140000000);
    IMAGE_NT_HEADERS64 *nt = dearxan_fixture_init_pe(
        bytes, sizeof(bytes), preferred_base, 0x300, 2);
    IMAGE_SECTION_HEADER *sections = dearxan_fixture_sections(nt);
    dearxan_image_t image;
    callback_state_t state = { 0 };
    size_t available = 0;

    memcpy(sections[0].Name, ".text", 5);
    sections[0].VirtualAddress = 0x200;
    sections[0].Misc.VirtualSize = 0x100;
    memcpy(sections[1].Name, ".data", 5);
    sections[1].VirtualAddress = 0xf00;
    sections[1].Misc.VirtualSize = 0x400;

    EXPECT_TRUE(dearxan_image_from_module(bytes, &image));
    EXPECT_TRUE(image.base == bytes);
    EXPECT_EQ(image.size, sizeof(bytes));
    EXPECT_EQ(image.base_va, (uint64_t)(uintptr_t)bytes);
    EXPECT_EQ(image.preferred_base, preferred_base);
    EXPECT_EQ(image.entrypoint_rva, 0x300);
    EXPECT_TRUE(dearxan_image_read(&image, image.base_va + 0x20, 4, &available) ==
                bytes + 0x20);
    EXPECT_EQ(available, sizeof(bytes) - 0x20);
    EXPECT_TRUE(dearxan_image_read(&image, image.base_va + sizeof(bytes), 0, NULL) ==
                bytes + sizeof(bytes));
    EXPECT_NULL(dearxan_image_read(&image, image.base_va - 1, 1, NULL));
    EXPECT_NULL(dearxan_image_read(&image, image.base_va + sizeof(bytes), 1, NULL));

    EXPECT_TRUE(dearxan_image_for_each_section(&image, section_callback, &state));
    EXPECT_EQ(state.count, 2);
    EXPECT_EQ(state.values[0], image.base_va + 0x200);
    EXPECT_EQ(state.sizes[0], 0x100);
    EXPECT_EQ(state.values[1], image.base_va + 0xf00);
    EXPECT_EQ(state.sizes[1], 0x100);
    state = (callback_state_t){ 0 };
    state.stop = true;
    EXPECT_TRUE(!dearxan_image_for_each_section(&image, section_callback, &state));
    EXPECT_EQ(state.count, 1);
    return 0;
}

static int test_relocations(void) {
    unsigned char bytes[0x1000];
    IMAGE_NT_HEADERS64 *nt = dearxan_fixture_init_pe(
        bytes, sizeof(bytes), UINT64_C(0x140000000), 0x200, 1);
    IMAGE_SECTION_HEADER *section = dearxan_fixture_sections(nt);
    IMAGE_BASE_RELOCATION *block;
    WORD *entries;
    dearxan_image_t image;
    callback_state_t state = { 0 };

    section->VirtualAddress = 0x200;
    section->Misc.VirtualSize = 0x700;
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress = 0x700;
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size = 14;
    block = (IMAGE_BASE_RELOCATION *)(bytes + 0x700);
    block->VirtualAddress = 0x300;
    block->SizeOfBlock = 14;
    entries = (WORD *)(block + 1);
    entries[0] = (IMAGE_REL_BASED_DIR64 << 12) | 0x008;
    entries[1] = (IMAGE_REL_BASED_ABSOLUTE << 12) | 0x010;
    entries[2] = (IMAGE_REL_BASED_DIR64 << 12) | 0x120;

    EXPECT_TRUE(dearxan_image_from_module(bytes, &image));
    EXPECT_TRUE(dearxan_image_for_each_relocation64(&image,
                                                    relocation_callback, &state));
    EXPECT_EQ(state.count, 2);
    EXPECT_EQ(state.values[0], 0x308);
    EXPECT_EQ(state.values[1], 0x420);
    state = (callback_state_t){ 0 };
    state.stop = true;
    EXPECT_TRUE(!dearxan_image_for_each_relocation64(&image,
                                                     relocation_callback, &state));
    EXPECT_EQ(state.count, 1);

    block->SizeOfBlock = 7;
    EXPECT_TRUE(!dearxan_image_for_each_relocation64(&image,
                                                     relocation_callback, &state));
    block->SizeOfBlock = 14;
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size =
        (DWORD)(sizeof(bytes) - 0x700 + 1);
    EXPECT_TRUE(!dearxan_image_for_each_relocation64(&image,
                                                     relocation_callback, &state));
    return 0;
}

static int test_apply_relocations(void) {
    unsigned char bytes[0x1000];
    const uint64_t preferred_base = UINT64_C(0x10000000);
    IMAGE_NT_HEADERS64 *nt = dearxan_fixture_init_pe(
        bytes, sizeof(bytes), preferred_base, 0x200, 1);
    IMAGE_SECTION_HEADER *section = dearxan_fixture_sections(nt);
    IMAGE_BASE_RELOCATION *block;
    WORD *entry;
    dearxan_image_t image;
    dearxan_encrypted_region_t region = { 0, 16, 0x300 };
    unsigned char decrypted[16] = { 0 };
    dearxan_encrypted_region_list_t list = {
        DEARXAN_DECRYPTION_TEA, &region, 1, decrypted, sizeof(decrypted)
    };
    uint64_t value = preferred_base + 0x500;
    uint64_t relocated;

    section->VirtualAddress = 0x200;
    section->Misc.VirtualSize = 0x700;
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress = 0x700;
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size = 10;
    block = (IMAGE_BASE_RELOCATION *)(bytes + 0x700);
    block->VirtualAddress = 0x300;
    block->SizeOfBlock = 10;
    entry = (WORD *)(block + 1);
    entry[0] = (IMAGE_REL_BASED_DIR64 << 12) | 0x004;
    memcpy(decrypted + 4, &value, sizeof(value));

    EXPECT_TRUE(dearxan_image_from_module(bytes, &image));
    EXPECT_TRUE(dearxan_apply_relocations(&image, &list));
    memcpy(&relocated, decrypted + 4, sizeof(relocated));
    EXPECT_EQ(relocated, image.base_va + 0x500);

    value = preferred_base + sizeof(bytes) + 1;
    memcpy(decrypted + 4, &value, sizeof(value));
    EXPECT_TRUE(!dearxan_apply_relocations(&image, &list));
    return 0;
}

static int test_preferred_base_from_file(void) {
    unsigned char bytes[0x1000];
    const uint64_t actual_base = UINT64_C(0x180000000);
    const uint64_t preferred_base = UINT64_C(0x140000000);
    IMAGE_NT_HEADERS64 *nt;
    IMAGE_BASE_RELOCATION *block;
    dearxan_encrypted_region_t region = { 0, 16, 0x300 };
    unsigned char decrypted[16] = { 0 };
    dearxan_encrypted_region_list_t list = {
        DEARXAN_DECRYPTION_TEA, &region, 1, decrypted, sizeof(decrypted)
    };
    uint64_t value = preferred_base + 0x500;
    wchar_t path[MAX_PATH];
    HANDLE file;
    DWORD written;
    dearxan_image_t image;

    nt = dearxan_fixture_init_pe(bytes, sizeof(bytes), preferred_base, 0x200, 0);
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress = 0x700;
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size = 10;
    block = (IMAGE_BASE_RELOCATION *)(bytes + 0x700);
    block->VirtualAddress = 0x300;
    block->SizeOfBlock = 10;
    ((WORD *)(block + 1))[0] = (IMAGE_REL_BASED_DIR64 << 12) | 0x004;
    memcpy(decrypted + 4, &value, sizeof(value));
    EXPECT_TRUE(GetTempPathW(MAX_PATH, path) != 0);
    EXPECT_TRUE(GetTempFileNameW(path, L"dxn", 0, path) != 0);
    file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    EXPECT_TRUE(file != INVALID_HANDLE_VALUE);
    EXPECT_TRUE(WriteFile(file, bytes, sizeof(bytes), &written, NULL));
    EXPECT_EQ(written, sizeof(bytes));
    CloseHandle(file);

    ((IMAGE_NT_HEADERS64 *)(bytes + ((IMAGE_DOS_HEADER *)bytes)->e_lfanew))
        ->OptionalHeader.ImageBase = actual_base;
    EXPECT_TRUE(dearxan_image_from_module(bytes, &image));
    EXPECT_EQ(image.preferred_base, actual_base);
    EXPECT_TRUE(dearxan_image_set_preferred_base_from_file(&image, path));
    EXPECT_EQ(image.preferred_base, preferred_base);
    image.base_va = actual_base;
    EXPECT_TRUE(dearxan_apply_relocations(&image, &list));
    memcpy(&value, decrypted + 4, sizeof(value));
    EXPECT_EQ(value, actual_base + 0x500);
    DeleteFileW(path);
    return 0;
}

static int test_invalid_headers(void) {
    unsigned char bytes[0x400];
    dearxan_image_t image;
    IMAGE_NT_HEADERS64 *nt = dearxan_fixture_init_pe(
        bytes, sizeof(bytes), UINT64_C(0x140000000), 0x200, 0);

    ((IMAGE_DOS_HEADER *)bytes)->e_magic = 0;
    EXPECT_TRUE(!dearxan_image_from_module(bytes, &image));
    ((IMAGE_DOS_HEADER *)bytes)->e_magic = IMAGE_DOS_SIGNATURE;
    nt->Signature = 0;
    EXPECT_TRUE(!dearxan_image_from_module(bytes, &image));
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
    EXPECT_TRUE(!dearxan_image_from_module(bytes, &image));
    return 0;
}

int main(void) {
    if (test_image_and_sections() != 0 || test_relocations() != 0 ||
        test_apply_relocations() != 0 ||
        test_preferred_base_from_file() != 0 ||
        test_invalid_headers() != 0) return 1;
    printf("smoke_dearxan_image: all tests passed\n");
    return 0;
}
