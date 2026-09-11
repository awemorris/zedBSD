# ws019-p007: dedicated native UFS root installation

Status: completed q183; see [results](results.md)
Depends on p006; Parent: [WS019](../ws.md)

Install EFI/BOOT/BOOTX64.EFI, vmunix and boot configuration on ESP. The other
partition is native current UFS root, selected by stable identity in rootpart.
Do not use overlay root/data images in dedicated mode.

Mount immutable source rootfs.img read-only at a new owned private mount point.
Copy its directory tree into target UFS with recursive attribute-preserving cp,
as requested. Preserve owners, modes, timestamps, symlinks and hard links;
audit and extend existing cp where needed, and verify a source/target manifest.
Never copy the live source upper or active swap. Revalidate the source mount
and release it through owned cleanup. Compare preserved attributes against
its original immutable metadata;
do not compare a writable source's post-read access time to the copied
pre-read value. q170 demonstrates this distinction. A fresh read-only source
view and target remount provide the persistent acceptance reference.
Large image copies use Noct progress
loops; native directory-tree copies use cp. This supersedes a blanket cp ban.

Before cp, enumerate the complete immutable tree using find or an equivalent
walk and freeze a manifest with paths, types, identities and required attributes.
Show the total number of files, then completed/total during copying. Count only
successfully copied and verified entries; separate failed and pending entries.
Report directory preparation separately so directories are not falsely counted
as copied regular files. Define whether links are included in the file count
consistently in the UI and tests. Preserve hard-link groups across copy batches;
do not use independent per-path cp invocations that silently duplicate them.

Use a filename-safe enumeration/manifest protocol (including spaces, tabs,
newlines and leading dashes), never line-splitting unescaped find output or shell
interpolation. Revalidate manifest identity against the private read-only source
before copy. Metadata failure is not successful completion. Final verification
must reconcile the manifest and completed count rather than trusting progress
messages alone. Test empty trees, hidden files, symlinks, hard-link groups,
enumeration failures, copy failures and correct progress totals.

Prerequisite inventory updated q161: [p028](../phase028/results.md)
completed recursive cp -a with checked attributes/hardlinks and file-based
progress, filename-safe find census, Noct treecopy/treeprogress adapters and
independent diff --metadata verification. Native read-only remount tests retain
nanosecond timestamps. This phase still owns private source/target mount
lifetimes and the complete installer transaction around those accepted tools.

Create swap as a UFS file, initialize ZEDSWAP2 and configure boot activation.
Inspect/implement UFS extent export to loopback swap. Active claims must pin
extents and reject truncate, unlink, relocation and incompatible writers.
Test fragmentation, boundaries, missing mappings, activation failure, mutation
refusal, deactivation and release. FAT support is not evidence for UFS support.

Publish boot configuration after root and swap verification. After destruction,
report failures as incomplete installation with truthful recovery state.
Acceptance: source-USB-free fallback boot; native UFS root and active swap;
normal halt/reboot persistence; swap stress; copy/metadata/format/sync faults;
coexistence regression. WS009 must cover both accepted modes and shell escape.
Inventory capabilities, queue missing implementation, then complete integration.
No further product decision blocks this phase.


p038 / q171 proves the existing public read-only mount of the retained boot
loop device. Resolve its name by kern.boot.root_image registration through
strict diskpart inventory; check read-only geometry and mounted st_dev. Use
owned 0700 workspace children, reobserve source identity around acquisition,
and retain cleanup ownership. No new kernel mount API is needed. Generalize
workspace's FAT-only adapter explicitly for UFS before transaction integration.


p039 / q172 integrates the owned UFS source view into common preflight and
revalidation. `prepared.tree.mount.path` is the immutable tree copy source;
workspace reverse release owns unmount and directory cleanup.

Next prerequisite audit: file_format_reserve_locked/file_format_finalize in
kern/file.c, inode_key in kern/backing-claim.c and kern/swap.c file activation
currently admit FAT only and call drv_fat_file_extents. UFS bmap resolves direct
and indirect data fragments but does not expose a claimed file extent iterator.
Implement a common file-backing capability for identity and ordered 512-byte
extents, backed by FAT and UFS providers; do not repeat filesystem dispatch in
each consumer. UFS identity must be stable across mounts (canonical volume plus
inode number), and exported extents must reject holes, malformed pointers and
unrepresentable ranges. Extend reservations and swap only after claim/lock
ordering and mutation coverage are verified. Exercise fragmented/preallocated
files, indirect-block boundaries, truncate/unlink/rename/raw-write refusals,
activation/deactivation/failure cleanup and actual swap I/O. Keep FAT regression.
Existing FAT-specific loop map optimization does not by itself prove UFS swap.


Additional UFS swap audit, q173: ufs_snapshotctl(CREATE) currently checkpoints
and publishes under mount/snapshot/journal locks, without a backing-claim guard.
Swap writes directly to exported sectors, bypassing UFS snapshot preservation.
Define and enforce symmetric admission between active direct file backing and
snapshots before enabling UFS swap: prevent a new claim while a snapshot is
active and prevent snapshot creation racing an existing/preparing claim. Do
not silently let snapshots observe live swap mutations. Add a concurrency test
for both admission orders and retain failure cleanup. UFS extent validation
must exclude per-CG reserved metadata and persistence tail, inspect allocation
bits, reject holes/overlap, and safely coalesce runs across direct/indirect
boundaries in 512-byte units. A volume-size bound alone is insufficient.


q174 mapping-provider audit: backing_claim_finalize does not reject overlaps
WITHIN one new claim; swap validate_extents checks logical coverage/bounds only.
Before UFS backing admission is enabled, add self-overlap rejection and a proof
that exported data does not alias the file's indirect metadata. Bitmap allocation
checks alone do not prove this. Keep the extent iterator separate from final
backing ownership proof, and test duplicated data mappings and metadata aliases.


q175 closes physical self-overlap in the common registry using an in-place
O(n log n) heapsort before registry locking. Keep logical I/O maps separate from
sorted ownership ranges. Next admission design: expose UFS-owned indirect
metadata as additional claim-only ranges, not as logical file data. Include
those ranges when finalizing the same claim so the common self-overlap check
also rejects data/indirection collisions and duplicated indirect blocks.
FAT shared allocation tables must not be treated as file-owned metadata.
Traverse only the file-size-bounded portion of the UFS indirection tree, with
fixed recursion depth and checked allocation, and propagate both count/fill
failures. The data iterator remains unchanged for ordinary logical coverage.

Canonical identity should become a filesystem capability (FAT dirent location,
UFS inode number; both qualified by the canonical volume). Identity lookup must
remain valid for ordinary writes while snapshots are active. Snapshot admission
can use a whole-volume backing mutation guard before taking UFS mount locks:
this excludes existing/preparing claims, while claim creation rejects the guard;
a subsequent extent read rejects an already active snapshot. Check the actual
registry ordering and keep the guard until snapshot publication/unwind finishes.
This avoids holding registry spinlocks across filesystem I/O. Then connect UFS
format reservations and swap, test aliases/mutations/failure release, and perform
actual mkswap/swapon/swapoff and I/O/reboot acceptance before claiming completion.


q176 adds optional file_metadata_extents and the UFS EOF-bounded indirect tree
provider. The next consumer integration should keep logical maps independent:
add a common file-claim finalizer that counts owned metadata, allocates the
canonical ownership array for data plus metadata, canonicalizes both, then
applies self-overlap and registry exclusion before publishing. Existing data
maps stay untouched. Share that implementation with the current raw-range
finalizer rather than adding three copies in format/swap/loop. Preserve
preparing state on all count/fill/allocation/identity errors, validate callback
capacity and repeated counts, and test data/metadata and metadata/metadata
collisions. FAT's absent metadata provider contributes zero ranges. Connect all
three consumers before permitting canonical UFS inode claims, so no path can
publish an incomplete UFS ownership set.


q177 consumer audit: loop_finalize_claim now includes metadata, but loop creation
still calls the FAT-only drv_fat_file_set_loop_map whenever a map exists. When
UFS canonical claims become available, this changes a formerly unclaimed UFS
loop into a claimed one and would reach the FAT-only binding. Preserve UFS loop
behavior by making that cache optimization explicitly optional / provider-owned;
retain the complete claim even if no cache map is bound. Existing loop I/O uses
file_pread/file_pwrite_context, not that array directly. Do not silently regress
UFS loop creation when enabling canonical identity. Its existing 2-GiB bound is
separate from the direct file-swap source path and must not be confused with a
native swap-size restriction.

## Final acceptance after p049

Use one finite 120-active-minute acceptance queue after actual text integration
passes. Do not create another prerequisite sequence. Consolidate accepted codec,
claim, tree-copy, lifecycle and publication fault evidence above; add the missing
real paging proof on a disposable clone of the public-installer-created disk.
Reuse the production-ABI pressure worker in ws016/tests/runtime-
swap-guest.c through a test-only wrapper (not an installed command). Require
actual page-out and page-in increments, pattern readback, unchanged I/O error
count, repeated generations, swapoff release/reactivation and halt. Confirm the
active source is the installed UFS /swapfile and no boot USB or other swap source
is attached. Transfer the probe through the disposable ESP and copy to /run;
this is test instrumentation, not host provisioning of the installed root.

Retain explicit fragmented-file and indirect-boundary admission evidence. If
physical-pressure policy prevents the test from reaching swap, report that as
unaccepted paging rather than treating swapon success as stress coverage. Keep
any genuine missing kernel functionality as an explicit unresolved issue with
a bounded fix; do not mark p007 complete solely from q182 boot/activation proof.
