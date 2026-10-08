#include <stdio.h>
#include <stdlib.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <getopt.h>
#include <shlwapi.h>

#include "test_common.h"
#include "steam/app.h"

static wchar_t test_launcher_path[MAX_PATH];
static wchar_t test_config_path[MAX_PATH];
static int test_argc;
static wchar_t **test_argv;
static unsigned steam_searches;
static uint32_t selected_app_id;

static DWORD WINAPI test_get_module_filename(HMODULE module, LPWSTR path, DWORD count) {
    (void)module;
    lstrcpynW(path, test_launcher_path, (int)count);
    return (DWORD)lstrlenW(path);
}

/* Stop before process creation while recording the game the launcher chose. */
bool app_find_game_path(uint32_t app_id, wchar_t *path) {
    steam_searches++;
    selected_app_id = app_id;
    path[0] = L'\0';
    return false;
}

#undef __argc
#undef __wargv
#define __argc test_argc
#define __wargv test_argv
#define GetModuleFileNameW test_get_module_filename
#include "../src/launcher/launcher.c"
#undef GetModuleFileNameW
#undef __wargv
#undef __argc

static int run_launcher(const wchar_t *explicit_key, unsigned expected_searches,
                         uint32_t expected_app_id) {
    wchar_t *args[] = { L"YAFSML.exe", L"--config", test_config_path,
                       L"--launch-target", (wchar_t *)explicit_key, NULL };
    test_argc = explicit_key == NULL ? 3 : 5;
    test_argv = args;
    optind = 0;
    full_game_path[0] = L'\0';
    full_config_path[0] = L'\0';
    full_modengine_dll[0] = L'\0';
    launch_game = NULL;
    launch_game_explicit = false;
    steam_searches = 0;
    selected_app_id = 0;
    EXPECT_EQ(wWinMain(NULL, NULL, L"", 0), -1);
    EXPECT_EQ(steam_searches, expected_searches);
    EXPECT_EQ(selected_app_id, expected_app_id);
    return 0;
}

static int test_config(const char *contents, const wchar_t *explicit_key,
                        unsigned expected_searches, uint32_t expected_app_id) {
    FILE *file = _wfopen(test_config_path, L"wb");
    EXPECT_NOT_NULL(file);
    EXPECT_EQ(fwrite(contents, 1, strlen(contents), file), strlen(contents));
    EXPECT_EQ(fclose(file), 0);
    return run_launcher(explicit_key, expected_searches, expected_app_id);
}

int main(void) {
    wchar_t temp_dir[MAX_PATH];
    wchar_t root[MAX_PATH];
    wchar_t dll_path[MAX_PATH];
    HANDLE locked_file;
    FILE *file;
    char *long_comment = malloc(32768);
    EXPECT_NOT_NULL(long_comment);
    EXPECT_TRUE(GetTempPathW(MAX_PATH, temp_dir) != 0);
    EXPECT_TRUE(GetTempFileNameW(temp_dir, L"yaf", 0, root) != 0);
    EXPECT_TRUE(DeleteFileW(root));
    EXPECT_TRUE(CreateDirectoryW(root, NULL));
    EXPECT_NOT_NULL(PathCombineW(test_launcher_path, root, L"YAFSML.exe"));
    EXPECT_NOT_NULL(PathCombineW(test_config_path, root, L"YAFSML.ini"));
    EXPECT_NOT_NULL(PathCombineW(dll_path, root, L"YAFSML.dll"));
    file = _wfopen(dll_path, L"wb");
    EXPECT_NOT_NULL(file);
    EXPECT_EQ(fclose(file), 0);

    EXPECT_EQ(test_config("game=nightreign\n", NULL, 1, 2622380), 0);
    memcpy(long_comment, "game=nightreign\n;", strlen("game=nightreign\n;"));
    memset(long_comment + strlen("game=nightreign\n;"), 'x', 16384);
    long_comment[strlen("game=nightreign\n;") + 16384] = '\0';
    EXPECT_EQ(test_config(long_comment, NULL, 1, 2622380), 0);
    EXPECT_EQ(test_config("game=nightreign\n[broken\n", NULL, 0, 0), 0);
    EXPECT_EQ(test_config("game=not-a-game\n", NULL, 0, 0), 0);
    EXPECT_EQ(test_config("[patch]\nskip_intro=1\n", NULL, 1, 1245620), 0);
    EXPECT_EQ(test_config("game=eldenring\n", L"nightreign", 1, 2622380), 0);
    EXPECT_EQ(test_config("[broken\n", L"nightreign", 1, 2622380), 0);

    locked_file = CreateFileW(test_config_path, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    EXPECT_TRUE(locked_file != INVALID_HANDLE_VALUE);
    EXPECT_EQ(run_launcher(NULL, 0, 0), 0);
    EXPECT_TRUE(CloseHandle(locked_file));
    EXPECT_TRUE(DeleteFileW(test_config_path));

    /* Valid ModEngine2 TOML must not be rejected by the INI error check. */
    EXPECT_NOT_NULL(PathCombineW(test_config_path, root, L"config_eldenring.toml"));
    EXPECT_EQ(test_config("[extension.mod_loader]\nmods = [\n  { path = 'mod', enabled = true }\n]\n",
                          NULL, 1, 1245620), 0);
    EXPECT_TRUE(DeleteFileW(test_config_path));
    EXPECT_TRUE(DeleteFileW(dll_path));
    EXPECT_TRUE(RemoveDirectoryW(root));
    free(long_comment);
    printf("smoke_launcher_startup: all tests passed\n");
    return 0;
}
