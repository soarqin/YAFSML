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

void dearxan_tea_decrypt(uint32_t block[2], const uint32_t key[4]);
uint32_t dearxan_rmx_decrypt(uint32_t block, uint32_t *key, uint32_t *key_rotation);
uint32_t dearxan_sub_decrypt(uint32_t block, uint32_t key);
bool dearxan_read_varint(const unsigned char *bytes, size_t length,
                         size_t *consumed, uint32_t *value);
double dearxan_shannon_entropy(const unsigned char *bytes, size_t length);

typedef enum dearxan_decryption_kind {
    DEARXAN_DECRYPTION_TEA,
    DEARXAN_DECRYPTION_RMX,
    DEARXAN_DECRYPTION_SUB
} dearxan_decryption_kind_t;

typedef struct dearxan_encrypted_region {
    size_t stream_offset;
    size_t size;
    uint32_t rva;
} dearxan_encrypted_region_t;

typedef struct dearxan_encrypted_region_list {
    dearxan_decryption_kind_t kind;
    dearxan_encrypted_region_t *regions;
    size_t region_count;
    unsigned char *decrypted_stream;
    size_t decrypted_size;
} dearxan_encrypted_region_list_t;

bool dearxan_parse_encrypted_regions(const unsigned char *bytes, size_t length,
                                     dearxan_encrypted_region_t **regions,
                                     size_t *region_count,
                                     size_t *stream_size);
void dearxan_free_encrypted_region_list(dearxan_encrypted_region_list_t *list);
bool dearxan_apply_relocations(const dearxan_image_t *image,
                               dearxan_encrypted_region_list_t *list);
bool dearxan_resolve_encrypted_region_lists(
    const dearxan_image_t *image,
    const dearxan_encrypted_region_list_t *const *lists, size_t list_count,
    dearxan_encrypted_region_list_t **resolved, size_t *resolved_count);
void dearxan_free_encrypted_region_lists(dearxan_encrypted_region_list_t *lists,
                                         size_t list_count);

#ifdef __cplusplus
}
#endif
