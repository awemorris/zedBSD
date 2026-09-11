# UFS ordered metadata transaction design

Status: selected staged design, q111 / p021 in progress. No runtime metadata delay
is enabled until the owner, format profile and integration gates below are complete.

## Durable group and bounded slot

User decision: no journal v1 compatibility is required for this unreleased OS.
Replace the journal profile with the sole multi-extent version 2 format, using
ZUJ2 locator/version 2 and rejecting obsolete recognized locators.
The slot describes up to 30 disjoint home extents in one 512-byte descriptor.
Each entry carries 64-bit home sector, 32-bit sector count and payload checksum.
The descriptor binds sequence, extent count and aggregate payload sectors; its
checksum covers all descriptor bytes with the checksum field zeroed. The commit
record binds sequence and descriptor checksum, preventing substitution of targets.
The commit sector is fixed at the slot's final sector. Payloads occupy the prefix
between descriptor and commit. Aggregate payload is additionally capped at 128 sectors (64 KiB).
Slot capacity and caller-provided home-volume size
are checked before any I/O, including overflow, overlap and journal exclusion.

One bounded committed group may remain in the journal until checkpoint. Publication
must invalidate old commit durably, write descriptor/payload, flush all redo data,
then write/flush the commit. Home blocks are untouched until a checked checkpoint.
Checkpoint validates the entire descriptor and all payload checksums before any
home mutation, applies every extent, flushes homes, then clears the descriptor and
flushes again. Recovery replays the same immutable redo group; repeated interruption
is idempotent. A malformed committed record stops explicitly. No uncommitted group
is installed. An occupied slot cannot be overwritten to relieve log pressure.

First implement the core codec and synchronous group commit/recovery as a bounded
foundation. Then split durable publication from checkpoint, retaining the group's
read visibility and errors; finally connect opt-in VFS transactions. Core tests
alone never prove filesystem metadata write-back completion.

## VFS ownership still to implement

The current mount lock already protects allocation metadata and shared dinode
read/modify/write, while inode/namespace locks protect individual operations.
A transaction must hold one explicit metadata mutation owner from reservation to
publication; it cannot infer ownership from the currently executing thread. Reads
of metadata must see staged/committed group images until home checkpoint. Every
metadata adapter needs an explicit transaction context; ordinary file data keeps
its own VM owner. A block containing multiple dinodes may include only prepared
content generations. Reservation occurs before modifying persistent/in-memory
allocation state. Full slots cause checked checkpoint/backpressure before admitting
another mutation, not unbounded allocation or partial group publication.

Data initialization must complete and cross its durability frontier before a
metadata commit can publish its pointer. Allocation map and new references belong
to one redo group; unlink/truncate pointer removal and free-map publication likewise
need a single prepared group or a documented durable intermediate state. Failed
preparation rolls back private state; uncertain durable commit retains allocations
and stops further mutation until replay/checkpoint establishes ownership.

Snapshot preservation must precede each first home overwrite and use the same
retained physical/context owner. Quota/xattr/directory/rename and overlay boundaries
must join metadata ownership and fsync epochs. A versioned formatter/mount profile
must exist before opt-in is exposed; FAT remains synchronous. The p021 acceptance
matrix and p026 integration retain all these requirements.


Implemented core layout (little endian): descriptor magic/version at 0/4,
sequence at 8, extent count at 16, total payload sectors at 20, reserved zero at
24, descriptor checksum at 28; 30 entries begin at 32 with target/count/checksum
at entry offsets 0/8/12. Commit magic/version at 0/4, sequence at 8, descriptor
checksum at 16, reserved zero at 20, checksum of first 24 bytes at 24. Remaining
bytes are zero when written. Core initialization receives the real home volume
size and rejects a bound reaching the locator/journal region. Replay validates
all descriptors/payloads before any home write. Existing commitv immediately
checkpoints; deferred publication and VFS visibility remain subsequent steps.


User steering applied: there is one reader/writer, `ufs_journal_commitv`; the
single-extent `ufs_journal_commit` convenience API uses that same group format.
The separate v1 implementation and v2-only init entry point were removed. All
initializers receive home-volume geometry explicitly. Host Python/C and target
mkfs now generate ZUJ2/version 2; no legacy locator is silently treated as a
non-journal volume. Existing baseline images without a journal are unaffected.
Generated journal-profile images must be regenerated for this unreleased format.


Publication checkpoint now carries an expected (sequence, descriptor checksum)
witness internally. A missing/changed committed identity is EIO for the publishing
caller, even though boot recovery can legitimately discard uncommitted records.
Keep that witness in the future pending owner when splitting publish/checkpoint;
do not use permissive boot replay as proof that a newly requested commit succeeded.


## Explicit pending core ownership

Implemented publication/checkpoint separation retains sequence, descriptor digest,
readiness and home-durable/clearing state in the journal owner. Publish retains no
caller payload pointer and installs no home data. A failed/uncertain publication
blocks coherent reads and reuse until explicit recovery. Known committed identity
remains strict during replay; another commit cannot implicitly drain somebody
else's pending group. Once homes have crossed flush, retry only descriptor retirement
and serve reads from durable homes, even if descriptor clearing failed.

The checked read API overlays home gaps with committed redo, independently of
extent table order, and coalesces the returned data spans. The mount now supplies a shared-budget immutable redo image. One bounded read
loads the payload after descriptor/commit checks; all checksums pass before that
image becomes visible. Pending redo reads use RAM without media I/O, and checkpoint
writes each extent in one call. The standalone recovery core can still use sector
scratch when no image is supplied. No extra unaccounted permanent cache is allowed.
Concurrent read pins remain required before runtime deferred metadata is enabled;
the current API is serialized and does not yet allow a read during checkpoint I/O.

VFS read_block now chooses checked pending redo under journal serialization.
Bounded allocation runs use grouped metadata publication and synchronous checkpoint;
other writers still use the synchronous single-extent adapter.
The next step must account for every remaining metadata reader (including cached CG views),
mount/disk/context lifetime and snapshot preservation while adding operation-level
transaction ownership; do not enable a partial metadata-delay path.


The VFS pending owner also needs a recovery outcome distinct from errno: a failed
publish may nevertheless have a durable commit that replay installs. Retain the
transaction witness/outcome across recovery and slot retirement so in-memory
allocation rollback cannot free a committed reference. Do not infer rollback safety
from an empty slot or a recovered EIO alone. Current synchronous adapters retain
their existing conservative rollback/readonly behavior; grouped VFS admission must
make this outcome explicit.

Keep a read pin on the future immutable pending image so a cached metadata read can
copy bytes while checkpoint performs device I/O. A mutex held across device flush
must not become the only way to read already-validated pending metadata. Include
cached-CG view invalidation/visibility in the same design rather than hooking only
read_block and leaving another metadata reader on stale home buffers.


## Recovery outcome witness

Implemented `ufs_journal_committed` reports positive proof for an exact sequence
and descriptor digest after validation of the entire committed redo group. The
proof survives checkpoint/slot retirement, is replaced only by another verified
commit and resets on initialization. The serialized transaction owner must query
it before admitting the next group. A false result does not establish rollback
safety while pending I/O is unresolved; recovery/checkpoint must establish that
boundary separately. Tests distinguish durable commit despite publish error from
a lost commit, then verify retirement, replacement and initialization semantics.

CG bitmap and superblock totals currently use separate synchronous writes. Their
existing mount-owned images offer a bounded first integration point, but grouping
those two writes alone does not atomically couple allocation with dinode/indirect
references. The VFS design must retain that larger operation boundary, preflight
combined journal capacity, and preserve all snapshot before-images before home
mutation. Do not claim a two-target adapter completes allocation transactions.


## Mount redo backing

A journal-profile mount reserves physical backing for the bounded 64 KiB payload
plus descriptor before replay or write admission. The actual allocator-rounded
size is charged to CACHE_MEMORY_BUF_META; allocation or budget refusal unwinds
completely. Failed mount recovery and final unmount release the backing and charge
only after journal callers have been excluded. The core borrows that storage and
retains no caller payload pointer. Successful retirement invalidates the image;
uncertain home writes keep it for retry. Rebinding is forbidden while pending.

This buffer is the current mount-owned immutable image, not a general data cache.
Its lifetime is bounded by mount ownership and the shared budget. Before deferred
checkpoint is enabled, add explicit read pins and make retirement wait for readers;
current external serialization remains the authoritative contract. Operation-level
CG/dinode/namespace staging is still required and is not inferred from this buffer.


## Bounded allocation operation integration

Full-block runs with direct pointers or an existing indirect leaf now preflight
the combined footprint before quota/CG reservation. Under the mount mutation lock,
they retain the original CG and prepare a private inode/leaf. Initialized data
crosses flush before CG bitmap, super totals, shared dinode and optional leaf are
published as one journal group. Shared dinode preparation retains unrelated slots;
all metadata snapshot before-images are preserved before the first group home write.
The write epoch and logical I/O context span that complete metadata commit.

A sequence is unique within the serialized journal owner's lifetime. The adapter
captures next_sequence before commitv and inspects positive committed_sequence
before releasing admission. A recovered commit retains allocations and publishes
the prepared in-memory inode even when the original errno remains an error. If
recovery remains uncertain, mutation is stopped, allocations/charges are retained
and the dirty CG remains explicit. A definitely uncommitted group restores only
private CG state; it does not issue compensating metadata writes. Metadata block
reads consult pending/poison state under journal_lock, returning EIO for unresolved
poison rather than consuming an old indirect leaf.

This is the first real operation-level group, not complete metadata write-back.
Bounded missing indirect paths are now prepared in the same allocation group.
Oversized/fragmented reservations and other metadata paths retain their previous
ordered synchronous behavior. Namespace/unlink/free, quota
configuration/xattr transactions and concurrent immutable read pins remain required.
The current data path still uses its existing journal adapter; ordered data-only
routing and deferred checkpoint policy must be resolved with the complete operation
owners, not inferred from this bounded allocation optimization.


## Missing indirect path ownership

The allocation run now prepares a missing suffix of up to three indirect nodes,
including the last existing parent when its child is absent. Every new image is
zero-initialized privately; reserved addresses are patched only into private path
images and the private inode. CG, super totals, dinode and all changed path blocks
share one commit. Data crosses flush before those references become durable.
New tree blocks count toward bitmap, inode blocks and quota reservations; an
insufficient metadata-plus-data reservation is released without publication.

Mount mutation ownership now begins before the first indirect-path read and lasts
through reservation/publication/rollback. This closes the previous interval in
which a copied parent could become stale before acquiring the allocator lock.
The lock order admits the quota device-rank lock under the mount inode-rank lock.
Prepared path storage is operation-local and freed on all ordinary exits; the
journal retains its own immutable image for uncertain recovery.

The footprint is preflighted before quota/CG changes. A bounded contiguous run
that cannot fit metadata plus at least one data block declines without disk writes,
allowing the existing fragmented allocator to handle it. Oversized metadata groups
similarly retain the synchronous path. These fallbacks and namespace/free operations
remain explicit work before enabling complete deferred metadata behavior.


## Bounded truncate release groups

Allocation and release share `metadata_group_commit`, which owns snapshot
before-images, the logical write epoch, serialized journal admission and explicit
committed/uncertain outcome. The release owner prepares CG and inode/parent images
privately, validates the existing pointer and allocation, and journals pointer
removal, inode block count, CG free map and global free totals together. Optional
CG cache pins are dropped before checkpoint may overwrite the home block.

Each release is a durable intermediate state: a large truncate can leave an old
file size with coherent holes after failure/power loss, while no freed block stays
referenced and no removed block is lost from free/block accounting. Empty indirect
parents remain owned until the next release removes them. The final size update
retains its existing separate synchronous commit; whole-file crash atomicity is
not claimed. Partial tail zeroing retains its existing ordered data behavior.

Live pointers, free totals and quota release change only after positive committed
proof. Uncertain recovery stops mutation and preserves explicit dirty ownership.
A definitely uncommitted release needs no compensating disk write. Readonly/errno
handling remains separate from committed ownership. An explicit handled result
selects the old path only before admission (non-journal/oversized footprint);
EOPNOTSUPP from a real I/O callback never triggers fallback after an attempted group.

Full namespace/inode-number release and other metadata owners, fragmented or
oversized cases, concurrent read pins and deferred checkpoint policy remain before
p021 completion. The allocation/release boundaries do not enable metadata delay.


## q112 unlink namespace owner

A bounded journal unlink now prepares directory record removal and target nlink
in one group. Namespace admission excludes competing namespace operations; directory
and target inode locks plus the mount lock protect preparation through commit.
The target's shared dinode block is read/modified privately, preserving unrelated
slots. Successful or recovered committed ownership updates live nlink/DEAD state;
the original errno still propagates. Non-journal/oversized fallback is selected
only before admission with a handled result, not from an I/O errno.

Generic VFS cache invalidation runs only after success. For committed or uncertain
errors the filesystem therefore removes the name-cache entry and advances dirseq
after inode/mount locks are released. New directory reads reject poisoned journal
state rather than reading stale direct-block bytes after that invalidation.
The actual name-cache implementation is exercised by the focused host fixture;
normal successful wrapper invalidation remains covered by native namespace tests.

An allocated zero-link inode remains the durable marker for an unlinked object
until its last live reference closes. Crash-time reclamation of that orphan and
atomic inode-number retirement are still required work, not proven by name/nlink
agreement. Subsequent link/rmdir/rename need shared-dinode image merging and their
own live-outcome contracts; inode_link's success-only generic increment is explicit.
No deferred metadata policy is enabled by this step.


## q112 hard-link shared-dinode merge

Dinode encoding is now separate from block loading. A hard-link group prepares
the target's incremented nlink and the directory's resulting size. If their dinodes
share a physical block, both slots are encoded into one image and one redo extent;
otherwise each gets its own image. The directory data block and those deduplicated
metadata images commit together. Footprint admission uses the actual shared versus
distinct block count, so a two-extent slot is sufficient for a shared block.

The private directory builder validates the complete existing directory before
editing it, rejects duplicate names, reuses empty records/occupied-record slack,
and can extend size within the already allocated block. Malformed/full/name/space
or memory failures publish nothing. Directory-block allocation and oversized groups
still select the existing path before admission; no I/O errno requests fallback.

On success, the generic inode_link wrapper retains responsibility for the live
nlink increment. On committed error the filesystem applies the prepared live nlink
itself because that wrapper will not increment; the original errno propagates and
namespace cache/sequence are invalidated. Directory size is published for every
established commit. Native validation covers the actual generic wrapper, while
host tests assert the callback/wrapper count convention and actual name-cache misses.

Further namespace owners, orphan/inode-number retirement, read pins and deferred
checkpoint remain. This two-inode merge is not a general rename transaction builder.


## q112 empty-directory removal

The private removal owner now serves unlink and rmdir. Rmdir journals the parent
record removal, parent nlink decrement and target nlink zero together. Shared
parent/target dinode blocks merge into one image; distinct blocks get separate
extents. Existing emptiness/type validation precedes the group, and dot/dotdot
removal is rejected before namespace mutation. Committed outcome updates both live
counts and target DEAD state; error cache invalidation uses the existing explicit
owner after releasing inode/mount locks.

The target remains an allocated zero-link directory until final-reference cleanup.
Its data and inode-block count are not prematurely freed by the namespace group;
CG directory accounting follows actual inode retirement. Crash-time orphan cleanup
and inode-number retirement remain required. No inode reuse or reclamation is
claimed merely from successful entry/nlink removal.

Native acceptance now creates a child directory, refuses rmdir while it contains
a file, removes that file, captures a snapshot, and removes the directory. It checks
parent nlink restoration, absence from the live namespace, retention of the old
directory/nlink in the snapshot, and persistent absence after remount/reboot.


## q112 general shared-block staging for rename

A private metadata image set owns caller-allocated bounded memory and a list of
full filesystem-block extents. Its lookup returns the same mutable image for an
identical physical block and reads a new block only once. Partial overlaps are
rejected. Capacity is the minimum of reserved bytes, journal payload/descriptor
limits and the current slot capacity. A failed read does not admit an extent.
Prepared inode encoding changes its slot in the shared image; it never reloads a
block after another inode has edited it. The mount lock covers preparation through
commit, and all buffers outlive synchronous metadata_group_commit.

Hard link becomes the initial production consumer. Rename will reuse this owner
for both parent data blocks, changed dinodes and dotdot; same-directory moves must
edit one directory image. Namespace and inode locks, positive commit publication,
error cache invalidation and orphan retirement remain operation responsibilities.


Rename consumer integration requirements from current-source inspection:

- The generic inode_rename wrapper checks ancestry, source/target mutation guards
  and types, but invalidates parent names and the source directory sequence only
  after a successful callback. The filesystem must publish that invalidation on
  committed/uncertain errors as well. It must preserve the original errno.
- Prepare the destination insertion/replacement and source removal in private
  directory images. For same-parent rename, insertion and removal must use the
  same evolving bytes and size; a helper must not reload the original home image.
- A cross-parent directory move changes source dotdot and both parent nlinks;
  replacing a directory also removes one destination-parent link. Encode each
  unique prepared parent once so same-parent accounting cannot overwrite itself.
  A replaced target's nlink and DEAD state follow established commit outcome.
- Deduplicate inode locks before acquisition (old/new parent may be identical),
  and retain namespace exclusion through validation, commit and cache publication.
  Reject impossible aliases and link-count underflow/overflow before publishing.
- Preserve all before-images via metadata_group_commit. No post-commit rollback
  writes are permitted. Last-reference cleanup/orphan recovery remains a distinct
  persistent ownership requirement, not proof supplied by namespace atomicity.


## q112 rename group implementation

Rename of existing directory backing now uses private images for source removal,
destination insertion/replacement, directory dotdot and parent/target dinodes.
Same-parent operations share one prepared parent inode and one evolving directory
block. Removal precedes private insertion so a full directory can reuse the old
name's space; no removal is persistent before the complete group commits.
Directory record changes validate the complete image and expected inode number.
The insertion helper is shared with hard link and updates only its prepared size.

The callback retains the existing flags/type/emptiness and same-inode checks before
admission. The grouped owner rejects parent/source aliases, link-count underflow
and destination overflow. Four inode locks are deduplicated under namespace
exclusion; up to six candidate physical blocks are deduplicated to reserve the
actual footprint. Existing journal/profile capacity and unallocated destination
backing are declined before mutation with an explicit handled flag. No errno from
an admitted transaction triggers the historical rollback path.

A proven commit publishes prepared parent sizes/counts and target nlink/DEAD.
Committed/uncertain errors invalidate both names and directory sequences, including
the moved directory's cached dotdot, after inode/mount locks are released. The
original errno propagates. Generic VFS retains its success cache bookkeeping.
Host tests call the actual grouped owner with the already-resolved inode set and
simulate generic success invalidation; native tests exercise real lookup, ancestry,
callback and final-reference behavior. New backing allocation and persistent
orphan/inode-number retirement remain required, as do read pins/deferred policy.


## q112 final inode retirement

The final-reference reclaim callback retains its data truncate and xattr teardown,
then admits a private group containing the empty dinode, CG inode bitmap/free count
and super free-inode total. Directory retirement decrements CG/super directory
counts in that same group. Before admission the inode must be outside reserved/root
numbers and inside the mount inode range; under inode/mount locks it must have zero
nlink, size, block count, direct/indirect and xattr ownership. The caller owns the
final-reference lifetime; this helper does not establish reference exclusion itself.

The live CG is unchanged until positive commit. Optional CG cache pins are released
before checkpoint. Positive retirement clears live mode/type, updates totals and
releases the inode quota charge exactly once, including a recovered committed error.
Uncertain outcome retains accounting conservatively, marks the CG dirty and follows
the common read-only poison boundary. The original errno and handled flag remain
separate, so an admitted error cannot trigger the old independent free path.

This closes only final inode-number retirement after content teardown. Mount-time
orphan discovery/retry, abandoned create reservation, xattr release atomicity and
creation's allocation/reference transaction remain required. A successful final
retirement test is not evidence of crash-orphan discovery or complete create safety.


## q112 complete xattr area teardown

Clearing an xattr area now groups removal of both possible references, area length,
inode block count and affected CG/super free-block accounting. All serialized
attribute records are still validated by extattr_load before normal publication.
A two-block area is never shortened into a potentially invalid record prefix.
The grouped helper validates pointer/size agreement, distinct aligned allocated
blocks, remaining block accounting and geometry. Both affected CGs may be distinct;
each is loaded once, with its own validated bitmap offset, and edited privately.

Pre-admission capacity uses the actual distinct-CG footprint. One CG plus dinode
and super images can accept two blocks even in a slot that cannot hold two CGs.
The inode lock spans preparation and commit; the mount lock protects allocation
maps and total accounting. CG view pins are dropped before checkpoint. Positive
commit publishes zero xattr references/size, the reduced block count, free total
and block quota release; uncertain state keeps conservative ownership/read-only
poison. No admitted errno takes the historical compensating-free path.

Nonempty attribute replacement and first allocation remain separate required
integration work. Snapshot preservation occurs before changed allocation maps and
dinode homes, while old xattr content is left intact; any later reuse must still
preserve its own before-image through the existing snapshot write owner.


## q112 existing xattr replacement

The shared existing-area owner now retains/replaces block zero for a nonempty
area and atomically releases any old second block. It validates that retained
backing is allocated, stages a zero-padded full payload image, and publishes it
with the prepared dinode length, pointers and block count. Released backing uses
the same validated private CG maps and positive-outcome quota rules as clear.

A one-block replacement has exactly two extents (payload and dinode); unchanged
CG and super totals are excluded. Two-to-one shrink adds only the released block's
CG and changed super totals. All snapshot before-images are preserved before any
home overwrite. Capacity declines happen before admission; callback errors cannot
trigger old payload/metadata compensation. First allocation remains outside this
owner until it can group allocation, initialized payload and inode reference.

Inspection of inode_creation_prepare confirms ACL inheritance/preservation can
call child xattr operations before namespace publication. Creation integration
must preserve a recoverable zero-link reservation during such preparation, then
publish the name and final link state atomically. Existing new_inode currently
publishes a linked dinode before its directory entry; this remains required work,
not a state accepted merely because the existing-area update is now atomic.


## q112 first xattr allocation

The first-area owner reserves one block of quota, selects a complete free block
under mount exclusion, and changes only private CG bytes. Initialized zero-padded
payload, CG/super allocation and prepared dinode reference/length/block count are
four extents of one group. No separate allocator zero/write or compensating free
runs for an admitted operation. Exhausted/fragmented capacity and malformed summary
accounting are distinguished without publishing private reservations.

A positive commit publishes live reference/block count, CG/free total and rotor.
Positive or uncertain outcome retains the quota reservation; definite uncommitted
failure rolls it back. Uncertainty follows the common read-only/dirty boundary,
so an invisible but potentially durable allocation cannot be reused before replay.
Snapshot before-images cover every target, including the previously free data
block. The caller retains inode exclusion through the full owner lifetime.

This supplies the first-allocation path used by ordinary xattr and ACL preparation.
It does not make a currently named-or-unnamed inode's nlink protocol atomic by
itself. Creation must still start from a recoverable zero-link inode reservation
and group final name/link publication; mount orphan recovery remains required.


## q113 initialized inode reservation

Journal-backed new_inode now allocates the in-memory object before reserving its
number. A private zero-link dinode, inode bitmap/free total and directory accounting
are committed together. The selected slot is fully cleared before encoding, and
its generation advances (wrapping zero to one), preserving neighboring dinodes.
The initialized mode/uid/gid/device identity makes a committed reservation readable
without interpreting stale previous-owner fields. Directory count is not incremented
again by the existing completion path.

Positive commit publishes only inode identity fields, never copied mutex/reference
state. Quota is retained for committed/uncertain outcomes and rolled back only for
definite uncommitted failure. A failed reservation releases the new cache object;
known zero-link identity can follow ordinary final-reference cleanup, while a
poisoned mount preserves persistent ownership for recovery. Memory/profile admission
precedes mutation; unsupported profiles retain their prior path.

This first integration still assigns the caller's legacy nlink before ACL preparation
and final name publication. It therefore does not yet establish a zero-link lifetime
through all creation. The next creation owner must move that link transition into
the name transaction and replace independent rollback cleanup. Mount-time recovery
of surviving zero-link reservations also remains required. These are unchanged
p021 completion conditions, not optional follow-ups.


## q113 checked cleanup of unpublished creation

Supported journal creation cleanup now detaches borrowed socket state, commits a
zero-link dinode without dropping resource references, and calls an errno-returning
reclaim owner. That owner releases file backing, clears the attribute area, then
retires the inode number through the previously verified grouped owners. Each
intermediate failure preserves reachable ownership for the remaining resources;
no independent pointer-zeroing/free sequence is used for admitted reservations.
The caller must already establish name absence. A committed publication may not
be sent to this cleanup path.

The VFS final-reference callback wraps the same checked reclaim owner, retaining
its void ABI. Explicit cleanup and future mount recovery can now observe errors.
After positive final retirement, the live inode number becomes zero even if the
operation returns an I/O error. This prevents a later final-reference retry from
writing through an identity whose allocation bit is already reusable. Mode/type,
quota and bitmap ownership continue to follow positive commit evidence.

The current legacy creation completion can still make nlink nonzero before name
publication. If the initial cleanup unlink transition itself never commits, that
pre-existing state remains; this patch does not claim automatic discovery of such
an inode. Keeping nlink zero through all preparation and atomically publishing its
final name/link state remains required next, along with backing integration and
mount orphan discovery/retry.


The ordinary UFS inode sync callback also treats inode number zero as having no
persistent identity. This covers a retired cache object that retains a dirty flag
until final destruction, and prevents writes to the reserved inode-zero slot.
Retirement tests explicitly invoke both reclaim and sync after a positive retirement
(including returned errors) and require no additional disk writes.


## q113 atomic first directory backing

The initial-block owner now serves both first xattr allocation and empty directory
backing. Under the inode and mount locks it stages one zeroed payload, the CG free
map, superblock free total and the containing dinode block in one four-extent
transaction. Directory admission requires a directory with size zero and no direct
or indirect data pointers. Existing xattrs, link count and size remain unchanged.
Memory/quota are reserved before mutation; positive commit installs the live pointer
and block count, uncertain commit retains quota and prevents unsafe reuse.

`dir_add` uses this owner before its first insertion. After successful backing
commit its rollback baseline is the committed empty backing, not the former zero
pointer. A later entry failure therefore cannot detach the block while leaving its
bitmap allocated or release quota twice. This is a durable intermediate creation
state, not yet atomic final name/link publication. Legacy unsupported profiles are
still declined before admission. Moving all creation consumers to zero-link
preparation and grouped final publication, followed by mount orphan recovery,
remains required.


## q113 final creation publication

The grouped new_inode path now leaves nlink zero through inode_creation_prepare
and its attribute operations. Admission budgets 3*bsize + superblock bytes so
reservation, first metadata backing and final three-block publication all fit.
The final owner in ufs-creation.inc merges the parent directory block and both
dinodes, including shared physical dinode blocks, and commits the name with child
nlink 1 (ordinary kinds) or 2 (directory), plus the parent's directory link increment.
All create/mkdir/mknod/symlink consumers use this owner for zero-link preparation.
mkdir prepares dot/dotdot while unlinked; symlink prepares its inline content while
unlinked. A missing parent backing is first allocated as a recoverable empty block.

Unlike the hard-link wrapper, generic creation does not increment the child link
count. Therefore the owner publishes live parent size/links and child links on every
positive commit, including a returned checkpoint error. Committed or uncertain
publication errors invalidate the name cache/parent sequence and release the caller
reference without creation rollback; borrowed pathname-socket endpoints are detached.
Only proven unpublished children enter checked cleanup. Preparation errors and
uncommitted crashes can now leave a typed zero-link inode, not a nameless positive-
link inode. Discovering and reclaiming these owners before writable mount publication
is the next required step; this checkpoint alone does not solve mount orphan recovery.


## q113 private mount orphan recovery

After journal replay, structural/root validation and quota rebuild/config import,
but before publishing m_root or dirtying the admitted root, orphan_recover scans
allocated nonreserved inode numbers on writable journal mounts. It saves each CG's
candidate bitmap because per-inode reclamation can replace the mount's working CG.
Each candidate dinode is freshly read; linked identities are untouched. A shared
raw decoder preserves normal namespace rejection of zero-link/empty directories,
while a private recovery object accepts typed zero-link states including an empty
size with committed directory backing. It validates sizes, types, timestamps and
pointer geometry before release. The private object never enters the inode cache.

Recovery requires sufficient group capacity for checked release/retirement, then
uses existing data/xattr/inode reclamation with quota and snapshot ownership.
Malformed candidates or I/O failure stop admission and preserve remaining on-disk
owners for retry. No automatic inode-release callback performs a hidden second
cleanup. Readonly/nonjournal mounts do not run this reclaim path; an already
published mount is rejected. Repeated interrupted recovery remains idempotent.
Allocated mode-zero slots beyond reserved numbers are rejected, not guessed at.
This closes the new zero-link creation/unlink recovery chain; it is not a general
repair algorithm for arbitrary preexisting corrupt namespace/reference graphs.


## q114 immutable view lifetime

Keep one externally serialized writer, with a portable atomic image admission word:
a closed bit plus a bounded reader count. Publish opens acquisition only after the
entire committed image is validated. A pin captures immutable backing and sequence;
its copy operation performs no I/O and requires complete redo coverage before touching
the destination. Missing/partial coverage falls back to the existing serialized read.
Checkpoint may write/flush homes concurrently with those copies. Retirement closes
new acquisition before clearing the live pending identity, but existing pins retain
bytes. New publication, replay that overwrites the image, rebind and detach refuse
while retired pins remain. Owner-side backpressure must drain pins before reuse and
teardown; init requires an already quiescent owner, as before. Failed checkpoint does
not invalidate positively committed bytes already pinned; unresolved poison closes
new admission. Tests must prove the close/acquire race and retained generation, not
merely compare bytes with sequential calls. No deferred metadata is enabled by core
view support alone; mount readers/CG visibility and pressure scheduling follow.


## q115 bounded deferred checkpoint and error ownership

The existing per-mount writeback policy controls metadata delay as well as eligible
shared data. There is no second configuration surface or unbounded transaction
queue. A query-only writeback_mount_active checks live policy under the registry
spinlock without reserving a data ticket. The caller already owns the filesystem
metadata mutex, and queries before taking device-ranked snapshot/journal locks.
Policy pause closes admission before its final backend sync acquires that same
filesystem mutex. Thus a publication admitted before pause finishes before its
final drain; later calls use the synchronous path. Data tickets remain admitted
before content leases under their existing independent contract.

Only supported grouped metadata operations with no borrowed io_context defer home
checkpoint. Their commit record and immutable image are already durable before
success. One pending slot plus the previously charged fixed image bounds retained
work. Every subsequent synchronous or grouped journal writer drains the previous
slot before reserving its own sequence. A ready positively committed pending slot
is normal ownership, not uncertain failure. Checkpoint closes fresh image pins;
reuse then waits for retired immutable readers. Explicit through/drain contexts
still complete home checkpoint within the initiating operation.

ufs_journal_drain preserves the original checkpoint error across a successful
replay retry. A second failure poisons the journal and closes new image admission;
remount recovery is needed. A fresh synchronous context is scoped to each drain
and cleared before unlocking; no stack context or inode reference is retained.
ufs_sync excludes metadata admission through the physical flush. Snapshot creation
holds metadata/snapshot exclusion across checkpoint and snapshot publication;
clean marking drains before reading the superblock, so it cannot replace newly
committed summaries with obsolete home bytes. Sector-sized indirect lookups and
superblock preparation now use the same redo-aware metadata reader as full blocks.

A shared metadata checkpoint has no single data inode owner. Its failure is
recorded in a dedicated mount metadata ledger and the aggregate mount ledger.
file_fsync observes the metadata ledger with a separate description-owned cursor,
in addition to its existing inode error cursor. Independent opens each receive
notification; dup shares it; successful background retry never clears history.
This conservatively reports shared filesystem metadata failures to its openers,
without assigning unrelated inode data failures to every file. Aggregate backend
sync may also record the returned error; ledger sequences count observations,
not unique physical I/O failures. Existing stacked-file fsync uses its VM inode's
mount to identify the backing metadata owner.

Focused evidence is recorded in results.md. Full phase acceptance, especially
native opt-in snapshot/quota and final CRASH/META/WB/FLUSH coverage, remains required.
