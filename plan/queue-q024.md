# Queue record: Noct maintainer API/layout review

Last updated: 2026-08-28

QID: `q024`

Queue status: finished with an uncleared item

Queue finished: **Yes**

Authorization: on 2026-08-28 the user accepted the Noct work and instructed
that q024 execute after the separately requested kernel-refactoring work had
been written as Phases.  WS018 p001--p012 were planned without implementation;
q024 then crossed the execution boundary with its one Noct Phase and no fixed
wall-clock limit.

Timebox: no fixed wall-clock limit; one finite Phase only.

Parent: [master plan](master.md)

Previous Queue: [q023](queue-q023.md)

## Purpose

Apply the maintainer's Noct review without changing the accepted aggregate
public header: finish standalone ANSI/Win32 Term implementations, make every
BeUI platform implementation self-contained, combine the PC-98 files, remove
obsolete callback backends, and repair CMake for sources already moved to
`src/accel/` and `src/core/`.

## Execution registry

| Priority | WS / Phase | Authoritative documents | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws008-p006` | [WS008](ws008-noct/ws.md), [Phase](ws008-noct/phase006-maintainer-api-layout-review/phase.md), [tests](ws008-noct/tests/README.md) | uncleared after maintainer review | Automated gates passed, but the Principal Engineer rejected the implementation quality during the first manual source review; the maintainer has taken ownership of the repair |

## Entry evidence and decisions

- The authoritative checkout is `userland/noct/NoctLang`, detached at
  `c1e4e0fcdbb7b8cdf1705601b13d57b787c61621` with maintainer edits present.
  `/home/awe/NoctLang` is another dirty checkout and is excluded.
- The protected `include/noct/noct.h` planning hash is
  `90c2115d53840fe3d6c1fdff6751676a35d473d2503fc9a3d9c179e1fb22a7b3`.
- The fixed header intentionally exposes only `noct_register_api_term()` and
  `noct_register_api_beui()` for these subsystems.  Removed callback backend
  declarations are not restored privately or publicly.
- The standalone `jisx0208.c` currently serves both File API EUC-JP and PC-98
  glyph conversion.  The accepted independent-module design duplicates its
  static table into `api-file.c` and combined `api-beui-pc98.c`.
- The Queue intentionally excludes all kernel-layout planning and
  implementation from the preceding discussion.

## Execution procedure

1. Confirm the protected hash and record a path-scoped dirty inventory.
2. Execute `ws008-p006` exactly as specified by its Phase book.
3. Verify all applicable `NOCT-T050`--`NOCT-T057` gates and record unavailable
   optional platform/hardware gates honestly.
4. Mark the Phase completed only when local canonical changes are ready for
   maintainer inspection and all required local/zedBSD gates pass.
5. Synchronize P/W/M/Q status and report the exact local changes and evidence.

## Scope and stop rules

- Do not edit, regenerate, format, replace, or stage `include/noct/noct.h`.
- Do not delete or stage the checkout's untracked editor/backup files.
- Do not use reset, checkout, clean, `git add -A`, or broad destructive Git
  operations in either dirty Noct checkout.
- Do not restore a shared Term/BeUI implementation, callback backend API, or
  platform dispatcher merely to reduce duplication.
- Do not change language-level Term, File, or BeUI behavior.  A required public
  API change stops the Phase for maintainer judgment.
- Do not commit or push canonical Noct, update either zedBSD Noct revision pin,
  or commit/push zedBSD in q024.  Two-repository publication remains a separate
  explicit authorization after local review.
- Do not implement or plan the kernel source-layout changes in this Queue.
- Do not run aggregate `make check` or consume `.internal/`.

## Completion record

- `ws008-p006` passed its automated gates locally on 2026-08-28, but a later
  Principal Engineer review rejected the implementation quality. The Queue
  item is therefore `uncleared`, not complete, regardless of the passing
  build/test evidence.
- Static/shared host, Win32 Term, SDL2/PC-98/zedBSD BeUI, public File/Term and
  EUC-JP, moved accelerator/regex, outer `make -j16`, and non-JIT/BeUI/JIT QEMU
  gates passed wherever their toolchains were available.
- OpenWatcom and optional OpenGL/Vulkan/DX12 toolchains/hardware were absent
  and were not falsely reported as passes.
- One first QEMU non-JIT attempt encountered a pre-Noct IDE overlay-data EIO;
  a fresh identical run and all other required QEMU gates passed. Details are
  retained in the [Phase evidence](ws008-noct/phase006-maintainer-api-layout-review/evidence.md).
- No commit, push, publication, revision-pin update, `make check`, or
  `.internal/` consumption occurred.
- Resume condition: the maintainer completes or explicitly accepts a manual
  Noct repair and separately authorizes re-enabling the target package. Agent
  work must not edit the Noct source tree meanwhile.

## Completion definition

q024 finishes when p006 is either completed with the protected header unchanged
and all applicable evidence, or `uncleared` with the exact conflict and resume
condition.  Local review completion does not imply upstream publication or
revision-pin changes.
