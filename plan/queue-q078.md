# Queue: target regular-file UFS1 and ZEDSWAP2 formatters

Last updated: 2026-09-06

QID: `q078`

Queue status: finished

Queue finished: **Yes**

Parent: [master plan](master.md)

Previous Queue: [q077](queue-q077.md)

Authorization: the user requests returning to the original goal and continuing.
The master's recorded standing instruction authorizes repeated finite Queues
in dependency order. This Queue implements the existing p008/p009 exclusive
regular-file formatting contract. It does not restore the comprehensive
storage-snapshot proposal superseded by p002.

## Execution registry

| Priority | WS / Phase | Status | Purpose / dependency |
| --- | --- | --- | --- |
| 1 | [ws019-p008](ws019-installation/phase008-target-mkfs/phase.md) | uncleared | Implement target UFS1-in-file initializer and its descriptor reservation; p002 and q077 filesystem gates complete |
| 2 | [ws019-p009](ws019-installation/phase009-target-mkswap/phase.md) | uncleared | Implement production ZEDSWAP2-in-file initializer using the same reservation; no installer template |

These commands are independently testable prerequisites for p004. Installer
implementation and installed-system acceptance are outside this Queue;
boot-source provenance remains a p004 design dependency.

## Finite budget and verification

Implementation starts 2026-09-05 23:24 JST. Review after four active hours.
At most four new amd64 QEMU launches: up to two initial formatter acceptance
cells, and up to two further launches only after diagnosed in-scope corrections.
Each launch uses fresh disposable media/OVMF copies, with a 120-second boot
bound and a 600-second whole-cell bound. Record every launch and image digest.

Production-linked normal and ASan/UBSan formatter, reservation and fault
fixtures precede the amd64 `make -j16` build and runtime cells. Common
kernel/UAPI changes also require maintained i386 PC/AT and PC-98 builds and
fixed-width ABI checks. Never use aggregate `make check` or `.internal/`.

## Implementation decisions and remaining uncertainty

- P002's mount listing is diagnostic only. A descriptor-owned kernel backing
  reservation supplies exclusive mutation and blocks swap/loop activation;
  final close releases it. Other descriptors never inherit its write authority.
  Reopening read-only for verification retains the original reservation.
- V1 requires canonical FAT-backed regular-file identity and complete extent
  coverage. Unsupported containing filesystems fail before writes. This is
  the bounded capability needed by the existing-FAT installer.
- Idle descriptors may remain open; their writes fail while the reservation
  is held. Published shared mappings/cache objects and active mutations must
  be excluded atomically. Private read snapshots need no mutation authority.
  Do not infer exclusion from an advisory lock, mount listing or fuser scan.
  Reservation/mapping admission is the principal implementation uncertainty.
- UFS1 includes the maintained initial overlay journals and root marker.
  Shared production superblock decoding validates the reopened image;
  production-driver mount and overlay persistence validate the complete image.
- ZEDSWAP2 stores a 64-byte header and page-aligned data slots. Its allocation
  bitmap lives in memory; no on-disk slot map or new format is introduced.
- Writes are confined to the supplied pre-sized file; neither command creates
  or resizes it. No real media, partition formatting, table writes, active
  data/swap copying, or firmware-variable operations are selected.

Preserve all inherited worktree changes. Record results in the owning P/W/M
books and reusable fixtures under WS019 before closing the Queue.

## Runtime correction after launch three

Launches 1--3 failed before either formatter ran: creation helper assumptions
were corrected, then target FAT allocation of a 32-MiB file exceeded the
120-second command bound while making progress. The final remaining launch
uses host-preallocated all-zero regular files; no formatted data/swap template
is supplied. File creation performance remains an installer prerequisite.

The formatters now initialize only reserved metadata and allocated contents.
UFS1 zeros each newly allocated block in the production driver; the pager
writes complete swap pages before publishing them. Nonzero-input host tests
verify defined contents and preservation of unused areas. Formatting is not
an erasure operation.

The last cell starts from the existing test rootfs on native `sdb1`, formats
the blank NVMe files, and changes only its disposable boot configuration from
the guest. Two reboots in that same QEMU process check generated overlay/swap
startup and persistence. Native initial root avoids the active FAT backing
claims that correctly prevent another RW mount of the boot payload volume.
The four-launch and 600-second whole-cell limits are unchanged.

## Closure

All four launches are consumed. Launch four passed every guest format,
refusal, swap activation/deactivation, and two-reboot persistence assertion.
Its final host provenance guard failed because a concurrently invoked
`make MACHINE=pc98` ignored that target selection and regenerated the amd64
source image. The guest used only disposable copies. This cycle retains
p008/p009 as uncleared until a single fully isolated acceptance cell passes.
The next finite q079 repeats that gate after correctly configured builds end;
no target implementation change is required.

[Detailed q078 evidence](ws019-installation/tests/q078-results.md).
