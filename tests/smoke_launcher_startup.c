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
static wchar_t test_steam_folder[MAX_PATH];
static int test_argc;
static wchar_t **test_argv;
static unsigned steam_searches;
static uint32_t selected_app_id;
static unsigned dialogs;
static bool dialog_style_valid;
static wchar_t last_dialog[4096];

static DWORD WINAPI test_get_module_filename(HMODULE module, LPWSTR path, DWORD count) {
    (void)module;
    lstrcpynW(path, test_launcher_path, (int)count);
    return (DWORD)lstrlenW(path);
}

/* Record the game the launcher chose. Without a fixture folder the launcher
 * stops before process creation. */
bool app_find_game_path(uint32_t app_id, wchar_t *path) {
    steam_searches++;
    selected_app_id = app_id;
    lstrcpynW(path, test_steam_folder, MAX_PATH);
    return path[0] != L'\0';
}

/* Error dialogs are recorded instead of blocking the test. */
static int WINAPI test_message_box(HWND window, LPCWSTR text, LPCWSTR caption, UINT type) {
    (void)window;
    dialogs++;
    lstrcpynW(last_dialog, text, (int)(sizeof(last_dialog) / sizeof(last_dialog[0])));
    dialog_style_valid = caption != NULL && wcscmp(caption, L"YAFSML") == 0 &&
                         (type & MB_ICONERROR) == MB_ICONERROR;
    return IDOK;
}

/* Copies of this executable stand in for a YAFSML proxy DLL. */
__declspec(dllexport) DWORD WINAPI YAFSMLInit(LPVOID parameter) {
    (void)parameter;
    return 1;
}

#undef __argc
#undef __wargv
#define __argc test_argc
#define __wargv test_argv
#define GetModuleFileNameW test_get_module_filename
#define MessageBoxW test_message_box
#include "../src/launcher/launcher.c"
#undef MessageBoxW
#undef GetModuleFileNameW
#undef __wargv
#undef __argc

static wchar_t root[MAX_PATH];

static const wchar_t *const fixture_dirs[] = {
    L"configs", L"profile", L"loaders", L"game", L"game\\Game", L"cwd", L"cwd\\game",
    L"cwd\\game\\Game",
};
static const wchar_t *const fixture_files[] = {
    L"YAFSML.dll", L"loaders\\other.dll", L"game\\sekiro.exe", L"game\\custom.exe",
    L"game\\Game\\eldenring.exe", L"game\\Game\\nightreign.exe", L"cwd\\game\\Game\\eldenring.exe",
};

static bool fixture(wchar_t path[MAX_PATH], const wchar_t *relative) {
    return PathCombineW(path, root, relative) != NULL;
}

static int write_file(const wchar_t *path, const char *contents) {
    FILE *file = _wfopen(path, L"wb");
    EXPECT_NOT_NULL(file);
    EXPECT_EQ(fwrite(contents, 1, strlen(contents), file), strlen(contents));
    EXPECT_EQ(fclose(file), 0);
    return 0;
}

static int write_fixture(const wchar_t *relative, const char *contents) {
    wchar_t path[MAX_PATH];
    EXPECT_TRUE(fixture(path, relative));
    return write_file(path, contents);
}

static int enter_fixture_dir(const wchar_t *relative) {
    wchar_t path[MAX_PATH];
    EXPECT_TRUE(fixture(path, relative));
    EXPECT_TRUE(SetCurrentDirectoryW(path));
    return 0;
}

/* Runs the launcher with a NULL-terminated argument list. YAFSML_CONFIG is
 * inherited as `inherited_config`; the other exported variables are cleared. */
static int run_args(const wchar_t *inherited_config, const wchar_t *const *args) {
    wchar_t *argv[16] = { L"YAFSML.exe" };
    int argc = 1;
    while (args != NULL && *args != NULL && argc < 15) argv[argc++] = (wchar_t *)*args++;
    argv[argc] = NULL;
    test_argc = argc;
    test_argv = argv;
    optind = 0;
    game_path_arg = NULL;
    config_arg = NULL;
    dll_arg = NULL;
    suspend = false;
    launch_game = NULL;
    launch_game_explicit = false;
    steam_searches = 0;
    selected_app_id = 0;
    dialogs = 0;
    dialog_style_valid = false;
    last_dialog[0] = L'\0';
    SetEnvironmentVariableW(L"YAFSML_CONFIG", inherited_config);
    SetEnvironmentVariableW(L"YAFSML_GAME", NULL);
    SetEnvironmentVariableW(L"SteamAppId", NULL);
    return wWinMain(NULL, NULL, L"", 0);
}

/* Each failed launch reports exactly one error dialog. */
static int expect_error(const wchar_t *fragment) {
    EXPECT_EQ(dialogs, 1);
    EXPECT_TRUE(dialog_style_valid);
    if (wcsstr(last_dialog, fragment) == NULL) {
        fwprintf(stderr, L"dialog `%ls` lacks `%ls`\n", last_dialog, fragment);
        return 1;
    }
    return 0;
}

static int expect_environment(const wchar_t *name, const wchar_t *expected) {
    wchar_t value[MAX_PATH * 2];
    const DWORD length = GetEnvironmentVariableW(name, value, MAX_PATH * 2);
    if (expected == NULL) {
        EXPECT_EQ(length, 0);
        return 0;
    }
    EXPECT_TRUE(length != 0 && length < MAX_PATH * 2);
    if (wcscmp(value, expected) != 0) {
        fwprintf(stderr, L"%ls is `%ls`, expected `%ls`\n", name, value, expected);
        return 1;
    }
    return 0;
}

static int run_launcher(const wchar_t *explicit_key, unsigned expected_searches,
                        uint32_t expected_app_id) {
    const wchar_t *args[] = { L"--config", test_config_path, L"--launch-target", explicit_key, NULL };
    if (explicit_key == NULL) args[2] = NULL;
    EXPECT_EQ(run_args(NULL, args), -1);
    EXPECT_EQ(steam_searches, expected_searches);
    EXPECT_EQ(selected_app_id, expected_app_id);
    EXPECT_EQ(dialogs, 1);
    EXPECT_TRUE(dialog_style_valid);
    return 0;
}

static int test_config(const char *contents, const wchar_t *explicit_key,
                       unsigned expected_searches, uint32_t expected_app_id) {
    EXPECT_EQ(write_file(test_config_path, contents), 0);
    return run_launcher(explicit_key, expected_searches, expected_app_id);
}

/* Runs a launch that reaches process creation, which fails because the
 * fixture executables are empty files. */
static int expect_start(const wchar_t *inherited_config, const wchar_t *const *args,
                        const wchar_t *game_key, const wchar_t *app_id,
                        const wchar_t *exported_config, const wchar_t *executable) {
    wchar_t path[MAX_PATH];
    EXPECT_EQ(run_args(inherited_config, args), -1);
    EXPECT_EQ(expect_error(L"could not start"), 0);
    EXPECT_TRUE(fixture(path, executable));
    EXPECT_EQ(expect_error(path), 0);
    EXPECT_EQ(expect_environment(L"YAFSML_GAME", game_key), 0);
    EXPECT_EQ(expect_environment(L"SteamAppId", app_id), 0);
    if (exported_config == NULL) return expect_environment(L"YAFSML_CONFIG", NULL);
    EXPECT_TRUE(fixture(path, exported_config));
    return expect_environment(L"YAFSML_CONFIG", path);
}

static int test_config_paths(void) {
    wchar_t absolute[MAX_PATH];
    wchar_t profile[MAX_PATH];
    EXPECT_EQ(write_fixture(L"YAFSML.ini", "game=eldenring\n"), 0);
    EXPECT_EQ(write_fixture(L"configs\\custom.ini", "game=sekiro\n"), 0);
    EXPECT_EQ(write_fixture(L"profile\\YAFSML.ini", "game=nightreign\n"), 0);
    EXPECT_TRUE(fixture(test_steam_folder, L"game"));
    /* Relative paths are relative to the DLL, here next to the launcher, not
     * to the working directory. */
    EXPECT_EQ(enter_fixture_dir(L"cwd"), 0);

    {
        const wchar_t *args[] = { L"-c", L"configs\\custom.ini", NULL };
        EXPECT_EQ(expect_start(NULL, args, L"sekiro", L"814380", L"configs\\custom.ini",
                               L"game\\sekiro.exe"), 0);
        EXPECT_EQ(selected_app_id, 814380);
    }
    /* A directory is exported as is; the DLL picks YAFSML.ini or the TOML file. */
    {
        const wchar_t *args[] = { L"--config", L"profile", NULL };
        EXPECT_EQ(expect_start(NULL, args, L"nightreign", L"2622380", L"profile",
                               L"game\\Game\\nightreign.exe"), 0);
    }
    {
        const wchar_t *args[] = { L"-c", absolute, NULL };
        EXPECT_TRUE(_snwprintf(absolute, MAX_PATH, L"%ls/configs/../configs/custom.ini", root) > 0);
        EXPECT_EQ(expect_start(NULL, args, L"sekiro", L"814380", L"configs\\custom.ini",
                               L"game\\sekiro.exe"), 0);
    }
    /* An inherited YAFSML_CONFIG works exactly like -c and is exported absolute. */
    EXPECT_EQ(expect_start(L"configs\\custom.ini", NULL, L"sekiro", L"814380",
                           L"configs\\custom.ini", L"game\\sekiro.exe"), 0);
    EXPECT_TRUE(fixture(profile, L"profile"));
    EXPECT_EQ(expect_start(profile, NULL, L"nightreign", L"2622380", L"profile",
                           L"game\\Game\\nightreign.exe"), 0);
    {
        const wchar_t *args[] = { L"-c", L"profile", NULL };
        EXPECT_EQ(expect_start(L"configs\\custom.ini", args, L"nightreign", L"2622380",
                               L"profile", L"game\\Game\\nightreign.exe"), 0);
    }
    /* Without either, the default configuration is not exported. */
    EXPECT_EQ(expect_start(NULL, NULL, L"eldenring", L"1245620", NULL,
                           L"game\\Game\\eldenring.exe"), 0);
    test_steam_folder[0] = L'\0';
    return 0;
}

static int test_modloader_dll(void) {
    wchar_t path[MAX_PATH];
    EXPECT_EQ(write_fixture(L"YAFSML.ini", "game=eldenring\n"), 0);
    EXPECT_EQ(enter_fixture_dir(L"cwd"), 0);
    /* An explicit DLL is resolved against the launcher and never replaced. */
    {
        const wchar_t *args[] = { L"-d", L"missing.dll", NULL };
        EXPECT_EQ(run_args(NULL, args), -1);
        EXPECT_EQ(steam_searches, 0);
        EXPECT_TRUE(fixture(path, L"missing.dll"));
        EXPECT_EQ(expect_error(path), 0);
    }
    {
        const wchar_t *args[] = { L"--modloader-dll", L"loaders", NULL };
        EXPECT_EQ(run_args(NULL, args), -1);
        EXPECT_EQ(steam_searches, 0);
        EXPECT_EQ(expect_error(L"could not find mod loader DLL"), 0);
    }
    {
        const wchar_t *args[] = { L"--modengine-dll", L"loaders\\other.dll", NULL };
        EXPECT_EQ(run_args(NULL, args), -1);
        EXPECT_EQ(steam_searches, 1);
        EXPECT_EQ(selected_app_id, 1245620);
        EXPECT_EQ(expect_error(L"through Steam"), 0);
    }
    /* The configuration is the DLL's: its YAFSML.ini by default, and relative
     * paths from its directory. */
    EXPECT_EQ(write_fixture(L"loaders\\YAFSML.ini", "game=sekiro\n"), 0);
    EXPECT_EQ(write_fixture(L"loaders\\custom.ini", "game=nightreign\n"), 0);
    EXPECT_TRUE(fixture(test_steam_folder, L"game"));
    {
        const wchar_t *args[] = { L"-d", L"loaders\\other.dll", NULL };
        EXPECT_EQ(expect_start(NULL, args, L"sekiro", L"814380", NULL, L"game\\sekiro.exe"), 0);
        EXPECT_EQ(expect_start(L"custom.ini", args, L"nightreign", L"2622380", L"loaders\\custom.ini",
                               L"game\\Game\\nightreign.exe"), 0);
    }
    {
        const wchar_t *args[] = { L"-d", L"loaders\\other.dll", L"-c", L"custom.ini", NULL };
        EXPECT_EQ(expect_start(NULL, args, L"nightreign", L"2622380", L"loaders\\custom.ini",
                               L"game\\Game\\nightreign.exe"), 0);
    }
    test_steam_folder[0] = L'\0';
    return 0;
}

static int test_game_path(void) {
    wchar_t path[MAX_PATH];
    EXPECT_EQ(write_fixture(L"YAFSML.ini", "game=eldenring\n"), 0);
    /* A relative directory is relative to the working directory first. */
    EXPECT_EQ(enter_fixture_dir(L"cwd"), 0);
    {
        const wchar_t *args[] = { L"-p", L"game", NULL };
        EXPECT_EQ(expect_start(NULL, args, L"eldenring", L"1245620", NULL,
                               L"cwd\\game\\Game\\eldenring.exe"), 0);
        EXPECT_EQ(steam_searches, 0);
    }
    /* A bare executable name becomes an absolute path with a working directory. */
    EXPECT_EQ(enter_fixture_dir(L"cwd\\game\\Game"), 0);
    {
        const wchar_t *args[] = { L"--game-path", L"eldenring.exe", NULL };
        EXPECT_EQ(expect_start(NULL, args, L"eldenring", L"1245620", NULL,
                               L"cwd\\game\\Game\\eldenring.exe"), 0);
    }
    /* Otherwise it is still relative to the launcher directory. */
    EXPECT_EQ(enter_fixture_dir(L"configs"), 0);
    {
        const wchar_t *args[] = { L"-p", L"game", NULL };
        EXPECT_EQ(expect_start(NULL, args, L"eldenring", L"1245620", NULL,
                               L"game\\Game\\eldenring.exe"), 0);
    }
    /* The executable selects the game over game=... ... */
    EXPECT_TRUE(fixture(path, L"game\\Game\\nightreign.exe"));
    {
        const wchar_t *args[] = { L"-p", path, NULL };
        EXPECT_EQ(expect_start(NULL, args, L"nightreign", L"2622380", NULL,
                               L"game\\Game\\nightreign.exe"), 0);
        EXPECT_EQ(steam_searches, 0);
    }
    /* ... but must agree with an explicit target. */
    {
        const wchar_t *args[] = { L"-t", L"sekiro", L"-p", path, NULL };
        EXPECT_EQ(run_args(NULL, args), -1);
        EXPECT_EQ(expect_error(L"--launch-target"), 0);
        EXPECT_EQ(expect_environment(L"YAFSML_GAME", NULL), 0);
    }
    {
        const wchar_t *args[] = { L"-t", L"nightreign", L"-p", path, NULL };
        EXPECT_EQ(expect_start(NULL, args, L"nightreign", L"2622380", NULL,
                               L"game\\Game\\nightreign.exe"), 0);
    }
    /* Unknown executable names keep the selected game. */
    EXPECT_EQ(write_fixture(L"YAFSML.ini", "game=sekiro\n"), 0);
    EXPECT_TRUE(fixture(path, L"game\\custom.exe"));
    {
        const wchar_t *args[] = { L"-p", path, NULL };
        EXPECT_EQ(expect_start(NULL, args, L"sekiro", L"814380", NULL, L"game\\custom.exe"), 0);
    }
    /* A path without the game is an error; Steam is not searched. */
    {
        const wchar_t *args[] = { L"-p", L"nowhere", NULL };
        EXPECT_EQ(run_args(NULL, args), -1);
        EXPECT_EQ(steam_searches, 0);
        EXPECT_EQ(expect_error(L"at `nowhere`"), 0);
        EXPECT_NULL(wcsstr(last_dialog, L"Steam"));
    }
    return 0;
}

static int test_proxy_copy(void) {
    wchar_t self[MAX_PATH];
    wchar_t proxy[MAX_PATH];
    wchar_t game[MAX_PATH];
    const wchar_t *args[] = { L"-p", game, NULL };
    EXPECT_EQ(write_fixture(L"YAFSML.ini", "game=eldenring\n"), 0);
    EXPECT_TRUE(fixture(game, L"game\\Game\\eldenring.exe"));
    EXPECT_TRUE(GetModuleFileNameW(NULL, self, MAX_PATH) != 0);
    EXPECT_TRUE(fixture(proxy, L"game\\Game\\dinput8.dll"));
    EXPECT_TRUE(CopyFileW(self, proxy, FALSE));
    EXPECT_EQ(run_args(NULL, args), -1);
    EXPECT_EQ(expect_error(L"proxy"), 0);
    EXPECT_EQ(expect_error(proxy), 0);
    /* The launcher stops before exporting anything for the game. */
    EXPECT_EQ(expect_environment(L"YAFSML_GAME", NULL), 0);
    EXPECT_TRUE(DeleteFileW(proxy));

    /* Other DLLs with proxy names, such as graphics wrappers, are accepted. */
    EXPECT_TRUE(GetSystemDirectoryW(self, MAX_PATH) != 0);
    EXPECT_TRUE(PathAppendW(self, L"version.dll"));
    EXPECT_TRUE(fixture(proxy, L"game\\Game\\dxgi.dll"));
    EXPECT_TRUE(CopyFileW(self, proxy, FALSE));
    EXPECT_EQ(expect_start(NULL, args, L"eldenring", L"1245620", NULL,
                           L"game\\Game\\eldenring.exe"), 0);
    EXPECT_TRUE(DeleteFileW(proxy));
    return 0;
}

static int test_error_output(void) {
    static const char expected[] = "ERROR: [launcher] unsupported launch target: 不存在\r\n";
    const wchar_t *args[] = { L"-t", L"不存在", NULL };
    const HANDLE original = GetStdHandle(STD_ERROR_HANDLE);
    wchar_t path[MAX_PATH];
    char contents[256];
    DWORD read = 0;
    HANDLE output;
    EXPECT_TRUE(fixture(path, L"stderr.txt"));
    output = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL,
                         CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, NULL);
    EXPECT_TRUE(output != INVALID_HANDLE_VALUE);
    /* Redirected standard error receives UTF-8 lines. */
    EXPECT_TRUE(SetStdHandle(STD_ERROR_HANDLE, output));
    EXPECT_EQ(run_args(NULL, args), -1);
    EXPECT_TRUE(SetStdHandle(STD_ERROR_HANDLE, original));
    EXPECT_EQ(expect_error(L"unsupported launch target: 不存在"), 0);
    EXPECT_EQ(SetFilePointer(output, 0, NULL, FILE_BEGIN), 0);
    EXPECT_TRUE(ReadFile(output, contents, sizeof(contents) - 1, &read, NULL));
    contents[read] = '\0';
    EXPECT_TRUE(CloseHandle(output));
    EXPECT_STREQ_A(contents, expected);

    /* Process creation errors carry the system error and elevation advice. */
    launch_game = ml_game_by_id(ML_GAME_ELDEN_RING);
    dialogs = 0;
    report_start_failure(ERROR_ELEVATION_REQUIRED, L"C:\\Games\\eldenring.exe");
    EXPECT_EQ(expect_error(L"requires administrator rights (error 740)"), 0);
    dialogs = 0;
    report_start_failure(ERROR_FILE_NOT_FOUND, L"C:\\Games\\eldenring.exe");
    EXPECT_EQ(expect_error(L"(error 2)"), 0);
    return 0;
}

static int setup_fixture(void) {
    wchar_t temp_dir[MAX_PATH];
    wchar_t path[MAX_PATH];
    EXPECT_TRUE(GetTempPathW(MAX_PATH, temp_dir) != 0);
    EXPECT_TRUE(GetTempFileNameW(temp_dir, L"yaf", 0, root) != 0);
    EXPECT_TRUE(DeleteFileW(root));
    EXPECT_TRUE(CreateDirectoryW(root, NULL));
    for (size_t i = 0; i < sizeof(fixture_dirs) / sizeof(fixture_dirs[0]); i++) {
        EXPECT_TRUE(fixture(path, fixture_dirs[i]));
        EXPECT_TRUE(CreateDirectoryW(path, NULL));
    }
    for (size_t i = 0; i < sizeof(fixture_files) / sizeof(fixture_files[0]); i++)
        EXPECT_EQ(write_fixture(fixture_files[i], ""), 0);
    EXPECT_TRUE(fixture(test_launcher_path, L"YAFSML.exe"));
    EXPECT_TRUE(fixture(test_config_path, L"YAFSML.ini"));
    return 0;
}

static void cleanup_fixture(void) {
    static const wchar_t *const generated[] = {
        L"YAFSML.ini", L"config_eldenring.toml", L"configs\\custom.ini", L"profile\\YAFSML.ini",
        L"loaders\\YAFSML.ini", L"loaders\\custom.ini", L"game\\Game\\dinput8.dll", L"game\\Game\\dxgi.dll",
    };
    wchar_t path[MAX_PATH];
    if (root[0] == L'\0') return;
    for (size_t i = 0; i < sizeof(generated) / sizeof(generated[0]); i++)
        if (fixture(path, generated[i])) DeleteFileW(path);
    for (size_t i = 0; i < sizeof(fixture_files) / sizeof(fixture_files[0]); i++)
        if (fixture(path, fixture_files[i])) DeleteFileW(path);
    for (size_t i = sizeof(fixture_dirs) / sizeof(fixture_dirs[0]); i > 0; i--)
        if (fixture(path, fixture_dirs[i - 1])) RemoveDirectoryW(path);
    RemoveDirectoryW(root);
}

static int run_tests(void) {
    HANDLE locked_file;
    char *long_comment = malloc(32768);
    EXPECT_NOT_NULL(long_comment);
    EXPECT_EQ(setup_fixture(), 0);

    EXPECT_EQ(test_config("game=nightreign\n", NULL, 1, 2622380), 0);
    memcpy(long_comment, "game=nightreign\n;", strlen("game=nightreign\n;"));
    memset(long_comment + strlen("game=nightreign\n;"), 'x', 16384);
    long_comment[strlen("game=nightreign\n;") + 16384] = '\0';
    EXPECT_EQ(test_config(long_comment, NULL, 1, 2622380), 0);
    free(long_comment);
    EXPECT_EQ(test_config("game=nightreign\n[broken\n", NULL, 0, 0), 0);
    EXPECT_EQ(test_config("game=not-a-game\n", NULL, 0, 0), 0);
    EXPECT_EQ(expect_error(L"not-a-game"), 0);
    EXPECT_EQ(test_config("[patch]\nskip_intro=1\n", NULL, 1, 1245620), 0);
    EXPECT_EQ(test_config("game=eldenring\n", L"nightreign", 1, 2622380), 0);
    EXPECT_EQ(test_config("[broken\n", L"nightreign", 1, 2622380), 0);

    locked_file = CreateFileW(test_config_path, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    EXPECT_TRUE(locked_file != INVALID_HANDLE_VALUE);
    EXPECT_EQ(run_launcher(NULL, 0, 0), 0);
    EXPECT_TRUE(CloseHandle(locked_file));
    EXPECT_TRUE(DeleteFileW(test_config_path));

    /* Valid ModEngine2 TOML must not be rejected by the INI error check. */
    EXPECT_TRUE(fixture(test_config_path, L"config_eldenring.toml"));
    EXPECT_EQ(test_config("[extension.mod_loader]\nmods = [\n  { path = 'mod', enabled = true }\n]\n",
                          NULL, 1, 1245620), 0);
    EXPECT_TRUE(DeleteFileW(test_config_path));

    EXPECT_EQ(test_config_paths(), 0);
    EXPECT_EQ(test_modloader_dll(), 0);
    EXPECT_EQ(test_game_path(), 0);
    EXPECT_EQ(test_proxy_copy(), 0);
    EXPECT_EQ(test_error_output(), 0);
    return 0;
}

int main(void) {
    wchar_t original_dir[MAX_PATH];
    int result;
    if (GetCurrentDirectoryW(MAX_PATH, original_dir) == 0) return 1;
    result = run_tests();
    SetCurrentDirectoryW(original_dir);
    SetEnvironmentVariableW(L"YAFSML_CONFIG", NULL);
    cleanup_fixture();
    if (result == 0) printf("smoke_launcher_startup: all tests passed\n");
    return result;
}
