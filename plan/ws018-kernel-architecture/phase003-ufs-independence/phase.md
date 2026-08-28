# WS018 Phase 003: independent UFS1 and UFS2 implementations

Last updated: 2026-08-28

WSID: `ws018`

Phase ID: `p003`

Combined ID: `ws018-p003`

Status: Complete (`q025`)

Parent: [WS018](../ws.md)

Tests: [WS018 test index](../tests/README.md)

## Objective

Make UFS1 and UFS2 independent filesystem implementations behind the common
filesystem interface.  Remove the apparent shared UFS layer by giving each
format its own helpers and symbols, accepting deliberate code duplication so
either implementation can later be replaced without constraining the other.

## Entry evidence

- `src/kern/ufs1/` owns UFS1 disk structures, endian helpers, superblock code,
  and VFS implementation.
- UFS2 currently calls helpers named `ufs1_get*`/`ufs1_put*`, so it is not an
  independently linkable implementation.
- `src/kern/ufs/` contains journal, snapshot, and soft-dependency sources.  The
  current UFS2 VFS consumes journal and snapshot; no production UFS1/UFS2
  caller consumes softdep.
- Consistency and snapshot implementation structures are currently exposed in
  broad kernel headers even though they are implementation details.

## Fixed design

- `src/drivers/fs/ufs1/` and `src/drivers/fs/ufs2/` are complete, independent
  driver groups.  The former `src/kern/ufs1/`, `src/kern/ufs2/`, and
  `src/kern/ufs/` directories are removed.
- Both drivers share only stable generic kernel contracts such as
  `struct filesystem_type`, `struct mount`, inode/file operations, disk I/O,
  locks, and allocation.  There is no UFS-specific common implementation.
- UFS2 receives its own endian routines named `ufs2_*`; it must not reference
  a `ufs1_*` implementation symbol or private header.
- Journal and snapshot logic used by UFS2 moves into UFS2 ownership.  Its
  private structures and helper declarations remain inside the UFS2 source
  directory, and implementation symbols use `ufs2_` names or are `static`.
- The unreferenced softdep implementation and its unused declarations are
  deleted after a whole-tree reference and focused-test audit proves there is
  no maintained consumer.  Dead code is not copied into both drivers.
- `ufs1_filesystem_type` and `ufs2_filesystem_type` remain the stable
  registration boundaries.  On-disk formats, mount names, supported
  operations, error values, journaling, snapshot behavior, and sync/unmount
  ordering do not change.
- Small helper duplication is intentional.  Do not create a replacement
  common UFS header/source merely to deduplicate similar code.

## Implementation procedure

1. Establish separate UFS1-only and UFS2-only compile/link fixtures and record
   current read/write, endian, journal, snapshot, sync, and corruption results.
2. Copy the endian implementation into UFS2 ownership, rename every private
   helper and use site, and prove that UFS2 no longer links a UFS1 symbol.
3. Move UFS1 into `src/drivers/fs/ufs1/` and UFS2, including journal and
   snapshot sources and private declarations, into `src/drivers/fs/ufs2/`.
   Rename or internalize their symbols and preserve recovery and snapshot
   locking/lifetime behavior.
4. Audit softdep consumers across production and maintained WS tests; delete
   the source and declarations only when the audit is empty.
5. Remove broad consistency/snapshot headers and all three former
   `src/kern/ufs*` directories, then update platform manifests and maintained
   tests to exact independent driver source lists.
6. Run each filesystem fixture independently, followed by supported kernel
   builds and representative UFS boot/mount checks.

## Verification

- `KA-T020`: UFS1 and UFS2 fixtures each compile/link without the other
  driver's sources, mount their own images, exercise representative read/write
  operations, and reject unsupported/corrupt input deterministically.
- `KA-T021`: little- and big-endian UFS2 metadata, journal replay/commit, and
  snapshot create/read/delete coverage remain green under UFS2-owned code;
  UFS1 has no accidental UFS2 dependency.
- A symbol/include audit rejects `ufs1_*` references from UFS2, `ufs2_*`
  references from UFS1, production references to deleted common UFS headers,
  and any source below the former `src/kern/ufs*` paths.
- Run `make -j16` and `git diff --check`; do not run `make check` or use
  `.internal/`.

## Completion conditions

- UFS1 and UFS2 are independently buildable and operable from their
  `src/drivers/fs/ufs1/` and `src/drivers/fs/ufs2/` owners behind the existing
  filesystem-type interfaces.
- UFS2 owns endian, journal, and snapshot implementation; no common UFS source
  directory or cross-format private symbol remains.
- Unused softdep code is either proven dead and removed or the Phase is marked
  `uncleared` with the previously unknown maintained consumer identified.
- `KA-T020`, `KA-T021`, supported builds, and whitespace checks pass.

## Dependencies

`ws018-p001` must complete first so build recipes use the canonical source
layout.  `ws018-p004` consumes the independent filesystem owners established
here.

## Reconsideration boundary

Stop for human review if preserving an existing UFS feature requires a new
public cross-UFS API, if an on-disk behavior must change, or if softdep has a
maintained consumer not visible during planning.  Do not restore a common UFS
implementation simply to reduce duplicated code.

## Execution result

Completed on 2026-08-28 without reaching the reconsideration boundary.

- UFS1 and UFS2 now live in independent
  `src/drivers/fs/ufs1/` and `src/drivers/fs/ufs2/` driver groups.  The former
  `src/kern/ufs/`, `src/kern/ufs1/`, and `src/kern/ufs2/` implementations were
  removed.
- UFS2 owns its endian helpers, journal, snapshot, private disk structures,
  and consistency contract under `ufs2_` names.  The independent source and
  symbol audits found no UFS1 implementation reference from UFS2 and no UFS2
  implementation reference from UFS1.
- The unused soft-dependency implementation had no production or maintained
  consumer and was deleted.  Broad consistency and snapshot headers were
  replaced by UFS2-private declarations.
- KA-T020 passed 23 UFS1 and 23 UFS2 endian/superblock checks with independent
  compile/link/symbol gates.  KA-T021 passed 45 UFS2-owned journal and snapshot
  recovery, preservation, reopen, corruption, and deletion checks.
- Fresh kernels and their architecture checkers passed for amd64, i386 PC/AT,
  i386 PC-98, arm64/RPi4, sparcv9/sun4u, and m68k/X68k.
- The representative production-loader UFS boot/mount matrix passed 4/4 at
  `plan/ws018-kernel-architecture/temp/p003-br-t46-default`: i386 PC/AT,
  i386 PC-98, amd64 BIOS, and amd64 UEFI all reached the accepted default-boot
  observation.  `make -j16` and `git diff --check` also passed.
