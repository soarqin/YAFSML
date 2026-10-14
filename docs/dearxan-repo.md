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

## Code review (2026-10-14)

A review of the C11 port against `v0.5.7` aligned these behaviors with upstream:

- SYSCALL and SYSENTER are analyzed as calls to target 0, like upstream's
  `FlowControl::Call`: the stub walker steps over them and clears the volatile
  registers.
- The RMX ciphertext probe accepts both forms of `imul edx, r/m32`
  (`Code::Imul_r32_rm32`), and a failed RMX decryption consumes its region list
  like `regions.take()`, so a later `movzx` can supply a new one.
- Interrupts, UD0 to UD2, XBEGIN, XABORT, XEND and IRET end the current
  execution path, matching upstream's `rip = None`.
- PUSHF, PUSHFQ, POPF and POPFQ move RSP and invalidate the pushed slot, as the
  iced-x86 stack-pointer increment does upstream. The stale slot used to
  multiply execution paths; Elden Ring analysis dropped from about 3.8 s to
  0.2 s with identical results on all five games.

YAFSML-specific behavior:

- When the `.text` scan finds no security cookie, as under SteamStub before
  `.text` is decrypted, the cookie address is read from
  `IMAGE_LOAD_CONFIG_DIRECTORY64::SecurityCookie`. A failed lookup is not
  cached, so a later call can retry.
- While other threads are suspended, stub analysis runs on the calling thread
  only, so no worker can wait on a loader or heap lock held by a suspended
  thread.
