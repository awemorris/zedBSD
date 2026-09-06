# Queue q086: FS/USB functional corrections and 50 acceptance stories

Date: 2026-09-06
Status: in-progress
Baseline: 02eeed5
Authorization: user explicitly requested this plan, queue and execution.
Timebox: progress review every 90 active minutes, inherited from q085.

| Order | Phase | Status | Purpose / dependency |
| --- | --- | --- | --- |
| 1 | [ws018-p017](ws018-kernel-architecture/phase017-storage-baseline/phase.md) | in-progress | Baseline, four cells, image audit |
| 2 | [ws004-p049](ws004-hardware/phase049-storage-recovery/phase.md) | pending | Finite USB ownership and BOT recovery before latch |
| 3 | [ws018-p018](ws018-kernel-architecture/phase018-storage-io-path/phase.md) | pending | Coherent loop map and batched local FS I/O |
| 4 | [ws018-p019](ws018-kernel-architecture/phase019-storage-acceptance/phase.md) | pending | 50 stories, native persistence, Wi-Fi regression and builds |

Design: [implementation plan](fs-report-implementation-1.md).
Acceptance: [50 scenarios](fs-acceptance-50.md).
Q085 is archived unchanged. No commits or aggregate make check.
Keep retained DMA ownership distinct from successful recovery; record native
and hardware coverage separately. Do not mark unrun acceptance completed.
