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

typedef struct dearxan_msvc_entrypoint {
    uint64_t security_init_cookie_va;
    uint64_t scrt_common_main_seh_va;
    unsigned char *security_init_cookie_call;
    bool is_arxan_hooked;
} dearxan_msvc_entrypoint_t;

typedef struct dearxan_suspend_guard {
    void **threads;
    size_t count;
} dearxan_suspend_guard_t;

bool dearxan_parse_msvc_entrypoint(const dearxan_image_t *image,
                                   uint64_t entrypoint_va,
                                   dearxan_msvc_entrypoint_t *result);
bool dearxan_is_pre_entry_point(const dearxan_image_t *image);
bool dearxan_wait_for_gs_cookie(const dearxan_image_t *image,
                                 unsigned long timeout_ms);
bool dearxan_suspend_other_threads(dearxan_suspend_guard_t *guard);
void dearxan_resume_threads(dearxan_suspend_guard_t *guard);

#ifdef __cplusplus
}
#endif
