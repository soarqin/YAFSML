# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- Added stable Elden Ring Nightreign support, including launcher and process detection, Steam App ID `2622380`, `%APPDATA%\Nightreign` save mapping, FD4 host capabilities, Dantelion asset and Wwise routing, and me3-compatible aliases `nr` and `nightrein`.

### Changed

- Skip the Dantelion mimalloc replacement for Nightreign, matching me3 commit `6563ebb` where the memory patch is unsupported for this game.

## [0.7.4] - 2026-07-24

### Changed

- Replaced the Elden Ring-specific launcher icon with a multi-game YAFSML mark and added its editable SVG source.
- Synchronized BootBoost mount handling with me3 commit `6563ebb`.

### Fixed

- Release the Dantelion device-manager lock before invoking the game's mount routine, then reacquire it for mount extraction to avoid lock re-entry during BootBoost and fallback mounts.
- Preserve variadic log arguments when writing the same message to both the console and the log file.
- Fixed smoke-test linkage after the logging and allocator API changes.

## [0.7.3] - 2026-07-20

### Changed

- Added a versioned BootBoost cache format with source-key, size, and payload checksum validation. Existing cache files are invalidated and regenerated automatically.
- Write BootBoost cache files through per-process, per-thread temporary files before replacing the active cache.

### Fixed

- Fixed game startup crashes after BootBoost generated a cache by matching me3's BHD holder ownership behavior and deferring the cache-buffer assignment until the mount is retained or restored successfully.
- Fixed log initialization when the log directory does not exist.
- Fixed log output level check and config loading order.
- Reject and remove BootBoost cache files that are truncated, contain trailing data, belong to another source BHD, fail decompression or checksum validation, or contain inconsistent BHD metadata.

## [0.7.2] - 2026-07-19

### Changed

- Made the dedicated mimalloc heap used by `patch_mem` optional and disabled by default. Set `patch_mem_dedicated_heap=1` to enable it; otherwise the Dantelion allocator uses mimalloc's regular allocation functions.

## [0.7.1] - 2026-07-19

### Changed

- Updated mimalloc to 3.4.1.
- Reduced the preallocated memory used by the Dark Souls III and Sekiro adapters.

### Fixed

- Load delayed external DLLs asynchronously so they do not block game startup or unrelated DLLs.

## [0.7.0] - 2026-07-19

### Added

- Added top-level `game=...` launcher configuration. An explicit `--launch-target` overrides it; absent configuration defaults to Elden Ring.
- Added launcher and adapter coverage for Sekiro and Dark Souls III, plus game-target selection through `--launch-target`; Sekiro is stable and Dark Souls III remains experimental.
- Added the frozen VFS index, domain-specific lookup caches, Dantelion asset routing, BootBoost caching, explicit writable mappings, and long-path handling.

### Changed

- Renamed the project and release artifacts from YAERModLoader to YAFSML (Yet Another FromSoftware Mod Loader). This is a direct switch without compatibility aliases.
- Reorganized `YAFSML.ini` into top-level launcher selection plus `[patch]`, `[tweak]`, `[log]`, `[dll]`, and `[mod]` sections; updated English and Chinese templates and documentation. Distribution packages include both templates and both README files.
- Migrated the Elden Ring adapter onto the shared Win32 VFS hooks and save-mapping used by Sekiro and Dark Souls III. Save-file remapping now runs through the common path (including reparse-point and canonicalization guards) and the Seamless Co-op save (`.co2`) is remapped alongside the main save (`.sl2`).
- Split the asset signature and crypto parsing into a separately testable `asset_sig` unit.

### Removed

- Removed the built-in Elden Ring visual/input tweaks (`remove_chromatic_aberration`, `remove_vignette`, and `disable_mouse_camera_control`) and all project-owned extension DLLs from `src/extdlls` and release packages.
- Removed unused `filecache`, allocator-table, device-tracking, and VFS/mod routing helpers.

### Fixed

- Avoid ordinary VFS queries when no mods are loaded, and return stable cache-hit values without string allocations.
- Fixed an out-of-bounds read in the signature scanner when the remaining data was smaller than the pattern; added a `smoke_scanner` regression test.
- Fixed a data race on the FromSoftware FD4 singleton lookup table and validated the reflection pointer before use.
- Hardened Steam library parsing: read VDF input in binary mode, validate `ftell` and `fread`, NUL-terminate its buffer, reject malformed input, and decode library and install paths as UTF-8.
- Forward proxy exports (`dxgi`, `dinput8`, and `winhttp`) through naked assembly stubs so all arguments pass through unchanged regardless of build configuration.
- Fixed uninitialized reads on missing PE sections in the property hook, a GDI brush race in the window-flash hook, an unchecked process-affinity query, and PE header validation.

## [0.6.1] - 2026-07-03

### Fixed

- Fixed a possible crash.

## [0.6.0] - 2026-06-27

### Added

- Added the `almighty_kale.dll` external DLL, a 1:1 C port of the [Glorious Merchant](https://github.com/ThomasJClark/elden-ring-glorious-merchant) mod (MIT). It patches Kalé's dialogue to add free item-shop menus, unlocks all gestures, keeps Kalé alive, zeroes sell values while the shop is open, and keeps key items out of the storage box. Configure it with `almighty_kale.ini` (`auto_upgrade_weapons`).
- Extended `er_param` (API v2; existing field offsets preserved and new accessors appended) with C struct headers matching the merchant's reverse-engineered layouts and runtime pointer accessors for additional game interfaces.
- Exposed `ISteamApps::GetCurrentGameLanguage` through `steam_apps()` and `isteam_apps_get_current_game_language()`, with a `SteamAPI_SteamApps_v008` to `v007` fallback. `almighty_kale` uses it to select localized text for 14 languages, with English fallback.

### Changed

- **Breaking:** Re-anchored the extension API to V1. Moved the Elden Ring parameter interface into the standalone `er_param.dll` provider and made the main loader generic, without parameter, wide-string, or pointer coupling.
- **Breaking:** Removed `er_param_find_table` and `er_wstring_impl_str` from `modloader_ext_api_t`, and removed `on_param_initialized` from `modloader_ext_def_t`.
- Parameter consumers now obtain the API at runtime through `er_param_api_get()` and register `on_param_loaded` observers. They must list `er_param` before themselves in the `[dlls]` INI section.
- Ship `er_param.dll` under `dll/` with its own `er_param.ini`; moved `world_map_cursor_speed` there from the `[elden_ring]` section in `YAERModLoader.ini`.

### Removed

- Removed the `src/eldenring/` static library. The map archive path hook now implements the required wide-string SSO logic locally.

### Fixed

- Fixed `LocalReAlloc` shrinking the external-DLL, mod, and `er_param` observer arrays in place, which could silently drop entries after capacity growth.

## [0.5.1] - 2026-06-25

### Changed

- Updated credits: klib replaces uthash.

### Fixed

- Fixed missing audio when a mod overrides Wwise (`.wem` and `.bnk`) files by using `READ` mode instead of `READ_EBL` for override files on disk.

## [0.5.0] - 2026-05-13

### Added

- Added the `disable_mouse_camera_control` option.
- Added the `no_dup_loot.dll` external DLL to prevent duplicate loot drops.
- Added CTest smoke tests.

### Changed

- Replaced uthash with klib khash for all hash-table usage.

### Fixed

- Fixed a use-after-free in `CopyFile_hooked`.
- Fixed an inverted realloc condition and NULL dereference in `itemlot_rate`.
- Fixed a memory leak and signed-character out-of-bounds access in `sig_scan` and `sig_build`.
- Fixed a race by initializing `image_base` before spawning threads.
- Fixed `config_load` dropping an absolute file path from an environment variable.
- Fixed file-cache negative caching.

## [0.4.1] - 2025-06-16

### Fixed

- Fixed a crash caused by a save filename change.

## [0.4.0] - 2025-06-06

### Added

- Added the `world_map_cursor_speed` option.
- Added the `replace_save_filename` and `replace_seamless_coop_save_filename` options.
- Added two optional external DLLs, enabled from `YAERModLoader.ini`:
  - `autoloot.dll`: auto-loot materials and player corpse runes.
  - `itemlot_rate.dll`: change item drop rates; configure it with `itemlot_rate.ini`.

## [0.3.3] - 2025-01-10

### Fixed

- Fixed the `-p` option.

## [0.3.2] - 2025-01-07

### Fixed

- Fixed mod loading paths (#2).
- Fixed a critical string-matching issue for mod file paths.

## [0.3.1] - 2025-01-01

### Fixed

- Fixed a crash.

## [0.3.0] - 2024-12-27

### Added

- Added the `reset_achievements_on_new_game` and `enable_ime` options.
- Added compatibility with ModEngine2's mod-loading mechanism, allowing Seamless Co-op to work (#1).

## [0.2.0]

### Added

- Added the `cpu_affinity`, `skip_intro`, `remove_chromatic_aberration`, and `remove_vignette` configuration entries.

### Changed

- Avoid hooking certain functions when no mods are added to improve loading performance.

## [0.1.0] - 2024-11-01

### Added

- Initial release.

[Unreleased]: https://github.com/soarqin/YAFSML/compare/v0.7.4...HEAD
[0.7.4]: https://github.com/soarqin/YAFSML/compare/v0.7.3...v0.7.4
[0.7.3]: https://github.com/soarqin/YAFSML/compare/v0.7.2...v0.7.3
[0.7.2]: https://github.com/soarqin/YAFSML/compare/v0.7.1...v0.7.2
[0.7.1]: https://github.com/soarqin/YAFSML/compare/v0.7.0...v0.7.1
[0.7.0]: https://github.com/soarqin/YAFSML/compare/v0.6.1...v0.7.0
[0.6.1]: https://github.com/soarqin/YAFSML/compare/v0.6.0...v0.6.1
[0.6.0]: https://github.com/soarqin/YAFSML/compare/v0.5.1...v0.6.0
[0.5.1]: https://github.com/soarqin/YAFSML/compare/v0.5.0...v0.5.1
[0.5.0]: https://github.com/soarqin/YAFSML/compare/v0.4.1...v0.5.0
[0.4.1]: https://github.com/soarqin/YAFSML/compare/v0.4.0...v0.4.1
[0.4.0]: https://github.com/soarqin/YAFSML/compare/v0.3.3...v0.4.0
[0.3.3]: https://github.com/soarqin/YAFSML/compare/v0.3.2...v0.3.3
[0.3.2]: https://github.com/soarqin/YAFSML/compare/v0.3.1...v0.3.2
[0.3.1]: https://github.com/soarqin/YAFSML/compare/v0.3.0...v0.3.1
[0.3.0]: https://github.com/soarqin/YAFSML/compare/v0.1.0...v0.3.0
[0.1.0]: https://github.com/soarqin/YAFSML/releases/tag/v0.1.0
