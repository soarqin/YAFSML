/*
 * Copyright (C) 2024, Soar Qin<soarchin@gmail.com>

 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#include "gamehook.h"

#include "steam/api.h"
#include "config.h"
#include "patches/common.h"
#include "patches/allocator.h"
#include "patches/darksouls3.h"
#include "patches/eldenring.h"
#include "patches/logo.h"
#include "patches/nightreign.h"
#include "patches/properties.h"
#include "patches/regulation.h"
#include "patches/sekiro.h"
#include "mimalloc_allocator.h"
#include "lifecycle.h"
#include "log.h"

#include "game/game.h"

#include <MinHook.h>

static void install_logo_after_runtime(ml_lifecycle_phase_t phase, void *userp) {
    (void)phase;
    ml_logo_skip_install((const ml_game_descriptor_t *)userp);
}

static void install_properties_after_runtime(ml_lifecycle_phase_t phase, void *userp) {
    const ml_game_descriptor_t *game = (const ml_game_descriptor_t *)userp;
    bool applied;
    (void)phase;
    applied = ml_properties_install(game);
    ml_log_write(applied ? ML_LOG_LEVEL_INFO : ML_LOG_LEVEL_WARN,
                 L"properties", applied
                     ? L"property initialization hook APPLIED for %ls"
                     : L"property initialization hook HOOK_FAILED for %ls",
                 game == NULL ? L"<unknown>" : game->title);
}

static void install_regulation_after_runtime(ml_lifecycle_phase_t phase, void *userp) {
    (void)phase;
    (void)ml_regulation_install((const ml_game_descriptor_t *)userp);
}

static void install_allocator_after_runtime(ml_lifecycle_phase_t phase, void *userp) {
    (void)phase;
    (void)ml_allocator_install_after_runtime((const ml_game_descriptor_t *)userp);
}

static void install_allocator_before_main(ml_lifecycle_phase_t phase, void *userp) {
    const ml_game_descriptor_t *game = (const ml_game_descriptor_t *)userp;
    bool applied;
    (void)phase;
    applied = ml_allocator_install_before_main(game);
    if (!applied && game != NULL && game->id == ML_GAME_DARK_SOULS_3) {
        ML_LOG_WARN(L"darksouls3", L"heap allocator capability HOOK_FAILED stage=system_allocator");
    }
    if (applied && !ml_lifecycle_on_phase(ML_LIFECYCLE_PHASE_AFTER_RUNTIME_INIT,
                                          install_allocator_after_runtime, userp)) {
        ML_LOG_WARN(L"allocator", L"heap allocator capability HOOK_FAILED: could not schedule after-runtime stage");
    }
}

bool gamehook_install() {
    bool schedule_heap_allocator = false;
    if (!ml_game_context_init()) {
        ML_LOG_WARN(L"gamehook", L"unsupported or mismatched game process; game hooks are disabled");
        return false;
    }
    const ml_game_descriptor_t *game = ml_game_context_get();
    bool common_applied;
    bool adapter_applied;
    if (config.patch_mem && game->allocator_strategy != ML_ALLOCATOR_STRATEGY_UNSUPPORTED) {
        if (config.patch_mem_dedicated_heap &&
            !mimalloc_dl_allocator_prepare(config.patch_mem_heap_size)) {
            ML_LOG_WARN(L"allocator", L"dedicated mimalloc heap unavailable; using the default mimalloc heap");
        }
        schedule_heap_allocator = true;
    } else if (!config.patch_mem) {
        ML_LOG_INFO(L"allocator", L"heap allocators SKIPPED_DISABLED for %ls", game->title);
    } else {
        ML_LOG_INFO(L"allocator", L"heap allocators SKIPPED_UNSUPPORTED for %ls", game->title);
    }
    if (game->id != ML_GAME_ELDEN_RING && game->id != ML_GAME_NIGHTREIGN &&
        game->id != ML_GAME_SEKIRO &&
        game->id != ML_GAME_DARK_SOULS_3) {
        ML_LOG_WARN(L"gamehook", L"%ls adapter is not implemented; game hooks are disabled", game->title);
        return false;
    }
    common_apply_process_settings();
    /* me3 schedules the logo redirect before other after-runtime host work. */
    if (config.skip_intro &&
        !ml_lifecycle_on_phase(ML_LIFECYCLE_PHASE_AFTER_RUNTIME_INIT, install_logo_after_runtime, (void *)game)) {
        ML_LOG_WARN(L"logo", L"could not schedule Logo redirect");
    }
    if (schedule_heap_allocator &&
        !ml_lifecycle_on_phase(ML_LIFECYCLE_PHASE_BEFORE_MAIN,
                               install_allocator_before_main, (void *)game)) {
        ML_LOG_WARN(L"allocator", L"heap allocator capability HOOK_FAILED: could not schedule before-main stage");
    }
    if (!ml_lifecycle_on_phase(ML_LIFECYCLE_PHASE_AFTER_RUNTIME_INIT, install_properties_after_runtime, (void *)game)) {
        ML_LOG_WARN(L"properties", L"could not schedule installation");
    }
    if (config.prevent_regulation_save_write &&
        !ml_lifecycle_on_phase(ML_LIFECYCLE_PHASE_AFTER_RUNTIME_INIT, install_regulation_after_runtime, (void *)game)) {
        ML_LOG_WARN(L"regulation", L"could not schedule installation");
    }
    steamapi_init();
    common_applied = common_install_file_routing(game) && common_install_ime() &&
                     (game->id == ML_GAME_NIGHTREIGN || common_install_wwise());
    adapter_applied = game->id == ML_GAME_ELDEN_RING
        ? eldenring_install()
        : game->id == ML_GAME_NIGHTREIGN
            ? nightreign_install()
            : game->id == ML_GAME_SEKIRO ? sekiro_install() : darksouls3_install();
    return common_applied && adapter_applied;
}

void gamehook_uninstall() {
    const ml_game_descriptor_t *game = ml_game_context_get();
    if (game != NULL && game->id == ML_GAME_ELDEN_RING) {
        eldenring_uninstall();
    } else if (game != NULL && game->id == ML_GAME_NIGHTREIGN) {
        nightreign_uninstall();
    } else if (game != NULL && game->id == ML_GAME_SEKIRO) {
        sekiro_uninstall();
    } else if (game != NULL && game->id == ML_GAME_DARK_SOULS_3) {
        darksouls3_uninstall();
    }
    common_uninstall();
    common_uninstall_file_routing();

    MH_Uninitialize();
    steamapi_uninit();
}
