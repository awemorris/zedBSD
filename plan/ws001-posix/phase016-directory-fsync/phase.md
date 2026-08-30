# WS001 Phase 016: truthful and durable directory fsync

Last updated: 2026-08-31

Phase ID: `ws001-p016`

Status: Uncleared (`q042`, 2026-08-31); implementation and deterministic host
ordering tests pass, but disposable-image/remount acceptance is blocked by
`ws008-p010`

Parent: [WS001](../ws.md)

Tests: [WS001 test index](../tests/README.md)

Unblocks: [`ws005-p005`](../../ws005-networking/phase005-wifi-credential-store/phase.md)

## Objective

Make `fsync()` on a directory descriptor either establish the documented
namespace durability guarantee or return an explicit unsupported/error result.
UFS1, UFS2, and overlay directories must synchronously propagate directory
metadata, namespace records, overlay journal state, and the backing-device
flush.  FAT and tmpfs directories must explicitly report that this contract is
unsupported instead of returning a false success.

This Phase narrows `KERN-VFS-01`, `CROSS-FS-01`, and `CROSS-QEMU-01`.  It does
not claim a complete crash-consistency proof for every filesystem operation.

## Finding and accepted baseline

`file_fsync()` currently calls a file-operation `fsync` when present and
otherwise falls back to `inode_sync()`.  Missing inode sync operations return
success.  That behavior is not truthful for directories:

- UFS1 and UFS2 directory file operations have no `fsync`; the fallback can
  persist the directory inode but does not execute the backing `disk_sync()`
  used by regular-file `fsync`;
- overlay directory file operations have no `fsync`, and the overlay inode has
  no sync operation, so `fsync(directory_fd)` can return success without
  syncing the upper directory or overlay journal;
- FAT and tmpfs directory file operations also omit `fsync`, producing a
  success whose durability meaning is undefined; and
- generic mount/inode fallback paths may likewise treat a missing callback as
  successful, which hides unsupported durability from callers.

`ws005-p005` requires the conventional atomic-replacement sequence: write and
`fsync()` a sibling temporary file, `rename()` it over the target, then
`fsync()` the containing directory.  The final step cannot be accepted while a
successful return may be a no-op.  Object ownership is separately owned by
`ws001-p015`; these Phases are independent.

## Durability contract

1. `fsync()` on a UFS1 or UFS2 directory persists that directory's current
   inode and namespace blocks and issues the backing disk flush before success.
2. `fsync()` on an overlay directory persists the authoritative upper
   directory when one exists, the overlay journal/metadata needed to interpret
   that namespace, and the upper backing mount/device.  Every lower-layer
   failure is returned to the caller.
3. `fsync()` on a FAT or tmpfs directory returns `EOPNOTSUPP`.  Regular-file
   `fsync()` on those filesystems retains its existing contract; this Phase
   does not use directory success as an alias for whole-mount flush.
4. A directory filesystem without an explicit directory-sync implementation
   cannot inherit the generic inode/no-op fallback.  It returns
   `EOPNOTSUPP` rather than success.
5. A successful return covers namespace changes completed before the call on
   the same mounted filesystem.  It does not promise atomicity for a mutation
   still running concurrently after the call begins.

## Scope

In scope:

- directory-aware dispatch in `file_fsync()` and explicit directory file
  operations;
- UFS1/UFS2 directory inode persistence plus backing `disk_sync()`;
- overlay upper-directory, journal, and mount synchronization with exact
  failure propagation;
- explicit unsupported directory callbacks/dispatch for FAT and tmpfs;
- create, unlink, mkdir/rmdir, link, symlink, and rename namespace mutations as
  inputs to the directory-sync contract;
- fault injection and reopen/remount evidence for temporary-file rename and
  other representative namespace changes.

Out of scope:

- `fdatasync()` semantic expansion, asynchronous I/O, global `sync()` policy,
  storage-device firmware guarantees beyond a successful driver flush, or a
  new journaling filesystem;
- adding FAT directory durability or treating volatile tmpfs as persistent;
- ownership and effective-credential creation, owned by `ws001-p015`;
- a complete power-failure proof for every ordering combination or every
  supported platform.

## Design constraints

1. Directory sync is an explicit filesystem capability.  Do not infer it from
   an inode metadata callback and do not equate a missing callback with
   success.
2. Keep regular-file and block-device `fsync()` behavior unchanged unless a
   focused regression proves it depends on the same false-success branch.
3. UFS directory sync must flush both the directory inode and the device after
   the directory-entry write.  Preserve the first failing operation's errno;
   never call a later successful flush evidence that the earlier failure was
   repaired.
4. Overlay must synchronize the object that owns the namespace, not merely the
   synthetic overlay inode.  A lower-only directory with no upper mutation may
   complete through the overlay journal/mount contract, while a materialized
   upper directory must be synchronized through its real path.  Read-only
   overlay success is allowed only when there is no writable state to persist.
5. FAT and tmpfs directory descriptors return the same documented unsupported
   error deterministically.  They must not fall through `inode_sync()` or a
   mount callback which returns success for unrelated reasons.
6. Synchronization holds appropriate inode/mount references across dispatch
   without introducing namespace-lock inversion.  Do not hold a generic file
   lock across recursive stacked-filesystem calls if it conflicts with the
   established overlay/upper lock order.
7. Failure injection must observe which persistence and flush callbacks were
   reached.  Sleeps, clean shutdown alone, or QEMU process exit are not proof
   that a device flush occurred.

## Ordered implementation

1. Add Phase-owned focused fixtures under `plan/ws001-posix/tests/`; do not use
   `.internal/`.  Provide an instrumented mock inode/file/mount/disk path which
   distinguishes metadata persistence, directory-block persistence, journal
   persistence, and device flush.
2. Audit every directory `file_ops` table and every caller of `file_fsync()`,
   `inode_sync()`, and `mount_sync()`.  Record which paths are persistent,
   volatile, stacked, or unsupported before changing fallback behavior.
3. Change VFS dispatch so an opened directory without an explicit directory
   `fsync` capability returns `EOPNOTSUPP`.  Keep non-directory fallback
   behavior stable unless it is independently shown to be a false-success
   defect and recorded as a residual.
4. Add UFS1 and UFS2 directory `fsync` operations.  Persist the directory
   inode/state, then issue `disk_sync()` on the same mount; propagate metadata,
   directory-write, and device-flush failures exactly.
5. Add overlay directory `fsync`.  Resolve and hold the authoritative upper
   directory when present, synchronize it through its explicit filesystem
   operation, flush overlay journal state in the established order, and sync
   the upper mount/device.  Refresh/release all stacked references on every
   path.
6. Give FAT and tmpfs directory file-operation tables an explicit unsupported
   result, or enforce that outcome at the capability dispatch boundary.  Add
   regressions showing that regular FAT/tmpfs file `fsync()` behavior did not
   change.
7. Inject failures at UFS inode persistence, UFS directory-block write/device
   flush, overlay real-directory sync, overlay journal sync, upper-mount sync,
   and unsupported dispatch.  Assert exact non-zero results and no false
   success.
8. Run namespace sequences on UFS1, UFS2, and overlay: create+rename,
   mkdir+rmdir, link/unlink, and symlink replacement; call `fsync()` on the
   containing directory; close and remount the disposable filesystem; verify
   the final namespace.  Use disposable images for destructive storage tests.
9. Run the `ws005-p005` atomic-replacement durability sequence as a focused
   consumer regression.  Its non-root ownership gate may remain pending until
   `ws001-p015` completes.
10. Format changed source, run focused tests, run `make -j16`, and perform
    bounded `qemu-system-x86_64` acceptance.  Do not use `make check`.
11. Record actual commands and evidence, update the WS001 VFS/cross-cutting
    rows, and leave unsupported FAT/tmpfs behavior explicit before marking the
    Phase complete.

## Focused test matrix

- VFS dispatch: regular file with an explicit `fsync`, directory with an
  explicit implementation, directory with none, missing inode, and a
  lower-layer error;
- UFS1/UFS2: clean directory, new file, rename replacement, unlink, subdirectory
  creation/removal, hard link, and symlink where supported, followed by
  directory `fsync`, close, and remount;
- UFS1/UFS2 injection: inode persist fails, directory data write fails, and
  `disk_sync()` fails; each must return the original error and must not emit a
  success marker;
- overlay: upper-present and lower-only directories, upper materialization,
  whiteout/journal-changing operations, rename replacement, journal failure,
  real-upper-directory failure, and upper-mount/device failure;
- FAT/tmpfs: directory `fsync()` returns `EOPNOTSUPP` consistently, while a
  supported regular-file `fsync()` retains its prior result; and
- credential-store sequence: sibling temporary write+file `fsync`+rename+
  containing-directory `fsync`, followed by remount and exact content check on
  a disposable UFS/overlay image.

The instrumented tests must assert call order and flush reachability.  The
QEMU test supplies production integration and remount evidence but does not
replace those deterministic assertions.

## Completion conditions

This Phase is complete when UFS1, UFS2, and overlay directory descriptors have
explicit implementations which persist the namespace owner and issue the
required backing flush; every injected lower-layer failure reaches userspace;
FAT, tmpfs, and unknown directory implementations return `EOPNOTSUPP` rather
than false success; regular-file sync regressions pass; the atomic
rename+directory-sync sequence survives close/remount on disposable native
images; focused tests, `make -j16`, formatting, `git diff --check`, and bounded
amd64 QEMU acceptance pass; and the WS001 ledger accurately records the
remaining crash-consistency scope.

Completion removes the directory-durability blocker from `ws005-p005`.  That
Phase still depends on `ws001-p015` for non-root creator ownership.

## Reconsideration boundary

Mark the Phase `uncleared` and return the issue to planning if a truthful UFS
or overlay directory guarantee requires a new on-disk journal/order protocol,
if the disk layer cannot expose a verifiable flush result, or if stacked
directory synchronization creates an unresolved lock-order cycle.  Preserve
explicit `EOPNOTSUPP` rather than weakening the contract.  Do not accept a
clean unmount, whole-QEMU shutdown, delay, repeated retries, or a no-op callback
as evidence of `fsync()` durability.

## Resume point

This Phase is independent of `ws001-p015` and may be queued before or after it.
After both Phases complete, requeue `ws005-p005` and run its complete
root/non-root transactional credential-store acceptance.

## q042 execution result

The source implementation is retained as safe partial progress:

- `file_fsync()` now rejects a directory which has no explicit filesystem
  implementation with `EOPNOTSUPP`, without changing the regular-file inode
  fallback;
- UFS1 and UFS2 directory descriptors use their existing inode-sync followed
  by backing `disk_sync()` operation and preserve the first error; and
- overlay directories synchronize an authoritative upper directory when one
  exists, then the active overlay journal, then the upper mount.  A read-only
  overlay with no writable state remains a successful no-op.

The Phase-owned host fixture links the exact production functions and passes
13 VFS dispatch checks, 36 UFS ordering/error checks, and 86 overlay
ordering/error checks.  The affected UFS1, UFS2, overlay, and generic file
objects compile with warnings as errors for both configured PC-98 and amd64
targets, and `git diff --check` passes.

Completion is not claimed.  The ordinary production build reaches the final
PC-98 image step and then the pinned Noct interpreter rejects the established
`--path=tools/build` option.  Consequently q042 could not generate disposable
images or prove create/rename/directory-`fsync` survival across a native
remount and real storage flush.  Resume after `ws008-p010` restores the host
script CLI contract, rerun the host fixture, generate fresh disposable images,
and execute the bounded amd64 QEMU/remount matrix before changing this Phase to
complete.
