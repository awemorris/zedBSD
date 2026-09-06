# WS019 Phase 008: target `/sbin/mkfs`

Last updated: 2026-09-06

WSID: `ws019`

Phase ID: `p008`

Combined ID: `ws019-p008`

Status: completed in q079 after q078 implementation

Planned successor: [WS024](../../ws024-unified-ufs/ws.md) records the user's
2026-09-06 single-UFS/64-bit direction. Its p003 owns migration of this
completed UFS1 formatter to `mkfs -t ufs FILE`, retaining the reservation and
verification contract. The current command remains as recorded below until
that later implementation is selected and completed.

Parent: [WS019](../ws.md)

Tests: [WS019 test index](../tests/README.md)

## Objective

Add the smallest target-side filesystem initializer needed by installer v1:
create the existing zedBSD UFS1 on-disk format inside a caller-sized regular
file, without adding partition or block-device formatting.

## Initial command contract

```text
mkfs -t ufs1 FILE
```

- `FILE` must already exist, be a writable regular file, and have the exact
  desired nonzero size. `mkfs` neither creates nor resizes it.
- The initial implementation accepts only `ufs1`; unknown or omitted types
  fail before mutation.
- A mounted, root, active overlay, active swap, non-regular, aliased, or
  otherwise busy object is refused using a descriptor-owned backing reservation. P002's completed
  diagnostic mount listing is not an exclusion token.
- The command opens the object without following a final symlink, validates
  its identity after open, obtains exclusive access for the operation, and
  revalidates size and identity before publishing success.
- Formatting produces exactly the UFS1 structures already emitted by the
  maintained image builder and consumed by the kernel. This Phase does not
  create a second filesystem dialect. Shared format definitions and fixtures
  are preferred; implementation-code sharing is not required.
- Success is printed only after all required metadata has been written,
  flushed, reopened, and recognized by the production UFS1 probe.

## Scope limits

- no FAT formatter, UFS2 formatter, partition editor, label option, resize,
  repair, tuning, or block-device target;
- no implicit file creation, default size, overwrite prompt, or recursive
  mounting;
- no installer templates and no copy from the running overlay upper.

`zedinst` owns creation of its unpublished 32-MiB staging file and invokes
this command only after preflight and explicit installer confirmation.

## Completion conditions

- Focused fixtures compare geometry and metadata invariants with the existing
  host UFS1 image generator and mount the result through the production UFS1
  driver.
- Wrong type, wrong object kind, short/zero size, symlink, identity change,
  mounted/root/overlay/swap alias, short write, flush failure, and probe
  failure produce nonzero status and never claim success.
- A QEMU target invocation formats an unpublished regular file on FAT32; it is
  then mounted read/write as an overlay upper and survives write, unmount, and
  remount.
- Existing rootfs/data-image construction remains passing.

## Reconsideration boundary

Adding block-device or partition formatting, UFS2, labels, resize, or repair
requires a later Phase with an explicit destructive-operation contract.

## Q078 implementation boundary

The first implementation supports pre-sized FAT-backed regular files with
canonical identity and complete allocated extents; other containing
filesystems fail with `EOPNOTSUPP` before content writes. A descriptor-owned
reservation excludes other content/namespace mutations and swap/loop
activation until final close. Idle descriptors and read-only verification
opens remain possible; shared mappings/cache objects and in-flight mutations
prevent acquisition. Private read snapshots do not grant backing mutation.

The original descriptor retains its reservation while the command reopens
read-only and compares identity/size. Kernel production parsers are linked
into the target tools; no generic probe ioctl or comprehensive storage
snapshot is added. Failure never prints success and never resizes the file.

UFS1 accepts 1024-byte-aligned sizes from 4,194,304 to 130,940,928 bytes,
matching the maintained backend geometry. The initial tree includes both
128-KiB overlay journals and `/etc/zedbsd-root`, matching the data-image
builder with fixture permissions fixed by `umask 022`. Installer v1 still
selects exactly 32 MiB. Geometry and initialized structures are compared
against the maintained builder, not only against the new generator itself.

## Completion evidence

[Q078 implementation and diagnosed attempts](../tests/q078-results.md) and
[Q079 final acceptance](../tests/q079-results.md) close this Phase. Production-
linked normal/sanitizer fixtures and all three supported builds pass. The
isolated QEMU cell passes target formatting, refusals, swap activation and
deactivation, generated overlay/swap boot, two-reboot persistence, and
production-input/GPT/FAT/sentinel invariance.

Only required reserved metadata and allocated initial contents are initialized;
unused areas are not an erasure guarantee. The fixture supplies pre-sized
zero files, so installer staging-allocation performance remains a p004 finding.
