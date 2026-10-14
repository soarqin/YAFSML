/*
 * Copyright (C) 2026, Soar Qin<soarchin@gmail.com>
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#include "rip_scan.h"

#include <string.h>

const uint8_t *ml_find_utf16_string(const void *data, size_t size, const wchar_t *value) {
    size_t value_size;
    const uint8_t *cursor;
    const uint8_t *last;
    uint8_t first;

    if (data == NULL || value == NULL) return NULL;
    {
        size_t value_length = wcslen(value);
        if (value_length > (SIZE_MAX / sizeof(wchar_t)) - 1) return NULL;
        value_size = (value_length + 1) * sizeof(wchar_t);
    }
    if (value_size > size) return NULL;
    /* Only offsets holding the first byte can match; memchr finds them. */
    cursor = (const uint8_t *)data;
    last = cursor + (size - value_size);
    memcpy(&first, value, sizeof(first));
    while (cursor <= last) {
        cursor = memchr(cursor, first, (size_t)(last - cursor) + 1);
        if (cursor == NULL) return NULL;
        if (memcmp(cursor, value, value_size) == 0) return cursor;
        cursor++;
    }
    return NULL;
}

const uint8_t *ml_find_rip_relative_lea(const uint8_t *text, size_t size, const void *target) {
    uintptr_t target_address = (uintptr_t)target;

    if (text == NULL || target == NULL || size < 7) return NULL;
    for (size_t i = 0; i <= size - 7; i++) {
        int32_t displacement;
        uintptr_t instruction_end;
        uintptr_t resolved;

        if ((text[i] & 0xfb) != 0x48 ||
            (text[i + 1] != 0x8d && text[i + 1] != 0x8b) ||
            (text[i + 2] & 0xc7) != 0x05) continue;
        memcpy(&displacement, text + i + 3, sizeof(displacement));
        instruction_end = (uintptr_t)(text + i + 7);
        if (displacement >= 0) {
            if (instruction_end > UINTPTR_MAX - (uintptr_t)displacement) continue;
            resolved = instruction_end + (uintptr_t)displacement;
        } else {
            if (instruction_end < (uintptr_t)(-(int64_t)displacement)) continue;
            resolved = instruction_end - (uintptr_t)(-(int64_t)displacement);
        }
        if (resolved == target_address) return text + i;
    }
    return NULL;
}
