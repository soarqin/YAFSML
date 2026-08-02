/*
 * Copyright (C) 2026, Soar Qin<soarchin@gmail.com>
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#include "ime_sig.h"

#include "process/scanner.h"

uint8_t *ml_ime_sig_find_character_filter(uint8_t *text, size_t text_size) {
    static const char pattern[] =
        "66 3B 10 74 13 FF C1 48 83 C0 02 83 F9 60 72 F0 "
        "BA 2A 00 00 00 40 B7 01 66 89 54 24 20";
    uint8_t *match;
    uint8_t *next;
    size_t remaining;

    match = sig_scan(text, text_size, pattern);
    if (match == NULL) return NULL;
    remaining = text_size - (size_t)(match + 1 - text);
    next = sig_scan(match + 1, remaining, pattern);
    return next == NULL ? match + 3 : NULL;
}
