# Queue q100: WS024 unified UFS migration contract

Date: 2026-09-07
Status: finished
Authorization: autonomous WS025 completion includes its required WS024 integration;
public UFS naming and one 64-bit implementation are already user decisions.
Timebox: review every 90 active minutes; record facts and continue autonomously.

| Order | Phase | Status | Purpose / dependency |
| --- | --- | --- | --- |
| 1 | [ws024-p001](../ws024/phase001/phase.md) | completed | Freeze format, feature inventory, limits, producer/consumer switch and retained-data policy before single-driver implementation. |

Use the existing 64-bit codec as the disk-format basis. Inventory required features
and all active producers/consumers; define precise rebuild and old-image rejection
rules. Preserve generated source images as opaque backups before a later rebuild;
never reinterpret or silently reformat retained images. This Queue produces the
reviewable implementation contract and acceptance matrix; it does not yet switch
the production driver or formatter. Schedule consolidation before further UFS
allocation batching, carrying WS025 p008/p010/p014 into the single owner.

Previous: [q099](queue-q099.md). No commit; do not inspect `.internal/`.

Result: [p001 contract](../ws024/phase001/results.md).
