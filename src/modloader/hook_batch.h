#pragma once

#include "hook.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ml_hook_spec_s {
    void *target;
    void *detour;
    void **original;
} ml_hook_spec_t;

typedef ml_hook_result_t (*ml_hook_batch_install_fn_t)(void *target, void *detour, void **original);
typedef bool (*ml_hook_batch_remove_fn_t)(void *target);

bool ml_hook_batch_install(const ml_hook_spec_t *specs, size_t count,
                           ml_hook_batch_install_fn_t install_fn,
                           ml_hook_batch_remove_fn_t remove_fn,
                           bool *rollback_complete);
bool ml_hook_batch_remove(const ml_hook_spec_t *specs, size_t count,
                          ml_hook_batch_remove_fn_t remove_fn);
bool ml_hook_targets_remove_unique(void *const *targets, size_t count,
                                   ml_hook_batch_remove_fn_t remove_fn);

#ifdef __cplusplus
}
#endif
