#include "nightreign.h"
#include "log.h"

#include "asset_hooks.h"
#include "common.h"
#include "runtime_ready.h"

#include "modloader/lifecycle.h"

#include "game/game.h"
#include "process/image.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

static void *image_base;
static size_t image_size;

static BOOL CALLBACK install_after_runtime(PINIT_ONCE once, PVOID parameter, PVOID *context) {
    const ml_game_descriptor_t *game = ml_game_context_get();
    bool wwise_requested = common_wwise_requested();
    bool assets_requested = game != NULL && ml_asset_hooks_requested();
    bool assets_applied = !assets_requested;
    bool wwise_applied;
    (void)once;
    (void)parameter;
    (void)context;

    if (!ml_asset_hooks_install_game_data_ready(game)) {
        ML_LOG_WARN(L"nightreign", L"AFTER_GAME_DATA_READY trigger HOOK_FAILED; CPU affinity deferred capability disabled");
    }
    if (!ml_lifecycle_advance(ML_LIFECYCLE_PHASE_AFTER_RUNTIME_INIT)) {
        ML_LOG_WARN(L"nightreign", L"AFTER_RUNTIME_INIT lifecycle advance failed");
    }
    if (assets_requested) assets_applied = ml_asset_hooks_install(game, image_base, image_size);
    ml_log_write(assets_applied ? ML_LOG_LEVEL_INFO : ML_LOG_LEVEL_WARN,
                 L"nightreign", assets_requested
                     ? (assets_applied
                         ? L"AFTER_RUNTIME_INIT reached; asset capability APPLIED"
                         : L"AFTER_RUNTIME_INIT reached; asset capability HOOK_FAILED")
                     : L"AFTER_RUNTIME_INIT reached; asset capability NOT_REQUESTED");
    wwise_applied = common_install_wwise();
    ml_log_write(wwise_applied ? ML_LOG_LEVEL_INFO : ML_LOG_LEVEL_WARN,
                 L"nightreign", wwise_applied
                     ? (wwise_requested ? L"Wwise capability APPLIED" : L"Wwise capability NOT_REQUESTED")
                     : L"Wwise capability HOOK_FAILED");
    return assets_applied && wwise_applied;
}

bool nightreign_install(void) {
    const ml_game_descriptor_t *game = ml_game_context_get();
    if (game == NULL || game->id != ML_GAME_NIGHTREIGN ||
        game->runtime_ready_trigger != ML_RUNTIME_READY_STEAM_API_INIT) return false;

    image_base = get_module_image_base(NULL, &image_size);
    return image_base != NULL && image_size != 0 &&
           ml_runtime_ready_hook_install(L"nightreign", install_after_runtime);
}

void nightreign_uninstall(void) {
    if (!ml_asset_hooks_uninstall()) {
        ML_LOG_WARN(L"nightreign", L"one or more asset hooks could not be removed");
    }
    if (ml_runtime_ready_hook_uninstall(L"nightreign")) {
        image_base = NULL;
        image_size = 0;
    }
}
