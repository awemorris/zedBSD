# WS018 Phase 004: filesystem-owned identity probes

Last updated: 2026-08-28

WSID: `ws018`

Phase ID: `p004`

Combined ID: `ws018-p004`

Status: Complete (`q025`)

Parent: [WS018](../ws.md)

Tests: [WS018 test index](../tests/README.md)

## Objective

Remove FAT- and UFS-specific on-disk parsing from `block-identity.c`.  Extend
the stable filesystem-driver interface with an identity callback so each
filesystem owns recognition, UUID, and label decoding while generic block,
partition, selector, cache, and swap policy remain in kernel core.

## Entry evidence

- `struct filesystem_type` already owns a mount-recognition `probe` callback,
  but has no callback that returns filesystem identity metadata.
- `block-identity.c` currently duplicates FAT BPB offsets and UFS superblock
  offsets/endian decoding in addition to generic selector, duplicate-match,
  partition identity, cache, and swap handling.
- FAT, UFS1, and UFS2 already parse the same format metadata for mount/probe
  purposes.  Keeping a second parser in block identity permits the two paths
  to drift.
- The VFS filesystem registry is private to `mount.c`, and filesystems are
  registered before boot-source UUID/label resolution begins.

## Fixed interface and return contract

- Add this significant, deliberate member to `struct filesystem_type`:

  ```c
  int (*identify)(struct disk *, struct block_identity *);
  ```

- `0` means that the filesystem recognized the disk and filled only
  `ZEDBSD_BLKID_TYPE`, `ZEDBSD_BLKID_UUID`, and/or `ZEDBSD_BLKID_LABEL` fields.
  `EOPNOTSUPP` means format mismatch.  Other errno values report a real bounded
  metadata-read or validation failure.
- A filesystem without identity metadata may leave the callback `NULL`.
  Nodev filesystems are never asked to identify a disk.
- `probe` remains the mountability callback.  A driver should share one
  private format decoder between its `probe`, mount, and `identify` paths, but
  no decoder is shared across different filesystem drivers.
- Add one registry dispatcher owned by the filesystem/VFS core; callers do not
  gain direct access to the registry array.  Multiple successful filesystem
  identities for one disk are an ambiguity error rather than registration-
  order precedence.

## Fixed ownership and behavior

- FAT owns BPB/type, serial UUID, and volume-label decoding.  The callback may
  first live in the current FAT VFS adapter; p010/p011 must preserve it while
  consolidating and replacing FAT internals.
- UFS1 and UFS2 each own their own superblock-offset, endian, fs-id, and volume
  name decoding after p003.  Neither identity callback calls the other.
- `block-identity.c` retains identity caching, `/dev/name` and
  `UUID`/`LABEL`/`PARTUUID`/`PARTLABEL` selector resolution, case-folded
  matching, and the existing duplicate-device `EEXIST` result.
- Partition parsers remain the sole owners of PARTUUID/PARTLABEL.  Those fields
  may coexist with filesystem identity on a partition and are merged by core
  code, never written by a filesystem callback.
- Swap remains a block format rather than a filesystem.  Swap header parsing,
  UUID, and label remain generic swap/block-identity code and do not enter a
  filesystem callback.
- If filesystem and swap recognizers both claim one malformed/hybrid disk, the
  result is an explicit ambiguity error; no silent probe-order winner is used.
- Identity callbacks perform bounded metadata reads only.  They do not mount,
  allocate persistent inode state, publish devices, or mutate the disk.

## Implementation procedure

1. Extend the filesystem contract and implement a locked registry dispatcher
   without exposing the registry array or changing mount probe semantics.
2. Add focused callback/dispatcher tests for NULL callbacks, mismatch, hard
   I/O error, one match, multiple matches, and forbidden flag output.
3. Move FAT identity parsing beside the current FAT format decoder and reuse
   driver-private validation rather than retaining BPB offsets in core.
4. Move UFS1 and UFS2 identity parsing into their independent owners, using
   their respective private endian/superblock logic.
5. Reduce `block-identity.c` to generic composition, cache, swap, partition,
   and selector behavior.  Validate callback output before merging it so a
   faulty driver cannot overwrite PARTUUID/PARTLABEL or cached state.
6. Exercise selectors and boot sources using valid, corrupt, absent, duplicate,
   and mixed partition/filesystem identities, then run supported builds.

## Verification

- `KA-T030`: FAT12/16/32 and little-/big-endian UFS1/UFS2 images resolve their
  expected type, UUID, and label through registered callbacks.  Missing,
  truncated, corrupt, hard-error, and duplicate-selector cases retain bounded,
  deterministic errors.
- `KA-T031`: PARTUUID/PARTLABEL still originate in partition metadata, swap
  identity remains available without a filesystem callback, and a source
  audit finds no FAT/UFS format offsets or magic parsing in
  `block-identity.c`.
- Verify cache repeatability and invalidation behavior and confirm that
  ordinary auto-mount probing is unchanged.
- Run the accepted boot-source UUID/label fixtures, `make -j16`, and
  `git diff --check`.  Do not run `make check` or consume `.internal/`.

## Completion conditions

- The filesystem interface has one documented identity callback with the
  fixed return/field-ownership contract and a registry-owned dispatcher.
- FAT, UFS1, and UFS2 own all of their identity format parsing; core contains
  none of their offsets or magic decoding.
- Partition and swap identity remain generic, selector ambiguity behavior is
  preserved or made explicitly stricter for hybrid formats, and boot UUID/
  label selection still works.
- `KA-T030`, `KA-T031`, boot-source regression gates, `make -j16`, and
  whitespace checks pass.

## Dependencies

- `ws018-p001` establishes the canonical driver source root.
- `ws018-p003` establishes independent UFS1/UFS2 owners.

This Phase deliberately does not depend on FAT native-VFS work.  It may add
the callback to the current FAT adapter; `ws018-p010` and `ws018-p011` must
preserve the callback and its verified behavior.  This avoids a dependency
cycle and makes p004 a prerequisite of p011.

## Reconsideration boundary

Stop for human review if identity requires mounting a filesystem, if a callback
must modify partition or swap metadata, if two legitimate filesystem formats
must intentionally claim one disk, or if the new callback cannot coexist with
the stable mount `probe` contract.  Do not expose the registry array or restore
filesystem-specific parsing to generic core as a shortcut.

## Execution result

Completed on 2026-08-28 without reaching the reconsideration boundary.

- `struct filesystem_type` now has the fixed `identify` callback and VFS owns
  `filesystem_identify()`.  The dispatcher snapshots at most eight registered
  non-nodev callbacks under the namespace lock, performs metadata I/O after
  releasing the lock, validates every returned field, accepts one match, and
  rejects multiple matches with `EEXIST`.
- FAT, UFS1, and UFS2 each own their TYPE/UUID/LABEL decoding.  Identity reads
  use direct disk I/O, FAT's former shared BPB buffer is caller-local, and UFS1
  and UFS2 retain independent private endian/superblock paths.
- `block-identity.c` now contains only generic direct-read composition,
  partition identity, swap identity, selector ambiguity/error policy, and the
  existing disk-lifetime cache.  PARTUUID/PARTLABEL resolution reads only
  partition metadata and never invokes filesystem callbacks.
- Corruption tests found and fixed two boundedness defects: UFS1 validated a
  zero `inopb` only after a derived division, and FAT identity did not reject a
  BPB whose declared size exceeded the disk.  Both cases are now maintained
  regressions.
- KA-T030/KA-T031 passed 110 checks covering dispatcher validation, FAT12/16/32,
  little-/big-endian UFS1/UFS2, UUID/LABEL and PARTUUID/PARTLABEL selectors,
  duplicate and hard-error candidates, standalone swap, filesystem/swap
  hybrids, and cache lifetime.  The source audit found no FAT/UFS parser in
  generic block identity.  KA-T020 and KA-T021 also remained green.
- Fresh builds and architecture checkers passed for amd64, i386 PC/AT, i386
  PC-98, arm64/RPi4, sparcv9/sun4u, and m68k/X68k.  The BR-T44 boot-source host
  fixture and the normal `make -j16` and whitespace gates passed.
- Post-fix production boots passed for default i386 PC/AT, i386 PC-98, amd64
  BIOS, and amd64 UEFI.  UUID cross-boot passed on both amd64 firmware paths.
  Evidence is below `plan/ws018-kernel-architecture/temp/p004-br-t46-*`.

The first post-fix aggregate attempt passed PC/AT and PC-98, then one amd64
BIOS cell hit an unrelated intermittent ATA write failure (`op=2 count=0`)
during overlay-data cleanup.  The same final source passed a bounded standalone
amd64 BIOS retry, and amd64 UEFI passed separately; no identity/UFS decode
failure appeared in that log.  This observation is retained rather than
misreported as one uninterrupted 4/4 run.
