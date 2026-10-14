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
    .patch_mem_dedicated_heap = true,
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

static wchar_t *utf8_to_wide_alloc(const char *value) {
    int length = value == NULL ? 0 : MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, NULL, 0);
    wchar_t *result = length == 0 ? NULL : ml_mem_alloc(0, (size_t)length * sizeof(*result));
    if (result != NULL && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, result, length) == 0) {
        ml_mem_free(result);
        result = NULL;
    }
    return result;
}

static void config_add_mod_path(const char *name, const char *value) {
    wchar_t *path = utf8_to_wide_alloc(value);
    if (path == NULL) {
        ML_LOG_ERROR(L"config", L"invalid mod path for %hs", name);
    } else {
        mods_add(name, path);
    }
    ml_mem_free(path);
}

static void config_set_save_filename(wchar_t name[64], const char *key, const char *value) {
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, name, 64) == 0) {
        /* Keep the option requested but invalid, so the save mapping fails
         * closed instead of silently using the original save. */
        ML_LOG_ERROR(L"config", L"%hs must be valid UTF-8 shorter than 64 characters", key);
        lstrcpyW(name, L"?");
    }
}

static int ini_read_cb(void *user, const char *section,
                       const char *name, const char *value) {
    (void)user;
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
            config_set_save_filename(config.replaced_save_filename, name, value);
        } else if (lstrcmpA(name, "replace_seamless_coop_save_filename") == 0) {
            config_set_save_filename(config.replaced_seamless_coop_save_filename, name, value);
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
        config_add_mod_path(name, value);
    }
    return 1;
}

void config_load_ini(FILE *f) {
    int result = ini_parse_file(f, ini_read_cb, NULL);
    if (result > 0) {
        ML_LOG_WARN(L"config", L"configuration parse error at line %d; later settings may be missing", result);
    } else if (result < 0) {
        ML_LOG_ERROR(L"config", L"configuration could not be read (%d)", result);
    }
}

#if !defined(ML_STRIP_MODENGINE_CONFIG)
bool config_load_toml(FILE *f) {
    char errbuf[256];
    toml_table_t *root = toml_parse_file(f, errbuf, 256);
    toml_value_t value;
    toml_array_t *arr;
    int i, len;
    if (root == NULL) {
        /* Logging is configured by this file, so also report the error to a debugger. */
        wchar_t message[320];
        _snwprintf_s(message, sizeof(message) / sizeof(message[0]), _TRUNCATE,
                     L"YAFSML: ModEngine2 configuration parse error: %hs\n", errbuf);
        OutputDebugStringW(message);
        ML_LOG_ERROR(L"config", L"ModEngine2 configuration parse error: %hs", errbuf);
        return false;
    }
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
        wchar_t *path;
        value = toml_array_string(arr, i);
        if (!value.ok) continue;
        path = utf8_to_wide_alloc(value.u.s);
        if (path == NULL) {
            ML_LOG_ERROR(L"config", L"invalid external DLL path: %hs", value.u.s);
        } else {
            /* ModEngine2 has no DLL names; the path without extension serves as one. */
            PathRemoveExtensionA(value.u.s);
            extdlls_add(value.u.s, path);
            ml_mem_free(path);
        }
        free(value.u.s);
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
        if (!sub) continue;
        /* ModEngine2 requires only `path`; `enabled` defaults to true and `name`
         * to "Unknown". */
        const toml_value_t enabled = toml_table_bool(sub, "enabled");
        if (enabled.ok && !enabled.u.b) continue;
        const toml_value_t name = toml_table_string(sub, "name");
        const toml_value_t path = toml_table_string(sub, "path");
        if (path.ok) {
            config_add_mod_path(name.ok ? name.u.s : "Unknown", path.u.s);
            free(path.u.s);
        }
        if (name.ok) free(name.u.s);
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

/* Resolves YAFSML_CONFIG like the launcher's -c: relative to the DLL directory,
 * normalized, and a directory selects its YAFSML.ini (or the game's ModEngine2
 * TOML when there is none). */
static bool resolve_config_path(wchar_t config_path[MAX_PATH], const wchar_t *modengine_config_name) {
    wchar_t joined[MAX_PATH];
    if (env_config_path[0] == L'\0') {
        lstrcpyW(joined, modloader_module_path);
    } else if (StrChrW(env_config_path, L':') == NULL && env_config_path[0] != L'\\' && env_config_path[0] != L'/') {
        lstrcpyW(joined, modloader_module_path);
        if (!PathAppendW(joined, env_config_path)) return false;
    } else {
        lstrcpyW(joined, env_config_path);
    }
    {
        DWORD length = GetFullPathNameW(joined, MAX_PATH, config_path, NULL);
        if (length == 0 || length >= MAX_PATH) return false;
    }
    if (PathIsDirectoryW(config_path)) {
        wchar_t directory[MAX_PATH];
        lstrcpyW(directory, config_path);
        if (!PathAppendW(config_path, L"YAFSML.ini")) return false;
#if !defined(ML_STRIP_MODENGINE_CONFIG)
        if (!PathFileExistsW(config_path) || PathIsDirectoryW(config_path)) {
            lstrcpyW(config_path, directory);
            if (!PathAppendW(config_path, modengine_config_name)) return false;
        }
#else
        (void)modengine_config_name;
#endif
    }
    return true;
}

void config_load(void) {
    wchar_t config_path[MAX_PATH] = L"";
    FILE *config_file = NULL;
    const ml_game_descriptor_t *game = ml_game_detect_current_process();
    const wchar_t *modengine_config_name = game != NULL
        ? game->modengine_config_name : L"config_eldenring.toml";
    if (!resolve_config_path(config_path, modengine_config_name)) {
        env_config_path[0] = L'\0';
        return;
    }
    /* config_full_path* resolve relative paths against this file's directory. */
    if (env_config_path[0] != L'\0') lstrcpyW(env_config_path, config_path);
    config_file = _wfopen(config_path, L"r");
    if (config_file == NULL) return;
#if !defined(ML_STRIP_MODENGINE_CONFIG)
    if (lstrcmpiW(PathFindExtensionW(config_path), L".toml") == 0) {
        (void)config_load_toml(config_file);
    } else
#endif
    {
        config_load_ini(config_file);
    }
    fclose(config_file);
    ML_LOG_INFO(NULL, L"Log enabled; log level=%ls", ml_log_level_name(ml_log_get_level()));
}

/* Directory of the loaded configuration file, or of the DLL without one. After
 * config_load, a non-empty env_config_path always names a file. */
static void config_directory(wchar_t directory[MAX_PATH]) {
    lstrcpyW(directory, env_config_path[0] == L'\0' ? modloader_module_path : env_config_path);
    if (env_config_path[0] != L'\0') PathRemoveFileSpecW(directory);
}

wchar_t *config_full_path_alloc(const wchar_t *filename) {
    wchar_t base[MAX_PATH];
    size_t base_length;
    size_t filename_length = filename == NULL ? 0 : wcslen(filename);
    bool separator;
    wchar_t *result;
    config_directory(base);
    base_length = wcslen(base);
    separator = filename_length != 0 && base_length != 0 &&
                base[base_length - 1] != L'\\' && base[base_length - 1] != L'/';
    if (filename_length > SIZE_MAX / sizeof(*result) - base_length - 2) return NULL;
    result = ml_mem_alloc(0, (base_length + filename_length + 2) * sizeof(*result));
    if (result == NULL) return NULL;
    memcpy(result, base, base_length * sizeof(*result));
    if (separator) result[base_length++] = L'\\';
    if (filename_length != 0) memcpy(result + base_length, filename, filename_length * sizeof(*result));
    result[base_length + filename_length] = L'\0';
    return result;
}

void config_full_path(wchar_t *path, const wchar_t *filename) {
    config_directory(path);
    if (filename && filename[0] != 0)
        PathAppendW(path, filename);
}
