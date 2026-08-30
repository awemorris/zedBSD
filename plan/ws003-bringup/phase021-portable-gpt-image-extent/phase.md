# WS003 Phase 021: portable GPT image extent on larger USB media

Last updated: 2026-08-30

Phase ID: `ws003-p021`

Status: Planned; not yet assigned to a Queue

Parent: [WS003](../ws.md)

Tests: [WS003 test index](../tests/README.md), especially `BR-T53`

## Objective

Make the ordinary amd64 `hdd-image.img` boot after it has been copied byte for
byte to a larger USB device. A coherent GPT contained in the original image
must remain bounded by that image's declared GPT extent; unused physical media
after that extent must neither invalidate the image nor become part of a
published partition.

The Panasonic CF-SV7 acceptance result must resolve the configured `boot0`
UUID, mount `rootfs.img` plus `data.img`, and reach init/login. This Phase does
not resize a partition, consume trailing capacity, repair GPT in place, or
write partition metadata during boot.

## Captured boundary

The single q033 physical observation cleared `ws003-p020`: the CF-SV7 passed
IRQ, XMM, HAL, xHCI, USB enumeration, and USB-storage capacity discovery. It
then stopped at:

```text
usb-storage: sda blocks=60549120 block-size=512
gpt: sda rejected: invalid protective MBR (3)
vfs: scan sda H/S=255/63 blocks=60549120: -3 entries
vfs: boot0 selector resolution failed (error 6)
VFS initialization failed (6); entering idle.
```

The frozen q033 image is 203,423,744 bytes, or 397,312 logical 512-byte
sectors. Its internally consistent GPT geometry is:

| Field | Image value | CF-SV7 USB value |
| --- | ---: | ---: |
| Physical/logical sector count | 397,312 | 60,549,120 |
| Protective-MBR start/size | 1 / 397,311 | unchanged after raw copy |
| Primary-header alternate LBA | 397,311 | unchanged after raw copy |
| Backup-header LBA | 397,311 | unchanged after raw copy |
| Physical last LBA | 397,311 | 60,549,119 |

zedBSD's errno value 3 is `EINVAL`. The first rejection is exact:
`canonical_protective_mbr()` currently requires the protective entry size to
equal the physical device's `d_block_count - 1`. Relaxing only that comparison
would be incomplete: `validate_copy()` also requires the primary alternate
header at the physical last LBA and `gpt_scan()` reads the backup from that
physical last LBA.

This is a contract conflict between a valid fixed-size zedBSD GPT image copied
onto larger media and the q030 whole-device strict-GPT assumption. It is not
an xHCI, USB BOT, capacity, UUID, or FAT failure.

## Reproduction

The physical geometry was reproduced without hardware by copying the frozen
image, sparsely extending the copy to 60,549,120 sectors, and booting that copy
through the existing SeaBIOS q35/xHCI USB-only runner. It discovered exactly
`60549120` blocks and failed with the same protective-MBR `EINVAL` and VFS
error 6. The pristine exact-size image continues to reach `login:`.

Phase implementation must turn this observation into a maintained `BR-T53`
test rather than relying on the one-off `/tmp` artifact.

## Fixed safety contract

1. Keep the existing strict whole-device GPT path unchanged when the
   protective entry covers `min(physical-last-LBA, UINT32_MAX)`.
2. Recognize a smaller declared image extent only as the existing zedBSD
   hybrid raw-image profile. The unique inactive protective entry must start
   at LBA 1, have a nonzero unsaturated size smaller than the physical media,
   and end within the physical device. The GPT must contain the BIOS-loader
   partition type used by the image builder, and the active compatibility-MBR
   FAT entry must exactly mirror the GPT payload partition's start and length.
   Other shortened GPT media retain the strict whole-device rejection.
3. In smaller-image mode, validate the primary header at LBA 1 and the backup
   header at the protective entry's declared last LBA. Both copies and both
   entry arrays are mandatory, CRC-valid, role-correct, byte-identical where
   required, and mutually point to LBA 1 and the declared last LBA. The
   degraded single-copy recovery accepted for a canonical whole-device GPT is
   not accepted in this mode.
4. Validate header, table, usable range, and every partition against the
   declared image extent as well as the physical I/O bounds. No published
   extent may enter the trailing physical area.
5. Treat every sector after the declared image extent as outside the zedBSD
   image. Do not scan or publish stale partitions from that tail. A GPT
   signature at the physical last LBA may be diagnosed as ignored stale tail
   metadata, but it cannot override or invalidate a fully verified zedBSD
   bounded-image pair; raw-copying over previously partitioned USB media must
   not require the user to erase the otherwise unused tail first.
6. Keep GPT authoritative once an `0xee` entry or GPT signature is present.
   No malformed image may fall back to the active compatibility MBR entry.
7. Do not write, relocate, or repair either GPT copy at boot. Log the declared
   sector count, physical sector count, and ignored trailing sector count once.
8. Preserve the q030 corruption policy. This Phase adds one narrowly bounded
   portable-image form; it does not generally accept stale, truncated, or
   contradictory GPT metadata.

An `UINT32_MAX` protective-entry sentinel cannot define a smaller-image end;
it retains the existing whole-device semantics for media above 2 TiB.

## Implementation plan

1. Separate physical I/O capacity from GPT validation extent in
   `src/drivers/disklabel/gpt.c`.
2. Parse and classify the protective entry before selecting the backup-header
   LBA. Preserve the current canonical path and introduce the bounded
   smaller-image path described above.
3. Pass the selected GPT extent explicitly through header layout, copy, table,
   and partition validation. Physical bounds remain the outer I/O guard.
4. Add a distinct diagnostic for accepted trailing media and reason-specific
   rejection diagnostics sufficient to distinguish PMBR, declared extent,
   physical-last-LBA conflict, and primary/backup inconsistency.
5. Extend the production GPT host fixture and create the sparse-larger-media
   QEMU cell. Re-run all q030 strict-GPT malformed cases.
6. Run the ordinary amd64 BIOS USB and OVMF 4/8/16-GiB USB gates from a fresh
   build. Then freeze one image for one CF-SV7 observation.
7. If the CF-SV7 reaches a later boundary, record that boundary in a new Phase;
   do not expand this Phase into unrelated USB, FAT, overlay, or init work.

## Verification matrix

- canonical exact-size GPT: unchanged success;
- coherent image GPT sparsely extended to the photographed 60,549,120-sector
  capacity: success to `login:` in QEMU;
- coherent image GPT with nonzero trailing bytes but no second GPT: success;
- smaller image with missing/damaged backup, CRC mismatch, cross-pointer
  mismatch, partition crossing the declared extent, protective-size mismatch,
  or alternate LBA outside physical media: rejection with no publication;
- an otherwise valid generic shortened GPT without the zedBSD hybrid-image
  markers: rejection; stale GPT metadata at the physical last LBA of a valid
  zedBSD bounded image is ignored and never published;
- q030 primary-only/backup-only whole-device recovery and all malformed-GPT
  rejection fixtures: unchanged results;
- production amd64 BIOS USB and OVMF q35/xHCI 4/8/16-GiB boots: `login:`;
- one final CF-SV7 boot of the identified image: configured UUID resolves,
  the writable overlay mounts, and init reaches `login:`.

## Completion conditions

- A raw copy of the standard image boots from a larger USB device without any
  post-copy partition-table rewrite.
- Trailing physical sectors are ignored and never published through a
  partition from the bounded image GPT.
- Canonical whole-device GPT and all strict corruption/ambiguity rejection
  behavior remain passing.
- The single consolidated physical observation reaches at least init/login;
  any subsequent hardware boundary is recorded separately.
- `make -j16`, focused host fixtures, the declared QEMU gates, and
  `git diff --check` pass. The aggregate `make check` target is not used.

## Reconsideration boundary

Return for human review if the implementation cannot distinguish the bounded
image form from contradictory whole-device GPT without weakening q030, if
firmware itself requires GPT relocation before loading zedBSD, or if consuming
trailing capacity becomes a requirement. Destructive repair and expansion
belong to `diskpart`/installer work, not this boot-time Phase.
