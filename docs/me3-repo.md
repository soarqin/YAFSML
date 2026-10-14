# me3 Repository Reference

This file records the exact me3 baselines used for behavior comparisons and
ports. Do not infer the baseline from another local checkout.

## Original Repository

- Repository: https://github.com/garyttierney/me3
- Branch: `main`
- Synced commit: `5f4fa47fad537b915fa9dbc01a9fb91b8a081bb5`

Update the commit only after every change through a newer revision has been
reviewed. Project-inapplicable or intentionally deferred changes must be noted
below so that they are not mistaken for unreviewed parity gaps.

### Review through `5f4fa47` (2026-10-14)

- Ported `a0ecf62` / `73a2cd0`: when running under Wine, the launcher temporarily
  unsets Unix `LC_ALL` around `CreateProcessW` using Proton's ntdll environment
  exports. It restores the original value on success and failure and preserves
  the process-creation error. Missing exports or environment errors are logged
  and retain the existing launch behavior.
- Adapted `d66218e` / `ccd7136`: both remote `LoadLibraryW` and `YAFSMLInit` waits
  now observe the game-process handle as well as the remote thread. Game exit
  takes precedence over completed initialization. YAFSML has no attach-request
  IPC bridge, so the Rust worker-thread and channel implementation is unnecessary;
  initialization failure continues to terminate the spawned game.
- The CLI half of `bd0c5e0` / `ccd7136` fixes Ctrl+C while opening a monitor pipe.
  YAFSML's WIN32 launcher has neither that CLI nor a monitor pipe, so there is no
  corresponding blocking operation to port.
- `bbb9adf` / `9b1e080` fixes `STEAM_COMPAT_LIBRARY_PATHS` in the Linux compatibility
  tool launch strategy. YAFSML does not launch Proton or set that variable; its
  Windows launcher can run inside an already configured Wine environment.
- `ef396e1` / `4c1dce7` and `d800dac` / `5f4fa47` update dearxan dependencies. The
  C11 implementation is now based on v0.5.7; see
  [the dearxan review record](dearxan-repo.md). Cargo-only `bitvec` and `bytemuck`
  lockfile updates have no C11 dependency equivalent.
- Previously deferred system-property overrides remain deferred as documented
  below; this review does not change that configuration or ABI decision.

### Code review alignment (2026-10-14)

A review of the whole code base re-checked the following behavior against
`5f4fa47`:

- The SPRJ regulation-writer scan again requires `test al, al; je` after the
  call, as me3 does. The looser pattern also matched the save-regulation reader
  and Dark Souls III stats requests.
- A `modengine_ext_init` that returns false is reported, and the external DLLs
  after it still load.
- Every game advances `AFTER_RUNTIME_INIT` before installing the asset hooks;
  Elden Ring and Sekiro used to install them first.
- `MountEbl` detection prefers the call-site signature and uses the
  whole-function signature only when no call site matches.
- BootBoost measures the PEM key with `strlen` instead of the game's `key_len`,
  and reports a successful full mount as mounted even when no mount can be
  extracted for the cache.
- Sekiro's debug allocator getter is the most frequent call target in `.text`.
- Intentional difference: when the dedicated heap cannot satisfy a
  reallocation, YAFSML retries through the global mimalloc heap. me3 retries
  only allocations (`15a19de`).

### Review through `1ae84ae` (2026-08-13)

- Ported the dedicated mimalloc arena from `da9abcf`: YAFSML now reserves the
  arena with `MEM_TOP_DOWN`, registers it through `mi_manage_os_memory_ex`, and
  preserves the existing fallback to the regular mimalloc heap.
- Ported the optional heap mapping diagnostics under the YAFSML-specific
  environment variables `YAFSML_HEAP_MAPPING_FILE` and
  `YAFSML_HEAP_MAPPING_NAME`. Mapping failures fall back to private
  high-address memory instead of terminating the game.
- Deferred the system-property map support from `6d381d7`. YAFSML does not
  currently expose arbitrary property overrides, while a safe C11 port would
  require MSVC 2012/2015 tree insertion, a second property-init hook, and the
  Nightreign custom UTF-16 string ABI. Existing offline and Dark Souls III
  loose-param properties continue to use the established debug-property API.
- The `DlString` capacity adjustment in `6d381d7` is already reflected by
  YAFSML's string construction, and the Rust allocator-error handling change
  has no C equivalent to port.
- Ported the allocation fallback from `15a19de`: if the dedicated mimalloc heap
  cannot satisfy an allocation, YAFSML now retries through the global mimalloc
  heap. The existing `mi_free` path already accepts allocations from either
  heap.
- Deferred the installer quoting fixes from `2cd22c0` and `eed4919`. YAFSML
  does not ship me3's NSIS or shell installer, so there is no corresponding
  installer command path to update.
- The MSRV bump in `41e8f3f` is Rust-specific and has no CMake/MSVC equivalent.
- The `1ae84ae` release commit only updates me3 release metadata, changelog,
  issue-template versions, and installer versioning; none of those files or
  release artifacts are part of YAFSML's me3 behavior port.

## Performance Fork

- Repository: https://github.com/soarqin/me3
- Branch: `perf/input-path-override-cache`
- Synced commit: `d3f8700fbebe0815550386e30b6917e049e600a2`

The VFS dictionary cache follows this fork's stable cached-object optimization.

### Review through `d3f8700` (2026-10-14)

- The performance commit was rebased onto original me3 `1ae84ae`. Its cache,
  asset-hook, device-mount, nonblocking-log, and telemetry source files are
  identical to the previous `1cf225d` baseline; only the assets crate's version
  metadata differs in those paths. The existing C11 cache optimization is retained.
- Changes inherited from the newer original baseline were reviewed above,
  including the deferred system-property map support. This fork does not yet
  contain original me3's later changes through `5f4fa47`; those are reviewed and
  ported separately under the Original Repository section.
- 2026-10-14: the ordinary `MountEbl` path no longer extracts the newly mounted
  devices and pushes them back. As in this fork, the mounts stay resident in the
  device manager and their BND4 `open_file` is hooked; original me3 instead
  moves them into its `VFS_MOUNTS` list.
