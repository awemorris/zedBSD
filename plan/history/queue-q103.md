# Queue q103: unified UFS allocation batching

Date: 2026-09-07
Status: finished
Authorization: user's autonomous WS025 completion instruction covers successive
finite queues and necessary implementation. No repeated confirmation required.
Timebox: review every 90 active minutes, record findings, continue autonomously.

| Order | Phase | Status | Purpose / dependency |
| --- | --- | --- | --- |
| 1 | [ws025-p011](../ws025/phase011/phase.md) | completed | Batch bounded allocation/data/publication on the single UFS owner; p010, p014 and WS024 p001–p004 complete. |

Use the P book for publication/rollback, quota, lock and measurement contracts.
The main uncertainty is the existing shared-dinode and indirect publication
boundary: resolve it in private images before enabling the new allocation path.
No commit, aggregate make check, .internal reads or real-media reformatting.
Build/tests remain serialized; make -j16 and disposable QEMU media.

Next dependency-ready phase after completion: p013 FAT operation batching.
Previous: [q102](queue-q102.md), complete unified UFS acceptance and retirement.

Result: p011 complete. New-create host writes 41→4 for 32 KiB; native 256 KiB UFS writes 176→40. 7,973 dedicated checks per variant, maintained driver gates, three builds, mounted features, storage 50/50 and WiFi 30 pass. Both timing runs retained in results.
