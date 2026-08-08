# me3 Repository Reference

This file records the exact me3 baselines used for behavior comparisons and
ports. Do not infer the baseline from another local checkout.

## Original Repository

- Repository: https://github.com/garyttierney/me3
- Branch: `main`
- Synced commit: `da9abcf`

Update the commit only after every change through a newer revision has been
reviewed. Project-inapplicable or intentionally deferred changes must be noted
below so that they are not mistaken for unreviewed parity gaps.

### Review through `da9abcf` (2026-08-08)

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
- The remaining commits through `da9abcf` change me3-specific Cargo/CI,
  funding, repository policy, Sentry release handling, or mod-profile
  documentation and do not apply to this CMake/MSVC repository.

## Performance Fork

- Repository: https://github.com/soarqin/me3
- Branch: `perf/input-path-override-cache`
- Synced commit: `1cf225d`

The VFS dictionary cache follows this fork's stable cached-object optimization.
