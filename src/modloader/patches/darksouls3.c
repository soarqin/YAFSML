#include "darksouls3.h"
#include "log.h"

#include "asset_hooks.h"
#include "common.h"
#include "properties.h"
#include "regulation.h"

#include "modloader/config.h"

bool darksouls3_prepare(const ml_game_descriptor_t *game, bool assets_requested) {
    ML_LOG_WARN(L"darksouls3", L"experimental adapter enabled; Arxan neutralization is forced on");
    if (ml_asset_hooks_loose_params_present(game)) {
        ml_properties_set_loose_params(true);
        ML_LOG_INFO(L"darksouls3", L"loose param property override scheduled");
    } else {
        ml_properties_set_loose_params(false);
        ML_LOG_INFO(L"darksouls3", L"loose param property override NOT_REQUESTED");
    }
    ML_LOG_INFO(L"darksouls3", config.skip_intro
        ? L"SPRJ Logo capability REQUESTED" : L"SPRJ Logo capability SKIPPED_DISABLED");
    ML_LOG_INFO(L"darksouls3", L"offline property capability REQUESTED");
    ML_LOG_INFO(L"darksouls3", config.prevent_regulation_save_write
        ? (ml_regulation_override_present(game)
            ? L"regulation protection capability REQUESTED"
            : L"regulation protection capability SKIPPED_NO_OVERRIDE")
        : L"regulation protection capability SKIPPED_DISABLED");
    ML_LOG_INFO(L"darksouls3", config.patch_mem
        ? L"heap allocator capability REQUESTED"
        : L"heap allocator capability SKIPPED_DISABLED");
    ML_LOG_INFO(L"darksouls3", config.boot_boost && assets_requested
        ? L"BootBoost capability REQUESTED"
        : L"BootBoost capability NOT_REQUESTED");
    return true;
}
