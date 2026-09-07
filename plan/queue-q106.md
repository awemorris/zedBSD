# Queue q106: managed-memory budgets and clean reclaim

Date: 2026-09-07
Status: finished
Authorization: user-approved autonomous WS025 completion and successive queues.
Timebox: review every 90 active minutes, record findings and continue.

| Order | Phase | Status | Purpose / dependency |
| --- | --- | --- | --- |
| 1 | [ws025-p016](ws025-io-memory-cache/phase016-memory-budget-reclaim/phase.md) | completed | Share managed-RAM accounting across cache/pool/DMA, grow file-cache demand and reclaim clean ownership; p005 and p015 complete. |

M/W/P, actual VM/file/block/pool/DMA/allocator code and q105 results were inspected.
The P book selects explicit pending/resident accounting, optional versus mandatory
ownership, indexed page metadata, a no-I/O clean pressure path and transactional
policy shrink. Delayed writeback remains disabled. Preserve p015 content ownership.
No commit, aggregate make check or .internal access. Serialize builds/tests;
make -j16 and disposable QEMU images.

Previous: [q105](queue-q105.md), coherent retained file cache completed.
Next dependency-ready work after p016: p017 dirty/error/drain contracts.

Outcome: p016 complete; see its results for host, x86, FS50/Wi-Fi30/native and 12-cell RAM evidence. Next finite queue selects p017.
