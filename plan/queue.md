# Queue: evdev prerequisite repair and Noct completion

Last updated: 2026-08-28

QID: `q020`

Queue status: in-progress

Queue finished: **No**

Authorization: selected automatically under the user's approved WS-priority
Queue loop on 2026-08-28; Phase-boundary commit and push are authorized for the
zedBSD repository

Timebox: continuous execution through 2026-08-28 09:00 JST; stop earlier only
when every Queue item has been processed or no dependency-ready item remains

Parent: [master plan](master.md)

Previous Queue: [q019](queue-q019.md)

## Purpose

Repair the frozen evdev capability/state implementation needed by dynamic
consumers, then resume the canonical Noct BeUI migration stopped in q019 and
finish the independent amd64 JIT acceptance. The repair is deliberately placed
before WS008 rather than accepting event-number or name inference in BeUI.

## Execution registry

| Priority | WS / Phase | Authoritative documents | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws006-p005` | [WS006](ws006-input/ws.md), [Phase](ws006-input/phase005-evdev-capability-state/phase.md), [tests](ws006-input/tests/README.md) | complete | Production evdev reports registered type/code capabilities, current key/button state, and ABS metadata/state, and a guest discovers keyboard/pointer roles without numbers or names |
| 2 | `ws008-p002` | [WS008](ws008-noct/ws.md), [Phase](ws008-noct/phase002-beui-zedbsd/phase.md), [tests](ws008-noct/tests/README.md) | in-progress | Canonical BeUI uses `/dev/graphics` and capability-discovered evdev, the downstream duplicate is removed, and host/QEMU backend evidence passes |
| 3 | `ws008-p003` | [WS008](ws008-noct/ws.md), [Phase](ws008-noct/phase003-amd64-jit/phase.md), [tests](ws008-noct/tests/README.md) | pending | Direct VM and canonical Noct probes prove generated amd64 code executes after RW-to-RX protection with no accepted fallback or RWX mapping |

## Entry evidence and dependency order

- `ws006-p005` completed in q020. Native-word capability/state bitmaps,
  strict ioctl dispatch, producer declarations, host/sanitizer fixtures, full
  build, and capability-only QEMU discovery passed; the transcript is linked
  from the Phase. `ws008-p002` is therefore dependency-ready and active.
- q019 completed `ws008-p001` and proved the canonical CMake artifact in QEMU.
- q019 stopped p002 before canonical backend work because the production input
  core cannot answer the already-declared capability/state requests. The exact
  gap and prohibition on private fallback are recorded in its Phase book.
- The public evdev structures, constants, ioctl encoding, event values, queue,
  producers, and console coexistence are already fixed by `ws006-p001`--p004.
  p005 requires no new product decision or public ABI redesign.
- p002 resumes only after p005 completes. p003 starts only after p002 completes.
  An uncleared predecessor leaves dependent items unexecuted.
- `/home/awe/NoctLang` and `userland/noct` remain at
  `7d856856e16eb2d889ba49f557f2fda4dcaeea7e` with the same uncommitted p001
  change set. They remain uncommitted/unpublished and their gitlink is not
  advanced by this Queue.

## Ordered execution

1. Execute [ws006-p005](ws006-input/phase005-evdev-capability-state/phase.md):
   add bounded registered capability/ABS metadata, core-maintained state,
   length-safe queries, accurate keyboard/mouse declarations, focused host
   fixtures, and capability-only guest discovery.
2. On p005 completion, resume
   [ws008-p002](ws008-noct/phase002-beui-zedbsd/phase.md) from its recorded
   q019 stop point and complete canonical graphics/evdev migration and tests.
3. On p002 completion, execute
   [ws008-p003](ws008-noct/phase003-amd64-jit/phase.md) and prove direct and
   canonical Noct RW-to-RX execution without fallback or RWX mappings.
4. After each processed Phase, synchronize P/W/M/Q/test evidence, run
   `git add -A`, `git commit -m WIP`, and `git push`. If push is rejected by
   the environment, retain the local commit and continue.
5. Finish q020 when every item is complete or honestly uncleared, then select
   the next dependency-ready work from the approved priority sequence.

## Stop, defer, and continuation rules

- Ordinary bitset, validation, copyout, state, producer metadata, canonical
  backend, build, QEMU, VM, and JIT defects inside the linked Phase contracts
  remain in scope and are repaired.
- p005 must not change public ioctl encodings, create stable event numbering,
  or choose new USB/multitouch policy. Such a need makes p005 `uncleared` and
  requires human review.
- p002 must not use event numbers, names, product IDs, private ioctls, or LFB
  mapping as a substitute for the public capability/state contract.
- p003 must not weaken W^X, accept interpreter fallback, or enable permanent
  RWX mappings.
- Do not commit/push canonical Noct checkouts or advance their package gitlink.
  Use `make -j16`, never `make check` or `.internal/`, and use disposable QEMU
  images for mutating guest tests.

## Approval boundary

The user's automatic Queue-loop authorization covers this finite dependency
repair followed by the two already-selected WS008 Phases. It does not authorize
USB HID, Xzed migration, legacy console-UAPI removal, LFB acceleration, public
evdev ABI redesign, or publication of canonical Noct changes.
