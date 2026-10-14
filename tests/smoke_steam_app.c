#include <stdio.h>
#include <stdlib.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "test_common.h"
#include "steam/app.h"

static wchar_t fixture_root[MAX_PATH];
static wchar_t steam_root[MAX_PATH];
static wchar_t extra_root[MAX_PATH];
static wchar_t registry_path[128];
static HKEY registry_root;

static bool fixture_path(wchar_t *path, const wchar_t *relative) {
    int length = _snwprintf(path, MAX_PATH, L"%ls\\%ls", fixture_root, relative);
    return length >= 0 && length < MAX_PATH;
}

static bool write_fixture(const wchar_t *relative, const char *content) {
    wchar_t path[MAX_PATH];
    FILE *file;
    bool success;
    if (!fixture_path(path, relative)) return false;
    file = _wfopen(path, L"wb");
    if (file == NULL) return false;
    success = fwrite(content, 1, strlen(content), file) == strlen(content);
    return fclose(file) == 0 && success;
}

static bool escape_vdf_path(const wchar_t *wide, char *escaped, size_t capacity) {
    char utf8[MAX_PATH * 3];
    size_t length = 0;
    if (WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, sizeof(utf8), NULL, NULL) == 0)
        return false;
    for (const char *p = utf8; *p != '\0'; p++) {
        if (length + 3 > capacity) return false;
        if (*p == '\\' || *p == '"') escaped[length++] = '\\';
        escaped[length++] = *p;
    }
    escaped[length] = '\0';
    return true;
}

static int setup_fixture(void) {
    wchar_t temp[MAX_PATH];
    wchar_t path[MAX_PATH];
    HKEY steam_key;
    const wchar_t *directories[] = {
        L"Steam", L"Steam\\config", L"Steam\\steamapps", L"Steam\\steamapps\\common",
        L"Steam\\steamapps\\common\\ELDEN RING", L"Steam\\steamapps\\common\\1245620",
        L"额外游戏库", L"额外游戏库\\steamapps", L"额外游戏库\\steamapps\\common",
        L"额外游戏库\\steamapps\\common\\艾尔登法环",
    };
    EXPECT_TRUE(GetTempPathW(MAX_PATH, temp) != 0);
    EXPECT_TRUE(GetTempFileNameW(temp, L"yaf", 0, fixture_root) != 0);
    EXPECT_TRUE(DeleteFileW(fixture_root));
    EXPECT_TRUE(CreateDirectoryW(fixture_root, NULL));
    for (size_t i = 0; i < sizeof(directories) / sizeof(directories[0]); i++) {
        EXPECT_TRUE(fixture_path(path, directories[i]));
        EXPECT_TRUE(CreateDirectoryW(path, NULL));
    }
    EXPECT_TRUE(fixture_path(steam_root, L"Steam"));
    EXPECT_TRUE(fixture_path(extra_root, L"额外游戏库"));
    _snwprintf(registry_path, 128, L"Software\\YAFSMLSteamTest_%lu_%llu",
               GetCurrentProcessId(), GetTickCount64());
    EXPECT_EQ(RegCreateKeyExW(HKEY_CURRENT_USER, registry_path, 0, NULL, 0,
                              KEY_ALL_ACCESS, NULL, &registry_root, NULL), ERROR_SUCCESS);
    /* Redirect only this process; the user's actual Steam registry is untouched. */
    EXPECT_EQ(RegOverridePredefKey(HKEY_CURRENT_USER, registry_root), ERROR_SUCCESS);
    EXPECT_EQ(RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", 0, NULL, 0,
                              KEY_ALL_ACCESS, NULL, &steam_key, NULL), ERROR_SUCCESS);
    LONG result = RegSetValueExW(steam_key, L"SteamPath", 0, REG_SZ,
                                (const BYTE *)steam_root,
                                (DWORD)((wcslen(steam_root) + 1) * sizeof(wchar_t)));
    RegCloseKey(steam_key);
    EXPECT_EQ(result, ERROR_SUCCESS);
    return 0;
}

static void cleanup_fixture(void) {
    wchar_t path[MAX_PATH];
    const wchar_t *files[] = {
        L"Steam\\config\\libraryfolders.vdf",
        L"Steam\\steamapps\\libraryfolders.vdf",
        L"Steam\\steamapps\\appmanifest_1245620.acf",
        L"额外游戏库\\steamapps\\appmanifest_1245620.acf",
    };
    const wchar_t *directories[] = {
        L"Steam\\steamapps\\common\\ELDEN RING", L"Steam\\steamapps\\common\\1245620",
        L"Steam\\steamapps\\common",
        L"Steam\\steamapps", L"Steam\\config", L"Steam",
        L"额外游戏库\\steamapps\\common\\艾尔登法环", L"额外游戏库\\steamapps\\common",
        L"额外游戏库\\steamapps", L"额外游戏库",
    };
    if (registry_root != NULL) {
        RegOverridePredefKey(HKEY_CURRENT_USER, NULL);
        RegCloseKey(registry_root);
        RegDeleteTreeW(HKEY_CURRENT_USER, registry_path);
    }
    if (fixture_root[0] == L'\0') return;
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++)
        if (fixture_path(path, files[i])) DeleteFileW(path);
    for (size_t i = 0; i < sizeof(directories) / sizeof(directories[0]); i++)
        if (fixture_path(path, directories[i])) RemoveDirectoryW(path);
    RemoveDirectoryW(fixture_root);
}

static int expect_found(const wchar_t *relative) {
    wchar_t found[MAX_PATH];
    wchar_t expected[MAX_PATH];
    EXPECT_TRUE(fixture_path(expected, relative));
    EXPECT_TRUE(app_find_game_path(1245620, found));
    EXPECT_STREQ_W(found, expected);
    return 0;
}

static int expect_not_found(uint32_t app_id) {
    wchar_t found[MAX_PATH];
    wcscpy(found, L"unchanged");
    EXPECT_TRUE(!app_find_game_path(app_id, found));
    EXPECT_STREQ_W(found, L"");
    return 0;
}

static int test_manifests(const char *steam_path) {
    static const char *const rejected[] = {
        /* Another app, another or no universe, or no installed directory. */
        "\"AppState\" {\"appid\" \"1245621\" \"Universe\" \"1\" \"installdir\" \"ELDEN RING\"}",
        "\"AppState\" {\"appid\" \"1245620\" \"Universe\" \"2\" \"installdir\" \"ELDEN RING\"}",
        "\"AppState\" {\"appid\" \"1245620\" \"installdir\" \"ELDEN RING\"}",
        "\"AppState\" {\"appid\" \"1245620\" \"Universe\" \"1\" \"installdir\" \"Not Installed\"}",
        "\"AppState\" {\"appid\" \"1245620\" \"Universe\" \"1\" \"installdir\" {}}",
        "\"AppState\" {\"appid\" \"1245620\" \"Universe\" \"1\"}",
        /* Values only count inside the first entry. */
        "\"AppState\" {} \"AppState\" {\"appid\" \"1245620\" \"Universe\" \"1\" \"installdir\" \"ELDEN RING\"}",
    };
    char libraries[MAX_PATH * 6 + 256];
    /* Reproduce an installed game omitted from Steam's cached apps list. */
    snprintf(libraries, sizeof(libraries),
             "\"libraryfolders\"\n{\n\"0\" {\"path\" \"%s\" \"apps\" {\"374320\" \"1\"}}\n}\n",
             steam_path);
    EXPECT_TRUE(write_fixture(L"Steam\\steamapps\\libraryfolders.vdf", libraries));
    EXPECT_TRUE(write_fixture(L"Steam\\steamapps\\appmanifest_1245620.acf",
                              "\"AppState\"\n{\n\"appid\" \"1245620\"\n\"Universe\" \"1\"\n"
                              "\"installdir\" \"ELDEN RING\"\n}\n"));
    EXPECT_EQ(expect_found(L"Steam\\steamapps\\common\\ELDEN RING"), 0);
    EXPECT_EQ(expect_not_found(999999999), 0);

    for (size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); i++) {
        EXPECT_TRUE(write_fixture(L"Steam\\steamapps\\appmanifest_1245620.acf", rejected[i]));
        EXPECT_EQ(expect_not_found(1245620), 0);
    }

    /* Steam takes the first entry whatever its name, key names ignore case,
     * GetInt reads hex, and an all-digit install directory is still a name. */
    EXPECT_TRUE(write_fixture(L"Steam\\steamapps\\appmanifest_1245620.acf",
                              "\"State\" {\"APPID\" \"0x1301b4\" \"universe\" \"+1\" "
                              "\"InstallDir\" \"1245620\"}"));
    return expect_found(L"Steam\\steamapps\\common\\1245620");
}

static int test_libraries(const char *steam_path, const char *extra_path) {
    char libraries[MAX_PATH * 12 + 512];
    wchar_t cwd[MAX_PATH];
    int result;
    const char *rejected[] = {
        /* The "0" entry always means Steam's own folder. */
        "\"libraryfolders\" {\"0\" {\"path\" \"%s\"}}",
        /* Older string entries and empty paths are not libraries. */
        "\"LibraryFolders\" {\"TimeNextStatsReport\" \"1\" \"1\" \"%s\"}",
        "\"libraryfolders\" {\"1\" {\"path\" \"\"} \"2\" {}}",
        /* Only the first entry is the library list. */
        "\"libraryfolders\" {} \"libraryfolders\" {\"1\" {\"path\" \"%s\"}}",
    };
    /* Secondary libraries also work without an apps index, with CRLF and
     * UTF-8, and are searched before Steam's own folder. */
    snprintf(libraries, sizeof(libraries),
             "\"libraryfolders\"\r\n{\r\n\"0\" {\"path\" \"%s\"}\r\n"
             "\"1\" {\"path\" \"%s\"}\r\n}\r\n", steam_path, extra_path);
    EXPECT_TRUE(write_fixture(L"Steam\\steamapps\\libraryfolders.vdf", libraries));
    EXPECT_TRUE(write_fixture(L"额外游戏库\\steamapps\\appmanifest_1245620.acf",
                              "\"AppState\"\r\n{\r\n\"appid\" \"1245620\"\r\n\"Universe\" \"1\"\r\n"
                              "\"installdir\" \"艾尔登法环\"\r\n}\r\n"));
    EXPECT_EQ(expect_found(L"额外游戏库\\steamapps\\common\\艾尔登法环"), 0);

    /* Library block names do not matter. */
    snprintf(libraries, sizeof(libraries), "\"x\" {\"library\" {\"PATH\" \"%s\"}}", extra_path);
    EXPECT_TRUE(write_fixture(L"Steam\\steamapps\\libraryfolders.vdf", libraries));
    EXPECT_EQ(expect_found(L"额外游戏库\\steamapps\\common\\艾尔登法环"), 0);

    for (size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); i++) {
        snprintf(libraries, sizeof(libraries), rejected[i], extra_path);
        EXPECT_TRUE(write_fixture(L"Steam\\steamapps\\libraryfolders.vdf", libraries));
        EXPECT_EQ(expect_found(L"Steam\\steamapps\\common\\1245620"), 0);
    }

    /* A relative path is not a library, even from where it names one. */
    EXPECT_TRUE(write_fixture(L"Steam\\steamapps\\libraryfolders.vdf",
                              "\"libraryfolders\" {\"1\" {\"path\" \"额外游戏库\"}}"));
    EXPECT_TRUE(GetCurrentDirectoryW(MAX_PATH, cwd) != 0);
    EXPECT_TRUE(SetCurrentDirectoryW(fixture_root));
    result = expect_found(L"Steam\\steamapps\\common\\1245620");
    EXPECT_TRUE(SetCurrentDirectoryW(cwd));
    EXPECT_EQ(result, 0);

    /* Steam falls back to config\libraryfolders.vdf. */
    snprintf(libraries, sizeof(libraries), "\"libraryfolders\" {\"1\" {\"path\" \"%s\"}}", extra_path);
    EXPECT_TRUE(write_fixture(L"Steam\\config\\libraryfolders.vdf", libraries));
    EXPECT_TRUE(write_fixture(L"Steam\\steamapps\\libraryfolders.vdf", "\"libraryfolders\" {"));
    EXPECT_EQ(expect_found(L"额外游戏库\\steamapps\\common\\艾尔登法环"), 0);
    EXPECT_TRUE(write_fixture(L"Steam\\steamapps\\libraryfolders.vdf", ""));
    EXPECT_EQ(expect_found(L"额外游戏库\\steamapps\\common\\艾尔登法环"), 0);

    /* Steam's own folder remains a library without any library index. */
    EXPECT_TRUE(write_fixture(L"Steam\\config\\libraryfolders.vdf", "invalid\n"));
    return expect_found(L"Steam\\steamapps\\common\\1245620");
}

static int test_discovery(void) {
    char steam_path[MAX_PATH * 6];
    char extra_path[MAX_PATH * 6];
    wchar_t path[MAX_PATH];
    EXPECT_TRUE(escape_vdf_path(steam_root, steam_path, sizeof(steam_path)));
    EXPECT_TRUE(escape_vdf_path(extra_root, extra_path, sizeof(extra_path)));
    EXPECT_EQ(test_manifests(steam_path), 0);
    EXPECT_EQ(test_libraries(steam_path, extra_path), 0);

    EXPECT_TRUE(fixture_path(path, L"Steam\\steamapps\\libraryfolders.vdf"));
    EXPECT_TRUE(DeleteFileW(path));
    EXPECT_TRUE(fixture_path(path, L"Steam\\config\\libraryfolders.vdf"));
    EXPECT_TRUE(DeleteFileW(path));
    EXPECT_EQ(expect_found(L"Steam\\steamapps\\common\\1245620"), 0);
    EXPECT_TRUE(!app_find_game_path(1245620, NULL));
    return 0;
}

int main(int argc, char **argv) {
    int result;
    if (argc == 3 && strcmp(argv[1], "--local") == 0) {
        uint32_t app_id = (uint32_t)strtoul(argv[2], NULL, 10);
        wchar_t path[MAX_PATH];
        bool found = app_find_game_path(app_id, path);
        wprintf(L"app_find_game_path(%u): found=%d, path=%ls\n", app_id, found, path);
        return found && path[0] != L'\0' ? 0 : 1;
    }
    result = setup_fixture();
    if (result == 0) result = test_discovery();
    cleanup_fixture();
    if (result == 0) printf("smoke_steam_app: all tests passed\n");
    return result;
}
