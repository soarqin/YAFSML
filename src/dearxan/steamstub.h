/*
 * C11 implementation of dearxan v0.5.3 functionality.
 *
 * Original project copyright (c) 2025 William Tremblay.
 * Licensed under the MIT license; see THIRD_PARTY_NOTICES.md.
 */

#pragma once

#include "image.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#pragma pack(push, 1)
typedef struct dearxan_steamstub_header {
    uint32_t xor_key;
    uint32_t signature;
    uint64_t image_base;
    uint64_t steamstub_entry_point;
    uint32_t bind_section_ep_offset;
    uint32_t steamstub_ep_code_size;
    uint64_t original_entry_point;
    uint32_t strings_bind_offset;
    uint32_t strings_data_size;
    uint32_t drmp_dll_bind_offset;
    uint32_t drmp_dll_size;
    uint32_t steam_app_id;
    uint32_t drm_flags;
    uint32_t bind_section_virtual_size;
    uint32_t integrity_hash;
    uint64_t code_section_virtual_address;
    uint64_t code_section_size;
    unsigned char aes_key[32];
    unsigned char aes_iv[16];
    unsigned char code_section_bytes[16];
    uint32_t drmp_xtea_key[4];
    uint32_t unknown_a8[8];
    uint64_t get_module_handle_a_rva;
    uint64_t get_module_handle_w_rva;
    uint64_t load_library_a_rva;
    uint64_t load_library_w_rva;
    uint64_t get_proc_address_rva;
} dearxan_steamstub_header_t;
#pragma pack(pop)

typedef struct dearxan_steamstub_context {
    dearxan_steamstub_header_t header;
    dearxan_steamstub_header_t *encrypted_header;
    unsigned char *encrypted_strings;
    unsigned char *decrypted_strings;
    size_t strings_size;
    unsigned char *decrypted_drmp;
    size_t drmp_size;
} dearxan_steamstub_context_t;

typedef enum dearxan_steamstub_probe_result {
    DEARXAN_STEAMSTUB_ABSENT,
    DEARXAN_STEAMSTUB_PRESENT,
    DEARXAN_STEAMSTUB_ERROR
} dearxan_steamstub_probe_result_t;

uint32_t dearxan_steamstub_hash(const unsigned char *bytes, size_t size,
                                uint32_t hash);
uint32_t dearxan_steamstub_decrypt_header(dearxan_steamstub_header_t *header);
void dearxan_steamstub_crypt_strings(unsigned char *bytes, size_t size,
                                     uint32_t *key);
void dearxan_steamstub_decrypt_drmp(unsigned char *bytes, size_t size,
                                    const uint32_t key[4]);
bool dearxan_steamstub_detect(const dearxan_image_t *image,
                              dearxan_steamstub_context_t *context);
dearxan_steamstub_probe_result_t dearxan_steamstub_probe(
    const dearxan_image_t *image, dearxan_steamstub_context_t *context);
bool dearxan_steamstub_patch(const dearxan_image_t *image,
                             dearxan_steamstub_context_t *context,
                             uint64_t replacement_entrypoint);
void dearxan_steamstub_uninit(dearxan_steamstub_context_t *context);

#ifdef __cplusplus
}
#endif
