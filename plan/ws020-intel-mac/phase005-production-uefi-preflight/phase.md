# WS020 Phase 005: production UEFI-only larger-media preflight

Last updated: 2026-08-31

WSID: `ws020`

Phase ID: `p005`

Combined ID: `ws020-p005`

Status: Completed (`q038`, 2026-08-31)

Parent: [WS020](../ws.md)

## Objective

Eliminate stale or intermediate Intel-Mac handoff artifacts. Produce one fresh
amd64 PC/AT UEFI-only image from the current tree, prove its exact production
layout and 60,549,120-sector USB behavior, and retain enough partition
publication diagnostics to classify any remaining physical-only failure.

This Phase does not implement the Apple `05ac:8406` internal SD-card reader.
The photographed boot device is the independent `30de:6544` USB Mass Storage
device registered as `sda`; failure to attach the unused internal reader is not
a prerequisite for booting zedBSD.

## Trigger and diagnosis

The Intel Mac observation used UUID `0B40-6EB0` and reported three GPT entries.
Those two facts identify an older Hybrid-family intermediate image, not the
current UEFI-only layout. The current UEFI-only contract has exactly two GPT
partitions and is exactly 202,392,064 bytes. The existing
`build/amd64/hdd-image.img` was also stale and failed the current production
checker, so neither file is a valid next handoff.

The photograph also showed no `sda partition N` publication after the GPT scan.
The old kernel discarded `partition_create_disk()` errors and later reduced the
symptom to a misleading boot UUID `ENOENT`. A same-byte, same-capacity QEMU
copy publishes all partitions and resolves the UUID, so the FAT serial and
selector syntax are not the direct failure.

## Work

1. Add bounded VFS diagnostics for the otherwise impossible cases where a
   scanned partition has zero blocks or cannot be published. Record the parent
   disk, one-based partition number, and exact publication errno without
   changing the successful path.
2. Leave the user's selected repository configuration unchanged. Use an
   explicit private amd64/PC-AT/UEFI configuration and a fresh private build to
   generate the production image.
3. Run the production UEFI checker before publication. Freeze the exact byte
   length, SHA-256, payload UUID, pure Protective MBR, primary GPT CRC and
   geometry, two active entries, final 33 zero sectors, absence of every BIOS
   loader, and embedded `zedbsd.cfg` agreement.
4. Copy the accepted source to a disposable sparse image and extend only that
   copy to 60,549,120 sectors, matching the observed USB capacity. Boot it once
   with OVMF, Q35, xHCI, and USB Mass Storage.
5. Require the larger-media diagnostic for logical last LBA 395,296 and
   physical last LBA 60,549,119; two partition publications; the exact payload
   FAT UUID resolving to `/dev/sda2`; rootfs/data overlay, swap, runtime
   filesystems, init, and login; and no GPT, VFS, storage, xHCI, panic, or fault
   failure.
6. Verify the pristine source hash after QEMU and atomically publish that exact
   checked source as `build/amd64/hdd-image.img`. Record its absolute path,
   length, and SHA-256 for the next single physical observation.

## Verification

- `MAC-T021` runs the fresh production checker and the single larger-media
  OVMF/Q35/xHCI USB cell from a WS-owned Noct runner.
- The existing partition publication host fixture passes normally, with
  sanitizers, and with the static analyzer.
- `make -j16` passes. Aggregate `make check` and `.internal/` are not used.
- `git diff --check` passes and the repository `config.mk` hash is unchanged.

## Completion conditions

The Phase completes automatically when a newly generated, production-checked
UEFI-only source passes `MAC-T021`, its source hash remains unchanged, and the
same bytes are published at `build/amd64/hdd-image.img` with a frozen identity.
No Intel Mac boot is required to complete this preflight Phase.

The next human action belongs to p004: boot that one linked image once to
confirm the corrected handoff and, if it still stops, return the new explicit
partition-publication errno. The final five-run physical campaign remains
deferred until all corrections and p003's independent strict matrix are
settled.

## Result

`MAC-T021` passed from a fresh private amd64/PC-AT/UEFI build. The production
checker accepted the fixed pure-Protective-MBR, two-partition, primary-only
source. A disposable copy extended from 395,297 to 60,549,120 sectors then
booted once with OVMF/Q35/xHCI and reached `login:`:

```text
usb-storage: sda blocks=60549120 block-size=512 ...
gpt: sda bounded extent accepted: logical-last=395296 physical-last=60549119 declared-sectors=395297 physical-sectors=60549120 ignored-tail-sectors=60153823
vfs: scan sda H/S=255/63 blocks=60549120: 2 entries
vfs: boot0 UUID=42E7-0E4C -> /dev/sda2 (private FAT)
vfs: root=overlay lower=boot0:rootfs.img upper=boot0:data.img
swap: active sources=1 total=16383 free=16383
init: system running
login:
```

The source and repository configuration hashes remained unchanged. The exact
checked source was then published to the ordinary build path:

| Property | Value |
| --- | --- |
| Image | `/home/awe/zedBSD/build/amd64/hdd-image.img` |
| Size | 202,392,064 bytes (395,297 sectors) |
| SHA-256 | `3bca88c3f5673f0b447cac4a7af457b8aba3a745861a005459f374adbe50dc79` |
| Payload UUID | `42E7-0E4C` |

The published file is byte-identical to the checked source and passes the
production checker again after publication. `make -j16`, the partition
publication ordinary/sanitizer/analyzer fixture, and `git diff --check` pass.
Evidence is retained under `../temp/p005-production-preflight-final-003/`.
