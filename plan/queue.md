# Queue: Intel Mac automatic image groundwork

Last updated: 2026-08-31

QID: `q036`

Queue status: in-progress

Queue finished: **No**

Authorization: on 2026-08-30 the user inserted the Intel Mac bring-up
workstream immediately after WS018 and directed autonomous Queue execution.
The user-supplied design fixes a generic Board Variant axis and amd64 combined
UEFI+BIOS / UEFI-only / BIOS-only image layouts. On 2026-08-31 the user
removed the target-medium-capacity selector: Apple's relevant constraint is a
pure Protective MBR with no compatibility entry.

Timebox: none. This finite Queue contains the three automatic WS020 Phases;
the physical Intel Mac checkpoint remains outside this Queue.

Parent: [master plan](master.md)

Previous Queue: [q035](queue-q035.md)

## Purpose

Introduce the generic Architecture -> Board -> Variant configuration,
implement the three amd64 disk layouts without changing kernel or loader
compilation, then prove the six-cell positive and negative matrix under
SeaBIOS and OVMF. Produce a frozen fixed UEFI-only artifact suitable for one
bounded Intel Mac acceptance run.

## Execution registry

| Priority | WS / Phase | Authoritative document | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws020-p001` | [Phase](ws020-intel-mac/phase001-target-variant-config/phase.md) | completed (revised) | Generic board-owned Variant round-trips through menuconfig/config.mk, the removed capacity field is absent, and all amd64 compiled artifacts remain identical |
| 2 | `ws020-p002` | [Phase](ws020-intel-mac/phase002-image-layouts/phase.md) | in-progress | Combined, BIOS-only, and fixed pure-Protective-MBR UEFI-only layouts contain exactly their intended boot paths and strict GPT rules |
| 3 | `ws020-p003` | [Phase](ws020-intel-mac/phase003-qemu-acceptance/phase.md) | pending revised p002 | Six SeaBIOS/OVMF positive and negative acceptance cells pass |

## Dependency and deferral rules

- p002 starts only after p001 proves that Variant does not alter source
  selection or loader/kernel binaries and the capacity selector is gone.
- p003 starts only after p002's independent layout checker accepts all three
  profiles, including the primary-only compact GPT contract.
- `ws020-p004` is not part of q036. It requires a user-operated Intel Mac
  boot of the frozen artifact after the automatic matrix passes.
- If actual UEFI/GPT requirements contradict the fixed primary-only contract,
  stop the affected Phase and request human judgment; do not silently add a
  compatibility MBR entry or backup GPT.
- An unrelated defect is returned to M/W/P planning and does not expand q036.

## Execution rules

- Do not inspect or consume `.internal/`.
- Keep the target framework generic; do not encode Intel-Mac policy into the
  architecture or kernel source lists.
- Build both BIOS and UEFI loader families for every amd64 PC/AT Variant.
- Use disposable writable image copies and `qemu-system-x86_64`; do not use
  aggregate `make check`.
- Run `make -j16`, focused WS020 fixtures, and `git diff --check`.
- After each Phase, synchronize Phase/WS/master/Queue state and commit `WIP`;
  push when permitted.

## Completion definition

q036 is finished when p001--p003 are each `completed` or `uncleared` with
their exact evidence and resume condition recorded. A finished q036 does not
complete WS020: p004's physical Intel Mac checkpoint and final five-run
acceptance remain explicit human work.
