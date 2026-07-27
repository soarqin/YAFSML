#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "test_common.h"
#include "dearxan/encryption.h"
#include "dearxan/steamstub.h"
#include "fixtures/dearxan/synthetic_pe.h"

static int test_block_decryptors(void) {
    struct tea_vector {
        uint32_t block[2];
        uint32_t key[4];
        uint32_t expected[2];
    } tea_vectors[] = {
        { { 0x41EA3A0Au, 0x94BAA940u }, { 0, 0, 0, 0 }, { 0, 0 } },
        { { 0xDEADBEEFu, 0x01234567u },
          { 0x00112233u, 0x44556677u, 0x8899AABBu, 0xCCDDEEFFu },
          { 0x39E731A6u, 0xC98C6CDAu } },
        { { 0, 0 },
          { UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX },
          { 0xE34BF71Au, 0xAD249764u } }
    };
    const uint32_t rmx_ciphertext[] = {
        0, 0xDEADBEEFu, 0x01234567u, UINT32_MAX
    };
    const uint32_t rmx_plaintext[] = {
        0xBE4B17F0u, 0x93C15D99u, 0x225A6A6Fu, 0x114AF10Fu
    };
    const uint32_t rmx_keys[] = {
        0x78123456u, 0x2B3C091Au, 0x12345678u, 0x2468ACF0u
    };
    const uint32_t rmx_rotations[] = {
        0x41B4E817u, 0x2D8A4A71u, 0xF02FDFE1u, 0x1E9AD111u
    };
    uint32_t rmx_key = 0x12345678u;
    uint32_t rmx_rotation = rmx_key & 0x1fu;

    for (size_t i = 0; i < sizeof(tea_vectors) / sizeof(tea_vectors[0]); i++) {
        dearxan_tea_decrypt(tea_vectors[i].block, tea_vectors[i].key);
        EXPECT_EQ(tea_vectors[i].block[0], tea_vectors[i].expected[0]);
        EXPECT_EQ(tea_vectors[i].block[1], tea_vectors[i].expected[1]);
    }
    for (size_t i = 0; i < sizeof(rmx_ciphertext) / sizeof(rmx_ciphertext[0]); i++) {
        EXPECT_EQ(dearxan_rmx_decrypt(rmx_ciphertext[i], &rmx_key, &rmx_rotation),
                  rmx_plaintext[i]);
        EXPECT_EQ(rmx_key, rmx_keys[i]);
        EXPECT_EQ(rmx_rotation, rmx_rotations[i]);
    }
    EXPECT_EQ(dearxan_sub_decrypt(0x89ABCDEFu, 0x12345678u), 0x88888889u);
    EXPECT_EQ(dearxan_sub_decrypt(0, 0), 0);
    EXPECT_EQ(dearxan_sub_decrypt(UINT32_MAX, 0), 1);
    return 0;
}

static int test_varints(void) {
    struct varint_vector {
        unsigned char bytes[5];
        size_t size;
        uint32_t expected;
    } vectors[] = {
        { { 0x00 }, 1, 0 },
        { { 0x7f }, 1, 127 },
        { { 0x80, 0x01 }, 2, 128 },
        { { 0xac, 0x02 }, 2, 300 },
        { { 0xff, 0xff, 0xff, 0xff, 0x0f }, 5, UINT32_MAX },
        { { 0x80, 0x00 }, 2, 0 }
    };
    const unsigned char truncated[] = { 0x80 };
    const unsigned char overflow[] = { 0xff, 0xff, 0xff, 0xff, 0x10 };
    const unsigned char too_long[] = { 0x80, 0x80, 0x80, 0x80, 0x80, 0x00 };
    size_t consumed = 99;
    uint32_t value = 99;

    for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
        consumed = 0;
        value = 0;
        EXPECT_TRUE(dearxan_read_varint(vectors[i].bytes, vectors[i].size,
                                        &consumed, &value));
        EXPECT_EQ(consumed, vectors[i].size);
        EXPECT_EQ(value, vectors[i].expected);
    }
    EXPECT_TRUE(!dearxan_read_varint(truncated, sizeof(truncated), &consumed, &value));
    EXPECT_TRUE(!dearxan_read_varint(overflow, sizeof(overflow), &consumed, &value));
    EXPECT_TRUE(!dearxan_read_varint(too_long, sizeof(too_long), &consumed, &value));
    EXPECT_TRUE(!dearxan_read_varint(NULL, 0, &consumed, &value));
    EXPECT_TRUE(!dearxan_read_varint(truncated, sizeof(truncated), NULL, &value));
    EXPECT_TRUE(!dearxan_read_varint(truncated, sizeof(truncated), &consumed, NULL));
    return 0;
}

static int test_region_varints(void) {
    const unsigned char encoded[] = {
        0x10, 0x04, 0x7f, 0x02, 0xea, 0xfe, 0xff, 0xff, 0x0f
    };
    const unsigned char zero_delta[] = { 0x00 };
    const unsigned char zero_size[] = { 0x01, 0x00 };
    const unsigned char missing_terminator[] = { 0x01, 0x01 };
    dearxan_encrypted_region_t *regions = NULL;
    size_t count = 0;
    size_t stream_size = 0;

    EXPECT_TRUE(dearxan_parse_encrypted_regions(encoded, sizeof(encoded),
                                                &regions, &count, &stream_size));
    EXPECT_EQ(count, 2);
    EXPECT_EQ(regions[0].stream_offset, 0);
    EXPECT_EQ(regions[0].rva, 0x10);
    EXPECT_EQ(regions[0].size, 4);
    EXPECT_EQ(regions[1].stream_offset, 4);
    EXPECT_EQ(regions[1].rva, 0x93);
    EXPECT_EQ(regions[1].size, 2);
    EXPECT_EQ(stream_size, 6);
    free(regions);

    EXPECT_TRUE(!dearxan_parse_encrypted_regions(zero_delta, sizeof(zero_delta),
                                                 &regions, &count, &stream_size));
    EXPECT_TRUE(!dearxan_parse_encrypted_regions(zero_size, sizeof(zero_size),
                                                 &regions, &count, &stream_size));
    EXPECT_TRUE(!dearxan_parse_encrypted_regions(missing_terminator,
                                                 sizeof(missing_terminator),
                                                 &regions, &count, &stream_size));
    return 0;
}

static int test_entropy(void) {
    const unsigned char constant[16] = { 0 };
    const unsigned char balanced[] = { 0, 1, 0, 1 };
    const unsigned char skewed[] = { 0, 0, 0, 1 };
    unsigned char uniform[256];

    for (size_t i = 0; i < sizeof(uniform); i++) uniform[i] = (unsigned char)i;
    EXPECT_TRUE(isnan(dearxan_shannon_entropy(NULL, 0)));
    EXPECT_TRUE(fabs(dearxan_shannon_entropy(constant, sizeof(constant))) < 1e-12);
    EXPECT_TRUE(fabs(dearxan_shannon_entropy(balanced, sizeof(balanced)) - 1.0) < 1e-12);
    EXPECT_TRUE(fabs(dearxan_shannon_entropy(skewed, sizeof(skewed)) -
                     0.8112781244591328) < 1e-12);
    EXPECT_TRUE(fabs(dearxan_shannon_entropy(uniform, sizeof(uniform)) - 8.0) < 1e-12);
    return 0;
}

static int test_region_list_resolution(void) {
    unsigned char image_bytes[0x1000];
    unsigned char low_entropy[16] = { 0 };
    unsigned char high_entropy[16];
    IMAGE_NT_HEADERS64 *nt = dearxan_fixture_init_pe(
        image_bytes, sizeof(image_bytes), UINT64_C(0x140000000), 0x200, 1);
    IMAGE_SECTION_HEADER *section = dearxan_fixture_sections(nt);
    dearxan_image_t image;
    dearxan_encrypted_region_t low_region = { 0, 16, 0x300 };
    dearxan_encrypted_region_t high_region = { 0, 16, 0x308 };
    dearxan_encrypted_region_list_t low = {
        DEARXAN_DECRYPTION_TEA, &low_region, 1, low_entropy, sizeof(low_entropy)
    };
    dearxan_encrypted_region_list_t high = {
        DEARXAN_DECRYPTION_RMX, &high_region, 1, high_entropy, sizeof(high_entropy)
    };
    const dearxan_encrypted_region_list_t *lists[] = { &high, &low };
    dearxan_encrypted_region_list_t *resolved = NULL;
    size_t resolved_count = 99;

    section->VirtualAddress = 0x200;
    section->Misc.VirtualSize = 0x700;
    for (size_t i = 0; i < 24; i++) image_bytes[0x300 + i] = (unsigned char)i;
    for (size_t i = 0; i < sizeof(high_entropy); i++) high_entropy[i] = (unsigned char)i;
    EXPECT_TRUE(dearxan_image_from_module(image_bytes, &image));
    EXPECT_TRUE(dearxan_resolve_encrypted_region_lists(
        &image, lists, sizeof(lists) / sizeof(lists[0]),
        &resolved, &resolved_count));
    EXPECT_EQ(resolved_count, 1);
    EXPECT_EQ(resolved[0].kind, DEARXAN_DECRYPTION_TEA);
    EXPECT_EQ(resolved[0].regions[0].rva, low_region.rva);
    EXPECT_TRUE(memcmp(resolved[0].decrypted_stream, low_entropy,
                       sizeof(low_entropy)) == 0);
    dearxan_free_encrypted_region_lists(resolved, resolved_count);

    resolved = (dearxan_encrypted_region_list_t *)(uintptr_t)1;
    resolved_count = 99;
    EXPECT_TRUE(dearxan_resolve_encrypted_region_lists(
        &image, NULL, 0, &resolved, &resolved_count));
    EXPECT_NULL(resolved);
    EXPECT_EQ(resolved_count, 0);
    return 0;
}

static int test_steamstub_hash(void) {
    unsigned char all_bytes[256];
    const unsigned char binary[] = { 0x00, 0xff, 0x55, 0xaa };

    for (size_t i = 0; i < sizeof(all_bytes); i++) all_bytes[i] = (unsigned char)i;
    EXPECT_EQ(dearxan_steamstub_hash(NULL, 0, 0), 0);
    EXPECT_EQ(dearxan_steamstub_hash((const unsigned char *)"123456789", 9, 0),
              0x4A90163Fu);
    EXPECT_EQ(dearxan_steamstub_hash(all_bytes, sizeof(all_bytes), 0), 0xAA8FFB50u);
    EXPECT_EQ(dearxan_steamstub_hash((const unsigned char *)"DearXan v0.5.3", 14,
                                    0x89ABCDEFu), 0x568FD941u);
    EXPECT_EQ(dearxan_steamstub_hash(binary, sizeof(binary), UINT32_MAX), 0x50B718B9u);
    return 0;
}

int main(void) {
    if (test_block_decryptors() != 0 || test_varints() != 0 ||
        test_region_varints() != 0 || test_entropy() != 0 ||
        test_region_list_resolution() != 0 || test_steamstub_hash() != 0) return 1;
    printf("smoke_dearxan_crypto: all tests passed\n");
    return 0;
}
