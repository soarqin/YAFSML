/*
 * Copyright (C) 2024,2025, Soar Qin<soarchin@gmail.com>

 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#pragma once

#ifdef ML_SINGLETON_TEST
#include <stdbool.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Returns the singleton instance pointer for name, or NULL. */
extern void *singleton_find(const char *name);

/* Returns the address of the static slot that holds the singleton pointer for
 * name, or NULL. The slot lives in the game's .data and stays valid, so a
 * per-frame caller can resolve it once and dereference it on every tick. FD4
 * singletons resolve only after the game publishes their reflection names, so
 * an early NULL can succeed on a later call. */
extern void **singleton_find_slot(const char *name);

#ifdef ML_SINGLETON_TEST
/* Builds the tables from image_base, a mapped PE image, instead of the game
 * executable. Must run before the first lookup. */
extern bool singleton_test_use_image(void *image_base);
#endif

#ifdef __cplusplus
}
#endif
