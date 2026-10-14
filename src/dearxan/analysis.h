/*
 * C11 implementation of dearxan v0.5.7 functionality.
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
/* As above, but caps the worker threads: `max_workers == 1` keeps all work on
 * the calling thread, which is required when other process threads are suspended
 * (a worker could otherwise deadlock on a lock a suspended thread holds).
 * `max_workers == 0` selects the default based on the processor count. */
bool dearxan_analyze_all_stubs_with_workers(const dearxan_image_t *image,
                                            dearxan_stub_list_t *stubs,
                                            const char **error_message,
                                            unsigned int max_workers);
void dearxan_free_stub_list(dearxan_stub_list_t *stubs);

#ifdef __cplusplus
}
#endif
