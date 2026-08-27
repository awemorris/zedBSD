# Queue: canonical Noct zedBSD integration

Last updated: 2026-08-28

QID: `q019`

Queue status: in-progress

Queue finished: **No**

Authorization: selected automatically under the user's approved WS-priority
Queue loop on 2026-08-28; Phase-boundary commit and push are authorized for the
zedBSD repository

Timebox: continuous execution through 2026-08-28 09:00 JST; stop earlier only
when every Queue item has been processed or no dependency-ready item remains

Parent: [master plan](master.md)

Previous Queue: [q018](queue-q018.md)

## Purpose

Complete WS008 as one finite, dependency-ordered Queue. First make canonical
Noct build the real static amd64 zedBSD target through the public CMake preset,
then move BeUI's zedBSD graphics/input backend into that canonical tree, and
finally prove the installed canonical executable uses the supported RW-to-RX
JIT path in amd64 QEMU.

## Execution registry

| Priority | WS / Phase | Authoritative documents | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws008-p001` | [WS008](ws008-noct/ws.md), [Phase](ws008-noct/phase001-zedbsd-preset/phase.md), [tests](ws008-noct/tests/README.md) | complete | `cmake --preset zedbsd` and its build preset produce the static canonical amd64 zedBSD Noct artifact, the package installs that artifact, and a non-JIT QEMU smoke passes |
| 2 | `ws008-p002` | [WS008](ws008-noct/ws.md), [Phase](ws008-noct/phase002-beui-zedbsd/phase.md), [tests](ws008-noct/tests/README.md) | pending | Canonical BeUI uses `/dev/graphics` and capability-discovered evdev, the downstream duplicate is removed, and host/QEMU backend evidence passes |
| 3 | `ws008-p003` | [WS008](ws008-noct/ws.md), [Phase](ws008-noct/phase003-amd64-jit/phase.md), [tests](ws008-noct/tests/README.md) | pending | Direct VM and canonical Noct probes prove generated amd64 code executes after RW-to-RX protection with no accepted fallback or RWX mapping |

## Entry evidence and dependency order

- WS004 has no Phase after completed `ws004-p009`, and WS012 closed with
  `ws012-p006`; WS008 is therefore the next existing Phase set in the approved
  priority order.
- `/home/awe/NoctLang` and `userland/noct` were both clean at
  `7d856856e16eb2d889ba49f557f2fda4dcaeea7e` when q019 was selected.
- `ws006-p001` through p004, required by the p002 input backend, are complete.
- p002 starts only after p001 completes. p003 starts only after p002 completes.
  An uncleared predecessor leaves its dependent items unexecuted.
- The official Noct checkout and integration checkout remain governed by the
  WS008 Phase books: record parity, but do not publish, commit, push, or advance
  their revision/gitlink implicitly. The user's Phase checkpoint applies to
  the enclosing zedBSD repository.

## Ordered execution

1. Execute [p001](ws008-noct/phase001-zedbsd-preset/phase.md): preserve the
   clean baselines, implement target/toolchain/presets in canonical Noct,
   migrate the package to the CMake artifact, add machine-readable target and
   ELF checks, and pass a non-JIT amd64 QEMU smoke plus `make -j16`.
2. If p001 completes, execute
   [p002](ws008-noct/phase002-beui-zedbsd/phase.md): move the zedBSD BeUI HAL to
   canonical Noct, use public graphics and evdev UAPI with dynamic capability
   discovery, delete the downstream duplicate after parity, and pass focused
   host plus amd64 QEMU input/render evidence.
3. If p002 completes, execute
   [p003](ws008-noct/phase003-amd64-jit/phase.md): prove the VM transition with
   a direct guest probe, then require positive canonical Noct JIT evidence and
   a matching interpreter reference in a disposable amd64 image.
4. After each processed Phase, synchronize P/WS/M/Q/test evidence, run
   `git add -A`, `git commit -m WIP`, and `git push` in the zedBSD repository.
   If push is rejected by the environment, retain the local commit and continue.
5. Archive q019 when every item has completed or been honestly deferred, then
   select the next dependency-ready WS from the approved priority order.

## Stop, defer, and continuation rules

- Ordinary target, build, package, backend, guest-probe, and narrow VM/JIT
  defects inside the fixed Phase contracts remain in scope and are repaired.
- If p001 requires a new public ABI, general sysroot framework, shared-library
  policy, or CLI product choice, mark it `uncleared`, record the exact failure,
  and leave p002/p003 unexecuted.
- If p002 shows the public evdev contract cannot express required state, hand
  the issue to WS006. If graphics correctness requires the planned LFB mapping,
  hand it to WS017. Do not add a private BeUI ioctl.
- If p003 requires weakening W^X, permanent RWX, or a public VM redesign, mark
  it `uncleared` and request human review rather than accepting fallback.
- Do not test a host Noct binary as if it were the zedBSD artifact. Do not
  preserve an empty/manual-link false success or duplicate UAPI definitions.
- Use `make -j16`; do not run `make check` or consume `.internal/`. Use
  disposable images for guest mutation and preserve `config.mk`.

## Approval boundary

q019 authorizes only `ws008-p001` through p003 in dependency order. It does not
authorize publishing canonical Noct changes, advancing the package revision,
or mixing WS016/WS017 implementation into this Queue.
