# dearxan Repository Reference

This file records the upstream baseline used by the C11 implementation in
`src/dearxan`.

- Repository: https://github.com/tremwil/dearxan
- Tag: `v0.5.7`
- Synced commit: `a14847b005bef368662b2b5c714227bb2a6a8504`
- Previous baseline: `v0.5.6` (`29f69c091a16917a34f032b90df66381aff01570`)

## Review through `v0.5.7` (2026-10-14)

- Ported `b98473e`: MSVC entry-point detection accepts an interrupt or exception
  instruction at the start, including INT3 and UD2, while continuing to validate
  the relative security-cookie call, stack restoration, and CRT jump.
- Adapted the skip to Zydis: a breakpoint replacing the original opcode retains
  the four-byte `SUB RSP, imm8` slot. The parser also accepts upstream's skip of
  the decoded trap followed by that four-byte slot, covering inserted traps.
  Both layouts preserve the cookie-call hook address and relative targets.
- Added regression coverage for both layouts, ordinary first-instruction
  mismatches, invalid later instructions, and truncated images.
- `8dbfada` and `a14847b` update the upstream changelog and Rust release metadata;
  the C11 port records the tag and commit without adopting Cargo files.
