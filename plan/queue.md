# Queue: WS018 completion

Last updated: 2026-08-30

QID: `q035`

Queue status: in-progress

Queue finished: **No**

Authorization: on 2026-08-30 the user supplied an ordered backlog, directed
the agent to reflect it into the plan, construct Queues in that order, and
execute autonomously until stopped or no human-judgment-free work remains.

Timebox: no fixed wall-clock limit. This Queue is nevertheless finite and
contains only the four already-defined residual WS018 Phases.

Parent: [master plan](master.md)

Previous Queue: [q034](queue-q034.md)

## Purpose

Finish the kernel source-ownership workstream without silently broadening its
contracts: close the retained graphics runtime evidence if maintained runners
can be established, mechanically consolidate FAT, migrate FAT to native VFS,
then remove the legacy bootfs/startup residue only after its final consumer is
gone.

## Execution registry

| Priority | WS / Phase | Authoritative document | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws018-p009` | [Phase](ws018-kernel-architecture/phase009-independent-graphics-frontends/phase.md) | completed | Complete the remaining PC/AT VGA/Cirrus, PC-98 GDC/Cirrus, and graphics-disabled runtime matrix, or record the exact missing-runner boundary without changing the accepted UAPI |
| 2 | `ws018-p010` | [Phase](ws018-kernel-architecture/phase010-fat-source-consolidation/phase.md) | completed | One `src/drivers/fs/fat.c` and one `include/kern/fat.h` preserve the measured legacy behavior |
| 3 | `ws018-p011` | [Phase](ws018-kernel-architecture/phase011-fat-native-vfs/phase.md) | completed | FAT uses ordinary filesystem/VFS objects for boot media, overlay images, and swap with no embedded bootfs adapter |
| 4 | `ws018-p012` | [Phase](ws018-kernel-architecture/phase012-legacy-bootfs-removal/phase.md) | pending | Remove the now-unreferenced bootfs/namespace/startup/M9/internal state while preserving explicit `/dev/system` handoff ownership and four-platform boot |

## Dependency and deferral rules

- p009 is runtime-only residual work and does not gate p010. If no maintained
  runner can provide a required cell, mark p009 `uncleared` with an exact
  resume condition and continue.
- p010 is deliberately mechanical. Do not change FAT semantics while joining
  sources and headers.
- p011 starts only after p010 completes. If native VFS cannot represent stable
  identity, rename/orphan lifetime, private mount promotion, loop ownership,
  or swap extents without weakening the current guarantees, mark p011
  `uncleared`, record the required human decision, and do not execute p012.
- p012 starts only after p011 completes and a fresh live-caller audit proves
  each deletion safe.
- An unrelated defect is returned to M/W/P planning. It is not absorbed into
  q035.

## Execution rules

- Do not inspect or consume `.internal/`.
- Preserve unrelated work and maintainer-owned dirty Noct checkouts.
- Use focused WS018 fixtures, `make -j16`, and `qemu-system-x86_64`; do not use
  aggregate `make check`.
- Use disposable image copies for runtime filesystem mutation.
- After each item, synchronize P/W/M/Q state and commit `WIP`. Push only when
  the approved environment permits it; a push review failure does not block
  the next Queue item.
- Human decisions are reported and deferred while dependency-independent
  later priorities continue under a new finite Queue.

## Completion definition

q035 is finished when every registry item is `completed` or `uncleared`, each
result is synchronized into its Phase/WS/master record, and no deletion has
crossed an uncleared dependency. WS018 is complete only if all four items meet
their own completion conditions; a finished Queue with an uncleared item does
not imply that result.
