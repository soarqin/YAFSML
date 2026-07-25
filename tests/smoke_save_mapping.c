#include <stdio.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlwapi.h>

#include "test_common.h"
#include "game/game.h"
#include "modloader/patches/save_mapping.h"
#include "modloader/vfs.h"

static bool create_empty_file(const wchar_t *path) {
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) return false;
    CloseHandle(file);
    return true;
}

static bool create_file_with_byte(const wchar_t *path, BYTE value) {
    DWORD written;
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) return false;
    bool result = WriteFile(file, &value, 1, &written, NULL) && written == 1;
    CloseHandle(file);
    return result;
}

static bool file_has_byte(const wchar_t *path, BYTE expected) {
    BYTE value;
    DWORD read;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) return false;
    bool result = ReadFile(file, &value, 1, &read, NULL) && read == 1 && value == expected;
    CloseHandle(file);
    return result;
}

int main(void) {
    wchar_t old_appdata[MAX_PATH];
    wchar_t root[MAX_PATH];
    wchar_t save_root[MAX_PATH];
    wchar_t account[MAX_PATH];
    wchar_t source[MAX_PATH];
    wchar_t backup[MAX_PATH];
    wchar_t long_appdata[600];
    wchar_t long_source[700];
    wchar_t outside[MAX_PATH];
    wchar_t link_path[MAX_PATH];
    wchar_t linked_source[MAX_PATH];
    const wchar_t *mapped = NULL;
    const ml_game_descriptor_t *game = ml_game_by_id(ML_GAME_SEKIRO);
    const ml_game_descriptor_t *ds3 = ml_game_by_id(ML_GAME_DARK_SOULS_3);
    DWORD old_length = GetEnvironmentVariableW(L"APPDATA", old_appdata, MAX_PATH);

    EXPECT_NOT_NULL(game);
    EXPECT_NOT_NULL(ds3);
    EXPECT_TRUE(GetTempPathW(MAX_PATH, root) != 0);
    EXPECT_TRUE(GetTempFileNameW(root, L"sav", 0, root) != 0);
    DeleteFileW(root);
    EXPECT_TRUE(CreateDirectoryW(root, NULL));
    EXPECT_TRUE(SetEnvironmentVariableW(L"APPDATA", root));

    lstrcpyW(save_root, root); PathAppendW(save_root, L"Sekiro");
    EXPECT_TRUE(CreateDirectoryW(save_root, NULL));
    lstrcpyW(account, save_root); PathAppendW(account, L"76561198000000000");
    EXPECT_TRUE(CreateDirectoryW(account, NULL));
    lstrcpyW(source, account); PathAppendW(source, L"S0000.sl2");
    EXPECT_TRUE(create_empty_file(source));
    lstrcpyW(backup, source); lstrcatW(backup, L".bak");
    EXPECT_TRUE(create_file_with_byte(backup, 0x5a));

    vfs_init();
    EXPECT_TRUE(!ml_save_mapping_init(game, L"..\\escaped.sl2"));
    EXPECT_TRUE(!ml_save_mapping_init(game, L"abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijkl.sl2"));
    EXPECT_TRUE(!ml_save_mapping_init(game, L"NUL.sl2"));
    EXPECT_TRUE(!ml_save_mapping_init(game, L"com1.sl2"));
    EXPECT_TRUE(!ml_save_mapping_init(game, L"invalid?.sl2"));
    EXPECT_TRUE(!ml_save_mapping_init(game, L"trailing.sl2 "));
    EXPECT_TRUE(!ml_save_mapping_init(game, L"."));
    lstrcpyW(long_appdata, L"C:\\");
    for (size_t i = 3; i < 580; i++) long_appdata[i] = (i % 40 == 0) ? L'\\' : L'a';
    long_appdata[580] = L'\0';
    EXPECT_TRUE(SetEnvironmentVariableW(L"APPDATA", long_appdata));
    EXPECT_TRUE(ml_save_mapping_init(game, L"sekiro-long.sl2"));
    _snwprintf(long_source, 700, L"%ls\\Sekiro\\76561198000000000\\S0000.sl2", long_appdata);
    long_source[699] = L'\0';
    EXPECT_TRUE(ml_save_mapping_route(long_source, &mapped));
    EXPECT_NOT_NULL(mapped);
    EXPECT_TRUE(wcslen(mapped) > MAX_PATH);
    EXPECT_TRUE(wcsstr(mapped, L"sekiro-long.sl2") != NULL);
    EXPECT_TRUE(SetEnvironmentVariableW(L"APPDATA", root));
    EXPECT_TRUE(ml_save_mapping_init(game, L"sekiro-test.sl2"));
    EXPECT_TRUE(ml_save_mapping_route(source, &mapped));
    EXPECT_NOT_NULL(mapped);
    EXPECT_TRUE(wcsstr(mapped, L"sekiro-test.sl2") != NULL);
    EXPECT_TRUE(PathFileExistsW(mapped));
    EXPECT_STREQ_W(vfs_route_writable_path(source), mapped);

    mapped = NULL;
    EXPECT_TRUE(ml_save_mapping_route(backup, &mapped));
    EXPECT_NOT_NULL(mapped);
    EXPECT_TRUE(wcsstr(mapped, L"sekiro-test.sl2.bak") != NULL);
    EXPECT_TRUE(PathFileExistsW(mapped));
    EXPECT_TRUE(file_has_byte(mapped, 0x5a));

    DeleteFileW(vfs_route_writable_path(backup));
    DeleteFileW(vfs_route_writable_path(source));
    ml_save_mapping_uninit();
    vfs_uninit();
    DeleteFileW(backup);
    DeleteFileW(source);
    RemoveDirectoryW(account);
    RemoveDirectoryW(save_root);

    lstrcpyW(save_root, root); PathAppendW(save_root, L"DarkSoulsIII");
    EXPECT_TRUE(CreateDirectoryW(save_root, NULL));
    lstrcpyW(account, save_root); PathAppendW(account, L"76561198000000000");
    EXPECT_TRUE(CreateDirectoryW(account, NULL));
    lstrcpyW(source, account); PathAppendW(source, L"DS30000.sl2");
    EXPECT_TRUE(create_empty_file(source));
    vfs_init();
    EXPECT_TRUE(ml_save_mapping_init(ds3, L"ds3-test.sl2"));
    EXPECT_TRUE(ml_save_mapping_route(source, &mapped));
    EXPECT_NOT_NULL(mapped);
    EXPECT_TRUE(wcsstr(mapped, L"ds3-test.sl2") != NULL);
    EXPECT_TRUE(PathFileExistsW(mapped));

    mapped = (const wchar_t *)1;
    EXPECT_TRUE(!ml_save_mapping_route(L"C:\\outside\\S0000.sl2", &mapped));
    EXPECT_NULL(mapped);
    lstrcpyW(backup, save_root); PathAppendW(backup, L"..\\outside\\DS30000.sl2");
    mapped = (const wchar_t *)1;
    EXPECT_TRUE(!ml_save_mapping_route(backup, &mapped));
    EXPECT_NULL(mapped);

    lstrcpyW(outside, root); PathAppendW(outside, L"outside");
    EXPECT_TRUE(CreateDirectoryW(outside, NULL));
    lstrcpyW(link_path, save_root); PathAppendW(link_path, L"linked-account");
    if (CreateSymbolicLinkW(link_path, outside,
                            SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
        lstrcpyW(linked_source, link_path); PathAppendW(linked_source, L"DS30000.sl2");
        EXPECT_TRUE(create_empty_file(linked_source));
        mapped = (const wchar_t *)1;
        EXPECT_TRUE(!ml_save_mapping_route(linked_source, &mapped));
        EXPECT_NULL(mapped);
        DeleteFileW(linked_source);
        RemoveDirectoryW(link_path);
    }

    DeleteFileW(vfs_route_writable_path(source));
    ml_save_mapping_uninit();
    vfs_uninit();
    DeleteFileW(source);
    RemoveDirectoryW(account);
    RemoveDirectoryW(save_root);
    RemoveDirectoryW(outside);

    /* Elden Ring: dual-extension mapping (.sl2 main save + .co2 seamless coop). */
    {
        const ml_game_descriptor_t *er = ml_game_by_id(ML_GAME_ELDEN_RING);
        wchar_t er_root[MAX_PATH];
        wchar_t er_account[MAX_PATH];
        wchar_t er_sl2[MAX_PATH];
        wchar_t er_co2[MAX_PATH];
        EXPECT_NOT_NULL(er);
        lstrcpyW(er_root, root); PathAppendW(er_root, L"EldenRing");
        EXPECT_TRUE(CreateDirectoryW(er_root, NULL));
        lstrcpyW(er_account, er_root); PathAppendW(er_account, L"76561198000000000");
        EXPECT_TRUE(CreateDirectoryW(er_account, NULL));
        lstrcpyW(er_sl2, er_account); PathAppendW(er_sl2, L"ER0000.sl2");
        EXPECT_TRUE(create_empty_file(er_sl2));
        lstrcpyW(er_co2, er_account); PathAppendW(er_co2, L"ER0000.co2");
        EXPECT_TRUE(create_empty_file(er_co2));

        vfs_init();
        EXPECT_TRUE(ml_save_mapping_init_root(er));
        EXPECT_TRUE(ml_save_mapping_add_extension(L".sl2", L"ER-main.sl2"));
        EXPECT_TRUE(ml_save_mapping_add_extension(L".co2", L"ER-coop.co2"));
        /* invalid extension / override are rejected */
        EXPECT_TRUE(!ml_save_mapping_add_extension(L"sl2", L"noleadingdot.sl2"));
        EXPECT_TRUE(!ml_save_mapping_add_extension(L".bad", L"invalid?.bad"));

        mapped = NULL;
        EXPECT_TRUE(ml_save_mapping_route(er_sl2, &mapped));
        EXPECT_NOT_NULL(mapped);
        EXPECT_TRUE(wcsstr(mapped, L"ER-main.sl2") != NULL);
        EXPECT_TRUE(PathFileExistsW(mapped));

        mapped = NULL;
        EXPECT_TRUE(ml_save_mapping_route(er_co2, &mapped));
        EXPECT_NOT_NULL(mapped);
        EXPECT_TRUE(wcsstr(mapped, L"ER-coop.co2") != NULL);
        EXPECT_TRUE(PathFileExistsW(mapped));

        DeleteFileW(vfs_route_writable_path(er_sl2));
        DeleteFileW(vfs_route_writable_path(er_co2));
        ml_save_mapping_uninit();
        vfs_uninit();
        DeleteFileW(er_sl2);
        DeleteFileW(er_co2);
        RemoveDirectoryW(er_account);
        RemoveDirectoryW(er_root);
    }

    {
        const ml_game_descriptor_t *nightreign = ml_game_by_id(ML_GAME_NIGHTREIGN);
        wchar_t nr_root[MAX_PATH];
        wchar_t nr_account[MAX_PATH];
        wchar_t nr_sl2[MAX_PATH];
        EXPECT_NOT_NULL(nightreign);
        lstrcpyW(nr_root, root); PathAppendW(nr_root, L"Nightreign");
        EXPECT_TRUE(CreateDirectoryW(nr_root, NULL));
        lstrcpyW(nr_account, nr_root); PathAppendW(nr_account, L"76561198000000000");
        EXPECT_TRUE(CreateDirectoryW(nr_account, NULL));
        lstrcpyW(nr_sl2, nr_account); PathAppendW(nr_sl2, L"NR0000.sl2");
        EXPECT_TRUE(create_empty_file(nr_sl2));

        vfs_init();
        EXPECT_TRUE(ml_save_mapping_init(nightreign, L"nightreign-test.sl2"));
        mapped = NULL;
        EXPECT_TRUE(ml_save_mapping_route(nr_sl2, &mapped));
        EXPECT_NOT_NULL(mapped);
        EXPECT_TRUE(wcsstr(mapped, L"nightreign-test.sl2") != NULL);
        EXPECT_TRUE(PathFileExistsW(mapped));

        DeleteFileW(vfs_route_writable_path(nr_sl2));
        ml_save_mapping_uninit();
        vfs_uninit();
        DeleteFileW(nr_sl2);
        RemoveDirectoryW(nr_account);
        RemoveDirectoryW(nr_root);
    }

    RemoveDirectoryW(root);
    SetEnvironmentVariableW(L"APPDATA", old_length != 0 && old_length < MAX_PATH ? old_appdata : NULL);
    printf("smoke_save_mapping: all tests passed\n");
    return 0;
}
