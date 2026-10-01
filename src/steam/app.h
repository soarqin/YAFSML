/*
 * Copyright (C) 2024, Soar Qin<soarchin@gmail.com>

 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#pragma once

#include <wchar.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* path must hold MAX_PATH wchar_t values; a failed search clears it. */
extern bool app_find_game_path(uint32_t app_id, wchar_t *path);

#ifdef __cplusplus
}
#endif
