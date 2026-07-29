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

/* Installs the trigger that advances ML_LIFECYCLE_PHASE_AFTER_DATA_READY once
   every game param has been loaded. Must run after the runtime-ready phase so
   that the image is unpacked and RTTI is resolvable. */
bool ml_data_ready_install(const ml_game_descriptor_t *game);
bool ml_data_ready_uninstall(void);
