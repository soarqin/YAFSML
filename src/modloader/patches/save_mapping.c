#include "save_mapping.h"

#include "common/allocator.h"
#include "common/atomic.h"
#include "log.h"

#include "modloader/vfs.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlwapi.h>

#include <stdint.h>
#include <wchar.h>

typedef struct save_mapping_entry_s {
    wchar_t extension[16];
    wchar_t override_name[64];
} save_mapping_entry_t;

#define SAVE_MAPPING_MAX 8

static SRWLOCK mapping_lock = SRWLOCK_INIT;
static wchar_t *save_root;
static save_mapping_entry_t mappings[SAVE_MAPPING_MAX];
static size_t mapping_count;
/* Requested extensions whose mapping could not be set up; their saves fail
 * closed instead of opening the original files. */
static wchar_t failed_extensions[SAVE_MAPPING_MAX][16];
static size_t failed_extension_count;
/* Nonzero while any mapping or failure exists, read without the lock on every
 * CreateFile. Written under the exclusive lock. */
static volatile LONG mapping_active;
static volatile LONG link_warning_logged;

static void update_mapping_active_locked(void) {
    InterlockedExchange(&mapping_active, mapping_count != 0 || failed_extension_count != 0);
}

static void remember_failed_extension(const wchar_t *extension) {
    if (extension == NULL || extension[0] != L'.' || wcslen(extension) >= 16) return;
    for (size_t i = 0; i < failed_extension_count; i++) {
        if (lstrcmpiW(failed_extensions[i], extension) == 0) return;
    }
    if (failed_extension_count < SAVE_MAPPING_MAX) {
        lstrcpynW(failed_extensions[failed_extension_count], extension, 16);
        failed_extension_count++;
    }
    update_mapping_active_locked();
}

static wchar_t *join_path(const wchar_t *left, const wchar_t *right) {
    size_t left_length;
    size_t right_length;
    bool separator;
    wchar_t *result;
    if (left == NULL || right == NULL) return NULL;
    left_length = wcslen(left);
    right_length = wcslen(right);
    separator = left_length != 0 && left[left_length - 1] != L'\\' && left[left_length - 1] != L'/';
    if (left_length > SIZE_MAX - right_length - (separator ? 2 : 1)) return NULL;
    result = ml_mem_alloc(0, (left_length + right_length + (separator ? 2 : 1)) * sizeof(*result));
    if (result == NULL) return NULL;
    memcpy(result, left, left_length * sizeof(*result));
    if (separator) result[left_length++] = L'\\';
    memcpy(result + left_length, right, (right_length + 1) * sizeof(*result));
    return result;
}

static wchar_t *canonicalize_path(const wchar_t *path) {
    DWORD capacity;
    if (path == NULL) return NULL;
    capacity = GetFullPathNameW(path, 0, NULL, NULL);
    while (capacity != 0) {
        wchar_t *result = ml_mem_alloc(0, (size_t)capacity * sizeof(*result));
        DWORD length;
        if (result == NULL) return NULL;
        length = GetFullPathNameW(path, capacity, result, NULL);
        if (length == 0) {
            ml_mem_free(result);
            return NULL;
        }
        if (length < capacity) {
            while (length > 1 && (result[length - 1] == L'\\' || result[length - 1] == L'/')) {
                result[--length] = L'\0';
            }
            return result;
        }
        ml_mem_free(result);
        capacity = length + 1;
    }
    return NULL;
}

static wchar_t *environment_value(const wchar_t *name) {
    DWORD capacity = GetEnvironmentVariableW(name, NULL, 0);
    while (capacity != 0) {
        wchar_t *result = ml_mem_alloc(0, (size_t)capacity * sizeof(*result));
        DWORD length;
        if (result == NULL) return NULL;
        length = GetEnvironmentVariableW(name, result, capacity);
        if (length == 0) {
            ml_mem_free(result);
            return NULL;
        }
        if (length < capacity) return result;
        ml_mem_free(result);
        capacity = length + 1;
    }
    return NULL;
}

static bool path_is_under_root(const wchar_t *canonical, const wchar_t *root) {
    size_t root_length = root == NULL ? 0 : wcslen(root);
    return root_length != 0 && wcslen(canonical) > root_length &&
           _wcsnicmp(canonical, root, root_length) == 0 &&
           (canonical[root_length] == L'\\' || canonical[root_length] == L'/');
}

static bool is_name_surrogate(const wchar_t *path) {
    WIN32_FIND_DATAW find;
    HANDLE handle = FindFirstFileW(path, &find);
    if (handle == INVALID_HANDLE_VALUE) return true;
    FindClose(handle);
    return (find.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 &&
           IsReparseTagNameSurrogate(find.dwReserved0);
}

/* True when a component below `root` is a symbolic link or junction, which could
 * lead outside the save root. Other reparse points, such as cloud placeholders,
 * stay where they are. `canonical` must be under `root` (path_is_under_root). */
static bool path_contains_link(const wchar_t *canonical, const wchar_t *root) {
    size_t root_length = wcslen(root);
    wchar_t *prefix;
    wchar_t *cursor;
    DWORD attributes;
    if (wcslen(canonical) <= root_length) return false;
    prefix = ml_mem_strdup_w(canonical);
    if (prefix == NULL) return true;
    cursor = prefix + root_length;
    for (;;) {
        wchar_t saved;
        bool link;
        while (*cursor == L'\\' || *cursor == L'/') cursor++;
        while (*cursor != L'\0' && *cursor != L'\\' && *cursor != L'/') cursor++;
        saved = *cursor;
        *cursor = L'\0';
        attributes = GetFileAttributesW(prefix);
        link = attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 &&
               is_name_surrogate(prefix);
        *cursor = saved;
        if (link) {
            ml_mem_free(prefix);
            return true;
        }
        if (saved == L'\0') break;
    }
    ml_mem_free(prefix);
    return false;
}

static wchar_t *build_override_name(const wchar_t *source_name, const wchar_t *override_name) {
    const wchar_t *extension;
    size_t stem_length;
    size_t suffix_length;
    wchar_t *result;
    if (override_name[0] != L'.') return ml_mem_strdup_w(override_name);
    /* Nested kernelbase calls (CopyFileW -> CopyFileExW -> CreateFileW) route the
     * already mapped name again, so the mapping must be idempotent. */
    suffix_length = wcslen(override_name);
    stem_length = wcslen(source_name);
    if (stem_length > suffix_length && _wcsicmp(source_name + stem_length - suffix_length, override_name) == 0)
        return ml_mem_strdup_w(source_name);
    extension = PathFindExtensionW(source_name);
    stem_length = (size_t)(extension - source_name);
    if (stem_length > SIZE_MAX - suffix_length - 1) return NULL;
    result = ml_mem_alloc(0, (stem_length + suffix_length + 1) * sizeof(*result));
    if (result == NULL) return NULL;
    memcpy(result, source_name, stem_length * sizeof(*result));
    memcpy(result + stem_length, override_name, (suffix_length + 1) * sizeof(*result));
    return result;
}

static bool valid_override_name(const wchar_t *name) {
    static const wchar_t *reserved[] = { L"con", L"prn", L"aux", L"nul" };
    const wchar_t *extension;
    size_t stem_length;
    size_t length = wcslen(name);
    if (length == 0 || length >= 64 || name[length - 1] == L' ' || name[length - 1] == L'.') return false;
    for (const wchar_t *p = name; *p != L'\0'; p++) {
        if (*p < 32 || wcschr(L"<>:\"/\\|?*", *p) != NULL) return false;
    }
    if (name[0] == L'.') return length > 1;
    extension = wcschr(name, L'.');
    stem_length = extension == NULL ? length : (size_t)(extension - name);
    for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++) {
        if (wcslen(reserved[i]) == stem_length && _wcsnicmp(name, reserved[i], stem_length) == 0) return false;
    }
    if (stem_length == 4 && (_wcsnicmp(name, L"com", 3) == 0 || _wcsnicmp(name, L"lpt", 3) == 0) &&
        name[3] >= L'1' && name[3] <= L'9') return false;
    return true;
}

bool ml_save_mapping_init_root(const ml_game_descriptor_t *game) {
    wchar_t *canonical = NULL;
    if (game != NULL && game->save_root_name != NULL) {
        wchar_t *appdata = environment_value(L"APPDATA");
        wchar_t *joined = appdata == NULL ? NULL : join_path(appdata, game->save_root_name);
        canonical = joined == NULL ? NULL : canonicalize_path(joined);
        ml_mem_free(joined);
        ml_mem_free(appdata);
    }
    AcquireSRWLockExclusive(&mapping_lock);
    ml_mem_free(save_root);
    save_root = canonical;
    mapping_count = 0;
    failed_extension_count = 0;
    update_mapping_active_locked();
    ReleaseSRWLockExclusive(&mapping_lock);
    if (canonical == NULL) return false;
    ML_LOG_INFO(L"save-mapping", L"save root resolved: %ls", canonical);
    return true;
}

/* Without a resolved root the extension is remembered as failed, so its saves
 * are denied rather than opened under their original names. */
bool ml_save_mapping_add_extension(const wchar_t *extension, const wchar_t *override_name) {
    bool valid = extension != NULL && extension[0] == L'.' && wcslen(extension) < 16 &&
                 override_name != NULL && override_name[0] != L'\0' && valid_override_name(override_name);
    bool result = false;
    if (!valid) {
        ML_LOG_WARN(L"save-mapping", L"extension mapping rejected: extension=%ls override=%ls",
                    extension == NULL ? L"<null>" : extension,
                    override_name == NULL ? L"<null>" : override_name);
    }
    AcquireSRWLockExclusive(&mapping_lock);
    if (valid && save_root != NULL && mapping_count < SAVE_MAPPING_MAX) {
        lstrcpynW(mappings[mapping_count].extension, extension, 16);
        lstrcpynW(mappings[mapping_count].override_name, override_name, 64);
        mapping_count++;
        update_mapping_active_locked();
        result = true;
    } else {
        remember_failed_extension(extension);
    }
    ReleaseSRWLockExclusive(&mapping_lock);
    if (valid) {
        ML_LOG_INFO(L"save-mapping", L"extension mapping %ls -> %ls: %ls",
                    extension, override_name, result ? L"registered" : L"NOT registered (root unset or table full)");
    }
    return result;
}

bool ml_save_mapping_init(const ml_game_descriptor_t *game, const wchar_t *override_name) {
    /* Convenience for the common single-extension (.sl2) case. Validate the
     * override up front so a bad name is rejected without side effects, matching
     * the original contract. */
    if (game == NULL || override_name == NULL || override_name[0] == L'\0' ||
        !valid_override_name(override_name)) {
        return false;
    }
    if (!ml_save_mapping_init_root(game)) return false;
    return ml_save_mapping_add_extension(L".sl2", override_name);
}

static bool extension_in(const wchar_t *extension, const wchar_t list[][16], size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (lstrcmpiW(extension, list[i]) == 0) return true;
    }
    return false;
}

bool ml_save_mapping_route(const wchar_t *path, const wchar_t **mapped_path) {
    wchar_t *source;
    wchar_t *logical_source;
    wchar_t *target = NULL;
    wchar_t *target_name = NULL;
    wchar_t *root_snapshot;
    save_mapping_entry_t snapshot[SAVE_MAPPING_MAX];
    wchar_t failed_snapshot[SAVE_MAPPING_MAX][16];
    size_t snapshot_count;
    size_t failed_count;
    const wchar_t *override_name = NULL;
    const wchar_t *logical_ext;
    const wchar_t *source_name;
    const wchar_t *registered;
    const wchar_t *raw_ext;
    bool backup;
    bool interesting;
    size_t i;

    if (mapped_path != NULL) *mapped_path = NULL;
    if (path == NULL || mapped_path == NULL || ml_atomic_load_acquire(&mapping_active) == 0) return false;
    raw_ext = PathFindExtensionW(path);
    AcquireSRWLockShared(&mapping_lock);
    /* Extension pre-filter. This function is on the CreateFile hot path, so
     * reject uninteresting paths before the root strdup, the table snapshots and
     * canonicalize_path (two GetFullPathNameW calls, which take the process
     * current-directory lock).
     *
     * Every path through the rest of the function that returns something other
     * than `false` requires the logical extension -- the raw one, or the stem's
     * for the `.bak` backup rotation -- to be mapped or remembered as failed. */
    interesting = lstrcmpiW(raw_ext, L".bak") == 0 ||
                  extension_in(raw_ext, failed_extensions, failed_extension_count);
    for (i = 0; !interesting && i < mapping_count; i++) {
        interesting = lstrcmpiW(raw_ext, mappings[i].extension) == 0;
    }
    if (!interesting) {
        ReleaseSRWLockShared(&mapping_lock);
        return false;
    }
    root_snapshot = save_root == NULL ? NULL : ml_mem_strdup_w(save_root);
    snapshot_count = mapping_count;
    failed_count = failed_extension_count;
    memcpy(snapshot, mappings, snapshot_count * sizeof(snapshot[0]));
    memcpy(failed_snapshot, failed_extensions, failed_count * sizeof(failed_snapshot[0]));
    ReleaseSRWLockShared(&mapping_lock);
    source = canonicalize_path(path);
    if (source == NULL) {
        ml_mem_free(root_snapshot);
        return false;
    }
    if (root_snapshot != NULL) {
        if (!path_is_under_root(source, root_snapshot)) {
            ML_LOG_DEBUG(L"save-mapping", L"skip %ls: canonical=%ls is outside root=%ls", path, source, root_snapshot);
            ml_mem_free(root_snapshot);
            ml_mem_free(source);
            return false;
        }
        if (path_contains_link(source, root_snapshot)) {
            if (InterlockedExchange(&link_warning_logged, 1) == 0) {
                ML_LOG_WARN(L"save-mapping", L"save path %ls goes through a symbolic link or junction below %ls; "
                            L"save access is denied instead of using the original save", source, root_snapshot);
            }
            ml_mem_free(root_snapshot);
            ml_mem_free(source);
            return true;
        }
        registered = vfs_route_writable_path(path);
        if (registered != NULL) {
            *mapped_path = registered;
            ml_mem_free(root_snapshot);
            ml_mem_free(source);
            return true;
        }
    }

    backup = lstrcmpiW(PathFindExtensionW(source), L".bak") == 0;
    logical_source = ml_mem_strdup_w(source);
    if (logical_source == NULL) {
        ml_mem_free(root_snapshot);
        ml_mem_free(source);
        return true;
    }
    if (backup) PathRemoveExtensionW(logical_source);
    logical_ext = PathFindExtensionW(logical_source);
    if (extension_in(logical_ext, failed_snapshot, failed_count)) {
        ml_mem_free(logical_source);
        ml_mem_free(root_snapshot);
        ml_mem_free(source);
        return true;
    }
    for (i = 0; root_snapshot != NULL && i < snapshot_count; i++) {
        if (lstrcmpiW(logical_ext, snapshot[i].extension) == 0) {
            override_name = snapshot[i].override_name;
            break;
        }
    }
    ml_mem_free(root_snapshot);
    if (override_name == NULL) {
        ml_mem_free(logical_source);
        ml_mem_free(source);
        return false;
    }
    source_name = PathFindFileNameW(logical_source);
    target_name = build_override_name(source_name, override_name);
    if (target_name != NULL) {
        size_t directory_length = (size_t)(source_name - logical_source);
        wchar_t *directory = ml_mem_alloc(0, (directory_length + 1) * sizeof(*directory));
        if (directory != NULL) {
            memcpy(directory, logical_source, directory_length * sizeof(*directory));
            directory[directory_length] = L'\0';
            target = join_path(directory, target_name);
            ml_mem_free(directory);
        }
    }
    ml_mem_free(target_name);
    if (target == NULL) {
        AcquireSRWLockExclusive(&mapping_lock);
        remember_failed_extension(logical_ext);
        ReleaseSRWLockExclusive(&mapping_lock);
        ml_mem_free(logical_source);
        ml_mem_free(source);
        return true;
    }
    if (backup) {
        size_t length = wcslen(target);
        wchar_t *with_backup = ml_mem_alloc(0, (length + 5) * sizeof(*with_backup));
        if (with_backup == NULL) {
            ml_mem_free(target);
            ml_mem_free(logical_source);
            ml_mem_free(source);
            return true;
        }
        memcpy(with_backup, target, length * sizeof(*with_backup));
        memcpy(with_backup + length, L".bak", 5 * sizeof(*with_backup));
        ml_mem_free(target);
        target = with_backup;
    }

    AcquireSRWLockExclusive(&mapping_lock);
    registered = vfs_route_writable_path(path);
    if (registered == NULL) {
        BOOL target_exists = PathFileExistsW(target);
        BOOL source_exists = PathFileExistsW(source);
        if (!target_exists && source_exists) {
            BOOL copied;
            DWORD error;
            vfs_recursion_guard_enter();
            copied = CopyFileW(source, target, TRUE);
            error = copied ? ERROR_SUCCESS : GetLastError();
            vfs_recursion_guard_leave();
            ML_LOG_INFO(L"save-mapping", L"seeded renamed save from original: %ls -> %ls copied=%d err=%lu",
                        source, target, copied ? 1 : 0, error);
            if (!copied && error != ERROR_FILE_EXISTS) {
                remember_failed_extension(logical_ext);
                ReleaseSRWLockExclusive(&mapping_lock);
                ml_mem_free(target);
                ml_mem_free(logical_source);
                ml_mem_free(source);
                return true;
            }
        }
        if (vfs_register_writable_path(path, target)) registered = vfs_route_writable_path(path);
        if (registered == NULL) remember_failed_extension(logical_ext);
    }
    *mapped_path = registered;
    ReleaseSRWLockExclusive(&mapping_lock);
    if (registered == NULL) {
        ML_LOG_WARN(L"save-mapping", L"route %ls: target=%ls computed but writable registration returned NULL (primary save access denied)",
                    path, target);
    }
    ml_mem_free(target);
    ml_mem_free(logical_source);
    ml_mem_free(source);
    return true;
}

void ml_save_mapping_uninit(void) {
    AcquireSRWLockExclusive(&mapping_lock);
    ml_mem_free(save_root);
    save_root = NULL;
    mapping_count = 0;
    failed_extension_count = 0;
    update_mapping_active_locked();
    ReleaseSRWLockExclusive(&mapping_lock);
    InterlockedExchange(&link_warning_logged, 0);
}
