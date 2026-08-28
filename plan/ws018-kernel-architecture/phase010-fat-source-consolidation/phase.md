# WS018 Phase 010: FAT source consolidation

Last updated: 2026-08-28

WSID: `ws018`

Phase ID: `p010`

Combined ID: `ws018-p010`

Status: Planned; Queue-ready after `ws018-p001`

Parent: [WS018](../ws.md)

Tests: [WS018 test index](../tests/README.md)

## Objective

Move and mechanically consolidate the current FAT12/FAT16/FAT32, LFN, and VFS
adapter implementation into `src/drivers/fs/fat.c`, with one consolidated FAT
header, while deliberately retaining the existing legacy `bootfs` adapter and
all behavior.  This creates one auditable driver boundary before the semantic
native-VFS migration in `ws018-p011`.

## Preconditions and dependencies

- Complete `ws018-p001`; `src/drivers/` is the only driver source root before
  this Phase adds `src/drivers/fs/fat.c`.
- Capture FAT12/16/32 probe, short/long-name lookup, file mutation, boot-source,
  loop image, overlay, and file-backed swap baselines before moving code.
- Do not combine this Phase with `ws018-p011`.  A source/layout failure must be
  distinguishable from a filesystem semantic migration failure.

## Fixed transitional contract

For this Phase only, the resulting single driver may still contain
`struct bootfs`, `struct bootfs_file`, `bootfat_*`, and the native VFS adapter.
Their call graph, result conversion, pools, locks, cache, and disk I/O remain
unchanged.  This compatibility is a measured migration checkpoint, not the
desired final architecture.

No on-disk structure, accepted FAT variant, case-fold/LFN rule, default Unix
mode, timestamp encoding, allocation policy, mutation ordering, or exported
VFS helper changes in p010.

## Work packages

### 1. Inventory implementation and interface ownership

Record every definition, external declaration, static helper, section
attribute, object-list entry, and test include supplied by:

- `src/kern/fat.c`;
- `src/kern/fat16.c` (including its FAT12/FAT32 paths);
- `src/kern/fat-lfn.c`;
- `src/kern/fat-vfs.c`; and
- `include/kern/fat.h`, `fat16.h`, `fat32.h`, `fat-lfn.h`, and `fat-vfs.h`.

Use this inventory to distinguish genuine cross-subsystem interfaces from
helpers that can become `static` after joining one translation unit.

### 2. Build one driver translation unit

Copy the implementations into `src/drivers/fs/fat.c` in dependency order:
sector/BPB/cache primitives, FAT chain and directory mutation, LFN handling,
then native inode/file/mount adaptation.  Preserve function bodies and data
layout first.  Resolve formerly file-local name collisions with FAT-private
names only; do not opportunistically redesign algorithms.

Retain `FAT_MUTATION`/section placement, fixed object-pool sections, lock ranks,
cache flush ordering, read-only behavior, error mappings, and all FAT12/16/32
driver tables.  Preserve compile-time assertions protecting the legacy private
storage sizes until p011 removes those structures.

### 3. Consolidate headers without changing the contract

Make `include/kern/fat.h` the one FAT header and move into it every declaration
still consumed outside `fat.c`: the FAT type enum, filesystem type, inode
extension, extent callback, probe/type query, contiguous-block query, and
backing-identity query, plus transitional legacy declarations needed through
p010.

Delete `fat16.h`, `fat32.h`, `fat-lfn.h`, and `fat-vfs.h` after updating every
consumer to `kern/fat.h`.  Keep implementation-only structures and helpers in
`fat.c`; header consolidation is not permission to export them.  Do not rename
public identifiers in this mechanical Phase.

### 4. Update all builds and focused tests atomically

Replace the four old C source/object entries with
`src/drivers/fs/fat.c` in every platform manifest.  Update maintained test
source lists and includes to compile one FAT implementation.  Remove old files
only after source, header, manifest, test, and linker audits are empty.

## Verification

- KA-T090 covers FAT12/16/32 probe/mount/read and LFN/SFN lookup/readdir.
- Mutation coverage includes create, append/pwrite, truncate, allocation and
  no-space behavior, mkdir/rmdir, unlink, same/cross-directory rename,
  replacement, open-writer rename, orphan reclaim, sync, and read-only errors.
- Existing WS003 boot-source/swap-source and WS016 backing-claim/runtime swap
  fixtures pass unchanged against the consolidated source.
- A symbol inventory shows one definition of every retained FAT entry point,
  no unintentionally global formerly-static helper, and no old FAT source or
  header path.
- Relevant supported builds, `make -j16`, and `git diff --check` pass without
  `make check` or `.internal/`.

## Completion conditions

- all current FAT implementation is owned by
  `src/drivers/fs/fat.c` and `include/kern/fat.h`;
- all old FAT C/header files and manifest references are absent;
- the compatibility `bootfs` path remains intentionally present and produces
  behavior identical to the baseline; and
- FAT boot media, ordinary mounts, loop images, overlays, and swap remain
  operational, providing the controlled starting point for p011.

## Reconsideration boundary

Stop if joining translation units exposes conflicting assumptions that require
an on-disk, locking, lifetime, or public-API decision.  Record that issue for
p011 or a new Phase.  Do not hide it by changing semantics during this
mechanical checkpoint, and do not delete the compatibility path until p011 has
its own verified replacement.

