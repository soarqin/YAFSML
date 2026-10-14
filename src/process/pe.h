/*
 * Copyright (C) 2024,2025, Soar Qin<soarchin@gmail.com>

 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns the NT headers of a mapped PE32+ image, or NULL when the DOS or NT
 * signature or the optional header magic does not match. */
extern const IMAGE_NT_HEADERS64 *pe_nt_headers(const void *image_base);
extern const IMAGE_SECTION_HEADER *pe_section_by_name(void *image_base, const char *name);
/* *out_size is the section size pe_section_rva_end also uses. */
extern void *pe_section_data(void *image_base, const IMAGE_SECTION_HEADER *sh, size_t *out_size);
extern uint32_t pe_section_rva_start(const IMAGE_SECTION_HEADER *sh);
extern uint32_t pe_section_rva_end(const IMAGE_SECTION_HEADER *sh);
extern bool pe_section_contains_rva(const IMAGE_SECTION_HEADER *sh, uint32_t rva);
extern bool pe_section_contains_va(void *image_base, const IMAGE_SECTION_HEADER *sh, const void *va);

#ifdef __cplusplus
}
#endif
