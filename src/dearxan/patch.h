/*
 * C11 implementation of dearxan v0.5.3 functionality.
 *
 * Original project copyright (c) 2025 William Tremblay.
 * Licensed under the MIT license; see THIRD_PARTY_NOTICES.md.
 */

#pragma once

#include "analysis.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

bool dearxan_apply_stub_patches(const dearxan_image_t *image,
                                const dearxan_stub_list_t *stubs,
                                const char **error_message);

#ifdef __cplusplus
}
#endif
