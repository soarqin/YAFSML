# YAFSML

[中文说明](README.zhCN.md)

YAFSML (Yet Another FromSoftware Mod Loader) is a Windows mod loader for FromSoftware games. It provides a
standalone launcher, proxy DLL support, ordered loose-file overrides, and
external ModEngine-compatible DLL loading.

## Supported games

| Game | Launcher target | Status |
| --- | --- | --- |
| Elden Ring | `eldenring` | Stable |
| Elden Ring Nightreign | `nightreign` | Stable |
| Sekiro: Shadows Die Twice | `sekiro` | Stable |
| Dark Souls III | `darksouls3` | Experimental adapter; Arxan neutralization is enabled |

Elden Ring remains the primary target. Select another game with
`--launch-target nightreign`, `--launch-target sekiro`, or
`--launch-target darksouls3`. When `--launch-target` is omitted, the launcher
reads the top-level `game=...` value from `YAFSML.ini`; if the value is absent,
it starts Elden Ring. An explicit `--launch-target` always takes precedence.

## Installation

### Standalone launcher

1. Extract `YAFSML.exe`, `YAFSML.dll`, and `YAFSML.ini`
   into a directory of choice.
2. Edit `YAFSML.ini` and enable the required external DLLs or mods.
3. Run `YAFSML.exe`.

The launcher finds the game in its current directory, from an explicit path,
or through the Steam library. It starts the game suspended, injects the loader
DLL, and resumes the game unless `--suspend` is used.

### Proxy DLL

1. Put `YAFSML.dll` and `YAFSML.ini` in the game directory.
2. Rename `YAFSML.dll` to `dxgi.dll`, `dinput8.dll`, or `winhttp.dll`.
3. Start the game without Easy Anti-Cheat.

For a direct `eldenring.exe` launch, create `steam_appid.txt` beside the game
executable and put `1245620` in it. The corresponding Steam App ID is selected
automatically for other launcher targets.

## Configuration

The complete template is `src/YAFSML.ini` and is copied into release
packages as `YAFSML.ini`. Boolean values accept `true`, `yes`, `on`, or
`1`; other values are false.

### Top-level options

These options are outside a section:

| Option | Default | Description |
| --- | --- | --- |
| `game` | `eldenring` | Select the standalone launcher's game when `--launch-target` is omitted. Accepted values include `eldenring`, `nightreign`, `sekiro`, `darksouls3`, and their aliases. |

### `[patch]`

This section contains common patch settings. The loader applies settings that
are supported by the current game; Dark Souls III remains experimental and
includes a compact C11 port of dearxan v0.5.3. Its before-main scheduling follows
me3 commit `6563ebb`; Dark Souls III forces Arxan neutralization on.

| Option | Default | Description |
| --- | --- | --- |
| `skip_intro` | `1` | Skip the intro logo. |
| `prevent_regulation_save_write` | `1` | Prevent raw modded or oversized `regulation.bin` data from being written to saves. |
| `patch_mem` | `1` | Replace the Dantelion allocator with mimalloc. Nightreign does not support this patch, matching me3. |
| `patch_mem_heap_size` | `0` | Dedicated mimalloc heap size in MB; `0` uses the current game's default when `patch_mem` is supported. |
| `boot_boost` | `1` | Cache decrypted BHD headers to reduce archive startup time. |
| `disable_arxan` | `0` | Neutralize Arxan after its entry stub completes. Dark Souls III forces this setting on. |
| `replace_save_filename` | Empty | Replace a save filename; a leading dot replaces only its extension. |
| `replace_seamless_coop_save_filename` | Empty | Replace the additional Seamless Co-op save filename. |
| `enable_ime` | `0` | Keep IME enabled for mods that need non-Latin text input. |

### `[tweak]`

| Option | Default | Description |
| --- | --- | --- |
| `cpu_affinity` | `0` | Select the game process CPU affinity strategy: `0` leaves affinity unchanged, `1` uses all logical cores except the first, `2` uses efficient cores, `3` uses performance cores, and `4` uses performance cores except their first logical core. Strategies `1` through `4` are applied asynchronously after game data initialization in all four games. Unsupported processor-group layouts or an empty selected mask leave affinity unchanged. Do not use `2`, `3`, or `4` on Intel Ultra CPUs with Elden Ring 1.16.2 or later. |

The game-data-ready trigger is optional. If its hook cannot be installed, the loader
keeps the current affinity and continues with runtime initialization, VFS, logo,
property, regulation, and external-DLL capabilities. The affinity worker is joined
during unload.

### `[log]`

| Option | Default | Description |
| --- | --- | --- |
| `console` | `0` | Open a debug console for logging. |
| `log_file` | `0` | When present, write logs to `log/YAFSML.log` beside the configuration file. |
| `log_level` | `warn` | Minimum log level: `trace`, `debug`, `info`, `warn`, `error`, or `off`. |

### DLLs and mods

The `[dll]` section loads external DLLs at game startup. Paths can be relative
to the configuration file or absolute. Without conditions, DLLs keep the
backward-compatible behavior and load after `SteamAPI_Init`. A value can append
pipe-separated conditions using `name=path_to_file.dll|conditions...`:

- `early` loads the DLL before `SteamAPI_Init`.
- `delay,500` waits 500 ms before loading the DLL.
- `after,abc` loads the DLL after the `[dll]` entry named `abc`, not after its
  file path.

Dependencies reorder entries only when necessary and preserve the configured
order otherwise. If an `early` DLL depends on a normal entry, that prerequisite
is promoted to early loading. Cyclic dependencies are reported and leave the
configured order unchanged. Project-owned extension DLLs are no longer shipped
with this repository.

The `[mod]` section lists directories containing loose-file overrides. Paths
can be relative to the configuration file or absolute. When multiple mods
contain the same file, the later declaration overrides the earlier one.

### ModEngine2 TOML compatibility

If `YAFSML.ini` is absent, the loader looks for the game-specific
ModEngine2 file: `config_eldenring.toml`, `config_nightreign.toml`,
`config_sekiro.toml`, or `config_darksouls3.toml`. The `-c` launcher option or `YAFSML_CONFIG`
environment variable can select another configuration path.

## Launcher options

```text
-t, --launch-target <game>  Select eldenring, nightreign, sekiro, or darksouls3.
-p, --game-path <path>      Game executable or game directory.
-c, --config <path>         Configuration file or directory.
-d, --modloader-dll <path> Loader DLL to inject.
    --modengine-dll <path> Compatibility alias for --modloader-dll.
-s, --suspend               Leave the game suspended after injection.
```

`--launch-target` overrides the top-level `game=...` setting in `YAFSML.ini`.

## Changelog

See [CHANGELOG.md](CHANGELOG.md).

## Credits

- [ModEngine](https://github.com/soulsmods/ModEngine2): original Souls mod loader.
- [minhook](https://github.com/TsudaKageyu/minhook): function hooking.
- [klib](https://github.com/attractivechaos/klib): hash tables.
- [inih](https://github.com/benhoyt/inih): INI parsing.
- [toml-c](https://github.com/arp242/toml-c): ModEngine2 TOML compatibility.
- [wingetopt](https://github.com/alex85k/wingetopt): command-line parsing.
- [mimalloc](https://github.com/microsoft/mimalloc): loader allocator.
- [dearxan](https://github.com/tremwil/dearxan): Arxan analysis and neutralization behavior ported to C11.
- [Zydis](https://github.com/zyantific/zydis): x86/x86-64 disassembly used by the dearxan port.
- [libofdf](https://github.com/Jan200101/libofdf): Steam library discovery.
- [LZMA SDK](https://7-zip.org/sdk.html): public-domain compression SDK.
