# Queue: target-package hold and kernel ownership foundation

Last updated: 2026-08-28

QID: `q025`

Queue status: completed

Queue finished: **Yes**

Authorization: on 2026-08-28 the user instructed the agent to remove Noct
from the target build options, leave Noct to Principal Engineer manual repair,
then fill and execute a kernel-work Queue. This authorizes the finite sequence
below without authorizing any Noct source edit.

Timebox: no fixed wall-clock limit; six finite Phases.

Parent: [master plan](master.md)

Previous Queue: [q024](queue-q024.md)

## Purpose

Quarantine the rejected target Noct package without breaking the separate
host script runtime, then establish the first WS018 kernel ownership wave:
relocate the driver tree, consolidate dead/split kernel core and boot APIs,
and separate UFS implementations plus filesystem identity dispatch.

## Execution registry

| Priority | WS / Phase | Authoritative documents | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws008-p007` | [Phase](ws008-noct/phase007-target-package-hold/phase.md) | completed | Target Noct and dependent Remacs are unavailable to image selection while host Noct scripting remains operational |
| 2 | `ws018-p001` | [Phase](ws018-kernel-architecture/phase001-driver-source-tree/phase.md) | completed | Move repository-root drivers to `src/drivers/` and repair every maintained build/test path without behavior change |
| 3 | `ws018-p005` | [Phase](ws018-kernel-architecture/phase005-core-source-consolidation/phase.md) | completed | Merge exec preparation into `exec.c` and delete only proven-dead image code |
| 4 | `ws018-p006` | [Phase](ws018-kernel-architecture/phase006-boot-api-consolidation/phase.md) | completed | Consolidate boot implementation and public declarations into one stable `boot.c`/`boot.h` contract |
| 5 | `ws018-p003` | [Phase](ws018-kernel-architecture/phase003-ufs-independence/phase.md) | completed | Make UFS1 and UFS2 independent filesystem implementations without a common implementation directory |
| 6 | `ws018-p004` | [Phase](ws018-kernel-architecture/phase004-filesystem-identity/phase.md) | completed | Move filesystem recognition behind the filesystem-driver identity callback while generic identities remain generic |

## Dependency order

```text
ws008-p007 (independent target-package hold)

ws018-p001
  +-- ws018-p005 -> ws018-p006
  +-- ws018-p003 -> ws018-p004
```

An item starts only after every predecessor in this Queue has completed. An
`uncleared` predecessor makes its dependent item `uncleared/not started`; the
independent branch may continue.

## Execution rules

- Do not inspect or modify `userland/noct/NoctLang` or `/home/awe/NoctLang`.
- Preserve the host Noct runtime under `build/NoctLang`; it is infrastructure
  for accepted project-owned build scripts, not the disabled target package.
- Perform source moves mechanically first, then semantic consolidation in the
  owning Phase. Do not combine later FAT/input/graphics/platform deletion work
  into q025.
- Preserve all unrelated dirty work. Do not reset, checkout, clean, or broadly
  stage files.
- Use `make -j16`, focused WS018 fixtures, and `qemu-system-x86_64` where an
  amd64 runtime gate is required. Do not run `make check` or consume
  `.internal/`.
- A newly discovered API/product decision is recorded in its Phase and that
  item becomes `uncleared`; continue an independent dependency-ready branch.
- Do not commit or push during q025 unless the user separately requests it.

## Completion definition

q025 is finished when all six items are `completed` or `uncleared`, all actual
results are synchronized to P/W/M/Q, and any human decisions have exact resume
conditions. The Queue may finish with residual work, but it must not claim a
Phase whose source ownership or behavioral gates are incomplete.

## Execution result

Completed on 2026-08-28.  All six items completed; no item was deferred and no
human decision boundary was reached.

- The target Noct/Remacs package selections are held while the independent
  host Noct scripting runtime remains available.
- The root driver tree moved to `src/drivers`, exec and boot implementation
  boundaries were consolidated, and the aggregate boot API remains stable.
- UFS1/UFS2 are independent driver groups, and FAT/UFS filesystem identity is
  now dispatched through the filesystem interface rather than parsed by
  generic block core.
- Focused WS018 fixtures, six supported builds, representative four-platform
  boots, amd64 BIOS/UEFI UUID cross-boot, normal `make -j16`, and whitespace
  checks passed.  The p004 Phase records one unrelated intermittent ATA retry
  transparently.
