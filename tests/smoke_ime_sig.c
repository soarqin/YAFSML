#include "test_common.h"

#include "modloader/patches/ime_sig.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const uint8_t character_filter[] = {
    0x66, 0x3b, 0x10, 0x74, 0x13, 0xff, 0xc1, 0x48,
    0x83, 0xc0, 0x02, 0x83, 0xf9, 0x60, 0x72, 0xf0,
    0xba, 0x2a, 0x00, 0x00, 0x00, 0x40, 0xb7, 0x01,
    0x66, 0x89, 0x54, 0x24, 0x20,
};

static int test_finds_unique_filter(void) {
    uint8_t text[96] = { 0 };
    memcpy(text + 16, character_filter, sizeof(character_filter));
    EXPECT_TRUE(ml_ime_sig_find_character_filter(text, sizeof(text)) == text + 16 + 3);
    return 0;
}

static int test_rejects_missing_and_truncated_filter(void) {
    uint8_t text[96] = { 0 };
    memcpy(text + 16, character_filter, sizeof(character_filter) - 1);
    EXPECT_NULL(ml_ime_sig_find_character_filter(text, sizeof(text)));
    EXPECT_NULL(ml_ime_sig_find_character_filter(text + 16,
                                                 sizeof(character_filter) - 1));
    return 0;
}

static int test_rejects_ambiguous_filter(void) {
    uint8_t text[128] = { 0 };
    memcpy(text + 8, character_filter, sizeof(character_filter));
    memcpy(text + 72, character_filter, sizeof(character_filter));
    EXPECT_NULL(ml_ime_sig_find_character_filter(text, sizeof(text)));
    return 0;
}

int main(void) {
    EXPECT_EQ(test_finds_unique_filter(), 0);
    EXPECT_EQ(test_rejects_missing_and_truncated_filter(), 0);
    EXPECT_EQ(test_rejects_ambiguous_filter(), 0);
    printf("smoke_ime_sig: all tests passed\n");
    return 0;
}
