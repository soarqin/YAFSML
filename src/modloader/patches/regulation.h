#pragma once

#include "game/game.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Installs the save-write protection; gamehook schedules it only when
   ml_regulation_requested is true and logs the skipped cases itself. */
bool ml_regulation_install(const ml_game_descriptor_t *game);
/* Whether a mod overrides the game's regulation file (game->regulation_file). */
bool ml_regulation_override_present(const ml_game_descriptor_t *game);
bool ml_regulation_requested(const ml_game_descriptor_t *game);

#ifdef ML_REGULATION_TEST
void ml_regulation_test_suppress_fd4_save(void *manager);
bool ml_regulation_test_skip_write(void *state);
#endif

#ifdef __cplusplus
}
#endif
