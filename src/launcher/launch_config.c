/*
 * Copyright (C) 2026, Soar Qin<soarchin@gmail.com>
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#include "launch_config.h"

#define ML_PATH_USE_CRT_ALLOCATOR
#include "common/path.h"

#include <ini.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlwapi.h>

#include <stdio.h>

typedef struct {
    bool found;
    wchar_t key[64];
    const ml_game_descriptor_t *game;
} game_config_state_t;

static int game_config_handler(void *user, const char *section,
                               const char *name, const char *value) {
    game_config_state_t *state = (game_config_state_t *)user;
    int length;

    if (section == NULL || section[0] != '\0' || name == NULL ||
        lstrcmpA(name, "game") != 0) {
        return 1;
    }

    state->found = true;
    state->game = NULL;
    state->key[0] = L'\0';
    if (value == NULL) return 1;
    length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1,
                                 state->key, (int)(sizeof(state->key) / sizeof(state->key[0])));
    if (length == 0) return 1;
    state->key[(sizeof(state->key) / sizeof(state->key[0])) - 1] = L'\0';
    state->game = ml_game_by_key(state->key);
    return 1;
}

static ml_launcher_game_config_result_t game_config_result(
    const game_config_state_t *state, int parse_result,
    const ml_game_descriptor_t **game, wchar_t *invalid_key,
    size_t invalid_key_count) {
    if (game != NULL) *game = NULL;
    if (invalid_key != NULL && invalid_key_count != 0) invalid_key[0] = L'\0';
    if (parse_result != 0) return ML_LAUNCHER_GAME_CONFIG_ERROR;
    if (!state->found) return ML_LAUNCHER_GAME_CONFIG_NOT_SPECIFIED;
    if (state->game == NULL) {
        if (invalid_key != NULL && invalid_key_count != 0)
            lstrcpynW(invalid_key, state->key, (int)invalid_key_count);
        return ML_LAUNCHER_GAME_CONFIG_INVALID;
    }
    if (game != NULL) *game = state->game;
    return ML_LAUNCHER_GAME_CONFIG_FOUND;
}

ml_launcher_game_config_result_t ml_launcher_game_from_ini_string(
    const char *contents, const ml_game_descriptor_t **game,
    wchar_t *invalid_key, size_t invalid_key_count) {
    game_config_state_t state = { 0 };
    int parse_result;

    if (contents == NULL) return ML_LAUNCHER_GAME_CONFIG_ERROR;
    parse_result = ini_parse_string(contents, game_config_handler, &state);
    return game_config_result(&state, parse_result, game, invalid_key, invalid_key_count);
}

ml_launcher_game_config_result_t ml_launcher_game_from_ini_file(
    const wchar_t *path, const ml_game_descriptor_t **game,
    wchar_t *invalid_key, size_t invalid_key_count) {
    game_config_state_t state = { 0 };
    FILE *file;
    int parse_result;

    if (path == NULL || path[0] == L'\0') return ML_LAUNCHER_GAME_CONFIG_ERROR;
    file = _wfopen(path, L"r");
    if (file == NULL) return ML_LAUNCHER_GAME_CONFIG_ERROR;
    parse_result = ini_parse_file(file, game_config_handler, &state);
    fclose(file);
    return game_config_result(&state, parse_result, game, invalid_key, invalid_key_count);
}

const ml_game_descriptor_t *ml_launcher_select_game(
    const ml_game_descriptor_t *explicit_game,
    const ml_game_descriptor_t *config_game) {
    if (explicit_game != NULL) return explicit_game;
    if (config_game != NULL) return config_game;
    return ml_game_by_id(ML_GAME_ELDEN_RING);
}

bool ml_launcher_path_is_relative(const wchar_t *path) {
    return path != NULL && StrChrW(path, L':') == NULL && path[0] != L'\\' && path[0] != L'/';
}

bool ml_launcher_resolve_path(wchar_t path[MAX_PATH], const wchar_t *base_dir,
                              const wchar_t *requested) {
    wchar_t *resolved;
    if (path == NULL) return false;
    path[0] = L'\0';
    resolved = ml_launcher_resolve_path_alloc(base_dir, requested);
    if (resolved == NULL) return false;
    if (wcslen(resolved) >= MAX_PATH) {
        free(resolved);
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return false;
    }
    wcscpy(path, resolved);
    free(resolved);
    return true;
}

wchar_t *ml_launcher_resolve_path_alloc(const wchar_t *base_dir, const wchar_t *requested) {
    if (requested == NULL || requested[0] == L'\0') {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    return ml_path_resolve_alloc(base_dir, requested);
}

wchar_t *ml_launcher_resolve_config_path_alloc(const wchar_t *module_dir, const wchar_t *requested_path) {
    if (module_dir == NULL || module_dir[0] == L'\0') return NULL;
    return ml_launcher_resolve_path_alloc(module_dir,
        requested_path != NULL && requested_path[0] != L'\0' ? requested_path : L"YAFSML.ini");
}

wchar_t *ml_launcher_config_ini_path_alloc(const wchar_t *config_path) {
    wchar_t *path;
    if (config_path == NULL || config_path[0] == L'\0') return NULL;
    path = ml_path_resolve_alloc(NULL, config_path);
    if (path != NULL && ml_path_is_directory(path)) {
        wchar_t *file = ml_path_resolve_alloc(path, L"YAFSML.ini");
        free(path);
        return file;
    }
    return path;
}

bool ml_launcher_resolve_config_path(wchar_t path[MAX_PATH],
                                     const wchar_t *module_dir,
                                     const wchar_t *requested_path) {
    if (path == NULL) return false;
    path[0] = L'\0';
    if (module_dir == NULL || module_dir[0] == L'\0') return false;
    return ml_launcher_resolve_path(path, module_dir,
                                    requested_path != NULL && requested_path[0] != L'\0'
                                        ? requested_path : L"YAFSML.ini");
}

bool ml_launcher_config_ini_path(wchar_t path[MAX_PATH], const wchar_t *config_path) {
    wchar_t *resolved;
    if (path == NULL) return false;
    path[0] = L'\0';
    resolved = ml_launcher_config_ini_path_alloc(config_path);
    if (resolved == NULL) return false;
    if (wcslen(resolved) >= MAX_PATH) {
        free(resolved);
        return false;
    }
    wcscpy(path, resolved);
    free(resolved);
    return true;
}
