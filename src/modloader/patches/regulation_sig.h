#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Finds unique SPRJ regulation writer wrappers referenced by the legacy
 * stack-object call site. Passing NULL for writers returns an upper bound that
 * can be used to allocate the output array. Otherwise the return value is the
 * number of initialized entries and never exceeds writer_capacity.
 */
size_t ml_regulation_sprj_find_writers(uint8_t *text, size_t text_size,
                                       uint8_t **writers,
                                       size_t writer_capacity);

#ifdef __cplusplus
}
#endif
