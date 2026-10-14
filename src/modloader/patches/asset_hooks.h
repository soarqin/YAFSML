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

#ifdef __cplusplus
extern "C" {
#endif

bool ml_asset_hooks_install(const ml_game_descriptor_t *game, void *image_base, size_t image_size);
bool ml_asset_hooks_install_render_ready(const ml_game_descriptor_t *game);
bool ml_asset_hooks_uninstall(void);
bool ml_asset_hooks_requested(void);
bool ml_asset_hooks_loose_params_present(const ml_game_descriptor_t *game);
#ifdef ML_ASSET_HOOKS_TEST
bool ml_asset_hooks_test_assign_bhd_contents(void *holder, uint8_t *contents,
                                             uint32_t bucket_count, uint32_t bucket_offset);
#endif

#ifdef __cplusplus
}
#endif
