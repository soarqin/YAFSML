/*
 * Copyright (C) 2024,2025, Soar Qin<soarchin@gmail.com>

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

extern void common_apply_process_settings(void);
extern bool common_schedule_process_settings(void);
extern void common_wait_for_process_settings(void);
extern bool common_install_file_routing(const ml_game_descriptor_t *game);
extern void common_uninstall_file_routing(void);
extern bool common_install_ime(void);
extern bool common_wwise_requested(void);
extern bool common_install_wwise(void);
extern void common_uninstall(void);

#ifdef __cplusplus
}
#endif
