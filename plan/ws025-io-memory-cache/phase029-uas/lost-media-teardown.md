# BUG-021: explicit revoked-medium teardown

Date: 2026-09-10
Status: completed in q230; HS/SS clean and retained-dirty native acceptance passed
Parent: [p029](phase.md), [evidence audit](acceptance-audit.md)

## Observed boundary

q218-mounted-super1 proves that cached UFS reads reject the old medium, while
ordinary unmount returns ENXIO and retains the attachment. This is consistent with
current durability requirements: mount_io_quiesce drains writeback,
unmount_owned synchronizes VM objects, prepare_filesystem_destroy calls mount_sync,
and ufs_prepare_unmount writes the clean-superblock flag. None can certify a lost
medium. The missing function is explicit disposal of that revoked identity, not
turning failed synchronization into success.

## Public operation

Implement the conventional `umount -f directory` through the existing
`unmount(path, flags)` syscall argument. Root authorization remains mandatory.
Define a named UAPI force flag; reject unknown bits. Initially support force only
for a disk-backed mount whose physical medium is irrevocably revoked according to
the existing disk-generation chain. Reject a live medium and unsupported filesystem
with an explicit error. This is not lazy unmount, live-device surprise teardown,
or permission to discard another generation's cache.

Ordinary `umount directory` retains its current synchronization contract. The
forced operation succeeds when the namespace/resources are retired; success does
not assert data persistence. Emit one diagnostic identifying discarded pending
state on the revoked medium. Error observers already attached to old files must
not be silently reset; external references prevent commit in this initial form.

## Transaction and owners

1. Resolve and retain the target through existing mount/context namespace rules.
   Refuse root, bind/overlay/unsupported mounts, children and filesystem snapshot
   dependencies. Join the same readahead boundary used by ordinary unmount.
2. Add a writeback boundary mode that pauses/joins the relevant worker without
   attempting writes to revoked media. Preserve shared-worker siblings. Refused
   teardown restores worker policy and releases the temporary token. Do not call
   the ordinary sync-and-pause routine and merely suppress its errors.
3. Under the mount admission transaction, establish no external fd/cwd/mmap or
   active operation remains. Account separately for registry/cache/writeback
   references. New references must be closed before destructive cache work.
   A pinned page, active DMA, outstanding writeback ticket, child mount, snapshot,
   or unproven count returns EBUSY while preserving all retained ownership.
4. Preflight VM object and buffer-cache discard for this revoked disk/mount only.
   Reserve any bounded workspace before mutation. Do not reclaim pages with live
   pins/mappings/content leases or rely on a generic ENXIO as proof of revocation.
   Stage the complete eligible set or roll back; dropping some dirty objects and
   then returning EBUSY is not an acceptable failure-atomic implementation.
5. At the irreversible commit, discard old dirty/cache ownership locally, discharge
   its writeback credits once, and preserve a diagnostic count. No backend write,
   clean-superblock update or stable-frontier advancement is allowed. Unlink
   objects from dirty/cache indexes and release backing pins/claims in their
   established order. This needs explicit VM/writeback helpers, not flag writes
   into another module's private objects.
6. Give supporting filesystems an explicit revoked-teardown preparation contract.
   UFS must still reject live snapshot dependencies, but must not attempt its normal
   clean-superblock write. Its local finalizer may be shared only after the new
   preflight proves all owned inodes/cache/snapshot resources can retire.
7. Retire namespace/inodes/filesystem state, close the old disk and release the
   paused writeback policy. Only then can the USB control worker destroy the old
   generation and publish the inserted medium. Failed quiescence retains mount,
   disk, URBs and worker resources; it never falls through to successful free.

## Bounded implementation sequence

- A: implement and test revoked-medium eligibility and reversible worker boundary;
  no public successful force-unmount path until downstream commit exists.
- B: add failure-atomic VM/buffer dirty-discard ownership and budget handling under
  the closed mount boundary, including busy pin/mmap/ticket and rollback checks.
- C: wire filesystem preparation, namespace commit, syscall flag and existing
  umount option. Keep unsupported and normal paths explicit.
- D: use the retained --mounted-exchange fixture with explicit `umount -f` after
  asserting ordinary unmount failure. Cover clean and deliberately dirty old
  mounts, busy fd/mmap refusal/retry, old read rejection, unchanged replacement
  backing and fresh publication/readback after successful disposal. Repeat the
  functional path at HS/SS; use focused host checks for ownership failure cases.

The direct-UAS transport remains independent of this generic VFS capability. Do
not erase q218 failure or mark REC05 recovery complete until the path is implemented
and exercised. No user approval wait is introduced: this plan is within the standing
authorization to fix the related filesystem/USB goal.

## Independent BOT correction in q219

storage_refresh_partitions had the same missing administrative disk_open as the
first UAS implementation in q216. It now opens outside the command lock, reloads,
and closes on every outcome. Open failure leaves partitions_pending set and does
not call reload. Existing actual-source BOT fixture now models the real open-count
contract, so it can detect this omission. This patch does not implement forced
unmount or claim native BOT replacement acceptance.


q220: writeback_unmount_begin_revoked now implements the no-sync reversible worker
boundary. Ordinary begin is unchanged semantically. Eligibility uses non-null
retained disk plus disk_media_status, whose current implementation specifically
walks immutable retained parent/media-backing relationships and checks revocation.
Actual concurrent worker fixture passes normal/sanitized runs, and three builds
pass. Mount namespace eligibility and cache/dirty disposal are still outstanding.


q221: buf_discard_media now preflights all old-medium buffers before the first
irreversible eviction. Existing pinned/busy/inflight buffers return EBUSY without
partial dirty discard. This relies on its existing caller-owned exclusion of new
external users throughout check and commit; it is not a new standalone reservation
against concurrent callers. Cross-layer VM/mount preflight and commit remain.


q222: VM now provides non-destructive vm_object_discard_mount_check for retained
cache/writeback objects on a revoked mount. It refuses external object/page/orphan
ownership, preserves dirty/error state, and is verified in a focused actual-source
fixture plus builds. This is not a reservation: caller-owned closed admission,
whole-transaction preflight, dirty accounting and destructive commit still remain.


q223: vm_object_discard_mount now rechecks the complete matching registry before
mutation, retires dirty credits and registry identity, and destroys detached
resources outside the registry lock. Focused actual-code sanitized checks and
three architecture builds pass. Closed admission, cross-layer ownership counting,
filesystem preparation and public operation remain prerequisites for using it.


q224: filesystem_type now has an optional prepare_unmount_revoked callback. UFS
implements a read-only, no-backend-I/O preflight requiring initialized state and
revoked media; retained snapshot devices or journal image readers return EBUSY.
The ordinary prepare_unmount clean write is untouched. No public caller yet.

Integration ordering found during q224: ufs_state_free releases cg_view, which
holds buf references plus a disk reference, and journal_image_free closes/drains
journal readers before freeing memory. Thus a blanket buffer refs==1 preflight
before UFS finalization can mistake an internal cg_view for an external owner.
The mount transaction must account for these known internal pins explicitly and
reserve their disposal, or introduce reversible preparation; it must not discard
VM dirty state first and discover this refusal afterward. Likewise the existing
inode_cache_mount_busy count allows namespace/root/cache refs but not retained VM
file paths. Count these known internal references before destructive teardown,
without treating arbitrary excess references as internal. Journal reader refusal
is now explicit; closed mount admission must keep new readers out through commit.


q225: VM preflight now checks retained file descriptions as well as object refs.
An unmapped cache object can still retain a former mmap caller's description, so
an extra f_refs owner must refuse before clearing dirty/error state. The same
read/write description is allowed with two VM slot refs but contributes one path;
distinct private reader/writer descriptions contribute two. Identity must match
the exact mount and backing inode. Cross-object shared descriptions are currently
conservatively refused rather than credited without an ownership proof.

vm_object_discard_mount_refs returns these proven internal mount-path refs, or
refs for one selected inode, only after full preflight. Caller must keep admission
closed; the result is not a reservation. Call outside the inode-cache lock, then
validate inode counts under that lock with lifetime held; do not add a registry
lock acquisition beneath the inode-cache spinlock. Direct inode owners, external
FDs and cwd refs still require the mount/inode layer's independent refusal.


q226: inode_cache_mount_revoked_check requires a DYING attachment, checks VM
eligibility even with no cached inodes, and accounts cache/root/namespace/VM refs.
A temporary per-inode pin spans VM inspection outside inode_cache_lock. Exact
count mismatch, overflow or VM refusal leaves state untouched and drops only the
temporary pin (without invoking inode_free). Closed admission remains the caller's
responsibility; this helper does not establish it.

Further integration findings (q226): ufs_reclaim calls reclaim_unlinked_inode for
writable zero-link inodes. Explicit revoked commit must disable this backend
reclaim before VM final file_close or inode destruction, after all refusal-capable
checks. Ordinary teardown must retain its reclaim behavior. A no-fail filesystem
commit callback is preferable to changing private UFS flags from mount.c.

Refinement to the buffer ordering above: disk_media_retire already owns whole-leaf
buf_discard_media after disk_media_idle_locked excludes all opens, inflight/cache
users, claims and non-buffer external refs across partitions. Mount teardown need
not destroy the shared physical buffer cache itself: release UFS views locally,
close its disk, and let that existing retirement owner discard buffers when every
old attachment is gone. This preserves sibling-mount ownership and avoids a
failure-capable whole-disk discard after partial mount commit. VM/inode dirty state
still requires local disposal, and native acceptance must prove final retirement
and replacement publication. Retained buffers while another old mount exists are
not a successful persistence claim or permission to publish the new generation.


q227: filesystem_type.commit_unmount_revoked provides the explicit no-fail local
transition. UFS validates its preflight/DYING invariants, closes journal views and
sets only local writable=0. The existing ufs_reclaim guard then prevents zero-link
backend reclamation on final file/inode release. inode_cache_discard_mount_dirty
clears only INODE_DIRTY on the revoked DYING mount, counts affected inodes, and
preserves error history and other flags. Neither path issues backend I/O or is
called by ordinary unmount.

Required final sequence after admission and all preflight: UFS local commit;
dirty-inode flag disposal; VM discard/final file closes; ordinary local namespace
and inode destruction; UFS state free; disk close and policy-token release in the
existing owned order. A failure reported by VM discard after this point is a
caller-contract violation, not grounds to restore a partially discarded mount.
All preflight predicates must be stable under the retained admission boundary.


q228 integrates the public operation: MNT_FORCE is shared UAPI, the root-only
syscall validates flags, and umount accepts -f plus --. The separate revoked path
requires paired filesystem callbacks and lifecycle/VM providers. It pauses I/O,
reserves DYING, purges reconstructible namecache, checks exact mount refs including
VM paths, and runs inode/UFS preflight. Every refusal restores LIVE and the worker
boundary. Commit suppresses UFS reclaim, disposes inode/VM dirty state, logs loss
counts, then follows existing policy/namespace/filesystem/disk lifetime teardown.
Ordinary unmount remains a synchronization operation.

q228-mounted-super1 proves native ordinary ENXIO retention followed by explicit
force success, old mount removal, replacement publication/readback and unchanged
replacement bytes. Its VM/inode dirty counts were zero. Dirty-retained acceptance
and further owner-refusal coverage are still separate requirements.
q228-mounted-high1 stopped earlier, during mkfs, before any new force path ran:
90-second timeout, CPU0 RIP resolves to drv_usb_urb_wait. Captured UAS packets end
with WRITE(16) tag 0xfa, READY, data-out submit/completion; no subsequent status
request was captured. This is not a forced-unmount failure or a proven root cause;
retain the trace for bounded USB completion/deadline investigation.


q228 admission refinement: global VM object eviction, clean/dirty page reclaim and
unscoped sync scans now skip DYING mount objects. Name lookup closure alone does
not exclude these registry-driven owners. The VM fixture verifies clean reclaim
and whole-object eviction leave a reserved mount intact; targeted mount sync is
unchanged for ordinary teardown. This closes the newly identified global-reclaim
admission path before the irreversible commit.


q230 closes this design's acceptance: HS and SS keep 4096 dirty bytes across loss
and FD close, reject force with the held FD/cwd, explicitly discard those bytes,
and publish/read back the unchanged replacement. HS additionally confirms zero
mount/worker/dirty/reserved/ticket accounting after disposal while preserving error
history. See results.md for the completed p029 requirement/evidence audit.
