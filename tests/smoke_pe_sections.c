#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "test_common.h"
#include "image.h"
#include "pe.h"

typedef struct fake_image_s {
    IMAGE_DOS_HEADER dos;
    IMAGE_NT_HEADERS64 nt;
    IMAGE_SECTION_HEADER sections[2];
} fake_image_t;

static void build_fake_image(fake_image_t *image) {
    memset(image, 0, sizeof(*image));
    image->dos.e_magic = IMAGE_DOS_SIGNATURE;
    image->dos.e_lfanew = (LONG)offsetof(fake_image_t, nt);
    image->nt.Signature = IMAGE_NT_SIGNATURE;
    image->nt.FileHeader.NumberOfSections = 2;
    image->nt.FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
    image->nt.OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    memcpy(image->sections[0].Name, ".text", 5);
    image->sections[0].VirtualAddress = 0x1000;
    image->sections[0].Misc.VirtualSize = 0x1234;
    image->sections[0].SizeOfRawData = 0x1400;
    /* Some linkers leave VirtualSize zero; the raw size stands in. */
    memcpy(image->sections[1].Name, ".bss", 4);
    image->sections[1].VirtualAddress = 0x3000;
    image->sections[1].SizeOfRawData = 0x200;
}

static int test_fake_image(void) {
    fake_image_t image;
    const IMAGE_SECTION_HEADER *text;
    const IMAGE_SECTION_HEADER *bss;
    size_t size = 0;

    build_fake_image(&image);
    EXPECT_EQ(pe_nt_headers(&image), &image.nt);
    EXPECT_EQ((const void *)IMAGE_FIRST_SECTION(&image.nt), (const void *)image.sections);

    text = pe_section_by_name(&image, ".text");
    bss = pe_section_by_name(&image, ".bss");
    EXPECT_EQ(text, &image.sections[0]);
    EXPECT_EQ(bss, &image.sections[1]);
    EXPECT_NULL(pe_section_by_name(&image, ".tex"));

    /* pe_section_data and pe_section_rva_end agree on the section size. */
    EXPECT_EQ(pe_section_data(&image, text, &size), (uint8_t *)&image + 0x1000);
    EXPECT_EQ(size, 0x1234);
    EXPECT_EQ(pe_section_rva_end(text), 0x1000 + 0x1234);
    EXPECT_EQ(pe_section_data(&image, bss, &size), (uint8_t *)&image + 0x3000);
    EXPECT_EQ(size, 0x200);
    EXPECT_EQ(pe_section_rva_end(bss), 0x3000 + 0x200);
    EXPECT_TRUE(pe_section_contains_rva(bss, 0x31FF));
    EXPECT_TRUE(!pe_section_contains_rva(bss, 0x3200));
    EXPECT_NULL(pe_section_data(&image, NULL, &size));
    EXPECT_EQ(size, 0);

    /* Every header check rejects the image. */
    image.dos.e_magic = 0;
    EXPECT_NULL(pe_nt_headers(&image));
    build_fake_image(&image);
    image.dos.e_lfanew = 0;
    EXPECT_NULL(pe_nt_headers(&image));
    build_fake_image(&image);
    image.nt.Signature = 0;
    EXPECT_NULL(pe_nt_headers(&image));
    build_fake_image(&image);
    image.nt.OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
    EXPECT_NULL(pe_nt_headers(&image));
    EXPECT_NULL(pe_section_by_name(&image, ".text"));
    EXPECT_NULL(pe_nt_headers(NULL));
    return 0;
}

static int test_module(HMODULE self) {
    const IMAGE_NT_HEADERS64 *nt = pe_nt_headers(self);
    const IMAGE_SECTION_HEADER *sections;
    size_t image_size = 0;

    EXPECT_NOT_NULL(nt);
    EXPECT_EQ(get_module_image_base(NULL, &image_size), (void *)self);
    EXPECT_EQ(image_size, nt->OptionalHeader.SizeOfImage);
    EXPECT_EQ(get_module_entrypoint(NULL), (void *)((uint8_t *)self + nt->OptionalHeader.AddressOfEntryPoint));
    EXPECT_NULL(get_module_image_base(L"smoke_pe_sections_missing.dll", &image_size));
    EXPECT_NULL(get_module_entrypoint(L"smoke_pe_sections_missing.dll"));

    sections = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        size_t size = 0;
        EXPECT_EQ(pe_section_data(self, &sections[i], &size), (void *)((uint8_t *)self + sections[i].VirtualAddress));
        EXPECT_EQ(size, (size_t)(pe_section_rva_end(&sections[i]) - pe_section_rva_start(&sections[i])));
    }
    return 0;
}

int main(void) {
    HMODULE self = GetModuleHandleW(NULL);
    const IMAGE_SECTION_HEADER *text;
    const IMAGE_SECTION_HEADER *data;
    const IMAGE_SECTION_HEADER *rdata;
    size_t size = 0;

    EXPECT_NOT_NULL(self);

    text = pe_section_by_name(self, ".text");
    EXPECT_NOT_NULL(text);
    EXPECT_NOT_NULL(pe_section_data(self, text, &size));
    EXPECT_TRUE(size > 0);

    data = pe_section_by_name(self, ".data");
    EXPECT_NOT_NULL(data);
    EXPECT_NOT_NULL(pe_section_data(self, data, &size));
    EXPECT_TRUE(size > 0);

    rdata = pe_section_by_name(self, ".rdata");
    EXPECT_NOT_NULL(rdata);
    EXPECT_NOT_NULL(pe_section_data(self, rdata, &size));
    EXPECT_TRUE(size > 0);

    EXPECT_NULL(pe_section_by_name(self, ".missing"));

    EXPECT_EQ(test_module(self), 0);
    EXPECT_EQ(test_fake_image(), 0);

    printf("smoke_pe_sections: all tests passed\n");
    return 0;
}
