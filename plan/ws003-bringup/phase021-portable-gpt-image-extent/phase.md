# WS003 Phase 021: portable GPT image extent on larger USB media

Last updated: 2026-08-30

Phase ID: `ws003-p021`

Status: Completed (`q034`, 2026-08-30)

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

1. Keep the existing strict whole-device recovery path when the validated
   GPT-declared last LBA equals the physical last LBA. A saturated protective
   entry alone does not distinguish a bounded layout from a whole-device one.
2. Permit this as a general GPT geometry, not a zedBSD-image exception. The
   unique inactive protective entry must start at LBA 1 and the GPT-declared
   last LBA must be less than or equal to the physical last LBA. A declared
   end beyond the physical device is always truncation and is rejected.
3. In bounded mode, validate the primary header at LBA 1 and the backup
   header at the protective entry's declared last LBA. Both copies and both
   entry arrays are mandatory, CRC-valid, role-correct, byte-identical where
   required, and mutually point to LBA 1 and the declared last LBA. The
   degraded single-copy recovery accepted for a canonical whole-device GPT is
   not accepted in this mode.
4. Validate header, table, usable range, and every partition against the
   declared GPT extent as well as the physical I/O bounds. No published
   extent may enter the trailing physical area.
5. Treat every sector after the GPT-declared extent as unallocated tail space.
   Do not scan or publish stale partitions from that tail. A GPT signature at
   the physical last LBA may be diagnosed as ignored stale metadata, but it
   cannot override or invalidate a fully verified bounded GPT pair.
6. Keep GPT authoritative once an `0xee` entry or GPT signature is present.
   No malformed image may fall back to the active compatibility MBR entry.
7. Do not write, relocate, or repair either GPT copy at boot. Log the declared
   sector count, physical sector count, and ignored trailing sector count once.
8. Preserve the q030 corruption policy. This Phase accepts a fully coherent
   GPT on a larger physical device; it does not accept stale, truncated, or
   contradictory metadata within the GPT-declared extent.

This is a zedBSD read-only tolerance extension to the usual whole-device GPT
layout, whose backup header is normally at the current physical last LBA.
“General” here means that acceptance is based only on coherent GPT metadata
and not on zedBSD labels, GUIDs, files, or MBR markers. zedBSD does not claim
that external partitioning tools must preserve the unused physical tail.

Let `S` be the protective entry's `SizeInLBA`. When `S < UINT32_MAX`, `S`
directly supplies the candidate GPT last LBA regardless of physical-disk size.
Only `S == UINT32_MAX` with a physical last LBA above that value is ambiguous;
a CRC-valid primary header must then supply the candidate through its alternate
LBA before either backup location or entry array is trusted. In all cases the
final relation is `S == min(GPT-last-LBA, UINT32_MAX)`. If the ambiguous header
cannot establish the extent, reject rather than risk selecting stale GPT
metadata from the physical tail. The saturation threshold is 2 TiB for
512-byte logical blocks and 16 TiB for 4096-byte logical blocks.

## Implementation plan

1. Separate physical I/O capacity from GPT validation extent in
   `src/drivers/disklabel/gpt.c`.
2. Parse and classify the protective entry before selecting the backup-header
   LBA. Preserve the current canonical path and introduce the bounded GPT path
   described above.
3. Pass the selected GPT extent explicitly through header layout, copy, table,
   and partition validation. Physical bounds remain the outer I/O guard.
4. Add a distinct diagnostic for accepted trailing media and reason-specific
   rejection diagnostics sufficient to distinguish PMBR, declared extent,
   physical-capacity conflict, and primary/backup inconsistency.
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
- coherent generic GPT with nonzero trailing bytes: success;
- smaller image with missing/damaged backup, CRC mismatch, cross-pointer
  mismatch, partition crossing the declared extent, protective-size mismatch,
  or alternate LBA outside physical media: rejection with no publication;
- coherent non-zedBSD GPT copied onto larger media: success; stale GPT metadata
  at the physical last LBA is ignored and never published;
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

## Automated checkpoint (2026-08-30)

The implementation and every automated gate are complete. The frozen physical
handoff artifact is:

| Property | Value |
| --- | --- |
| Image | `/home/awe/zedBSD/build/amd64/hdd-image.img` |
| Size | 203,423,744 bytes (397,312 sectors) |
| SHA-256 | `6cf5fe81ce2695450a376e116b595291e5329e7c40fdc2820e2ebeb126732637` |

Implemented behavior:

- A non-saturated protective-MBR size fixes the GPT logical last LBA even
  when the physical device is larger. A physical device smaller than that
  declaration is rejected.
- Only the saturated-PMBR case above the 32-bit LBA boundary uses a CRC-valid
  primary header to establish the logical end. If that extent cannot be
  established unambiguously, zedBSD rejects stale physical-tail metadata.
- A bounded layout requires complete, CRC-valid, mutually consistent primary
  and backup headers and entry arrays. All metadata, usable ranges, and
  partitions are checked against the logical end; the physical tail is never
  scanned or published.
- Exact whole-device layouts retain the q030 read-only single-copy recovery
  behavior where the extent remains unambiguous. Boot never rewrites GPT.

Automated evidence:

| Gate | Result | Preserved evidence |
| --- | --- | --- |
| GPT host fixture, ordinary + ASan/UBSan + analyzer | PASS | `tests/run-gpt-host-test.sh` |
| 60,549,120-sector SeaBIOS q35/xHCI USB copy | PASS to `login:` | `temp/q034-final/br-t53/seabios/` |
| 60,549,120-sector OVMF q35/xHCI USB copy | PASS to `login:` | `temp/q034-final/br-t53/ovmf/` |
| Exact-size SeaBIOS q35/xHCI USB | PASS to `login:` | `temp/q034-final/bios-exact/` |
| Exact-size OVMF q35/xHCI USB, 4/8/16 GiB | PASS 3/3 | `temp/q034-final/br-t24/` |
| q030 malformed-GPT QEMU rejection | PASS | `temp/q034-final/gpt-negative/` |
| `make -j16`, shell syntax, `git diff --check` | PASS | current worktree |

The larger-media guest diagnostic is:

```text
gpt: sda bounded extent accepted: logical-last=397311 physical-last=60549119 declared-sectors=397312 physical-sectors=60549120 ignored-tail-sectors=60151808
```

The disposable sparse image reaches UUID resolution for `/dev/sda2`, mounts
`boot0:rootfs.img` plus `boot0:data.img`, mounts runtime filesystems, and
reaches `login:` under both firmware paths. The pristine source hash is
unchanged.

## Physical handoff (one observation)

Boot the exact frozen image above once on the Panasonic CF-SV7. The purpose is
only to confirm that the photographed 60,549,120-sector USB device now accepts
the bounded GPT, resolves `boot0`, mounts the overlay, and reaches init/login.
A photograph of the final screen is sufficient. Repeated boots are reserved
for later WS-level acceptance and are not part of this checkpoint.

## Physical result (2026-08-30)

The user reported that the frozen image identified above boots successfully on
the Panasonic CF-SV7 and requested that the issue be closed. The result is
accepted as the single planned `BR-T53` physical PASS: the larger physical USB
medium no longer stops at protective-MBR validation and the USB-root overlay
continues through init/login.

Together with the preserved host, malformed-GPT, SeaBIOS, OVMF, exact-size,
and 4/8/16-GiB evidence, this satisfies every Phase completion condition.
There is no residual p021 blocker and no follow-up Phase is required for this
issue.

## Reconsideration boundary

Return for human review if a primary and its declared-end backup cannot define
a safe validation extent without weakening q030 corruption checks, if firmware
itself requires GPT relocation before loading zedBSD, or if consuming trailing
capacity becomes a requirement. Destructive repair and expansion belong to
`diskpart`/installer work, not this boot-time Phase.
