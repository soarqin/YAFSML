/*
 * Copyright (C) 2026, Soar Qin<soarchin@gmail.com>
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

/* Returns the conditional-branch patch site in the unique matching sequence. */
uint8_t *ml_ime_sig_find_character_filter(uint8_t *text, size_t text_size);
