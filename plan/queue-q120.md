# Queue q120: owned DMA scatter and shared USB staging

Date: 2026-09-08 JST
Status: finished
Authorization: standing user approval to complete WS025 autonomously.
Timebox: review every 90 active minutes, starting 2026-09-07 15:34 UTC.
Previous: [q119](queue-q119.md), p023 completed.

| Order | Phase | Status | Purpose / dependency |
| --- | --- | --- | --- |
| 1 | [ws025-p024](ws025-io-memory-cache/phase024-sg-dma/phase.md) | completed | Bounded owned DMA vectors, transactional core/HCD staging ownership and normal xHCI SG TD/short completion, disk vector compatibility adapter. Depends on p009/p019/p023. |

Follow sg-design.md: keep the caller-isolated staging across late cancellation;
share that HCD-owned reservation with USB core, never DMA arbitrary reusable caller
scratch. Validate vector/mask/TRB limits and preserve reclaim/unsupported fallback.
Verify production owners and fault boundaries before native counters/physical
vector evidence. Full phase requires acceptance, not just helper implementation.
No commits, aggregate make check, .internal access or concurrent builds/tests.
Use make -j16; force relink and verify wrapper absence on native probe restoration.

Result: p024 completed; [evidence](ws025-io-memory-cache/phase024-sg-dma/results.md). Ordinary artifact restored, all gates terminal. Continue with p025.
