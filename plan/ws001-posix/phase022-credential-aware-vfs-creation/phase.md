# WS001 Phase 022: credential-aware VFS object creation

Last updated: 2026-09-01

Phase ID: `ws001-p022`

Status: Complete (`q050`, 2026-09-01)

Parent: [WS001](../ws.md)

Tests: [WS001 test index](../tests/README.md)

Unblocks: [`ws005-p005`](../../ws005-networking/phase005-wifi-credential-store/phase.md)

## Objective

Make the ownership of every newly created filesystem object derive from the
originating process's effective credentials instead of backend defaults.  A
successful user-originated creation must publish an object whose `st_uid` is
the caller's effective UID and whose `st_gid` follows one documented zedBSD
rule based on the effective GID and a set-GID parent directory. A failed
ownership or metadata commit must restore every mutation while cleanup I/O
succeeds. If cleanup itself fails, the operation returns that cleanup error,
detaches borrowed resources, publishes no partial cache object, retains any
uncertain allocation needed for safety, and quarantines the filesystem
read-only rather than pretending that rollback completed.

This Phase narrows `KERN-CRED-01`, `KERN-VFS-01`, `CROSS-FS-01`, and
`CROSS-QEMU-01`.  It is a VFS/filesystem correctness Phase, not a general
account-database, ACL, or utility-conformance Phase.

## Finding and accepted baseline

The current syscall paths use the process credential to authorize creation,
but the credential is discarded before `inode_create()`, `inode_mkdir()`,
`inode_mknod()`, or `inode_symlink()` reaches a filesystem backend.
Consequently:

- `open()`/`openat()` with `O_CREAT` can create a pathname after an effective-
  credential permission check but cannot require that inode to have the same
  effective owner;
- UFS1-created objects retain zero-initialized ownership;
- UFS2-created objects copy the parent ownership rather than the caller's
  effective ownership;
- tmpfs and stacked overlay creation have no explicit originating-credential
  contract;
- FAT has no native per-object UID/GID fields and must not report successful
  POSIX ownership that it cannot persist; and
- `mkdir*()`, `mknod*()`/`mkfifo*()`, `symlink*()`, and pathname AF_UNIX
  `bind()` reach the same credential-free creation boundary.

This blocks the non-root native acceptance in `ws005-p005`: creating
`~/.wifi.conf.lock`, a temporary credential file, or `~/.wifi.conf` cannot
currently prove the required owner before publication.  Repairing directory
durability is separately owned by `ws001-p023`; neither Phase depends on the
other.

## Ownership contract

1. A user-originated object is created with `uid = credential.euid`.
2. Its group is `credential.egid` unless the parent directory has `S_ISGID`;
   under a set-GID parent it inherits the parent's GID.  A newly created
   subdirectory also inherits `S_ISGID`; regular files and special nodes do
   not gain that bit merely because the parent has it.
3. The caller's umask remains applied exactly once by the syscall/VFS layer.
   Supplying ownership to a backend must not reapply or bypass the umask.
4. Existing POSIX ACL inheritance runs against the final requested
   owner/group/mode before the name is considered committed.  This Phase does
   not expand ACL semantics, but an ACL failure must use the same complete
   rollback as any other create failure.
5. Internal kernel creation must declare its ownership source explicitly.
   Overlay copy-up/materialization preserves the source object's metadata;
   it must not accidentally use the current process or root defaults.
6. FAT may create only when the requested owner/group can be represented by
   its persistent mount/metadata policy.  An unrepresentable request fails
   before namespace mutation with an explicit unsupported error.  This Phase
   does not turn `etc/unixmode` into a new transactional ownership database or
   claim full POSIX ownership for FAT.

## Scope

In scope:

- one immutable internal creation-request/attribute contract carrying the
  resolved UID, GID, mode, object type, device number where applicable, and
  creation origin;
- `open()`/`openat()` with `O_CREAT`, `mkdir()`/`mkdirat()`,
  `mknod()`/`mknodat()` and `mkfifo()`, `symlink()`/`symlinkat()`, and pathname
  AF_UNIX socket creation through `bind()`;
- VFS permission checks, collision checks, umask/set-GID resolution, ACL
  inheritance, timestamps, name-cache invalidation, and publication ordering;
- UFS1, UFS2, tmpfs, FAT, and overlay behavior at the shared creation
  boundary;
- backend rollback for inode/block/quota allocation, directory-entry writes,
  parent link counts, `.`/`..`, special-node state, overlay upper objects, and
  name-cache state;
- exact errno propagation without a successful but wrongly owned object.

Out of scope:

- supplementary-group lookup, passwd/group databases, `chown`, set-ID
  execution transitions, or a general credential redesign;
- changing hard-link ownership (a hard link creates a name, not an inode);
- FAT per-file POSIX ownership emulation beyond its existing persistent
  representation;
- ACL feature expansion, default ACL syntax, extended attributes, quotas as a
  standalone feature, or namespace/container credentials;
- directory durability and truthful `fsync()`, which belong to `ws001-p023`.

## Design constraints

1. Resolve the effective UID/GID and set-GID-parent rule once while the caller
   credential and parent inode are stable.  Backends receive resolved values;
   they do not consult an ambient `current_process()` or weak current-
   credential hook.
2. Make the creation contract explicit at the stable inode-operation boundary.
   Do not repair ownership with a post-create `chown()`: that exposes a
   wrongly owned name, requires privileges the creator may not have, and makes
   rollback dependent on a second unrelated mutation.
3. The backend initializes and, where persistent, records ownership before the
   new directory entry becomes observable.  In-memory inode fields and on-disk
   metadata must agree when the operation returns.
4. The caller receives either one referenced, fully initialized inode or a
   null result. A failure after allocation or publication must restore the
   parent and reclaim the child while cleanup succeeds. A rollback failure
   must be surfaced and leave uncertain storage retained in the filesystem's
   conservative/read-only failure state rather than returning success or
   freeing still-referenced media state. A pathname AF_UNIX endpoint carried by the
   creation request is borrowed by the child: every failed publication must
   detach it before releasing a possibly cached inode.  If a directory
   rollback fails and leaves the socket dirent allocated, that inert inode
   must not retain an unbound or subsequently freed endpoint pointer.
5. Overlay forwards the same resolved attributes to its upper layer for a new
   object, while copy-up uses source attributes.  The visible overlay inode is
   refreshed and must match the authoritative upper inode.
6. FAT's lack of representable ownership is an explicit capability outcome,
   not a root-owned success.  Unsupported special nodes/symlinks likewise
   preserve their current honest failure behavior with no partial name.
7. Existing kernel-only call sites must choose explicit system ownership or
   source-preserved ownership.  A nullable request must not silently recreate
   the current root-default bug.

## Ordered implementation

1. Add Phase-owned test fixtures under `plan/ws001-posix/tests/` that inspect
   owner, group, mode, type, link count, and namespace residue after success
   and injected failure.  Do not use `.internal/` material.
2. Inventory every call to the four inode creation entry points and classify
   it as user-originated, kernel-system, or overlay-preservation creation.
   Keep this inventory in the Phase evidence so no call site retains an
   implicit owner.
3. Define the internal creation-request/attribute structure and update VFS and
   inode-operation interfaces in one coherent change.  Derive user attributes
   from the referenced process credential and parent while holding the mount
   namespace transaction used for the create.
4. Convert syscall paths in this order: `O_CREAT`; directory creation;
   FIFO/device-node creation; symlink creation; AF_UNIX pathname `bind()`.
   Preserve their current permission, umask, collision, and cleanup behavior.
5. Implement tmpfs ownership before `publish_new()` and make every allocation
   and publish failure restore node/accounting state.
6. Implement UFS1 and UFS2 ownership in the new-inode allocation/persistence
   transaction.  Verify inode allocation and quota/accounting use the final
   UID/GID, and complete rollback of directory entries, blocks, inode numbers,
   `.`/`..`, and parent link counts for every failure point.
7. Thread explicit attributes through overlay creation and upper-directory
   materialization.  Preserve lower ownership during copy-up, use caller
   ownership for a genuinely new leaf, and refresh the visible inode only
   after the upper operation commits.
8. Give FAT an explicit representability check before mutation.  Preserve
   supported root/mount-owner creation, reject unrepresentable ownership, and
   prove that neither the FAT entry nor cached inode survives rejection or a
   later flush/stat failure.
9. Add deterministic failure injection at allocation, inode metadata persist,
   directory-entry write, ACL inheritance, parent metadata persist, overlay
   upper creation/sync, and FAT representability/flush boundaries.
10. Format changed source, run focused host/kernel tests, run `make -j16`, and
    perform bounded amd64 QEMU acceptance on native UFS1, native UFS2 where
    supported by the fixture, tmpfs, FAT, and the normal overlay-root setup.
    Do not use `make check`.
11. Record actual evidence and update the WS001 credential/VFS/cross-cutting
    rows before marking the Phase complete.  Then rerun the non-root ownership
    portion of `ws005-p005`; its durability completion still waits for
    `ws001-p023`.

## Focused test matrix

For root and at least one non-root effective UID/GID, cover:

- regular creation by `openat(O_CREAT|O_EXCL)` and creation of a directory,
  FIFO, symlink, and AF_UNIX pathname socket;
- ordinary-parent ownership, a set-GID parent with a different group, umask
  application, and set-GID inheritance for a child directory;
- `O_EXCL` collision and permission denial with no metadata mutation;
- UFS1, UFS2, tmpfs, and overlay success with `stat()` values matching the
  resolved contract before and after close/reopen and remount where persistent;
- pathname AF_UNIX publication failure, including a directory-write rollback
  failure which freezes UFS, with no endpoint pointer retained by the failed
  child or a surviving cache entry;
- overlay creation in an existing upper directory and in a lower-only
  directory which must first be materialized;
- FAT representable creation and non-root/unrepresentable rejection with the
  documented errno and no residual entry;
- each injected post-allocation and post-directory-write failure, proving no
  leaked inode/block/quota charge, child name, `.`/`..`, parent link count, or
  name-cache hit while cleanup succeeds; plus double-fault cases which prove
  cleanup-error precedence, endpoint detachment, no partial cache publication,
  safe uncertain-state retention, and read-only quarantine; and
- unchanged ownership of an existing inode when a new hard link is made.

The native QEMU case must create a non-root home-directory file with mode
`0600`, close and reopen it, and observe that its UID/GID remain the creator's
effective credentials.  This is the direct prerequisite for the credential
store, not a host-only proxy.

## Completion conditions

This Phase is complete when every user-originated inode creation path carries
resolved effective ownership to its backend; UFS1, UFS2, tmpfs, and overlay
publish the documented UID/GID/mode atomically; FAT either represents the
request or rejects it before mutation; every injected failure restores all
namespace and allocation state while cleanup succeeds, and every injected
cleanup failure returns the cleanup error, detaches borrowed resources,
retains uncertain state safely, and freezes the filesystem read-only; existing
permission/umask/ACL behavior remains green; focused tests, `make -j16`,
formatting, `git diff --check`, and bounded
amd64 QEMU acceptance pass; and the WS001 ledger records any remaining POSIX
credential debt without overstating conformance.

Completion of this Phase removes the creation-ownership blocker from
`ws005-p005`.  That Phase still requires `ws001-p023` before it can claim its
atomic credential-file replacement durable.

## Reconsideration boundary

Mark the Phase `uncleared` and return the unresolved choice for planning if
correct ownership would require a persistent FAT metadata format change, a
new namespace credential model, or an incompatible public ABI.  Mark a
backend-specific residual instead of weakening the contract if an ordinary
single failure cannot roll back an already published object, or if a cleanup
failure cannot detach resources and quarantine uncertain state safely. Do not
accept post-publication `chown()`, ambient
credentials, root-default ownership, silent FAT coercion, or tests that run
only as root.

## Resume point

This Phase is independent of `ws001-p023` and may be queued before or after it.
After both Phases complete, requeue `ws005-p005` and run its full root/non-root
native credential-store acceptance.

## q042 execution result

The source implementation is retained as safe partial progress:

- one validated `inode_creation_request` now distinguishes caller-derived,
  explicit-system, and source-preserving creation and carries final type,
  mode, UID, GID, device number, and pathname-socket endpoint through the
  stable inode-operation boundary;
- `openat(O_CREAT)`, `mkdirat`, `mknodat`/`mkfifo`, `symlinkat`, pathname
  AF_UNIX `bind`, and the internal VFS/overlay callers now choose an explicit
  origin while holding referenced credentials where user identity is needed;
- user-originated create authorization and the parent set-GID/GID snapshot
  now execute under the same parent `i_io_lock` metadata domain, so concurrent
  `setattr` cannot combine an old `S_ISGID` decision with a newly published
  parent GID;
- tmpfs, UFS1, UFS2, FAT, and overlay initialize the final attributes before
  publication. FAT rejects an unrepresentable request before mutation;
- FAT mirror rollback attempts every copy, returns rollback failure, freezes
  read-only on uncertain state, and restores the exact FAT32 directory slots,
  including a pre-existing `0x00` end marker;
- UFS1/UFS2 detach a borrowed socket endpoint before destructive cleanup or
  release of a rollback-retained inode, and preserve uncertain allocations
  after a rollback freeze; and
- AF_UNIX lookup accepts only an endpoint whose referenced inode path matches
  the atomically published bound path. Overlay visible paths use referenced
  snapshots under the overlay inode lock, so lookup, copy-up, rename, open,
  getattr, readlink, readdir, truncate, setattr, and fsync cannot consume a
  torn upper/lower publication. Cleanup failure has precedence over the
  initiating error.

Focused evidence passes:

- the exact production creation-request helper passes 50 host checks covering
  authorization-under-lock, ordinary and set-GID ownership, a complete
  metadata replacement at the authorization hook, denial cleanup, and unlock;
- the native FAT production-path fixture passes 441,782 ordinary checks and
  the same 441,782 checks under ASan/UBSan, including double-fault mirror
  rollback and exact LFN-slot restoration;
- the UFS independence fixtures pass 23 UFS1 and 23 UFS2 checks, and the UFS2
  consistency fixture passes 45 checks;
- the AF_UNIX publication contract passes; and
- all affected PC-98 and amd64 kernel objects compile with warnings as errors,
  both x86 kernel link steps succeed and reach their established Noct
  post-link validators, and `git diff --check` passes.

At q042 closure, completion was not claimed. The pinned host Noct rejected the
established `--path=tools/build` option, so q042 could not generate disposable
images or run the native UFS1/UFS2/tmpfs/FAT/overlay root/non-root, reopen, and remount
matrix. Deterministic injection for the exact UFS rollback-failed pathname
socket branch and the full overlay create/copy-up rollback matrix was also not
yet present. The q050 result below records how the restored runtime `--path`,
the two focused fault cells, and the Phase-owned amd64 guest matrix resolved
those exact handoffs. The unrelated compile/application CLI form did not gate
these tests.

## q050 completion result

q050 completed every retained boundary without changing the public ABI or an
on-disk format:

- `tmpfs_link()` no longer applies a second link-count increment after the
  generic `inode_link()` publication step;
- lower-only overlay ancestors remain provisional until the leaf operation
  commits and are removed deepest-first after failure;
- failed copy-up publication and temporary pathname-socket cleanup now remove
  and synchronize persistent upper state, preserve the first cleanup errno,
  detach borrowed socket endpoints, and make the overlay read-only when the
  namespace cannot be restored with certainty;
- exact production-linked UFS1/UFS2 pathname-socket rollback-failure fixtures
  each pass 64 checks in ordinary, ASan/UBSan, and fixture-scoped analyzer
  builds; and
- the overlay create/materialization/copy-up fixture passes 2,667 checks in
  each of those modes. A final adversarial review found no remaining
  actionable publication, rollback, stale-lower, reference, or test-fixture
  issue.

The retained production gates also pass: 50 creation-request checks, AF_UNIX
publication, 441,782 FAT production checks in both ordinary and ASan/UBSan
modes, 23 UFS1 plus 23 UFS2 independence checks, and 45 UFS2 consistency
checks. Five bounded amd64 launches then prove overlay, native UFS1, external
UFS2, tmpfs, and FAT behavior. Overlay and native UFS1 each survive an
immediate QMP stop after stage 1 and verify the same writable media in stage 2;
the FAT cell verifies representable creation, explicit non-root rejection,
and external remount. All frozen source image hashes remain unchanged.

`make -j16` and `git diff --check` pass. This Phase is complete and no longer
blocks `ws005-p005`. Historical guest output continues to say
`WS001-P015`; it is retained evidence for this canonical Phase, not a second
active use of Phase 015.
