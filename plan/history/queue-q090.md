# Queue q090: WS025 RAM direct map

Date: 2026-09-07
Status: finished
Authorization: the user authorized WS025 completion and successive finite Queues on 2026-09-07.
Timebox: review progress every 90 active minutes; retain evidence and a concrete resume point.
Baseline: q089 / ws025-p002 completed; changes remain uncommitted.

| Order | Phase | Status | Purpose / dependency |
| --- | --- | --- | --- |
| 1 | [ws025-p003](../ws025/phase003/phase.md) | completed | Separate the RAM direct map from the kernel image and MMIO, construct sparse tables from firmware ranges, retain low allocation until p005. Requires completed p002. |

The approved virtual layout fixes image/RAM/vmap separation. Main uncertainty is early table allocation and conversion callers that currently treat image and RAM pointers alike. Resolve these within p003 with production-linked geometry fixtures and BIOS/UEFI SMP boot checks. Do not publish high pages before the following allocator/DMA Phases.

Previous: [q089](queue-q089.md). No commit or aggregate make check; use make -j16, serialized build/runtime, disposable images. Follow coding-style.md broadly.

Result: sparse RAM map, image/MMIO separation, shared roots and expanding early arena completed. Host ordinary/sanitizer, three builds, BIOS/UEFI low/high boots, forced expansion and diagnosed exhaustion passed. See [p003 results](../ws025/phase003/results.md).
