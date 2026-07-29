/*
 * Copyright (C) 2026, Soar Qin<soarchin@gmail.com>
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#pragma once

#include "game/game.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool ml_asset_hooks_install(const ml_game_descriptor_t *game, void *image_base, size_t image_size);
bool ml_asset_hooks_install_render_ready(const ml_game_descriptor_t *game);
bool ml_asset_hooks_uninstall(void);
bool ml_asset_hooks_requested(void);
bool ml_asset_hooks_test_match_mount_ebl(const uint8_t *bytes, size_t size,
                                         size_t *displacement_offset, size_t *instruction_end_offset);
bool ml_asset_hooks_test_rsa_public_key_block_size(const char *pem, size_t pem_length, size_t *block_size);
bool ml_asset_hooks_test_assign_bhd_contents(void *holder, uint8_t *contents,
                                             uint32_t bucket_count, uint32_t bucket_offset);
bool ml_asset_hooks_loose_params_present(const ml_game_descriptor_t *game);
bool ml_asset_hooks_is_loose_param_path(const ml_game_descriptor_t *game, const wchar_t *path);
