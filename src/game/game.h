/*
 * Copyright (C) 2026, Soar Qin<soarchin@gmail.com>
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum ml_game_id_e {
    ML_GAME_UNKNOWN = 0,
    ML_GAME_DARK_SOULS_3,
    ML_GAME_SEKIRO,
    ML_GAME_ELDEN_RING,
    ML_GAME_ARMORED_CORE_6,
    ML_GAME_NIGHTREIGN,
} ml_game_id_t;

typedef enum ml_stl_abi_e {
    ML_STL_ABI_MSVC2012,
    ML_STL_ABI_MSVC2015,
} ml_stl_abi_t;

typedef enum ml_support_level_e {
    ML_SUPPORT_STABLE,
    ML_SUPPORT_EXPERIMENTAL,
} ml_support_level_t;

typedef enum ml_runtime_ready_trigger_e {
    ML_RUNTIME_READY_STEAM_API_INIT,
    ML_RUNTIME_READY_UNSUPPORTED,
} ml_runtime_ready_trigger_t;

typedef enum ml_logo_strategy_e {
    ML_LOGO_STRATEGY_UNSUPPORTED,
    ML_LOGO_STRATEGY_FD4,
    ML_LOGO_STRATEGY_SPRJ,
} ml_logo_strategy_t;

typedef enum ml_allocator_strategy_e {
    ML_ALLOCATOR_STRATEGY_UNSUPPORTED,
    ML_ALLOCATOR_STRATEGY_ELDEN_RING,
    ML_ALLOCATOR_STRATEGY_SEKIRO,
    ML_ALLOCATOR_STRATEGY_DARK_SOULS_3,
} ml_allocator_strategy_t;

typedef enum ml_regulation_strategy_e {
    ML_REGULATION_STRATEGY_UNSUPPORTED,
    ML_REGULATION_STRATEGY_FD4,
    ML_REGULATION_STRATEGY_SPRJ,
} ml_regulation_strategy_t;

/* How the adapter learns that the title/menu flow has been constructed. This is
   render readiness only: game params may still be loading at that point. */
typedef enum ml_render_ready_strategy_e {
    ML_RENDER_READY_UNSUPPORTED,
    ML_RENDER_READY_STEP_AFTER_ORIGINAL,
    /* Reuses the asset FileStep hook instead of a dedicated title step. No game
       selects this today; retained as a fallback when the title step of a build
       cannot be resolved. */
    ML_RENDER_READY_FILE_STEP_AFTER_ORIGINAL,
} ml_render_ready_strategy_t;

/* How the adapter learns that every game param has been loaded. */
typedef enum ml_data_ready_strategy_e {
    ML_DATA_READY_UNSUPPORTED,
    /* Elden Ring / Armored Core VI / Nightreign: hook the named FD4 step
       `ParamStep::STEP_Wait`,
       which only becomes active once `ParamStep::STEP_LoadWait` observed the
       regulation manager finishing and ran its post-load fixups. */
    ML_DATA_READY_FD4_NAMED_STEP,
    /* Sekiro / Dark Souls III: `NS_SPRJ::Step<ParamStep>` has an unnamed step
       table, so the wait step is derived from the class vtable and constructor. */
    ML_DATA_READY_SPRJ_PARAM_STEP,
} ml_data_ready_strategy_t;

typedef struct ml_game_descriptor_s {
    ml_game_id_t id;
    const char *key;
    const wchar_t *title;
    uint32_t steam_app_id;
    const wchar_t *const *exe_relpaths;
    size_t exe_relpath_count;
    const wchar_t *save_root_name;
    const wchar_t *ini_section;
    const wchar_t *modengine_config_name;
    ml_stl_abi_t stl_abi;
    ml_support_level_t support_level;
    const wchar_t *file_step_name;
    const char *control_api_class;
    size_t ebl_bhd_holder_offset;
    const wchar_t *render_ready_step_name;
    ml_render_ready_strategy_t render_ready_strategy;
    /* ML_DATA_READY_FD4_NAMED_STEP only. */
    const wchar_t *data_ready_step_name;
    /* ML_DATA_READY_SPRJ_PARAM_STEP only: RTTI class name of the param step. */
    const char *data_ready_class;
    ml_data_ready_strategy_t data_ready_strategy;
    ml_runtime_ready_trigger_t runtime_ready_trigger;
    ml_logo_strategy_t logo_strategy;
    ml_allocator_strategy_t allocator_strategy;
    ml_regulation_strategy_t regulation_strategy;
} ml_game_descriptor_t;

const ml_game_descriptor_t *ml_game_by_id(ml_game_id_t id);
const ml_game_descriptor_t *ml_game_by_key(const wchar_t *key);
const ml_game_descriptor_t *ml_game_by_exe_path(const wchar_t *path);
const ml_game_descriptor_t *ml_game_detect_current_process(void);

bool ml_game_context_init(void);
const ml_game_descriptor_t *ml_game_context_get(void);
