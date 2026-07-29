/*
 * Copyright (C) 2026, Soar Qin<soarchin@gmail.com>
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#pragma once

#include <stdbool.h>

typedef enum ml_lifecycle_phase_e {
    ML_LIFECYCLE_PHASE_UNKNOWN = 0,
    ML_LIFECYCLE_PHASE_PRE_ENTRY_SAFE,
    ML_LIFECYCLE_PHASE_BEFORE_MAIN,
    ML_LIFECYCLE_PHASE_AFTER_RUNTIME_INIT,
    ML_LIFECYCLE_PHASE_AFTER_PROPERTIES_READY,
    /* Title/menu construction finished: the game can render, but game params
       are not guaranteed to be loaded yet. */
    ML_LIFECYCLE_PHASE_AFTER_RENDER_READY,
    /* Game params are fully loaded and post-processed. */
    ML_LIFECYCLE_PHASE_AFTER_DATA_READY,
} ml_lifecycle_phase_t;

#define ML_LIFECYCLE_PHASE_LAST ML_LIFECYCLE_PHASE_AFTER_DATA_READY

typedef void (*ml_lifecycle_callback_t)(ml_lifecycle_phase_t phase, void *userp);

void ml_lifecycle_init(void);
void ml_lifecycle_uninit(void);
ml_lifecycle_phase_t ml_lifecycle_current(void);
bool ml_lifecycle_on_phase(ml_lifecycle_phase_t phase, ml_lifecycle_callback_t callback, void *userp);
bool ml_lifecycle_advance(ml_lifecycle_phase_t phase);
