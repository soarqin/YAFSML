/*
 * Copyright (C) 2024, Soar Qin<soarchin@gmail.com>

 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

extern bool gamehook_install(void);
extern void gamehook_uninstall(void);

#ifdef __cplusplus
}
#endif
