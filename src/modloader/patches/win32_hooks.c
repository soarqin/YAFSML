#include "win32_hooks.h"
#include "common/allocator.h"
#include "log.h"

#include <MinHook.h>

#include "save_mapping.h"

#include "modloader/hook.h"
#include "modloader/hook_batch.h"
#include "modloader/mod.h"
#include "modloader/path_convert.h"
#include "modloader/vfs.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

/*
 * File-API hooks used for VFS routing and save-file remapping.
 *
 * Hooks CreateFile{W,A,2}, CreateDirectory{W,A,ExW}, DeleteFile{W,A} and the
 * CopyFile{W,ExW,2,A,ExA}, MoveFileEx and ReplaceFile families on
 * kernelbase.dll. Read/open calls route through the VFS and save mapping; the
 * copy/move/replace family routes save paths through the save mapping so the
 * game's save backup rotation (e.g. .sl2 <-> .sl2.bak) stays consistent with
 * the redirected save.
 *
 * Every hook routes the path(s) and calls the real API exactly once. Paths that
 * do not match a mapping remain unchanged; save paths whose explicit mapping
 * cannot be produced fail closed instead of opening the primary save.
 */

#define ML_WIN32_HOOK_COUNT 15

typedef HANDLE (WINAPI *create_file_w_t)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef HANDLE (WINAPI *create_file_a_t)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef HANDLE (WINAPI *create_file_2_t)(LPCWSTR, DWORD, DWORD, DWORD, LPCREATEFILE2_EXTENDED_PARAMETERS);
typedef BOOL (WINAPI *delete_file_w_t)(LPCWSTR);
typedef BOOL (WINAPI *delete_file_a_t)(LPCSTR);
typedef BOOL (WINAPI *create_directory_w_t)(LPCWSTR, LPSECURITY_ATTRIBUTES);
typedef BOOL (WINAPI *create_directory_a_t)(LPCSTR, LPSECURITY_ATTRIBUTES);
typedef BOOL (WINAPI *create_directory_ex_w_t)(LPCWSTR, LPCWSTR, LPSECURITY_ATTRIBUTES);
typedef BOOL (WINAPI *copy_file_w_t)(LPCWSTR, LPCWSTR, BOOL);
typedef BOOL (WINAPI *copy_file_ex_w_t)(LPCWSTR, LPCWSTR, LPPROGRESS_ROUTINE, LPVOID, LPBOOL, DWORD);
typedef HRESULT (WINAPI *copy_file_2_t)(PCWSTR, PCWSTR, COPYFILE2_EXTENDED_PARAMETERS *);
typedef BOOL (WINAPI *copy_file_a_t)(LPCSTR, LPCSTR, BOOL);
typedef BOOL (WINAPI *copy_file_ex_a_t)(LPCSTR, LPCSTR, LPPROGRESS_ROUTINE, LPVOID, LPBOOL, DWORD);
typedef BOOL (WINAPI *move_file_ex_w_t)(LPCWSTR, LPCWSTR, DWORD);
typedef BOOL (WINAPI *replace_file_w_t)(LPCWSTR, LPCWSTR, LPCWSTR, DWORD, LPVOID, LPVOID);

static create_file_w_t old_create_file_w;
static create_file_a_t old_create_file_a;
static create_file_2_t old_create_file_2;
static delete_file_w_t old_delete_file_w;
static delete_file_a_t old_delete_file_a;
static create_directory_w_t old_create_directory_w;
static create_directory_a_t old_create_directory_a;
static create_directory_ex_w_t old_create_directory_ex_w;
static copy_file_w_t old_copy_file_w;
static copy_file_ex_w_t old_copy_file_ex_w;
static copy_file_2_t old_copy_file_2;
static copy_file_a_t old_copy_file_a;
static copy_file_ex_a_t old_copy_file_ex_a;
static move_file_ex_w_t old_move_file_ex_w;
static replace_file_w_t old_replace_file_w;
static bool hooks_installed;
static void *hook_targets[ML_WIN32_HOOK_COUNT];

static HANDLE WINAPI create_file_w_hooked(LPCWSTR path, DWORD access, DWORD share,
                                           LPSECURITY_ATTRIBUTES security, DWORD disposition,
                                           DWORD flags, HANDLE template_file);
static HANDLE WINAPI create_file_a_hooked(LPCSTR path, DWORD access, DWORD share,
                                           LPSECURITY_ATTRIBUTES security, DWORD disposition,
                                           DWORD flags, HANDLE template_file);
static HANDLE WINAPI create_file_2_hooked(LPCWSTR path, DWORD access, DWORD share, DWORD disposition,
                                           LPCREATEFILE2_EXTENDED_PARAMETERS parameters);
static BOOL WINAPI delete_file_w_hooked(LPCWSTR path);
static BOOL WINAPI delete_file_a_hooked(LPCSTR path);
static BOOL WINAPI create_directory_w_hooked(LPCWSTR path, LPSECURITY_ATTRIBUTES security);
static BOOL WINAPI create_directory_a_hooked(LPCSTR path, LPSECURITY_ATTRIBUTES security);
static BOOL WINAPI create_directory_ex_w_hooked(LPCWSTR template_path, LPCWSTR path,
                                                  LPSECURITY_ATTRIBUTES security);
static BOOL WINAPI copy_file_w_hooked(LPCWSTR existing_path, LPCWSTR new_path, BOOL fail_if_exists);
static BOOL WINAPI copy_file_ex_w_hooked(LPCWSTR existing_path, LPCWSTR new_path,
                                         LPPROGRESS_ROUTINE progress, LPVOID data,
                                         LPBOOL cancel, DWORD flags);
static HRESULT WINAPI copy_file_2_hooked(PCWSTR existing_path, PCWSTR new_path,
                                         COPYFILE2_EXTENDED_PARAMETERS *parameters);
static BOOL WINAPI copy_file_a_hooked(LPCSTR existing_path, LPCSTR new_path, BOOL fail_if_exists);
static BOOL WINAPI copy_file_ex_a_hooked(LPCSTR existing_path, LPCSTR new_path,
                                         LPPROGRESS_ROUTINE progress, LPVOID data,
                                         LPBOOL cancel, DWORD flags);
static BOOL WINAPI move_file_ex_w_hooked(LPCWSTR existing_path, LPCWSTR new_path, DWORD flags);
static BOOL WINAPI replace_file_w_hooked(LPCWSTR replaced_path, LPCWSTR replacement_path,
                                         LPCWSTR backup_path, DWORD flags,
                                         LPVOID exclude, LPVOID reserved);

static bool remove_hook(void *target) {
    MH_STATUS status = MH_RemoveHook(target);
    if (status == MH_OK || status == MH_ERROR_NOT_CREATED) return true;
    ML_LOG_WARN(L"win32-vfs", L"failed to remove hook at %p: %d", target, status);
    return false;
}

static ml_hook_result_t install_hook(void *target, void *detour, void **original) {
    ml_hook_result_t result = ml_hook_install(target, detour, original);
    if (result != ML_HOOK_APPLIED) {
        ML_LOG_WARN(L"win32-vfs", L"failed to install hook at %p: %hs",
                    target, ml_hook_result_name(result));
    }
    return result;
}

static void *kernelbase_proc(HMODULE kernelbase, const char *name, void *fallback) {
    void *target = kernelbase == NULL ? NULL : (void *)GetProcAddress(kernelbase, name);
    return target != NULL ? target : fallback;
}

static void build_hook_specs(ml_hook_spec_t specs[ML_WIN32_HOOK_COUNT], bool resolve_targets) {
    if (resolve_targets) {
        HMODULE kernelbase = GetModuleHandleW(L"kernelbase.dll");
        hook_targets[0] = kernelbase_proc(kernelbase, "CreateFileW", CreateFileW);
        hook_targets[1] = kernelbase_proc(kernelbase, "CreateFileA", CreateFileA);
        hook_targets[2] = kernelbase_proc(kernelbase, "CreateFile2", CreateFile2);
        hook_targets[3] = kernelbase_proc(kernelbase, "DeleteFileW", DeleteFileW);
        hook_targets[4] = kernelbase_proc(kernelbase, "DeleteFileA", DeleteFileA);
        hook_targets[5] = kernelbase_proc(kernelbase, "CreateDirectoryW", CreateDirectoryW);
        hook_targets[6] = kernelbase_proc(kernelbase, "CreateDirectoryA", CreateDirectoryA);
        hook_targets[7] = kernelbase_proc(kernelbase, "CreateDirectoryExW", CreateDirectoryExW);
        hook_targets[8] = kernelbase_proc(kernelbase, "CopyFileW", CopyFileW);
        hook_targets[9] = kernelbase_proc(kernelbase, "CopyFileExW", CopyFileExW);
        hook_targets[10] = kernelbase_proc(kernelbase, "CopyFile2", CopyFile2);
        hook_targets[11] = kernelbase_proc(kernelbase, "CopyFileA", CopyFileA);
        hook_targets[12] = kernelbase_proc(kernelbase, "CopyFileExA", CopyFileExA);
        hook_targets[13] = kernelbase_proc(kernelbase, "MoveFileExW", MoveFileExW);
        hook_targets[14] = kernelbase_proc(kernelbase, "ReplaceFileW", ReplaceFileW);
    }
    specs[0] = (ml_hook_spec_t){ hook_targets[0], create_file_w_hooked, (void **)&old_create_file_w };
    specs[1] = (ml_hook_spec_t){ hook_targets[1], create_file_a_hooked, (void **)&old_create_file_a };
    specs[2] = (ml_hook_spec_t){ hook_targets[2], create_file_2_hooked, (void **)&old_create_file_2 };
    specs[3] = (ml_hook_spec_t){ hook_targets[3], delete_file_w_hooked, (void **)&old_delete_file_w };
    specs[4] = (ml_hook_spec_t){ hook_targets[4], delete_file_a_hooked, (void **)&old_delete_file_a };
    specs[5] = (ml_hook_spec_t){ hook_targets[5], create_directory_w_hooked, (void **)&old_create_directory_w };
    specs[6] = (ml_hook_spec_t){ hook_targets[6], create_directory_a_hooked, (void **)&old_create_directory_a };
    specs[7] = (ml_hook_spec_t){ hook_targets[7], create_directory_ex_w_hooked, (void **)&old_create_directory_ex_w };
    specs[8] = (ml_hook_spec_t){ hook_targets[8], copy_file_w_hooked, (void **)&old_copy_file_w };
    specs[9] = (ml_hook_spec_t){ hook_targets[9], copy_file_ex_w_hooked, (void **)&old_copy_file_ex_w };
    specs[10] = (ml_hook_spec_t){ hook_targets[10], copy_file_2_hooked, (void **)&old_copy_file_2 };
    specs[11] = (ml_hook_spec_t){ hook_targets[11], copy_file_a_hooked, (void **)&old_copy_file_a };
    specs[12] = (ml_hook_spec_t){ hook_targets[12], copy_file_ex_a_hooked, (void **)&old_copy_file_ex_a };
    specs[13] = (ml_hook_spec_t){ hook_targets[13], move_file_ex_w_hooked, (void **)&old_move_file_ex_w };
    specs[14] = (ml_hook_spec_t){ hook_targets[14], replace_file_w_hooked, (void **)&old_replace_file_w };
}

typedef enum path_route_result_e {
    PATH_ROUTE_UNCHANGED,
    PATH_ROUTE_MAPPED,
    PATH_ROUTE_DENIED,
} path_route_result_t;

static path_route_result_t route_wide(const wchar_t *path, DWORD access, DWORD disposition,
                                      const wchar_t **mapped) {
    *mapped = NULL;
    if (vfs_recursion_guard_active()) return PATH_ROUTE_UNCHANGED;
    if (ml_save_mapping_route(path, mapped)) {
        return *mapped != NULL ? PATH_ROUTE_MAPPED : PATH_ROUTE_DENIED;
    }
    *mapped = vfs_route_writable_path(path);
    if (*mapped == NULL) *mapped = mods_file_route_read(path, access, disposition);
    return *mapped != NULL ? PATH_ROUTE_MAPPED : PATH_ROUTE_UNCHANGED;
}

static bool route_copy_paths(LPCWSTR existing_path, LPCWSTR new_path,
                             const wchar_t **mapped_existing, const wchar_t **mapped_new) {
    bool existing_handled;
    bool new_handled;
    *mapped_existing = NULL;
    *mapped_new = NULL;
    existing_handled = ml_save_mapping_route(existing_path, mapped_existing);
    new_handled = ml_save_mapping_route(new_path, mapped_new);
    return (!existing_handled || *mapped_existing != NULL) &&
           (!new_handled || *mapped_new != NULL);
}

static bool route_save_path(LPCWSTR path, const wchar_t **mapped) {
    *mapped = NULL;
    return path == NULL || !ml_save_mapping_route(path, mapped) || *mapped != NULL;
}

static HANDLE WINAPI create_file_w_hooked(LPCWSTR path, DWORD access, DWORD share,
                                           LPSECURITY_ATTRIBUTES security, DWORD disposition,
                                           DWORD flags, HANDLE template_file) {
    const wchar_t *mapped;
    path_route_result_t route = route_wide(path, access, disposition, &mapped);
    if (route == PATH_ROUTE_DENIED) {
        SetLastError(ERROR_CANNOT_MAKE);
        return INVALID_HANDLE_VALUE;
    }
    return old_create_file_w(mapped != NULL ? mapped : path, access, share, security, disposition, flags, template_file);
}

static HANDLE WINAPI create_file_a_hooked(LPCSTR path, DWORD access, DWORD share,
                                           LPSECURITY_ATTRIBUTES security, DWORD disposition,
                                           DWORD flags, HANDLE template_file) {
    wchar_t *wide = ml_path_from_ansi(path);
    const wchar_t *mapped = NULL;
    path_route_result_t route = wide != NULL
        ? route_wide(wide, access, disposition, &mapped) : PATH_ROUTE_UNCHANGED;
    /* Free the temporary before the real call so GetLastError() reflects the
     * API result, not the deallocation. `mapped` is owned by the VFS / save
     * mapping and never aliases `wide`. */
    ml_mem_free(wide);
    if (route == PATH_ROUTE_DENIED) {
        SetLastError(ERROR_CANNOT_MAKE);
        return INVALID_HANDLE_VALUE;
    }
    if (mapped != NULL) {
        return old_create_file_w(mapped, access, share, security, disposition, flags, template_file);
    }
    return old_create_file_a(path, access, share, security, disposition, flags, template_file);
}

static HANDLE WINAPI create_file_2_hooked(LPCWSTR path, DWORD access, DWORD share, DWORD disposition,
                                           LPCREATEFILE2_EXTENDED_PARAMETERS parameters) {
    const wchar_t *mapped;
    path_route_result_t route = route_wide(path, access, disposition, &mapped);
    if (route == PATH_ROUTE_DENIED) {
        SetLastError(ERROR_CANNOT_MAKE);
        return INVALID_HANDLE_VALUE;
    }
    return old_create_file_2(mapped != NULL ? mapped : path, access, share, disposition, parameters);
}

static BOOL WINAPI delete_file_w_hooked(LPCWSTR path) {
    const wchar_t *mapped = NULL;
    bool save_handled;
    if (vfs_recursion_guard_active()) return old_delete_file_w(path);
    save_handled = ml_save_mapping_route(path, &mapped);
    if (save_handled && mapped == NULL) {
        SetLastError(ERROR_CANNOT_MAKE);
        return FALSE;
    }
    if (mapped == NULL) mapped = vfs_route_writable_path(path);
    return old_delete_file_w(mapped != NULL ? mapped : path);
}

static BOOL WINAPI delete_file_a_hooked(LPCSTR path) {
    wchar_t *wide;
    const wchar_t *mapped = NULL;
    bool save_handled;
    if (vfs_recursion_guard_active()) return old_delete_file_a(path);
    wide = ml_path_from_ansi(path);
    if (wide == NULL) return old_delete_file_a(path);
    save_handled = ml_save_mapping_route(wide, &mapped);
    if (save_handled && mapped == NULL) {
        ml_mem_free(wide);
        SetLastError(ERROR_CANNOT_MAKE);
        return FALSE;
    }
    if (mapped == NULL) mapped = vfs_route_writable_path(wide);
    /* Free before the real call so GetLastError() reflects the delete, not the
     * deallocation; `mapped` does not alias `wide`. */
    ml_mem_free(wide);
    return mapped != NULL ? old_delete_file_w(mapped) : old_delete_file_a(path);
}

static BOOL WINAPI create_directory_w_hooked(LPCWSTR path, LPSECURITY_ATTRIBUTES security) {
    const wchar_t *mapped = vfs_route_writable_path(path);
    return old_create_directory_w(mapped != NULL ? mapped : path, security);
}

static BOOL WINAPI create_directory_a_hooked(LPCSTR path, LPSECURITY_ATTRIBUTES security) {
    wchar_t *wide = ml_path_from_ansi(path);
    const wchar_t *mapped = wide != NULL ? vfs_route_writable_path(wide) : NULL;
    /* Free before the real call so GetLastError() reflects it; `mapped` does
     * not alias `wide`. */
    ml_mem_free(wide);
    return mapped != NULL ? old_create_directory_w(mapped, security) : old_create_directory_a(path, security);
}

static BOOL WINAPI create_directory_ex_w_hooked(LPCWSTR template_path, LPCWSTR path,
                                                  LPSECURITY_ATTRIBUTES security) {
    const wchar_t *mapped = vfs_route_writable_path(path);
    return old_create_directory_ex_w(template_path, mapped != NULL ? mapped : path, security);
}

static BOOL WINAPI copy_file_w_hooked(LPCWSTR existing_path, LPCWSTR new_path, BOOL fail_if_exists) {
    const wchar_t *mapped_existing = NULL;
    const wchar_t *mapped_new = NULL;
    if (vfs_recursion_guard_active()) return old_copy_file_w(existing_path, new_path, fail_if_exists);
    if (!route_copy_paths(existing_path, new_path, &mapped_existing, &mapped_new)) {
        SetLastError(ERROR_CANNOT_MAKE);
        return FALSE;
    }
    return old_copy_file_w(mapped_existing != NULL ? mapped_existing : existing_path,
                           mapped_new != NULL ? mapped_new : new_path, fail_if_exists);
}

static BOOL WINAPI copy_file_ex_w_hooked(LPCWSTR existing_path, LPCWSTR new_path,
                                         LPPROGRESS_ROUTINE progress, LPVOID data,
                                         LPBOOL cancel, DWORD flags) {
    const wchar_t *mapped_existing = NULL;
    const wchar_t *mapped_new = NULL;
    if (vfs_recursion_guard_active()) {
        return old_copy_file_ex_w(existing_path, new_path, progress, data, cancel, flags);
    }
    if (!route_copy_paths(existing_path, new_path, &mapped_existing, &mapped_new)) {
        SetLastError(ERROR_CANNOT_MAKE);
        return FALSE;
    }
    return old_copy_file_ex_w(mapped_existing != NULL ? mapped_existing : existing_path,
                              mapped_new != NULL ? mapped_new : new_path,
                              progress, data, cancel, flags);
}

static HRESULT WINAPI copy_file_2_hooked(PCWSTR existing_path, PCWSTR new_path,
                                         COPYFILE2_EXTENDED_PARAMETERS *parameters) {
    const wchar_t *mapped_existing = NULL;
    const wchar_t *mapped_new = NULL;
    if (vfs_recursion_guard_active()) return old_copy_file_2(existing_path, new_path, parameters);
    if (!route_copy_paths(existing_path, new_path, &mapped_existing, &mapped_new)) {
        return HRESULT_FROM_WIN32(ERROR_CANNOT_MAKE);
    }
    return old_copy_file_2(mapped_existing != NULL ? mapped_existing : existing_path,
                           mapped_new != NULL ? mapped_new : new_path, parameters);
}

static BOOL WINAPI copy_file_a_hooked(LPCSTR existing_path, LPCSTR new_path, BOOL fail_if_exists) {
    wchar_t *wide_existing;
    wchar_t *wide_new;
    const wchar_t *mapped_existing = NULL;
    const wchar_t *mapped_new = NULL;
    BOOL result;
    if (vfs_recursion_guard_active()) return old_copy_file_a(existing_path, new_path, fail_if_exists);
    wide_existing = ml_path_from_ansi(existing_path);
    wide_new = ml_path_from_ansi(new_path);
    if (wide_existing == NULL || wide_new == NULL) {
        ml_mem_free(wide_existing);
        ml_mem_free(wide_new);
        return old_copy_file_a(existing_path, new_path, fail_if_exists);
    }
    if (!route_copy_paths(wide_existing, wide_new, &mapped_existing, &mapped_new)) {
        ml_mem_free(wide_existing);
        ml_mem_free(wide_new);
        SetLastError(ERROR_CANNOT_MAKE);
        return FALSE;
    }
    result = mapped_existing != NULL || mapped_new != NULL
        ? old_copy_file_w(mapped_existing != NULL ? mapped_existing : wide_existing,
                          mapped_new != NULL ? mapped_new : wide_new, fail_if_exists)
        : old_copy_file_a(existing_path, new_path, fail_if_exists);
    ml_mem_free(wide_existing);
    ml_mem_free(wide_new);
    return result;
}

static BOOL WINAPI copy_file_ex_a_hooked(LPCSTR existing_path, LPCSTR new_path,
                                         LPPROGRESS_ROUTINE progress, LPVOID data,
                                         LPBOOL cancel, DWORD flags) {
    wchar_t *wide_existing;
    wchar_t *wide_new;
    const wchar_t *mapped_existing = NULL;
    const wchar_t *mapped_new = NULL;
    BOOL result;
    if (vfs_recursion_guard_active()) {
        return old_copy_file_ex_a(existing_path, new_path, progress, data, cancel, flags);
    }
    wide_existing = ml_path_from_ansi(existing_path);
    wide_new = ml_path_from_ansi(new_path);
    if (wide_existing == NULL || wide_new == NULL) {
        ml_mem_free(wide_existing);
        ml_mem_free(wide_new);
        return old_copy_file_ex_a(existing_path, new_path, progress, data, cancel, flags);
    }
    if (!route_copy_paths(wide_existing, wide_new, &mapped_existing, &mapped_new)) {
        ml_mem_free(wide_existing);
        ml_mem_free(wide_new);
        SetLastError(ERROR_CANNOT_MAKE);
        return FALSE;
    }
    result = mapped_existing != NULL || mapped_new != NULL
        ? old_copy_file_ex_w(mapped_existing != NULL ? mapped_existing : wide_existing,
                             mapped_new != NULL ? mapped_new : wide_new, progress, data, cancel, flags)
        : old_copy_file_ex_a(existing_path, new_path, progress, data, cancel, flags);
    ml_mem_free(wide_existing);
    ml_mem_free(wide_new);
    return result;
}

static BOOL WINAPI move_file_ex_w_hooked(LPCWSTR existing_path, LPCWSTR new_path, DWORD flags) {
    const wchar_t *mapped_existing = NULL;
    const wchar_t *mapped_new = NULL;
    if (vfs_recursion_guard_active()) return old_move_file_ex_w(existing_path, new_path, flags);
    if (!route_save_path(existing_path, &mapped_existing) ||
        !route_save_path(new_path, &mapped_new)) {
        SetLastError(ERROR_CANNOT_MAKE);
        return FALSE;
    }
    return old_move_file_ex_w(mapped_existing != NULL ? mapped_existing : existing_path,
                              mapped_new != NULL ? mapped_new : new_path, flags);
}

static BOOL WINAPI replace_file_w_hooked(LPCWSTR replaced_path, LPCWSTR replacement_path,
                                         LPCWSTR backup_path, DWORD flags,
                                         LPVOID exclude, LPVOID reserved) {
    const wchar_t *mapped_replaced = NULL;
    const wchar_t *mapped_replacement = NULL;
    const wchar_t *mapped_backup = NULL;
    if (vfs_recursion_guard_active()) {
        return old_replace_file_w(replaced_path, replacement_path, backup_path, flags, exclude, reserved);
    }
    if (!route_save_path(replaced_path, &mapped_replaced) ||
        !route_save_path(replacement_path, &mapped_replacement) ||
        !route_save_path(backup_path, &mapped_backup)) {
        SetLastError(ERROR_CANNOT_MAKE);
        return FALSE;
    }
    return old_replace_file_w(mapped_replaced != NULL ? mapped_replaced : replaced_path,
                              mapped_replacement != NULL ? mapped_replacement : replacement_path,
                              mapped_backup != NULL ? mapped_backup : backup_path,
                              flags, exclude, reserved);
}

bool ml_win32_file_hooks_install(void) {
    ml_hook_spec_t specs[ML_WIN32_HOOK_COUNT];
    bool rollback_complete;
    if (hooks_installed) return true;
    build_hook_specs(specs, true);
    bool result = ml_hook_batch_install(specs, ML_WIN32_HOOK_COUNT, install_hook, remove_hook, &rollback_complete);
    hooks_installed = result || !rollback_complete;
    ml_log_write(result ? ML_LOG_LEVEL_INFO : ML_LOG_LEVEL_WARN,
                 L"win32-vfs", result ? L"file hooks APPLIED" : L"file hooks HOOK_FAILED");
    if (!rollback_complete) {
        ML_LOG_WARN(L"win32-vfs", L"hook rollback incomplete; uninstall will retry cleanup");
    }
    return result;
}

void ml_win32_file_hooks_uninstall(void) {
    ml_hook_spec_t specs[ML_WIN32_HOOK_COUNT];
    if (!hooks_installed) return;
    build_hook_specs(specs, false);
    if (ml_hook_batch_remove(specs, ML_WIN32_HOOK_COUNT, remove_hook)) {
        hooks_installed = false;
    } else {
        ML_LOG_WARN(L"win32-vfs", L"one or more file hooks could not be removed");
    }
}

#ifdef ML_WIN32_HOOKS_TEST
void ml_win32_file_hooks_test_init(void) {
    old_create_file_w = CreateFileW;
    old_create_file_a = CreateFileA;
    old_create_file_2 = CreateFile2;
    old_delete_file_w = DeleteFileW;
    old_delete_file_a = DeleteFileA;
    old_create_directory_w = CreateDirectoryW;
    old_create_directory_a = CreateDirectoryA;
    old_create_directory_ex_w = CreateDirectoryExW;
    old_copy_file_w = CopyFileW;
    old_copy_file_ex_w = CopyFileExW;
    old_copy_file_2 = CopyFile2;
    old_copy_file_a = CopyFileA;
    old_copy_file_ex_a = CopyFileExA;
    old_move_file_ex_w = MoveFileExW;
    old_replace_file_w = ReplaceFileW;
}

void *ml_win32_file_hooks_test_target(const char *name) {
    HMODULE kernelbase = GetModuleHandleW(L"kernelbase.dll");
    return kernelbase == NULL ? NULL : (void *)GetProcAddress(kernelbase, name);
}

HANDLE ml_win32_file_hooks_test_create_file_w(LPCWSTR path, DWORD access, DWORD share,
                                               DWORD disposition, DWORD flags) {
    return create_file_w_hooked(path, access, share, NULL, disposition, flags, NULL);
}

HANDLE ml_win32_file_hooks_test_create_file_2(LPCWSTR path, DWORD access, DWORD share,
                                               DWORD disposition) {
    return create_file_2_hooked(path, access, share, disposition, NULL);
}

BOOL ml_win32_file_hooks_test_copy_file_w(LPCWSTR source, LPCWSTR target) {
    return copy_file_w_hooked(source, target, FALSE);
}

BOOL ml_win32_file_hooks_test_move_file_ex_w(LPCWSTR source, LPCWSTR target) {
    return move_file_ex_w_hooked(source, target, 0);
}

BOOL ml_win32_file_hooks_test_create_directory_w(LPCWSTR path) {
    return create_directory_w_hooked(path, NULL);
}

BOOL ml_win32_file_hooks_test_delete_file_w(LPCWSTR path) {
    return delete_file_w_hooked(path);
}
#endif
