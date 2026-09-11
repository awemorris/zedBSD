# ws025-p021 results

Status: completed (q116), 2026-09-07. Bounded v2 metadata groups, creation/orphan
recovery, immutable concurrent reads, opt-in deferred checkpoint and independent
metadata failure observers pass focused ordinary/sanitizer, supported builds and
native acceptance. The final verdict and contract mapping below supersede earlier
in-progress descriptions, which remain as execution history.

## Journal slot reuse

The v1 core resets `next_sequence` on mount and clears only the descriptor after
successful home persistence. Old payload/commit records remain. A fresh descriptor
with the same sequence, length and payload digest can therefore match a commit
from the previous mount before the new transaction writes its own commit; changing
the home target makes this an observable uncommitted write after recovery.

The production-linked volatile-media fixture fails before the repair:
`temp/p021-journal-before.log`, assertion that the new home remains unchanged when
no new commit was issued. The repaired writer durably clears the selected commit
sector before publishing a new descriptor. Wire v1 and legacy replay remain
unchanged. This adds one required write/flush boundary to existing per-write
journaling; future grouped metadata transactions must amortize the cost rather
than drop the ordering requirement.

`tests/journal-crash-host.c` uses separate volatile and durable media, hard power
loss via nonlocal termination of the actual production operation, remount and
replay. It crosses old/new payload sizes 1–3 sectors and every write/flush cut,
with volatile-only, immediate persistence and 8/20/256-byte torn final writes.
A committed pending transaction is also interrupted at each replay home/flush/
clear boundary; a subsequent replay converges and another remount is idempotent.
Malformed records may cause explicit EIO without speculative recovery guesses.
Prior committed home data must remain intact. This covers a bounded core model,
not yet filesystem-wide allocation/rename/fsync crash acceptance.

The final focused runner `tests/run-journal-crash-host.py` passes 18755 journal
checks in both ordinary and ASan/UBSan variants (`temp/p021-foundation-host.log`).
Historical journal/snapshot tests initially failed because their injected third
flush formerly represented commit publication; the added invalidation moves that
boundary to the fourth flush. The fixture now checks all six boundaries and
preserves the same semantic expectation. The maintained unified consistency gate
passes 119 checks (`temp/p021-legacy-consistency-after.log`). The initial expected
fixture mismatch remains in `temp/p021-legacy-consistency.log`.

## Snapshot flush owner

The typed snapshot callback context is `struct ufs_io_owner *`, but the snapshot
BIO_FLUSH adapter passed it directly to `disk_sync` as a disk. The actual-adapter
fixture reproduces the wrong identity (`temp/p021-snapshot-before.log`). The
adapter now uses `ms->snapshot_io.disk`; success and EIO each produce exactly one
completion with zero transferred bytes. The final focused runner tests this in
ordinary and ASan/UBSan builds alongside the journal model. No callback wire
interface or snapshot format changes are involved.

## Remaining phase work

The existing common `write_sectors_impl` journals individual contiguous writes,
including data, under journal/snapshot locks. It cannot atomically commit multiple
home targets. `persist_inode_locked` serializes shared dinode-block modification
under the mount lock; future delayed metadata must preserve that ownership and
must not copy unrelated unprepared inode pointers into the same committed image.

Still required: bounded multi-target transaction/home-read visibility, ordered
allocation/data/reference and unlink/free relationships, versioned journal profile
and replay/checkpoint/log pressure, opt-in/epoch/error ownership, quota/xattr/
snapshot/overlay integration, and complete CRASH/META/WB/FLUSH acceptance. These
foundation fixes do not authorize claiming metadata write-back is implemented.


Foundation supported builds pass: `temp/p021-foundation-pcat.log`,
`temp/p021-foundation-pc98.log` and ordinary amd64 restoration plus native fixture
in `temp/p021-foundation-amd64.log`. The mounted journal/snapshot profile passes
existing feature operations, remount and reboot verification in
`plan/ws024/temp/ws025-p021-foundation/results.json`; transcript
`temp/p021-foundation-native.log` reports PASS. This checks existing journal,
snapshot, quota and xattr compatibility; it does not test not-yet-implemented
multi-target metadata transactions. Source hashes are retained in
`temp/p021-foundation-source.json`. Whitespace check passes; no build/runtime is
active. q111/p021 remain in progress for grouped ordered metadata design/work.


## Sole multi-extent format and user steering

The user explicitly removed the requirement to preserve journal version 1 for this
unreleased OS. The initial dual-codec direction was replaced: `ufs-journal.c` now
contains one version-2 reader/writer. `ufs_journal_commitv` validates up to 30
nonoverlapping home extents and at most 128 payload sectors (64 KiB); the existing
single-extent convenience API constructs one extent and calls that same codec.
The separate v1 core and provisional v2-only initializer/include were removed.
`ufs_journal_init` now requires explicit home-volume sectors and validates disjoint
home/locator/journal geometry. The remaining mock initializer was updated too.

Descriptor checksums cover target/count/payload-digest entries; the fixed final
commit sector binds the descriptor checksum and sequence. Old commit evidence is
invalidated durably before redo publication. All redo is flushed before commit;
all descriptors/payloads are validated before any home write. A live slot returns
EBUSY without overwriting unresolved ownership. Single/group commit currently
checkpoint synchronously via replay. It does not yet defer VFS metadata.

Host C, Python and target mkfs emit ZUJ2/version 2 locators. Mount validation
refuses recognized obsolete ZUJ locators instead of ignoring recovery. Snapshot
ZSL1/ZSN1 remains a separate format. No v1 journal reader/writer remains. The
versioned layout is documented in transaction-design.md and the superseding
WS024 format-contract decision. Journal-profile generated images are regenerated;
there is no migration requirement for the removed unreleased format.

### Verification

- `temp/p021-single-format-host.log`: 30624 crash/reuse/group checks in ordinary
  and ASan/UBSan, plus snapshot physical flush/error checks. Includes multi-extent
  crash atomicity, interrupted replay, invalid/overlapping/out-of-volume extents,
  capacity/sequence refusal, occupied-slot refusal, corrupt final payload and
  descriptor target. v1-era focused logs remain historical evidence, not current
  format acceptance.
- `temp/p021-single-format-consistency-after.log`: 114 consistency checks.
  Initial updated-format run failed the old assumption that a failed initial
  slot read poisons a journal before mutation; the maintained failure fixture now
  injects read failure during recovery after a failed flush, preserving the
  intended uncertain-ownership check. New grouped journal uses five durability
  boundaries (redo descriptor/payload share one flush), all tested.
- `temp/p021-single-format-producers.log`: 12 target cells, six three-producer
  byte comparisons and two sparse maximum gates pass. Results and exact commands
  live in `plan/ws024/temp/ws025-p021-single-format-producers`.
  A first runner invocation refused an already-existing output directory before
  doing work; its path-error log is retained separately.
- `temp/p021-single-format-driver.log`: actual UFS metadata/run/view/consistency
  host gates pass in ordinary and sanitizer variants.
- `plan/ws024/temp/ws025-p021-single-format/results.json`: new-profile
  journal/snapshot/quota/xattr operations, remount and reboot PASS. The amd64 image
  and fixture were rebuilt from changed producers in
  `temp/p021-single-format-amd64.log`.
- `plan/ws024/temp/ws025-p021-obsolete-journal/results.json`: a disposable
  valid profile image with its journal locator converted to ZUJ1/version 1 and
  checksum recomputed is refused with EINVAL; source and test media remain unchanged.
  This is a negative test only, not a retained old-format implementation.

Next: separate durable publication from checked home checkpoint under explicit
pending ownership, add metadata read visibility and bounded pressure, then connect
real VFS operation transactions and opt-in/error/fsync boundaries. Group-core
checks do not prove those remaining filesystem-wide requirements.


### Commit proof versus boot replay

A further focused case suppresses a commit write while reporting successful I/O.
The initial group implementation reused permissive boot replay after publication,
so a missing commit was cleared as uncommitted and the new write incorrectly
returned success (`temp/p021-commit-proof-before.log`). Checkpoint after a new
publication now requires the exact expected sequence and descriptor checksum;
missing or different descriptor/commit evidence returns EIO. Boot replay still
permits discarding uncommitted redo. Recovery may establish a safe slot afterward,
but it never rewrites the original operation's failure into success.

`temp/p021-commit-proof-after.log` passes 30641 crash/group checks in ordinary and
ASan/UBSan, plus snapshot flush/error checks; the current consistency gate passes
in `temp/p021-commit-proof-consistency.log`. This explicit success-proof distinction
must remain when publication and checkpoint gain separate owners.


Final checkpoint (2026-09-07 08:39 UTC): the strict commit-proof revision passes
pcat/pc98 and ordinary amd64 restoration in `temp/p021-commit-proof-{pcat,pc98,amd64}.log`.
The current new-format journal/snapshot/quota/xattr feature run, remount and reboot
pass in `plan/ws024/temp/ws025-p021-commit-proof/results.json` and
`temp/p021-commit-proof-native.log`. Source hashes are in
`temp/p021-commit-proof-source.json`. Whitespace check passes and no build/runtime
remains active. q111/p021 remain in-progress; the next required implementation is
explicit pending-group publication/checkpoint/read ownership and real VFS grouping.


## Pending publication, reads and checkpoint

The core now exposes publishv, checkpoint and checked read APIs. Publication
validates durable redo and retains an exact pending witness without home writes or
caller-buffer references. Synchronous commit composes publish/checkpoint and keeps
its existing error/recovery behavior. A second caller receives EBUSY without
retiring a prior pending group. Uncertain publication blocks reads/checkpoint until
explicit recovery; a known committed group cannot be silently downgraded to an
uncommitted boot record.

Home durability and slot retirement are separate states. After home flush, a failed
clear write/flush retains a clearing witness: reads use the known durable homes and
retry performs only slot retirement. This avoids demanding a descriptor that may
already have been cleared. Failed home writes retain verified redo for reads/retry.

`temp/p021-pending-coalesced-host-2.log` passes 34870 crash/replay/pending checks
in ordinary and ASan/UBSan, plus snapshot flush/error checks. Added cases cover
unsorted extents, mixed home/redo reads, caller payload mutation after publication,
live-slot refusal without write side effects, failed home write, failed retirement
flush, two-operation retirement retry, uncertain publication/recovery, loss of a
known commit, four coalesced home gaps and a contiguous multi-sector redo read.
The prior coalescing fixture attempt failed compilation due to misleading indentation;
its log is retained. Current consistency regression remains in
`temp/p021-pending-consistency.log`.

Returned data reads are coalesced. Full-log validation and home replay still use
sector scratch; this overhead must be addressed by the accounted immutable pending
owner before deferred metadata is exposed. The pending APIs are not yet connected
to VFS metadata readers or operation grouping. This is progress within p021, not
completion of metadata write-back or its full crash/performance acceptance.


Pending-core supported builds pass: `temp/p021-pending-pcat.log`,
`temp/p021-pending-pc98.log`, and ordinary amd64 restoration/fixture in
`temp/p021-pending-amd64.log`. The existing mounted feature/snapshot/quota/xattr,
remount and reboot regression passes with the synchronous publish/checkpoint
adapter in `plan/ws024/temp/ws025-p021-pending/results.json` and
`temp/p021-pending-native.log`. Source hashes: `temp/p021-pending-source.json`.
No build/runtime remains active; whitespace check passes. p021/q111 remain active
for the accounted immutable pending image, explicit recovery outcome, CG/metadata
read visibility and VFS operation grouping before deferred policy admission.


## Recovery outcome and current-format checkpoint

Positive commit evidence now survives pending slot retirement. A failed publish
with a durable commit can be distinguished from an abandoned publication after
recovery, before another serialized group is admitted. Negative evidence alone
is not rollback permission during unresolved I/O. The witness resets at init and
is replaced by the next fully validated commit.

`temp/p021-outcome-host.log`: 34948 checks PASS in ordinary and ASan/UBSan, with
snapshot physical-flush/error checks in both variants. Supported pcat, pc98 and
ordinary amd64 builds pass in `temp/p021-outcome-{pcat,pc98,amd64}.log`.
The journal/snapshot/quota/xattr native profile, remount and reboot pass in
`plan/ws024/temp/ws025-p021-outcome/results.json`;
runner log is `temp/p021-outcome-native.log`. Source hashes are recorded in
`temp/p021-outcome-source.json`. Whitespace check passes; no test/runtime remains.

The active phase text now directly states the sole version 2 codec/profile,
rather than retaining superseded v1 maintenance requirements. q111/p021 remain
in-progress: shared-budget pending images/read pins, VFS operation grouping,
coalesced checkpoint, delayed policy and full acceptance are still required.
CG/superblock grouping alone is insufficient to claim atomic allocation/reference
updates; the transaction design explicitly retains that larger boundary.


## Accounted immutable redo and coalesced checkpoint

Journal-profile mounts now own physical backing for the full bounded redo image
plus descriptor, charge actual rounded backing to the shared metadata budget and
unwind both allocation and accounting on failure. A valid image is immutable until
retirement. Publish validates a single bulk payload read; pending redo reads then
require no media read, and checkpoint submits each extent as one write. Standalone
recovery without supplied backing retains its bounded scratch path. External
serialization still applies; concurrent read pins and operation-level VFS staging
are not yet implemented.

Evidence for the current source:

- `temp/p021-image-validated.log`: 39193 ordinary and ASan/UBSan crash checks PASS,
  including both backing modes, corrupt final payload/descriptor, interrupted
  replay, no-I/O RAM redo reads, caller-buffer independence and three-sector
  checkpoint in one home write. Actual snapshot adapter and mount image allocation,
  budget refusal, binding refusal, rounded charge and cleanup pass both variants.
- `temp/p021-image-driver.log` and
  `plan/ws024/temp/ws025-p021-image-driver`: actual UFS metadata,
  allocation/run/view and consistency host/sanitizer regression PASS.
- `temp/p021-image-{pcat,pc98,amd64}.log`: supported builds PASS.
- `plan/ws024/temp/ws025-p021-image/results.json`: current-profile
  journal/snapshot/quota/xattr features, remount and reboot PASS; runner output in
  `temp/p021-image-native.log`.
- `temp/p021-image-source.json`: exact production/fixture source hashes.

Whitespace check passes; no build/runtime remains active and ordinary amd64 is
restored. q111/p021 remain in-progress. Next integrate explicit VFS operation
ownership and immutable read pins; allocation map plus references must share a
prepared transaction boundary. Do not substitute just CG/superblock coalescing
for the required complete allocation/namespace transaction implementation.


## Bounded allocation VFS group

The full-block run path now groups CG bitmap, super totals, shared dinode and an
existing optional indirect leaf after initialized data is flushed. It preflights
capacity before quota/allocator reservation. Snapshot before-images for every
metadata extent precede home mutation. Positive committed outcome is inspected
while journal admission is still serialized; a recovered commit keeps its
allocation even when the syscall receives the original error. Uncertain recovery
keeps charges, marks readonly and preserves dirty ownership. Definitely uncommitted
private CG changes roll back in memory without compensating disk metadata writes.
Shared dinode serialization is factored into preparation and write. read_block now
selects checked redo or rejects poisoned metadata while excluding checkpoint.

Focused actual VFS + actual journal evidence:
`temp/p021-allocation-recovery.log` passes 13768 checks in ASan/UBSan and ordinary.
Both direct and existing-indirect allocation are covered, along with failed writes
(with and without bytes landing), failed flushes, consecutive write/recovery errors,
volatile-cache and immediate-write power cuts, replay, caller-visible errno versus
retained allocation, and each metadata snapshot-preserve failure. Recovery checks
bitmap/reference/super-total/CG-total agreement and initialized data. Poisoned live
metadata reads return EIO; borrowed logical contexts are cleared on ordinary exits.

The first crash fixture exposed host/target libc jmp_buf mismatch (diagnostic logs
retained). The host jump boundary now lives in `allocation-crash-bridge.c`, compiled
without target libc headers. Simulated power loss releases only tracked volatile
fixture allocations; sanitizer checks do not hide leaks. The first driver attempt
also found missing commitv stubs in historical single-extent-only fixtures; their
non-group paths now fail explicitly if unexpectedly called through a group.

Existing driver metadata/run/view/consistency ordinary/sanitizer regressions pass
in `temp/p021-allocation-driver-final.log` and
`plan/ws024/temp/ws025-p021-allocation-driver-final`.
This step does not enable deferred metadata or cover missing indirect trees,
oversized transactions, namespace/unlink/free or full p021 acceptance.


Allocation-group final checkpoint: pcat, pc98 and ordinary amd64 builds pass in
`temp/p021-allocation-{pcat,pc98,amd64}.log`. Current journal/snapshot/quota/xattr
features, remount and reboot pass in
`plan/ws024/temp/ws025-p021-allocation/results.json` and
`temp/p021-allocation-native.log`; the feature workload includes full-block quota
allocation on the 256-sector journal profile. Source hashes are recorded in
`temp/p021-allocation-source.json`. Whitespace check passes, no runtime/build is
active, ordinary amd64 restored. Continue remaining operation ownership and read
pins before deferred checkpoint policy; p021 is not complete.


## Missing indirect paths in the allocation group

The bounded allocation owner now prepares missing single/double/triple indirect
suffixes privately. A modified existing parent and all newly initialized nodes
join CG, super totals and dinode in the same commit. Tree blocks are reserved and
charged with data, included in inode block totals, and released with the private
run after the journal takes its own immutable image. Root pointers become live
only with committed ownership. Path reads now occur under the mount mutation lock,
closing the stale-parent interval before bitmap reservation.

`temp/p021-tree-snapshot.log` passes 104106 actual VFS/journal checks in sanitizer
and ordinary builds. Eight path shapes cover direct, existing leaf, all three
new-root depths and partial missing suffixes beneath existing parents. Failed and
landed writes, flush errors, consecutive recovery failures and power cuts cover
volatile and immediate media. Oracles check references, every newly allocated
bitmap bit, CG/global free totals, inode block count/size and initialized data;
untouched sibling pointers survive. Every allocation-time media read checks mount
ownership. Metadata-plus-data space refusal, private-image allocation failure and
quota refusal issue no disk write and return reservations. Each metadata snapshot
preservation boundary is also injected, including all new tree nodes.

The first expanded fixture build found a logical-not comparison warning; parentheses
were corrected before execution (`temp/p021-tree-paths.log`). Existing driver
metadata/run/view/consistency ordinary/sanitizer tests pass in
`temp/p021-tree-driver.log` and
`plan/ws024/temp/ws025-p021-tree-driver`.

Contiguous runs that cannot reserve the tree plus data, and oversized groups,
still decline before mutation so the existing ordered allocator can handle them.
Other metadata operations, concurrent read pins and deferred checkpoint policy
remain required before p021 completion.


Missing-path final checkpoint: supported pcat/pc98/ordinary amd64 builds pass in
`temp/p021-tree-{pcat,pc98,amd64}.log`. Current-profile native features,
remount/reboot pass in `plan/ws024/temp/ws025-p021-tree/results.json`
and `temp/p021-tree-native.log`. Source hashes are in `temp/p021-tree-source.json`.
Whitespace check passes; no live build/runtime remains and ordinary amd64 is
restored. q111/p021 remain in-progress; retain the remaining operation/read-pin/
policy gates rather than treating the allocation boundary as phase completion.


## Bounded truncate reference/free groups

`ufs-transaction.inc` now owns the shared snapshot/epoch/commit outcome boundary.
`ufs-release.inc` prepares one dinode or indirect-parent reference removal, inode
block count, owning CG map and global free totals as a single group. Private images
leave live allocation untouched before committed evidence. CG cache pins are
released before home writes. Committed frees update live state and quota; uncertain
recovery stops mutation and retains explicit dirty state. An explicit handled flag
selects pre-admission fallback, independently of an actual callback errno.

`temp/p021-release-errno.log`: 95603 actual truncate/journal checks PASS ordinary
and sanitizer. Direct through triple-indirect trees cover failed/landed writes,
flush errors, two consecutive failures, volatile/immediate power cuts at commit
and between release groups, and snapshot preservation failures. Recovery checks
all references against free bits, exact reachable/allocated block counts, inode
block count and CG/global totals; successful truncation also requires zero size.
EOPNOTSUPP at the first flush returns that error without falling back and releasing
the still-owned blocks. Old-size coherent holes are accepted as documented durable
intermediate states after partial truncate; whole-file crash atomicity is not claimed.

The old non-journal fixture retains its strict immediate-home free-before-reference
oracle. The journal-specific fixture checks positive durable commit evidence before
a CG home write and validates the whole filesystem state after replay; transient
home installation within a committed group is not confused with unsafe allocation
admission. Both use actual production VFS and journal code.

Shared commit refactoring retains the 104106 ordinary/sanitizer allocation checks
in `temp/p021-release-allocation.log`. Existing metadata/run/view/consistency host
regressions pass in `temp/p021-release-driver-final.log` and
`plan/ws024/temp/ws025-p021-release-driver-final`.
Namespace/inode-number changes, other metadata owners, read pins and deferred
checkpoint policy remain required. The p021 completion criteria are unchanged.


Release final checkpoint: supported pcat/pc98/ordinary amd64 builds pass in
`temp/p021-release-{pcat,pc98,amd64}.log`. Current-profile native features,
quota-file deletion, remount/reboot pass in
`plan/ws024/temp/ws025-p021-release/results.json` and
`temp/p021-release-native.log`. Source hashes: `temp/p021-release-source.json`.
Whitespace check passes; no live build/runtime remains, ordinary amd64 restored.

q111's reviewed implementation/verification cycle is finished. Its p021 item is
uncleared against the full phase criteria because namespace/other metadata owners,
read pins and deferred checkpoint remain. This is a bounded-cycle handoff, not a
failure of the completed allocation/release gates or a blocked WS025 goal. Select
the next finite p021 queue from current source before further implementation.


## q112 grouped unlink and error-path cache outcome

Unlink now prepares directory record removal and target nlink under namespace,
directory/target inode and mount locks, then commits both extents together.
Committed outcome updates live links/DEAD state. Committed or uncertain errors
explicitly remove the name-cache entry and advance directory sequence after the
inode/mount locks are released; generic VFS otherwise handles only success.
Directory reads reject poisoned journal state after invalidation. Capacity fallback
is separate from actual callback errno. Zero-link inode reclamation after crash
is still a required separate owner; this step proves entry/nlink consistency only.

`temp/p021-unlink-cache.log`: 27982 checks PASS ordinary and ASan/UBSan using actual
UFS, journal and name-cache sources. One/two links and first/merged directory records
cover failed/landed writes, flush errors, consecutive failures, power cuts under
volatile/immediate persistence, snapshot failures and EOPNOTSUPP. Recovery requires
matching directory-name count/nlink and unchanged neighboring directory dinode in
the shared block. Live committed/uncertain errors must produce a real cache miss;
poisoned next_dirent returns EIO. Host spin/IRQ and dirseq scaffolding isolates the
kernel environment; native validation exercises generic VFS integration.

The initial old-fixture links lacked newly reachable cache API stubs; the actual
cache fixture also needed host IRQ-spin callbacks. Corrected fixtures pass.
`temp/p021-unlink-driver-2.log` records metadata/run/view/consistency regressions
in ordinary/sanitizer; `temp/p021-unlink-socket-final.log` records 78 actual UFS
pathname socket rollback-fault checks. Earlier diagnostic link logs are retained.


Unlink checkpoint: supported builds pass in
`temp/p021-unlink-{pcat,pc98,amd64}.log`. Current-profile native namespace,
journal/snapshot/quota/xattr, remount and reboot pass in
`plan/ws024/temp/ws025-p021-unlink/results.json` and
`temp/p021-unlink-native.log`. Hashes: `temp/p021-unlink-source.json`.
Whitespace check passes; no active runtime/build, ordinary amd64 restored.
q112/p021 remain in-progress. Next inspect shared-dinode merging and generic
success-only link bookkeeping before grouping link/other namespace operations;
retain orphan/inode-number, read-pin and metadata-delay completion requirements.


## q112 hard-link insertion and shared-dinode merge

Dinode encoding is separated from loading, allowing a directory image and target
nlink image to share one physical redo block without replacing either slot's update.
The grouped hard-link path covers occupied-record slack, empty-record reuse and
size growth within an existing directory block. It validates the complete directory
before editing. Actual deduplicated footprint governs admission. Success leaves
live nlink increment to generic inode_link; committed error applies it in the driver
and invalidates cache/dirseq. Directory size follows established committed outcome.

`temp/p021-link-refusals.log`: 65364 ordinary/sanitizer checks PASS with actual UFS,
journal and name-cache sources. Shared/distinct dinode blocks and four directory
layouts cover write/flush failure, landed writes, consecutive recovery failures,
volatile/immediate power cuts, snapshot boundaries and EOPNOTSUPP. Recovery requires
new-name presence, target nlink and directory size to agree, while directory nlink
and both inode data pointers remain intact. The host simulates the verified generic
success-only increment; native validation exercises the actual wrapper.

Refusals include EMLINK, invalid name, duplicate name, corrupt/full directory and
allocation failure with no writes. A two-extent journal accepts shared-dinode
insertion but declines three distinct extents before admission. Existing cached
names are seeded only where present in the fixture directory; an initially empty
directory tests sequence publication without fabricating a cache entry.

`temp/p021-link-driver.log`: existing metadata/run/view/consistency ordinary/sanitizer
regressions PASS. Shared encoder regressions also pass 104106 allocation checks in
`temp/p021-link-allocation.log` and 95603 truncate checks in
`temp/p021-link-release.log`, both ordinary and sanitizer.

Unallocated directory backing/oversized groups retain their pre-admission path.
Rmdir/rename/create and inode/orphan owners, read pins and deferred checkpoint remain;
this step does not complete p021 or enable metadata delay.


Hard-link final checkpoint: supported builds pass in
`temp/p021-link-{pcat,pc98,amd64}.log`. Native hard-link/unlink with the real generic
VFS wrapper, journal/snapshot/quota/xattr features, remount and reboot pass in
`plan/ws024/temp/ws025-p021-link/results.json` and
`temp/p021-link-native.log`. Source hashes: `temp/p021-link-source.json`.
Whitespace check passes; no live build/runtime, ordinary amd64 restored.
q112/p021 remain in-progress. Continue remaining namespace and orphan/inode-number
owners, then read pins/deferred checkpoint and the unchanged full acceptance gates.


## q112 empty-directory removal

The removal owner now stages the parent directory entry, target nlink zero and
parent nlink decrement as one journal group. Shared parent/target dinode blocks
merge both slots before publication. A distinct parent dinode adds a third extent.
Committed outcomes publish both live counts; committed/uncertain error paths
invalidate the namespace cache after inode/mount ownership is released. Dot names
are rejected before namespace locking; existing emptiness checks remain in force.

`temp/p021-rmdir-final.log`: 39435 actual UFS/journal/name-cache checks PASS in both
ordinary and sanitizer runs. Shared/distinct dinodes, first/subsequent directory
records, failed/landed writes, flush failures, consecutive recovery failures,
volatile/immediate power cuts, snapshot preservation and EOPNOTSUPP are covered.
Replay requires the entry and both link counts to agree. Refusals cover nonempty
directories, regular-file targets and dot names. Child data ownership is retained;
this fixture does not establish orphan reclamation or inode-number retirement.
The initial fixture warning and subsequent diagnostic logs are retained.

The shared removal refactor also passes 27982 unlink checks per ordinary/sanitizer
run (`temp/p021-rmdir-unlink.log`). Existing metadata/run/view/consistency ordinary
and sanitizer regressions pass (`temp/p021-rmdir-driver.log`). Supported pcat,
pc98 and amd64 builds pass (`temp/p021-rmdir-{pcat,pc98,amd64}.log`).

Native feature/remount/reboot acceptance passes in
`plan/ws024/temp/ws025-p021-rmdir/results.json` and
`temp/p021-rmdir-native.log`. The guest rejects nonempty rmdir, removes the child,
creates a snapshot, removes the empty directory, checks restored parent nlink,
checks snapshot preservation and verifies live absence after remount/reboot.
Source hashes are in `temp/p021-rmdir-source.json`; they match this checkpoint.
No build/runtime remains live; the ordinary amd64 image is restored.

q112/p021 remain in-progress. Remaining create/rename and orphan/inode-number
owners, read pins and deferred checkpoint remain required before full acceptance.
The user-approved sole version-2 journal format remains unchanged.


## q112 general private metadata image owner

The new bounded image owner deduplicates physical blocks before private edits.
Repeated prepared dinodes share one loaded image; partial overlaps, zero/overflow
addresses and exhausted capacity fail before publication. Failed reads admit no
extent. Capacity accounts for caller memory, journal payload/descriptor limits
and the current slot. Hard-link insertion now uses this owner for directory bytes
and both dinodes, preserving the existing committed/error and VFS count contract.

`temp/p021-images-link.log`: 65365 hard-link fault/replay checks PASS in both
ordinary/sanitizer runs. `temp/p021-images-owner-2.log`: 465 focused ownership
checks PASS in both modes. Four inode updates spanning two shared blocks are
permuted through all 24 orders; whole-block comparisons preserve unrelated bytes.
The fixture checks one read per unique block, repeated mutable-image identity,
failed-read retry, partial overlaps from both directions, capacity/slot rounding,
foreign mounts and absence of persistent writes. The first fixture build failed
because its output referenced a nonexistent check counter; the corrected fixture
uses functional_checks. That diagnostic log is retained.

Existing metadata/run/view/consistency ordinary/sanitizer regressions pass in
`temp/p021-images-driver.log`. Supported pcat, pc98 and amd64 builds pass in
`temp/p021-images-{pcat,pc98,amd64}.log`. Current native namespace/features,
remount and reboot PASS in
`plan/ws024/temp/ws025-p021-images/results.json` and
`temp/p021-images-native.log`. Source hashes: `temp/p021-images-source.json`.
Whitespace check passes; all build/runtime handles are terminal and ordinary
amd64 is restored. This verifies the shared image foundation and its hard-link
consumer; it does not claim rename atomicity. Rename integration requirements
are recorded in transaction-design.md. q112/p021 remain in-progress.


## q112 grouped rename of existing directory backing

The rename callback now admits one private group for source removal, destination
insertion/replacement, cross-parent directory dotdot, parent sizes/link counts and
replacement target nlink. Same-parent edits share one evolving directory and inode
image. Physical footprint and inode locks are deduplicated. Positive commit controls
live publication; committed/uncertain errors invalidate names and parent/source
sequences after releasing inode/mount locks. Admitted I/O errors never enter the
historical compensating-write path. Existing callback and generic VFS validation
remain responsible for flags, ancestry, type and nonempty replacement checks.

`temp/p021-rename-refusals.log`: 234341 actual UFS/group/journal/name-cache checks
PASS in each ordinary/sanitizer run. Sixteen same/cross-parent, file/directory,
replacement/no-replacement and shared/distinct-dinode combinations cover every
selected write/flush failure, landed write, consecutive recovery failure, volatile
and immediate durability power cut, snapshot preservation failure and EOPNOTSUPP.
Recovery checks both visible names, both parent nlinks, target retirement count and
source dotdot against one committed outcome. Source inode/pointer remains intact.
Live committed/uncertain errors invalidate the actual name cache including dotdot.
The fixture uses resolved inode objects and models generic success invalidation;
it does not claim that host mocks exercise final-reference inode reclamation.

Refusals additionally cover invalid aliases, zero source/parent link counts,
destination EMLINK, slash-containing name, allocation/read failure, capacity decline,
and malformed directory bytes after the matching entry. No refused operation writes
persistent state. An initial fixture signedness warning was corrected; diagnostic
`temp/p021-rename-core.log` is retained. The earlier core-only run passed 234306
checks per mode in `temp/p021-rename-core-2.log`.

The shared insertion helper passes 65365 hard-link fault/replay checks in both modes
(`temp/p021-rename-link.log`). Existing metadata/run/view/consistency ordinary and
sanitizer regressions pass (`temp/p021-rename-driver.log`). Supported builds pass
in `temp/p021-rename-{pcat,pc98,amd64}.log`.

Native mounted features, remount and reboot PASS in
`plan/ws024/temp/ws025-p021-rename/results.json` and
`temp/p021-rename-native.log` (guest RUN 131 checks; reboot VERIFY 26 checks).
The real callback/generic VFS path replaces a directory across parents, replaces an
open file, renames within one parent, checks cached dotdot and parent nlinks, retains
old open-file content, preserves snapshot source/victim directories and rechecks
persistent names/content after remount/reboot. Source hashes are recorded in
`temp/p021-rename-source.json`. Whitespace checks pass; all handles are terminal and
ordinary amd64 is restored.

q112/p021 remain in-progress. Unallocated destination directory backing and oversized
profiles still decline before admission to the existing path; integrating backing
allocation with namespace publication remains required. Create/mkdir/mknod/symlink,
persistent orphan/inode-number retirement, concurrent read pins, deferred policy
and the original complete phase acceptance remain outstanding.


## q112 final inode-number retirement

After successful content/xattr teardown, final-reference reclaim now groups dinode
mode/type retirement, inode bitmap release and CG/super free-inode totals. Directory
count decrements are included in the same transaction. The owner rejects reserved
or out-of-range inode numbers, nonzero nlink/size/block counts or direct/indirect/
xattr owners. It retains private CG bytes until positive commit and releases inode
quota exactly once on established retirement, including recovered committed errors.
An admitted error never falls through to the old independent retirement writes.

`temp/p021-retire-refusals.log`: 14863 actual UFS/journal/quota checks PASS per
ordinary/sanitizer run. File/directory retirement covers failed/landed writes,
flush failures, two consecutive write failures, volatile/immediate power cuts,
snapshot preservation/failure and EOPNOTSUPP. Replay requires mode, allocation bit,
free-inode totals and directory totals to agree. Live quota usage follows positive
commit; neighboring inode allocation bits remain intact. Refusals cover every
remaining block-owner class, reserved/wide-invalid inode numbers, already-free
bitmap, memory refusal and pre-admission capacity decline without writes.
The earlier core fixture passed 14844 checks per mode (`temp/p021-retire-core.log`).

Existing metadata/run/view/consistency regressions pass ordinary/sanitizer in
`temp/p021-retire-driver.log`. Supported builds pass in
`temp/p021-retire-{pcat,pc98,amd64}.log`. Native mounted features/remount/reboot PASS
in `plan/ws024/temp/ws025-p021-retire/results.json` and
`temp/p021-retire-native.log` (RUN 131, reboot VERIFY 26). This exercises unlink,
rmdir and replaced-open-inode final-reference cleanup with real VFS, snapshots and
quota, in addition to the host final-retirement invariant checks. Hashes are in
`temp/p021-retire-source.json`. Whitespace checks pass; ordinary amd64 restored
and all build/runtime handles terminal.

q112/p021 remain in-progress. This is final retirement after teardown, not mount
orphan discovery. Create/backing-allocation publication, interrupted creation,
xattr-reference release, mount-time orphan recovery, read pins/deferred policy
and full phase acceptance remain required.


## q112 grouped xattr area release

Clearing an attribute area now removes all one/two pointers and serialized length,
reduces inode block count, and frees the affected CG/super block accounting in one
transaction. CG images and pre-admission footprint are deduplicated. Pointer/size
agreement, aligned allocation ownership, duplicate blocks and block-count bounds
are checked before publication. Positive commit updates live ownership/free total
and quota; admitted errors never enter the historical compensating-free path.
Nonempty replacement and first allocation are not covered by this clear operation.

`temp/p021-xattr-release-refusals.log`: 34889 actual UFS/extattr_publish/journal/quota
checks PASS per ordinary/sanitizer run. One block, two blocks in one CG, and two
blocks in different CGs cover failed/landed writes, flush failures, consecutive
recovery failures, volatile/immediate power cuts, snapshot preservation failures
and EOPNOTSUPP. Replay requires pointers/length/block count, each free map and free
totals to agree. File data ownership and old attribute bytes remain intact. Live
quota usage follows positive commit without consuming the inode charge.

Refusals cover duplicate/missing/unaligned pointers, oversized serialized area,
insufficient inode blocks, already-free bitmap, allocation failure and capacity.
A two-block area sharing one CG fits exactly the smaller three-extent slot. The
initial two-CG fixture omitted the second CG's index and was correctly rejected;
that diagnostic is retained in `temp/p021-xattr-release-core.log`. After fixing
fixture geometry, the core run passed 34846 checks per mode in
`temp/p021-xattr-release-core-2.log` before adding refusal checks.

Existing metadata/run/view/consistency ordinary/sanitizer regressions pass in
`temp/p021-xattr-release-driver.log`. Supported builds pass in
`temp/p021-xattr-release-{pcat,pc98,amd64}.log`. Native features/remount/reboot PASS
in `plan/ws024/temp/ws025-p021-xattr-release/results.json` and
`temp/p021-xattr-release-native.log` (RUN 135, reboot VERIFY 26). A victim file now
owns an xattr before snapshot/rename replacement; closing its last live reference
clears that area, while the snapshot still returns the old xattr. Source hashes:
`temp/p021-xattr-release-source.json`. Whitespace checks pass; all handles terminal,
ordinary amd64 restored.

q112/p021 remain in-progress. Nonempty xattr update/allocation, create/backing
publication, mount-time orphan recovery, read pins/deferred policy and full phase
acceptance remain. This result proves teardown, not complete xattr crash safety.


## q112 existing xattr payload replacement

The existing-area owner now replaces the retained first block in the same group
as the dinode's length/pointers/block count. Shrinking a two-block area to one also
releases the second block through the established private CG/super/quota owner.
One-block updates contain exactly payload and dinode extents, with no unchanged
CG/super images. Retained backing is checked allocated; payload padding is zeroed.
The first-allocation path remains separate pending grouped allocation integration.

`temp/p021-xattr-replace-boundaries.log`: 35619 actual extattr_publish/UFS/journal/
quota checks PASS per ordinary/sanitizer run. One-to-one replacement and same/other
CG two-to-one shrink cover failed/landed writes, flush failures, consecutive recovery
failures, volatile/immediate power cuts, snapshot failures and EOPNOTSUPP. Replay
requires payload bytes and serialized length, pointers, block count, released maps
and totals to agree; retained/file data backing remains allocated. Positive live
outcome controls quota and attributes even after recovered committed errors.

Boundary checks cover NULL/oversized payload, allocation refusal, freed retained
backing and pre-admission capacity. A valid 8-byte old record grows to a complete
4096-byte payload within a two-extent slot. Full payload comparison and inode fields
prove growth; crash tests use the same full-block redo shape. The earlier replacement
core run passed 35584 checks per mode (`temp/p021-xattr-replace-core.log`).
The refactored clear fixture still passes 34889 checks per mode in
`temp/p021-xattr-replace-clear-final.log`.

Existing metadata/run/view/consistency ordinary/sanitizer regressions pass in
`temp/p021-xattr-replace-driver.log`. Supported builds pass in
`temp/p021-xattr-replace-{pcat,pc98,amd64}.log`. Native mounted features/remount/reboot
PASS in `plan/ws024/temp/ws025-p021-xattr-replace/results.json` and
`temp/p021-xattr-replace-native.log`, including real xattr create/replace/remove,
quota configuration and snapshot old-value preservation. Source hashes are in
`temp/p021-xattr-replace-source.json`. Whitespace checks pass; all handles terminal
and ordinary amd64 restored.

q112/p021 remain in-progress. First xattr allocation, create/backing publication,
mount orphan recovery, read pins/deferred policy and full acceptance remain required.
Creation inspection also confirmed ACL inheritance/preservation can write child
xattrs before name publication; the reservation must stay recoverably zero-link
through that preparation, then atomically acquire its final name/link state.


## q112 first xattr allocation

First attribute publication now groups private CG/super allocation, initialized
zero-padded payload and prepared dinode reference/length/block count. A quota block
is reserved before mutation. Positive commit publishes live ownership/free total;
positive or uncertain outcome retains quota, while definite uncommitted failure
rolls back the reservation. No admitted errno retries the compatibility allocator.
Complete free blocks are selected under mount exclusion, with no early live map
mutation or separate data initialization write.

`temp/p021-xattr-allocation-refusals.log`: 21660 actual extattr_publish/UFS/journal/
quota checks PASS per ordinary/sanitizer run. First-CG and next-CG allocation cover
failed/landed writes, flush failures, consecutive recovery failures, volatile and
immediate power cuts, snapshot preservation failures and EOPNOTSUPP. Replay checks
payload initialization, inode references/length/block count, bitmap and CG/super
totals as one outcome. Existing file backing remains intact. Quota retains uncertain
allocations even when live pointers are not yet publishable. The earlier core run
passed 21609 checks per mode (`temp/p021-xattr-allocation-core.log`).

Refusals cover NULL/oversized payload, existing stray pointer, inode block-count
overflow, memory/read failure, fragmented capacity, EDQUOT and capacity decline.
Rejected reservations leave quota at the original usage and produce no disk writes.
The exact four-extent slot accepts a successful first area after those refusals.
Existing replacement still passes 35619 ordinary/sanitizer checks in
`temp/p021-xattr-allocation-replace.log`.

Existing metadata/run/view/consistency ordinary/sanitizer regressions pass in
`temp/p021-xattr-allocation-driver.log`. Supported builds pass in
`temp/p021-xattr-allocation-{pcat,pc98,amd64}.log`. Native features/remount/reboot
PASS in `plan/ws024/temp/ws025-p021-xattr-allocation/results.json` and
`temp/p021-xattr-allocation-native.log` (RUN 135, reboot VERIFY 26), exercising first
attribute creation, replacement/removal, quota persistence and snapshot retention.
Hashes: `temp/p021-xattr-allocation-source.json`. Whitespace checks pass; all handles
terminal and ordinary amd64 restored.

q112/p021 remain in-progress. The attribute lifecycle now has grouped owners for
first allocation, existing replacement/shrink and full teardown on admitted
profiles. This does not resolve creation's separate inode/name publication.
Recoverable inode reservation, create/mkdir/mknod/symlink/backing publication, mount
orphan recovery, read pins/deferred policy and full phase acceptance remain required.


## q113 initialized inode reservation

q112 closed as a finite cycle at 2026-09-07 11:17 UTC with p021 uncleared against
its full criteria. q113 selects recoverable creation/backing and orphan ownership
using the verified namespace/xattr/retirement owners; standing autonomous authority
covers this successive queue.

Journal-backed new_inode now reserves a number with a fully initialized zero-link
dinode, bitmap/free totals and directory accounting in one group. The old inode
slot is cleared and its generation advanced before encoding, while sibling slots
are preserved. Quota follows committed/uncertain ownership. The directory count is
not added twice by the subsequent existing completion path. Failed reservations
release the new in-memory inode; positive identity can follow final-reference
cleanup, while poisoned ownership remains for recovery.

`temp/p021-reserve-refusals.log`: 18896 actual UFS/journal/quota checks PASS per
ordinary/sanitizer run. File/directory reservations cover failed/landed writes,
flush failures, consecutive recovery failures, volatile/immediate power cuts,
snapshot failures and EOPNOTSUPP. Replay couples bitmap, zero-link typed inode,
identity/generation and CG/super inode/directory counts. Stale pointers and attribute
fields are cleared; whole neighboring dinode regions retain their old bytes.
Refusals cover invalid kind/nonfresh identity, memory/read failure, full inode map
and EDQUOT without writes or leaked definite-uncommitted quota.
An initial fixture signedness warning was corrected; the diagnostic log remains.
The earlier core run passed 18876 checks per mode (`temp/p021-reserve-core-2.log`).

Existing metadata/run/view/consistency ordinary/sanitizer regressions pass in
`temp/p021-reserve-driver.log`. The host harness supplies the generic kind-to-mode
conversion without kernel inode-cache internals; native uses the real exported
function. Pathname socket rollback still passes 78 checks
(`temp/p021-reserve-socket.log`). Supported builds pass in
`temp/p021-reserve-{pcat,pc98,amd64}.log`. Native mounted features/remount/reboot PASS
in `plan/ws024/temp/ws025-p021-reserve/results.json` and
`temp/p021-reserve-native.log` (RUN 135, reboot VERIFY 26), exercising new_inode,
create/mkdir, xattrs, quota and snapshots. Source hashes: `temp/p021-reserve-source.json`.
Whitespace checks pass; all handles terminal and ordinary amd64 restored.

q113/p021 remain in-progress. The reservation transaction is zero-link, but the
legacy completion still sets nlink before ACL preparation/name publication. Do not
claim zero-link lifetime or atomic complete creation yet. Final name/link publication,
creation rollback/backing integration, mount orphan recovery, concurrent read pins,
deferred metadata policy and full phase acceptance remain required.


## q113 checked creation cleanup and retired identity

Supported journal creation cleanup now commits zero nlink while retaining data and
xattr references, then uses a checked errno-returning reclamation owner for content,
attribute and inode-number retirement. This replaces independent pointer clearing
and frees on that path. The caller must establish name absence; committed creation
publication must not use this cleanup. The existing final-reference callback wraps
the same checked owner without changing its void VFS ABI.

Positive final retirement now invalidates the live inode number even when returning
an I/O error. Subsequent final-reference reclaim and ordinary inode sync cannot
write through that reusable identity or reserved inode-zero slot. Explicit cleanup
reports failure while preserving the resources still owned by a zero-link inode.

`temp/p021-cleanup-final.log`: 63174 actual UFS multi-transaction cleanup/journal/quota
checks PASS per ordinary/sanitizer run. File/directory cleanup with data and xattrs
covers every observed write/flush/read failure, landed writes, consecutive recovery
failures, volatile/immediate power cuts and snapshot-preservation failures. Replay
checks each remaining data/xattr reference against its bitmap and block count,
inode allocation against mode/free totals, and directory accounting. Live quota
matches proven remaining ownership. The host release stub deliberately does not
perform a second automatic final-reference cleanup; the explicit owner is exercised.
Earlier core and snapshot runs are retained (54978 and 63174 checks per mode).

`temp/p021-cleanup-retire-final.log`: 15445 retirement ordinary/sanitizer checks PASS,
including reclaim and sync after positive retirement with zero additional writes.
The preceding reclaim-only check passed 15251 checks per mode. Existing driver
regressions pass in `temp/p021-cleanup-driver.log`; pathname socket rollback passes
78 checks in `temp/p021-cleanup-socket.log`. Final supported builds pass in
`temp/p021-cleanup-pcat-final.log`, `temp/p021-cleanup-pc98-final.log` and
`temp/p021-cleanup-amd64.log` (earlier build logs retained).

Native features/remount/reboot PASS in
`plan/ws024/temp/ws025-p021-cleanup/results.json` and
`temp/p021-cleanup-native.log` (RUN 135, reboot VERIFY 26). This verifies ordinary
create/namespace, actual final-reference reclamation, quota, xattrs and snapshot
integration; forced failed-creation boundaries are established by the host fixture.
Source hashes: `temp/p021-cleanup-source.json`. Whitespace checks pass; all handles
terminal and ordinary amd64 restored.

q113/p021 remain in-progress. Until nlink stays zero through legacy preparation,
an initial cleanup transition that never commits can leave the old nonzero-link
unpublished state. Final name/link publication, backing integration and mount orphan
discovery/retry remain required next. Concurrent read pins, deferred metadata policy
and full phase acceptance also remain unchanged completion requirements.


## q113 directory backing checkpoint — 2026-09-07

Shared initial-block allocation now atomically initializes empty directory backing
and publishes CG/super/dinode ownership. `dir_add` retains committed backing across
later insertion failure. First xattr behavior remains verified by
`temp/p021-backing-xattr.log` (21660 checks per ordinary/sanitizer run).

`directory-backing-journal-host.c` exercises the actual common owner and `dir_add`:
first/next CG, preserved xattr payload and sibling dinode, full initialized backing,
zero-link lifetime within backing allocation, live/durable ownership and quota,
read/write/flush failures, landed writes, consecutive failures, volatile/immediate
power cuts, snapshot preservation failure and unsupported flush. Invalid directory
state, stale direct/indirect pointers, overflow, allocation failure and insufficient
journal capacity are rejected before mutation. Final ordinary/sanitizer runs pass
in `temp/p021-directory-final.log` (10675692 assertions each, including bytewise
payload/sibling checks; this is not a scenario count). Earlier backing-only and
insertion runs are retained in `p021-directory-backing.log` and
`p021-directory-insert.log`.

Driver regressions and 78 pathname-socket rollback checks pass in
`temp/p021-directory-driver.log` and `temp/p021-directory-socket.log`.
Supported builds pass in `temp/p021-directory-pcat.log`,
`temp/p021-directory-pc98.log` and `temp/p021-directory-amd64.log`.
Native features/remount/reboot PASS in
`plan/ws024/temp/ws025-p021-directory/results.json` with RUN 135 and
VERIFY 26 (`temp/p021-directory-native.log`). All processes are terminal; ordinary
amd64 remains restored. Source hashes: `temp/p021-directory-source.json`.

q113/p021 remain in-progress. The insertion fixture proves backing ownership,
not atomic final name/child/parent link publication. Zero-link creation through ACL
preparation, final publication, mount orphan ownership, concurrent read pins,
deferred policy and full phase acceptance remain required.


## q113 final creation publication checkpoint — 2026-09-07

new_inode keeps supported journal creations at nlink zero through preparation.
create/mkdir/mknod/symlink all publish through the shared parent-name/child-link
owner; mkdir also groups the parent's increment. Positive/uncertain publication
errors no longer discard a possibly named inode. Committed errors update live
links and invalidate caches; failed socket creation detaches the borrowed endpoint.

`temp/p021-creation-core-fixed.log`: 132818 ordinary/sanitizer checks PASS for
shared/distinct dinodes, directory/regular children, insertion layouts and size
growth, write/flush failures, landed/consecutive failures, volatile/immediate power
cuts, snapshot refusal and unsupported flush. Initial core compilation diagnosed a
fixture signedness warning; `temp/p021-creation-core.log` is retained.

`temp/p021-creation-vfs-final.log`: 1647533 ordinary/sanitizer assertions PASS for
actual create/mkdir/mknod/symlink callbacks, seven inode kinds, initially empty and
populated parent backing, read/write/flush failures, landed/consecutive errors,
power cuts, snapshot failure, preparation failure and unsupported flush. Replay
checks name versus child/parent nlink, initialized dot/dotdot/inline symlink, inode
map/mode/free totals/directory accounting and block references versus bitmap/free
totals. Live quota matches owned resources, with bounded conservative retention
only on uncertain readonly outcomes. The preparation test double asserts nlink zero
and uses the actual xattr publisher for modeled ACL storage; it does not replace a
full generic ACL acceptance test. Host inode_release remains a lifetime stub, while
native integration exercises actual references. Earlier compile signedness diagnostic
and pre-quota/snapshot run are retained (`p021-creation-vfs.log`,
`p021-creation-vfs-fixed.log`, 1522905 checks in the latter).

Driver regressions PASS (`temp/p021-creation-driver.log`), 78 pathname-socket rollback
checks PASS (`temp/p021-creation-socket.log`). Supported builds PASS in
`temp/p021-creation-pcat.log`, `temp/p021-creation-pc98.log` and
`temp/p021-creation-amd64.log`. Native features/remount/reboot PASS in
`plan/ws024/temp/ws025-p021-creation/results.json`, RUN 135 / VERIFY 26
(`temp/p021-creation-native.log`). Source hashes: `temp/p021-creation-source.json`.
Whitespace checks pass; all handles terminal, ordinary amd64 restored.

q113/p021 remain in-progress. Mount orphan discovery/reclamation must now consume
the recoverable typed zero-link states. Concurrent read pins, deferred scheduling,
full CRASH/META/WB/FLUSH acceptance and later WS025 phases remain required.


## q113 private mount orphan recovery checkpoint — 2026-09-07

Mount now reclaims typed zero-link allocated inodes after root/quota admission and
before publishing the mount. Normal namespace loads retain their zero-link rejection;
recovery uses a private decoded object outside the inode cache. Checked release and
retirement preserve quota, snapshot before-images and retry ownership.

`temp/p021-orphan-final.log`: 93754 ordinary/sanitizer checks PASS. Recovery is
interrupted at observed writes/flushes/reads, landed/consecutive failures, volatile
and immediate power cuts, snapshot failures and unsupported flush. Each outcome is
replayed, quota rebuilt, recovery retried and then repeated with zero writes. Live
neighbors, free maps/totals and directory accounting remain correct. Multi-CG,
empty-directory backing, readonly/nonjournal/published-mount exclusion, memory/slot
refusal and malformed raw inode/pointer checks also pass. Earlier retry-only runs
passed 93604 checks. Initial fixture diagnostics are retained: missing host mutex
initializer (`p021-orphan-core.log`) and a 90-second timeout from carrying simulated
pre-crash mutex ownership into reboot (`p021-orphan-core-fixed.log`). Reinitializing
volatile mount locks/context in the test's reboot path resolved that fixture defect;
`p021-orphan-retry.log` and the final run complete normally.

`temp/p021-orphan-creation.log`: 1949973 ordinary/sanitizer assertions PASS for all
seven actual creation callback kinds, including every previously exercised crash
state followed by actual orphan discovery, quota rebuild, reclamation and idempotent
retry. This extends the prior creation fixture through the mount recovery owner.
Driver decoder regressions PASS (`temp/p021-orphan-decode.log`); 78 pathname-socket
rollback checks PASS (`temp/p021-orphan-socket.log`). Supported builds PASS in
`temp/p021-orphan-pcat.log`, `temp/p021-orphan-pc98.log` and
`temp/p021-orphan-amd64.log`.

Native seeded-orphan features/remount/reboot PASS in
`plan/ws024/temp/ws025-p021-orphan/results.json`, including explicit
`orphan_recovery: PASS`, RUN 135 / VERIFY 26. `orphan-image.py` uses the canonical
formatter to seed unreachable file+xattr and empty-size directory backing owners in
different CGs (inode 255 and 511). `run-features-qemu.py --orphans` records the seed
and verifies both inode bitmap bits, mode/link fields, size and blocks are retired
after the real mount path and ordinary feature run. Source boot image is unchanged.
Logs: `temp/p021-orphan-native.log`; hashes: `temp/p021-orphan-source.json`.
All handles terminal; ordinary amd64 restored; whitespace checks pass.

q113's creation/backing/orphan cycle is finished with p021 uncleared against its
full criteria. Read pins/concurrent checkpoint visibility, remaining metadata profile
coverage, deferred policy and full CRASH/META/WB/FLUSH acceptance remain required.


## q114 immutable reader checkpoint — 2026-09-07

The core now admits immutable image views with an atomic closed bit/reader count.
Checkpoint device I/O proceeds while pinned readers copy covered extents without
I/O. Slot retirement closes new acquisition; retained pins prevent payload reuse,
replay overwrite, rebinding or detach. View copy rejects uncovered/invalid requests
before touching the destination. An already admitted reader keeps positively
committed bytes even across failed home checkpoint; unresolved poison closes fresh
admission. VFS read_block and CG loading try these views before the journal mutex.
CG redo copies discard stale home-view identity. Uncovered reads retain serialization.
Writers drain retired pins before admission; mount backing teardown closes/drains
readers before freeing the accounted physical image.

`temp/p021-view-core.log`: ordinary/sanitizer concurrent checkpoint/view/reuse PASS
(36 deterministic blocked-checkpoint scenarios per run with additional racing reads;
assertion totals vary with scheduling). Home write/flush failures, retained generations,
partial coverage, unchanged refusal buffers and reuse/detach refusal are covered.
`temp/p021-view-vfs.log`: 69 checks PASS per ordinary/sanitizer mode for actual
metadata and CG reads during a paused checkpoint, uncovered-read fallback after pin
release, writer backpressure, and physical image teardown waiting for a reader.

`temp/p021-view-crash-final.log`: journal crash/reuse 39193 checks per mode, snapshot
physical flush and image-owner accounting PASS. Allocation 104106, creation/orphan
1949973 and rename 234341 checks per ordinary/sanitizer mode PASS in
`p021-view-allocation.log`, `p021-view-orphan-creation.log`, `p021-view-rename.log`.
Driver regressions PASS (`p021-view-driver-final.log`), pathname socket rollback
78 PASS (`p021-view-socket.log`), legacy namespace write/rollback 83 PASS
(`p021-view-fsync-fixed.log`). Initial driver/legacy fixture link diagnostics are
retained; nonjournal-only test doubles now provide explicit no-view stubs, while
all new view and recovery fixtures link the real journal core.

Supported builds PASS (`temp/p021-view-pcat.log`, `p021-view-pc98.log`,
`p021-view-amd64.log`). Native features/remount/reboot plus seeded orphan recovery
PASS in `plan/ws024/temp/ws025-p021-view/results.json`, RUN 135 /
VERIFY 26 (`temp/p021-view-native.log`). Source hashes: `temp/p021-view-source.json`.
All handles terminal, ordinary amd64 restored; whitespace checks pass.

Coverage review: serialized xattr writes are already capped at one filesystem block
by extattr_publish; legacy two-block areas can be read, shrunk or cleared through
the verified owner. New two-block publication is not an existing supported API.
Standard formatter geometry (8 KiB blocks) fits the complete current group families;
oversized/nonjournal pre-admission fallbacks remain synchronous. Deferred publication
must separately join full-slot draining, mount/worker/fsync/snapshot boundaries and
error epochs before enabling policy. q114 cycle finished, full p021 uncleared.


## q115 opt-in deferred checkpoint integration

Implemented existing-policy admission, one-slot prefix draining, fresh context
ownership, fsync/snapshot/clean-super boundaries, sector-granularity redo reads,
and mount metadata failure history with independent file-description cursors.
All q115 handles are terminal; ordinary amd64 is restored. q115 is finished with
full p021 uncleared pending its complete acceptance coverage, not an implementation
claim for later WS phases.

Evidence (paths below are relative to WS025 unless otherwise stated):

- `temp/p021-deferred-view-3.log`: actual UFS/core/io-error paths, 240 checks per
  ordinary/sanitizer run. Pending-prefix sync, read visibility, explicit through,
  policy off drain, indirect pointer, recovered original error, retained ledger,
  second-failure poison, remount and immutable-reader retirement all pass.
- `temp/p021-deferred-creation-1.log`: 3,889,360 checks per mode across synchronous
  and deferred creation, seven inode types, existing/empty parents, failed writes,
  flushes, double failures, snapshot preservation, crashes/replay/orphan cleanup.
- `temp/p021-deferred-policy-2.log`: real policy/worker query on/off/pause/rollback,
  pressure/age, sibling-device progress and failed drain retention pass. The first
  run exposed a fixture race: its fake clock advanced before the pressure pass
  installed its next deadline. Waiting for the pass's busy flag to clear fixes the
  deterministic test boundary; the initial timeout remains in policy-1 logs.
- `temp/p021-deferred-file-cache.log`: actual file/VM and descriptor observers pass,
  including shared metadata failure, independent opens, dup and repeated errors;
  ordinary 424,711 / sanitizer 427,629 checks plus formatter reservation 626 each.
- `temp/p021-deferred-driver-3.log`, `p021-deferred-socket.log`,
  `p021-deferred-fsync.log`: legacy driver/data-run, socket and directory mutation
  regressions pass. driver-1/2 preserve initial legacy mock compile/abort diagnostics;
  non-core stubs now accept only an empty non-poisoned drain, never simulate pending
  recovery. Real recovery fixtures continue to link the production core.
- `temp/p021-deferred-core.log`: crash/reuse 39,193 per mode, snapshot flush and
  image accounting pass. Earlier allocation regression 104,106 per mode passes in
  `temp/p021-deferred-allocation-1.log`.
- `temp/p021-deferred-pcat.log`, `p021-deferred-pc98.log`,
  `p021-deferred-amd64.log`: all supported builds pass with make -j16.
- `plan/ws024/temp/ws025-p021-deferred/results.json`: QEMU USB boot,
  disposable NVMe journal+snapshot profile, explicit metadata policy, features,
  snapshot/quota/xattr/namespace, off/on, unmount/remount, reboot and seeded orphan
  recovery PASS (RUN 141 / VERIFY 26). Runner output `temp/p021-deferred-native.log`.
  Physical acceptance remains user-accepted, not agent-measured.
- Exact selected source hashes: `temp/p021-deferred-source.json`.

Next q116 broadens deferred-mode failure injection to allocation, truncate and
namespace families, checks every CRASH/META/WB/FLUSH criterion against actual
scope, and runs the final FS50/Wi-Fi30/native storage regression. No pending
criterion is silently treated as covered by the narrow native feature count.


## q116 final acceptance — p021 complete

The deferred-policy mode now runs the actual production grouping/core under the
same failed-write, failed-flush, second-failure, crash/replay and preservation
oracles as synchronous mode. `tests/run-allocation-journal-host.py` accepts the
explicit `--deferred` test option; it affects only the host policy adapter.
The production policy/worker and native enable paths were verified separately.

| Deferred family | Checks per ordinary/sanitizer run | Log under temp/ |
| --- | ---: | --- |
| allocation | 105229 | p021-accept-allocation-deferred.log |
| truncate release | 91146 | p021-accept-release-deferred.log |
| unlink | 28010 | p021-accept-unlink-deferred.log |
| hard link | 66334 | p021-accept-link-deferred-2.log |
| rmdir | 39243 | p021-accept-rmdir-deferred-2.log |
| rename | 239363 | p021-accept-rename-deferred-2.log |
| final inode retirement | 15861 | p021-accept-retire-deferred-2.log |
| xattr release | 35262 | p021-accept-xattr-release-deferred-2.log |
| xattr replace | 35983 | p021-accept-xattr-replace-deferred-3.log |
| first xattr allocation | 21801 | p021-accept-xattr-allocation-deferred-3.log |
| initialized inode reservation | 19120 | p021-accept-inode-reservation-deferred-3.log |
| checked creation cleanup | 53900 | p021-accept-creation-cleanup-deferred-3.log |
| first directory backing | 10676731 | p021-accept-directory-backing-deferred-3.log |

Matrix-1/2/3 JSON files retain exact commands and terminal statuses. The initial
link and xattr-replacement boundary fixtures assumed immediate home installation
on callback success. Their corrected checks perform explicit sync before inspecting
raw homes; replacement also checks immediate redo-aware visibility. Their original
failures are retained; no production condition was relaxed to pass them. The
q115 creation matrix additionally covers all seven actual creation callbacks in
both policies and mount orphan recovery after their crash cuts.

`temp/p021-accept-owner-gates.log` passes real VFS failed-unmount rollback (153 per
mode) and flush frontier (77 per mode). These include pending logical owners,
current/historical failure, out-of-order completion, short/error/reset and captured
flush targets. `temp/p021-accept-fs50.log` and
`plan/ws018/temp/ws025-p021-final/results.json` record FS50/50
with no unrun items, Wi-Fi30 in both variants, and two native USB boots. Their
manifest/source hashes retain complete build and runner provenance. Native includes
actual overlay copy-up, journal/temp/rename/directory-fsync and settings persistence.

`temp/p021-accept-writeback-usb/results.json` passes the combined data/metadata
policy on a disposable USB UFS journal-snapshot profile, including batching,
repeated explicit fsync, worker age, off/on, unmount drain and remount verification.
Its profile is explicit in filesystem-profile.json; earlier ordinary-profile data
WB results are not silently relabeled. `temp/p021-accept-writeback-usb.log` records
three worker passes and final zero mounts/workers/dirty/reserved/tickets/errors.
The separate q115 NVMe feature run covers active snapshot, quota, xattr and seeded
orphan recovery in deferred mode. Supported production builds from q115 remain
exact: all selected src/include hashes in p021-deferred-source.json are unchanged.
q116 changed only maintained test adapters/runners and planning records.

| Contract | Evidence and applicable scope |
| --- | --- |
| CRASH01, META04–META07 | Real allocation/release/creation families initialize data before grouped pointer publication, reserve quota conservatively, inject every write/flush and repeated failure, and replay private orphan cleanup. Both policies pass; unsupported geometry declines before grouped admission. |
| CRASH02–CRASH04 | q115 core volatile/torn/reordered media/repeated replay (39193 per mode), q114 immutable pinned readers, q115 prefix drain and poisoned retry (240), and q116 deferred operation matrices. Committed evidence survives retirement; old-slot pins cannot be overwritten. |
| CRASH05 | Namespace/rename/truncate/retire/xattr matrices above, creation quota/snapshot-preserve faults, native snapshot/quota/xattr features, and FS50 overlay persistence. Distinct overlay fsync boundaries continue to drain metadata through the common backend fence. |
| CRASH06 | Host sync assertions inspect durable home bytes independently of cached redo, preserve original errors and poison repeated failure. Native successful file fsync, off/on, unmount/remount and reboot preserve expected bytes and namespace. Unsynced committed redo is allowed to survive and is required to replay atomically. |
| META01–META03 | Existing adapter/CG generation/indirect tests plus q114 CG pinned visibility and q115 sector-sized redo lookup. Driver regressions retain allocation/truncate/indirect behavior; no unbounded per-inode cache is added. |
| META08–META10 | Existing p012/p013 FAT cursor/mirror/loop synchronous batching contracts remain unchanged. Final real FAT/claim/loop/overlay FS50 and file/VM mutation/lifetime tests pass; FAT operation-to-operation metadata delay remains disabled. |
| WB01–WB05 | Real file/VM observer tests (q115), real UFS metadata error ledger and failed retry, mount frontier and failed unmount. Independent descriptions observe shared metadata failures separately, dup shares its cursor, and a successful retry cannot erase retained history. |
| WB06–WB10 | Combined USB data+metadata native, q115 real policy/worker rollback and deadline tests, file/VM redirty/through-context tests, bounded one-slot pressure/read-pin drain and q116 VFS teardown tests. Metadata uses its precharged single image, not data dirty tickets or an unbounded queue. |
| FLUSH01–FLUSH06 | q116 production frontier and unmount gates, unchanged p014 proof ownership, q115 sync/snapshot/clean-super fences, explicit through-context group tests, native data WB and full overlay storage regression. No successful upper sync is inferred solely from an already-stable leaf. |

Remaining limits are deliberate phase scope: nonjournal/oversized grouped geometry
keeps synchronous fallback; only explicit policy enables supported metadata delay;
one pending committed slot bounds retention and subsequent admission performs
backpressure. Writable data read-ahead/cache behavior and FAT batching retain their
separate owners. Physical acceptance is user-accepted, not measured by the agent.
No commit or aggregate make check was run. All handles are terminal, ordinary
amd64 is restored, and git diff --check passes. Later WS025 phases remain required.
