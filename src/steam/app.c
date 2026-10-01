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

static bool find_game_in_library(uint32_t app_id, const wchar_t *library_root, wchar_t *path) {
    wchar_t manifest_path[MAX_PATH];
    wchar_t install_dir[MAX_PATH];
    wchar_t game_path[MAX_PATH];
    int length = _snwprintf(manifest_path, MAX_PATH, L"%ls\\steamapps\\appmanifest_%u.acf",
                            library_root, app_id);
    if (length < 0 || length >= MAX_PATH) return false;
    struct vdf_object *acf = vdf_parse_file(manifest_path);
    if (acf == NULL) return false;
    if (acf->key == NULL || strcmp(acf->key, "AppState") != 0 || acf->type != VDF_TYPE_ARRAY) {
        vdf_free_object(acf);
        return false;
    }
    const struct vdf_object *entry = vdf_object_index_array_str(acf, "installdir");
    bool valid = entry != NULL && entry->type == VDF_TYPE_STRING &&
                 utf8_to_wide(vdf_object_get_string(entry), install_dir, MAX_PATH) &&
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

bool app_find_game_path(const uint32_t app_id, wchar_t *path) {
    wchar_t steam_path[MAX_PATH];
    wchar_t library_path[MAX_PATH];
    struct vdf_object *library_folders = NULL;
    if (path == NULL) return false;
    /* A failed search always leaves an empty output path. */
    path[0] = L'\0';
    DWORD cbdata = sizeof(steam_path);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath",
                      RRF_RT_REG_SZ, NULL, steam_path, &cbdata) != ERROR_SUCCESS ||
        steam_path[0] == L'\0') {
        return false;
    }
    int length = _snwprintf(library_path, MAX_PATH, L"%ls\\steamapps\\libraryfolders.vdf", steam_path);
    if (length >= 0 && length < MAX_PATH) library_folders = vdf_parse_file(library_path);
    if (library_folders != NULL && library_folders->key != NULL &&
        _stricmp(library_folders->key, "libraryfolders") == 0 &&
        library_folders->type == VDF_TYPE_ARRAY) {
        for (size_t i = vdf_object_get_array_length(library_folders); i > 0; i--) {
            wchar_t library_root[MAX_PATH];
            const struct vdf_object *library = vdf_object_index_array(library_folders, i - 1);
            if (library == NULL || library->key == NULL || library->key[0] == '\0' ||
                strspn(library->key, "0123456789") != strlen(library->key)) continue;
            /* Older Steam versions use a string path instead of a library object. */
            const struct vdf_object *entry = library->type == VDF_TYPE_ARRAY
                ? vdf_object_index_array_str(library, "path") : library;
            if (entry == NULL || entry->type != VDF_TYPE_STRING ||
                !utf8_to_wide(vdf_object_get_string(entry), library_root, MAX_PATH) ||
                library_root[0] == L'\0') continue;
            /* The cached apps list can omit installed games. Read each library's
             * manifest directly instead of using that list as a filter. */
            if (find_game_in_library(app_id, library_root, path)) {
                vdf_free_object(library_folders);
                return true;
            }
        }
    }
    vdf_free_object(library_folders);
    /* Steam's own library remains usable when the library index is unavailable. */
    return find_game_in_library(app_id, steam_path, path);
}
