/*
 * Copyright (C) 2026, Soar Qin<soarchin@gmail.com>
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#pragma once

#include "modloader/hook.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

ml_hook_result_t ml_window_flash_install(void);
void ml_window_class_blacken(const WNDCLASSEXW *source, HBRUSH background, WNDCLASSEXW *result);

#ifdef __cplusplus
}
#endif
