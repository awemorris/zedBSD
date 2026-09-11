# Queue q095: WS025 UFS data runs

Date: 2026-09-07
Status: finished
Authorization: user's autonomous WS025 completion instruction covers successive finite Queues.
Timebox: review every 90 active minutes; record facts and resume autonomously.

| Order | Phase | Status | Purpose / dependency |
| --- | --- | --- | --- |
| 1 | [ws025-p008](../ws025/phase008/phase.md) | completed | Coalesce existing contiguous UFS data blocks up to 64 KiB and retain FAT/loop extents. Requires completed p007. |

Scope: current UFS1 and UFS2 until WS024 merges them. Preserve allocation/zero/pointer publication and UFS2 journal/snapshot hooks; cap journal runs by journal capacity. Bound mapping at hole, fragment, indirect leaf, partial block and error. Existing loop/FAT extent path already passes large contiguous requests; verify rather than duplicate it. Focused production mapping/data tests, q086 affected FS acceptance, three builds and native counters. No commit or aggregate make check; serialize builds/runtime.

Previous: [q094](queue-q094.md). Physical acceptance remains user-accepted; no agent physical runtime claimed.

Result: both UFS run/fault/hook fixtures, metadata regression, FS50 plus WiFi30, three supported builds and native baseline PASS. See p008 results.
