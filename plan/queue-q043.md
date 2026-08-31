# Queue: PC-9821V13 IPL compatibility and Xzed mouse reproduction

Last updated: 2026-08-31

QID: `q043`

Queue status: finished

Queue finished: **Yes**

Authorization: the user requested repair of the PC-9821V13 boot regression
and investigation of the reproducible PC-98 Xzed mouse report, then directed
continuous execution of the remaining workstreams. q043 is the finite PC-98
repair and diagnostic Queue before that wider continuation.

Timebox: none. Process both rows to `completed` or `uncleared`, synchronize
P/W/M, commit locally, and immediately select the next executable Queue.

Parent: [master plan](master.md)

Previous Queue: [q042](queue-q042.md)

## Purpose

Restore the conservative fixed-sector Stage-1 path used by native PC-98 IPLs,
produce one exact PC-9821V13 handoff artifact, and reconcile the user's Xzed
mouse report against a hash-recorded qemu-pc98 reproduction matrix.

## Execution registry

| Priority | WS / Phase | Authoritative document | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws003-p024` | [Phase](ws003-bringup/phase024-pc9821-v13-stage1-fixed-read-compatibility/phase.md) | uncleared | Fixed-read source/binary/layout and normal/diagnostic QEMU milestones pass; exact artifact `7d4e7d67...` awaits one PC-9821V13 boot, while Make-owned Noct gates resume through `ws008-p010` |
| 2 | `ws007-p004` | [Phase](ws007-graphics/phase004-pc98-xzed-mouse-exact-reproduction/phase.md) | uncleared | Final image/QEMU headless oracle passes exactly `(320,240) -> (420,290)` with all focused host gates; maintained QEMU exposes no interactive backend, so the user's exact failing GUI environment is the sole resume condition |

## Fixed boundaries

- p024 changes only LBA-0 Stage 1's unnecessary AH=`84h` SENSE path. PBR,
  BOOTZBSD, disk layout, `IPL1`, fixed CHS 0/0/2 read, boot-device identity,
  and bounded diagnostics remain authoritative.
- Ordinary boot remains silent. Diagnostic artifacts use only finite markers
  already defined by the Phase.
- p004 makes no speculative driver, public ABI, `/dev/mouse`, or PIC change.
  The maintained qemu-pc98 build currently exposes only the `none` display
  backend; exhaust its deterministic cells and record that limitation rather
  than inventing GUI evidence.
- Do not modify Noct or add a build workaround. Record the independent
  `ws008-p010` host-CLI regression wherever it blocks a production target.
- Do not use `.internal/` or aggregate `make check`. Use focused fixtures,
  disposable images, bounded QEMU runs, `make -j16`, and `git diff --check`.

## Completion definition

q043 is finished when both rows are processed. It may close with p024
`uncleared` solely for its one physical PC-9821V13 result and p004 `uncleared`
when every locally controllable cell passes but the user's failing GUI
environment is not identifiable. Each such result must include the exact
resume condition and must not block creation of the next Queue.

## Closure

q043 is finished with both rows processed honestly. p024 removes only the
unused Stage-1 SENSE path, retains the native PC-98 disk and later geometry
contracts, passes independent review, and reaches login in both ordinary and
diagnostic qemu-pc98 runs. Its one physical result and independently blocked
Noct-owned production checks remain explicit resume conditions. p004 changes
no source: the final immutable headless image again moves Xzed exactly
`(320,240) -> (420,290)` and all focused gates pass, while the maintained
emulator supplies no interactive display backend with which to reproduce the
user's GUI report. Neither external condition blocks the next Queue.
