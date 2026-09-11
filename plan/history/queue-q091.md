# Queue q091: WS025 range allocator and constrained DMA

Date: 2026-09-07
Status: finished
Authorization: the user authorized WS025 completion and successive finite Queues on 2026-09-07.
Timebox: review progress every 90 active minutes, recording evidence and a concrete resume point.
Baseline: q090 / ws025-p003 completed; changes remain uncommitted.

| Order | Phase | Status | Purpose / dependency |
| --- | --- | --- | --- |
| 1 | [ws025-p004](../ws025/phase004/phase.md) | completed | Range-based RAM ownership, constrained allocation and DMA masks. Requires completed p003; keep the normal high-memory publication gate closed until p005. |

Replace span-sized bitmap accounting with actual RAM extents. Transfer early reservations, add an explicit constrained API without extending uninitialized request fields, and inspect DMA/VM narrowing assumptions. Main uncertainty is metadata allocation and compatibility across HALs; production-linked host tests and supported builds are the verification path. High RAM host fixtures precede native publication under p005.

Previous: [q090](queue-q090.md). No commit or aggregate make check; use make -j16 and serialize build/runtime on disposable images. Follow coding-style.md broadly.

Result: p004 completed. See phase results and address audit. Next: p005 high-memory publication.
