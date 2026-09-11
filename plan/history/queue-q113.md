# Queue q113: Recoverable UFS creation and orphan ownership

Date: 2026-09-07
Status: finished
Authorization: standing user approval for autonomous WS025 completion and successive queues.
Timebox: review every 90 active minutes; start 2026-09-07 11:17 UTC.
Previous: [q112](queue-q112.md), finished with p021 uncleared against full criteria.

| Order | Phase | Status | Purpose / dependency |
| --- | --- | --- | --- |
| 1 | [ws025-p021](../ws025/phase021/phase.md) | uncleared | Integrate recoverable inode reservation, creation/backing publication and mount orphan ownership using q112's verified metadata, xattr and retirement owners. |

Source review: allocate_inode_number currently publishes bitmap/summaries before
initializing the inode. new_inode publishes a nonzero link count before its name;
ACL preparation can allocate xattrs. mkdir splits dot/dotdot/name/parent count;
creation rollback independently clears pointers before freeing resources.

First bounded implementation: atomically reserve an inode number with a fully
initialized zero-link dinode and directory accounting, with conservative quota and
explicit commit outcome. Connect reservation to new_inode, preserving necessary
legacy completion while the final publication owner is implemented next. Do not
claim zero-link lifetime through all preparation until those consumers are moved.
Test file/directory reservations, stale dinode bytes, failure/replay/quota boundaries
and supported/native gates before continuing creation publication and orphan scan.

Keep first/remaining backing allocation and all create/mkdir/mknod/symlink outcomes
in scope. Preserve original errors, committed-error cache invalidation, delayed final
reference reclamation and snapshot before-images. No commit, aggregate make check,
.internal access or concurrent build/runtime; use make -j16. Physical acceptance
remains user-accepted rather than agent-measured. Read pins/deferred policy and the
original full p021/WS025 completion criteria remain required beyond this cycle.


Reservation checkpoint: initialized zero-link inode identity, bitmap/free totals
and directory accounting now commit together in new_inode's reservation step.
18896 ordinary/sanitizer fault/replay/refusal checks, driver regressions, 78 socket
rollback checks, supported builds and native features/remount/reboot pass. Evidence
and hashes in p021 results. All handles terminal; ordinary amd64 restored.
The legacy completion still sets nlink before ACL/name publication. Next move that
transition into the name transaction and integrate rollback/backing/orphan recovery;
q113/p021 remain in-progress against the unchanged full completion criteria.


Selected checked creation cleanup: factor reclaim into an errno-returning owner;
for supported journal reservations, commit zero nlink without dropping block/xattr
references, then use grouped content/xattr/final inode retirement. Preserve failed
cleanup state for retry/recovery instead of independently clearing pointers first.
The caller must establish name absence; committed publication must never use this
cleanup. Legacy nonzero-link preparation/name publication remains required to move.


Checked-cleanup checkpoint: zero-link transition preserves references, then checked
content/xattr/final retirement owners report failure. Positive retirement invalidates
live inode identity; repeat reclaim/sync issue no writes. 63174 cleanup and 15445
retirement checks pass ordinary/sanitizer; driver/socket regressions, final supported
builds and native features/remount/reboot pass. Evidence/hashes in p021 results.
All handles terminal; ordinary amd64 restored. q113/p021 remain in-progress.
Next keep creation zero-link through preparation and group final name/nlink; integrate
backing and mount orphan recovery before the remaining concurrent/deferred gates.


Selected directory backing prerequisite: share first-block allocation ownership
with initial xattr allocation. Stage zeroed directory backing, CG/super consumption
and the inode pointer/block count together. dir_add must retain a committed empty
backing if its later entry insertion fails; it must not restore the pre-allocation
pointer or release quota twice. Test common-owner xattr regression and directory
allocation/error/replay before connecting zero-link creation and final publication.


Directory-backing checkpoint: shared first-block owner and dir_add rollback retain
committed empty backing. Fault/replay/quota, preserved xattr/sibling bytes and
refusal checks pass ordinary/sanitizer; xattr/driver/socket regressions, supported
builds and native features/remount/reboot pass. See p021 results and hashes.
All handles terminal; ordinary amd64 restored. q113/p021 remain in-progress.
Next keep creation zero-link through ACL/preparation and group name/child/parent
links, then establish mount orphan ownership before remaining phase gates.


Selected final creation publication: use a common journal profile large enough for
reservation, initial backing and final parent/child dinode publication. Keep grouped
new_inode at nlink zero through ACL preparation. Migrate create/mkdir/mknod/symlink
together; retain zero-link dot/dotdot and inline symlink preparation, then group the
parent name, child final nlink and directory parent increment. Merge shared dinodes.
A committed/uncertain publication error invalidates caches and never discards the
potentially named inode; detach borrowed socket endpoints before failed return.
Verify failure/replay outcomes and existing/native creation before orphan recovery.


Final-creation checkpoint: grouped preparation stays zero-link through ACL storage;
all creation callbacks group name/child links and directory parent increment.
Committed/uncertain publication errors retain named ownership and invalidate caches.
Core and full callback fault/replay/quota checks pass ordinary/sanitizer, along with
driver/socket regressions, supported builds and native features/remount/reboot.
See p021 results/hashes. All handles terminal; ordinary amd64 restored.
q113/p021 remain in-progress. Next discover and reclaim zero-link allocated inodes
before writable mount publication, preserving quota/snapshot/retry ownership.


Selected mount orphan owner: factor raw dinode decoding so namespace loads still
reject zero-link/empty-directory identities, while a private recovery object can
validate typed zero-link states including partial empty directory preparation.
After root/quota validation and before mount publication, snapshot each CG inode
map, decode zero-link allocated nonreserved inodes, and use checked grouped reclaim.
Do not put recovery objects in the inode cache. Refuse unsupported recovery groups
and malformed state before freeing it; propagate errors, prevent implicit cleanup
retry, and leave remaining references recoverable on the next mount. Readonly and
nonjournal mounts do not reclaim. Test recovery interruptions, multiple orphan
slots/CGs, live neighbors, quota/snapshot and readonly/refusal boundaries.


Cycle finished 2026-09-07 13:08 UTC. Reservation, checked cleanup, initial backing,
final zero-link creation publication and private mount orphan recovery are verified
with focused ordinary/sanitizer, supported builds and native evidence. p021 remains
uncleared against the unchanged full completion criteria. Resume in q114 with
concurrent immutable read pins, reader/adaptor coverage and deferred-policy gates.
