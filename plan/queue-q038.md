# Queue: Intel Mac production UEFI preflight

Last updated: 2026-08-31

QID: `q038`

Archive record: closed Queue Book retained after q039 was opened.

Queue status: finished

Queue finished: **Yes**

Authorization: on 2026-08-31 the user supplied the Intel Mac physical log and
explicitly requested a correction while independently checking PC-98. The user
then classified the unrelated Apple USB device as deferrable.

Timebox: none. This Queue contains one bounded automatic Phase. It ends with a
fresh frozen image ready for one later user-operated Intel Mac observation.

Parent: [master plan](master.md)

Previous Queue: [q037](queue-q037.md)

## Purpose

Replace every stale or intermediate Intel Mac handoff with a freshly generated
and checked UEFI-only artifact. Prove the exact observed larger-USB path in
QEMU and make any remaining physical partition-publication failure explicit.

## Execution registry

| Priority | WS / Phase | Authoritative document | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws020-p005` | [Phase](ws020-intel-mac/phase005-production-uefi-preflight/phase.md) | completed | Fresh production UEFI-only image passes its checker and the 60,549,120-sector OVMF/xHCI USB boot, then is frozen for p004 |

## Fixed boundaries

- Do not implement or require the Apple `05ac:8406` internal SD-card reader;
  the independent boot USB already registers as `sda`.
- Do not repair, relocate, or expand GPT on the source image or disposable
  larger medium. Do not add a BIOS path to UEFI-only.
- Do not weaken p003's separate six-cell/login contract or mark p004 complete.
- Do not consume `.internal/` or run aggregate `make check`.
- Use `make -j16`, the production image checker, focused partition tests, and
  one OVMF/Q35/xHCI larger-media cell.
- Modify only a disposable sparse copy during runtime testing and prove the
  pristine source hash remains unchanged.
- Commit and push after the Phase reaches completed or uncleared state. If
  push is unavailable, preserve the local commit and continue reporting it.

## Completion definition

q038 is finished when `ws020-p005` is completed, or uncleared with the exact
diagnostic and a concrete resume condition recorded.

## Result

Source/photo comparison proved that the first physical log used an older
three-partition Hybrid-family artifact. `MAC-T021` then built a fresh fixed
UEFI-only source with exactly two partitions, passed the production checker,
and booted a disposable 60,549,120-sector copy through OVMF/Q35/xHCI to UUID
resolution, overlay root, swap, init, and login.

The checked source was published byte-for-byte as
`/home/awe/zedBSD/build/amd64/hdd-image.img`, 202,392,064 bytes, SHA-256
`3bca88c3f5673f0b447cac4a7af457b8aba3a745861a005459f374adbe50dc79`.
Partition publication failures now report their exact partition and errno.
The unrelated Apple `05ac:8406` internal reader remains intentionally deferred.

q038 is finished by the successful branch of its completion definition. One
provisional p004 Intel Mac boot of only this exact artifact is the next human
checkpoint; final repetition remains later.
