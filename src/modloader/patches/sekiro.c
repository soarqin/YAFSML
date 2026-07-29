#include "sekiro.h"
#include "log.h"

#include "asset_hooks.h"
#include "runtime_ready.h"

#include "modloader/lifecycle.h"
#include "modloader/mod.h"

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
    bool assets_requested = game != NULL && mods_count() > 0;
    bool assets_applied = !assets_requested;
    (void)once;
    (void)parameter;
    (void)context;

    if (assets_requested) {
        assets_applied = ml_asset_hooks_install(game, image_base, image_size);
    }
    if (!ml_asset_hooks_install_render_ready(game)) {
        ML_LOG_WARN(L"sekiro", L"AFTER_RENDER_READY trigger HOOK_FAILED; CPU affinity deferred capability disabled");
    }
    if (assets_requested) {
        ml_log_write(assets_applied ? ML_LOG_LEVEL_INFO : ML_LOG_LEVEL_WARN,
                     L"sekiro", assets_applied
                         ? L"AFTER_RUNTIME_INIT reached; asset capability APPLIED"
                         : L"AFTER_RUNTIME_INIT reached; asset capability HOOK_FAILED");
    } else {
        ML_LOG_INFO(L"sekiro", L"AFTER_RUNTIME_INIT reached; asset capability NOT_REQUESTED");
    }
    if (!ml_lifecycle_advance(ML_LIFECYCLE_PHASE_AFTER_RUNTIME_INIT)) {
        ML_LOG_WARN(L"sekiro", L"AFTER_RUNTIME_INIT lifecycle advance failed");
    }
    return assets_applied ? TRUE : FALSE;
}

bool sekiro_install(void) {
    const ml_game_descriptor_t *game = ml_game_context_get();
    if (game == NULL || game->id != ML_GAME_SEKIRO ||
        game->runtime_ready_trigger != ML_RUNTIME_READY_STEAM_API_INIT) return false;

    image_base = get_module_image_base(NULL, &image_size);
    return image_base != NULL && image_size != 0 &&
           ml_runtime_ready_hook_install(L"sekiro", install_after_runtime);
}

void sekiro_uninstall(void) {
    if (!ml_asset_hooks_uninstall()) {
        ML_LOG_WARN(L"sekiro", L"one or more asset hooks could not be removed");
    }
    if (ml_runtime_ready_hook_uninstall(L"sekiro")) {
        image_base = NULL;
        image_size = 0;
    }
}
