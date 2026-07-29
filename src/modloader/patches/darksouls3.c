#include "darksouls3.h"
#include "log.h"

#include "asset_hooks.h"
#include "common.h"
#include "properties.h"
#include "runtime_ready.h"

#include "modloader/config.h"
#include "modloader/lifecycle.h"

#include "game/game.h"
#include "process/image.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

static void *image_base;
static size_t image_size;
static volatile LONG runtime_ready_reached;
static bool assets_requested_at_start;

static BOOL CALLBACK install_after_runtime(PINIT_ONCE once, PVOID parameter, PVOID *context) {
    const ml_game_descriptor_t *game = ml_game_context_get();
    bool wwise_requested = common_wwise_requested();
    bool assets_requested = assets_requested_at_start;
    bool assets_applied = !assets_requested;
    bool wwise_applied;
    (void)once;
    (void)parameter;
    (void)context;

    if (!ml_asset_hooks_install_render_ready(game)) {
        ML_LOG_WARN(L"darksouls3", L"AFTER_RENDER_READY trigger HOOK_FAILED; CPU affinity deferred capability disabled");
    }
    if (!ml_lifecycle_advance(ML_LIFECYCLE_PHASE_AFTER_RUNTIME_INIT)) {
        ML_LOG_WARN(L"darksouls3", L"AFTER_RUNTIME_INIT lifecycle advance failed");
    }
    if (assets_requested) assets_applied = ml_asset_hooks_install(game, image_base, image_size);
    ml_log_write(assets_applied ? ML_LOG_LEVEL_INFO : ML_LOG_LEVEL_WARN,
                 L"darksouls3", assets_requested
                     ? (assets_applied
                         ? L"AFTER_RUNTIME_INIT reached; asset initialization hook APPLIED"
                         : L"AFTER_RUNTIME_INIT reached; asset initialization hook HOOK_FAILED")
                     : L"AFTER_RUNTIME_INIT reached; asset capability NOT_REQUESTED");
    wwise_applied = common_install_wwise();
    ml_log_write(wwise_applied ? ML_LOG_LEVEL_INFO : ML_LOG_LEVEL_WARN,
                 L"darksouls3", wwise_applied
                     ? (wwise_requested ? L"Wwise capability APPLIED" : L"Wwise capability NOT_REQUESTED")
                     : L"Wwise capability HOOK_FAILED");
    InterlockedExchange(&runtime_ready_reached, 1);
    return TRUE;
}

bool darksouls3_install(void) {
    const ml_game_descriptor_t *game = ml_game_context_get();
    bool result = true;
    if (game == NULL || game->id != ML_GAME_DARK_SOULS_3 ||
        game->support_level != ML_SUPPORT_EXPERIMENTAL ||
        game->runtime_ready_trigger != ML_RUNTIME_READY_STEAM_API_INIT) return false;

    ML_LOG_WARN(L"darksouls3", L"experimental adapter enabled; Arxan neutralization is forced on");
    if (ml_asset_hooks_loose_params_present(game)) {
        if (ml_properties_set_loose_params(true)) {
            ML_LOG_INFO(L"darksouls3", L"loose param property override scheduled");
        } else {
            ML_LOG_WARN(L"darksouls3", L"loose param property override HOOK_FAILED");
            result = false;
        }
    } else {
        (void)ml_properties_set_loose_params(false);
        ML_LOG_INFO(L"darksouls3", L"loose param property override NOT_REQUESTED");
    }
    assets_requested_at_start = game != NULL && ml_asset_hooks_requested();

    image_base = get_module_image_base(NULL, &image_size);
    if (image_base == NULL || image_size == 0 ||
        !ml_runtime_ready_hook_install(L"darksouls3", install_after_runtime)) {
        ML_LOG_WARN(L"darksouls3", L"runtime-ready trigger HOOK_FAILED; deferred capabilities disabled");
        result = false;
    }
    ML_LOG_INFO(L"darksouls3", common_wwise_requested()
        ? L"Wwise capability REQUESTED" : L"Wwise capability NOT_REQUESTED");
    ML_LOG_INFO(L"darksouls3", config.skip_intro
        ? L"SPRJ Logo capability REQUESTED" : L"SPRJ Logo capability SKIPPED_DISABLED");
    ML_LOG_INFO(L"darksouls3", L"offline property capability REQUESTED");
    ML_LOG_INFO(L"darksouls3", config.prevent_regulation_save_write
        ? L"regulation protection capability REQUESTED"
        : L"regulation protection capability SKIPPED_DISABLED");
    ML_LOG_INFO(L"darksouls3", config.patch_mem
        ? L"heap allocator capability REQUESTED"
        : L"heap allocator capability SKIPPED_DISABLED");
    ML_LOG_INFO(L"darksouls3", config.boot_boost && assets_requested_at_start
        ? L"BootBoost capability REQUESTED"
        : L"BootBoost capability NOT_REQUESTED");
    return result;
}

void darksouls3_uninstall(void) {
    if (!ml_runtime_ready_hook_uninstall(L"darksouls3")) return;
    if (InterlockedCompareExchange(&runtime_ready_reached, 0, 0) == 0) {
        ML_LOG_WARN(L"darksouls3", L"runtime-ready capabilities DEFERRED_NOT_REACHED");
    }
    if (!ml_asset_hooks_uninstall()) {
        ML_LOG_WARN(L"darksouls3", L"one or more asset hooks could not be removed");
    }
    image_base = NULL;
    image_size = 0;
    runtime_ready_reached = 0;
    assets_requested_at_start = false;
}
