# AGENTS.md — YAFSML

## What this repo is

A Windows-only C11 mod loader for FromSoftware games (`YAFSML`). Elden Ring, Armored Core VI: Fires of Rubicon, Elden Ring Nightreign, and Sekiro are stable; Dark Souls III is experimental. Produces two artifacts:
- `YAFSML.exe` — standalone launcher (`modloader_launcher`, WIN32 subsystem)
- `YAFSML.dll` — injected DLL (`modloader_dll`, proxy for `dxgi.dll` / `dinput8.dll` / `winhttp.dll`)

When comparing or porting behavior from me3, use `docs/me3-repo.md` as the source of truth for the repository, branch, and synced commit. Do not infer the comparison baseline from whichever me3 working tree happens to be checked out. The cache dictionary system intentionally follows the performance optimization in the documented me3 fork; treat it as the project's chosen me3 implementation, not as a parity difference from the original repository.

## Build system

CMake + MSVC (Visual Studio 2026 Community observed in build cache). No Makefile, no npm, no Python build step.

**Configure (out-of-source, debug):**
```
cmake -S . -B cmake-build-debug -G "Visual Studio 18 2026" -DBUILD_TESTING=ON
```

**Build:**
```
cmake --build cmake-build-debug --config Debug
```

**Build release (with LTO + dynamic CRT + stripped binary):**
```
cmake -S . -B cmake-build-release -G "Visual Studio 18 2026" -DCMAKE_BUILD_TYPE=Release
cmake --build cmake-build-release --config Release
```

**Distribution package** (zip + 7z in `<build>/dist/`):
```
cmake --build cmake-build-release --target dist
```

Release builds link the dynamic CRT (`RELEASE_USE_STATIC_CRT` defaults to OFF).
Every supported game already depends on the VS2015+ dynamic CRT, whose later
versions stay backward compatible, so any system that can run the game can also
load the release binaries. The release workflow also selects the v142 toolset
(`-T v142`). An existing build directory keeps its cached
`RELEASE_USE_STATIC_CRT` value until it is reconfigured with `-D`.

**Run tests (CTest):**
```
ctest --test-dir cmake-build-debug -C Debug --output-on-failure
```

Or build the `RUN_TESTS` target in Visual Studio.

## Build prerequisites

- MSVC (Visual Studio with C/C++ workload)
- NASM — required for `src/modloader/patches/properties_hook.asm` and `src/modloader/proxy/proxy_stubs.asm`. CMake finds it via `CMAKE_ASM_NASM_COMPILER`. Install via scoop: `scoop install nasm`.

## Source layout

```
src/
  common/         Logging (log.c), the loader allocator mapping (allocator.h/.c), khash_wstr.h
  from/           Layouts of FromSoftware's MSVC 2012/2015 STL containers
  game/           Game registry, descriptors, aliases, and current-process context
  modloader/      Core DLL: config, mod loading, VFS and ZIP packages, external DLLs, hooks, lifecycle
    patches/      Host capabilities, the descriptor-driven game adapter (adapter.c), and game-specific pieces (Elden Ring archive resolver, Dark Souls III preparation)
    proxy/        Proxy DLL stubs for dxgi/dinput8/winhttp
  launcher/       Standalone EXE: injects the DLL into the game process
  steam/          Locates the game folder through the Steam library VDF files
  process/        PE and signature scanning, RTTI, DLRF reflection, singleton and FD4/SPRJ step discovery, CPU Sets
  dearxan/        C11 port of dearxan (Arxan neutralization)
deps/
  klib/           khash.h (header-only, INTERFACE target `klib` / alias `klib::headers`)
  minhook/        Function hooking
  inih/           INI parser
  toml-c/         TOML parser (ModEngine2 config compatibility; can be stripped via -DML_STRIP_MODENGINE_CONFIG_SUPPORT=ON)
  getopt/         wingetopt — command-line argument parsing
  mimalloc/       Allocator for the loader DLL and the game heap patch
  miniz/          Deflate for ZIP packages and the BootBoost cache
  xxhash/         Hashing for the BootBoost cache
  zydis/          x86 decoder used by dearxan
tests/
  smoke_*.c       Smoke and host tests for VFS, hooks, lifecycle, ABI, allocators, config routing, saves, and binary analysis
  test_common.h   Minimal assert macros (EXPECT_TRUE, EXPECT_EQ, EXPECT_STREQ_W, etc.)
```

## Key conventions

**Language:** C11 only. No C++. Headers use `#ifdef __cplusplus extern "C"` guards for potential C++ consumers.

**Memory:** Code in `src/modloader` and `src/common` allocates through the `ml_mem_*` functions of `src/common/allocator.h`. `YAFSML.dll` implements them with mimalloc (`src/common/allocator.c`); the smoke tests map them to `LocalAlloc`/`LocalFree` (`ML_USE_WIN32_ALLOCATOR`, with `allocator.h` force-included by `tests/CMakeLists.txt`) or provide their own (`ML_USE_TEST_ALLOCATOR`). `mimalloc_allocator.c`, the allocator handed to the game, calls `mi_*` directly. The launcher uses the C runtime allocator. Of the other shared modules, `process/` still calls `LocalAlloc`/`LocalFree` (`process/util.c` uses the C runtime), and `steam/` and `dearxan/` use the C runtime. Strings returned by toml-c are released with `free`.

**Hash tables:** `khash.h` (klib) is the only hash table. Two instantiation patterns in use:
- `KHASH_INIT(wstr, ...)` with `kh_wstr_hash_func`/`kh_wstr_hash_equal` — wide-string keys, defined in `src/common/khash_wstr.h`
- `KHASH_MAP_INIT_INT(name, value_type)` — integer keys

Always `#define kcalloc/kmalloc/krealloc/kfree` before `#include "khash.h"`, mapped to the allocator of the including file.

**Extension DLLs:** Project-owned extension DLLs have been split out of this repository. External ModEngine extensions continue to use `modengine_ext_init` through the `[dll]` configuration section.

**Signature scanning:** Game function addresses are found at runtime via byte-pattern scanning (`sig_scan`). Patterns use `??` for wildcard bytes. Offsets in comments (e.g. `/* 0x5B8 for version < 1.12 */`) track version-specific differences.

**Version string:** Defined once in `src/CMakeLists.txt` as `YAFSML_VERSION`. Update there only.

**Unicode:** All file paths use `wchar_t`. MSVC `/utf-8` flag is set globally. Non-MSVC targets require `-municode` linker flag (already set in CMakeLists).

## Tests

Tests are lightweight smoke and host tests rather than in-game integration tests. They cover VFS routing and concurrency, Win32 Hook behavior, save mapping, lifecycle and rollback, ABI layouts, allocators, and binary-analysis helpers. No mocking framework is used; failures print to stderr and return non-zero.

Tests are only built when `BUILD_TESTING=ON` is passed to CMake. Test executables are discovered from `tests/smoke_*.c`; tests that need production sources, libraries, or compile definitions must also be mapped in `tests/CMakeLists.txt`.

## Release process

Releases are automated via `.github/workflows/release.yml`. Before tagging a release:

1. **Update `CHANGELOG.md`** — move the `## [Unreleased]` entries into a new `## [X.Y.Z] - YYYY-MM-DD` section at the top (Keep a Changelog format); the workflow matches that heading exactly.
2. **Update `src/CMakeLists.txt`** — change the `YAFSML_VERSION` value to match.
3. **Verify consistency** — the tag, `src/CMakeLists.txt`, and `CHANGELOG.md` must all carry the same version string.
4. **Tag and push** — use the format `vX.Y.Z` (e.g. `v0.5.0`). Pushing the tag triggers the workflow.

```
git add CHANGELOG.md src/CMakeLists.txt
git commit -m "chore: release vX.Y.Z"
git tag vX.Y.Z
git push origin main --tags
```

The workflow will fail fast if the version in `src/CMakeLists.txt` or `CHANGELOG.md` does not match the pushed tag. It then builds the `dist` target and attaches `YAFSML-X.Y.Z.zip` and `YAFSML-X.Y.Z.7z` to the GitHub release.

## What to avoid

- Do not add C++ source files; the project is intentionally C11-only.
- Do not introduce direct `LocalAlloc`/`LocalFree` calls in new production code. Use `ml_mem_*` in `src/modloader` and `src/common` and `malloc`/`free` in the launcher; the `LocalAlloc` call sites in `process/` predate this rule.
- Do not add new dependencies without a corresponding `deps/` subdirectory and CMakeLists entry.
- The DLL embedding step of the `dist` target (`embeddll`) and the launcher's `lzma` link are commented out, and their sources are no longer in the tree; leave them unless intentionally re-adding DLL embedding.
