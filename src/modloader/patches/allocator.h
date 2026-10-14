#pragma once

#include "game/game.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

bool ml_allocator_install_before_main(const ml_game_descriptor_t *game);
bool ml_allocator_install_after_runtime(const ml_game_descriptor_t *game);

#ifdef __cplusplus
}
#endif
