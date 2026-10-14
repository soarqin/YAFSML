/*
 * Copyright (C) 2026, Soar Qin<soarchin@gmail.com>
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#include "game.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlwapi.h>

static const wchar_t *const eldenring_exes[] = { L"Game\\eldenring.exe", L"eldenring.exe" };
static const wchar_t *const nightreign_exes[] = { L"Game\\nightreign.exe", L"nightreign.exe" };
static const wchar_t *const sekiro_exes[] = { L"sekiro.exe" };
static const wchar_t *const darksouls3_exes[] = { L"Game\\DarkSoulsIII.exe", L"DarkSoulsIII.exe" };
static const wchar_t *const armoredcore6_exes[] = { L"Game\\armoredcore6.exe" };

static const ml_game_descriptor_t games[] = {
    { ML_GAME_DARK_SOULS_3, "darksouls3", L"Dark Souls III", 374320, darksouls3_exes, 2, L"DarkSoulsIII", L"darksouls3", L"config_darksouls3.toml", ML_STL_ABI_MSVC2012, ML_SUPPORT_EXPERIMENTAL, L"SprjFileStep::STEP_Init", "SprjAutoControlAPI", 0xC0, L"TitleFlowStep::STEP_Init", ML_RENDER_READY_STEP_AFTER_ORIGINAL, NULL, "NS_SPRJ::ParamStep", ML_DATA_READY_SPRJ_PARAM_STEP, ML_RUNTIME_READY_STEAM_API_INIT, ML_LOGO_STRATEGY_SPRJ, ML_ALLOCATOR_STRATEGY_DARK_SOULS_3, ML_REGULATION_STRATEGY_SPRJ, L"Data0.bdt", ML_WWISE_UNSUPPORTED },
    { ML_GAME_SEKIRO, "sekiro", L"Sekiro: Shadows Die Twice", 814380, sekiro_exes, 1, L"Sekiro", L"sekiro", L"config_sekiro.toml", ML_STL_ABI_MSVC2015, ML_SUPPORT_STABLE, L"SprjFileStep::STEP_Init", "SprjAutoControlAPI", 0xB0, L"TitleFlowStep::STEP_Init", ML_RENDER_READY_STEP_AFTER_ORIGINAL, NULL, "NS_SPRJ::ParamStep", ML_DATA_READY_SPRJ_PARAM_STEP, ML_RUNTIME_READY_STEAM_API_INIT, ML_LOGO_STRATEGY_SPRJ, ML_ALLOCATOR_STRATEGY_SEKIRO, ML_REGULATION_STRATEGY_SPRJ, L"regulation.bin", ML_WWISE_UNSUPPORTED },
    { ML_GAME_ELDEN_RING, "eldenring", L"Elden Ring", 1245620, eldenring_exes, 2, L"EldenRing", L"eldenring", L"config_eldenring.toml", ML_STL_ABI_MSVC2015, ML_SUPPORT_STABLE, L"CSFileStep::STEP_Init", "CSAutoControlAPI", 0xB0, L"TitleStep::STEP_InitMenu", ML_RENDER_READY_STEP_AFTER_ORIGINAL, L"ParamStep::STEP_Wait", NULL, ML_DATA_READY_FD4_NAMED_STEP, ML_RUNTIME_READY_STEAM_API_INIT, ML_LOGO_STRATEGY_FD4, ML_ALLOCATOR_STRATEGY_ELDEN_RING, ML_REGULATION_STRATEGY_FD4, L"regulation.bin", ML_WWISE_AT_INSTALL },
    { ML_GAME_ARMORED_CORE_6, "armoredcore6", L"Armored Core VI: Fires of Rubicon", 1888160, armoredcore6_exes, 1, L"ArmoredCore6", L"armoredcore6", L"config_armoredcore6.toml", ML_STL_ABI_MSVC2015, ML_SUPPORT_STABLE, L"CSFileStep::STEP_Init", "CSAutoControlAPI", 0xB0, L"TitleStep::STEP_InitMenu", ML_RENDER_READY_STEP_AFTER_ORIGINAL, L"ParamStep::STEP_Wait", NULL, ML_DATA_READY_FD4_NAMED_STEP, ML_RUNTIME_READY_STEAM_API_INIT, ML_LOGO_STRATEGY_FD4, ML_ALLOCATOR_STRATEGY_UNSUPPORTED, ML_REGULATION_STRATEGY_FD4, L"regulation.bin", ML_WWISE_AFTER_RUNTIME },
    { ML_GAME_NIGHTREIGN, "nightreign", L"Elden Ring Nightreign", 2622380, nightreign_exes, 2, L"Nightreign", L"nightreign", L"config_nightreign.toml", ML_STL_ABI_MSVC2015, ML_SUPPORT_STABLE, L"CSFileStep::STEP_Init", "CSAutoControlAPI", 0xB0, L"TitleStep::STEP_InitMenu", ML_RENDER_READY_STEP_AFTER_ORIGINAL, L"ParamStep::STEP_Wait", NULL, ML_DATA_READY_FD4_NAMED_STEP, ML_RUNTIME_READY_STEAM_API_INIT, ML_LOGO_STRATEGY_FD4, ML_ALLOCATOR_STRATEGY_UNSUPPORTED, ML_REGULATION_STRATEGY_FD4, L"regulation.bin", ML_WWISE_AFTER_RUNTIME },
};

static const struct {
    const wchar_t *alias;
    ml_game_id_t id;
} aliases[] = {
    { L"darksouls3", ML_GAME_DARK_SOULS_3 },
    { L"dark-souls-3", ML_GAME_DARK_SOULS_3 },
    { L"ds3", ML_GAME_DARK_SOULS_3 },
    { L"sekiro", ML_GAME_SEKIRO },
    { L"eldenring", ML_GAME_ELDEN_RING },
    { L"elden-ring", ML_GAME_ELDEN_RING },
    { L"er", ML_GAME_ELDEN_RING },
    { L"armoredcore6", ML_GAME_ARMORED_CORE_6 },
    { L"ac6", ML_GAME_ARMORED_CORE_6 },
    { L"nightreign", ML_GAME_NIGHTREIGN },
    { L"nr", ML_GAME_NIGHTREIGN },
    { L"nightrein", ML_GAME_NIGHTREIGN },
};

const ml_game_descriptor_t *ml_game_by_id(ml_game_id_t id) {
    for (size_t i = 0; i < sizeof(games) / sizeof(games[0]); i++) {
        if (games[i].id == id) return &games[i];
    }
    return NULL;
}

const ml_game_descriptor_t *ml_game_by_key(const wchar_t *key) {
    if (key == NULL) return NULL;
    for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++) {
        if (lstrcmpiW(key, aliases[i].alias) == 0) return ml_game_by_id(aliases[i].id);
    }
    return NULL;
}

const ml_game_descriptor_t *ml_game_by_exe_path(const wchar_t *path) {
    const wchar_t *filename;
    if (path == NULL) return NULL;
    filename = PathFindFileNameW(path);
    for (size_t i = 0; i < sizeof(games) / sizeof(games[0]); i++) {
        for (size_t j = 0; j < games[i].exe_relpath_count; j++) {
            if (lstrcmpiW(filename, PathFindFileNameW(games[i].exe_relpaths[j])) == 0) return &games[i];
        }
    }
    return NULL;
}

const ml_game_descriptor_t *ml_game_detect_current_process(void) {
    wchar_t path[MAX_PATH];
    if (GetModuleFileNameW(NULL, path, MAX_PATH) == 0) return NULL;
    return ml_game_by_exe_path(path);
}
