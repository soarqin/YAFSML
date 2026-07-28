/*
 * Copyright (C) 2024, Soar Qin<soarchin@gmail.com>

 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#include "config.h"

#include "allocator.h"
#include "log.h"
#include "extdll.h"
#include "mod.h"

#include "game/game.h"

#include <ini.h>
#if !defined(ML_STRIP_MODENGINE_CONFIG)
#include <toml.h>
#endif

#include <shlwapi.h>

config_t config = {
    .skip_intro = true,
    .prevent_regulation_save_write = true,
    .patch_mem = true,
    .patch_mem_dedicated_heap = false,
    .patch_mem_heap_size = 0,
    .boot_boost = true,
    .replaced_save_filename = L"",
    .replaced_seamless_coop_save_filename = L"",
    .enable_ime = false,
    .cpu_affinity_strategy = 0,
};

static wchar_t modloader_module_path[MAX_PATH];
wchar_t env_config_path[MAX_PATH];

static bool value_to_bool(const char *value) {
    return lstrcmpA(value, "true") == 0 || lstrcmpA(value, "yes") == 0 || lstrcmpA(value, "on") == 0 || lstrcmpA(value, "1") == 0;
}

static int ini_read_cb(void *user, const char *section,
                       const char *name, const char *value) {
    wchar_t path[MAX_PATH];
    if (lstrcmpA(section, "patch") == 0) {
        if (lstrcmpA(name, "skip_intro") == 0) {
            config.skip_intro = value_to_bool(value);
        } else if (lstrcmpA(name, "prevent_regulation_save_write") == 0) {
            config.prevent_regulation_save_write = value_to_bool(value);
        } else if (lstrcmpA(name, "patch_mem") == 0) {
            config.patch_mem = value_to_bool(value);
        } else if (lstrcmpA(name, "patch_mem_dedicated_heap") == 0) {
            config.patch_mem_dedicated_heap = value_to_bool(value);
        } else if (lstrcmpA(name, "patch_mem_heap_size") == 0) {
            char *end;
            unsigned long long size = strtoull(value, &end, 10);
            if (end != value && *end == '\0' && size <= UINT32_MAX) config.patch_mem_heap_size = (uint32_t)size;
        } else if (lstrcmpA(name, "boot_boost") == 0) {
            config.boot_boost = value_to_bool(value);
        } else if (lstrcmpA(name, "disable_arxan") == 0) {
            config.disable_arxan = value_to_bool(value);
        } else if (lstrcmpA(name, "replace_save_filename") == 0) {
            MultiByteToWideChar(CP_UTF8, 0, value, -1, config.replaced_save_filename, 64);
            config.replaced_save_filename[63] = L'\0';
        } else if (lstrcmpA(name, "replace_seamless_coop_save_filename") == 0) {
            MultiByteToWideChar(CP_UTF8, 0, value, -1,
                                config.replaced_seamless_coop_save_filename, 64);
            config.replaced_seamless_coop_save_filename[63] = L'\0';
        } else if (lstrcmpA(name, "enable_ime") == 0) {
            config.enable_ime = value_to_bool(value);
        }
    } else if (lstrcmpA(section, "log") == 0) {
        if (lstrcmpA(name, "console") == 0) {
            if (value_to_bool(value)) {
                ml_log_enable_console();
            }
        } else if (lstrcmpA(name, "log_file") == 0) {
            if (value_to_bool(value)) {
                wchar_t *file = config_full_path_alloc(L"log\\YAFSML.log");
                wchar_t *path = file == NULL ? NULL : ml_mem_strdup_w(file);
                if (path != NULL) {
                    PathRemoveFileSpecW(path);
                    CreateDirectoryW(path, NULL);
                    ml_mem_free(path);
                }
                if (file != NULL) {
                    ml_log_enable_file(file);
                    ml_mem_free(file);
                }
            }
        } else if (lstrcmpA(name, "log_level") == 0) {
            ml_log_level_t level;
            if (ml_log_level_parse(value, &level)) {
                ml_log_set_level(level);
            } else {
                ML_LOG_WARN(L"config", L"Unknown log level: %hs", value);
            }
        }
    } else if (lstrcmpA(section, "tweak") == 0) {
        if (lstrcmpA(name, "cpu_affinity") == 0) {
            config.cpu_affinity_strategy = strtol(value, NULL, 0);
        }
    } else if (lstrcmpA(section, "dll") == 0) {
        extdlls_add_spec(name, value);
    } else if (lstrcmpA(section, "mod") == 0) {
        if (MultiByteToWideChar(CP_UTF8, 0, value, -1, path, MAX_PATH) == 0) {
            ML_LOG_ERROR(L"config", L"invalid mod path for %hs", name);
            return 1;
        }
        path[MAX_PATH - 1] = L'\0';
        mods_add(name, path);
    }
    return 1;
}

void config_load_ini(FILE *f) {
    ini_parse_file(f, ini_read_cb, NULL);
}

#if !defined(ML_STRIP_MODENGINE_CONFIG)
bool config_load_toml(FILE *f) {
    char errbuf[256];
    toml_table_t *root = toml_parse_file(f, errbuf, 256);
    toml_value_t value;
    toml_array_t *arr;
    int i, len;
    if (root == NULL) return false;
    const toml_table_t *me = toml_table_table(root, "modengine");
    if (me == NULL) goto TOML_LOAD_MODS;
    value = toml_table_bool(me, "debug");
    if (value.ok && value.u.b) {
        ml_log_enable_console();
    }
    value = toml_table_bool(me, "disable_arxan");
    if (value.ok) config.disable_arxan = value.u.b;
    {
        toml_value_t log_level = toml_table_string(me, "log_level");
        if (log_level.ok) {
            ml_log_level_t level;
            if (ml_log_level_parse(log_level.u.s, &level)) {
                ml_log_set_level(level);
            } else {
                ML_LOG_WARN(L"config", L"Unknown log level: %hs", log_level.u.s);
            }
            free(log_level.u.s);
        }
    }
    arr = toml_table_array(me, "external_dlls");
    if (arr == NULL) goto TOML_LOAD_MODS;
    len = toml_array_len(arr);
    for (i = 0; i < len; i++) {
        wchar_t path[MAX_PATH];
        char name[MAX_PATH];
        value = toml_array_string(arr, i);
        if (!value.ok) continue;
        lstrcpynA(name, value.u.s, MAX_PATH);
        free(value.u.s);
        MultiByteToWideChar(CP_UTF8, 0, name, -1, path, MAX_PATH);
        PathRemoveExtensionA(name);
        extdlls_add(name, path);
    }

    TOML_LOAD_MODS:
    me = toml_table_table(root, "extension");
    if (me == NULL) goto TOML_LOAD_END;
    me = toml_table_table(me, "mod_loader");
    if (me == NULL) goto TOML_LOAD_END;
    value = toml_table_bool(me, "enabled");
    if (!value.ok || !value.u.b) goto TOML_LOAD_END;
    arr = toml_table_array(me, "mods");
    if (arr == NULL) goto TOML_LOAD_END;
    len = toml_array_len(arr);
    for (i = 0; i < len; i++) {
        toml_table_t *sub = toml_array_table(arr, i);
        wchar_t wpath[MAX_PATH];
        if (!sub) continue;
        const toml_value_t enabled = toml_table_bool(sub, "enabled");
        if (!enabled.ok || !enabled.u.b) continue;
        const toml_value_t name = toml_table_string(sub, "name");
        if (!name.ok) continue;
        const toml_value_t path = toml_table_string(sub, "path");
        if (!path.ok) {
            free(name.u.s);
            continue;
        }
        MultiByteToWideChar(CP_UTF8, 0, path.u.s, -1, wpath, MAX_PATH);
        mods_add(name.u.s, wpath);
        free(name.u.s);
        free(path.u.s);
    }

    TOML_LOAD_END:
    toml_free(root);
    return true;
}
#endif

void config_init(void *module) {
    GetEnvironmentVariableW(L"YAFSML_CONFIG", env_config_path, MAX_PATH);
    GetModuleFileNameW(module, modloader_module_path, MAX_PATH);
    PathRemoveFileSpecW(modloader_module_path);
    PathRemoveBackslashW(modloader_module_path);
}

void config_load() {
    wchar_t config_path[MAX_PATH] = L"";
    FILE *config_file = NULL;
    const ml_game_descriptor_t *game = ml_game_detect_current_process();
    const wchar_t *modengine_config_name = game != NULL
        ? game->modengine_config_name : L"config_eldenring.toml";
    if (env_config_path[0] == L'\0') {
        lstrcpyW(config_path, modloader_module_path);
        PathAppendW(config_path, L"YAFSML.ini");
#if !defined(ML_STRIP_MODENGINE_CONFIG)
        if (!PathFileExistsW(config_path) || PathIsDirectoryW(config_path)) {
            lstrcpyW(config_path, modloader_module_path);
            PathAppendW(config_path, modengine_config_name);
        }
#endif
    } else {
        if (StrChrW(env_config_path, L':') == NULL && env_config_path[0] != L'\\' && env_config_path[0] != L'/') {
            lstrcpyW(config_path, modloader_module_path);
            PathAppendW(config_path, env_config_path);
        } else if (PathIsDirectoryW(env_config_path)) {
            lstrcpyW(config_path, env_config_path);
            PathAppendW(config_path, L"YAFSML.ini");
#if !defined(ML_STRIP_MODENGINE_CONFIG)
            if (!PathFileExistsW(config_path) || PathIsDirectoryW(config_path)) {
                lstrcpyW(config_path, env_config_path);
                PathAppendW(config_path, modengine_config_name);
            }
#endif
        } else {
            /* Absolute file path */
            lstrcpyW(config_path, env_config_path);
        }
        lstrcpyW(env_config_path, config_path);
    }
    config_file = _wfopen(config_path, L"r");
    if (config_file == NULL) return;
#if !defined(ML_STRIP_MODENGINE_CONFIG)
    if (lstrcmpiW(PathFindExtensionW(config_path), L".toml") != 0 || !config_load_toml(config_file))
#endif
        config_load_ini(config_file);
    fclose(config_file);
    ML_LOG_INFO(NULL, L"Log enabled; log level=%ls", ml_log_level_name(ml_log_get_level()));
}

const wchar_t *module_path() { return modloader_module_path; }

wchar_t *config_full_path_alloc(const wchar_t *filename) {
    const wchar_t *base = env_config_path[0] == L'\0' ? modloader_module_path : env_config_path;
    size_t base_length = wcslen(base);
    size_t filename_length = filename == NULL ? 0 : wcslen(filename);
    bool separator = filename_length != 0 && base_length != 0 &&
                     base[base_length - 1] != L'\\' && base[base_length - 1] != L'/';
    wchar_t *result;
    if (base_length > SIZE_MAX - filename_length - (separator ? 2 : 1)) return NULL;
    result = ml_mem_alloc(0, (base_length + filename_length + (separator ? 2 : 1)) * sizeof(*result));
    if (result == NULL) return NULL;
    memcpy(result, base, (base_length + 1) * sizeof(*result));
    if (env_config_path[0] != L'\0' && PathFileExistsW(result) && !PathIsDirectoryW(result)) {
        PathRemoveFileSpecW(result);
        base_length = wcslen(result);
        separator = filename_length != 0 && base_length != 0 &&
                    result[base_length - 1] != L'\\' && result[base_length - 1] != L'/';
    }
    if (separator) result[base_length++] = L'\\';
    if (filename_length != 0) memcpy(result + base_length, filename, (filename_length + 1) * sizeof(*result));
    return result;
}

void config_full_path(wchar_t *path, const wchar_t *filename) {
    if (env_config_path[0] == L'\0') {
        lstrcpyW(path, modloader_module_path);
    } else {
        lstrcpyW(path, env_config_path);
        if (PathFileExistsW(path) && !PathIsDirectoryW(path)) {
            PathRemoveFileSpecW(path);
        }
    }
    if (filename && filename[0] != 0)
        PathAppendW(path, filename);
}
