#pragma once

#include "game/game.h"

#include <stdbool.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Resolves the save root (%APPDATA%\<game save dir>) and clears any registered
 * extension mappings. Call before ml_save_mapping_add_extension. */
bool ml_save_mapping_init_root(const ml_game_descriptor_t *game);

/* Registers an extension (e.g. L".sl2") -> override-name mapping. The root must
 * already be initialized. `override_name` may be a full name or a ".suffix"
 * form that reuses the source file's stem. */
bool ml_save_mapping_add_extension(const wchar_t *extension, const wchar_t *override_name);

/* Convenience for the common single-extension (.sl2) case: init root + register
 * ".sl2" -> override_name in one call. */
bool ml_save_mapping_init(const ml_game_descriptor_t *game, const wchar_t *override_name);

bool ml_save_mapping_route(const wchar_t *path, const wchar_t **mapped_path);
void ml_save_mapping_uninit(void);

#ifdef __cplusplus
}
#endif
