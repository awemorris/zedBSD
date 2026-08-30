# Queue: Generic bounded GPT and CF-SV7 USB-root continuation

Last updated: 2026-08-30

QID: `q034`

Queue status: finished

Queue finished: **Yes**

Authorization: the user approved implementation of `ws003-p021` on
2026-08-30 and clarified that a GPT-declared end before the physical disk end
must be accepted generally rather than only for zedBSD-marked images.

Timebox: no fixed wall-clock limit. Complete implementation and all automated
gates, then stop at one final Panasonic CF-SV7 physical observation.

Parent: [master plan](master.md)

Previous Queue: [q033](queue-q033.md)

## Purpose

Allow a fully coherent GPT to describe a logical disk extent smaller than the
physical device containing it. This makes raw-copied disk images boot from
larger USB media without modifying GPT at write time, while retaining strict
metadata, CRC, range, and no-MBR-fallback behavior.

## Execution registry

| Priority | WS / Phase | Authoritative document | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws003-p021` | [Phase](ws003-bringup/phase021-portable-gpt-image-extent/phase.md) | completed | Generic bounded GPT validation, automated regressions, and the final CF-SV7 boot of the frozen image all pass |

## Frozen execution boundary

- The GPT-declared last LBA may equal or precede the physical last LBA. A
  declared end beyond physical capacity is rejected.
- A bounded GPT must have valid and mutually consistent primary and backup
  copies at LBA 1 and the GPT-declared last LBA. Unlike canonical whole-device
  recovery, one damaged copy is insufficient to establish a shortened extent.
- Every header, entry array, usable range, and partition remains within the
  declared extent. Physical tail sectors are unallocated and never published.
- This is generic GPT behavior. Do not require zedBSD labels, partition types,
  hybrid-MBR entries, files, or other product markers.
- Preserve whole-device GPT degraded-copy recovery where the GPT extent is
  unambiguous, and preserve every q030 corruption rejection. A saturated PMBR
  above the 32-bit LBA boundary is not sufficient by itself to select a stale
  physical-tail backup. GPT evidence never falls back to legacy MBR.
- Do not resize, repair, relocate, or write GPT metadata during boot.
- Do not request another physical boot until parser, host fixtures, fresh
  production build, larger-media BIOS/UEFI boots, and ordinary regressions all
  pass against one frozen image.

## Execution rules

- Do not inspect or modify `.internal/` or `userland/noct/NoctLang`.
- Preserve unrelated work. Use `make -j16`, focused fixtures, and
  `qemu-system-x86_64`; do not use aggregate `make check`.
- Use only disposable sparse image copies for enlarged-media and malformed-GPT
  tests. Verify that the source production image hash does not change.
- Run the q030 GPT host regression, SeaBIOS q35/xHCI larger-media cell, OVMF
  q35/xHCI larger-media cell, ordinary BIOS USB gate, and BR-T24 OVMF
  4/8/16-GiB matrix.
- Synchronize actual results into P/W/M/Q. Commit `WIP` and push at the final
  automated handoff and after the physical result.
- A later independent failure becomes another Phase; do not silently expand
  q034 beyond GPT extent selection and CF-SV7 root continuity.

## Completion definition

q034 is finished when `ws003-p021` is either:

- `completed`, with all host/QEMU gates and one CF-SV7 boot resolving `boot0`,
  mounting the overlay, and reaching init/login; or
- `uncleared`, with all safe automated work complete, an exact residual
  decision or physical boundary recorded, and a reproducible resume condition.

The Queue may remain `in-progress` while its automated checkpoint awaits the
single physical observation. No intermediate or repeatability hardware boot
is authorized by this Queue.

## Automated checkpoint (2026-08-30)

At the automated checkpoint, the software batch was complete and q034 paused
at its one authorized physical observation. The frozen artifact was
`/home/awe/zedBSD/build/amd64/hdd-image.img`, 203,423,744 bytes, SHA-256
`6cf5fe81ce2695450a376e116b595291e5329e7c40fdc2820e2ebeb126732637`.

- The production GPT host fixture passes normally, with ASan/UBSan, and with
  the compiler analyzer. It includes 512/4096-byte sectors, PMBR saturation,
  physical truncation, both-copy corruption, cross-pointer and extent
  violations, stale physical-tail GPT data, and canonical recovery cases.
- A disposable copy sparsely enlarged from 397,312 to 60,549,120 sectors
  reaches `login:` through both SeaBIOS and OVMF q35/xHCI USB paths. Both
  report the selected logical end at LBA 397,311 and ignore 60,151,808 trailing
  sectors. The source image SHA-256 remains unchanged.
- The ordinary exact-size SeaBIOS USB gate reaches `login:` and the OVMF
  4/8/16-GiB matrix passes 3/3.
- The q030 malformed-GPT QEMU cell still rejects the GPT without publishing a
  partition or falling back to MBR, while the system continues to `login:`.
- `make -j16`, shell syntax, and `git diff --check` pass. The aggregate
  `make check` target was not used.

Evidence is preserved under `plan/ws003-bringup/temp/q034-final/`. The final
physical result is recorded below.

## Physical result and closure (2026-08-30)

The user reported that the exact frozen image boots successfully on the
Panasonic CF-SV7 and requested closure. This accepts the bounded GPT, USB-root,
overlay, and init/login continuation objective of `ws003-p021`. No residual
failure or follow-up Phase was extracted from q034.

q034 is finished by the successful branch of its completion definition:
`ws003-p021` is `completed` and all authorized Queue work is closed.
