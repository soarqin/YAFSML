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
#include <string.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Resolves a rel32 operand (rip-relative memory, call or jmp target): the end
 * of the instruction plus the signed 32-bit displacement at disp32, which
 * need not be aligned. */
static inline uintptr_t ml_rel32_target(const void *disp32, uintptr_t instruction_end) {
    int32_t displacement;
    memcpy(&displacement, disp32, sizeof(displacement));
    return instruction_end + (uintptr_t)(intptr_t)displacement;
}

const uint8_t *ml_find_utf16_string(const void *data, size_t size, const wchar_t *value);
const uint8_t *ml_find_rip_relative_lea(const uint8_t *text, size_t size, const void *target);

#ifdef __cplusplus
}
#endif
