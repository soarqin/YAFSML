/*
 * C11 implementation of dearxan v0.5.3 functionality.
 *
 * Original project copyright (c) 2025 William Tremblay.
 * Licensed under the MIT license; see THIRD_PARTY_NOTICES.md.
 */

#include "steamstub.h"

#include <stdlib.h>
#include <string.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#define STEAMSTUB_SIGNATURE 0xC0DEC0DFu
#define STEAMSTUB_FLAG_NO_MODULE_VERIFICATION (1u << 1)
#define STEAMSTUB_FLAG_NO_OWNERSHIP_CHECK (1u << 4)
#define STEAMSTUB_FLAG_NO_DEBUGGER_CHECK (1u << 5)

_Static_assert(sizeof(dearxan_steamstub_header_t) == 240,
               "SteamStub 3.1 header ABI mismatch");

static size_t align16(size_t value) {
    if (value > SIZE_MAX - 15) return SIZE_MAX;
    return (value + 15) & ~(size_t)15;
}

uint32_t dearxan_steamstub_hash(const unsigned char *bytes, size_t size,
                                uint32_t hash) {
    for (size_t i = 0; i < size; i++) {
        hash ^= (uint32_t)bytes[i] << 24;
        for (unsigned int bit = 0; bit < 8; bit++) {
            hash = (hash & 0x80000000u) != 0
                ? (hash << 1) ^ 0x488781edu : hash << 1;
        }
    }
    return hash;
}

uint32_t dearxan_steamstub_decrypt_header(dearxan_steamstub_header_t *header) {
    uint32_t key = 0;
    uint32_t *blocks = (uint32_t *)header;
    for (size_t i = 0; i < sizeof(*header) / sizeof(*blocks); i++) {
        uint32_t next = blocks[i];
        blocks[i] ^= key;
        key = next;
    }
    return key;
}

static uint32_t steamstub_encrypt_header(dearxan_steamstub_header_t *header) {
    uint32_t key = 0;
    uint32_t *blocks = (uint32_t *)header;
    for (size_t i = 0; i < sizeof(*header) / sizeof(*blocks); i++) {
        blocks[i] ^= key;
        key = blocks[i];
    }
    return key;
}

void dearxan_steamstub_crypt_strings(unsigned char *bytes, size_t size,
                                     uint32_t *key) {
    for (size_t i = 0; i + 4 <= size; i += 4) {
        uint32_t block;
        uint32_t next;
        memcpy(&block, bytes + i, sizeof(block));
        next = block;
        block ^= *key;
        memcpy(bytes + i, &block, sizeof(block));
        *key = next;
    }
}

static void steamstub_encrypt_strings(unsigned char *bytes, size_t size,
                                      uint32_t *key) {
    for (size_t i = 0; i + 4 <= size; i += 4) {
        uint32_t block;
        memcpy(&block, bytes + i, sizeof(block));
        block ^= *key;
        memcpy(bytes + i, &block, sizeof(block));
        *key = block;
    }
}

void dearxan_steamstub_decrypt_drmp(unsigned char *bytes, size_t size,
                                    const uint32_t key[4]) {
    uint32_t xor_key[2] = { 0x55555555u, 0x55555555u };
    for (size_t i = 0; i + 8 <= size; i += 8) {
        uint32_t block[2];
        uint32_t next_xor[2];
        uint32_t sum = 0xC6EF3720u;
        memcpy(block, bytes + i, sizeof(block));
        next_xor[0] = block[0];
        next_xor[1] = block[1];
        for (unsigned int round = 0; round < 32; round++) {
            block[1] -= (block[0] + ((block[0] << 4) ^ (block[0] >> 5))) ^
                        (sum + key[(sum >> 11) & 3]);
            sum -= 0x9E3779B9u;
            block[0] -= (block[1] + ((block[1] << 4) ^ (block[1] >> 5))) ^
                        (sum + key[sum & 3]);
        }
        block[0] ^= xor_key[0];
        block[1] ^= xor_key[1];
        memcpy(bytes + i, block, sizeof(block));
        xor_key[0] = next_xor[0];
        xor_key[1] = next_xor[1];
    }
}

static unsigned char *read_rva(const dearxan_image_t *image, uint64_t rva,
                               size_t size) {
    return (unsigned char *)dearxan_image_read(image, image->base_va + rva,
                                                size, NULL);
}

dearxan_steamstub_probe_result_t dearxan_steamstub_probe(
    const dearxan_image_t *image, dearxan_steamstub_context_t *context) {
    uint64_t header_rva;
    uint64_t strings_rva;
    uint64_t drmp_rva;
    uint32_t string_key;
    if (image == NULL || context == NULL) return DEARXAN_STEAMSTUB_ERROR;
    memset(context, 0, sizeof(*context));
    if (image->entrypoint_rva < sizeof(dearxan_steamstub_header_t)) {
        return DEARXAN_STEAMSTUB_ABSENT;
    }
    header_rva = image->entrypoint_rva - sizeof(dearxan_steamstub_header_t);
    context->encrypted_header = (dearxan_steamstub_header_t *)
        read_rva(image, header_rva, sizeof(*context->encrypted_header));
    if (context->encrypted_header == NULL) return DEARXAN_STEAMSTUB_ERROR;
    if ((context->encrypted_header->xor_key ^ context->encrypted_header->signature) !=
        STEAMSTUB_SIGNATURE) return DEARXAN_STEAMSTUB_ABSENT;
    context->header = *context->encrypted_header;
    string_key = dearxan_steamstub_decrypt_header(&context->header);
    context->strings_size = align16(context->header.strings_data_size);
    context->drmp_size = context->header.drmp_dll_size;
    if (context->strings_size == SIZE_MAX) goto fail;
    if (context->header.bind_section_ep_offset > image->entrypoint_rva) goto fail;
    strings_rva = image->entrypoint_rva - context->header.bind_section_ep_offset +
                   context->header.strings_bind_offset;
    drmp_rva = image->entrypoint_rva - context->header.bind_section_ep_offset +
                context->header.drmp_dll_bind_offset;
    if (strings_rva > UINT32_MAX || drmp_rva > UINT32_MAX) goto fail;
    context->encrypted_strings = read_rva(image, strings_rva, context->strings_size);
    if (context->encrypted_strings == NULL ||
        read_rva(image, drmp_rva, context->drmp_size) == NULL) goto fail;
    context->decrypted_strings = malloc(context->strings_size);
    context->decrypted_drmp = malloc(context->drmp_size);
    if ((context->strings_size != 0 && context->decrypted_strings == NULL) ||
        (context->drmp_size != 0 && context->decrypted_drmp == NULL)) goto fail;
    memcpy(context->decrypted_strings, context->encrypted_strings, context->strings_size);
    memcpy(context->decrypted_drmp, read_rva(image, drmp_rva, context->drmp_size),
           context->drmp_size);
    dearxan_steamstub_crypt_strings(context->decrypted_strings,
                                    context->strings_size, &string_key);
    dearxan_steamstub_decrypt_drmp(context->decrypted_drmp, context->drmp_size,
                                  context->header.drmp_xtea_key);
    return DEARXAN_STEAMSTUB_PRESENT;
fail:
    dearxan_steamstub_uninit(context);
    return DEARXAN_STEAMSTUB_ERROR;
}

bool dearxan_steamstub_detect(const dearxan_image_t *image,
                              dearxan_steamstub_context_t *context) {
    return dearxan_steamstub_probe(image, context) == DEARXAN_STEAMSTUB_PRESENT;
}

static bool protected_write(void *target, const void *source, size_t size) {
    DWORD old_protection;
    if (!VirtualProtect(target, size, PAGE_EXECUTE_READWRITE, &old_protection)) return false;
    memcpy(target, source, size);
    VirtualProtect(target, size, old_protection, &old_protection);
    FlushInstructionCache(GetCurrentProcess(), target, size);
    return true;
}

bool dearxan_steamstub_patch(const dearxan_image_t *image,
                             dearxan_steamstub_context_t *context,
                             uint64_t replacement_entrypoint) {
    dearxan_steamstub_header_t encrypted;
    unsigned char *strings;
    uint32_t key;
    uint32_t hash = 0;
    size_t code_size;
    unsigned char *code;
    if (image == NULL || context == NULL ||
        context->encrypted_header == NULL) return false;
    context->header.drm_flags |= STEAMSTUB_FLAG_NO_MODULE_VERIFICATION |
                                 STEAMSTUB_FLAG_NO_OWNERSHIP_CHECK |
                                 STEAMSTUB_FLAG_NO_DEBUGGER_CHECK;
    context->header.original_entry_point = replacement_entrypoint - image->base_va;
    context->header.integrity_hash = 0;
    code_size = align16(context->header.steamstub_ep_code_size);
    if (code_size == SIZE_MAX) return false;
    code = read_rva(image, image->entrypoint_rva, code_size);
    if (code == NULL) return false;
    hash = dearxan_steamstub_hash(context->decrypted_strings, context->strings_size, hash);
    hash = dearxan_steamstub_hash((const unsigned char *)&context->header,
                                  sizeof(context->header), hash);
    hash = dearxan_steamstub_hash(code, code_size, hash);
    hash = dearxan_steamstub_hash(context->decrypted_drmp, context->drmp_size, hash);
    context->header.integrity_hash = hash;
    encrypted = context->header;
    key = steamstub_encrypt_header(&encrypted);
    strings = malloc(context->strings_size);
    if (context->strings_size != 0 && strings == NULL) return false;
    memcpy(strings, context->decrypted_strings, context->strings_size);
    steamstub_encrypt_strings(strings, context->strings_size, &key);
    if (!protected_write(context->encrypted_header, &encrypted, sizeof(encrypted)) ||
        (context->strings_size != 0 &&
         !protected_write(context->encrypted_strings, strings,
                          context->strings_size))) {
        free(strings);
        return false;
    }
    free(strings);
    return true;
}

void dearxan_steamstub_uninit(dearxan_steamstub_context_t *context) {
    if (context == NULL) return;
    free(context->decrypted_strings);
    free(context->decrypted_drmp);
    memset(context, 0, sizeof(*context));
}
