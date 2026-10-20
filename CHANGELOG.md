# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- `[mod]` entries and ModEngine2 TOML `mods` paths can name ZIP files.
  `abc.zip:def` uses `def` inside the archive as the mod root. Stored and
  Deflate entries, including ZIP64, use bounded miniz streams for forward-only
  BND4/EBL overrides. Seekable file handles share one copy of each entry,
  decoded on first read and kept for later opens: in memory for files up to
  32 MiB within a 128 MiB budget, otherwise in one temporary file per entry.
  Unused copies are evicted least recently used first when memory is needed or
  unused temporary files exceed 1 GiB; temporary files are deleted on process
  exit. The ZIP decoder no longer seeks or replays compressed data.
  Win32 reads, seeks, size queries, duplicate handles, and overlapped completion
  notifications use the selected backend. ZIP mods remain read-only; memory mapping is
  unsupported.

### Changed

- Configuration, external DLL, and mod disk paths now resolve relative to the
  loader DLL directory, including ModEngine2 TOML paths. Moving a configuration
  no longer changes the DLL/mod, log, or BootBoost cache base directory.
  Drive-relative paths such as `C:mod`, including launcher arguments, are now
  rejected instead of using that drive's current directory.
- Updated the dearxan C11 implementation to v0.5.7 and reviewed me3 through
  `5f4fa47`, retaining the documented performance fork's cache behavior.
- All five games now share one descriptor-driven adapter. After
  `SteamAPI_Init`, every game advances to `AFTER_RUNTIME_INIT` before the asset
  hooks are installed, as in me3; Elden Ring and Sekiro used to install the
  asset hooks first.
- Dark Souls III and Sekiro, which use FMOD audio, no longer try to install the
  Wwise resolver hook.
- Dark Souls III regulation save-write protection now activates when a mod
  overrides `Data0.bdt`, the file that holds its regulation, instead of
  `regulation.bin`.
- ModEngine2 TOML `mods` entries now need only `path`; as in ModEngine2,
  `enabled` defaults to true and `name` to `Unknown`. Entries without `enabled`
  or `name` used to be ignored. A malformed TOML file is now reported instead of
  being read again as INI.
- A ModEngine extension whose `modengine_ext_init` returns false is now
  reported as a warning and no longer stops the external DLLs after it, as in
  me3.
- A `[dll]` dependency cycle still keeps the configured order, but no longer
  turns off promotion to early loading and moves onto the background thread for
  the other entries. A dependency on a missing DLL is now also reported when
  only one DLL is configured.
- A proxy DLL now loads only the system DLL it replaces instead of all three of
  `dxgi.dll`, `dinput8.dll` and `winhttp.dll`.
- Launcher options:
  - `-d` is resolved relative to the launcher and must name an existing file.
    A missing DLL used to fall back silently to `YAFSML.dll`.
  - Without `--launch-target`, a recognized game executable given by `-p`
    selects its game, overriding `game=`. With `--launch-target`, the
    executable of another game is an error.
  - A relative `-p` is resolved against the current directory first, then the
    launcher directory. A `-p` that does not lead to the game is now an error;
    the launcher used to warn and start the game it found through Steam.
  - The launcher reads `game=` from the same configuration as the DLL. `-c`,
    or an inherited `YAFSML_CONFIG` without `-c`, is resolved relative to the
    directory of the loader DLL, as the DLL does, and reaches the DLL as an
    absolute path; without either, the launcher reads the `YAFSML.ini` next to
    the DLL. With `-d` naming a DLL in another directory, the launcher used to
    read the `YAFSML.ini` next to itself and the DLL its own.
- Steam game discovery reads `libraryfolders.vdf` and the app manifests as the
  Steam client does:
  - The VDF reader was rewritten after the Steam client's KeyValues parser. It
    accepts unquoted tokens and comments, skips `#include` lines, decodes
    Steam's escape sequences, evaluates conditionals such as `[$WIN64]` as the
    client does on 64-bit Windows, returns integer values as Steam formats
    them, and compares key names without regard to case. Text that Steam
    rejects, such as an unterminated block or stray text after the last entry,
    is rejected as a whole.
  - Only the first top-level entry is used, whatever its name, but later
    entries must still parse. A later entry used to replace the first one.
  - `config\libraryfolders.vdf` is read when `steamapps\libraryfolders.vdf` is
    missing, empty or malformed.
  - Steam's own folder stands in for the `"0"` library entry, and every other
    library block needs an absolute `path`. Library entries in the older string
    form, accepted since 0.10.4, are ignored, as the current Steam client
    ignores them.
  - An app manifest counts only when its `AppID` matches and its `Universe` is
    the public universe.
- Release builds now link the dynamic CRT by default, as the published
  packages already did (`RELEASE_USE_STATIC_CRT` defaults to OFF).
- Log calls below the configured `log_level` no longer format their message or
  evaluate their arguments. Console logging writes Unicode, so non-Latin paths
  display correctly.
- Signature scans with many wildcards are 5 to 20 times faster, and dearxan
  stub analysis of Elden Ring dropped from about 3.8 s to 0.2 s with identical
  results.
- Release archives are now named `YAFSML-X.Y.Z.zip` and `YAFSML-X.Y.Z.7z` and
  are built by the same `dist` target as local packages.
  `THIRD_PARTY_NOTICES.md` now reproduces the license of every bundled
  component.

### Fixed

- Support disk paths longer than `MAX_PATH` for configuration files, external
  DLLs, loose-file mods, ZIP packages, and loader DLL selection/injection.
  Normalize mixed separators before resolving `.` and `..`, use the same root
  for DLL and mod paths, and report resolved DLL paths and Windows error codes.
- MSVC entry-point detection now accepts debugger traps such as INT3 and UD2
  at the first instruction while still validating the remaining CRT prologue.
- The launcher now temporarily clears Wine's Unix `LC_ALL` when spawning the
  game, restoring it afterward so non-ANSI mod paths can load under Proton.
- DLL injection and remote initialization now also wait for game-process exit,
  abandoning initialization when the game closes before it completes.
- Launcher:
  - Errors were not shown anywhere. They now appear in a message box and on
    standard error, or in the console of the command prompt that started the
    launcher. Failures to start or inject the game include the system error
    text, and error 740 explains how to remove the elevation requirement.
  - A game folder that also holds a YAFSML proxy DLL (`dinput8.dll`, `dxgi.dll`
    or `winhttp.dll` exporting `YAFSMLInit`) made the game crash. The launcher
    now reports the proxy and does not start the game.
  - The game now receives its quoted path as the command line, so a path with
    spaces is no longer split into several arguments.
  - Injection no longer reports a loader DLL loaded at a 4 GiB-aligned address
    as failed, finds the loaded DLL by its full path instead of its file name,
    and no longer stops after 256 modules.
  - Steam VDF parsing no longer corrupts the heap on unquoted values inside
    nested blocks, and reads an escaped backslash before a closing quote and
    braces inside quoted values correctly.
- Fixed possible hangs when the game exits. Process exit no longer runs the
  loader's teardown, which could wait on locks held by threads the system had
  already ended. The system reclaims streams and other resources at process
  exit. Unloading the DLL while the game runs still tears everything down.
- Save mapping (`replace_save_filename` and
  `replace_seamless_coop_save_filename`):
  - A save reached through a symbolic link or junction inside the save folder
    is now denied, with one warning, instead of opening the original save
    through the link. Other reparse points, such as cloud-storage placeholders,
    no longer prevent mapping.
  - A leading-dot replacement such as `.mod.sl2` is no longer applied twice when
    Windows routes an already redirected name again, for example `CopyFileW`
    calling `CreateFileW`.
  - A replacement that cannot be set up, including an invalid name or an
    unresolved save folder, now denies access to those saves instead of using
    the original files.
  - Fixed an out-of-bounds read when a path was shorter than the save folder.
- Regulation save-write protection on Sekiro and Dark Souls III again requires
  the `test al, al; je` after the writer call, as in me3, so it no longer hooks
  the save-regulation reader or Dark Souls III stats requests. On Elden Ring,
  Armored Core VI and Nightreign, the regulation step no longer searches for
  `CSRegulationManager` on every frame.
- Asset hooks:
  - The disk `open_file` and `DlFileOperator::SetPath` hooks looked up their
    original function through the caller's vtable, so calls from another device
    or file operator class failed. Each hook now keeps its own original.
  - BootBoost no longer reports a failed mount to the game after the full mount
    succeeded without new mounts to cache, and measures the RSA key by its
    string length, as in me3.
  - A regular `MountEbl` call no longer extracts and pushes back the new mounts.
  - Override lookups no longer run while the game's device-manager lock is held,
    and a path is replaced only when its override file is available.
  - MountEbl detection prefers the call-site signature, as in me3.
- The `winhttp.dll` proxy now forwards the ten WinHTTP exports it was missing,
  so programs that import them can start.
- `YAFSML_CONFIG` now resolves a relative path against the directory of
  `YAFSML.dll`, accepts forward slashes, and selects `YAFSML.ini` or the
  ModEngine2 TOML when it names a directory.
- A directory mod whose scan fails part-way is no longer added partially, and
  mod paths written with forward slashes, such as `/mods/a`, resolve like
  backslash paths.
- The dedicated heap now also falls back to the global mimalloc heap when it
  cannot satisfy a reallocation, as allocation already did.
- The runtime-ready trigger no longer deadlocks when an external DLL calls
  `SteamAPI_Init` while the after-runtime work runs.
- RTTI lookups no longer lose a vtable when a class has several and one with a
  lower offset is found later.
- FD4 singleton discovery now checks that the reflection object lies in `.data`.
- The heap patch checks that the allocator table lies in one writable section,
  and Sekiro's debug allocator getter is chosen by call frequency, as in me3.
- dearxan:
  - Under SteamStub, the security cookie is read from the load configuration
    when `.text` is still encrypted.
  - The RMX probe accepts the register form of `imul`, `syscall` is analyzed as
    a call, interrupts and transactions end an execution path, and `pushfq` and
    `popfq` keep the emulated stack consistent, as upstream does.
  - Analysis stays on the calling thread while other threads are suspended, so
    a worker cannot wait on a lock held by a suspended thread.
  - Reading a truncated executable no longer loops forever.
- The DLL's version resource now reports a DLL file type.

## [0.10.4] - 2026-10-02

### Changed

- Updated mimalloc to 3.5.3, retaining page-map lookups for dedicated arenas
  at arbitrary addresses.
- Set `patch_mem_dedicated_heap=1` in both shipped INI files to match the
  loader's enabled default.

### Fixed

- Fixed Steam game discovery skipping installed games omitted from the
  cached library index, including ELDEN RING. The launcher now checks each
  library's app manifest directly.
- Fixed Steam game discovery reporting success without a valid installation
  directory. The default library remains searchable when the library index
  is missing or invalid.
- Fixed parsing Steam VDF files with Windows line endings and added support
  for older Steam library-folder formats.

## [0.10.3] - 2026-09-12

### Changed

- Updated mimalloc to 3.5.1.
- Updated the dearxan C11 implementation to v0.5.6.
- Enabled `patch_mem_dedicated_heap` by default. The allocator uses the
  mimalloc page map so the dedicated arena remains compatible with arbitrary
  64 MiB arena addresses.

## [0.10.2] - 2026-08-14

### Changed

- Updated mimalloc to 3.4.5.
- Replaced process-affinity mask handling with Windows CPU Sets, preserving
  strategies 1 through 4 across heterogeneous cores and processor groups.
- Dedicated mimalloc arenas now reserve memory from the highest available
  addresses, matching the dedicated-heap behavior in me3 commit `da9abcf`.
  Developers can expose an arena
  through `YAFSML_HEAP_MAPPING_NAME` or `YAFSML_HEAP_MAPPING_FILE`; mapping
  failures retain the existing non-fatal allocator fallback behavior.

### Fixed

- Dedicated mimalloc heap allocation now retries through the global mimalloc
  heap when the dedicated heap cannot satisfy an allocation, matching me3
  commit `15a19de`.

## [0.10.1] - 2026-08-02

### Changed

- Added stable Armored Core VI support, including launcher and process detection,
  Steam App ID `1888160`, `%APPDATA%\ArmoredCore6` save mapping, FD4 host
  capabilities, Dantelion asset and Wwise routing, and aliases `ac6` and
  `armoredcore6`. The adapter follows the shared AC6 behavior in me3 commit
  `6563ebb`; the mimalloc allocator replacement remains unsupported.
- Regulation save-write protection now activates only when
  `prevent_regulation_save_write` is enabled and a mod overrides the root
  `regulation.bin`. Games keep their normal regulation save behavior when the
  vanilla file is in use.
- `enable_ime` now preserves IME availability in every supported game so
  injected tools can use it. Elden Ring, Armored Core VI, Nightreign, and Dark
  Souls III also bypass the software-keyboard filter that replaced non-Latin
  characters with asterisks; Sekiro does not contain this character-input path
  and skips that patch.

### Fixed

- Fixed GCC builds failing because the force-inline helper macro did not give
  header-defined functions internal linkage.
- Fixed Elden Ring and Nightreign regulation protection releasing a
  game-owned buffer while a queued save job could still read it, which could
  produce corrupted Slot 11 data. The protection now suppresses future
  regulation writes without invalidating the buffer used by existing jobs.
- Fixed the dearxan scheduler smoke test assuming one exact ordering between
  queued and reentrant callbacks. It now verifies the ordering guarantees of
  each callback class without depending on thread timing, including in Release
  builds.

## [0.10.0] - 2026-07-29

### Added

- Added a `data_ready` condition for `[dll]` entries. Such a DLL loads only once
  the game has finished reading every param, which is what mods that patch or read
  param data need. It cannot be combined with `early` or `delay`, and anything
  listed `after` a `data_ready` DLL waits for the same point. If a game update
  breaks detection of that point, the loader reports an error and skips those DLLs
  rather than starting them while params may still be loading.

### Changed

- The mod loader now distinguishes "the game can render" from "the game has read
  its params". CPU affinity still applies at the earlier of the two; only the
  reported lifecycle name changed, from `AFTER_GAME_DATA_READY` to
  `AFTER_RENDER_READY`.
- Sekiro now uses the same title-flow trigger as Dark Souls III instead of firing
  right after the game mounted its archives, so deferred CPU affinity happens at a
  comparable point across all four games.
- Big mods load much faster. Every file the game opened used to be checked against
  the mod's entire file list, so the more files a mod had, the slower loading got.
- Memory no longer grows the longer you play. The mod loader was remembering every
  file path the game had ever opened, even paths that had nothing to do with mods.
- Loading is faster when a renamed save file is set up. Files that cannot possibly
  be a save are no longer checked against the save folder.
- Replaced sounds stream more smoothly, especially in mods that replace a lot of
  audio.
- The game starts up faster. Locating the code the mod loader needs to hook takes
  about half as long, and Arxan neutralization (`disable_arxan`) is considerably
  quicker.
- Waiting for Arxan neutralization to finish no longer keeps a CPU core busy.

### Fixed

- Fixed Sekiro 1.06 regulation save-write protection failing to locate its
  writer because the game consumes the writer's return value differently from
  the older me3 call-site signature.
- Fixed a crash at startup when the log file could not be set up.
- A mod path the mod loader cannot read is now reported and skipped, instead of
  being used as garbage.
- Fixed a small resource leak at startup.

## [0.9.0] - 2026-07-27

### Added

- Added a C11 static-library port of dearxan v0.5.3, including SteamStub 3.1 handling, Arxan analysis and patching, process-wide scheduling, and the v0.5.3 proxy-DLL entry detection behavior.
- Added `disable_arxan`; Dark Souls III always enables Arxan neutralization.
- Added game-data-ready lifecycle triggers for Elden Ring, Elden Ring Nightreign, Sekiro, and Dark Souls III.
- Added asynchronous CPU affinity application after game data initialization, with processor-topology validation and unload-time worker synchronization.

### Changed

- Matched me3 commit `6563ebb` before-main scheduling: early external DLLs now load after the Arxan entry stub or MSVC security-cookie initialization, even when Arxan neutralization is disabled.
- Deferred CPU affinity until each game's verified data-ready step instead of applying it during early process initialization.

### Fixed

- Matched me3's ModEngine compatibility initializer ABI without dereferencing the returned extension object, and prevented duplicate external DLL initialization.

## [0.8.0] - 2026-07-25

### Added

- Added stable Elden Ring Nightreign support, including launcher and process detection, Steam App ID `2622380`, `%APPDATA%\Nightreign` save mapping, FD4 host capabilities, Dantelion asset and Wwise routing, and me3-compatible aliases `nr` and `nightrein`.

### Changed

- Skip the Dantelion mimalloc replacement for Nightreign, matching me3 commit `6563ebb` where the memory patch is unsupported for this game.

### Fixed

- Run before-main initialization during remote suspended-process injection so allocator patching and early external DLL loading are applied before the game starts.

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

[Unreleased]: https://github.com/soarqin/YAFSML/compare/v0.10.0...HEAD
[0.10.0]: https://github.com/soarqin/YAFSML/compare/v0.9.0...v0.10.0
[0.9.0]: https://github.com/soarqin/YAFSML/compare/v0.8.0...v0.9.0
[0.8.0]: https://github.com/soarqin/YAFSML/compare/v0.7.4...v0.8.0
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
