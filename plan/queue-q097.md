# Queue q097: WS025 allocation adapter and metadata views

Date: 2026-09-07
Status: finished
Authorization: user's autonomous WS025 completion instruction covers successive finite Queues.
Timebox: review every 90 active minutes; record facts and resume autonomously.

| Order | Phase | Status | Purpose / dependency |
| --- | --- | --- | --- |
| 1 | [ws025-p010](ws025-io-memory-cache/phase010-ufs-metadata-adapter/phase.md) | completed | Introduce immediate allocation adapter and bounded CG/indirect cache reuse. Requires completed p008. |

Design: retain the existing allocation/write/rollback order in the initial adapter. Indirect pointer reads use a sector-sized stack window through the common buffer cache rather than per-lookup filesystem-block allocation. CG working images gain an explicit valid bit and a bounded referenced buffer view; generation/lifetime validation prevents stale reuse. All view references are prepared before I/O, never hold multi-line busy ownership over allocation/reclaim. Unavailable views retain the existing disk-read copy fallback. Disk lifecycle admission applies to view reads/hits; media/device replacement cannot reuse a pinned identity. CG writes and error/rollback invalidate the view conservatively. Common cache pins count against its existing cap; no per-inode large cache.

Inspect and validate source owners including disk.c/disk.h for lifecycle admission. Test initial CG zero, switch/hit, stale generation, invalidation/reuse, memory cap/reentry, adapter allocation/rollback equivalence, three builds and native counters. No commit, no aggregate make check; serialize build/runtime.

Previous: [q096](queue-q096.md). Physical gate remains user-accepted; no agent physical runtime claimed.

Result: [p010 results](ws025-io-memory-cache/phase010-ufs-metadata-adapter/results.md). All selected gates passed.
