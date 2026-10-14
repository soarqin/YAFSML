#include <stdio.h>
#include <string.h>

#include "test_common.h"
#include "dearxan/entry_point.h"

static void make_entrypoint(unsigned char bytes[128], const dearxan_image_t *image,
                             size_t offset) {
    int32_t displacement;
    memset(bytes, 0, 128);
    bytes += offset;
    bytes[0] = 0x48; bytes[1] = 0x83; bytes[2] = 0xec; bytes[3] = 0x28;
    bytes[4] = 0xe8;
    displacement = (int32_t)((image->base_va + 64) - (image->base_va + offset + 9));
    memcpy(bytes + 5, &displacement, sizeof(displacement));
    bytes[9] = 0x48; bytes[10] = 0x83; bytes[11] = 0xc4; bytes[12] = 0x28;
    bytes[13] = 0xe9;
    displacement = (int32_t)((image->base_va + 96) - (image->base_va + offset + 18));
    memcpy(bytes + 14, &displacement, sizeof(displacement));
}

static int expect_entrypoint(dearxan_image_t *image, size_t call_offset) {
    dearxan_msvc_entrypoint_t entrypoint;
    EXPECT_TRUE(dearxan_parse_msvc_entrypoint(image, image->base_va, &entrypoint));
    EXPECT_EQ(entrypoint.security_init_cookie_va, image->base_va + 64);
    EXPECT_EQ(entrypoint.scrt_common_main_seh_va, image->base_va + 96);
    EXPECT_TRUE(entrypoint.security_init_cookie_call == image->base + call_offset);
    EXPECT_TRUE(!entrypoint.is_arxan_hooked);
    return 0;
}

int main(void) {
    unsigned char bytes[128] = { 0 };
    dearxan_image_t image = { bytes, sizeof(bytes), 0x10000000, 0x10000000, 0 };
    dearxan_msvc_entrypoint_t entrypoint;
    static const unsigned char traps[][3] = {
        { 0xcc },              /* INT3 */
        { 0x0f, 0x0b },        /* UD2 */
        { 0xcd, 0x03 },        /* INT 3 */
        { 0xf1 },              /* INT1 */
        { 0x0f, 0xb9, 0xc0 },  /* UD1 EAX, EAX */
        { 0x0f, 0xff, 0xc0 },  /* UD0 EAX, EAX */
    };
    static const size_t trap_sizes[] = { 1, 2, 2, 1, 3, 3 };

    make_entrypoint(bytes, &image, 0);
    EXPECT_EQ(expect_entrypoint(&image, 4), 0);
    for (size_t i = 0; i < sizeof(traps) / sizeof(traps[0]); i++) {
        make_entrypoint(bytes, &image, 0);
        memcpy(bytes, traps[i], trap_sizes[i]);
        EXPECT_EQ(expect_entrypoint(&image, 4), 0);
        make_entrypoint(bytes, &image, trap_sizes[i]);
        memcpy(bytes, traps[i], trap_sizes[i]);
        EXPECT_EQ(expect_entrypoint(&image, trap_sizes[i] + 4), 0);
    }

    /* The remainder of the MSVC prologue must still match exactly. */
    make_entrypoint(bytes, &image, 0);
    bytes[0] = 0x90; /* An ordinary mismatch is not a debugger trap. */
    EXPECT_TRUE(!dearxan_parse_msvc_entrypoint(&image, image.base_va, &entrypoint));
    make_entrypoint(bytes, &image, 0);
    bytes[2] = 0xe8; /* SUB RAX instead of SUB RSP. */
    EXPECT_TRUE(!dearxan_parse_msvc_entrypoint(&image, image.base_va, &entrypoint));
    make_entrypoint(bytes, &image, 0);
    bytes[4] = 0xcc; /* A trap is only permitted at the first instruction. */
    EXPECT_TRUE(!dearxan_parse_msvc_entrypoint(&image, image.base_va, &entrypoint));
    make_entrypoint(bytes, &image, 0);
    bytes[0] = 0xcc;
    bytes[11] = 0xc0; /* ADD RAX instead of ADD RSP. */
    EXPECT_TRUE(!dearxan_parse_msvc_entrypoint(&image, image.base_va, &entrypoint));
    make_entrypoint(bytes, &image, 0);
    bytes[0] = 0xcc;
    image.size = 4; /* Skipping must not read past a truncated image. */
    memset(&entrypoint, 0xa5, sizeof(entrypoint));
    dearxan_msvc_entrypoint_t unchanged = entrypoint;
    EXPECT_TRUE(!dearxan_parse_msvc_entrypoint(&image, image.base_va, &entrypoint));
    EXPECT_EQ(memcmp(&entrypoint, &unchanged, sizeof(entrypoint)), 0);
    printf("smoke_dearxan_entrypoint: all tests passed\n");
    return 0;
}
