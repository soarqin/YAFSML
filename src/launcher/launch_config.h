/*
 * Copyright (C) 2026, Soar Qin<soarchin@gmail.com>
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#pragma once

#include "game/game.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <stdbool.h>
#include <stddef.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ml_launcher_game_config_result_e {
    ML_LAUNCHER_GAME_CONFIG_NOT_SPECIFIED,
    ML_LAUNCHER_GAME_CONFIG_FOUND,
    ML_LAUNCHER_GAME_CONFIG_INVALID,
    ML_LAUNCHER_GAME_CONFIG_ERROR,
} ml_launcher_game_config_result_t;

ml_launcher_game_config_result_t ml_launcher_game_from_ini_string(
    const char *contents, const ml_game_descriptor_t **game,
    wchar_t *invalid_key, size_t invalid_key_count);
ml_launcher_game_config_result_t ml_launcher_game_from_ini_file(
    const wchar_t *path, const ml_game_descriptor_t **game,
    wchar_t *invalid_key, size_t invalid_key_count);
const ml_game_descriptor_t *ml_launcher_select_game(
    const ml_game_descriptor_t *explicit_game,
    const ml_game_descriptor_t *config_game);
/* True unless the path names a drive or starts with a separator. */
bool ml_launcher_path_is_relative(const wchar_t *path);
/* Resolves `requested` to an absolute, normalized path. A relative path is
 * joined with `base_dir`, or resolved against the working directory when
 * `base_dir` is NULL. Paths that do not fit in MAX_PATH fail. */
bool ml_launcher_resolve_path(wchar_t path[MAX_PATH], const wchar_t *base_dir,
                              const wchar_t *requested);
/* Resolves the value exported as YAFSML_CONFIG. Relative paths are relative
 * to `module_dir`; a directory stays a directory so the DLL applies its
 * INI and ModEngine2 TOML fallback. Without a request this is YAFSML.ini in
 * `module_dir`. */
bool ml_launcher_resolve_config_path(wchar_t path[MAX_PATH],
                                     const wchar_t *module_dir,
                                     const wchar_t *requested_path);
/* The INI that can select the game: YAFSML.ini in a configuration directory,
 * otherwise the configuration file itself. */
bool ml_launcher_config_ini_path(wchar_t path[MAX_PATH], const wchar_t *config_path);

#ifdef __cplusplus
}
#endif
