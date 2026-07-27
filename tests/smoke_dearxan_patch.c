#include <stdint.h>
#include <stdio.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "test_common.h"
#include "dearxan/image.h"
#include "dearxan/patch.h"
#include "fixtures/dearxan/synthetic_pe.h"

int main(void) {
    unsigned char *memory = VirtualAlloc(NULL, 0x1000, MEM_COMMIT | MEM_RESERVE,
                                         PAGE_EXECUTE_READWRITE);
    dearxan_image_t image;
    dearxan_stub_list_t stubs = { 0 };
    const char *error = "unchanged";

    EXPECT_NOT_NULL(memory);
    {
        IMAGE_NT_HEADERS64 *nt = dearxan_fixture_init_pe(
            memory, 0x1000, (uint64_t)(uintptr_t)memory, 0x200, 0);
        (void)nt;
    }
    EXPECT_TRUE(dearxan_image_from_module(memory, &image));
    EXPECT_TRUE(dearxan_apply_stub_patches(&image, &stubs, &error));
    EXPECT_NULL(error);
    VirtualFree(memory, 0, MEM_RELEASE);
    printf("smoke_dearxan_patch: all tests passed\n");
    return 0;
}
