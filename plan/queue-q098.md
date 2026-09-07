# Queue q098: WS025 FAT clean cache and chain reuse

Date: 2026-09-07
Status: finished
Authorization: user's autonomous WS025 completion instruction covers successive finite Queues.
Timebox: review every 90 active minutes; record facts and resume autonomously.

| Order | Phase | Status | Purpose / dependency |
| --- | --- | --- | --- |
| 1 | [ws025-p012](ws025-io-memory-cache/phase012-fat-cache-cursor/phase.md) | completed | Bounded clean sector retention and generation-checked sequential chain reuse. Requires completed p001. |

Keep exactly one mutable sector buffer and its existing flush/mirror/rollback
order. Additional retained slots contain clean copies only. Mount-wide chain
generation conservatively invalidates every open cursor before FAT link mutation
and on external cache invalidation; wrap disables reuse. Retain a cursor only
after whole-chain validation, and use it across calls only for an immediately
sequential offset with matching generation and first cluster. Seek or failed
operations fall back to validation. Loop extent claims and retained mapping stay
under their existing owners. Focused FAT/loop failure tests, sequential versus
seek/other-open mutation tests, three builds, and native readback are the gates.

Previous: [q097](queue-q097.md). Physical gate remains user-accepted;
no agent physical runtime claimed. No commit, no aggregate make check;
serialize build/runtime.

Result: [p012 results](ws025-io-memory-cache/phase012-fat-cache-cursor/results.md); all selected gates passed.
