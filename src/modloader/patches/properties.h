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

bool ml_properties_install(const ml_game_descriptor_t *game);
void ml_properties_on_init(void);
void ml_properties_set_loose_params(bool enabled);

#ifdef __cplusplus
}
#endif
