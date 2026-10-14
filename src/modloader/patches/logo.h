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

#ifdef __cplusplus
extern "C" {
#endif

bool ml_logo_fd4_redirect(void **step_slot, void *next_step);
bool ml_logo_sprj_redirect(void **step_slot);
bool ml_logo_skip_install(const ml_game_descriptor_t *game);

#ifdef __cplusplus
}
#endif
