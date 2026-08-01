#pragma once

#include "game/game.h"

#include <stdbool.h>

bool ml_regulation_install(const ml_game_descriptor_t *game);
bool ml_regulation_override_present(void);
bool ml_regulation_requested(void);

#ifdef ML_REGULATION_TEST
void ml_regulation_test_suppress_fd4_save(void *manager);
bool ml_regulation_test_skip_write(void *state);
#endif
