/*
 * Copyright (C) 2024, Soar Qin<soarchin@gmail.com>

 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* patches */
    bool skip_intro;
    bool prevent_regulation_save_write;
    bool patch_mem;
    bool patch_mem_dedicated_heap;
    uint32_t patch_mem_heap_size;
    bool boot_boost;
    bool disable_arxan;
    wchar_t replaced_save_filename[64];
    wchar_t replaced_seamless_coop_save_filename[64];
    bool enable_ime;

    /* tweak */
    int cpu_affinity_strategy;
} config_t;

extern void config_init(void *module);
extern void config_load();

extern const wchar_t *module_path();
extern void config_full_path(wchar_t *path, const wchar_t *filename);
extern wchar_t *config_full_path_alloc(const wchar_t *filename);

extern config_t config;

#ifdef __cplusplus
}
#endif
