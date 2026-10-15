#include <stdio.h>

#include <windows.h>
#include <shlwapi.h>
#include <miniz.h>

#include "test_common.h"
#include "modloader/config.h"
#include "modloader/mod.h"
#include "modloader/vfs.h"
#include "modloader/zip_package.h"

static HMODULE config_module;
void config_load_ini(FILE *file);
#ifndef ML_STRIP_MODENGINE_CONFIG
bool config_load_toml(FILE *file);
#endif

void extdlls_add(const char *name, const wchar_t *path) { (void)name; (void)path; }
void extdlls_add_spec(const char *name, const char *value) { (void)name; (void)value; }

static bool read_matches(const wchar_t *path, const char *contents) {
    char buffer[32];
    DWORD read;
    HANDLE file;
    bool matches;
    if (path == NULL) return false;
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) return false;
    matches = ReadFile(file, buffer, sizeof(buffer), &read, NULL) &&
              read == strlen(contents) && memcmp(buffer, contents, read) == 0;
    CloseHandle(file);
    return matches;
}

static int test_nested_config_paths(void) {
    static const char *paths[] = {
        "模组.zip:def/subdir/mod",
        "模组.zip:/def/subdir/mod",
        "模组.zip:def\\subdir\\mod",
        "模组.zip:\\def\\subdir\\mod",
        "模组.zip:/def\\subdir\\mod",
        "模组.zip:\\def/subdir\\mod",
    };
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        FILE *file = tmpfile();
        EXPECT_NOT_NULL(file);
        EXPECT_TRUE(fputs("[mod]\n;", file) >= 0);
        for (size_t comment_byte = 0; comment_byte < 16384; comment_byte++)
            EXPECT_TRUE(fputc('x', file) != EOF);
        EXPECT_TRUE(fprintf(file, "\nnested=%s ;", paths[i]) >= 0);
        for (size_t comment_byte = 0; comment_byte < 16384; comment_byte++)
            EXPECT_TRUE(fputc('x', file) != EOF);
        EXPECT_TRUE(fputc('\n', file) != EOF);
        rewind(file);
        mods_init();
        config_load_ini(file);
        EXPECT_EQ(fclose(file), 0);
        EXPECT_EQ(mods_count(), 1);
        EXPECT_TRUE(read_matches(mods_file_search(L"parts\\nested.bin"), "nested"));
        EXPECT_NULL(mods_file_search(L"def/subdir/mod/parts/nested.bin"));
        EXPECT_NULL(mods_file_search(L"parts/test.bin"));
        mods_uninit();
#ifndef ML_STRIP_MODENGINE_CONFIG
        file = tmpfile();
        EXPECT_NOT_NULL(file);
        EXPECT_TRUE(fprintf(file, "[extension.mod_loader]\nenabled = true\n"
                            "mods = [ { enabled = true, name = 'nested', path = '%s' } ]\n", paths[i]) >= 0);
        rewind(file);
        mods_init();
        EXPECT_TRUE(config_load_toml(file));
        EXPECT_EQ(fclose(file), 0);
        EXPECT_EQ(mods_count(), 1);
        EXPECT_TRUE(read_matches(mods_file_search(L"data0:/parts/nested.bin"), "nested"));
        EXPECT_NULL(mods_file_search(L"def/subdir/mod/parts/nested.bin"));
        EXPECT_NULL(mods_file_search(L"parts/test.bin"));
        mods_uninit();
#endif
    }
#ifndef ML_STRIP_MODENGINE_CONFIG
    {
        FILE *file = tmpfile();
        EXPECT_NOT_NULL(file);
        /* A TOML basic (double-quoted) string needs escaped backslashes. */
        EXPECT_TRUE(fputs("[extension.mod_loader]\nenabled = true\n"
                          "mods = [ { enabled = true, name = 'nested', path = \"模组.zip:\\\\def\\\\subdir\\\\mod\" } ]\n", file) >= 0);
        rewind(file);
        mods_init();
        EXPECT_TRUE(config_load_toml(file));
        EXPECT_EQ(fclose(file), 0);
        EXPECT_EQ(mods_count(), 1);
        EXPECT_TRUE(read_matches(mods_file_search(L"parts/nested.bin"), "nested"));
        mods_uninit();
    }
#endif
    return 0;
}

static bool write_bytes(const wchar_t *path, const char *contents) {
    FILE *file = _wfopen(path, L"wb");
    bool written;
    if (file == NULL) return false;
    written = fputs(contents, file) >= 0;
    return fclose(file) == 0 && written;
}

/* config_load resolves YAFSML_CONFIG like the launcher: forward slashes and
 * directories are accepted, and a directory without YAFSML.ini falls back to the
 * ModEngine2 TOML. */
static int test_config_resolution(const wchar_t *root, const wchar_t *config_path) {
    wchar_t forward[MAX_PATH];
    wchar_t me2[MAX_PATH];
    wchar_t toml_path[MAX_PATH];
    lstrcpyW(forward, root);
    for (wchar_t *p = forward; *p != L'\0'; p++) {
        if (*p == L'\\') *p = L'/';
    }
    SetEnvironmentVariableW(L"YAFSML_CONFIG", forward);
    config_init(config_module);
    mods_init();
    config_load();
    EXPECT_EQ(mods_count(), 2);
    EXPECT_TRUE(read_matches(mods_file_search(L"parts/test.bin"), "selected"));
    mods_uninit();
    lstrcatW(forward, L"/YAFSML.ini");
    SetEnvironmentVariableW(L"YAFSML_CONFIG", forward);
    config_init(config_module);
    mods_init();
    config_load();
    EXPECT_EQ(mods_count(), 2);
    mods_uninit();
#ifndef ML_STRIP_MODENGINE_CONFIG
    _snwprintf(me2, MAX_PATH, L"%ls\\me2", root);
    _snwprintf(toml_path, MAX_PATH, L"%ls\\config_eldenring.toml", me2);
    EXPECT_TRUE(CreateDirectoryW(me2, NULL));
    EXPECT_TRUE(write_bytes(toml_path, "[extension.mod_loader]\nenabled = true\n"
                                       "mods = [ { path = '模组.zip:def' } ]\n"));
    SetEnvironmentVariableW(L"YAFSML_CONFIG", me2);
    config_init(config_module);
    mods_init();
    config_load();
    EXPECT_EQ(mods_count(), 1);
    EXPECT_TRUE(read_matches(mods_file_search(L"parts/test.bin"), "selected"));
    mods_uninit();

    /* A malformed TOML yields an empty configuration instead of parsing the
     * exhausted stream as INI. */
    EXPECT_TRUE(write_bytes(toml_path, "[extension.mod_loader\nenabled = true\n"));
    SetEnvironmentVariableW(L"YAFSML_CONFIG", me2);
    config_init(config_module);
    mods_init();
    config_load();
    EXPECT_EQ(mods_count(), 0);
    mods_uninit();
    EXPECT_TRUE(DeleteFileW(toml_path));
    EXPECT_TRUE(RemoveDirectoryW(me2));
#else
    (void)me2;
    (void)toml_path;
#endif
    return 0;
}

int main(void) {
    wchar_t temp[MAX_PATH];
    wchar_t root[MAX_PATH];
    wchar_t zip_path[MAX_PATH];
    wchar_t absolute[MAX_PATH + 16];
    wchar_t config_path[MAX_PATH];
    wchar_t loader_path[MAX_PATH];
    wchar_t source_path[MAX_PATH];
    FILE *file;
    mz_zip_archive archive = { 0 };
    static const char ini[] = "[mod]\nbase=模组.zip\nselected=模组.zip:/def\n";

    EXPECT_NULL(zip_package_root_separator(L""));
    EXPECT_NULL(zip_package_root_separator(L"C:\\mods\\abc.zip"));
    EXPECT_NULL(zip_package_root_separator(L"\\\\?\\C:\\mods\\abc.zip"));
    EXPECT_NULL(zip_package_root_separator(L"//?/C:/mods/abc.zip"));
    EXPECT_STREQ_W(zip_package_root_separator(L"//?/C:/mods/abc.zip:/def"), L":/def");
    EXPECT_STREQ_W(zip_package_root_separator(L"C:\\mods\\abc.zip:/def"), L":/def");
    EXPECT_STREQ_W(zip_package_root_separator(L"abc.zip:def"), L":def");
    EXPECT_STREQ_W(zip_package_root_separator(L"\\\\server\\share\\abc.zip:def"), L":def");

    EXPECT_TRUE(GetTempPathW(MAX_PATH, temp) != 0);
    EXPECT_TRUE(GetTempFileNameW(temp, L"mod", 0, root) != 0);
    EXPECT_TRUE(DeleteFileW(root));
    EXPECT_TRUE(CreateDirectoryW(root, NULL));
    EXPECT_TRUE(GetModuleFileNameW(NULL, source_path, MAX_PATH) != 0);
    PathRemoveFileSpecW(source_path);
    EXPECT_TRUE(PathAppendW(source_path, L"extdll_fixture.dll"));
    _snwprintf(loader_path, MAX_PATH, L"%ls\\loader.dll", root);
    EXPECT_TRUE(CopyFileW(source_path, loader_path, TRUE));
    config_module = LoadLibraryExW(loader_path, NULL, DONT_RESOLVE_DLL_REFERENCES);
    EXPECT_NOT_NULL(config_module);
    _snwprintf(zip_path, MAX_PATH, L"%ls\\模组.zip", root);
    file = _wfopen(zip_path, L"wb");
    EXPECT_NOT_NULL(file);
    EXPECT_TRUE(mz_zip_writer_init_cfile(&archive, file, 0));
    EXPECT_TRUE(mz_zip_writer_add_mem(&archive, "parts/test.bin", "base", 4, 0));
    EXPECT_TRUE(mz_zip_writer_add_mem(&archive, "def/parts/test.bin", "selected", 8, 6));
    EXPECT_TRUE(mz_zip_writer_add_mem(&archive, "def/subdir/mod/parts/nested.bin", "nested", 6, 6));
    EXPECT_TRUE(mz_zip_writer_finalize_archive(&archive));
    EXPECT_TRUE(mz_zip_writer_end(&archive));
    EXPECT_EQ(fclose(file), 0);
    _snwprintf(config_path, MAX_PATH, L"%ls\\YAFSML.ini", root);
    file = _wfopen(config_path, L"wb");
    EXPECT_NOT_NULL(file);
    EXPECT_EQ(fwrite(ini, 1, sizeof(ini) - 1, file), sizeof(ini) - 1);
    EXPECT_EQ(fclose(file), 0);

    SetEnvironmentVariableW(L"YAFSML_CONFIG", config_path);
    config_init(config_module);
    mods_init();
    file = _wfopen(config_path, L"rb");
    EXPECT_NOT_NULL(file);
    config_load_ini(file);
    EXPECT_EQ(fclose(file), 0);
    EXPECT_EQ(mods_count(), 2);
    EXPECT_TRUE(read_matches(mods_file_search(L"parts/test.bin"), "selected"));
    EXPECT_TRUE(read_matches(mods_file_search(L"def/parts/test.bin"), "selected"));
    EXPECT_TRUE(read_matches(mods_file_route_read(vfs_virtual_to_uid(L"parts/test.bin"),
                                                 GENERIC_READ, OPEN_EXISTING), "selected"));
    _snwprintf(absolute, MAX_PATH + 16, L"%ls:def", zip_path);
    mods_add("absolute", absolute);
    EXPECT_EQ(mods_count(), 3);
    EXPECT_TRUE(read_matches(mods_file_search(L"parts/test.bin"), "selected"));
    mods_add("empty", L"");
    mods_add("bad", L"missing.zip:def");
    EXPECT_EQ(mods_count(), 3);
    mods_uninit();

#ifndef ML_STRIP_MODENGINE_CONFIG
    mods_init();
    file = tmpfile();
    EXPECT_NOT_NULL(file);
    EXPECT_TRUE(fputs("[extension.mod_loader]\nenabled = true\n"
                      "mods = [ { enabled = true, name = 'zip', path = '模组.zip:def' } ]\n", file) >= 0);
    rewind(file);
    EXPECT_TRUE(config_load_toml(file));
    EXPECT_EQ(fclose(file), 0);
    EXPECT_EQ(mods_count(), 1);
    EXPECT_TRUE(read_matches(mods_file_search(L"data0:/parts/test.bin"), "selected"));
    EXPECT_NULL(mods_file_search(L"def/parts/test.bin"));
    mods_uninit();
#endif

    EXPECT_EQ(test_nested_config_paths(), 0);
    EXPECT_EQ(test_config_resolution(root, config_path), 0);

    config_uninit();
    SetEnvironmentVariableW(L"YAFSML_CONFIG", NULL);
    EXPECT_TRUE(FreeLibrary(config_module));
    EXPECT_TRUE(DeleteFileW(loader_path));
    EXPECT_TRUE(DeleteFileW(config_path));
    EXPECT_TRUE(DeleteFileW(zip_path));
    EXPECT_TRUE(RemoveDirectoryW(root));
    printf("smoke_mod_zip_config: all tests passed\n");
    return 0;
}
