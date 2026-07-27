#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "test_common.h"
#include "dearxan/steamstub.h"

static uint32_t encrypt_header_reference(dearxan_steamstub_header_t *header) {
    unsigned char *bytes = (unsigned char *)header;
    uint32_t key = 0;

    for (size_t i = 0; i < sizeof(*header); i += sizeof(uint32_t)) {
        uint32_t block;
        memcpy(&block, bytes + i, sizeof(block));
        block ^= key;
        memcpy(bytes + i, &block, sizeof(block));
        key = block;
    }
    return key;
}

static int test_header_round_trip(void) {
    dearxan_steamstub_header_t header;
    unsigned char expected[sizeof(header)];
    const unsigned char first_ciphertext[] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x04, 0x04, 0x04,
        0x0c, 0x0d, 0x0e, 0x0f, 0x00, 0x00, 0x00, 0x00
    };
    const unsigned char last_ciphertext[] = {
        0xe0, 0xe1, 0xe2, 0xe3, 0x04, 0x04, 0x04, 0x04,
        0xec, 0xed, 0xee, 0xef, 0x00, 0x00, 0x00, 0x00
    };

    for (size_t i = 0; i < sizeof(expected); i++) expected[i] = (unsigned char)i;
    memcpy(&header, expected, sizeof(header));
    EXPECT_EQ(encrypt_header_reference(&header), 0);
    EXPECT_TRUE(memcmp(&header, first_ciphertext, sizeof(first_ciphertext)) == 0);
    EXPECT_TRUE(memcmp((unsigned char *)&header + sizeof(header) - sizeof(last_ciphertext),
                       last_ciphertext, sizeof(last_ciphertext)) == 0);
    EXPECT_EQ(dearxan_steamstub_decrypt_header(&header), 0);
    EXPECT_TRUE(memcmp(&header, expected, sizeof(expected)) == 0);
    return 0;
}

static int test_string_round_trip(void) {
    const unsigned char plaintext[] = "DearXan-Strings!";
    const unsigned char ciphertext[] = {
        0xab, 0xa8, 0xca, 0xfb, 0xf3, 0xc9, 0xa4, 0xd6,
        0xa0, 0xbd, 0xd6, 0xbf, 0xce, 0xda, 0xa5, 0x9e
    };
    unsigned char bytes[sizeof(ciphertext)];
    uint32_t key = 0x89ABCDEFu;

    memcpy(bytes, ciphertext, sizeof(bytes));
    dearxan_steamstub_crypt_strings(bytes, sizeof(bytes), &key);
    EXPECT_EQ(key, 0x9EA5DACEu);
    EXPECT_TRUE(memcmp(bytes, plaintext, sizeof(bytes)) == 0);
    return 0;
}

static void encrypt_strings_reference(unsigned char *bytes, size_t size,
                                      uint32_t *key) {
    for (size_t i = 0; i + sizeof(uint32_t) <= size; i += sizeof(uint32_t)) {
        uint32_t block;
        memcpy(&block, bytes + i, sizeof(block));
        block ^= *key;
        memcpy(bytes + i, &block, sizeof(block));
        *key = block;
    }
}

static int test_detect_and_patch_round_trip(void) {
    enum {
        IMAGE_SIZE = 0x1000,
        ENTRYPOINT_RVA = 0x400,
        HEADER_RVA = ENTRYPOINT_RVA - sizeof(dearxan_steamstub_header_t),
        STRINGS_RVA = 0x500,
        DRMP_RVA = 0x520
    };
    unsigned char *bytes = VirtualAlloc(NULL, IMAGE_SIZE, MEM_COMMIT | MEM_RESERVE,
                                        PAGE_EXECUTE_READWRITE);
    dearxan_image_t image;
    dearxan_steamstub_header_t plaintext = { 0 };
    dearxan_steamstub_header_t encrypted;
    dearxan_steamstub_header_t decrypted;
    dearxan_steamstub_context_t context;
    unsigned char strings[16] = "kernel32.dll";
    unsigned char encrypted_strings[sizeof(strings)];
    unsigned char drmp[8] = { 0 };
    uint32_t string_key;
    uint32_t expected_hash = 0;
    const uint64_t replacement = (uint64_t)(uintptr_t)bytes + 0x700;

    EXPECT_NOT_NULL(bytes);
    memset(bytes, 0, IMAGE_SIZE);
    image = (dearxan_image_t){ bytes, IMAGE_SIZE, (uint64_t)(uintptr_t)bytes,
                              UINT64_C(0x140000000), ENTRYPOINT_RVA };
    plaintext.xor_key = 0x10203040u;
    plaintext.signature = 0xC0DEC0DFu;
    plaintext.bind_section_ep_offset = 0;
    plaintext.steamstub_ep_code_size = 17;
    plaintext.original_entry_point = 0x600;
    plaintext.strings_bind_offset = STRINGS_RVA - ENTRYPOINT_RVA;
    plaintext.strings_data_size = 13;
    plaintext.drmp_dll_bind_offset = DRMP_RVA - ENTRYPOINT_RVA;
    plaintext.drmp_dll_size = sizeof(drmp);
    plaintext.drmp_xtea_key[0] = 0x00112233u;
    plaintext.drmp_xtea_key[1] = 0x44556677u;
    plaintext.drmp_xtea_key[2] = 0x8899AABBu;
    plaintext.drmp_xtea_key[3] = 0xCCDDEEFFu;
    encrypted = plaintext;
    string_key = encrypt_header_reference(&encrypted);
    memcpy(bytes + HEADER_RVA, &encrypted, sizeof(encrypted));
    memcpy(encrypted_strings, strings, sizeof(strings));
    encrypt_strings_reference(encrypted_strings, sizeof(encrypted_strings), &string_key);
    memcpy(bytes + STRINGS_RVA, encrypted_strings, sizeof(encrypted_strings));
    memcpy(bytes + DRMP_RVA, drmp, sizeof(drmp));
    for (size_t i = 0; i < 32; i++) bytes[ENTRYPOINT_RVA + i] = (unsigned char)(0xa0 + i);

    EXPECT_TRUE(dearxan_steamstub_detect(&image, &context));
    EXPECT_EQ(context.strings_size, 16);
    EXPECT_EQ(context.drmp_size, sizeof(drmp));
    EXPECT_TRUE(memcmp(context.decrypted_strings, strings, sizeof(strings)) == 0);
    EXPECT_TRUE(dearxan_steamstub_patch(&image, &context, replacement));

    decrypted = *(dearxan_steamstub_header_t *)(bytes + HEADER_RVA);
    string_key = dearxan_steamstub_decrypt_header(&decrypted);
    EXPECT_EQ(decrypted.original_entry_point, 0x700);
    EXPECT_EQ(decrypted.drm_flags & 0x32u, 0x32u);
    expected_hash = dearxan_steamstub_hash(strings, sizeof(strings), expected_hash);
    {
        dearxan_steamstub_header_t hash_header = decrypted;
        hash_header.integrity_hash = 0;
        expected_hash = dearxan_steamstub_hash((unsigned char *)&hash_header,
                                              sizeof(hash_header), expected_hash);
    }
    expected_hash = dearxan_steamstub_hash(bytes + ENTRYPOINT_RVA, 32, expected_hash);
    expected_hash = dearxan_steamstub_hash(context.decrypted_drmp,
                                          context.drmp_size, expected_hash);
    EXPECT_EQ(decrypted.integrity_hash, expected_hash);
    memcpy(encrypted_strings, bytes + STRINGS_RVA, sizeof(encrypted_strings));
    dearxan_steamstub_crypt_strings(encrypted_strings, sizeof(encrypted_strings), &string_key);
    EXPECT_TRUE(memcmp(encrypted_strings, strings, sizeof(strings)) == 0);
    dearxan_steamstub_uninit(&context);
    VirtualFree(bytes, 0, MEM_RELEASE);
    return 0;
}

static int test_drmp_vector(void) {
    const uint32_t key[4] = {
        0x00112233u, 0x44556677u, 0x8899AABBu, 0xCCDDEEFFu
    };
    const unsigned char ciphertext[] = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
        0x10, 0x20, 0x30
    };
    const unsigned char plaintext[] = {
        0x98, 0x02, 0x6b, 0x55, 0x86, 0x01, 0xf6, 0x43,
        0x1a, 0xf3, 0xb6, 0x21, 0xe9, 0x26, 0x4b, 0x4d,
        0x10, 0x20, 0x30
    };
    unsigned char bytes[sizeof(ciphertext)];

    memcpy(bytes, ciphertext, sizeof(bytes));
    dearxan_steamstub_decrypt_drmp(bytes, sizeof(bytes), key);
    EXPECT_TRUE(memcmp(bytes, plaintext, sizeof(bytes)) == 0);
    return 0;
}

int main(void) {
    if (test_header_round_trip() != 0 || test_string_round_trip() != 0 ||
        test_detect_and_patch_round_trip() != 0 || test_drmp_vector() != 0) return 1;
    printf("smoke_dearxan_steamstub: all tests passed\n");
    return 0;
}
