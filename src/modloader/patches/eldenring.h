/*
 * Copyright (C) 2024,2025, Soar Qin<soarchin@gmail.com>

 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Elden Ring's archive position resolver hook, which redirects "dataN:/" paths
 * that a mod overrides to loose files. */
extern bool eldenring_archive_resolver_install(void *image_base, size_t image_size);
extern void eldenring_archive_resolver_uninstall(void);

#ifdef __cplusplus
}
#endif
