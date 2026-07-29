#include "test_common.h"

#include "modloader/patches/regulation_sig.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const uint8_t writer_near_branch[] = {
    0x48, 0x8b, 0xd1, 0x48, 0x8b, 0x0d, 0, 0, 0, 0,
    0x48, 0x85, 0xc9, 0x0f, 0x85, 0, 0, 0, 0,
    0x32, 0xc0, 0xc3
};

static const uint8_t writer_short_branch[] = {
    0x48, 0x8b, 0xd1, 0x48, 0x8b, 0x0d, 0, 0, 0, 0,
    0x48, 0x85, 0xc9, 0x75, 0,
    0x32, 0xc0, 0xc3
};

static void write_writer(uint8_t *text, size_t offset,
                         const uint8_t *writer, size_t writer_size) {
    memcpy(text + offset, writer, writer_size);
}

static void write_stack_call(uint8_t *text, size_t call_offset,
                             size_t target_offset) {
    int32_t displacement =
        (int32_t)((int64_t)target_offset - (int64_t)(call_offset + 10));
    memcpy(text + call_offset, "\x48\x8d\x4c\x24\x28\xe8", 6);
    memcpy(text + call_offset + 6, &displacement, sizeof(displacement));
}

static int expect_single_writer(uint8_t *text, size_t text_size,
                                uint8_t *expected) {
    uint8_t *writers[2] = { 0 };
    EXPECT_EQ(ml_regulation_sprj_find_writers(
                  text, text_size, writers, 2),
              1);
    EXPECT_TRUE(writers[0] == expected);
    return 0;
}

static int test_sekiro_106_post_call(void) {
    uint8_t text[256] = { 0 };
    write_stack_call(text, 32, 160);
    memcpy(text + 42, "\x0f\xb6\xe8", 3); /* movzx ebp, al */
    write_writer(text, 160, writer_near_branch,
                 sizeof(writer_near_branch));
    return expect_single_writer(text, sizeof(text), text + 160);
}

static int test_legacy_post_call(void) {
    uint8_t text[256] = { 0 };
    write_stack_call(text, 32, 160);
    memcpy(text + 42, "\x84\xc0\x74\x05", 4); /* test al, al; je */
    write_writer(text, 160, writer_short_branch,
                 sizeof(writer_short_branch));
    return expect_single_writer(text, sizeof(text), text + 160);
}

static int test_unique_targets_and_decoys(void) {
    uint8_t text[320] = { 0 };
    uint8_t *writers[4] = { 0 };

    write_stack_call(text, 24, 240);
    write_stack_call(text, 64, 240);
    write_writer(text, 240, writer_near_branch,
                 sizeof(writer_near_branch));

    write_stack_call(text, 104, 280);
    memcpy(text + 280, writer_near_branch, sizeof(writer_near_branch));
    text[280] = 0x90;

    EXPECT_EQ(ml_regulation_sprj_find_writers(
                  text, sizeof(text), writers, 4),
              1);
    EXPECT_TRUE(writers[0] == text + 240);
    return 0;
}

static int test_requires_stack_call_and_in_range_target(void) {
    uint8_t text[256] = { 0 };
    uint8_t *writers[2] = { 0 };
    int32_t out_of_range = INT32_MAX;

    write_writer(text, 160, writer_near_branch,
                 sizeof(writer_near_branch));
    EXPECT_EQ(ml_regulation_sprj_find_writers(
                  text, sizeof(text), writers, 2),
              0);

    memcpy(text + 32, "\x48\x8d\x4c\x24\x28\xe8", 6);
    memcpy(text + 38, &out_of_range, sizeof(out_of_range));
    EXPECT_EQ(ml_regulation_sprj_find_writers(
                  text, sizeof(text), writers, 2),
              0);
    return 0;
}

int main(void) {
    EXPECT_EQ(test_sekiro_106_post_call(), 0);
    EXPECT_EQ(test_legacy_post_call(), 0);
    EXPECT_EQ(test_unique_targets_and_decoys(), 0);
    EXPECT_EQ(test_requires_stack_call_and_in_range_target(), 0);
    printf("smoke_regulation_sig: all tests passed\n");
    return 0;
}
