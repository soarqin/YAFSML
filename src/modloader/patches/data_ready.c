/*
 * Copyright (C) 2026, Soar Qin<soarchin@gmail.com>
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#include "data_ready.h"

#include "log.h"

#include "modloader/hook.h"
#include "modloader/lifecycle.h"

#include "process/fd4_step.h"
#include "process/image.h"
#include "process/pe.h"
#include "process/rtti.h"
#include "process/sprj_step.h"

#include <MinHook.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string.h>

/* Legacy `NS_SPRJ::Step<T>` step functions are called with the object in rcx,
   the frame time in xmm1 and an opaque argument in r8. Under the MSVC x64
   convention those are argument slots 0, 1 and 2, so this declaration matches
   without touching rdx, which the game never sets for these calls. */
typedef void (*sprj_step_fn_t)(void *this_ptr, float time, void *argument);

static void *hook_target;
static const wchar_t *trigger_label;
static SRWLOCK hook_lock = SRWLOCK_INIT;
static volatile LONG hook_shutting_down;
static volatile LONG hook_active;
static volatile LONG phase_reached;

static fd4_step_fn_t old_named_step;
static sprj_step_fn_t old_sprj_step;
static size_t sprj_index_offset;

static void advance_data_ready(void) {
    if (InterlockedCompareExchange(&phase_reached, 1, 0) != 0) return;
    ML_LOG_INFO(L"data-ready", L"AFTER_DATA_READY reached at %ls",
                trigger_label == NULL ? L"<unknown>" : trigger_label);
    if (!ml_lifecycle_advance(ML_LIFECYCLE_PHASE_AFTER_DATA_READY)) {
        ML_LOG_WARN(L"data-ready", L"AFTER_DATA_READY lifecycle advance failed");
    }
}

/* `ParamStep::STEP_Wait` only becomes the active step after
   `ParamStep::STEP_LoadWait` saw the regulation manager finish and ran its
   post-load fixups, so the first call already means every param is in place. */
static void __cdecl named_step_hooked(void *this_ptr, fd4_time_t *time) {
    fd4_step_fn_t original;
    InterlockedIncrement(&hook_active);
    AcquireSRWLockShared(&hook_lock);
    original = old_named_step;
    if (original != NULL) original(this_ptr, time);
    /* This step runs every frame for the rest of the session; check the latch
       before touching the lifecycle registry. */
    if (InterlockedCompareExchange(&phase_reached, 0, 0) == 0 &&
        InterlockedCompareExchange(&hook_shutting_down, 0, 0) == 0) {
        advance_data_ready();
    }
    ReleaseSRWLockShared(&hook_lock);
    InterlockedDecrement(&hook_active);
}

/* The legacy param step polls its load handles and the regulation manager, then
   drives its own step index negative to finish the step machine. That single
   transition is the point where every param is loaded. */
static void sprj_step_hooked(void *this_ptr, float time, void *argument) {
    sprj_step_fn_t original;
    int32_t before = -1;
    int32_t after = -1;
    bool track;
    InterlockedIncrement(&hook_active);
    AcquireSRWLockShared(&hook_lock);
    original = old_sprj_step;
    track = this_ptr != NULL && sprj_index_offset != 0 &&
            InterlockedCompareExchange(&phase_reached, 0, 0) == 0;
    if (track) memcpy(&before, (const char *)this_ptr + sprj_index_offset, sizeof(before));
    if (original != NULL) original(this_ptr, time, argument);
    if (track && InterlockedCompareExchange(&hook_shutting_down, 0, 0) == 0) {
        memcpy(&after, (const char *)this_ptr + sprj_index_offset, sizeof(after));
        if (before >= 0 && after < 0) advance_data_ready();
    }
    ReleaseSRWLockShared(&hook_lock);
    InterlockedDecrement(&hook_active);
}

static bool pin_own_module(void *address) {
    HMODULE module;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_PIN,
                           (LPCWSTR)(uintptr_t)address, &module)) return true;
    ML_LOG_WARN(L"data-ready", L"could not pin the data-ready hook module");
    return false;
}

static bool install_named_step(const ml_game_descriptor_t *game) {
    void *step;
    ml_hook_result_t result;

    if (game->data_ready_step_name == NULL) return false;
    step = fd4_step_find(game->data_ready_step_name);
    if (step == NULL) {
        ML_LOG_WARN(L"data-ready", L"%ls SIGNATURE_NOT_FOUND",
                    game->data_ready_step_name);
        return false;
    }
    if (!pin_own_module((void *)(uintptr_t)named_step_hooked)) return false;
    trigger_label = game->data_ready_step_name;
    result = ml_hook_install(step, named_step_hooked, (void **)&old_named_step);
    if (result != ML_HOOK_APPLIED) {
        ML_LOG_WARN(L"data-ready", L"%ls hook %hs",
                    game->data_ready_step_name, ml_hook_result_name(result));
        trigger_label = NULL;
        return false;
    }
    hook_target = step;
    ML_LOG_INFO(L"data-ready", L"%ls hook APPLIED at %p",
                game->data_ready_step_name, step);
    return true;
}

static bool install_sprj_param_step(const ml_game_descriptor_t *game) {
    void *image_base;
    size_t image_size = 0;
    const IMAGE_SECTION_HEADER *text_section;
    const uint8_t *text;
    size_t text_size = 0;
    /* Every build observed so far keeps the step table in `.data`, matching the
       constraint fd4_step_static.c puts on FD4 step slots. `.rdata` is accepted
       too so that a build emitting the table as read-only data still resolves. */
    static const char *const table_section_names[] = { ".data", ".rdata" };
    sprj_step_range_t table_ranges[sizeof(table_section_names) / sizeof(table_section_names[0])];
    size_t table_range_count = 0;
    size_t vtable_count;
    sprj_step_table_t table;
    ml_hook_result_t result;
    bool resolved = false;

    if (game->data_ready_class == NULL) return false;
    image_base = get_module_image_base(NULL, &image_size);
    if (image_base == NULL || image_size == 0) return false;
    text_section = pe_section_by_name(image_base, ".text");
    if (text_section == NULL) return false;
    text = (const uint8_t *)pe_section_data(image_base, text_section, &text_size);
    if (text == NULL || text_size == 0) return false;
    for (size_t i = 0; i < sizeof(table_section_names) / sizeof(table_section_names[0]); i++) {
        const IMAGE_SECTION_HEADER *section =
            pe_section_by_name(image_base, table_section_names[i]);
        size_t size = 0;
        const uint8_t *base = section == NULL
            ? NULL : (const uint8_t *)pe_section_data(image_base, section, &size);
        if (base == NULL || size < 2 * sizeof(void *)) continue;
        table_ranges[table_range_count].base = base;
        table_ranges[table_range_count].size = size;
        table_range_count++;
    }
    if (table_range_count == 0) return false;

    vtable_count = rtti_vtable_count(game->data_ready_class);
    if (vtable_count == 0) {
        ML_LOG_WARN(L"data-ready", L"%hs RTTI vtable SIGNATURE_NOT_FOUND",
                    game->data_ready_class);
        return false;
    }
    for (size_t i = 0; i < vtable_count && !resolved; i++) {
        void *vtable = rtti_find_vtable_at(game->data_ready_class, i);
        if (vtable == NULL) continue;
        resolved = sprj_step_find_from_vtable(text, text_size, vtable, table_ranges,
                                             table_range_count, &table);
    }
    if (!resolved) {
        ML_LOG_WARN(L"data-ready", L"%hs step table could not be derived from its constructor",
                    game->data_ready_class);
        return false;
    }
    if (!pin_own_module((void *)(uintptr_t)sprj_step_hooked)) return false;
    sprj_index_offset = table.index_offset;
    trigger_label = L"ParamStep wait step";
    result = ml_hook_install(table.second_step, sprj_step_hooked, (void **)&old_sprj_step);
    if (result != ML_HOOK_APPLIED) {
        ML_LOG_WARN(L"data-ready", L"%hs wait step hook %hs",
                    game->data_ready_class, ml_hook_result_name(result));
        sprj_index_offset = 0;
        trigger_label = NULL;
        return false;
    }
    hook_target = table.second_step;
    ML_LOG_INFO(L"data-ready",
                L"%hs wait step hook APPLIED at %p (table %p, step index offset 0x%llx)",
                game->data_ready_class, table.second_step, (void *)table.table,
                (unsigned long long)table.index_offset);
    return true;
}

bool ml_data_ready_install(const ml_game_descriptor_t *game) {
    if (hook_target != NULL) return true;
    if (game == NULL) return false;
    switch (game->data_ready_strategy) {
        case ML_DATA_READY_FD4_NAMED_STEP:
            return install_named_step(game);
        case ML_DATA_READY_SPRJ_PARAM_STEP:
            return install_sprj_param_step(game);
        case ML_DATA_READY_UNSUPPORTED:
        default:
            ML_LOG_INFO(L"data-ready", L"data ready trigger NOT_SUPPORTED for %ls",
                        game->title);
            return false;
    }
}

bool ml_data_ready_uninstall(void) {
    MH_STATUS status;
    bool result = true;
    void *target = hook_target;

    if (target != NULL) {
        InterlockedExchange(&hook_shutting_down, 1);
        AcquireSRWLockExclusive(&hook_lock);
        status = MH_DisableHook(target);
        if (status != MH_OK && status != MH_ERROR_DISABLED && status != MH_ERROR_NOT_CREATED) {
            ML_LOG_WARN(L"data-ready", L"failed to disable hook at %p: %d", target, status);
            result = false;
        }
        ReleaseSRWLockExclusive(&hook_lock);
        while (InterlockedCompareExchange(&hook_active, 0, 0) != 0) Sleep(0);
        status = MH_RemoveHook(target);
        if (status != MH_OK && status != MH_ERROR_NOT_CREATED) {
            ML_LOG_WARN(L"data-ready", L"failed to remove hook at %p: %d", target, status);
            result = false;
        }
    }
    hook_target = NULL;
    old_named_step = NULL;
    old_sprj_step = NULL;
    sprj_index_offset = 0;
    trigger_label = NULL;
    InterlockedExchange(&hook_shutting_down, 0);
    InterlockedExchange(&phase_reached, 0);
    return result;
}
