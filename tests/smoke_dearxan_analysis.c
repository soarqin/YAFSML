#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test_common.h"
#include "dearxan/analysis.h"
#include "dearxan/image.h"
#include "fixtures/dearxan/synthetic_pe.h"

static int init_analysis_image(unsigned char *bytes, size_t size,
                               dearxan_image_t *image) {
    IMAGE_NT_HEADERS64 *nt = dearxan_fixture_init_pe(
        bytes, size, UINT64_C(0x140000000), 0x200, 1);
    IMAGE_SECTION_HEADER *section = dearxan_fixture_sections(nt);
    section->VirtualAddress = 0x200;
    section->Misc.VirtualSize = (DWORD)(size - 0x200);
    EXPECT_TRUE(dearxan_image_from_module(bytes, image));
    return 0;
}

static int test_no_candidates(void) {
    unsigned char bytes[0x1000];
    dearxan_image_t image;
    dearxan_stub_list_t stubs = { (dearxan_stub_info_t *)(uintptr_t)1, 99 };
    const char *error = "unchanged";

    if (init_analysis_image(bytes, sizeof(bytes), &image) != 0) return 1;
    EXPECT_TRUE(dearxan_analyze_all_stubs(&image, &stubs, &error));
    EXPECT_EQ(stubs.count, 0);
    EXPECT_NULL(stubs.items);
    EXPECT_NULL(error);
    return 0;
}

static int test_false_positive_is_rejected(void) {
    unsigned char bytes[0x1000];
    dearxan_image_t image;
    dearxan_stub_list_t stubs = { 0 };
    const char *error = NULL;
    const unsigned char false_positive[] = {
        0x48, 0xf7, 0xc4, 0x0f, 0x00, 0x00, 0x00, 0xc3
    };

    if (init_analysis_image(bytes, sizeof(bytes), &image) != 0) return 1;
    memset(bytes + 0x200, 0xcc, sizeof(bytes) - 0x200);
    memcpy(bytes + 0x240, false_positive, sizeof(false_positive));
    EXPECT_TRUE(dearxan_analyze_all_stubs(&image, &stubs, &error));
    EXPECT_NULL(error);
    EXPECT_EQ(stubs.count, 0);
    dearxan_free_stub_list(&stubs);
    EXPECT_NULL(stubs.items);
    return 0;
}

static int test_context_pop_and_return_gadget(void) {
    unsigned char bytes[0x1000];
    dearxan_image_t image;
    dearxan_stub_list_t stubs = { 0 };
    const char *error = NULL;
    const unsigned char stub[] = {
        0x48, 0xf7, 0xc4, 0x0f, 0x00, 0x00, 0x00,
        0x48, 0xb8, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11,
        0x48, 0x89, 0x44, 0x24, 0x20,
        0xe8, 0x05, 0x00, 0x00, 0x00,
        0x48, 0x03, 0x64, 0x24, 0x08,
        0xc3
    };
    const uint64_t base_offset = 0x280;

    if (init_analysis_image(bytes, sizeof(bytes), &image) != 0) return 1;
    memset(bytes + 0x200, 0xcc, sizeof(bytes) - 0x200);
    memcpy(bytes + base_offset, stub, sizeof(stub));
    EXPECT_TRUE(dearxan_analyze_all_stubs(&image, &stubs, &error));
    EXPECT_NULL(error);
    EXPECT_EQ(stubs.count, 1);
    EXPECT_EQ(stubs.items[0].test_rsp_va, image.base_va + base_offset);
    EXPECT_EQ(stubs.items[0].context_pop_va, image.base_va + base_offset + 27);
    EXPECT_TRUE(stubs.items[0].has_return_gadget);
    EXPECT_EQ(stubs.items[0].return_gadget.stack_offset, 0x20);
    EXPECT_EQ(stubs.items[0].return_gadget.address,
              UINT64_C(0x1122334455667788));
    dearxan_free_stub_list(&stubs);
    EXPECT_EQ(stubs.count, 0);
    EXPECT_NULL(stubs.items);
    return 0;
}

int main(void) {
    if (test_no_candidates() != 0 || test_false_positive_is_rejected() != 0 ||
        test_context_pop_and_return_gadget() != 0) return 1;
    printf("smoke_dearxan_analysis: all tests passed\n");
    return 0;
}
