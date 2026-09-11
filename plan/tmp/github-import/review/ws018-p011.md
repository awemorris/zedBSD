# WS018 Phase 011: FAT native VFS migration

Last updated: 2026-08-30

WSID: `ws018`

Phase ID: `p011`

Combined ID: `ws018-p011`

Status: Complete (`q035`)

Parent: [WS018](../ws.md)

Tests: [WS018 test index](../tests/README.md)

## Objective

Replace the FAT driver's embedded legacy `bootfs` engine/adapter with a direct
implementation of the ordinary `filesystem_type`, `mount`, `inode`, and
`file` contracts.  Preserve FAT12/16/32, LFN and mutation behavior while making
boot media, loop-backed root/data images, runtime boot-slot paths, and
file-backed swap use the same native VFS path as every other filesystem.

## Preconditions and dependencies

- `ws018-p010` supplies one mechanically verified `fat.c` and one FAT header.
- `ws018-p004` supplies the filesystem-owned probe/identity callback; the
  native implementation must preserve it and must not reintroduce raw FAT
  parsing into `block-identity.c`.
- Retain the p010 behavior baseline and add fault/ownership coverage before
  removing the compatibility structures.
- This Phase does not delete generic `fs.c`, `namespace.c`, or `internal.h`;
  p012 removes them only after the final caller audit.

## Target private model

The FAT driver directly owns:

- a mount-private state containing the referenced `struct disk`, mount flags,
  decoded BPB/FAT geometry, sector cache and dirty state, allocation hint,
  metadata table, and mount mutex;
- an inode extension containing first cluster, directory-entry identity,
  attributes, orphan state, and any canonical-path state still required by
  the current namei model; and
- a file-private state containing the open file's cluster/directory position
  and owner relation required for coherent writes and rename.

Low-level helpers accept these FAT-private types and return errno-style
results.  They call `disk_read`, `disk_write_filesystem`, and `disk_sync`
through the mounted disk directly.  No FAT-private structure embeds, aliases,
or emulates `struct boot_volume`, `struct bootfs`, `struct bootfs_file`, or a
`bootfs_driver` table.

## Work packages

### 1. Convert mount, probe, and sector/cache ownership

Move BPB decoding and FAT12/16/32 selection behind the existing FAT
`filesystem_type` probe/identify operations.  Initialize the private mount
state directly from `mount->m_disk` and flags.  Preserve 512-byte physical
block scaling for larger BPB logical sectors, read-only enforcement, all FAT
copy updates, dirty-sector flush ordering, mount-failure cleanup, and private
mount support.

Replace the generic `bootfs_result` adapter with checked errno conversion at
the low-level boundary.  Preserve the currently observable errno for corrupt,
unsupported, absent, read-only, no-space, type, and I/O failures.

### 2. Convert path and inode operations

Make lookup/casefold, create, mkdir, unlink, rmdir, rename, getattr/setattr,
truncate, readdir, reclaim, and root construction call FAT-private directory
and chain routines directly.  Retain:

- LFN validation and SFN fallback/collision rules;
- stable directory-entry-derived inode/backing identity;
- namecache invalidation and canonical descendant repathing;
- replacement/orphan lifetime and delayed cluster reclamation;
- open-writer authority across rename;
- root and regular-file type/mode defaults and `etc/unixmode` metadata; and
- checked FAT time conversion and its overflow behavior.

No VFS operation constructs a temporary `bootfs_file` or converts through a
legacy directory-entry structure after this package.

### 3. Convert file I/O and mutation

Make open/close/read/pread/write/pwrite/fsync/truncate and allocation operate
on the FAT inode/file state directly.  Preserve append serialization, short
read/write rules, offset/size limits, cluster-chain validation, zeroing,
rollback on partial allocation or directory mutation, propagation to all open
handles, read-only mounts, and disk synchronization.

Keep private helpers `static` and use `fat_` names for family-wide behavior;
retain `fat12_`, `fat16_`, or `fat32_` only where the algorithm genuinely
differs.  Preserve identifiers already recorded by p006 in the stable public
boot ledger; this Phase renames implementation-private `bootfat_*` operations,
not an unrelated public API.  Do not expose private engine structures.

### 4. Preserve file-backed consumers and ownership

Retain native implementations of:

- `fat_file_extents()` with checked extent coalescing and full-file coverage;
- `fat_file_contiguous_block()`;
- `fat_file_backing_identity()` based on disk plus directory-entry identity;
  and
- the filesystem identity/probe callback introduced by p004.

Verify the complete boot chain through ordinary `struct path`/`struct file`:
`bootN:` private lookup, read-only `rootfs.img` loop backing, writable
`data.img` loop backing, overlay-root mount, runtime boot-source publication,
and FAT-resident swap extent pinning.  Backing claims must continue to reject
rename, unlink, truncate, or conflicting mutable use while loop or swap owns a
file.

### 5. Remove FAT's compatibility implementation

Delete all legacy FAT driver tables, result adapters, callbacks, private-data
word arrays, and `bootfs_*`/`bootfat_*` call sites from `fat.c` and `fat.h`.
Retain no look-alike compatibility structure under another name.  Generic
legacy files remain temporarily only if a non-FAT caller still exists; p012
owns their final deletion.

## Verification

- KA-T090 is rerun to compare all FAT12/16/32, LFN, read, directory, and
  mutation behavior with the p010 checkpoint.
- KA-T100 boots an image whose FAT boot source provides read-only rootfs,
  writable data overlay, and file-backed swap solely through native VFS calls;
  it exercises reads and durable writes after startup.
- KA-T101 covers native partition root, multiple private/runtime FAT mounts,
  publish/promote/release lifetime, missing/corrupt images, read-only media,
  forced allocation/I/O failures, unmount cleanup, and retry safety.
- WS016 backing-claim tests prove active loop/swap backing cannot be renamed,
  unlinked, truncated, or reused and becomes available after final release.
- A source/symbol audit finds no legacy FAT tables, embedded legacy structures,
  or `bootfs_`/`bootfat_` operation inside `fat.c`/`fat.h`, and finds no
  filesystem-specific FAT decoder in generic block identity.
- A disposable amd64 USB-storage image boots through overlay root, reaches
  userspace, writes the data layer, and exercises swap under
  `qemu-system-x86_64` without BOT/I/O/loop errors.
- Affected fixtures, `make -j16`, and `git diff --check` pass without
  `make check` or `.internal/`.

## Completion conditions

- FAT is a direct native filesystem implementation with no embedded or
  emulated bootfs layer;
- FAT12/16/32, LFN, mutation, sync, error, and lifetime behavior match the
  p010 baseline;
- boot-source lookup, root/data loop images, overlay, runtime mounts, and
  file-backed swap use ordinary VFS objects and retain their safety contracts;
- p004 identity dispatch remains filesystem-owned; and
- only non-FAT legacy callers, if any, can remain for p012's explicit audit.

## Result and evidence

Completed on 2026-08-30.  `src/drivers/fs/fat.c` now mounts and operates on
ordinary `struct mount`, `struct inode`, and `struct file` objects backed
directly by `struct disk`.  FAT-private mount, inode, file, cache, allocation,
orphan, and retry state is private to that translation unit.  The public FAT
header now contains only the filesystem type, stable probe type, and the
extent/contiguous-block/backing-identity interfaces used by generic VFS
consumers.  No `bootfs_*` operation, `bootfat_*` operation, legacy driver
table, embedded compatibility object, or private inode layout remains in the
FAT source/header boundary.

The conversion preserves filesystem-owned identity, directory-entry-derived
backing identity, open-writer authority across rename and path truncate,
unlinked-open inode lifetime, delayed orphan reclamation, file extent
enumeration, mirrored FAT updates, read-only enforcement, and mount-sync
durability.  Allocation, shrink/grow, close metadata, LFN deletion/rename, and
directory-entry publication retain retry-safe rollback under injected
allocation and I/O failures.  Focused non-regression hardening also covers
replacement rename, cross-directory `..`, FAT12/16 insertion, the FAT32 LFN
sector boundary, and 255-byte names.

Verification evidence:

- KA-T100/KA-T101 passed ordinary and ASan/UBSan runs with 441,528 checks per
  run across FAT12, FAT16, FAT32, two FAT copies, and 1024-byte logical
  sectors.  Exact regressions cover failed-close retry, deleted-slot reuse by
  an old open inode, FAT32 LFN rollback with the SFN at the next sector's
  first entry, replacement/orphan lifetime, and partial allocation rollback.
- KA-T090's maintained post-conversion wrapper passed by dispatching to the
  native gate.  KA-T030/KA-T031 passed 110 checks and the generic identity
  source audit.  WS016 backing-claim and runtime boot-source runners passed.
- `make -j16`, strict i386 PC/AT and i386 PC-98 FAT compilation,
  `-fanalyzer`, and `git diff --check` passed.
- The source/symbol audit found only `fat_filesystem_type`, `fat_probe_type`,
  `fat_file_extents`, `fat_file_contiguous_block`, and
  `fat_file_backing_identity` as global FAT definitions, no legacy FAT
  adapter call, and no FAT/UFS decoder in generic `block-identity.c`.
- A disposable amd64 OVMF/q35/xHCI USB-storage boot with 4096 MiB reached
  `login:` with read-only `loop0` rootfs, read-write `loop1` data overlay, and
  one active file-backed swap source.  The maintained runner reported PASS
  with no BOT, loop-write, xHCI, or syslog I/O error.

No reconsideration boundary was reached.  Generic legacy bootfs/startup
residue remains intentionally untouched for `ws018-p012`'s fresh caller audit.

## Reconsideration boundary

Stop and request human review if native VFS objects cannot represent stable
directory-entry identity, open-writer rename, orphan lifetime, private mount
promotion, loop backing, or swap extents without weakening an ownership or
durability guarantee.  Do not restore an adapter, invent a renamed bootfs
facsimile, expose private state in a public header, or accept a boot-only FAT
special case.
