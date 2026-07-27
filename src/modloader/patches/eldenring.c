/*
 * Copyright (C) 2024,2025,2026, Soar Qin<soarchin@gmail.com>

 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#include "eldenring.h"

#include "asset_hooks.h"
#include "common.h"
#include "runtime_ready.h"

#include "modloader/hook.h"
#include "modloader/lifecycle.h"
#include "modloader/mod.h"

#include "log.h"

#include "game/game.h"

#include "process/image.h"
#include "process/scanner.h"
#include <MinHook.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlwapi.h>

#include <stdint.h>
#include <stddef.h>

typedef struct er_wstring_local_s {
    void *unk;
    wchar_t *string;
    void *unk2;
    uint64_t length;
    uint64_t capacity;
} er_wstring_local_t;

static wchar_t *wstring_str_mutable(er_wstring_local_t *str) {
    return (sizeof(wchar_t) * str->capacity >= 16) ? str->string : (wchar_t*)&str->string;
}

typedef void *(__cdecl *map_archive_path_t)(er_wstring_local_t *path, uint64_t p2, uint64_t p3, uint64_t p4, uint64_t p5, uint64_t p6);
static void *image_base;
static size_t image_size;

static map_archive_path_t old_map_archive_path = NULL;
static void *archive_resolver_hook_target;

/* Elden Ring resolves archive-relative virtual paths ("dataN:/...") through this
 * function; rewrite the prefix to a loose on-disk path when a mod provides the
 * file. This is ER-specific and unrelated to the Win32 file API hooks. */
void *__cdecl map_archive_path(er_wstring_local_t *path, const uint64_t p2, const uint64_t p3, const uint64_t p4, const uint64_t p5, const uint64_t p6) {
    void *res = old_map_archive_path(path, p2, p3, p4, p5, p6);
    if (path == NULL) return res;
    wchar_t *str = wstring_str_mutable(path);
    if (StrCmpNW(str, L"data", 4) == 0 && StrCmpNW(str + 5, L":/", 2) == 0) {
        const wchar_t *replace = mods_file_search(str + 6);
        if (replace == NULL) return res;
        memcpy(str, L"./////", 6 * sizeof(wchar_t));
    }
    return res;
}

static bool hook_eldenring_archive_position_resolver(void) {
    uint8_t *addr = sig_scan(image_base, image_size, "48 83 7B 20 08 48 8D 4B 08 72 03 48 8B 09 4C 8B 4B 18 41 B8 05 00 00 00 4D 3B C8");
    ml_hook_result_t result;
    if (!addr) {
        ML_LOG_WARN(L"eldenring", L"archive position resolver signature not found");
        return false;
    }
    addr += *(int32_t*)(addr - 4);
    while (*addr == 0xE9) {
        addr += *(int32_t*)(addr + 1) + 5;
    }
    result = ml_hook_install(addr, (void *)&map_archive_path, (void **)&old_map_archive_path);
    if (result == ML_HOOK_APPLIED) archive_resolver_hook_target = addr;
    ml_log_write(result == ML_HOOK_APPLIED ? ML_LOG_LEVEL_INFO : ML_LOG_LEVEL_WARN,
                 L"eldenring", L"archive position resolver hook %hs", ml_hook_result_name(result));
    return result == ML_HOOK_APPLIED;
}

static BOOL CALLBACK install_after_runtime(PINIT_ONCE once, PVOID parameter, PVOID *context) {
    bool assets_applied = true;
    (void)once;
    (void)parameter;
    (void)context;
    if (mods_count() > 0) {
        assets_applied = ml_asset_hooks_install(ml_game_context_get(), image_base, image_size);
    }
    if (!ml_asset_hooks_install_game_data_ready(ml_game_context_get())) {
        ML_LOG_WARN(L"eldenring", L"AFTER_GAME_DATA_READY trigger HOOK_FAILED; CPU affinity deferred capability disabled");
    }
    if (!ml_lifecycle_advance(ML_LIFECYCLE_PHASE_AFTER_RUNTIME_INIT)) {
        ML_LOG_WARN(L"eldenring", L"AFTER_RUNTIME_INIT lifecycle advance failed");
    }
    return assets_applied;
}

bool eldenring_install(void) {
    const ml_game_descriptor_t *game = ml_game_context_get();
    int modcount;
    if (game == NULL || game->id != ML_GAME_ELDEN_RING) return false;

    image_base = get_module_image_base(NULL, &image_size);
    modcount = mods_count();
    if (image_base == NULL || image_size == 0 ||
        !ml_runtime_ready_hook_install(L"eldenring", install_after_runtime)) return false;
    /* Do not hook the archive resolver if no mod is added, to improve game
     * performance during loading. */
    if (modcount > 0) {
        if (!hook_eldenring_archive_position_resolver()) return false;
    }
    return true;
}

void eldenring_uninstall(void) {
    if (!ml_asset_hooks_uninstall()) {
        ML_LOG_WARN(L"eldenring", L"one or more asset hooks could not be removed");
    }

    if (archive_resolver_hook_target != NULL) {
        MH_STATUS status = MH_RemoveHook(archive_resolver_hook_target);
        if (status == MH_OK || status == MH_ERROR_NOT_CREATED) {
            archive_resolver_hook_target = NULL;
            old_map_archive_path = NULL;
        } else {
            ML_LOG_WARN(L"eldenring", L"failed to remove archive resolver hook at %p: %d",
                        archive_resolver_hook_target, status);
        }
    }

    if (ml_runtime_ready_hook_uninstall(L"eldenring")) {
        image_base = NULL;
        image_size = 0;
    }
}
