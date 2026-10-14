#pragma once

#include "game/game.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Dark Souls III specifics run by the generic adapter before its hooks:
 * loose-param properties and the experimental capability summary. */
bool darksouls3_prepare(const ml_game_descriptor_t *game, bool assets_requested);

#ifdef __cplusplus
}
#endif
