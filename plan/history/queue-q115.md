# Queue q115: UFS deferred checkpoint policy and durability boundaries

Date: 2026-09-07
Status: finished
Authorization: standing user approval for autonomous WS025 completion and successive queues.
Timebox: review every 90 active minutes; start 2026-09-07 13:40 UTC.
Previous: [q114](queue-q114.md), finished with p021 uncleared against full criteria.

| Order | Phase | Status | Purpose / dependency |
| --- | --- | --- | --- |
| 1 | [ws025-p021](../ws025/phase021/phase.md) | uncleared | Integrate opt-in durable metadata publication with bounded deferred checkpoint, full-slot draining, filesystem sync/snapshot/unmount and error ownership. Depends on verified q111–q114 journal, metadata/orphan and immutable reader owners. |

First bounded implementation: join the existing per-mount writeback policy and
physical-device sync worker instead of adding an unrelated unbounded queue. Query
live policy while holding the filesystem metadata owner, before device-ranked locks.
Only supported journal geometry and metadata calls without borrowed provenance can
opt into deferred home checkpoint. Explicit through/drain callers stay synchronous.
One committed slot and its accounted image bound retained work; a new writer drains
the prior slot before admission, never mistakes normal committed pending state for
uncertain failure, and never overwrites retained reader backing.

Add a checked checkpoint owner and connect filesystem sync, synchronous writes,
snapshot creation and clean unmount. Reconstruct a fresh synchronous drain context;
do not retain stack io_context pointers. Policy-off/worker pause must synchronize
with the filesystem owner so admitted publication cannot escape the final drain.
Keep original errors and retained mount error epochs across background failure.
Verify policy on/off, slot pressure, fsync, snapshot and failed checkpoint/retry using
actual core/VFS and deterministic host workers, then supported/native acceptance.
Full CRASH/META/WB/FLUSH and later WS025 gates remain required; do not infer complete
phase coverage from a green narrow fixture. No commits, aggregate make check,
.internal access or concurrent build/runtime; use make -j16. Physical acceptance
remains user-accepted, not agent-measured.

Outcome: bounded integration and focused/supported/native gates PASS. Full p021
acceptance remains; continue q116 under standing authorization. See phase results.
