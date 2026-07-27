/*
 * Copyright (C) 2026, Soar Qin<soarchin@gmail.com>
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#include "lifecycle.h"

#include "allocator.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

typedef struct ml_phase_observer_s {
    ml_lifecycle_phase_t phase;
    ml_lifecycle_callback_t callback;
    void *userp;
} ml_phase_observer_t;

static SRWLOCK observers_lock = SRWLOCK_INIT;
static ml_phase_observer_t *observers;
static size_t observer_count;
static size_t observer_capacity;
static volatile LONG reached_phases;

void ml_lifecycle_init(void) {
    AcquireSRWLockExclusive(&observers_lock);
    ml_mem_free(observers);
    observers = NULL;
    observer_count = 0;
    observer_capacity = 0;
    InterlockedExchange(&reached_phases, 0);
    ReleaseSRWLockExclusive(&observers_lock);
}

void ml_lifecycle_uninit(void) {
    ml_lifecycle_init();
}

ml_lifecycle_phase_t ml_lifecycle_current(void) {
    LONG reached = InterlockedCompareExchange(&reached_phases, 0, 0);
    for (int phase = ML_LIFECYCLE_PHASE_AFTER_GAME_DATA_READY;
         phase > ML_LIFECYCLE_PHASE_UNKNOWN; phase--) {
        if ((reached & (1L << phase)) != 0) return (ml_lifecycle_phase_t)phase;
    }
    return ML_LIFECYCLE_PHASE_UNKNOWN;
}

bool ml_lifecycle_on_phase(ml_lifecycle_phase_t phase, ml_lifecycle_callback_t callback, void *userp) {
    if (phase <= ML_LIFECYCLE_PHASE_UNKNOWN || phase > ML_LIFECYCLE_PHASE_AFTER_GAME_DATA_READY || callback == NULL) {
        return false;
    }

    AcquireSRWLockExclusive(&observers_lock);
    if ((InterlockedCompareExchange(&reached_phases, 0, 0) & (1L << phase)) != 0) {
        ReleaseSRWLockExclusive(&observers_lock);
        return false;
    }
    if (observer_count == observer_capacity) {
        size_t capacity = observer_capacity == 0 ? 8 : observer_capacity * 2;
        ml_phase_observer_t *new_observers = observers == NULL
            ? ml_mem_alloc(LMEM_ZEROINIT, capacity * sizeof(*observers))
            : ml_mem_realloc(observers, capacity * sizeof(*observers), LMEM_MOVEABLE | LMEM_ZEROINIT);
        if (new_observers == NULL) {
            ReleaseSRWLockExclusive(&observers_lock);
            return false;
        }
        observers = new_observers;
        observer_capacity = capacity;
    }
    observers[observer_count++] = (ml_phase_observer_t){ phase, callback, userp };
    ReleaseSRWLockExclusive(&observers_lock);
    return true;
}

bool ml_lifecycle_advance(ml_lifecycle_phase_t phase) {
    LONG mask;
    if (phase <= ML_LIFECYCLE_PHASE_UNKNOWN || phase > ML_LIFECYCLE_PHASE_AFTER_GAME_DATA_READY) return false;

    mask = 1L << phase;
    if ((InterlockedOr(&reached_phases, mask) & mask) != 0) return true;

    for (;;) {
        ml_lifecycle_callback_t callback = NULL;
        ml_lifecycle_phase_t callback_phase = ML_LIFECYCLE_PHASE_UNKNOWN;
        void *userp = NULL;

        AcquireSRWLockExclusive(&observers_lock);
        for (size_t i = 0; i < observer_count; i++) {
            if (observers[i].callback != NULL && observers[i].phase == phase) {
                callback = observers[i].callback;
                callback_phase = observers[i].phase;
                userp = observers[i].userp;
                observers[i].callback = NULL;
                break;
            }
        }
        ReleaseSRWLockExclusive(&observers_lock);
        if (callback == NULL) break;
        callback(callback_phase, userp);
    }
    return true;
}
