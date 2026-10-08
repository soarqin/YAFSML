#include <stdio.h>
#include <stdlib.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "test_common.h"
#include "game/game.h"
#include "launcher/launch_config.h"

static int test_shipped_config(const char *name) {
    char source_path[MAX_PATH];
    wchar_t template_path[MAX_PATH];
    wchar_t temp_dir[MAX_PATH];
    wchar_t config_path[MAX_PATH];
    const ml_game_descriptor_t *game = NULL;
    wchar_t invalid_key[64];
    FILE *file;
    long length;
    char *contents;
    char *value;
    int path_length;
    const size_t old_length = strlen("eldenring");
    const size_t new_length = strlen("nightreign");

    path_length = snprintf(source_path, sizeof(source_path), "%s/src/%s",
                           ML_TEST_SOURCE_ROOT, name);
    EXPECT_TRUE(path_length > 0 && path_length < (int)sizeof(source_path));
    EXPECT_TRUE(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, source_path, -1,
                                    template_path, MAX_PATH) != 0);
    file = _wfopen(template_path, L"rb");
    EXPECT_NOT_NULL(file);
    EXPECT_EQ(fseek(file, 0, SEEK_END), 0);
    length = ftell(file);
    EXPECT_TRUE(length > 0);
    rewind(file);
    contents = malloc((size_t)length + new_length - old_length + 1);
    EXPECT_NOT_NULL(contents);
    EXPECT_EQ(fread(contents, 1, (size_t)length, file), (size_t)length);
    EXPECT_EQ(fclose(file), 0);
    contents[length] = '\0';

    value = strstr(contents, "game=eldenring");
    EXPECT_NOT_NULL(value);
    value += strlen("game=");
    memmove(value + new_length, value + old_length, strlen(value + old_length) + 1);
    memcpy(value, "nightreign", new_length);
    EXPECT_EQ(ml_launcher_game_from_ini_string(contents, &game, invalid_key, 64),
              ML_LAUNCHER_GAME_CONFIG_FOUND);
    EXPECT_NOT_NULL(game);
    EXPECT_EQ(game->id, ML_GAME_NIGHTREIGN);
    EXPECT_EQ(ml_launcher_select_game(NULL, game)->id, ML_GAME_NIGHTREIGN);

    EXPECT_TRUE(GetTempPathW(MAX_PATH, temp_dir) != 0);
    EXPECT_TRUE(GetTempFileNameW(temp_dir, L"yaf", 0, config_path) != 0);
    file = _wfopen(config_path, L"wb");
    EXPECT_NOT_NULL(file);
    EXPECT_EQ(fwrite(contents, 1, strlen(contents), file), strlen(contents));
    EXPECT_EQ(fclose(file), 0);
    free(contents);
    EXPECT_EQ(ml_launcher_game_from_ini_file(config_path, &game, invalid_key, 64),
              ML_LAUNCHER_GAME_CONFIG_FOUND);
    EXPECT_NOT_NULL(game);
    EXPECT_EQ(game->id, ML_GAME_NIGHTREIGN);
    EXPECT_EQ(ml_launcher_select_game(NULL, game)->id, ML_GAME_NIGHTREIGN);
    EXPECT_TRUE(DeleteFileW(config_path));
    return 0;
}

static int test_long_comments(void) {
    static const char *prefixes[] = {
        ";", "#", " \t;", "\xEF\xBB\xBF;",
        "game=nightreign ;",
    };
    static const char *suffixes[] = {
        "\ngame=nightreign\n[patch]\nenable_ime=0\n",
        "\r\ngame=nightreign\r\n[patch]\r\nenable_ime=0\r\n",
        "\n[patch]\nenable_ime=0\n",
    };
    const size_t comment_length = 1024 * 1024;
    char *contents = malloc(comment_length + 128);
    const ml_game_descriptor_t *game = NULL;
    wchar_t invalid_key[64];
    wchar_t temp_dir[MAX_PATH];
    wchar_t config_path[MAX_PATH];
    FILE *file;

    EXPECT_NOT_NULL(contents);
    EXPECT_TRUE(GetTempPathW(MAX_PATH, temp_dir) != 0);
    EXPECT_TRUE(GetTempFileNameW(temp_dir, L"yaf", 0, config_path) != 0);
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        size_t prefix_length = strlen(prefixes[i]);
        const char *suffix = i == 4 ? suffixes[2] : suffixes[i % 2];
        size_t length = prefix_length + comment_length + strlen(suffix);
        memcpy(contents, prefixes[i], prefix_length);
        memset(contents + prefix_length, 'x', comment_length);
        /* Text inside a comment must never become a separate configuration key. */
        memcpy(contents + prefix_length + 8192, "game=eldenring", strlen("game=eldenring"));
        memcpy(contents + prefix_length + comment_length, suffix, strlen(suffix) + 1);
        EXPECT_EQ(ml_launcher_game_from_ini_string(contents, &game, invalid_key, 64),
                  ML_LAUNCHER_GAME_CONFIG_FOUND);
        EXPECT_NOT_NULL(game);
        EXPECT_EQ(ml_launcher_select_game(NULL, game)->id, ML_GAME_NIGHTREIGN);
        file = _wfopen(config_path, L"wb");
        EXPECT_NOT_NULL(file);
        EXPECT_EQ(fwrite(contents, 1, length, file), length);
        EXPECT_EQ(fclose(file), 0);
        EXPECT_EQ(ml_launcher_game_from_ini_file(config_path, &game, invalid_key, 64),
                  ML_LAUNCHER_GAME_CONFIG_FOUND);
        EXPECT_NOT_NULL(game);
        EXPECT_EQ(ml_launcher_select_game(NULL, game)->id, ML_GAME_NIGHTREIGN);
    }
    /* A long final comment without a newline must also preserve the target. */
    memcpy(contents, "game=nightreign\n;", strlen("game=nightreign\n;"));
    memset(contents + strlen("game=nightreign\n;"), 'x', comment_length);
    contents[strlen("game=nightreign\n;") + comment_length] = '\0';
    EXPECT_EQ(ml_launcher_game_from_ini_string(contents, &game, invalid_key, 64),
              ML_LAUNCHER_GAME_CONFIG_FOUND);
    EXPECT_NOT_NULL(game);
    EXPECT_EQ(ml_launcher_select_game(NULL, game)->id, ML_GAME_NIGHTREIGN);
    free(contents);
    EXPECT_TRUE(DeleteFileW(config_path));
    return 0;
}

int main(void) {
    const ml_game_descriptor_t *game = NULL;
    wchar_t invalid_key[64];

    EXPECT_EQ(test_shipped_config("YAFSML.ini"), 0);
    EXPECT_EQ(test_shipped_config("YAFSML.zhCN.ini"), 0);
    EXPECT_EQ(test_long_comments(), 0);

    /* Keep the long UTF-8 comment from affected releases to cover existing INIs. */
    EXPECT_EQ(ml_launcher_game_from_ini_string(
                  "game=nightreign\n"
                  "; 在受支持的游戏中保持 IME 启用。Elden Ring、Armored Core VI、Nightreign 和 Dark Souls III 还允许软件键盘输入非拉丁文字；Sekiro 仅保持 IME 启用。修改后需重启游戏。\n"
                  "[patch]\nenable_ime=0\n", &game, invalid_key, 64),
              ML_LAUNCHER_GAME_CONFIG_FOUND);
    EXPECT_NOT_NULL(game);
    EXPECT_EQ(game->id, ML_GAME_NIGHTREIGN);
    EXPECT_EQ(ml_launcher_select_game(NULL, game)->id, ML_GAME_NIGHTREIGN);

    EXPECT_EQ(ml_launcher_game_from_ini_string("game=sekiro\n[sekiro]\nskip_intro=1\n",
                                               &game, invalid_key, 64),
              ML_LAUNCHER_GAME_CONFIG_FOUND);
    EXPECT_EQ(game->id, ML_GAME_SEKIRO);

    EXPECT_EQ(ml_launcher_game_from_ini_string("game=nightrein\n", &game, invalid_key, 64),
              ML_LAUNCHER_GAME_CONFIG_FOUND);
    EXPECT_EQ(game->id, ML_GAME_NIGHTREIGN);

    EXPECT_EQ(ml_launcher_game_from_ini_string("game=armoredcore6\n", &game,
                                                invalid_key, 64),
              ML_LAUNCHER_GAME_CONFIG_FOUND);
    EXPECT_EQ(game->id, ML_GAME_ARMORED_CORE_6);

    EXPECT_EQ(ml_launcher_game_from_ini_string("game=ac6\n", &game,
                                                invalid_key, 64),
              ML_LAUNCHER_GAME_CONFIG_FOUND);
    EXPECT_EQ(game->id, ML_GAME_ARMORED_CORE_6);

    game = NULL;
    EXPECT_EQ(ml_launcher_game_from_ini_string("[elden_ring]\ngame=darksouls3\n",
                                               &game, invalid_key, 64),
              ML_LAUNCHER_GAME_CONFIG_NOT_SPECIFIED);
    EXPECT_NULL(game);

    EXPECT_EQ(ml_launcher_game_from_ini_string("game=DS3\n", &game, invalid_key, 64),
              ML_LAUNCHER_GAME_CONFIG_FOUND);
    EXPECT_EQ(game->id, ML_GAME_DARK_SOULS_3);

    EXPECT_EQ(ml_launcher_game_from_ini_string("game=not-a-game\n", &game, invalid_key, 64),
              ML_LAUNCHER_GAME_CONFIG_INVALID);
    EXPECT_STREQ_W(invalid_key, L"not-a-game");

    EXPECT_EQ(ml_launcher_game_from_ini_string("[broken\n", &game, invalid_key, 64),
              ML_LAUNCHER_GAME_CONFIG_ERROR);
    EXPECT_NULL(game);

    EXPECT_EQ(ml_launcher_game_from_ini_string("game=nightreign\n[broken\n",
                                               &game, invalid_key, 64),
              ML_LAUNCHER_GAME_CONFIG_ERROR);
    EXPECT_NULL(game);

    {
        wchar_t root[MAX_PATH];
        wchar_t config_path[MAX_PATH];
        EXPECT_TRUE(GetTempPathW(MAX_PATH, root) != 0);
        EXPECT_TRUE(GetTempFileNameW(root, L"yaf", 0, config_path) != 0);
        EXPECT_TRUE(DeleteFileW(config_path));
        EXPECT_TRUE(CreateDirectoryW(config_path, NULL));
        EXPECT_TRUE(ml_launcher_resolve_config_path(config_path, root, NULL));
        EXPECT_TRUE(wcsstr(config_path, L"YAFSML.ini") != NULL);
        RemoveDirectoryW(config_path);
    }

    EXPECT_EQ(ml_launcher_select_game(ml_game_by_id(ML_GAME_SEKIRO),
                                      ml_game_by_id(ML_GAME_DARK_SOULS_3))->id,
              ML_GAME_SEKIRO);
    EXPECT_EQ(ml_launcher_select_game(NULL, ml_game_by_id(ML_GAME_DARK_SOULS_3))->id,
              ML_GAME_DARK_SOULS_3);
    EXPECT_EQ(ml_launcher_select_game(NULL, NULL)->id, ML_GAME_ELDEN_RING);

    printf("smoke_launcher_game: all tests passed\n");
    return 0;
}
