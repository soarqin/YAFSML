/*
 * Copyright (C) 2024,2025, Soar Qin<soarchin@gmail.com>

 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#include "common.h"
#include "common/allocator.h"
#include "modloader/hook.h"
#include "wwise_path.h"
#include "save_mapping.h"
#include "win32_hooks.h"

#include "modloader/config.h"
#include "log.h"
#include "modloader/mod.h"
#include "modloader/vfs.h"

#include "process/rtti.h"
#include "process/pe.h"
#include "process/util.h"

#include <MinHook.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlwapi.h>

#include <stdint.h>

BOOL WINAPI ImmDisableIME_hooked(DWORD unused) {
    (void)unused;
    return TRUE;
}

static void *ime_hook_target;

static bool patch_ime_disable() {
    void *func = NULL;
    MH_STATUS status = MH_CreateHookApiEx(L"imm32", "ImmDisableIME", ImmDisableIME_hooked, NULL, &func);
    if (status != MH_OK || func == NULL) return false;
    status = MH_EnableHook(func);
    if (status != MH_OK) {
        MH_RemoveHook(func);
        return false;
    }
    ime_hook_target = func;
    return true;
}

typedef enum ak_open_mode_e {
    READ            = 0,
    WRITE           = 1,
    WRITE_OVERWRITE = 2,
    READ_WRITE      = 3,

    // Custom mode specific to From Software's implementation
    READ_EBL = 10,
} ak_open_mode_t;

_Static_assert(READ == 0, "AkOpenMode READ value");
_Static_assert(WRITE == 1, "AkOpenMode WRITE value");
_Static_assert(WRITE_OVERWRITE == 2, "AkOpenMode WRITE_OVERWRITE value");
_Static_assert(READ_WRITE == 3, "AkOpenMode READ_WRITE value");
_Static_assert(READ_EBL == 10, "AkOpenMode READ_EBL value");

typedef void *(__cdecl *ak_file_location_resolver_open_t)(uint64_t p1, wchar_t *path, ak_open_mode_t openMode, uint64_t p4, uint64_t p5, uint64_t p6);
static ak_file_location_resolver_open_t old_ak_file_location_resolver_open = NULL;
static void *wwise_hook_target;

typedef struct dlmow_io_hook_blocking_vtable_s {
    void *dtor;
    void *open_by_id;
    ak_file_location_resolver_open_t open_by_name;
} dlmow_io_hook_blocking_vtable_t;

/* Look up `prefix + middle + path` in the Wwise VFS domain.
 *
 * Wwise streams audio continuously, so this builds the candidate in a stack
 * buffer and only falls back to the heap for paths that do not fit. The lookup
 * result is an interned VFS pointer, never a pointer into the candidate buffer. */
static const wchar_t *wwise_lookup_candidate(const wchar_t *prefix, const wchar_t *middle,
                                             const wchar_t *path) {
    wchar_t stack[512];
    wchar_t *heap = NULL;
    const wchar_t *result;
    size_t capacity = sizeof(stack) / sizeof(stack[0]);
    size_t needed = wwise_join3(stack, capacity, prefix, middle, path);
    if (needed == SIZE_MAX) return NULL;
    if (needed + 1 > capacity) {
        heap = ml_mem_alloc(0, (needed + 1) * sizeof(*heap));
        if (heap == NULL) return NULL;
        if (wwise_join3(heap, needed + 1, prefix, middle, path) != needed) {
            ml_mem_free(heap);
            return NULL;
        }
    }
    result = vfs_lookup_domain(heap != NULL ? heap : stack, VFS_LOOKUP_WWISE);
    ml_mem_free(heap);
    return result;
}

void *__cdecl ak_file_location_resolver_open(const uint64_t p1, wchar_t *path, const ak_open_mode_t openMode, const uint64_t p4, const uint64_t p5, const uint64_t p6) {
    static const wchar_t *prefixes[3] = {
        L"sd/",
        L"sd/enus/",
        L"sd/ja/",
    };
    const wchar_t *replace = wwise_strip_prefixes(path);
    if (replace == NULL)
        return old_ak_file_location_resolver_open(p1, path, openMode, p4, p5, p6);
    const wchar_t *ext = PathFindExtensionW(replace);
    if (ext != NULL && StrCmpIW(ext, L".wem") == 0 && replace[0] != L'\0' && replace[1] != L'\0') {
        wchar_t bucket[WWISE_WEM_BUCKET_SIZE];
        const wchar_t *new_replace = NULL;
        wwise_wem_bucket(replace, bucket);
        for (int i = 0; i < 3 && new_replace == NULL; i++) {
            new_replace = wwise_lookup_candidate(prefixes[i], L"wem/", replace);
        }
        for (int i = 0; i < 3 && new_replace == NULL; i++) {
            new_replace = wwise_lookup_candidate(prefixes[i], bucket, replace);
        }
        if (new_replace != NULL) {
        /* FromSoftware's READ_EBL (9) mode yields an EBLFileOperator that
         * only reads from BDT archives. An override file lives on disk, so
         * we must switch back to READ (0) to get a FileOperator that reads
         * the absolute disk path we pass in. See ModEngine2's
         * wwise_file_overrides.cpp for the same rationale. */
            return old_ak_file_location_resolver_open(p1, (wchar_t*)new_replace, READ, p4, p5, p6);
        }
    }
    for (int i = 0; i < 3; i++) {
        const wchar_t *new_replace = wwise_lookup_candidate(prefixes[i], NULL, replace);
        if (new_replace != NULL) {
            return old_ak_file_location_resolver_open(p1, (wchar_t*)new_replace, READ, p4, p5, p6);
        }
    }
    return old_ak_file_location_resolver_open(p1, path, openMode, p4, p5, p6);
}

static bool hook_wwise_archive_position_resolver() {
    void **vtable = rtti_find_vtable("DLMOW::IOHookBlocking");
    ak_file_location_resolver_open_t open_by_name = vtable == NULL ? NULL :
        ((dlmow_io_hook_blocking_vtable_t *)vtable)->open_by_name;
    if (open_by_name == NULL) {
        void *image = GetModuleHandleW(NULL);
        const IMAGE_SECTION_HEADER *text = pe_section_by_name(image, ".text");
        size_t text_size = 0;
        uint8_t *text_base = pe_section_data(image, text, &text_size);
        size_t call_offset = wwise_find_open_call(text_base, text_size);
        if (call_offset != SIZE_MAX) {
            int32_t displacement;
            void *candidate;
            memcpy(&displacement, text_base + call_offset + 1, sizeof(displacement));
            candidate = text_base + call_offset + 5 + displacement;
            /* A false-positive CALL match could resolve anywhere; only accept a
             * target that lands inside .text before hooking it. */
            if (pe_section_contains_va(image, text, candidate)) {
                open_by_name = (ak_file_location_resolver_open_t)candidate;
                ML_LOG_INFO(L"common", L"Wwise resolver found by signature fallback at %p", open_by_name);
            } else {
                ML_LOG_WARN(L"common", L"Wwise resolver fallback target %p rejected (outside .text)", candidate);
            }
        }
    }
    if (open_by_name == NULL) {
        ML_LOG_WARN(L"common", L"Wwise resolver SIGNATURE_NOT_FOUND: RTTI and fallback scan found no target");
        return false;
    }
    ml_hook_result_t result = ml_hook_install((void *)open_by_name, (void *)&ak_file_location_resolver_open, (void **)&old_ak_file_location_resolver_open);
    if (result != ML_HOOK_APPLIED) {
        ML_LOG_WARN(L"common", L"Wwise archive resolver hook %hs", ml_hook_result_name(result));
        return false;
    }
    wwise_hook_target = (void *)open_by_name;
    return true;
}

void common_apply_process_settings(void) {
    uint64_t applied_mask = 0;
    uint32_t error_code = ERROR_SUCCESS;
    if (config.cpu_affinity_strategy != 0) {
        if (set_process_cpu_affinity_strategy(config.cpu_affinity_strategy,
                                              &applied_mask, &error_code)) {
            ML_LOG_INFO(L"common", L"CPU affinity strategy %d APPLIED mask=0x%llx",
                        config.cpu_affinity_strategy,
                        (unsigned long long)applied_mask);
        } else {
            ML_LOG_WARN(L"common", L"CPU affinity strategy %d SKIPPED error=%lu",
                        config.cpu_affinity_strategy,
                        (unsigned long)error_code);
        }
    }
}

static DWORD WINAPI apply_process_settings_worker(void *parameter) {
    (void)parameter;
    ML_LOG_INFO(L"common", L"CPU affinity worker started thread=%lu",
                (unsigned long)GetCurrentThreadId());
    common_apply_process_settings();
    return 0;
}

static SRWLOCK process_settings_worker_lock = SRWLOCK_INIT;
static HANDLE process_settings_worker;

bool common_schedule_process_settings(void) {
    DWORD thread_id;
    HANDLE thread;
    if (config.cpu_affinity_strategy == 0) return true;
    AcquireSRWLockExclusive(&process_settings_worker_lock);
    if (process_settings_worker != NULL) {
        ReleaseSRWLockExclusive(&process_settings_worker_lock);
        return true;
    }
    thread = CreateThread(NULL, 0, apply_process_settings_worker, NULL, 0,
                          &thread_id);
    if (thread == NULL) {
        ReleaseSRWLockExclusive(&process_settings_worker_lock);
        ML_LOG_WARN(L"common", L"could not create CPU affinity worker error=%lu",
                    (unsigned long)GetLastError());
        return false;
    }
    process_settings_worker = thread;
    ReleaseSRWLockExclusive(&process_settings_worker_lock);
    ML_LOG_INFO(L"common", L"CPU affinity strategy %d scheduled asynchronously worker=%lu",
                config.cpu_affinity_strategy, (unsigned long)thread_id);
    return true;
}

void common_wait_for_process_settings(void) {
    HANDLE thread;
    AcquireSRWLockExclusive(&process_settings_worker_lock);
    thread = process_settings_worker;
    process_settings_worker = NULL;
    ReleaseSRWLockExclusive(&process_settings_worker_lock);
    if (thread != NULL) {
        (void)WaitForSingleObject(thread, INFINITE);
        CloseHandle(thread);
    }
}

bool common_install_file_routing(const ml_game_descriptor_t *game) {
    bool want_main_save = config.replaced_save_filename[0] != L'\0';
    bool want_additional_save = config.replaced_seamless_coop_save_filename[0] != L'\0';
    bool save_mapping_applied = true;
    bool needs_file_hooks = mods_count() > 0 || want_main_save || want_additional_save;

    if (!want_main_save && !want_additional_save) {
        ML_LOG_INFO(L"common", L"save mapping NOT_REQUESTED");
    } else if (!ml_save_mapping_init_root(game)) {
        ML_LOG_WARN(L"common", L"save mapping initialization failed");
        save_mapping_applied = false;
    } else {
        if (want_main_save &&
            !ml_save_mapping_add_extension(L".sl2", config.replaced_save_filename)) {
            ML_LOG_WARN(L"common", L"main save mapping registration failed");
            save_mapping_applied = false;
        }
        if (want_additional_save &&
            !ml_save_mapping_add_extension(L".co2", config.replaced_seamless_coop_save_filename)) {
            ML_LOG_WARN(L"common", L"additional save mapping registration failed");
            save_mapping_applied = false;
        }
    }
    if (!needs_file_hooks) {
        ML_LOG_INFO(L"common", L"Win32 VFS hooks NOT_REQUESTED");
        return save_mapping_applied;
    }
    if (!ml_win32_file_hooks_install()) {
        ML_LOG_WARN(L"common", L"Win32 VFS hook installation failed");
        return false;
    }
    return save_mapping_applied;
}

void common_uninstall_file_routing(void) {
    ml_win32_file_hooks_uninstall();
    ml_save_mapping_uninit();
}

bool common_install_ime(void) {
    if (config.enable_ime) {
        return ime_hook_target != NULL || patch_ime_disable();
    }
    return true;
}

bool common_wwise_requested(void) {
    return vfs_has_wwise_entries();
}

bool common_install_wwise(void) {
    if (!common_wwise_requested()) return true;
    if (wwise_hook_target != NULL) return true;
    if (!hook_wwise_archive_position_resolver()) return false;
    return true;
}

void common_uninstall(void) {
    void **targets[2] = { &wwise_hook_target, &ime_hook_target };
    common_wait_for_process_settings();
    for (size_t i = 0; i < 2; i++) {
        void *target = *targets[i];
        if (target == NULL) continue;
        MH_STATUS status = MH_RemoveHook(target);
        if (status == MH_OK || status == MH_ERROR_NOT_CREATED) {
            *targets[i] = NULL;
        } else {
            ML_LOG_WARN(L"common", L"failed to remove hook at %p: %d", target, status);
        }
    }
    if (wwise_hook_target == NULL) old_ak_file_location_resolver_open = NULL;
}
