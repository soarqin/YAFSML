/*
 * Copyright (C) 2026, Soar Qin<soarchin@gmail.com>
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#pragma once

/*
 * Pure signature-scanning and crypto-parsing helpers used by the Dantelion
 * asset hooks. Kept free of hook state so they can be unit-tested in isolation
 * (see tests/smoke_asset_hooks.c).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mount_pattern_match_s {
    size_t displacement_offset;
    size_t instruction_end_offset;
    /* The call-site pattern; otherwise the less reliable whole-function one. */
    bool call_site;
} mount_pattern_match_t;

/* Matches one of the known MountEbl call-site byte patterns at `p`. On success
 * fills `match` with the offsets of the RIP-relative displacement and the end
 * of the CALL instruction. */
bool ml_asset_sig_match_mount_ebl(const uint8_t *p, size_t remaining, mount_pattern_match_t *match);

/* Parses a PEM "RSA PUBLIC KEY" block and returns the modulus block size in
 * bytes via `block_size`. Returns false on malformed input. */
bool ml_asset_sig_rsa_block_size(const char *pem, size_t pem_length, size_t *block_size);

#ifdef __cplusplus
}
#endif
