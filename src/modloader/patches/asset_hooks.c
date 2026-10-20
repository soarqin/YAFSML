/*
 * Copyright (C) 2026, Soar Qin<soarchin@gmail.com>
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#include "asset_hooks.h"
#include "asset_sig.h"

#include "log.h"

#include "common/allocator.h"
#include "game/game.h"

#ifndef ML_ASSET_HOOKS_TEST
#include "dl_device.h"

#include "modloader/config.h"
#include "modloader/dl_allocator.h"
#include "modloader/hook.h"
#include "modloader/hook_batch.h"
#include "modloader/lifecycle.h"
#include "modloader/mod.h"
#include "modloader/vfs.h"
#include "process/fd4_step.h"
#include "process/image.h"
#include "process/pe.h"
#include "process/rip_scan.h"
#include "process/rtti.h"
#include "process/singleton.h"

#include <MinHook.h>

#define kcalloc(N,Z) ml_mem_alloc(LPTR, (N) * (Z))
#define kmalloc(Z) ml_mem_alloc(0, (Z))
#define krealloc(P,Z) ((P) ? ml_mem_realloc((P), (Z), LMEM_MOVEABLE) : ml_mem_alloc(0, (Z)))
#define kfree(P) ml_mem_free(P)
#include "khash.h"
#include "khash_wstr.h"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string.h>

typedef struct bhd5_holder_s {
    uint8_t *header;
    uint32_t bucket_count;
    uint32_t *buckets;
} bhd5_holder_t;

static void assign_bhd_contents(bhd5_holder_t *holder, uint8_t *contents,
                                uint32_t bucket_count, uint32_t bucket_offset) {
    holder->header = contents;
    holder->bucket_count = bucket_count;
    holder->buckets = (uint32_t *)(contents + bucket_offset);
}

static const wchar_t *loose_param_paths[] = {
    L"data1:/param/gameparam/gameparam.parambnd.dcx",
    L"data1:/param/gameparam/gameparam_dlc1.parambnd.dcx",
    L"data1:/param/gameparam/gameparam_dlc2.parambnd.dcx",
};

bool ml_asset_hooks_requested(void) {
#ifdef ML_ASSET_HOOKS_TEST
    return false;
#else
    return vfs_entry_count() != 0;
#endif
}

bool ml_asset_hooks_loose_params_present(const ml_game_descriptor_t *game) {
    if (game == NULL || game->id != ML_GAME_DARK_SOULS_3) return false;
#ifdef ML_ASSET_HOOKS_TEST
    return false;
#else
    for (size_t i = 0; i < sizeof(loose_param_paths) / sizeof(loose_param_paths[0]); i++) {
        if (vfs_lookup(loose_param_paths[i]) != NULL) return true;
    }
    return false;
#endif
}

#ifndef ML_ASSET_HOOKS_TEST
typedef void *(__cdecl *dl_open_file_t)(ml_dl_device_t *, ml_dl_string_t *, const wchar_t *, void *, void *, bool);
typedef bool (__cdecl *dl_set_path_t)(ml_dl_file_operator_t *, ml_dl_string_t *, bool, bool);
typedef void *(__cdecl *make_ebl_object_t)(void *, const wchar_t *, void *);
typedef bool (__cdecl *mount_ebl_t)(const wchar_t *, const wchar_t *, const wchar_t *, void *, const char *, size_t);

static void *game_image_base;
static size_t game_image_size;
static ml_dl_device_manager_t *device_manager;
static ml_stl_abi_t game_stl_abi;
static fd4_step_fn_t old_file_step_init;
static make_ebl_object_t old_make_ebl_object;
static mount_ebl_t old_mount_ebl;
static bool pre_hooks_installed;
static bool post_hooks_installed;
static bool file_step_hook_installed;
static void *file_step_hook_target;
static bool file_step_assets_requested;
static SRWLOCK file_step_hook_lock = SRWLOCK_INIT;
static volatile LONG file_step_hook_active;
static fd4_step_fn_t old_render_ready_step_init;
static void *render_ready_hook_target;
static const wchar_t *render_ready_step_name;
static SRWLOCK render_ready_hook_lock = SRWLOCK_INIT;
static volatile LONG render_ready_hook_shutting_down;
static volatile LONG render_ready_hook_active;
static void *mount_ebl_hook_target;
static void *make_ebl_object_hook_target;

typedef struct ebl_open_hook_s {
    void *target;
    void *original;
} ebl_open_hook_t;

static bool remove_asset_hook(void *target);
static bool make_override_path(const ml_dl_string_t *path, ml_dl_string_t *replacement,
                               const wchar_t *route, bool streaming);
static void *__cdecl disk_open_file_hooked(ml_dl_device_t *device, ml_dl_string_t *path,
                                           const wchar_t *path_cstr, void *container,
                                           void *allocator, bool temporary);
static void remove_ebl_device_opens_from(size_t count);
/* Each detour calls the trampoline of the function it was installed on. MinHook
 * stores the trampoline before enabling the hook, so no call can see it unset. */
static dl_open_file_t old_disk_open_file;
static void *disk_open_hook_target;
static dl_set_path_t old_set_path[3];
static void *set_path_hook_targets[3];
/* One detour serves every BND4 device class, so it looks its trampoline up by
 * target. An entry is published before its hook is enabled. */
static ebl_open_hook_t ebl_open_hooks[64];
static volatile size_t ebl_open_hook_count;
static ml_dl_device_t *disk_device;
static SRWLOCK set_path_hook_lock = SRWLOCK_INIT;
static SRWLOCK override_log_lock = SRWLOCK_INIT;
static bool set_path_hook_attempted;
static bool set_path_hook_result;
static LONG logged_ebl_open;
static LONG logged_disk_open;
static LONG logged_disk_open_return;
static LONG logged_mount_ebl;
static LONG boot_boost_status_reported;
static khash_t(wstr) *logged_override_paths;
static volatile LONG set_path_hook_settled;

static bool pointer_in_text(const void *pointer) {
    const IMAGE_SECTION_HEADER *text = pe_section_by_name(game_image_base, ".text");
    return text != NULL && pe_section_contains_va(game_image_base, text, pointer);
}
#endif

#ifdef ML_ASSET_HOOKS_TEST
bool ml_asset_hooks_test_assign_bhd_contents(void *holder, uint8_t *contents,
                                             uint32_t bucket_count, uint32_t bucket_offset) {
    if (holder == NULL || contents == NULL) return false;
    assign_bhd_contents(holder, contents, bucket_count, bucket_offset);
    return true;
}
#endif

#ifndef ML_ASSET_HOOKS_TEST
static void *find_mount_ebl(void) {
    const IMAGE_SECTION_HEADER *text = pe_section_by_name(game_image_base, ".text");
    size_t text_size = 0;
    uint8_t *bytes = text == NULL ? NULL : pe_section_data(game_image_base, text, &text_size);
    void *fallback = NULL;
    if (bytes == NULL) return NULL;
    /* Like me3, the whole-function pattern is only a fallback for builds without
     * the more reliable call-site pattern anywhere in .text. */
    for (size_t i = 0; i < text_size; i++) {
        uint8_t *p = bytes + i;
        mount_pattern_match_t match;
        if (ml_asset_sig_match_mount_ebl(p, text_size - i, &match)) {
            void *target = (void *)ml_rel32_target(p + match.displacement_offset,
                                                   (uintptr_t)(p + match.instruction_end_offset));
            if (!pointer_in_text(target)) continue;
            if (match.call_site) {
                ML_LOG_DEBUG(L"asset-hooks", L"MountEbl call-site signature matched at %p target=%p", p, target);
                return target;
            }
            if (fallback == NULL) fallback = target;
        }
    }
    if (fallback != NULL) ML_LOG_DEBUG(L"asset-hooks", L"MountEbl function signature matched target=%p", fallback);
    return fallback;
}
#endif

#ifndef ML_ASSET_HOOKS_TEST
static bool write_boot_boost_stub(const uint8_t *source, size_t size, size_t block_size,
                                  wchar_t *path, size_t path_capacity) {
    wchar_t directory[MAX_PATH];
    HANDLE file;
    DWORD written;
    size_t count;
    DWORD directory_length = GetTempPathW(MAX_PATH, directory);
    if (source == NULL || size == 0 || path == NULL || path_capacity == 0 ||
        directory_length == 0 || directory_length >= MAX_PATH ||
        GetTempFileNameW(directory, L"me3", 0, path) == 0) return false;
    count = block_size < size ? block_size : size;
    file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, NULL);
    if (file == INVALID_HANDLE_VALUE || !WriteFile(file, source, (DWORD)count, &written, NULL) || written != count) {
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        DeleteFileW(path);
        return false;
    }
    CloseHandle(file);
    return true;
}

/* Returns whether the archive is mounted. `*handled` reports whether a mount
 * through `original` was attempted, so the caller must not retry the plain one. */
static bool boot_boost_mount(const wchar_t *bhd_path, dl_allocator_t *allocator, mount_ebl_t original,
                             const wchar_t *mount_name, const wchar_t *bdt_path,
                             const char *rsa_key, size_t key_len, bool *handled) {
    wchar_t *cache_directory = NULL;
    wchar_t *cache_path = NULL;
    wchar_t *expanded = NULL;
    wchar_t stub_path[MAX_PATH] = { 0 };
    HANDLE source = INVALID_HANDLE_VALUE;
    LARGE_INTEGER source_size = { 0 };
    uint8_t *encrypted = NULL;
    DWORD read = 0;
    uint64_t key[2];
    size_t block_size;
    bool result = false;
    ml_dl_mount_snapshot_t snapshot = { 0 };
    ml_dl_vfs_mounts_t stub_mounts = { 0 };
    ml_dl_vfs_mounts_t full_mounts = { 0 };
    ml_dl_device_manager_guard_t guard = { 0 };
    *handled = false;
    ML_LOG_DEBUG(L"asset-hooks", L"BootBoost enter: bhd=%p allocator=%p key=%p key_len=%zu",
                 bhd_path, allocator, rsa_key, key_len);
    /* The PEM length comes from the string: like me3 notes, key_len can be off
     * by one (sd.bhd in Elden Ring). */
    if (bhd_path == NULL || allocator == NULL || device_manager == NULL || rsa_key == NULL ||
        !ml_asset_sig_rsa_block_size(rsa_key, strlen(rsa_key), &block_size)) return false;
    ML_LOG_DEBUG(L"asset-hooks", L"BootBoost RSA block size: %zu", block_size);
    if (!ml_dl_device_manager_lock(device_manager, &guard) ||
        !ml_dl_device_expand_path(device_manager, game_stl_abi, bhd_path, &expanded)) goto done;
    ML_LOG_DEBUG(L"asset-hooks", L"BootBoost expanded BHD path: %ls", expanded);
    source = CreateFileW(expanded, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (source == INVALID_HANDLE_VALUE || !GetFileSizeEx(source, &source_size) || source_size.QuadPart <= 0 ||
        source_size.QuadPart > 64 * 1024 * 1024) goto done;
    encrypted = ml_mem_alloc(0, (size_t)source_size.QuadPart);
    if (encrypted == NULL || !ReadFile(source, encrypted, (DWORD)source_size.QuadPart, &read, NULL) ||
        read != (DWORD)source_size.QuadPart || !ml_boot_boost_cache_key(encrypted, (size_t)source_size.QuadPart, key)) goto done;
    ML_LOG_DEBUG(L"asset-hooks", L"BootBoost source loaded: %lld bytes", source_size.QuadPart);
    CloseHandle(source); source = INVALID_HANDLE_VALUE;
    cache_directory = config_full_path_alloc(L"cache");
    cache_path = ml_boot_boost_cache_path(cache_directory, key);
    if (cache_directory == NULL || cache_path == NULL) goto done;
    ML_LOG_DEBUG(L"asset-hooks", L"BootBoost cache path: %ls", cache_path);
    CreateDirectoryW(cache_directory, NULL);
    if (!write_boot_boost_stub(encrypted, (size_t)source_size.QuadPart, block_size, stub_path, MAX_PATH) ||
        !ml_dl_mount_snapshot(device_manager, game_stl_abi, &snapshot) || !ml_dl_mounts_init(&stub_mounts)) goto done;
    ml_dl_device_manager_unlock(&guard);
    ML_LOG_TRACE(L"asset-hooks", L"BootBoost invoking stub MountEbl: %ls", stub_path);
    result = original(mount_name, stub_path, bdt_path, allocator, rsa_key, key_len);
    *handled = result;
    ML_LOG_TRACE(L"asset-hooks", L"BootBoost stub MountEbl returned: %d", result ? 1 : 0);
    DeleteFileW(stub_path);
    if (!result || !ml_dl_device_manager_lock(device_manager, &guard)) goto done;
    if (!ml_dl_device_extract_new(device_manager, game_stl_abi, &snapshot, &stub_mounts)) goto done;
    ML_LOG_TRACE(L"asset-hooks", L"BootBoost stub mounts extracted: %zu", stub_mounts.count);
    /* No new mount to read the size from: mount the full archive, like me3. */
    if (stub_mounts.count == 0) goto full_mount;
    {
        ml_dl_virtual_mount_t *mount = &stub_mounts.items[0];
        const ml_game_descriptor_t *game = ml_game_context_get();
        bhd5_holder_t *holder;
        uint32_t size;
        uint8_t *cached;
        if (game == NULL || game->ebl_bhd_holder_offset == 0) goto full_mount;
        holder = (bhd5_holder_t *)((uint8_t *)mount->device + game->ebl_bhd_holder_offset);
        ML_LOG_TRACE(L"asset-hooks", L"BootBoost stub holder: device=%p holder=%p header=%p",
                     mount->device, holder, holder->header);
        if (holder->header == NULL || memcmp(holder->header, "BHD5", 4) != 0 ||
            (memcpy(&size, holder->header + 12, sizeof(size)), size < 24 || size > 64 * 1024 * 1024)) goto full_mount;
        ML_LOG_TRACE(L"asset-hooks", L"BootBoost stub header valid: size=%u allocator=%p vtable=%p",
                     size, allocator, allocator->vtable);
        cached = allocator->vtable->allocate_aligned(allocator, size, 4096);
        ML_LOG_TRACE(L"asset-hooks", L"BootBoost cache buffer: %p", cached);
        if (cached != NULL) {
            if (ml_boot_boost_cache_load(cache_path, key, cached, size)) {
                uint32_t bucket_count;
                uint32_t bucket_offset;
                ML_LOG_INFO(L"asset-hooks", L"BootBoost cache hit");
                /* ml_boot_boost_cache_load validated these against `size`. */
                memcpy(&bucket_count, cached + 16, sizeof(bucket_count));
                memcpy(&bucket_offset, cached + 20, sizeof(bucket_offset));
                if (!ml_dl_device_push_mounts_permanent(device_manager, game_stl_abi, &stub_mounts)) {
                    dl_allocator_dealloc(allocator, cached);
                    *handled = false;
                    result = false;
                    goto done;
                }
                assign_bhd_contents(holder, cached, bucket_count, bucket_offset);
                goto done;
            }
            ML_LOG_INFO(L"asset-hooks", L"BootBoost cache miss; doing full mount");
            dl_allocator_dealloc(allocator, cached);
        }
    }
full_mount:
    ML_LOG_DEBUG(L"asset-hooks", L"BootBoost entering full mount");
    result = false;
    if (stub_mounts.count != 0) {
        /* Match me3: the stub device remains detached while the full mount is
           attempted. Its game-owned root string must not be freed here. */
        ml_dl_mounts_release(&stub_mounts);
        ml_dl_mount_snapshot_destroy(&snapshot);
        if (!ml_dl_mount_snapshot(device_manager, game_stl_abi, &snapshot)) {
            *handled = false;
            goto done;
        }
    }
    if (!ml_dl_mounts_init(&full_mounts)) goto done;
    ml_dl_device_manager_unlock(&guard);
    ML_LOG_TRACE(L"asset-hooks", L"BootBoost invoking full MountEbl");
    result = original(mount_name, bhd_path, bdt_path, allocator, rsa_key, key_len);
    if (result) *handled = true;
    ML_LOG_TRACE(L"asset-hooks", L"BootBoost full MountEbl returned: %d", result ? 1 : 0);
    /* From here on the archive is mounted; failures only skip the cache. */
    if (!result || !ml_dl_device_manager_lock(device_manager, &guard)) goto done;
    ML_LOG_TRACE(L"asset-hooks", L"BootBoost extracting full mounts");
    if (!ml_dl_device_extract_new(device_manager, game_stl_abi, &snapshot, &full_mounts)) goto done;
    ML_LOG_TRACE(L"asset-hooks", L"BootBoost full mounts extracted: %zu", full_mounts.count);
    if (full_mounts.count == 0) goto done;
    {
        ml_dl_virtual_mount_t *mount = &full_mounts.items[0];
        const ml_game_descriptor_t *game = ml_game_context_get();
        bhd5_holder_t *holder;
        uint32_t size;
        holder = game == NULL || game->ebl_bhd_holder_offset == 0 ? NULL
            : (bhd5_holder_t *)((uint8_t *)mount->device + game->ebl_bhd_holder_offset);
        if (holder != NULL && holder->header != NULL && memcmp(holder->header, "BHD5", 4) == 0 &&
            (memcpy(&size, holder->header + 12, sizeof(size)), size >= 24 && size <= 64 * 1024 * 1024) &&
            ml_bhd5_header_valid(holder->header, size)) {
            (void)ml_boot_boost_cache_store(cache_path, key, holder->header, size);
        }
    }
    ML_LOG_TRACE(L"asset-hooks", L"BootBoost full mount cache-store stage");
    if (!ml_dl_device_push_mounts_permanent(device_manager, game_stl_abi, &full_mounts)) {
        *handled = false;
        result = false;
        goto done;
    }
    ML_LOG_DEBUG(L"asset-hooks", L"BootBoost full mount retained");
done:
    if (source != INVALID_HANDLE_VALUE) CloseHandle(source);
    DeleteFileW(stub_path);
    ml_mem_free(encrypted);
    ml_mem_free(cache_directory);
    ml_mem_free(cache_path);
    ml_mem_free(expanded);
    ml_dl_mounts_destroy(&stub_mounts, game_stl_abi);
    ml_dl_mounts_destroy(&full_mounts, game_stl_abi);
    ml_dl_mount_snapshot_destroy(&snapshot);
    ml_dl_device_manager_unlock(&guard);
    return result;
}

/* Log each overridden physical path once.
 *
 * Every caller passes an interned VFS path (`entries[i].path`, reached through
 * vfs_uid_to_path / vfs_lookup), so distinct files are always distinct strings
 * and a hash set partitions them exactly as the previous case-insensitive
 * comparison did -- without the O(logged) scan under an exclusive lock that this
 * used to cost on every override hit. */
static void log_override_once(const wchar_t *route, const wchar_t *requested,
                              const wchar_t *expanded, const wchar_t *physical) {
    bool added = false;
    khiter_t slot;
    if (physical == NULL) return;
    AcquireSRWLockShared(&override_log_lock);
    if (logged_override_paths != NULL) {
        slot = kh_get(wstr, logged_override_paths, physical);
        if (slot != kh_end(logged_override_paths)) {
            ReleaseSRWLockShared(&override_log_lock);
            return;
        }
    }
    ReleaseSRWLockShared(&override_log_lock);

    AcquireSRWLockExclusive(&override_log_lock);
    if (logged_override_paths == NULL) logged_override_paths = kh_init(wstr);
    if (logged_override_paths != NULL) {
        int ret;
        slot = kh_put(wstr, logged_override_paths, physical, &ret);
        if (ret > 0) {
            wchar_t *copy = ml_mem_strdup_w(physical);
            if (copy == NULL) {
                kh_del(wstr, logged_override_paths, slot);
            } else {
                kh_key(logged_override_paths, slot) = copy;
                added = true;
            }
        }
    }
    ReleaseSRWLockExclusive(&override_log_lock);
    if (added) {
        ML_LOG_INFO(L"asset-hooks", L"file override [%ls]: request=%ls expanded=%ls mod=%ls",
                    route, requested, expanded, physical);
    }
}

static void clear_override_log(void) {
    AcquireSRWLockExclusive(&override_log_lock);
    if (logged_override_paths != NULL) {
        for (khiter_t slot = kh_begin(logged_override_paths);
             slot != kh_end(logged_override_paths); slot++) {
            if (kh_exist(logged_override_paths, slot)) {
                ml_mem_free((void *)kh_key(logged_override_paths, slot));
            }
        }
        kh_destroy(wstr, logged_override_paths);
        logged_override_paths = NULL;
    }
    ReleaseSRWLockExclusive(&override_log_lock);
}

static bool make_override_path(const ml_dl_string_t *path, ml_dl_string_t *replacement,
                               const wchar_t *route, bool streaming) {
    ml_dl_device_manager_guard_t guard = { 0 };
    wchar_t *expanded = NULL;
    const wchar_t *uid;
    const wchar_t *physical;
    bool expanded_ok;
    bool result = false;
    const wchar_t *raw = ml_dl_string_data(path, game_stl_abi);
    if (raw == NULL || device_manager == NULL || wcsncmp(raw, L"\\\\me3??", 7) == 0 ||
        !ml_dl_device_manager_lock(device_manager, &guard)) return false;
    /* Only the expansion reads device manager state; every other thread that
     * opens an asset waits on this lock. */
    expanded_ok = ml_dl_device_expand_path(device_manager, game_stl_abi, raw, &expanded);
    ml_dl_device_manager_unlock(&guard);
    if (!expanded_ok) return false;
    uid = mods_file_virtual_to_uid_prefixed(expanded);
    if (uid == NULL) uid = vfs_virtual_to_uid(expanded);
    /* The BND4/EBL caller has no seek operation. Preserve that origin through
     * the disk-device redirect; ordinary file handles use the decoded cache. */
    if (streaming && uid != NULL) uid = vfs_uid_for_streaming(uid);
    physical = uid == NULL ? NULL : vfs_uid_to_path(uid);
    if (physical != NULL) {
        result = ml_dl_string_clone_replace(path, game_stl_abi, uid, replacement);
        if (result) log_override_once(route, raw, expanded, physical);
    }
    ml_mem_free(expanded);
    return result;
}

static bool install_asset_hook(void *target, void *detour, void **original) {
    return target != NULL && pointer_in_text(target) &&
           ml_hook_install(target, detour, original) == ML_HOOK_APPLIED;
}

static bool hook_file_operator(ml_dl_file_operator_t *file_operator);

static void *__cdecl disk_open_file_hooked(ml_dl_device_t *device, ml_dl_string_t *path,
                                            const wchar_t *path_cstr, void *container, void *allocator, bool temporary) {
    void *result;
    ml_dl_string_t replacement = { 0 };
    bool overridden = make_override_path(path, &replacement, L"disk open_file", false);
    dl_open_file_t original = old_disk_open_file;
    if (ReadAcquire(&logged_disk_open) == 0 && InterlockedCompareExchange(&logged_disk_open, 1, 0) == 0) {
        ML_LOG_DEBUG(L"asset-hooks", L"disk open_file enter: device=%p path=%p path_cstr=%p container=%p allocator=%p temporary=%d",
                     device, path, path_cstr, container, allocator, temporary ? 1 : 0);
    }
    if (overridden) {
        path = &replacement;
        path_cstr = ml_dl_string_data(&replacement, game_stl_abi);
    }
    if (original == NULL) { ml_dl_string_destroy(&replacement, game_stl_abi); return NULL; }
    result = original(device, path, path_cstr, container, allocator, temporary);
    ml_dl_string_destroy(&replacement, game_stl_abi);
    if (ReadAcquire(&logged_disk_open_return) == 0 &&
        InterlockedCompareExchange(&logged_disk_open_return, 1, 0) == 0) {
        ML_LOG_DEBUG(L"asset-hooks", L"disk open_file returned: operator=%p", result);
    }
    if (result != NULL) (void)hook_file_operator(result);
    return result;
}

static dl_open_file_t ebl_open_original(void *target) {
    const size_t count = ebl_open_hook_count;
    for (size_t i = 0; i < count; i++) {
        if (ebl_open_hooks[i].target == target) return (dl_open_file_t)ebl_open_hooks[i].original;
    }
    return NULL;
}

static void *__cdecl ebl_open_file_hooked(ml_dl_device_t *device, ml_dl_string_t *path,
                                          const wchar_t *path_cstr, void *container, void *allocator, bool temporary) {
    ml_dl_string_t replacement = { 0 };
    dl_open_file_t original = device == NULL || device->vtable == NULL ? NULL :
                              ebl_open_original(device->vtable->open_file);
    if (ReadAcquire(&logged_ebl_open) == 0 && InterlockedCompareExchange(&logged_ebl_open, 1, 0) == 0) {
        ML_LOG_DEBUG(L"asset-hooks", L"BND4 open_file enter: device=%p path=%ls original=%p",
                     device, path_cstr == NULL ? L"<null>" : path_cstr, original);
    }
    if (make_override_path(path, &replacement, L"BND4 open_file", true)) {
        void *result;
        const wchar_t *replacement_cstr = ml_dl_string_data(&replacement, game_stl_abi);
        result = disk_open_file_hooked(disk_device, &replacement, replacement_cstr,
                                       container, allocator, temporary);
        ml_dl_string_destroy(&replacement, game_stl_abi);
        return result;
    }
    return original == NULL ? NULL : original(device, path, path_cstr, container, allocator, temporary);
}

static bool hook_ebl_device_opens(void) {
    size_t mount_count;
    size_t start_count = ebl_open_hook_count;
    ml_dl_virtual_mount_t *mounts;
    if (device_manager == NULL || disk_device == NULL) return false;
    mount_count = ml_dl_vector_count(&device_manager->bnd4_mounts, game_stl_abi, sizeof(ml_dl_virtual_mount_t));
    mounts = game_stl_abi == ML_STL_ABI_MSVC2012
        ? (ml_dl_virtual_mount_t *)device_manager->bnd4_mounts.msvc2012.first
        : (ml_dl_virtual_mount_t *)device_manager->bnd4_mounts.msvc2015.first;
    for (size_t i = 0; i < mount_count; i++) {
        void *target;
        size_t count = ebl_open_hook_count;
        bool already_hooked = false;
        if (mounts[i].device == NULL || mounts[i].device->vtable == NULL) continue;
        target = mounts[i].device->vtable->open_file;
        for (size_t j = 0; j < count; j++) {
            if (ebl_open_hooks[j].target == target) already_hooked = true;
        }
        if (already_hooked) continue;
        if (count == sizeof(ebl_open_hooks) / sizeof(ebl_open_hooks[0])) {
            remove_ebl_device_opens_from(start_count);
            return false;
        }
        ebl_open_hooks[count] = (ebl_open_hook_t){ target, NULL };
        ebl_open_hook_count = count + 1;
        if (ml_hook_install(target, ebl_open_file_hooked, &ebl_open_hooks[count].original) != ML_HOOK_APPLIED) {
            ebl_open_hook_count = count;
            ebl_open_hooks[count] = (ebl_open_hook_t){ 0 };
            remove_ebl_device_opens_from(start_count);
            return false;
        }
        ML_LOG_DEBUG(L"asset-hooks", L"BND4 open_file hook applied: target=%p", target);
    }
    ML_LOG_DEBUG(L"asset-hooks", L"BND4 open_file hooks active: mounts=%zu hooks=%zu",
                 mount_count, ebl_open_hook_count);
    return true;
}

static void remove_ebl_device_opens_from(size_t count) {
    while (ebl_open_hook_count > count) {
        size_t last = ebl_open_hook_count - 1;
        (void)remove_asset_hook(ebl_open_hooks[last].target);
        ebl_open_hook_count = last;
        ebl_open_hooks[last] = (ebl_open_hook_t){ 0 };
    }
}

static bool set_path_with_override(ml_dl_file_operator_t *file_operator, ml_dl_string_t *path,
                                   bool p3, bool p4, size_t index) {
    dl_set_path_t original = old_set_path[index];
    ml_dl_string_t replacement = { 0 };
    bool overridden = make_override_path(path, &replacement, L"DlFileOperator::SetPath", false);
    bool result;
    if (overridden) path = &replacement;
    result = original != NULL && original(file_operator, path, p3, p4);
    ml_dl_string_destroy(&replacement, game_stl_abi);
    return result;
}

static bool __cdecl set_path_hooked(ml_dl_file_operator_t *file_operator, ml_dl_string_t *path, bool p3, bool p4) {
    return set_path_with_override(file_operator, path, p3, p4, 0);
}

static bool __cdecl set_path2_hooked(ml_dl_file_operator_t *file_operator, ml_dl_string_t *path, bool p3, bool p4) {
    return set_path_with_override(file_operator, path, p3, p4, 1);
}

static bool __cdecl set_path3_hooked(ml_dl_file_operator_t *file_operator, ml_dl_string_t *path, bool p3, bool p4) {
    return set_path_with_override(file_operator, path, p3, p4, 2);
}

static bool hook_file_operator(ml_dl_file_operator_t *file_operator) {
    void *targets[3];
    void *detours[3] = { set_path_hooked, set_path2_hooked, set_path3_hooked };
    bool result = true;
    /* This runs once per process but is called from every disk open_file return,
     * so keep the settled case off the exclusive lock. `set_path_hook_settled` is
     * published after `set_path_hook_result` is final, and both are only ever
     * written under the exclusive lock. */
    if (ReadAcquire(&set_path_hook_settled) != 0) return set_path_hook_result;
    AcquireSRWLockExclusive(&set_path_hook_lock);
    if (set_path_hook_attempted) {
        result = set_path_hook_result;
        ReleaseSRWLockExclusive(&set_path_hook_lock);
        return result;
    }
    set_path_hook_attempted = true;
    if (file_operator == NULL || file_operator->vtable == NULL ||
        !pointer_in_text(file_operator->vtable->set_path) ||
        !pointer_in_text(file_operator->vtable->set_path2) ||
        !pointer_in_text(file_operator->vtable->set_path3)) {
        ML_LOG_WARN(L"asset-hooks", L"DlFileOperator layout validation failed: operator=%p vtable=%p",
                    file_operator, file_operator == NULL ? NULL : file_operator->vtable);
        set_path_hook_result = false;
        InterlockedExchange(&set_path_hook_settled, 1);
        ReleaseSRWLockExclusive(&set_path_hook_lock);
        return false;
    }
    targets[0] = file_operator->vtable->set_path;
    targets[1] = file_operator->vtable->set_path2;
    targets[2] = file_operator->vtable->set_path3;
    ML_LOG_DEBUG(L"asset-hooks", L"DlFileOperator layout: operator=%p vtable=%p set_path=%p set_path2=%p set_path3=%p",
                 file_operator, file_operator->vtable, targets[0], targets[1], targets[2]);
    for (size_t i = 0; i < 3; i++) {
        bool shared = false;
        ML_LOG_TRACE(L"asset-hooks", L"installing DlFileOperator::SetPath%zu hook", i + 1);
        /* A function behind several slots is hooked once; its detour serves all. */
        for (size_t j = 0; j < i; j++) {
            if (targets[j] == targets[i] && set_path_hook_targets[j] != NULL) {
                old_set_path[i] = old_set_path[j];
                shared = true;
            }
        }
        if (shared) {
            ML_LOG_DEBUG(L"asset-hooks", L"DlFileOperator::SetPath%zu shares an applied hook", i + 1);
        } else if (!install_asset_hook(targets[i], detours[i], (void **)&old_set_path[i])) {
            ML_LOG_WARN(L"asset-hooks", L"DlFileOperator::SetPath%zu hook failed", i + 1);
            result = false;
        } else {
            set_path_hook_targets[i] = targets[i];
            ML_LOG_DEBUG(L"asset-hooks", L"DlFileOperator::SetPath%zu hook applied", i + 1);
        }
    }
    set_path_hook_result = result;
    InterlockedExchange(&set_path_hook_settled, 1);
    ReleaseSRWLockExclusive(&set_path_hook_lock);
    return result;
}

static void *__cdecl make_ebl_object_hooked(void *utility, const wchar_t *path, void *allocator) {
    ml_dl_device_manager_guard_t guard = { 0 };
    wchar_t *expanded = NULL;
    bool expanded_ok = false;
    if (device_manager != NULL && path != NULL && ml_dl_device_manager_lock(device_manager, &guard)) {
        expanded_ok = ml_dl_device_expand_path(device_manager, game_stl_abi, path, &expanded);
        ml_dl_device_manager_unlock(&guard);
    }
    if (expanded_ok) {
        const wchar_t *physical = mods_file_search_prefixed_domain(expanded, VFS_LOOKUP_VIRTUAL);
        if (physical == NULL) physical = vfs_lookup(expanded);
        if (physical != NULL) log_override_once(L"MakeEblObject", path, expanded, physical);
        ml_mem_free(expanded);
        /* The game falls back to its disk resolution, where the disk-device hook
           serves the loose file. */
        if (physical != NULL) return NULL;
    }
    return old_make_ebl_object(utility, path, allocator);
}

static void hook_mounted_ebl_device_opens(const wchar_t *context) {
    ml_dl_device_manager_guard_t guard = { 0 };
    bool hooked = ml_dl_device_manager_lock(device_manager, &guard) && hook_ebl_device_opens();
    ml_dl_device_manager_unlock(&guard);
    if (!hooked) ML_LOG_WARN(L"asset-hooks", L"failed to hook %ls BND4 device opens", context);
}

/* Mounts stay resident in the device manager, so every BND4 device mounted after
 * FileStep needs its open_file hooked for override precedence. */
static bool __cdecl mount_ebl_hooked(const wchar_t *mount_name, const wchar_t *bhd_path, const wchar_t *bdt_path,
                                     void *allocator, const char *rsa_key, size_t key_len) {
    bool result;
    ML_LOG_DEBUG(L"asset-hooks", L"MountEbl enter: mount=%p bhd=%p bdt=%p allocator=%p key=%p key_len=%zu boot_boost=%d",
                 mount_name, bhd_path, bdt_path, allocator, rsa_key, key_len, config.boot_boost ? 1 : 0);
    if (config.boot_boost) {
        bool handled = false;
        result = boot_boost_mount(bhd_path, (dl_allocator_t *)allocator, old_mount_ebl,
                                  mount_name, bdt_path, rsa_key, key_len, &handled);
        if (result) {
            ML_LOG_INFO(L"asset-hooks", L"MountEbl BootBoost result: 1");
            if (InterlockedCompareExchange(&boot_boost_status_reported, 1, 0) == 0) {
                ML_LOG_INFO(L"asset-hooks", L"BootBoost capability APPLIED");
            }
            hook_mounted_ebl_device_opens(L"BootBoost");
            return true;
        }
        if (handled) return false;
    }
    ML_LOG_TRACE(L"asset-hooks", L"MountEbl calling original");
    result = old_mount_ebl(mount_name, bhd_path, bdt_path, allocator, rsa_key, key_len);
    if (config.boot_boost && result && InterlockedCompareExchange(&boot_boost_status_reported, 1, 0) == 0) {
        ML_LOG_INFO(L"asset-hooks", L"BootBoost capability FALLBACK");
    }
    if (result) hook_mounted_ebl_device_opens(L"mounted");
    ML_LOG_INFO(L"asset-hooks", L"MountEbl original returned: %d", result ? 1 : 0);
    if (result && InterlockedCompareExchange(&logged_mount_ebl, 1, 0) == 0) {
        ML_LOG_INFO(L"asset-hooks", L"MountEbl completed: %ls", mount_name);
    }
    return result;
}

static bool install_pre_hooks(void) {
    void *mount;
    if (pre_hooks_installed) return true;
    device_manager = ml_dl_device_manager_find(game_image_base, game_stl_abi);
    if (device_manager == NULL) {
        ML_LOG_WARN(L"asset-hooks", L"DlDeviceManager validation failed; archive device hooks disabled");
        return false;
    }
    /* Scan before installing anything, so a miss leaves no hook behind. */
    mount = find_mount_ebl();
    if (mount == NULL) {
        ML_LOG_WARN(L"asset-hooks", L"MountEbl signature not found");
        return false;
    }
    disk_device = device_manager->disk_device;
    if (!install_asset_hook(disk_device->vtable->open_file, disk_open_file_hooked, (void **)&old_disk_open_file)) {
        ML_LOG_WARN(L"asset-hooks", L"disk open_file hook failed");
        disk_device = NULL;
        return false;
    }
    disk_open_hook_target = disk_device->vtable->open_file;
    ML_LOG_INFO(L"asset-hooks", L"disk open_file hook applied");
    if (ml_hook_install(mount, mount_ebl_hooked, (void **)&old_mount_ebl) != ML_HOOK_APPLIED) {
        ML_LOG_WARN(L"asset-hooks", L"MountEbl hook failed");
        (void)remove_asset_hook(disk_open_hook_target);
        disk_open_hook_target = NULL;
        disk_device = NULL;
        return false;
    }
    mount_ebl_hook_target = mount;
    ML_LOG_INFO(L"asset-hooks", L"MountEbl hook applied");
    pre_hooks_installed = true;
    return true;
}

static bool install_post_hooks(void) {
    void **vtable;
    void *target = NULL;
    if (post_hooks_installed) return true;
    vtable = rtti_find_vtable("DLEBL::DLEncryptedBinderLightUtility");
    if (vtable != NULL) target = vtable[1];
    if (target == NULL) {
        void **manager = singleton_find("CSEblFileManager");
        const IMAGE_SECTION_HEADER *rdata = pe_section_by_name(game_image_base, ".rdata");
        if (manager != NULL && rdata != NULL) {
            void *interface_pointer = pe_section_contains_va(game_image_base, rdata, manager[0])
                ? manager[1] : manager[0];
            void **utility = interface_pointer == NULL ? NULL : *(void ***)interface_pointer;
            if (utility != NULL) target = utility[1];
        }
    }
    if (target != NULL && pointer_in_text(target) &&
        ml_hook_install(target, make_ebl_object_hooked, (void **)&old_make_ebl_object) == ML_HOOK_APPLIED) {
        make_ebl_object_hook_target = target;
        ML_LOG_INFO(L"asset-hooks", L"MakeEblObject hook applied");
    } else {
        ML_LOG_WARN(L"asset-hooks", L"MakeEblObject RTTI/hook failed");
        return false;
    }
    {
        ml_dl_device_manager_guard_t guard = { 0 };
        size_t previous_open_count = ebl_open_hook_count;
        bool hooked = ml_dl_device_manager_lock(device_manager, &guard) && hook_ebl_device_opens();
        ml_dl_device_manager_unlock(&guard);
        if (!hooked) {
            ML_LOG_WARN(L"asset-hooks", L"initial BND4 device open hooks failed");
            remove_ebl_device_opens_from(previous_open_count);
            (void)remove_asset_hook(make_ebl_object_hook_target);
            make_ebl_object_hook_target = NULL;
            old_make_ebl_object = NULL;
            return false;
        }
    }
    post_hooks_installed = true;
    return true;
}

static void __cdecl file_step_init_hooked(void *this_ptr, fd4_time_t *time) {
    fd4_step_fn_t original;
    bool assets_requested;
    bool pre_hooks_applied = false;
    bool post_hooks_applied = false;
    InterlockedIncrement(&file_step_hook_active);
    AcquireSRWLockShared(&file_step_hook_lock);
    original = old_file_step_init;
    assets_requested = file_step_assets_requested;
    if (assets_requested) {
        vfs_begin_lookup_reset();
        pre_hooks_applied = install_pre_hooks();
    }
    if (original != NULL) original(this_ptr, time);
    if (assets_requested) {
        const unsigned long long generation = (unsigned long long)vfs_reset_lookup_caches();
        ML_LOG_INFO(L"asset-hooks", L"VFS cache generation %llu enabled", generation);
        if (pre_hooks_applied) post_hooks_applied = install_post_hooks();
        ML_LOG_INFO(L"asset-hooks", L"asset capability %ls",
                    pre_hooks_applied && post_hooks_applied ? L"APPLIED" : L"HOOK_FAILED");
    }
    ReleaseSRWLockShared(&file_step_hook_lock);
    InterlockedDecrement(&file_step_hook_active);
}

static void __cdecl render_ready_step_init_hooked(void *this_ptr, fd4_time_t *time) {
    fd4_step_fn_t original;
    InterlockedIncrement(&render_ready_hook_active);
    AcquireSRWLockShared(&render_ready_hook_lock);
    original = old_render_ready_step_init;
    if (original != NULL) original(this_ptr, time);
    if (InterlockedCompareExchange(&render_ready_hook_shutting_down, 0, 0) == 0) {
        ML_LOG_INFO(L"asset-hooks", L"AFTER_RENDER_READY reached at %ls",
                    render_ready_step_name == NULL ? L"<unknown>" : render_ready_step_name);
        if (!ml_lifecycle_advance(ML_LIFECYCLE_PHASE_AFTER_RENDER_READY)) {
            ML_LOG_WARN(L"asset-hooks", L"AFTER_RENDER_READY lifecycle advance failed");
        }
    }
    ReleaseSRWLockShared(&render_ready_hook_lock);
    InterlockedDecrement(&render_ready_hook_active);
}

bool ml_asset_hooks_install_render_ready(const ml_game_descriptor_t *game) {
    void *step;
    ml_hook_result_t result;
    if (render_ready_hook_target != NULL) return true;
    if (game == NULL || game->render_ready_step_name == NULL) return false;
    if (game->render_ready_strategy != ML_RENDER_READY_STEP_AFTER_ORIGINAL) return false;
    game_image_base = get_module_image_base(NULL, &game_image_size);
    if (game_image_base == NULL || game_image_size == 0) return false;
    game_stl_abi = game->stl_abi;
    step = fd4_step_find(game->render_ready_step_name);
    if (step == NULL) {
        ML_LOG_WARN(L"asset-hooks", L"%ls late-ready hook SIGNATURE_NOT_FOUND",
                    game->render_ready_step_name);
        return false;
    }
    if (!ml_hook_pin_module()) {
        ML_LOG_WARN(L"asset-hooks", L"could not pin late-ready hook module");
        return false;
    }
    render_ready_step_name = game->render_ready_step_name;
    result = ml_hook_install(step, render_ready_step_init_hooked,
                             (void **)&old_render_ready_step_init);
    if (result != ML_HOOK_APPLIED) {
        ML_LOG_WARN(L"asset-hooks", L"%ls late-ready hook %hs",
                    game->render_ready_step_name, ml_hook_result_name(result));
        render_ready_step_name = NULL;
        return false;
    }
    render_ready_hook_target = step;
    ML_LOG_INFO(L"asset-hooks", L"%ls late-ready hook applied",
                game->render_ready_step_name);
    return true;
}

bool ml_asset_hooks_install(const ml_game_descriptor_t *game, void *image_base, size_t image_size) {
    void *step;
    const wchar_t *step_name;
    if (game == NULL || game->file_step_name == NULL || image_base == NULL || image_size == 0 ||
        vfs_entry_count() == 0) return true;
    file_step_assets_requested = true;
    if (file_step_hook_installed) return true;
    game_image_base = image_base;
    game_image_size = image_size;
    game_stl_abi = game->stl_abi;
    step_name = game->file_step_name;
    step = fd4_step_find(step_name);
    if (step == NULL && wcscmp(step_name, L"SprjFileStep::STEP_Init") != 0) {
        step_name = L"SprjFileStep::STEP_Init";
        step = fd4_step_find(step_name);
    }
    if (step == NULL) {
        ML_LOG_WARN(L"asset-hooks", L"%ls and SprjFileStep::STEP_Init not found for %ls; Dantelion VFS disabled",
                    game->file_step_name, game->title);
        return false;
    }
    if (!ml_hook_pin_module()) {
        ML_LOG_WARN(L"asset-hooks", L"could not pin FileStep hook module");
        return false;
    }
    if (ml_hook_install(step, file_step_init_hooked, (void **)&old_file_step_init) != ML_HOOK_APPLIED) {
        ML_LOG_WARN(L"asset-hooks", L"%ls hook failed for %ls",
                    step_name, game->title);
        return false;
    }
    file_step_hook_installed = true;
    file_step_hook_target = step;
    ML_LOG_INFO(L"asset-hooks", L"%ls hook applied for %ls", step_name, game->title);
    return true;
}

static bool remove_asset_hook(void *target) {
    return ml_hook_remove(target, L"asset-hooks");
}

bool ml_asset_hooks_uninstall(void) {
    bool result = true;
    InterlockedExchange(&render_ready_hook_shutting_down, 1);
    AcquireSRWLockExclusive(&file_step_hook_lock);
    if (!ml_hook_disable(file_step_hook_target, L"asset-hooks")) result = false;
    ReleaseSRWLockExclusive(&file_step_hook_lock);
    AcquireSRWLockExclusive(&render_ready_hook_lock);
    if (!ml_hook_disable(render_ready_hook_target, L"asset-hooks")) result = false;
    ReleaseSRWLockExclusive(&render_ready_hook_lock);
    while (InterlockedCompareExchange(&file_step_hook_active, 0, 0) != 0) Sleep(0);
    while (InterlockedCompareExchange(&render_ready_hook_active, 0, 0) != 0) Sleep(0);
    if (!result) return false;

    if (!remove_asset_hook(file_step_hook_target)) result = false;
    if (!remove_asset_hook(render_ready_hook_target)) result = false;
    if (!result) return false;
    old_file_step_init = NULL;
    old_render_ready_step_init = NULL;
    file_step_hook_target = NULL;
    render_ready_hook_target = NULL;
    render_ready_step_name = NULL;
    if (!remove_asset_hook(make_ebl_object_hook_target)) result = false;
    if (!remove_asset_hook(mount_ebl_hook_target)) result = false;
    if (!ml_hook_targets_remove_unique(set_path_hook_targets, 3, remove_asset_hook)) result = false;
    if (!remove_asset_hook(disk_open_hook_target)) result = false;
    for (size_t i = 0; i < ebl_open_hook_count; i++) {
        if (!remove_asset_hook(ebl_open_hooks[i].target)) result = false;
    }
    if (!result) return false;

    memset(old_set_path, 0, sizeof(old_set_path));
    memset(set_path_hook_targets, 0, sizeof(set_path_hook_targets));
    memset(ebl_open_hooks, 0, sizeof(ebl_open_hooks));
    ebl_open_hook_count = 0;
    old_disk_open_file = NULL;
    disk_open_hook_target = NULL;
    game_image_base = NULL;
    game_image_size = 0;
    game_stl_abi = ML_STL_ABI_MSVC2015;
    device_manager = NULL;
    disk_device = NULL;
    old_file_step_init = NULL;
    old_make_ebl_object = NULL;
    old_mount_ebl = NULL;
    file_step_hook_target = NULL;
    file_step_assets_requested = false;
    InterlockedExchange(&render_ready_hook_shutting_down, 0);
    mount_ebl_hook_target = NULL;
    make_ebl_object_hook_target = NULL;
    pre_hooks_installed = false;
    post_hooks_installed = false;
    file_step_hook_installed = false;
    logged_ebl_open = 0;
    logged_disk_open = 0;
    logged_disk_open_return = 0;
    logged_mount_ebl = 0;
    boot_boost_status_reported = 0;
    clear_override_log();
    InterlockedExchange(&set_path_hook_settled, 0);
    set_path_hook_attempted = false;
    set_path_hook_result = false;
    return true;
}
#endif
