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

/* Installs the descriptor-driven game adapter: the SteamAPI_Init runtime-ready
 * trigger, then asset, render-ready and Wwise capabilities after it fires. */
bool ml_adapter_install(const ml_game_descriptor_t *game);
void ml_adapter_uninstall(void);

#ifdef __cplusplus
}
#endif
