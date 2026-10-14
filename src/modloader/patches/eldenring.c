/*
 * Copyright (C) 2024,2025,2026, Soar Qin<soarchin@gmail.com>

 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#include "eldenring.h"

#include "modloader/hook.h"
#include "modloader/mod.h"

#include "log.h"

#include "process/rip_scan.h"
#include "process/scanner.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlwapi.h>

#include <stdint.h>
#include <string.h>

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

static map_archive_path_t old_map_archive_path = NULL;
static void *archive_resolver_hook_target;

/* Elden Ring resolves archive-relative virtual paths ("dataN:/...") through this
 * function; rewrite the prefix to a loose on-disk path when a mod provides the
 * file. This is ER-specific and unrelated to the Win32 file API hooks. */
static void *__cdecl map_archive_path(er_wstring_local_t *path, const uint64_t p2, const uint64_t p3,
                                      const uint64_t p4, const uint64_t p5, const uint64_t p6) {
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

bool eldenring_archive_resolver_install(void *image_base, size_t image_size) {
    uint8_t *addr = sig_scan(image_base, image_size, "48 83 7B 20 08 48 8D 4B 08 72 03 48 8B 09 4C 8B 4B 18 41 B8 05 00 00 00 4D 3B C8");
    ml_hook_result_t result;
    if (!addr) {
        ML_LOG_WARN(L"eldenring", L"archive position resolver signature not found");
        return false;
    }
    /* The signature starts right after a call's rel32; resolve it and follow
     * jump thunks to the resolver itself. */
    addr = (uint8_t *)ml_rel32_target(addr - 4, (uintptr_t)addr);
    while (*addr == 0xE9) addr = (uint8_t *)ml_rel32_target(addr + 1, (uintptr_t)addr + 5);
    result = ml_hook_install(addr, (void *)&map_archive_path, (void **)&old_map_archive_path);
    if (result == ML_HOOK_APPLIED) archive_resolver_hook_target = addr;
    ml_log_write(result == ML_HOOK_APPLIED ? ML_LOG_LEVEL_INFO : ML_LOG_LEVEL_WARN,
                 L"eldenring", L"archive position resolver hook %hs", ml_hook_result_name(result));
    return result == ML_HOOK_APPLIED;
}

void eldenring_archive_resolver_uninstall(void) {
    if (archive_resolver_hook_target != NULL && ml_hook_remove(archive_resolver_hook_target, L"eldenring")) {
        archive_resolver_hook_target = NULL;
        old_map_archive_path = NULL;
    }
}
