#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test_common.h"
#include "process/rip_scan.h"

static int test_utf16_candidates(void) {
    /* Only the last copy is complete; the earlier ones share the first byte,
       a prefix, or sit at an odd offset. */
    uint8_t data[96] = { 0 };
    const wchar_t *value = L"Step";
    const size_t value_size = (wcslen(value) + 1) * sizeof(wchar_t);

    data[0] = 'S';
    memcpy(data + 8, L"Ste", 3 * sizeof(wchar_t));
    data[14] = 'x';
    memcpy(data + 21, value, value_size);
    memcpy(data + 64, value, value_size);
    EXPECT_EQ(ml_find_utf16_string(data, sizeof(data), value), data + 21);
    EXPECT_EQ(ml_find_utf16_string(data + 22, sizeof(data) - 22, value), data + 64);
    /* The terminator must fit as well. */
    EXPECT_EQ(ml_find_utf16_string(data + 22, 64 - 22 + value_size, value), data + 64);
    EXPECT_NULL(ml_find_utf16_string(data + 22, 64 - 22 + value_size - 1, value));
    EXPECT_NULL(ml_find_utf16_string(data, sizeof(data), L"Steps"));
    EXPECT_NULL(ml_find_utf16_string(NULL, sizeof(data), value));
    EXPECT_NULL(ml_find_utf16_string(data, sizeof(data), NULL));
    /* An empty string is just its terminator. */
    EXPECT_EQ(ml_find_utf16_string(data + 1, sizeof(data) - 1, L""), data + 1);
    return 0;
}

static int test_rel32_target(void) {
    uint8_t code[16] = { 0 };
    const int32_t forward = 0x1234;
    const int32_t backward = -0x40;

    /* Unaligned displacement, positive and negative. */
    memcpy(code + 3, &forward, sizeof(forward));
    EXPECT_EQ(ml_rel32_target(code + 3, (uintptr_t)(code + 7)), (uintptr_t)(code + 7) + 0x1234);
    memcpy(code + 3, &backward, sizeof(backward));
    EXPECT_EQ(ml_rel32_target(code + 3, (uintptr_t)(code + 7)), (uintptr_t)(code + 7) - 0x40);
    /* The end of the instruction need not be the buffer address. */
    EXPECT_EQ(ml_rel32_target(code + 3, (uintptr_t)0x140001007), (uintptr_t)0x140000FC7);
    return 0;
}

int main(void) {
    uint8_t image[128] = { 0 };
    const wchar_t *property = L"Game.Debug.NearOnlyDraw";
    uint8_t *text = image;
    uint8_t *rdata = image + 64;
    uint8_t *instruction = text + 8;
    int64_t displacement;

    memcpy(rdata, property, (wcslen(property) + 1) * sizeof(wchar_t));
    instruction[0] = 0x48;
    instruction[1] = 0x8d;
    instruction[2] = 0x0d;
    displacement = (int64_t)(rdata - (instruction + 7));
    EXPECT_TRUE(displacement >= INT32_MIN && displacement <= INT32_MAX);
    {
        int32_t relative = (int32_t)displacement;
        memcpy(instruction + 3, &relative, sizeof(relative));
    }

    EXPECT_EQ(ml_find_utf16_string(rdata, 64, property), rdata);
    EXPECT_EQ(ml_find_rip_relative_lea(text, 64, rdata), instruction);
    EXPECT_EQ((const uint8_t *)ml_rel32_target(instruction + 3, (uintptr_t)(instruction + 7)), rdata);
    instruction[1] = 0x8b;
    EXPECT_EQ(ml_find_rip_relative_lea(text, 64, rdata), instruction);
    EXPECT_NULL(ml_find_utf16_string(rdata, 16, property));
    EXPECT_NULL(ml_find_rip_relative_lea(text, 7, rdata));
    EXPECT_EQ(test_utf16_candidates(), 0);
    EXPECT_EQ(test_rel32_target(), 0);
    printf("smoke_rip_scan: all tests passed\n");
    return 0;
}
