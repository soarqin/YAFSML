/*
 * C11 implementation of dearxan v0.5.3 functionality.
 *
 * Original project copyright (c) 2025 William Tremblay.
 * Licensed under the MIT license; see THIRD_PARTY_NOTICES.md.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dearxan_image {
    const unsigned char *base;
    size_t size;
    uint64_t base_va;
    uint64_t preferred_base;
    uint32_t entrypoint_rva;
} dearxan_image_t;

typedef bool (*dearxan_section_callback_t)(uint64_t va,
                                           const unsigned char *bytes,
                                           size_t size, void *opaque);
typedef bool (*dearxan_relocation_callback_t)(uint32_t rva, void *opaque);

bool dearxan_image_from_module(void *module, dearxan_image_t *image);
bool dearxan_image_set_preferred_base_from_file(dearxan_image_t *image,
                                                 const wchar_t *path);
const unsigned char *dearxan_image_read(const dearxan_image_t *image,
                                        uint64_t va, size_t min_size,
                                        size_t *available);
bool dearxan_image_for_each_section(const dearxan_image_t *image,
                                    dearxan_section_callback_t callback,
                                    void *opaque);
bool dearxan_image_for_each_relocation64(const dearxan_image_t *image,
                                         dearxan_relocation_callback_t callback,
                                         void *opaque);
bool dearxan_image_make_sections_rwe(const dearxan_image_t *image);

#ifdef __cplusplus
}
#endif
