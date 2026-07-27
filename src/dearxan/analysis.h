/*
 * C11 implementation of dearxan v0.5.3 functionality.
 *
 * Original project copyright (c) 2025 William Tremblay.
 * Licensed under the MIT license; see THIRD_PARTY_NOTICES.md.
 */

#pragma once

#include "encryption.h"
#include "image.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dearxan_return_gadget {
    size_t stack_offset;
    uint64_t address;
} dearxan_return_gadget_t;

typedef struct dearxan_stub_info {
    uint64_t test_rsp_va;
    uint64_t context_pop_va;
    bool has_return_gadget;
    dearxan_return_gadget_t return_gadget;
    bool has_encrypted_regions;
    dearxan_encrypted_region_list_t encrypted_regions;
} dearxan_stub_info_t;

typedef struct dearxan_stub_list {
    dearxan_stub_info_t *items;
    size_t count;
} dearxan_stub_list_t;

bool dearxan_analyze_all_stubs(const dearxan_image_t *image,
                               dearxan_stub_list_t *stubs,
                               const char **error_message);
void dearxan_free_stub_list(dearxan_stub_list_t *stubs);

#ifdef __cplusplus
}
#endif
