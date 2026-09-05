# WS019 Phase 008: target `/sbin/mkfs`

Last updated: 2026-09-05

WSID: `ws019`

Phase ID: `p008`

Combined ID: `ws019-p008`

Status: planned; follows p002 and precedes p004

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
  otherwise busy object is refused using the stable p002 storage snapshot.
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
