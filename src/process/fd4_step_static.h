#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fd4_step_static_sections_s {
    const uint8_t *text;
    size_t text_size;
    uintptr_t text_va;
    uintptr_t data_va;
    size_t data_size;
    const uint8_t *rdata;
    size_t rdata_size;
    uintptr_t rdata_va;
} fd4_step_static_sections_t;

typedef struct fd4_step_static_result_s {
    void *step;
    void **slot;
} fd4_step_static_result_t;

extern bool fd4_step_static_find(const fd4_step_static_sections_t *sections,
                                 const wchar_t *step_name,
                                 fd4_step_static_result_t *result);

#ifdef __cplusplus
}
#endif
