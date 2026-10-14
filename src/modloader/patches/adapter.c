/*
 * Copyright (C) 2026, Soar Qin<soarchin@gmail.com>
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#include "adapter.h"

#include "asset_hooks.h"
#include "common.h"
#include "darksouls3.h"
#include "eldenring.h"
#include "runtime_ready.h"

#include "log.h"

#include "modloader/lifecycle.h"

#include "process/image.h"

static const ml_game_descriptor_t *adapter_game;
static void *image_base;
static size_t image_size;
static bool assets_requested;

/* Runs after SteamAPI_Init succeeded. As in me3, the AFTER_RUNTIME_INIT consumers
 * (heap allocator, properties, regulation, external DLLs) run before the asset
 * hooks are installed. Returning false retries on the next SteamAPI_Init. */
static bool install_after_runtime(void) {
    const ml_game_descriptor_t *game = adapter_game;
    bool assets_applied = !assets_requested;
    bool wwise_applied = true;
    if (!ml_asset_hooks_install_render_ready(game)) {
        ML_LOG_WARN(game->log_scope, L"AFTER_RENDER_READY trigger HOOK_FAILED; CPU Set deferred capability disabled");
    }
    ml_lifecycle_advance(ML_LIFECYCLE_PHASE_AFTER_RUNTIME_INIT);
    if (assets_requested) assets_applied = ml_asset_hooks_install(game, image_base, image_size);
    ml_log_write(assets_applied ? ML_LOG_LEVEL_INFO : ML_LOG_LEVEL_WARN,
                 game->log_scope, assets_requested
                     ? (assets_applied
                         ? L"AFTER_RUNTIME_INIT reached; asset capability APPLIED"
                         : L"AFTER_RUNTIME_INIT reached; asset capability HOOK_FAILED")
                     : L"AFTER_RUNTIME_INIT reached; asset capability NOT_REQUESTED");
    if (game->wwise_strategy == ML_WWISE_AFTER_RUNTIME) {
        const bool wwise_requested = common_wwise_requested();
        wwise_applied = common_install_wwise();
        ml_log_write(wwise_applied ? ML_LOG_LEVEL_INFO : ML_LOG_LEVEL_WARN,
                     game->log_scope, wwise_applied
                         ? (wwise_requested ? L"Wwise capability APPLIED" : L"Wwise capability NOT_REQUESTED")
                         : L"Wwise capability HOOK_FAILED");
    }
    return assets_applied && wwise_applied;
}

bool ml_adapter_install(const ml_game_descriptor_t *game) {
    bool result = true;
    if (game == NULL || game->runtime_ready_trigger != ML_RUNTIME_READY_STEAM_API_INIT) return false;
    adapter_game = game;
    assets_requested = ml_asset_hooks_requested();
    if (game->id == ML_GAME_DARK_SOULS_3) result = darksouls3_prepare(game, assets_requested);
    image_base = get_module_image_base(NULL, &image_size);
    if (image_base == NULL || image_size == 0 ||
        !ml_runtime_ready_hook_install(game->log_scope, install_after_runtime)) {
        ML_LOG_WARN(game->log_scope, L"runtime-ready trigger HOOK_FAILED; deferred capabilities disabled");
        return false;
    }
    /* Without mods the archive resolver hook would only slow down loading. */
    if (game->id == ML_GAME_ELDEN_RING && assets_requested &&
        !eldenring_archive_resolver_install(image_base, image_size)) result = false;
    return result;
}

void ml_adapter_uninstall(void) {
    const ml_game_descriptor_t *game = adapter_game;
    if (game == NULL) return;
    if (!ml_asset_hooks_uninstall()) {
        ML_LOG_WARN(game->log_scope, L"one or more asset hooks could not be removed");
    }
    if (game->id == ML_GAME_ELDEN_RING) eldenring_archive_resolver_uninstall();
    if (ml_runtime_ready_hook_uninstall(game->log_scope)) {
        image_base = NULL;
        image_size = 0;
        adapter_game = NULL;
    }
}
