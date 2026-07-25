#include <stdio.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "test_common.h"
#include "game/game.h"
#include "launcher/launch_config.h"

int main(void) {
    const ml_game_descriptor_t *game = NULL;
    wchar_t invalid_key[64];

    EXPECT_EQ(ml_launcher_game_from_ini_string("game=sekiro\n[sekiro]\nskip_intro=1\n",
                                               &game, invalid_key, 64),
              ML_LAUNCHER_GAME_CONFIG_FOUND);
    EXPECT_EQ(game->id, ML_GAME_SEKIRO);

    EXPECT_EQ(ml_launcher_game_from_ini_string("game=nightrein\n", &game, invalid_key, 64),
              ML_LAUNCHER_GAME_CONFIG_FOUND);
    EXPECT_EQ(game->id, ML_GAME_NIGHTREIGN);

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
