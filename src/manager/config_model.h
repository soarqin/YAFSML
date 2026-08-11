#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <wchar.h>

#define YAFSML_MANAGER_MAX_ITEMS 128
#define YAFSML_MANAGER_MAX_CONDITION 256

typedef struct manager_dll_item_s {
    wchar_t name[64];
    wchar_t path[1024];
    wchar_t condition[YAFSML_MANAGER_MAX_CONDITION];
} manager_dll_item_t;

typedef struct manager_mod_item_s {
    wchar_t name[64];
    wchar_t path[1024];
} manager_mod_item_t;

typedef struct manager_config_s {
    wchar_t game[32];
    bool skip_intro;
    bool prevent_regulation_save_write;
    bool patch_mem;
    bool patch_mem_dedicated_heap;
    uint32_t patch_mem_heap_size;
    bool boot_boost;
    bool disable_arxan;
    wchar_t replace_save_filename[64];
    wchar_t replace_seamless_coop_save_filename[64];
    bool enable_ime;
    int cpu_affinity;
    bool console;
    bool log_file;
    wchar_t log_level[16];
    manager_dll_item_t dlls[YAFSML_MANAGER_MAX_ITEMS];
    size_t dll_count;
    manager_mod_item_t mods[YAFSML_MANAGER_MAX_ITEMS];
    size_t mod_count;
    size_t unknown_count;
    int parse_error;
} manager_config_t;

void manager_config_defaults(manager_config_t *config);
bool manager_config_load(manager_config_t *config, const wchar_t *path);
bool manager_config_save(const manager_config_t *config, const wchar_t *path);
bool manager_config_validate(const manager_config_t *config, wchar_t *error, size_t error_count);

