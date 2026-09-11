# Queue q114: UFS immutable metadata readers and checkpoint ownership

Date: 2026-09-07
Status: finished
Authorization: standing user approval for autonomous WS025 completion and successive queues.
Timebox: review every 90 active minutes; start 2026-09-07 13:08 UTC.
Previous: [q113](queue-q113.md), finished with p021 uncleared against full criteria.

| Order | Phase | Status | Purpose / dependency |
| --- | --- | --- | --- |
| 1 | [ws025-p021](../ws025/phase021/phase.md) | uncleared | Add immutable pending-image read pins and integrate coherent metadata readers around checkpoint I/O, using q111–q113 verified transaction/creation/orphan ownership. |

Inspection: read_block holds journal_lock through ufs_journal_read and device I/O;
metadata_group_commit holds that same lock through synchronous commit/checkpoint.
The core owns an immutable image but retirement invalidates it with no reader pins.
CG views read homes directly. Existing serialization is safe but cannot let a reader
copy already-validated redo while checkpoint blocks on device I/O.

First bounded work: define a portable immutable view/pin contract with explicit
publication, poison, generation, retirement and backing-reuse ownership. Implement
pin acquisition/copy/release and enforce lifetime across checkpoint/retirement, then
connect mount readers and CG visibility. Keep slow I/O out of the short reader-state
critical section; retain writer/transaction exclusion and exact commit outcomes.
Prove with deterministic host concurrency that a pinned read proceeds while home
write/flush is paused, and that retirement/rebind/new publication cannot reuse live
reader backing. Cover refusal, failed checkpoint, partial coverage and teardown.

Review remaining metadata profile/adapter coverage, including multi-block xattrs and
legacy pre-admission fallbacks, before enabling deferred metadata. Do not infer full
write-back readiness merely from core pin tests. Operation-level checkpoint policy,
pressure/fsync/error epochs and full CRASH/META/WB/FLUSH gates remain required.
Run appropriate driver/supported/native regressions serially. No commits, aggregate
make check, .internal access or concurrent build/runtime; use make -j16.
Physical acceptance remains user-accepted, not agent-measured.


Cycle finished 2026-09-07 13:40 UTC. Immutable core views, VFS/CG reads, writer drain and
backing teardown are verified with concurrency, crash/recovery, supported and native
regressions. Profile/adapter review recorded in p021 results. Full p021 remains
uncleared; continue operation-level deferred policy and full acceptance in q115.
