# WS024: one 64-bit UFS implementation

Last updated: 2026-09-07

WSID: `ws024`

Status: completed (q102); p001–p004 complete; unified production owner and selected U01–U24 acceptance pass

Parent: [master plan](../master.md)

## User decision

On 2026-09-06 the user requested:

> UFS1とUFS2はまとめてUFSと呼んで、64bit実装に一本化する

The decision to unify is settled. The product name is **UFS**, and the final
system has one filesystem implementation using 64-bit block addresses and
file-size/offset handling. Keeping separate UFS1 and UFS2 implementations is
not the target architecture. This request records a workstream; it does not
start implementation or add work to the finished q079 Queue. Scheduling
relative to the existing implementation wave remains for a later Queue.

The approved 2026-09-07 [WS025](../ws025-io-memory-cache/ws.md) owns the
cross-layer I/O/cache/physical-memory redesign. Its p021 consumes WS024's
single driver, format, image migration and journal/snapshot foundation for
ordered metadata write-back. WS024 can complete with write-through behavior;
it must not depend on completion of WS025 write-back. Earlier WS025 batching
changes are carried into the single driver rather than retained as a second
UFS1 production implementation.

## Objective

Consolidate the present drivers, formatters, image builders and consumers
into a single UFS implementation. Use the current UFS2 64-bit disk codec as
the starting point, carry forward the required working features from both
drivers, and retire the separate UFS1 implementation after the image and
consumer migration is verified.

This replaces the future architectural direction of
[WS018 p003](../ws018-kernel-architecture/phase003-ufs-independence/phase.md).
Its completed q025 independence work remains valid historical evidence.
Likewise [WS019 p008](../ws019-installation/phase008-target-mkfs/phase.md)
and q079 remain the completed UFS1 formatter baseline to migrate.

## Fixed boundaries

- One public filesystem type, `ufs`, with `mount -t ufs` and
  `mkfs -t ufs FILE`; one driver owner, intended as `src/drivers/fs/ufs/`,
  one registration and one production disk-codec path. This is consolidation
  of behavior and ownership, not merely renaming two retained drivers.
- 64-bit storage addresses and sizes apply on both 32-bit and 64-bit CPUs.
  PC/AT, PC-98 and other maintained 32-bit targets remain supported; they do
  not keep a UFS1-only branch. This does not require widening inode numbers,
  per-group indices or bounded transfer counts without a format/API reason.
  Audit conversions throughout allocation, block mapping, caches and I/O,
  and state actual supported limits rather than assuming pointer width alone
  makes every layer support the full address range.
- Preserve working native-root and overlay-root behavior, ordinary file and
  directory operations, metadata, sync/unmount and error propagation.
  Preserve the existing UFS2 extended attributes, persistent quotas, journal
  and snapshot functionality, with format and recovery rules accounted for.
- Treat the overlay's `.zovl0`/`.zovl1` files separately from the UFS2
  filesystem journal/snapshot regions (`ZUJ1`/`ZSL1`). Record how each required
  component is initialized and discovered in unified images, with distinct,
  nonoverlapping storage. Quota persistence in `system.zedbsd.quota` requires
  its own round-trip coverage as well as generic extended-attribute coverage.
- Carry the q078/q079 descriptor-owned formatting reservation, no-follow
  identity checks, fixed file size, flush/read-back verification and refusal
  cases into the unified formatter. Changing the filesystem type does not
  authorize block-device formatting or alter ZEDSWAP2.
- The current limited BSD-derived disk layouts do not have measured
  FreeBSD/NetBSD interoperability. Cross-BSD interoperability is not a reason
  to retain two drivers or a completion requirement for this consolidation;
  describe any future compatibility claim only to its tested extent.

## Migration contract to freeze in p001

An existing UFS1 image cannot become the 64-bit format by changing its name
or magic number: inode sizes, address entries and metadata placement differ.
Inventory existing rootfs/data/test images and select a concrete transition
for disposable generated images and for any retained user data. Document
rebuild/export/import requirements and rejection of unsupported old images.
Do not silently reformat, reinterpret or convert a mounted image in place.

P001 records the final on-disk identification/version, initialized metadata,
supported geometry and limits, old-image handling, and CLI/name transition.
If a temporary compatibility reader or alias is needed for migration, bound
its lifetime and removal gate; it must not become a second permanent driver.
These are execution details of the selected unification, not a reopening of
whether to unify.

## Implementation surfaces and dependencies

- Current drivers and public registration headers under
  `src/drivers/fs/ufs1/`, `src/drivers/fs/ufs2/`, `include/kern/ufs1.h` and
  `include/kern/ufs2.h`, plus generic VFS registration and root discovery.
- Target `/sbin/mkfs`, host Noct/image backend, Python fixture builders,
  image checkers and every platform source/package/image manifest.
  The current UFS2 Python builder/checker depend on UFS1-named base code;
  migrate those dependencies before retiring the old source owner. Include
  BIOS, RPi4 and SPARC pack/check consumers, and `blkid` type identification.
- Native-root markers/probes, immutable `rootfs.img`, writable `data.img`,
  loop/overlay integration, boot acceptance fixtures and installer plans.
- WS019 p004/p005 must consume the selected unified formatter/image contract
  when this transition is queued. Keep their source-provenance/publication
  prerequisites explicit; this WS does not implement the installer.
- Retain prior filesystem, backing-claim, mount and formatter regressions.
  Preserve q077 namespace protection, inode lifetime and failure rollback.
  Update active fixtures with the implementation; retain completed Queue
  records as history instead of rewriting their UFS1/UFS2 evidence.

## Phase registry

| Phase | Status | Required result |
| --- | --- | --- |
| [ws024-p001](phase001-format-and-migration-contract/phase.md) | Completed (q100) | Freeze one 64-bit disk/CLI contract, feature inventory and old-image transition |
| [ws024-p002](phase002-single-driver/phase.md) | Completed (q101) | Implement the unified driver and core registration with required feature preservation |
| [ws024-p003](phase003-formatters-and-image-consumers/phase.md) | Completed (q101) | Unify target mkfs, host builders, boot/image consumers and installer contracts |
| [ws024-p004](phase004-acceptance-and-retirement/phase.md) | Complete (q102) | Pass width/functional/boot acceptance and remove superseded production paths |

## Completion conditions

- The active system exposes and builds one UFS implementation and format
  family; UFS1/UFS2 are no longer separate production choices.
- Target and host formatters agree on the unified format and production
  probe/driver behavior; both native root and writable overlay images boot
  and retain data across sync, unmount/remount and reboot.
- Existing journal, snapshot, quota and extended-attribute capabilities retain
  measured functional coverage after consolidation.
- Focused high-address and conversion-boundary fixtures exercise 64-bit
  storage handling on both user/kernel ABIs. Invalid or unsupported old
  images fail predictably without mutation.
- Supported configured builds and the finite runtime matrix selected by the
  implementation Queue pass. Maintained source/symbol/manifest audits find
  no retired active UFS1/UFS2 driver or formatter branch.

Shared tests: [WS024 test index](tests/README.md).

Frozen details and existing ABI limits: [format contract](format-contract.md).
