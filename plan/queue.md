# Queue proposal: read-only storage administration

Last updated: 2026-09-05

QID: `q076`

Queue status: proposed; awaiting execution approval

Queue finished: **No**

Authorization: the user requested extraction of the next work item. This
authorizes planning only; implementation, builds, and QEMU await approval.
Standing permission permits a planning-only `WIP` commit and push.

Parent: [master plan](master.md)

Previous Queue: [q075](queue-q075.md)

## Purpose

Provide the read-only storage snapshot needed before implementing diskpart
and the existing-FAT installer. Q075 completed WS011's automatic prerequisite;
its physical follow-up remains separate and does not block this next wave.

## Execution registry

| Priority | WS / Phase | Status | Purpose / dependency |
| --- | --- | --- | --- |
| 1 | [`ws019-p002`](ws019-installation/phase002-readonly-block-gpt-administration/phase.md) | pending | Versioned disk/GPT/filesystem/use-state queries with bounded enumeration and change detection; depends on completed WS004 p024 and existing disk/mount/swap/boot-source foundations |

## Why this is next

- WS019 is next in the master's dependency-ready order. P003 diskpart and the
  target formatters/installer need reliable identity and current-use evidence.
- Existing disk registry and blkid queries do not compose parent, full GPT
  metadata, boot-source/use state, and change detection into one snapshot.
- The GPT parser validates type GUIDs but does not retain them in published
  partition records. The full GPT label exceeds the old blkid text field.
- WS011 p008 still needs physical transport/topology choices; no remote
  service or physical test is part of this Queue.

## Proposed timebox and execution boundary

- One Phase; estimated 2--3 hours of active work, reviewed at three hours.
  Approve this proposed timebox together with the Queue.
- Implement only read-only kernel/UAPI queries, narrow metadata retention and
  lifetime/change tracking, and phase-owned fixtures.
- Host ordinary/sanitizer/fault/ABI gates first, then maintained amd64/i386
  builds with `make -j16`; no aggregate `make check`.
- At most one amd64 OVMF/QEMU NVMe query cell: 120-second boot, 300-second
  whole-cell bound. Use a disposable boot image and a separate host-prepared
  GPT namespace containing one ESP and one FAT32 payload. Guest queries must
  not write the target; verify its complete digest before/after.
- Exclude p003 diskpart, mkfs/mkswap, zedinst, GPT writing, rescan, mount/unmount
  commands, destructive claims, and all physical disk operations.
- Stop uncleared if safe snapshots or truthful boot-source reporting requires
  a broader lifecycle/loader redesign. Preserve evidence rather than silently
  widening this Queue or repeating a failed runtime cell.

## Important uncertainty

A snapshot describes a checked instant; it cannot freeze a disk until a later
open or write. P002 must expose incarnation/change information and a read-only
way to revalidate the opened object. Later installation-time exclusion is not
provided by this query. The selected boot/config FAT (`boot0`) must not be
confused with the firmware-loaded ESP; absent provenance is not inferred from
enumeration order or the current root.

## Approval boundary

Approve q076 to begin this one Phase under the proposed timebox and one-cell
limit. P003 and the rest of WS019 require later Queue selection.
