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

#define EXTDLL_LOAD_AFTER_DELAY INT64_MIN
#define EXTDLL_LOAD_EARLY -1

typedef struct extdll_t {
    char *name;
    wchar_t *base_path;
    int64_t load_condition;
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
static HANDLE delayed_worker;
static HANDLE delayed_cancel_event;

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

static bool parse_delay(const char *value, int64_t *delay_ms) {
    char *end;
    unsigned long parsed;
    if (value == NULL || value[0] < '0' || value[0] > '9') return false;
    errno = 0;
    parsed = strtoul(value, &end, 10);
    if (errno == ERANGE || end == value || *end != '\0' || parsed > UINT32_MAX) return false;
    *delay_ms = (int64_t)parsed;
    return true;
}

void extdlls_add(const char *name, const wchar_t *path) {
    extdll_t *extdll;
    if (name == NULL || path == NULL) return;
    if (extdll_count >= extdll_capacity &&
        !extdlls_reserve(extdll_capacity == 0 ? 8 : extdll_capacity * 2)) return;
    extdll = &extdlls[extdll_count++];
    memset(extdll, 0, sizeof(*extdll));
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
            if (extdll->load_condition > 0) {
                ML_LOG_ERROR(L"extdll", L"early and delay conditions cannot be combined for %hs", name);
            } else {
                extdll->load_condition = EXTDLL_LOAD_EARLY;
            }
        } else if (strncmp(condition, "delay,", 6) == 0) {
            int64_t delay_ms;
            if (!parse_delay(condition + 6, &delay_ms)) {
                ML_LOG_ERROR(L"extdll", L"invalid delay condition for %hs: %hs", name, condition);
            } else if (extdll->load_condition < 0) {
                ML_LOG_ERROR(L"extdll", L"early and delay conditions cannot be combined for %hs", name);
            } else {
                extdll->load_condition = delay_ms;
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
            if (extdlls[i].load_condition >= 0) continue;
            for (int j = 0; j < extdlls[i].after_count; j++) {
                int dependency = extdll_index_by_name(extdlls[i].after[j]);
                if (dependency >= 0 && extdlls[dependency].load_condition == 0) {
                    extdlls[dependency].load_condition = EXTDLL_LOAD_EARLY;
                    changed = true;
                }
            }
        }
        if (!changed) break;
    }
    for (int i = 0; i < extdll_count; i++) {
        for (int j = 0; j < extdlls[i].after_count; j++) {
            int dependency = extdll_index_by_name(extdlls[i].after[j]);
            if (dependency < 0) continue;
            if (extdlls[dependency].load_condition > 0 ||
                extdlls[dependency].load_condition == EXTDLL_LOAD_AFTER_DELAY) {
                if (extdlls[i].load_condition == EXTDLL_LOAD_EARLY) {
                    ML_LOG_ERROR(L"extdll", L"early DLL %hs depends on delayed DLL %hs; loading after the delay instead",
                                 extdlls[i].name, extdlls[dependency].name);
                    extdlls[i].load_condition = 0;
                }
                if (extdlls[i].load_condition == 0) {
                    extdlls[i].load_condition = EXTDLL_LOAD_AFTER_DELAY;
                }
                break;
            }
        }
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

static DWORD WINAPI extdlls_load_delayed(LPVOID parameter) {
    (void)parameter;

    for (int i = 0; i < extdll_count; i++) {
        if (extdlls[i].load_condition <= 0 &&
            extdlls[i].load_condition != EXTDLL_LOAD_AFTER_DELAY) continue;
        if (extdlls[i].load_condition > 0 &&
            WaitForSingleObject(delayed_cancel_event, (DWORD)extdlls[i].load_condition) != WAIT_TIMEOUT) {
            return 0;
        }
        if (WaitForSingleObject(delayed_cancel_event, 0) != WAIT_TIMEOUT) return 0;
        load_extdll_one(i);
    }
    return 0;
}

static bool extdlls_load(bool early) {
    bool has_delayed = false;
    bool loaded = true;

    if (ml_game_context_get() == NULL) {
        ML_LOG_WARN(L"extdll", L"external DLLs are disabled because the game context is unavailable");
        return false;
    }
    for (int i = 0; i < extdll_count; i++) {
        if ((extdlls[i].load_condition < 0) != early ||
            extdlls[i].load_condition > 0 ||
            extdlls[i].load_condition == EXTDLL_LOAD_AFTER_DELAY) continue;
        if (!load_extdll_one(i)) loaded = false;
    }
    if (early) return loaded;
    for (int i = 0; i < extdll_count; i++) {
        if (extdlls[i].load_condition <= 0 &&
            extdlls[i].load_condition != EXTDLL_LOAD_AFTER_DELAY) continue;
        has_delayed = true;
        break;
    }
    if (!has_delayed || delayed_worker != NULL) return loaded;
    if (delayed_cancel_event == NULL) {
        delayed_cancel_event = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (delayed_cancel_event == NULL) {
            ML_LOG_ERROR(L"extdll", L"cannot create delayed external DLL worker event");
            return false;
        }
    }
    delayed_worker = CreateThread(NULL, 0, extdlls_load_delayed, NULL, 0, NULL);
    if (delayed_worker == NULL) {
        ML_LOG_ERROR(L"extdll", L"cannot create delayed external DLL worker");
    } else {
        ML_LOG_INFO(L"extdll", L"scheduled delayed external DLL loading");
    }
    return loaded;
}

bool extdlls_load_early() {
    return extdlls_load(true);
}

void extdlls_load_all() {
    (void)extdlls_load(false);
}

void extdlls_unload_all() {
    if (delayed_cancel_event != NULL) SetEvent(delayed_cancel_event);
    if (delayed_worker != NULL) {
        WaitForSingleObject(delayed_worker, INFINITE);
        CloseHandle(delayed_worker);
        delayed_worker = NULL;
    }
    if (delayed_cancel_event != NULL) {
        CloseHandle(delayed_cancel_event);
        delayed_cancel_event = NULL;
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

bool extdlls_test_is_early_at(int index) {
    return index >= 0 && index < extdll_count
        ? extdlls[index].load_condition == EXTDLL_LOAD_EARLY : false;
}

bool extdlls_test_is_effective_early_at(int index) {
    return extdlls_test_is_early_at(index);
}

bool extdlls_test_is_deferred_at(int index) {
    return index >= 0 && index < extdll_count
        ? extdlls[index].load_condition == EXTDLL_LOAD_AFTER_DELAY : false;
}

bool extdlls_test_has_delayed_at(bool early) {
    for (int i = 0; i < extdll_count; i++) {
        if ((extdlls[i].load_condition == EXTDLL_LOAD_EARLY) == early &&
            (extdlls[i].load_condition > 0 ||
             extdlls[i].load_condition == EXTDLL_LOAD_AFTER_DELAY)) return true;
    }
    return false;
}

uint32_t extdlls_test_delay_at(int index) {
    return index >= 0 && index < extdll_count && extdlls[index].load_condition > 0
        ? (uint32_t)extdlls[index].load_condition : 0;
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
