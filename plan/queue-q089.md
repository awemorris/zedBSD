# Queue q089: WS025 typed boot-memory handoff

Date: 2026-09-07
Status: finished
Authorization: the user authorized WS025 completion and successive finite Queues on 2026-09-07.
Timebox: review progress every 90 active minutes; retain evidence and a concrete resume point.
Baseline: q088 / ws025-p001 completed; working-tree changes remain uncommitted.

| Order | Phase | Status | Purpose / dependency |
| --- | --- | --- | --- |
| 1 | [ws025-p002](ws025-io-memory-cache/phase002-boot-memory-handoff/phase.md) | completed | Typed BIOS E820 and UEFI handoff, full-width range reporting and boot reservations. Requires completed p001. |

The approved memory design fixes the direction. This Phase defines the v6 byte layout, preserves v1–v5 readers and the shared i386 loader, and validates malformed/capacity/fallback cases. Permanent mapping and general high-memory allocation remain separate dependent Phases.

Verification: production-linked handoff/normalizer fixtures; supported x86 builds; disposable amd64 BIOS/UEFI boots. Do not equate login with high-memory allocation.

Previous: [q088](queue-q088.md). Follow plan/coding-style.md broadly. No commit or aggregate make check; build/runtime are serialized.

Result: production E820/UEFI v6 handoff, 8 host fixture variants, 3 x86 builds and 4 BIOS/UEFI USB boot cells passed. See [p002 results](ws025-io-memory-cache/phase002-boot-memory-handoff/results.md).
