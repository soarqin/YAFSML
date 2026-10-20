#include <stdio.h>
#include <stdlib.h>
#include <miniz.h>

#include "test_common.h"
#include "test_vfs.h"
#include "common/path.h"
#include "modloader/config.h"
#include "modloader/extdll.h"
#include "modloader/mod.h"
#include "modloader/vfs.h"
#include "launcher/launch_config.h"
#include "launcher/launch_process.h"

static wchar_t *directories[32];
static wchar_t *files[32];
static size_t directory_count, file_count;

static wchar_t *directory(const wchar_t *parent, const wchar_t *name) {
    wchar_t *path = ml_path_resolve_alloc(parent, name);
    if (path == NULL) return NULL;
    if (!CreateDirectoryW(path, NULL)) {
        ml_mem_free(path);
        return NULL;
    }
    directories[directory_count++] = path;
    return path;
}

static wchar_t *filename(const wchar_t *parent, const wchar_t *name) {
    wchar_t *path = ml_path_resolve_alloc(parent, name);
    if (path != NULL) files[file_count++] = path;
    return path;
}

static int write_file(const wchar_t *path, const char *contents) {
    FILE *file = _wfopen(path, L"wb");
    EXPECT_NOT_NULL(file);
    EXPECT_TRUE(fputs(contents, file) >= 0);
    EXPECT_EQ(fclose(file), 0);
    return 0;
}

static bool read_matches(const wchar_t *path, const char *expected) {
    char data[16];
    size_t length;
    return test_vfs_read(path, data, sizeof(data), &length) &&
           length == strlen(expected) && memcmp(data, expected, length) == 0;
}

static wchar_t *relative_to(const wchar_t *base, const wchar_t *path) {
    wchar_t *plain = ml_path_copy_alloc(path);
    wchar_t *directory = ml_path_copy_alloc(base);
    wchar_t *relative = NULL;
    if (plain != NULL && directory != NULL) {
        ml_path_normalize_input(plain);
        ml_path_normalize_input(directory);
        size_t length = wcslen(directory);
        if (wcsncmp(plain, directory, length) == 0 && plain[length] == L'\\')
            relative = ml_path_copy_alloc(plain + length + 1);
    }
    ml_mem_free(directory);
    ml_mem_free(plain);
    return relative;
}

static char *utf8(const wchar_t *text) {
    int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
    char *result = length == 0 ? NULL : ml_mem_alloc(0, (size_t)length);
    if (result != NULL) WideCharToMultiByte(CP_UTF8, 0, text, -1, result, length, NULL, NULL);
    return result;
}

static int settings(const wchar_t *path, const wchar_t *mod, const wchar_t *dll, bool toml) {
    char *mod_utf8 = utf8(mod), *dll_utf8 = utf8(dll);
    FILE *file = _wfopen(path, L"wb");
    EXPECT_NOT_NULL(file);
    EXPECT_NOT_NULL(mod_utf8);
    EXPECT_NOT_NULL(dll_utf8);
    if (toml) {
        EXPECT_TRUE(fprintf(file, "[modengine]\nexternal_dlls = ['%s']\n"
                                 "[extension.mod_loader]\nenabled = true\nmods = [{path = '%s'}]\n",
                            dll_utf8, mod_utf8) > 0);
    } else {
        EXPECT_TRUE(fprintf(file, "game=sekiro\n[dll]\nplugin=%s\n[mod]\nloose=%s\n",
                            dll_utf8, mod_utf8) > 0);
    }
    ml_mem_free(mod_utf8);
    ml_mem_free(dll_utf8);
    EXPECT_EQ(fclose(file), 0);
    return 0;
}

static int load_config(HMODULE module, const wchar_t *configuration, const wchar_t *plugin,
                       const char *contents, const wchar_t *deep_virtual) {
    wchar_t *actual, *expected;
    HMODULE loaded;
    EXPECT_TRUE(SetEnvironmentVariableW(L"YAFSML_CONFIG", configuration));
    config_init(module);
    mods_init();
    config_load();
    EXPECT_EQ(mods_count(), 1);
    EXPECT_TRUE(read_matches(mods_file_search(L"parts/test.bin"), contents));
    if (deep_virtual != NULL) EXPECT_TRUE(read_matches(mods_file_search(deep_virtual), "long"));
    EXPECT_EQ(extdlls_count(), 1);
    extdlls_test_load_at(0);
    loaded = GetModuleHandleW(ml_path_filename(plugin));
    EXPECT_NOT_NULL(loaded);
    actual = ml_path_module_filename_alloc(loaded);
    EXPECT_NOT_NULL(actual);
    expected = ml_path_resolve_alloc(NULL, actual);
    EXPECT_NOT_NULL(expected);
    EXPECT_STREQ_W(expected, plugin);
    ml_mem_free(actual);
    ml_mem_free(expected);
    EXPECT_TRUE(ml_launcher_find_remote_module(GetCurrentProcess(), plugin) == loaded);
    extdlls_unload_all();
    mods_uninit();
    return 0;
}

static int test_resolution(void) {
    static const struct { const wchar_t *base, *input, *expected; } cases[] = {
        { L"C:\\Pack\\profiles", L"../unused\\..\\x.dll", L"C:\\Pack\\x.dll" },
        { L"C:/Pack/profiles", L"./mods/../x.dll", L"C:\\Pack\\profiles\\x.dll" },
        { L"D:\\Pack", L"/mods/x.dll", L"D:\\mods\\x.dll" },
        { L"D:\\Pack", L"C:/other/../x.dll", L"C:\\x.dll" },
        { L"\\\\server\\share\\Pack", L"/mods/x.dll", L"\\\\server\\share\\mods\\x.dll" },
        { L"C:\\Pack", L"\\\\?\\UNC\\server\\share\\mods\\..\\x.dll", L"\\\\server\\share\\x.dll" },
        { L"C:\\Pack", L"\\\\.\\C:\\Pack\\.\\x.dll", L"C:\\Pack\\x.dll" },
        { L"C:\\Pack", L"//./UNC/server/share/x.dll", L"\\\\server\\share\\x.dll" },
        { L"\\\\?\\UNC\\server\\share\\Pack", L"/mods/x.dll", L"\\\\server\\share\\mods\\x.dll" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        wchar_t *result = ml_path_resolve_alloc(cases[i].base, cases[i].input);
        EXPECT_NOT_NULL(result);
        EXPECT_STREQ_W(result, cases[i].expected);
        ml_mem_free(result);
    }
    EXPECT_NULL(ml_path_resolve_alloc(L"D:\\Pack", L"C:x.dll"));
    EXPECT_EQ(GetLastError(), ERROR_BAD_PATHNAME);
    /* Module paths can carry an extended prefix; the root is never removed. */
    wchar_t unc_module[] = L"\\\\?\\UNC\\server\\share\\x.dll";
    ml_path_remove_filename(unc_module);
    EXPECT_STREQ_W(unc_module, L"\\\\?\\UNC\\server\\share\\");
    wchar_t drive_module[] = L"\\\\?\\C:\\x.dll";
    ml_path_remove_filename(drive_module);
    EXPECT_STREQ_W(drive_module, L"\\\\?\\C:\\");
    wchar_t base[241] = L"C:\\";
    wmemset(base + 3, L'a', 237);
    base[240] = L'\0';
    wchar_t *result = ml_path_resolve_alloc(base, L"../extdll_fixture.dll");
    EXPECT_NOT_NULL(result);
    EXPECT_STREQ_W(result, L"C:\\extdll_fixture.dll");
    ml_mem_free(result);
    wchar_t unc_tail[321];
    wmemset(unc_tail, L'a', 320);
    for (size_t i = 63; i < 320; i += 64) unc_tail[i] = L'\\';
    unc_tail[320] = L'\0';
    result = ml_path_resolve_alloc(L"\\\\server\\share\\Pack", unc_tail);
    EXPECT_NOT_NULL(result);
    EXPECT_TRUE(wcsncmp(result, L"\\\\?\\UNC\\server\\share\\Pack\\", wcslen(L"\\\\?\\UNC\\server\\share\\Pack\\")) == 0);
    ml_mem_free(result);
    /* A long device path keeps its drive instead of becoming a UNC name. */
    wchar_t device[288] = L"\\\\.\\C:\\";
    wmemset(device + 7, L'b', 280);
    device[287] = L'\0';
    result = ml_path_resolve_alloc(NULL, device);
    EXPECT_NOT_NULL(result);
    EXPECT_STREQ_W(result + 4, device + 4);
    EXPECT_TRUE(wcsncmp(result, L"\\\\?\\", 4) == 0);
    ml_mem_free(result);
    return 0;
}

int main(void) {
    wchar_t temporary[MAX_PATH], seed[MAX_PATH];
    wchar_t *root, *loader, *profile, *loose, *parts, *deep, *long_mod, *long_parts;
    wchar_t *short_loader, *short_plugin, *long_loader, *long_plugin, *short_ini, *long_ini, *zip;
    wchar_t *source, *source_module, *long_mod_relative, *long_dll_relative, *deep_virtual;
    wchar_t *long_file, *zip_relative, *zip_spec, *original_directory;
    HMODULE short_module, long_module;
    FILE *file;
    mz_zip_archive archive = { 0 };
    EXPECT_EQ(test_resolution(), 0);
    EXPECT_TRUE(GetTempPathW(MAX_PATH, temporary) != 0);
    EXPECT_TRUE(GetTempFileNameW(temporary, L"mlp", 0, seed) != 0);
    EXPECT_TRUE(DeleteFileW(seed));
    root = directory(NULL, seed);
    EXPECT_NOT_NULL(root);
    loader = directory(root, L"加载器");
    profile = directory(root, L"profile");
    EXPECT_NOT_NULL(loader);
    EXPECT_NOT_NULL(profile);
    loose = directory(loader, L"mod");
    EXPECT_NOT_NULL(loose);
    parts = directory(loose, L"parts");
    EXPECT_NOT_NULL(parts);
    EXPECT_EQ(write_file(filename(parts, L"test.bin"), "short"), 0);
    deep = parts;
    for (size_t i = 0; i < 10; ++i) {
        deep = directory(deep, L"long-directory-abcdefghijklmnopqrstuvwxyz-0123456789");
        EXPECT_NOT_NULL(deep);
    }
    EXPECT_TRUE(wcslen(deep) > 500);
    long_mod = directory(deep, L"mod");
    EXPECT_NOT_NULL(long_mod);
    long_parts = directory(long_mod, L"parts");
    EXPECT_NOT_NULL(long_parts);
    long_file = filename(long_parts, L"test.bin");
    EXPECT_EQ(write_file(long_file, "long"), 0);

    source_module = ml_path_module_filename_alloc(NULL);
    EXPECT_NOT_NULL(source_module);
    ml_path_remove_filename(source_module);
    source = ml_path_resolve_alloc(source_module, L"extdll_fixture.dll");
    ml_mem_free(source_module);
    EXPECT_NOT_NULL(source);
    short_loader = filename(loader, L"loader.dll");
    short_plugin = filename(root, L"plugin.dll");
    long_loader = filename(deep, L"loader.dll");
    long_plugin = filename(deep, L"plugin-long.dll");
    EXPECT_TRUE(CopyFileW(source, short_loader, TRUE));
    EXPECT_TRUE(CopyFileW(source, short_plugin, TRUE));
    EXPECT_TRUE(CopyFileW(source, long_loader, TRUE));
    EXPECT_TRUE(CopyFileW(source, long_plugin, TRUE));
    ml_mem_free(source);
    short_module = LoadLibraryExW(short_loader, NULL, DONT_RESOLVE_DLL_REFERENCES);
    long_module = LoadLibraryExW(long_loader, NULL, DONT_RESOLVE_DLL_REFERENCES);
    EXPECT_NOT_NULL(short_module);
    EXPECT_NOT_NULL(long_module);
    original_directory = ml_path_resolve_alloc(NULL, L".");
    EXPECT_NOT_NULL(original_directory);
    EXPECT_TRUE(SetCurrentDirectoryW(profile));

    short_ini = filename(profile, L"YAFSML.ini");
    EXPECT_EQ(settings(short_ini, L"mod", L"../unused\\..\\plugin.dll", false), 0);
    deep_virtual = relative_to(loose, long_file);
    EXPECT_NOT_NULL(deep_virtual);
    EXPECT_EQ(load_config(short_module, short_ini, short_plugin, "short", deep_virtual), 0);
    ml_mem_free(deep_virtual);
    wchar_t *rooted = config_full_path_alloc(short_plugin + 2);
    EXPECT_NOT_NULL(rooted);
    EXPECT_STREQ_W(rooted, short_plugin);
    ml_mem_free(rooted);

    long_ini = filename(deep, L"YAFSML.ini");
    long_mod_relative = relative_to(loader, long_mod);
    long_dll_relative = relative_to(loader, long_plugin);
    EXPECT_NOT_NULL(long_mod_relative);
    EXPECT_NOT_NULL(long_dll_relative);
    EXPECT_TRUE(wcslen(long_dll_relative) > MAX_PATH);
    EXPECT_EQ(settings(long_ini, long_mod_relative, long_dll_relative, false), 0);
    wchar_t *forward_ini = ml_path_copy_alloc(long_ini);
    EXPECT_NOT_NULL(forward_ini);
    ml_path_normalize_input(forward_ini);
    for (wchar_t *p = forward_ini; *p != L'\0'; ++p) if (*p == L'\\') *p = L'/';
    EXPECT_EQ(load_config(short_module, forward_ini, long_plugin, "long", NULL), 0);
    ml_mem_free(forward_ini);
    wchar_t *launcher_config = ml_launcher_resolve_config_path_alloc(loader, long_ini);
    EXPECT_NOT_NULL(launcher_config);
    EXPECT_STREQ_W(launcher_config, long_ini);
    const ml_game_descriptor_t *game = NULL;
    EXPECT_EQ(ml_launcher_game_from_ini_file(launcher_config, &game, NULL, 0), ML_LAUNCHER_GAME_CONFIG_FOUND);
    EXPECT_EQ(game->id, ML_GAME_SEKIRO);
    free(launcher_config);

#ifndef ML_STRIP_MODENGINE_CONFIG
    wchar_t *toml = filename(deep, L"custom.toml");
    EXPECT_EQ(settings(toml, long_mod_relative, long_dll_relative, true), 0);
    EXPECT_EQ(load_config(short_module, toml, long_plugin, "long", NULL), 0);
#endif

    zip = filename(deep, L"package.zip");
    file = _wfopen(zip, L"wb");
    EXPECT_NOT_NULL(file);
    EXPECT_TRUE(mz_zip_writer_init_cfile(&archive, file, 0));
    EXPECT_TRUE(mz_zip_writer_add_mem(&archive, "selected/parts/test.bin", "zip", 3, 6));
    EXPECT_TRUE(mz_zip_writer_finalize_archive(&archive));
    EXPECT_TRUE(mz_zip_writer_end(&archive));
    EXPECT_EQ(fclose(file), 0);
    zip_relative = relative_to(loader, zip);
    EXPECT_NOT_NULL(zip_relative);
    zip_spec = ml_path_buffer(wcslen(zip_relative) + 12);
    EXPECT_NOT_NULL(zip_spec);
    wcscpy(zip_spec, zip_relative);
    wcscat(zip_spec, L":/selected");
    EXPECT_EQ(settings(long_ini, zip_spec, long_dll_relative, false), 0);
    EXPECT_EQ(load_config(short_module, long_ini, long_plugin, "zip", NULL), 0);
    ml_mem_free(zip_spec);
    ml_mem_free(zip_relative);
    zip_spec = ml_path_buffer(wcslen(zip) + 12);
    EXPECT_NOT_NULL(zip_spec);
    wcscpy(zip_spec, zip);
    wcscat(zip_spec, L":/selected");
    mods_init();
    mods_add("extended-zip", zip_spec);
    EXPECT_EQ(mods_count(), 1);
    EXPECT_TRUE(read_matches(mods_file_search(L"parts/test.bin"), "zip"));
    mods_uninit();
    ml_mem_free(zip_spec);
    ml_mem_free(long_mod_relative);
    ml_mem_free(long_dll_relative);

    /* The DLL itself is in a long directory, and chooses its adjacent INI. */
    EXPECT_EQ(settings(long_ini, L"mod", L"./ignored/..\\plugin-long.dll", false), 0);
    EXPECT_EQ(load_config(long_module, NULL, long_plugin, "long", NULL), 0);
    /* Exercise the actual LoadLibraryW and initialization threads as well as
     * remote module enumeration, without starting a game process. */
    HANDLE process = OpenProcess(PROCESS_ALL_ACCESS, FALSE, GetCurrentProcessId());
    EXPECT_NOT_NULL(process);
    bool injected_ok = ml_launcher_inject_dll(process, long_plugin);
    if (!injected_ok) fprintf(stderr, "long-path injection failed: error %lu\n", GetLastError());
    EXPECT_TRUE(injected_ok);
    EXPECT_TRUE(CloseHandle(process));
    HMODULE injected = GetModuleHandleW(ml_path_filename(long_plugin));
    EXPECT_NOT_NULL(injected);
    EXPECT_TRUE(FreeLibrary(injected));
    config_uninit();
    EXPECT_TRUE(FreeLibrary(short_module));
    EXPECT_TRUE(FreeLibrary(long_module));
    EXPECT_TRUE(SetCurrentDirectoryW(original_directory));
    ml_mem_free(original_directory);
    SetEnvironmentVariableW(L"YAFSML_CONFIG", NULL);
    while (file_count != 0) {
        wchar_t *path = files[--file_count];
        EXPECT_TRUE(DeleteFileW(path));
        ml_mem_free(path);
    }
    while (directory_count != 0) {
        wchar_t *path = directories[--directory_count];
        EXPECT_TRUE(RemoveDirectoryW(path));
        ml_mem_free(path);
    }
    puts("smoke_loading_paths: all tests passed");
    return 0;
}
