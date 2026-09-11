# Queue q112: UFS namespace transaction ownership

Date: 2026-09-07
Status: finished
Authorization: user-approved autonomous WS025 completion and successive queues.
Timebox: review every 90 active minutes; finite namespace integration cycle.
Previous: [q111](queue-q111.md), finished with p021 uncleared against full criteria.

| Order | Phase | Status | Purpose / dependency |
| --- | --- | --- | --- |
| 1 | [ws025-p021](../ws025/phase021/phase.md) | uncleared | Group unlink entry/link count and explicit cache/lifetime outcome; inspect shared-dinode staging for subsequent link/namespace operations. Depends on verified q111 metadata commit/allocation/release owners. |

Selected after inspecting ufs_unlink/ufs_link, directory block helpers and generic
inode namespace wrappers. Unlink currently persists removal before target nlink;
link also splits those records, while generic inode_link increments live nlink
only after success. Generic cache invalidation runs only after success. A committed
error therefore needs explicit filesystem-owned invalidation and live outcome.

First bounded implementation: group a prepared directory removal and target dinode
under namespace, inode and mount ownership. Existing block/profile capacity is
preflighted before admission. A handled result is separate from errno. Preserve
all snapshot before-images and carry committed/uncertain outcome. Invalidate the
name cache/directory sequence on committed error after inode/mount locks are released.
Test actual driver/core, same-block sibling preservation, power cuts, failed/landed
writes, failed recovery and snapshot failure; run existing/supported/native gates.

Next scope review: shared-dinode image merging for link/other namespace operations,
zero-link orphan recovery and inode-number ownership. Zero-link allocated inodes
are not claimed reclaimed by this first step. Full metadata delay, read pins and
remaining p021 acceptance stay required. No commits, aggregate make check or
.internal access; serialize builds/tests and use make -j16. Physical gate remains
user-accepted, not agent-measured.


Unlink checkpoint: actual directory removal and target nlink share one journal
group; committed/uncertain errors explicitly invalidate cached names, and poisoned
directory reads return EIO. 27982 actual UFS/journal/name-cache ordinary/sanitizer
checks, driver regressions, 78 pathname-socket rollback checks, supported builds
and native feature/remount/reboot pass. Evidence/hashes are in p021 results.
No process remains live; ordinary amd64 restored. q112/p021 remain in-progress.
Next: shared-dinode staging for link and remaining namespace owners; zero-link
orphan recovery is still required and is not claimed by the unlink test.


Selected link boundary: factor dinode encoding from block loading so two prepared
inodes sharing a block merge into one redo extent. Group an insertion into an
existing directory data block, its size update and target nlink. Retain the generic
inode_link success-only increment, while a committed error publishes the live
count and invalidates namespace cache explicitly. Preflight capacity and keep
unallocated-directory growth outside this first bounded path. Test both shared
and distinct dinode blocks, slack/free-record/size-growth and error/replay outcomes.


Hard-link checkpoint: shared dinode images merge before publication; insertion,
directory size and target nlink commit together. Callback/generic live-count
ownership and error cache invalidation are explicit. 65364 hard-link checks,
104106 allocation and 95603 release checks pass ordinary/sanitizer; existing driver
regressions, supported builds and native real-wrapper feature/remount/reboot pass.
Evidence/hashes are in p021 results. No active process; ordinary amd64 restored.
q112/p021 remain in-progress. Remaining namespace/orphan/inode-number ownership and
concurrent reads/deferred policy still require implementation and acceptance.


Selected rmdir boundary: generalize the private removal owner to include target
nlink zero and parent nlink decrement, merging shared dinode slots into one image.
Keep the existing empty-directory validation and delayed final-reference cleanup;
record parent entry/nlink and target state atomically. Test shared/distinct inode
blocks, nonempty/type/dot refusals, crash/error/cache outcomes and native rmdir.
Orphan/inode-number recovery remains separate required work.


Rmdir checkpoint: parent entry, parent nlink decrement and target nlink zero now
share one group with shared-dinode merging. 39435 rmdir and 27982 unlink checks
pass ordinary/sanitizer; existing driver regressions, three supported builds and
native features/remount/reboot pass. Evidence/hashes are in p021 results. No live
build/runtime; ordinary amd64 restored. q112/p021 remain in-progress; remaining
namespace/orphan/inode-number owners and concurrent reads/deferred policy remain.


Selected rename preparation boundary: introduce a bounded private metadata image
set keyed by physical extent. Repeated dinodes sharing a block edit one image;
partial overlaps and capacity overflow fail before publication. Use the owner in
hard-link staging first and rerun fault/replay tests before constructing rename's
multi-directory operation. The existing rename callback splits destination,
source removal, dotdot and link counts; it remains a required subsequent consumer.


Shared-image checkpoint: generic bounded physical-block deduplication is implemented
and consumed by hard link. 465 ownership checks (all 24 four-inode edit orders)
and 65365 hard-link fault/replay checks pass ordinary/sanitizer. Existing driver
regressions, supported builds and native features/remount/reboot pass. Evidence
and source hashes are in p021 results. No active process; ordinary amd64 restored.
Next implement rename's private directory edits, dotdot/parent/target accounting,
deduplicated locks and committed-error cache publication using the verified owner.
q112/p021 remain in-progress with the original full completion requirements.


Selected rename implementation: stage existing directory backing, source removal,
destination insertion/replacement, directory dotdot and parent/target accounting
using the shared image owner. Deduplicate up to four inode locks and up to six
physical blocks before admission; preserve explicit handled/error separation.
Test both same/cross-parent and file/directory replacement with actual driver/core,
then supported/native gates. New directory-backing allocation remains a separate
allocation/namespace integration requirement, not an errno fallback after commit.


Rename checkpoint: existing directory backing now stages both names, dotdot and
parent/target accounting in one group, with explicit committed-error cache outcome.
234341 rename and 65365 hard-link checks pass ordinary/sanitizer; driver regressions,
three supported builds and native features/remount/reboot pass. Results/source
hashes are in p021 results. All processes terminal; ordinary amd64 restored.
q112/p021 remain in-progress. Next integrate create/backing allocation and persistent
orphan/inode-number ownership; read pins/deferred policy and full acceptance remain.


Selected inode retirement boundary: after successful data/xattr teardown, group
zeroing the dinode mode/type with inode-bitmap release and CG/super free-inode and
directory totals. Require zero references/link count and no remaining block owners;
release inode quota only on positive commit, retain errno and prohibit fallback
for admitted errors. Test final-retirement failure/replay before mount orphan
recovery and create integration. Retirement alone does not discover crash orphans.


Final inode-retirement checkpoint: empty zero-link dinode invalidation, inode
bitmap/free totals and directory accounting commit together; quota release follows
positive outcome. 14863 ordinary/sanitizer fault/replay/refusal checks, driver
regressions, supported builds and native features/remount/reboot pass. Evidence and
hashes in p021 results; all handles terminal, ordinary amd64 restored. q112/p021
remain in-progress. Continue create/backing allocation, xattr release and mount
orphan recovery before read pins/deferred policy and full acceptance.


Selected xattr teardown boundary: remove all xattr references and release their
one/two backing blocks in one dinode/CG/super group. Deduplicate CG images when
both blocks share a group; validate ownership and quota before publication. Do not
split a serialized two-block attribute area into invalid partial records. Preserve
all old bytes for snapshots and test grouped clear failures/replay. Nonempty xattr
replacement/creation and mount orphan discovery remain subsequent requirements.


Xattr teardown checkpoint: one/two-block attribute references and CG/super free
accounting commit together, deduplicating CG images and capacity. 34889 checks pass
ordinary/sanitizer; driver regressions, supported builds and native snapshot xattr
retention/remount/reboot pass. Evidence/hashes in p021 results. All handles terminal;
ordinary amd64 restored. q112/p021 remain in-progress. Continue nonempty xattr and
create/backing ownership, mount orphan recovery, read pins/deferred policy and full
acceptance without claiming teardown alone completes these requirements.


Selected existing xattr replacement: generalize the verified clear owner to retain
and replace the first attribute block while atomically dropping any second block.
Stage payload bytes with the new dinode length and allocation/quota changes. Keep
unchanged CG/super images out of a one-block replacement's redo group. First xattr
allocation remains a separate creation/allocation integration requirement.


Existing xattr replacement checkpoint: payload/dinode update and optional second-
block release share a group; one-block replacement uses only two redo extents.
35619 replacement and 34889 clear checks pass ordinary/sanitizer; driver regressions,
supported builds and native features/remount/reboot pass. Evidence/hashes in p021
results. All handles terminal; ordinary amd64 restored. q112/p021 remain in-progress.
Next group first xattr allocation, then recoverable creation/backing and mount
orphan ownership; retain concurrent reads/deferred policy and full phase acceptance.


Selected first xattr allocation: reserve one quota block and select a free full
block under mount ownership, then group private CG/super allocation, initialized
payload and prepared dinode reference. Keep live bitmap/reference unchanged until
positive commit, retain quota on uncertain publication, and separate handled from
errno. Test failed allocation/publication/replay and existing xattr regression.


First xattr allocation checkpoint: CG/super allocation, initialized payload and
inode reference publish together, with conservative uncertain quota ownership.
21660 allocation and 35619 replacement checks pass ordinary/sanitizer; driver
regressions, supported builds and native features/remount/reboot pass. Evidence
and hashes in p021 results. All handles terminal; ordinary amd64 restored.
q112/p021 remain in-progress. Continue recoverable inode reservation and creation/
backing publication, then mount orphan recovery and concurrent/deferred ownership.


Cycle review at 2026-09-07 11:17 UTC: selected namespace/shared-image and attribute
owners are implemented and verified. The phase remains uncleared against full p021
criteria: creation/backing, mount orphan recovery, read pins and deferred metadata
policy/full acceptance remain. Resume under q113 with recoverable inode reservation
and creation publication. This is a normal finite-cycle boundary, not a blocker.
