#include "config_model.h"

#include <windows.h>
#include <ctype.h>
#include <ini.h>
#include <shlwapi.h>
#include <stdlib.h>
#include <string.h>

static bool text_equal(const char *a, const char *b) {
    return a != NULL && b != NULL && _stricmp(a, b) == 0;
}

static bool wide_equal(const wchar_t *a, const wchar_t *b) {
    return a != NULL && b != NULL && _wcsicmp(a, b) == 0;
}

static bool value_bool(const char *value, bool *out) {
    if (value == NULL || out == NULL) return false;
    if (text_equal(value, "true") || text_equal(value, "yes") ||
        text_equal(value, "on") || strcmp(value, "1") == 0) {
        *out = true;
        return true;
    }
    if (text_equal(value, "false") || text_equal(value, "no") ||
        text_equal(value, "off") || strcmp(value, "0") == 0) {
        *out = false;
        return true;
    }
    *out = false;
    return true;
}

static bool utf8_to_wide(const char *value, wchar_t *out, int count) {
    int result;
    if (value == NULL || out == NULL || count <= 0) return false;
    result = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, out, count);
    if (result == 0) {
        result = MultiByteToWideChar(CP_ACP, 0, value, -1, out, count);
    }
    if (result == 0) out[0] = L'\0';
    out[count - 1] = L'\0';
    return result != 0;
}

static bool wide_to_utf8(const wchar_t *value, char **out) {
    int count;
    char *buffer;
    if (value == NULL || out == NULL) return false;
    count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, NULL, 0, NULL, NULL);
    if (count == 0) return false;
    buffer = (char *)malloc((size_t)count);
    if (buffer == NULL) return false;
    if (WideCharToMultiByte(CP_UTF8, 0, value, -1, buffer, count, NULL, NULL) == 0) {
        free(buffer);
        return false;
    }
    *out = buffer;
    return true;
}

static bool known_fixed(const char *section, const char *name) {
    static const char *const patch[] = {
        "skip_intro", "prevent_regulation_save_write", "patch_mem",
        "patch_mem_dedicated_heap", "patch_mem_heap_size", "boot_boost",
        "disable_arxan", "replace_save_filename",
        "replace_seamless_coop_save_filename", "enable_ime"
    };
    static const char *const tweak[] = { "cpu_affinity" };
    static const char *const log[] = { "console", "log_file", "log_level" };
    const char *const *names = NULL;
    size_t count = 0;
    size_t i;
    if (section == NULL || name == NULL) return false;
    if (section[0] == '\0') return text_equal(name, "game");
    if (text_equal(section, "patch")) {
        names = patch; count = sizeof(patch) / sizeof(patch[0]);
    } else if (text_equal(section, "tweak")) {
        names = tweak; count = sizeof(tweak) / sizeof(tweak[0]);
    } else if (text_equal(section, "log")) {
        names = log; count = sizeof(log) / sizeof(log[0]);
    } else if (text_equal(section, "dll") || text_equal(section, "mod")) {
        return true;
    }
    for (i = 0; i < count; i++) if (text_equal(name, names[i])) return true;
    return false;
}

static bool valid_game(const wchar_t *game) {
    return wide_equal(game, L"eldenring") || wide_equal(game, L"elden-ring") ||
           wide_equal(game, L"er") || wide_equal(game, L"armoredcore6") ||
           wide_equal(game, L"ac6") || wide_equal(game, L"nightreign") ||
           wide_equal(game, L"sekiro") || wide_equal(game, L"darksouls3") ||
           wide_equal(game, L"dark-souls-3") || wide_equal(game, L"ds3");
}

static void normalize_game(wchar_t *game) {
    if (game == NULL) return;
    if (wide_equal(game, L"elden-ring") || wide_equal(game, L"er")) {
        lstrcpyW(game, L"eldenring");
    } else if (wide_equal(game, L"ac6")) {
        lstrcpyW(game, L"armoredcore6");
    } else if (wide_equal(game, L"nr") || wide_equal(game, L"nightrein")) {
        lstrcpyW(game, L"nightreign");
    } else if (wide_equal(game, L"dark-souls-3") || wide_equal(game, L"ds3")) {
        lstrcpyW(game, L"darksouls3");
    }
}

typedef struct config_parse_state_s {
    manager_config_t *config;
} config_parse_state_t;

static int config_handler(void *user, const char *section, const char *name, const char *value) {
    config_parse_state_t *state = (config_parse_state_t *)user;
    manager_config_t *config = state->config;
    bool boolean;
    wchar_t *target = NULL;
    char *end;
    unsigned long number;
    if (section == NULL || name == NULL || value == NULL) return 1;
    if (!known_fixed(section, name)) {
        if (!text_equal(section, "dll") && !text_equal(section, "mod")) config->unknown_count++;
        return 1;
    }
    if (section[0] == '\0' && text_equal(name, "game")) {
        utf8_to_wide(value, config->game, (int)(sizeof(config->game) / sizeof(config->game[0])));
    } else if (text_equal(section, "patch")) {
        if (text_equal(name, "skip_intro")) target = (wchar_t *)&config->skip_intro;
        else if (text_equal(name, "prevent_regulation_save_write")) target = (wchar_t *)&config->prevent_regulation_save_write;
        else if (text_equal(name, "patch_mem")) target = (wchar_t *)&config->patch_mem;
        else if (text_equal(name, "patch_mem_dedicated_heap")) target = (wchar_t *)&config->patch_mem_dedicated_heap;
        else if (text_equal(name, "boot_boost")) target = (wchar_t *)&config->boot_boost;
        else if (text_equal(name, "disable_arxan")) target = (wchar_t *)&config->disable_arxan;
        else if (text_equal(name, "enable_ime")) target = (wchar_t *)&config->enable_ime;
        if (target != NULL) {
            value_bool(value, &boolean);
            *(bool *)target = boolean;
        } else if (text_equal(name, "patch_mem_heap_size")) {
            number = strtoul(value, &end, 10);
            if (end != value && *end == '\0' && number <= UINT32_MAX) config->patch_mem_heap_size = (uint32_t)number;
        } else if (text_equal(name, "replace_save_filename")) {
            utf8_to_wide(value, config->replace_save_filename, (int)(sizeof(config->replace_save_filename) / sizeof(config->replace_save_filename[0])));
        } else if (text_equal(name, "replace_seamless_coop_save_filename")) {
            utf8_to_wide(value, config->replace_seamless_coop_save_filename, (int)(sizeof(config->replace_seamless_coop_save_filename) / sizeof(config->replace_seamless_coop_save_filename[0])));
        }
    } else if (text_equal(section, "tweak") && text_equal(name, "cpu_affinity")) {
        config->cpu_affinity = (int)strtol(value, &end, 0);
        if (end == value || *end != '\0') config->cpu_affinity = 0;
    } else if (text_equal(section, "log")) {
        if (text_equal(name, "console")) { value_bool(value, &config->console); }
        else if (text_equal(name, "log_file")) { value_bool(value, &config->log_file); }
        else if (text_equal(name, "log_level")) utf8_to_wide(value, config->log_level, (int)(sizeof(config->log_level) / sizeof(config->log_level[0])));
    } else if (text_equal(section, "dll")) {
        if (config->dll_count < YAFSML_MANAGER_MAX_ITEMS) {
            manager_dll_item_t *item = &config->dlls[config->dll_count++];
            utf8_to_wide(name, item->name, (int)(sizeof(item->name) / sizeof(item->name[0])));
            utf8_to_wide(value, item->path, (int)(sizeof(item->path) / sizeof(item->path[0])));
            item->condition[0] = L'\0';
            {
                wchar_t *separator = wcschr(item->path, L'|');
                if (separator != NULL) {
                    *separator = L'\0';
                    lstrcpynW(item->condition, separator + 1, (int)(sizeof(item->condition) / sizeof(item->condition[0])));
                }
            }
        }
    } else if (text_equal(section, "mod")) {
        if (config->mod_count < YAFSML_MANAGER_MAX_ITEMS) {
            manager_mod_item_t *item = &config->mods[config->mod_count++];
            utf8_to_wide(name, item->name, (int)(sizeof(item->name) / sizeof(item->name[0])));
            utf8_to_wide(value, item->path, (int)(sizeof(item->path) / sizeof(item->path[0])));
        }
    }
    return 1;
}

void manager_config_defaults(manager_config_t *config) {
    memset(config, 0, sizeof(*config));
    lstrcpyW(config->game, L"eldenring");
    config->skip_intro = true;
    config->prevent_regulation_save_write = true;
    config->patch_mem = true;
    config->boot_boost = true;
    lstrcpyW(config->log_level, L"warn");
}

bool manager_config_load(manager_config_t *config, const wchar_t *path) {
    FILE *file;
    long size;
    char *contents;
    size_t read_size;
    config_parse_state_t state;
    if (config == NULL || path == NULL) return false;
    manager_config_defaults(config);
    file = _wfopen(path, L"rb");
    if (file == NULL) return false;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return false; }
    size = ftell(file);
    if (size < 0 || size > 16 * 1024 * 1024) { fclose(file); return false; }
    rewind(file);
    contents = (char *)malloc((size_t)size + 1);
    if (contents == NULL) { fclose(file); return false; }
    read_size = fread(contents, 1, (size_t)size, file);
    fclose(file);
    contents[read_size] = '\0';
    state.config = config;
    config->parse_error = ini_parse_string(contents, config_handler, &state);
    free(contents);
    if (config->parse_error == 0) normalize_game(config->game);
    return config->parse_error == 0;
}

static bool write_wide(FILE *file, const wchar_t *value) {
    char *utf8;
    bool result = wide_to_utf8(value != NULL ? value : L"", &utf8);
    if (result) { fputs(utf8, file); free(utf8); }
    return result;
}

static bool write_key_value(FILE *file, const char *key, const wchar_t *value) {
    fputs(key, file);
    fputc('=', file);
    if (!write_wide(file, value)) return false;
    fputs("\r\n", file);
    return ferror(file) == 0;
}

bool manager_config_save(const manager_config_t *config, const wchar_t *path) {
    wchar_t temp_path[MAX_PATH];
    FILE *file;
    size_t i;
    bool ok = true;
    if (config == NULL || path == NULL || !manager_config_validate(config, temp_path, sizeof(temp_path) / sizeof(temp_path[0]))) return false;
    lstrcpynW(temp_path, path, MAX_PATH);
    lstrcatW(temp_path, L".tmp");
    file = _wfopen(temp_path, L"wb");
    if (file == NULL) return false;
    fputs("; Generated by YAFSML Manager\r\n\r\n", file);
    ok = write_key_value(file, "game", config->game);
    fputs("\r\n[patch]\r\n", file);
    ok = ok && write_key_value(file, "skip_intro", config->skip_intro ? L"1" : L"0");
    ok = ok && write_key_value(file, "prevent_regulation_save_write", config->prevent_regulation_save_write ? L"1" : L"0");
    ok = ok && write_key_value(file, "patch_mem", config->patch_mem ? L"1" : L"0");
    ok = ok && write_key_value(file, "patch_mem_dedicated_heap", config->patch_mem_dedicated_heap ? L"1" : L"0");
    { wchar_t number[32]; _snwprintf(number, 32, L"%u", config->patch_mem_heap_size); ok = ok && write_key_value(file, "patch_mem_heap_size", number); }
    ok = ok && write_key_value(file, "boot_boost", config->boot_boost ? L"1" : L"0");
    ok = ok && write_key_value(file, "disable_arxan", config->disable_arxan ? L"1" : L"0");
    ok = ok && write_key_value(file, "replace_save_filename", config->replace_save_filename);
    ok = ok && write_key_value(file, "replace_seamless_coop_save_filename", config->replace_seamless_coop_save_filename);
    ok = ok && write_key_value(file, "enable_ime", config->enable_ime ? L"1" : L"0");
    fputs("\r\n[tweak]\r\n", file);
    { wchar_t number[32]; _snwprintf(number, 32, L"%d", config->cpu_affinity); ok = ok && write_key_value(file, "cpu_affinity", number); }
    fputs("\r\n[log]\r\n", file);
    ok = ok && write_key_value(file, "console", config->console ? L"1" : L"0");
    ok = ok && write_key_value(file, "log_file", config->log_file ? L"1" : L"0");
    ok = ok && write_key_value(file, "log_level", config->log_level);
    fputs("\r\n[dll]\r\n", file);
    for (i = 0; i < config->dll_count && ok; i++) {
        char *name = NULL, *path_value = NULL;
        /* A path (1023 chars) plus the optional condition (255 chars), '|',
           and the terminator can exceed the old 1200-character buffer. */
        wchar_t combined[1024 + YAFSML_MANAGER_MAX_CONDITION + 2];
        if (!wide_to_utf8(config->dlls[i].name, &name) || !wide_to_utf8(config->dlls[i].path, &path_value)) { ok = false; free(name); free(path_value); break; }
        lstrcpynW(combined, config->dlls[i].path, (int)(sizeof(combined) / sizeof(combined[0])));
        if (config->dlls[i].condition[0] != L'\0') { lstrcatW(combined, L"|"); lstrcatW(combined, config->dlls[i].condition); }
        free(path_value);
        fputs(name, file); fputc('=', file); ok = write_wide(file, combined); fputs("\r\n", file); free(name);
    }
    fputs("\r\n[mod]\r\n", file);
    for (i = 0; i < config->mod_count && ok; i++) {
        char *name = NULL;
        if (!wide_to_utf8(config->mods[i].name, &name)) { ok = false; break; }
        fputs(name, file); fputc('=', file); ok = write_wide(file, config->mods[i].path); fputs("\r\n", file); free(name);
    }
    if (fclose(file) != 0) ok = false;
    if (!ok) { DeleteFileW(temp_path); return false; }
    {
        wchar_t backup[MAX_PATH];
        lstrcpynW(backup, path, MAX_PATH); lstrcatW(backup, L".bak");
        CopyFileW(path, backup, FALSE);
    }
    return MoveFileExW(temp_path, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

bool manager_config_validate(const manager_config_t *config, wchar_t *error, size_t error_count) {
    size_t i, j, k;
    bool edge[YAFSML_MANAGER_MAX_ITEMS][YAFSML_MANAGER_MAX_ITEMS] = { false };
    bool stage_seen;
    wchar_t condition[sizeof(config->dlls[0].condition) / sizeof(wchar_t)];
    wchar_t *context = NULL;
    wchar_t *token;
    if (error != NULL && error_count != 0) error[0] = L'\0';
    if (config == NULL || config->dll_count > YAFSML_MANAGER_MAX_ITEMS || config->mod_count > YAFSML_MANAGER_MAX_ITEMS ||
        !valid_game(config->game) || config->cpu_affinity < 0 || config->cpu_affinity > 4) goto invalid;
    if (!wide_equal(config->log_level, L"trace") && !wide_equal(config->log_level, L"debug") &&
        !wide_equal(config->log_level, L"info") && !wide_equal(config->log_level, L"warn") &&
        !wide_equal(config->log_level, L"error") && !wide_equal(config->log_level, L"off")) goto invalid;
    for (i = 0; i < config->dll_count; i++) {
        if (config->dlls[i].name[0] == L'\0' || config->dlls[i].path[0] == L'\0') goto invalid;
        for (j = 0; j < i; j++) if (wide_equal(config->dlls[i].name, config->dlls[j].name)) goto invalid;
    }
    for (i = 0; i < config->mod_count; i++) {
        if (config->mods[i].name[0] == L'\0' || config->mods[i].path[0] == L'\0') goto invalid;
        for (j = 0; j < i; j++) if (wide_equal(config->mods[i].name, config->mods[j].name)) goto invalid;
    }
    for (i = 0; i < config->dll_count; i++) {
        lstrcpynW(condition, config->dlls[i].condition, (int)(sizeof(condition) / sizeof(condition[0])));
        stage_seen = false;
        token = wcstok(condition, L"|", &context);
        while (token != NULL) {
            if (wide_equal(token, L"early") || wide_equal(token, L"data_ready") ||
                _wcsnicmp(token, L"delay,", 6) == 0) {
                if (stage_seen) goto invalid;
                stage_seen = true;
                if (_wcsnicmp(token, L"delay,", 6) == 0) {
                    wchar_t *end = NULL;
                    unsigned long delay = wcstoul(token + 6, &end, 10);
                    if (end == token + 6 || *end != L'\0' || delay > UINT32_MAX) goto invalid;
                }
            } else if (_wcsnicmp(token, L"after,", 6) == 0 && token[6] != L'\0') {
                for (j = 0; j < config->dll_count; j++) if (wide_equal(token + 6, config->dlls[j].name)) break;
                if (j == config->dll_count || j == i) goto invalid;
                edge[i][j] = true;
            } else if (token[0] != L'\0') goto invalid;
            token = wcstok(NULL, L"|", &context);
        }
    }
    for (k = 0; k < config->dll_count; k++) {
        for (i = 0; i < config->dll_count; i++) {
            for (j = 0; j < config->dll_count; j++) if (edge[i][k] && edge[k][j]) edge[i][j] = true;
        }
    }
    for (i = 0; i < config->dll_count; i++) if (edge[i][i]) goto invalid;
    return true;
invalid:
    if (error != NULL && error_count != 0) lstrcpynW(error, L"Invalid configuration values or duplicate dynamic entry names.", (int)error_count);
    return false;
}
