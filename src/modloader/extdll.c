/*
 * Copyright (C) 2024, Soar Qin<soarchin@gmail.com>

 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#include "extdll.h"

#include "allocator.h"

#include "config.h"
#include "log.h"
#include "game/game.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlwapi.h>

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* Enumerator order is load order. The first two stages run on the main thread
   during their lifecycle phase; the last two are deferred and share one worker,
   which walks them in array order rather than by stage. */
typedef enum extdll_stage_e {
    EXTDLL_STAGE_EARLY = 0,  /* ML_LIFECYCLE_PHASE_BEFORE_MAIN */
    EXTDLL_STAGE_NORMAL,     /* ML_LIFECYCLE_PHASE_AFTER_RUNTIME_INIT */
    EXTDLL_STAGE_DATA_READY, /* deferred worker, on ML_LIFECYCLE_PHASE_AFTER_DATA_READY */
    EXTDLL_STAGE_DELAY,      /* deferred worker, delay_ms after reaching the entry */
} extdll_stage_t;

typedef struct extdll_t {
    char *name;
    wchar_t *base_path;
    extdll_stage_t stage;
    /* EXTDLL_STAGE_DELAY only; 0 means "as soon as the worker reaches it". */
    uint32_t delay_ms;
    char **after;
    int after_count;
    int after_capacity;
    HMODULE dll_module;
    uint32_t load_order;
    bool module_load_attempted;
    bool init_attempted;
} extdll_t;

static const unsigned char modengine_connector_shim;

static extdll_t *extdlls = NULL;
static int extdll_count = 0;
static int extdll_capacity = 0;
static volatile LONG load_counter = 0;
static HANDLE deferred_worker;
static HANDLE cancel_event;
static HANDLE data_ready_event;

static bool extdlls_reserve(int capacity) {
    extdll_t *new_dlls;
    if (capacity <= extdll_capacity) return true;
    new_dlls = extdlls == NULL
        ? ml_mem_alloc(LMEM_ZEROINIT, (size_t)capacity * sizeof(*extdlls))
        : ml_mem_realloc(extdlls, (size_t)capacity * sizeof(*extdlls), LMEM_MOVEABLE | LMEM_ZEROINIT);
    if (new_dlls == NULL) return false;
    extdlls = new_dlls;
    extdll_capacity = capacity;
    return true;
}

static bool extdll_after_add(extdll_t *extdll, const char *name) {
    char **new_after;
    if (extdll->after_count >= extdll->after_capacity) {
        int capacity = extdll->after_capacity == 0 ? 4 : extdll->after_capacity * 2;
        new_after = extdll->after == NULL
            ? ml_mem_alloc(LMEM_ZEROINIT, (size_t)capacity * sizeof(*extdll->after))
            : ml_mem_realloc(extdll->after, (size_t)capacity * sizeof(*extdll->after),
                               LMEM_MOVEABLE | LMEM_ZEROINIT);
        if (new_after == NULL) return false;
        extdll->after = new_after;
        extdll->after_capacity = capacity;
    }
    extdll->after[extdll->after_count] = ml_mem_strdup_a(name);
    if (extdll->after[extdll->after_count] == NULL) return false;
    extdll->after_count++;
    return true;
}

static bool parse_delay(const char *value, uint32_t *delay_ms) {
    char *end;
    unsigned long parsed;
    if (value == NULL || value[0] < '0' || value[0] > '9') return false;
    errno = 0;
    parsed = strtoul(value, &end, 10);
    if (errno == ERANGE || end == value || *end != '\0' || parsed > UINT32_MAX) return false;
    *delay_ms = (uint32_t)parsed;
    return true;
}

static const char *stage_name(extdll_stage_t stage) {
    switch (stage) {
        case EXTDLL_STAGE_EARLY: return "early";
        case EXTDLL_STAGE_DATA_READY: return "data_ready";
        case EXTDLL_STAGE_DELAY: return "delay";
        case EXTDLL_STAGE_NORMAL:
        default: return "normal";
    }
}

void extdlls_add(const char *name, const wchar_t *path) {
    extdll_t *extdll;
    if (name == NULL || path == NULL) return;
    if (extdll_count >= extdll_capacity &&
        !extdlls_reserve(extdll_capacity == 0 ? 8 : extdll_capacity * 2)) return;
    extdll = &extdlls[extdll_count++];
    memset(extdll, 0, sizeof(*extdll));
    extdll->stage = EXTDLL_STAGE_NORMAL;
    extdll->name = ml_mem_strdup_a(name);
    extdll->base_path = ml_mem_strdup_w(path);
    if (extdll->name == NULL || extdll->base_path == NULL) {
        if (extdll->name != NULL) ml_mem_free(extdll->name);
        if (extdll->base_path != NULL) ml_mem_free(extdll->base_path);
        memset(extdll, 0, sizeof(*extdll));
        extdll_count--;
    }
}

void extdlls_add_spec(const char *name, const char *value) {
    char *spec;
    char *condition;
    char *next;
    wchar_t path[MAX_PATH];
    extdll_t *extdll;

    if (name == NULL || value == NULL) return;
    if (extdll_count >= extdll_capacity &&
        !extdlls_reserve(extdll_capacity == 0 ? 8 : extdll_capacity * 2)) return;
    spec = ml_mem_strdup_a(value);
    if (spec == NULL) return;
    condition = strchr(spec, '|');
    if (condition != NULL) *condition++ = '\0';
    if (spec[0] == '\0' || MultiByteToWideChar(CP_UTF8, 0, spec, -1, path, MAX_PATH) == 0) {
        ML_LOG_ERROR(L"extdll", L"invalid external DLL path for %hs", name);
        ml_mem_free(spec);
        return;
    }
    path[MAX_PATH - 1] = L'\0';

    extdll = &extdlls[extdll_count++];
    memset(extdll, 0, sizeof(*extdll));
    extdll->stage = EXTDLL_STAGE_NORMAL;
    extdll->name = ml_mem_strdup_a(name);
    extdll->base_path = ml_mem_strdup_w(path);
    if (extdll->name == NULL || extdll->base_path == NULL) {
        ML_LOG_ERROR(L"extdll", L"cannot allocate external DLL configuration for %hs", name);
        if (extdll->name != NULL) ml_mem_free(extdll->name);
        if (extdll->base_path != NULL) ml_mem_free(extdll->base_path);
        memset(extdll, 0, sizeof(*extdll));
        extdll_count--;
        ml_mem_free(spec);
        return;
    }
    while (condition != NULL && condition[0] != '\0') {
        next = strchr(condition, '|');
        if (next != NULL) *next++ = '\0';
        if (strcmp(condition, "early") == 0) {
            if (extdll->stage != EXTDLL_STAGE_NORMAL) {
                ML_LOG_ERROR(L"extdll", L"early cannot be combined with the %hs condition for %hs",
                             stage_name(extdll->stage), name);
            } else {
                extdll->stage = EXTDLL_STAGE_EARLY;
            }
        } else if (strcmp(condition, "data_ready") == 0) {
            if (extdll->stage != EXTDLL_STAGE_NORMAL) {
                ML_LOG_ERROR(L"extdll", L"data_ready cannot be combined with the %hs condition for %hs",
                             stage_name(extdll->stage), name);
            } else {
                extdll->stage = EXTDLL_STAGE_DATA_READY;
            }
        } else if (strncmp(condition, "delay,", 6) == 0) {
            uint32_t delay_ms;
            if (!parse_delay(condition + 6, &delay_ms)) {
                ML_LOG_ERROR(L"extdll", L"invalid delay condition for %hs: %hs", name, condition);
            } else if (extdll->stage != EXTDLL_STAGE_NORMAL) {
                ML_LOG_ERROR(L"extdll", L"delay cannot be combined with the %hs condition for %hs",
                             stage_name(extdll->stage), name);
            } else {
                extdll->stage = EXTDLL_STAGE_DELAY;
                extdll->delay_ms = delay_ms;
            }
        } else if (strncmp(condition, "after,", 6) == 0 && condition[6] != '\0') {
            if (!extdll_after_add(extdll, condition + 6)) {
                ML_LOG_ERROR(L"extdll", L"cannot allocate dependency for %hs", name);
            }
        } else {
            ML_LOG_WARN(L"extdll", L"unknown condition for %hs: %hs", name, condition);
        }
        condition = next;
    }
    ml_mem_free(spec);
}

int extdlls_count() {
    return extdll_count;
}

static int extdll_index_by_name(const char *name) {
    for (int i = 0; i < extdll_count; i++) {
        if (strcmp(extdlls[i].name, name) == 0) return i;
    }
    return -1;
}

void extdlls_prepare() {
    int *order;
    extdll_t *sorted;

    if (extdll_count < 2) {
        return;
    }
    order = ml_mem_alloc(0, (size_t)extdll_count * sizeof(*order));
    if (order == NULL) {
        ML_LOG_ERROR(L"extdll", L"cannot allocate external DLL dependency sorter");
        return;
    }

    for (int i = 0; i < extdll_count; i++) {
        for (int j = 0; j < extdlls[i].after_count; j++) {
            if (extdll_index_by_name(extdlls[i].after[j]) < 0) {
                ML_LOG_WARN(L"extdll", L"external DLL %hs depends on missing DLL %hs",
                            extdlls[i].name, extdlls[i].after[j]);
            }
        }
    }

    /* Order from the original positions. When a dependency is later than its
       dependent, move the nearest such dependency directly before the dependent.
       This keeps unrelated entries in their original order and changes only the
       entries needed to satisfy the constraints. A cycle never converges, which
       the move budget below detects; the configured order is then preserved. */
    for (int i = 0; i < extdll_count; i++) order[i] = i;
    for (int move_count = 0; move_count <= extdll_count * extdll_count; move_count++) {
        bool moved = false;
        for (int i = 0; i < extdll_count && !moved; i++) {
            int nearest_position = -1;
            for (int j = 0; j < extdlls[order[i]].after_count; j++) {
                int dependency = extdll_index_by_name(extdlls[order[i]].after[j]);
                int dependency_position = -1;
                if (dependency < 0) continue;
                for (int k = 0; k < extdll_count; k++) {
                    if (order[k] == dependency) {
                        dependency_position = k;
                        break;
                    }
                }
                if (dependency_position > i &&
                    (nearest_position < 0 || dependency_position < nearest_position)) {
                    nearest_position = dependency_position;
                }
            }
            if (nearest_position >= 0) {
                int dependency = order[nearest_position];
                memmove(&order[i + 1], &order[i], (size_t)(nearest_position - i) * sizeof(*order));
                order[i] = dependency;
                moved = true;
            }
        }
        if (!moved) break;
        if (move_count == extdll_count * extdll_count) {
            ML_LOG_ERROR(L"extdll", L"external DLL dependency cycle detected; preserving configured order");
            ml_mem_free(order);
            return;
        }
    }

    sorted = ml_mem_alloc(0, (size_t)extdll_count * sizeof(*sorted));
    if (sorted == NULL) {
        ML_LOG_ERROR(L"extdll", L"cannot allocate external DLL dependency order");
    } else {
        for (int i = 0; i < extdll_count; i++) sorted[i] = extdlls[order[i]];
        memcpy(extdlls, sorted, (size_t)extdll_count * sizeof(*extdlls));
        ml_mem_free(sorted);
    }
    /* Dependencies of an early DLL must also be available before the
       runtime-ready phase. Promote only the prerequisite closure. */
    for (;;) {
        bool changed = false;
        for (int i = 0; i < extdll_count; i++) {
            if (extdlls[i].stage != EXTDLL_STAGE_EARLY) continue;
            for (int j = 0; j < extdlls[i].after_count; j++) {
                int dependency = extdll_index_by_name(extdlls[i].after[j]);
                if (dependency >= 0 && extdlls[dependency].stage == EXTDLL_STAGE_NORMAL) {
                    extdlls[dependency].stage = EXTDLL_STAGE_EARLY;
                    changed = true;
                }
            }
        }
        if (!changed) break;
    }
    /* A DLL can never load before its dependencies. Both deferred stages share a
       single worker that walks the entries in array order, and the sort above put
       every dependency before its dependents, so entries that are already
       deferred need no adjustment whatever their dependencies are. Only entries
       still running on the main thread have to move into the worker, because the
       main thread finishes before the worker starts. */
    for (int i = 0; i < extdll_count; i++) {
        extdll_stage_t latest = EXTDLL_STAGE_NORMAL;
        int trigger = -1;
        if (extdlls[i].stage > EXTDLL_STAGE_NORMAL) continue;
        for (int j = 0; j < extdlls[i].after_count; j++) {
            int dependency = extdll_index_by_name(extdlls[i].after[j]);
            if (dependency < 0 || extdlls[dependency].stage <= latest) continue;
            latest = extdlls[dependency].stage;
            trigger = dependency;
        }
        if (trigger < 0) continue;
        if (extdlls[i].stage == EXTDLL_STAGE_EARLY) {
            ML_LOG_ERROR(L"extdll", L"early DLL %hs depends on %hs DLL %hs; loading at the %hs stage instead",
                         extdlls[i].name, stage_name(latest), extdlls[trigger].name,
                         stage_name(latest));
        }
        extdlls[i].stage = latest;
        /* Riding along with a dependency must not add another wait. */
        extdlls[i].delay_ms = 0;
    }
    ml_mem_free(order);
}

static HMODULE load_extdll_module(const extdll_t *extdll) {
    const wchar_t *path = extdll->base_path;
    if (StrChrW(path, L':') == NULL && path[0] != L'\\' && path[0] != L'/') {
        wchar_t full_path[MAX_PATH];
        config_full_path(full_path, path);
        return LoadLibraryW(full_path);
    }
    return LoadLibraryW(path);
}

static bool preload_extdll_one(int index) {
    extdll_t *extdll = &extdlls[index];
    if (extdll->module_load_attempted) return extdll->dll_module != NULL;
    extdll->module_load_attempted = true;
    extdll->dll_module = load_extdll_module(extdll);
    if (extdll->dll_module == NULL) {
        ML_LOG_ERROR(L"extdll", L"cannot load external DLL %hs from `%ls`", extdll->name, extdll->base_path);
        return false;
    }
    extdll->load_order = (uint32_t)InterlockedIncrement(&load_counter);
    ML_LOG_INFO(L"extdll", L"loaded external DLL %hs from `%ls`", extdll->name, extdll->base_path);
    return true;
}

static bool load_extdll_one(int index) {
    extdll_t *extdll = &extdlls[index];
    void *extension_object = NULL;
    if (!preload_extdll_one(index)) return false;
    if (extdll->init_attempted) return true;
    extdll->init_attempted = true;
    {
        FARPROC me_ext_init = GetProcAddress(extdll->dll_module, "modengine_ext_init");
        if (me_ext_init == NULL) return true;
        if (!((bool(*)(const void *, void **))me_ext_init)(
                &modengine_connector_shim, &extension_object)) return false;
        ML_LOG_INFO(L"extdll", L"invoked ModEngine compatibility initializer for external DLL %hs",
                    extdll->name);
    }
    return true;
}

static bool extdlls_cancelled(void) {
    return cancel_event != NULL &&
           WaitForSingleObject(cancel_event, 0) != WAIT_TIMEOUT;
}

static bool extdlls_has_stage(extdll_stage_t stage) {
    for (int i = 0; i < extdll_count; i++) {
        if (extdlls[i].stage == stage) return true;
    }
    return false;
}

static bool stage_is_deferred(extdll_stage_t stage) {
    return stage > EXTDLL_STAGE_NORMAL;
}

static bool extdlls_has_deferred(void) {
    for (int i = 0; i < extdll_count; i++) {
        if (stage_is_deferred(extdlls[i].stage)) return true;
    }
    return false;
}

static bool extdlls_load_stage(extdll_stage_t stage) {
    bool loaded = true;
    for (int i = 0; i < extdll_count; i++) {
        if (extdlls[i].stage != stage) continue;
        if (!load_extdll_one(i)) loaded = false;
    }
    return loaded;
}

/* Returns false when loading was cancelled. When the data-ready trigger failed
   to install the phase never happens and this blocks until unload signals the
   cancel event, so the entry and everything ordered after it stay unloaded
   instead of loading at an unverified point. */
static bool wait_for_data_ready(void) {
    HANDLE handles[2];
    if (cancel_event == NULL || data_ready_event == NULL) return false;
    handles[0] = cancel_event;
    handles[1] = data_ready_event;
    return WaitForMultipleObjects(2, handles, FALSE, INFINITE) == WAIT_OBJECT_0 + 1;
}

/* Both deferred stages share this one worker on purpose. Two concurrent workers
   would leave the relative order of a `delay` entry and a `data_ready` entry
   undefined, so an `after` dependency spanning them could load early. Walking
   every deferred entry sequentially in array order — which extdlls_prepare()
   sorted so dependencies come first — makes that order total. */
static DWORD WINAPI extdlls_load_deferred(LPVOID parameter) {
    (void)parameter;

    for (int i = 0; i < extdll_count; i++) {
        if (!stage_is_deferred(extdlls[i].stage)) continue;
        if (extdlls[i].stage == EXTDLL_STAGE_DATA_READY) {
            if (!wait_for_data_ready()) return 0;
        } else if (extdlls[i].delay_ms != 0 &&
                   WaitForSingleObject(cancel_event, extdlls[i].delay_ms) != WAIT_TIMEOUT) {
            return 0;
        }
        if (extdlls_cancelled()) return 0;
        load_extdll_one(i);
    }
    return 0;
}

static bool extdlls_ensure_events(void) {
    if (cancel_event == NULL) {
        cancel_event = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (cancel_event == NULL) {
            ML_LOG_ERROR(L"extdll", L"cannot create the external DLL cancel event");
            return false;
        }
    }
    if (data_ready_event == NULL) {
        data_ready_event = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (data_ready_event == NULL) {
            ML_LOG_ERROR(L"extdll", L"cannot create the external DLL data-ready event");
            return false;
        }
    }
    return true;
}

static bool extdlls_load(bool early) {
    bool loaded;

    if (ml_game_context_get() == NULL) {
        ML_LOG_WARN(L"extdll", L"external DLLs are disabled because the game context is unavailable");
        return false;
    }
    loaded = extdlls_load_stage(early ? EXTDLL_STAGE_EARLY : EXTDLL_STAGE_NORMAL);
    if (early) return loaded;
    if (!extdlls_has_deferred() || deferred_worker != NULL) return loaded;
    if (!extdlls_ensure_events()) return false;
    deferred_worker = CreateThread(NULL, 0, extdlls_load_deferred, NULL, 0, NULL);
    if (deferred_worker == NULL) {
        ML_LOG_ERROR(L"extdll", L"cannot create the deferred external DLL worker");
    } else {
        ML_LOG_INFO(L"extdll", L"scheduled deferred external DLL loading");
    }
    return loaded;
}

bool extdlls_load_early() {
    return extdlls_load(true);
}

void extdlls_load_all() {
    (void)extdlls_load(false);
}

/* Only releases the deferred worker, so this stays cheap enough to call from the
   game step thread that reported data readiness. */
void extdlls_load_data_ready() {
    if (data_ready_event == NULL) return;
    ML_LOG_INFO(L"extdll", L"data readiness released the deferred external DLL worker");
    SetEvent(data_ready_event);
}

void extdlls_unload_all() {
    if (cancel_event != NULL) SetEvent(cancel_event);
    if (deferred_worker != NULL) {
        WaitForSingleObject(deferred_worker, INFINITE);
        CloseHandle(deferred_worker);
        deferred_worker = NULL;
    }
    if (cancel_event != NULL) {
        CloseHandle(cancel_event);
        cancel_event = NULL;
    }
    if (data_ready_event != NULL) {
        CloseHandle(data_ready_event);
        data_ready_event = NULL;
    }
    for (;;) {
        extdll_t *extdll = NULL;
        for (int i = 0; i < extdll_count; i++) {
            if (extdlls[i].dll_module != NULL &&
                (extdll == NULL || extdlls[i].load_order > extdll->load_order)) {
                extdll = &extdlls[i];
            }
        }
        if (extdll == NULL) break;
        if (extdll->dll_module) {
            ML_LOG_INFO(L"extdll", L"uninitializing external DLL %hs", extdll->name);
            FreeLibrary(extdll->dll_module);
            extdll->dll_module = NULL;
            extdll->load_order = 0;
            ML_LOG_INFO(L"extdll", L"uninitialized external DLL %hs", extdll->name);
        }
    }
    for (int i = extdll_count - 1; i >= 0; i--) {
        extdll_t *extdll = &extdlls[i];
        for (int j = 0; j < extdll->after_count; j++) ml_mem_free(extdll->after[j]);
        ml_mem_free(extdll->after);
        ml_mem_free(extdll->name);
        ml_mem_free(extdll->base_path);
        memset(extdll, 0, sizeof(*extdll));
    }
    ml_mem_free(extdlls);
    extdlls = NULL;
    extdll_count = 0;
    extdll_capacity = 0;
    InterlockedExchange(&load_counter, 0);
}

#ifdef ML_EXTDLL_TEST
const char *extdlls_test_name_at(int index) {
    return index >= 0 && index < extdll_count ? extdlls[index].name : NULL;
}

int extdlls_test_stage_at(int index) {
    return index >= 0 && index < extdll_count ? (int)extdlls[index].stage : -1;
}

bool extdlls_test_is_early_at(int index) {
    return extdlls_test_stage_at(index) == (int)EXTDLL_STAGE_EARLY;
}

bool extdlls_test_is_effective_early_at(int index) {
    return extdlls_test_is_early_at(index);
}

bool extdlls_test_is_data_ready_at(int index) {
    return extdlls_test_stage_at(index) == (int)EXTDLL_STAGE_DATA_READY;
}

/* Rides the deferred worker without a wait of its own. */
bool extdlls_test_is_deferred_at(int index) {
    return extdlls_test_stage_at(index) == (int)EXTDLL_STAGE_DELAY &&
           extdlls[index].delay_ms == 0;
}

bool extdlls_test_has_delayed_at(bool early) {
    /* The early stage runs before any worker exists, so it never defers. */
    return early ? false : extdlls_has_stage(EXTDLL_STAGE_DELAY);
}

uint32_t extdlls_test_delay_at(int index) {
    return index >= 0 && index < extdll_count &&
           extdlls[index].stage == EXTDLL_STAGE_DELAY
        ? extdlls[index].delay_ms : 0;
}

int extdlls_test_after_count(int index) {
    return index >= 0 && index < extdll_count ? extdlls[index].after_count : 0;
}

const char *extdlls_test_after_at(int index, int dependency) {
    if (index < 0 || index >= extdll_count || dependency < 0 || dependency >= extdlls[index].after_count) return NULL;
    return extdlls[index].after[dependency];
}

void extdlls_test_load_at(int index) {
    if (index >= 0 && index < extdll_count) load_extdll_one(index);
}

#endif
