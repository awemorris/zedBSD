# Queue: Intel Mac automatic image groundwork

Last updated: 2026-08-30

QID: `q036`

Queue status: in-progress

Queue finished: **No**

Authorization: on 2026-08-30 the user inserted the Intel Mac bring-up
workstream immediately after WS018 and directed autonomous Queue execution.
The user-supplied design fixes a generic Board Variant axis, amd64 Hybrid /
BIOS-only / UEFI-only image layouts, and selectable target-medium capacity.

Timebox: none. This finite Queue contains the three automatic WS020 Phases;
the physical Intel Mac checkpoint remains outside this Queue.

Parent: [master plan](master.md)

Previous Queue: [q035](queue-q035.md)

## Purpose

Introduce the generic Architecture -> Board -> Variant and image-capacity
configuration, implement the three amd64 disk layouts without changing kernel
or loader compilation, then prove the complete positive and negative matrix
under SeaBIOS and OVMF. Produce a frozen UEFI-only artifact suitable for one
bounded Intel Mac acceptance run.

## Execution registry

| Priority | WS / Phase | Authoritative document | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws020-p001` | [Phase](ws020-intel-mac/phase001-target-variant-config/phase.md) | completed | Generic board-owned Variant and 2--256 GiB capacity selections round-trip through menuconfig/config.mk while all amd64 compiled artifacts remain identical |
| 2 | `ws020-p002` | [Phase](ws020-intel-mac/phase002-image-layouts/phase.md) | completed | Hybrid, BIOS-only, and compact UEFI-only layouts contain exactly their intended boot paths and strict GPT rules |
| 3 | `ws020-p003` | [Phase](ws020-intel-mac/phase003-qemu-acceptance/phase.md) | in-progress | SeaBIOS/OVMF positive and negative acceptance passes for every declared capacity |

## Dependency and deferral rules

- p002 starts only after p001 proves that Variant and capacity do not alter
  source selection or loader/kernel binaries.
- p003 starts only after p002's independent layout checker accepts all three
  profiles, including the primary-only compact GPT contract.
- `ws020-p004` is not part of q036. It requires a user-operated Intel Mac
  boot of the frozen artifact after the automatic matrix passes.
- If actual UEFI/GPT requirements contradict the fixed compact-image contract,
  stop the affected Phase and request human judgment; do not silently emit a
  full-capacity image or add a backup GPT.
- An unrelated defect is returned to M/W/P planning and does not expand q036.

## Execution rules

- Do not inspect or consume `.internal/`.
- Keep the target framework generic; do not encode Intel-Mac policy into the
  architecture or kernel source lists.
- Build both BIOS and UEFI loader families for every amd64 PC/AT Variant.
- Use disposable sparse media for capacity materialization and
  `qemu-system-x86_64`; do not use aggregate `make check`.
- Run `make -j16`, focused WS020 fixtures, and `git diff --check`.
- After each Phase, synchronize Phase/WS/master/Queue state and commit `WIP`;
  push when permitted.

## Completion definition

q036 is finished when p001--p003 are each `completed` or `uncleared` with
their exact evidence and resume condition recorded. A finished q036 does not
complete WS020: p004's physical Intel Mac checkpoint and final five-run
acceptance remain explicit human work.
