/*
 * Copyright (C) 2024, Soar Qin<soarchin@gmail.com>

 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#include "app.h"

#include "vdf.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <string.h>

/* The Steam client ignores app manifests from other universes. */
#define STEAM_UNIVERSE_PUBLIC 1

/* Steam's VDF files are UTF-8. Convert explicitly instead of relying on
 * printf's "%hs" (which uses the ANSI code page and corrupts non-ASCII
 * library paths / install directories, e.g. non-Latin folder names). */
static bool utf8_to_wide(const char *utf8, wchar_t *out, int out_count) {
    if (utf8 == NULL || out == NULL || out_count <= 0) return false;
    if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, out_count) == 0) {
        out[0] = L'\0';
        return false;
    }
    out[out_count - 1] = L'\0';
    return true;
}

/* KeyValues::GetString with an empty default. */
static const char *get_string(const struct vdf_object *object, const char *key) {
    const char *value = vdf_object_get_string(vdf_object_index_array_str(object, key));
    return value != NULL ? value : "";
}

/* KeyValues::GetInt with a default of 0. Integer values keep the decimal form
 * GetString returns, which V_atoi in Steam's vstdlib reads back unchanged:
 * an optional sign, then hex digits after "0x", a quoted character, or
 * decimal digits, all with 32-bit wraparound. */
static uint32_t get_int(const struct vdf_object *object, const char *key) {
    const char *s = get_string(object, key);
    uint32_t sign = 1;
    uint32_t value = 0;
    if (*s == '-') {
        sign = UINT32_MAX;
        s++;
    } else if (*s == '+') {
        s++;
    }
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        for (s += 2;; s++) {
            if (*s >= '0' && *s <= '9') value = value * 16 + (uint32_t)(*s - '0');
            else if (*s >= 'a' && *s <= 'f') value = value * 16 + (uint32_t)(*s - 'a' + 10);
            else if (*s >= 'A' && *s <= 'F') value = value * 16 + (uint32_t)(*s - 'A' + 10);
            else break;
        }
    } else if (s[0] == '\'') {
        value = (uint32_t)(int)(signed char)s[1];
    } else {
        for (; *s >= '0' && *s <= '9'; s++) value = value * 10 + (uint32_t)(*s - '0');
    }
    return value * sign;
}

/* V_IsAbsolutePath in Steam's vstdlib. */
static bool is_absolute_path(const char *path) {
    return strchr(path, ':') != NULL || path[0] == '/' || path[0] == '\\';
}

static bool find_game_in_library(uint32_t app_id, const wchar_t *library_root, wchar_t *path) {
    wchar_t manifest_path[MAX_PATH];
    wchar_t install_dir[MAX_PATH];
    wchar_t game_path[MAX_PATH];
    int length = _snwprintf(manifest_path, MAX_PATH, L"%ls\\steamapps\\appmanifest_%u.acf",
                            library_root, app_id);
    if (length < 0 || length >= MAX_PATH) return false;
    struct vdf_object *acf = vdf_parse_file(manifest_path);
    if (acf == NULL) return false;
    /* Like CSteamClientApp::BReadAppState: the first entry is used whatever
     * its name, and it must belong to this app in the public universe. */
    bool valid = get_int(acf, "AppID") == app_id &&
                 get_int(acf, "Universe") == STEAM_UNIVERSE_PUBLIC &&
                 utf8_to_wide(get_string(acf, "InstallDir"), install_dir, MAX_PATH) &&
                 install_dir[0] != L'\0';
    vdf_free_object(acf);
    if (!valid) return false;

    length = _snwprintf(game_path, MAX_PATH, L"%ls\\steamapps\\common\\%ls",
                        library_root, install_dir);
    if (length < 0 || length >= MAX_PATH) return false;
    DWORD attributes = GetFileAttributesW(game_path);
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY))
        return false;
    lstrcpyW(path, game_path);
    return true;
}

/* Like the Steam client, read steamapps\libraryfolders.vdf and fall back to
 * config\libraryfolders.vdf when the first is missing, empty or malformed. */
static struct vdf_object *load_library_folders(const wchar_t *steam_path) {
    static const wchar_t *const folders[] = { L"steamapps", L"config" };
    for (size_t i = 0; i < sizeof(folders) / sizeof(folders[0]); i++) {
        wchar_t library_path[MAX_PATH];
        struct vdf_object *library_folders;
        int length = _snwprintf(library_path, MAX_PATH, L"%ls\\%ls\\libraryfolders.vdf",
                                steam_path, folders[i]);
        if (length < 0 || length >= MAX_PATH) continue;
        library_folders = vdf_parse_file(library_path);
        if (library_folders != NULL) return library_folders;
    }
    return NULL;
}

bool app_find_game_path(const uint32_t app_id, wchar_t *path) {
    wchar_t steam_path[MAX_PATH];
    struct vdf_object *library_folders;
    const struct vdf_object *steam_library;
    if (path == NULL) return false;
    /* A failed search always leaves an empty output path. */
    path[0] = L'\0';
    DWORD cbdata = sizeof(steam_path);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath",
                      RRF_RT_REG_SZ, NULL, steam_path, &cbdata) != ERROR_SUCCESS ||
        steam_path[0] == L'\0') {
        return false;
    }
    /* Steam's libraries are its own folder, which stands in for the "0" entry
     * whatever path that names, and every other block with an absolute path.
     * They are searched from the last block back to Steam's own folder. */
    library_folders = load_library_folders(steam_path);
    steam_library = vdf_object_index_array_str(library_folders, "0");
    for (size_t i = vdf_object_get_array_length(library_folders); i > 0; i--) {
        wchar_t library_root[MAX_PATH];
        const struct vdf_object *library = vdf_object_index_array(library_folders, i - 1);
        const char *library_path = get_string(library, "path");
        if (library == steam_library || !is_absolute_path(library_path) ||
            !utf8_to_wide(library_path, library_root, MAX_PATH)) continue;
        /* The cached apps list can omit installed games. Read each library's
         * manifest directly instead of using that list as a filter. */
        if (find_game_in_library(app_id, library_root, path)) {
            vdf_free_object(library_folders);
            return true;
        }
    }
    vdf_free_object(library_folders);
    return find_game_in_library(app_id, steam_path, path);
}
