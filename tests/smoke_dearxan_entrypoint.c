#include <stdio.h>
#include <string.h>

#include "test_common.h"
#include "dearxan/entry_point.h"

int main(void) {
    unsigned char bytes[128] = { 0 };
    dearxan_image_t image = { bytes, sizeof(bytes), 0x10000000, 0x10000000, 0 };
    dearxan_msvc_entrypoint_t entrypoint;
    int32_t displacement;

    bytes[0] = 0x48; bytes[1] = 0x83; bytes[2] = 0xec; bytes[3] = 0x28;
    bytes[4] = 0xe8;
    displacement = (int32_t)((image.base_va + 64) - (image.base_va + 9));
    memcpy(bytes + 5, &displacement, sizeof(displacement));
    bytes[9] = 0x48; bytes[10] = 0x83; bytes[11] = 0xc4; bytes[12] = 0x28;
    bytes[13] = 0xe9;
    displacement = (int32_t)((image.base_va + 96) - (image.base_va + 18));
    memcpy(bytes + 14, &displacement, sizeof(displacement));

    EXPECT_TRUE(dearxan_parse_msvc_entrypoint(&image, image.base_va, &entrypoint));
    EXPECT_EQ(entrypoint.security_init_cookie_va, image.base_va + 64);
    EXPECT_EQ(entrypoint.scrt_common_main_seh_va, image.base_va + 96);
    EXPECT_TRUE(entrypoint.security_init_cookie_call == bytes + 4);
    printf("smoke_dearxan_entrypoint: all tests passed\n");
    return 0;
}
