# FS/USB review follow-ups after q086

Date: 2026-09-06. These are explicit residual work, not completed corrections.
q086 is the first bounded implementation cycle. It does not close all 43
findings of reports 1–3. Report-4 supersedes the mistaken claims in those reports.
The 50 acceptance stories cover q086's defined implementation and preserved
contracts, not the absence of every filesystem/USB defect.

| Findings | q086 result | Next bounded work / owner |
| --- | --- | --- |
| 1, 2, 5, 7, 20 | Loop data uses retained logical extents and parent cache inside the normal file/VM transaction; FAT slot coherence preserved. Full FAT write validation preserved. | General FAT read cursor, validated write generations, batching of non-loop FAT data/mirrors; WS018 |
| 4, 8, 12, 16, 17, 22, 26, F2 | Existing copy-up, directory, fsync ordering and scratch lifetimes retained and exercised; performance rewrites not implemented. | Copy-up batch/scratch reuse; UFS directory one-read iteration; overlay cursor/identity/flush generations; WS018/WS024 |
| 6, 9, 14, 15 | Existing UFS full-block overwrite no longer pre-reads data. Allocation initialization and publication order retained. | CG/summary/indirect metadata caching and initialization before pointer publication; WS024 |
| 10, 11, 19, 21, 23, 24 | Existing claim, name, buffer and VM contracts retained. | Claim/dirty/name/tmpfs indices; write-back and universal file page cache separately designed; WS018 |
| 13 | Regular syscall batching implemented, including positional/vector operations; small-buffer fallback and pipe contract preserved. | No selected remainder; larger sizes require measurement |
| 18, U9, U10 | Synchronous caller ownership isolated; storage transfer bounds expressed in bytes. Normal waits still spin; BOT stays serial. | Runtime sleep/wakeup, direct DMA and larger reclaim reserves; WS004 |
| 25 | Not part of USB-root correction. | NVMe transfer sizing/PRP lifecycle; WS004 |
| F1 | Current images checked (no over-limit directory); actual producer fixture demonstrates detection. | Multi-block directory mutation with single 64-bit UFS migration; WS024 |
| U1 | CSW retry, bounded reset/reissue, scoped reset-UA and medium-change stop implemented. | Physical device recovery/error injection and broader supported sense profiles; WS004 |
| U2 | Retry happens before final flush latch. Failed persistence remains visible; reads can proceed independently unless medium is uncertain. | Explicit recovery-flush state/reporting and confirmed mount recovery. No remount-rw added; WS004/WS018 |
| U3, U4, U5 | U5 synchronous hang fixed without releasing unretired DMA. Failed endpoint/controller resources remain retained. | Checked device/controller restart, failed command-ring recovery, resource generation replacement; WS004 |
| U6 | Not implemented in q086. | Bounded same-physical-generation enumeration retry and exhausted/replug status; WS004 |
| U7, U13, U14 | No speculative IRQ/command/terminal redesign. Existing recovery/ownership gates retained. | Monotonic command deadlines, diagnostics, CPU-A submit/CPU-B completion+cancel reproduction; WS004 |
| U8 | Four actual kernel/QEMU USB-root cells tested; default 4000 retained. | Physical IRQ interval histogram, CPU cost and repeated 0/160/4000 comparison; WS004 |
| U11 | No HID retry-policy change in this cycle. | Bounded rearm after non-STALL errors and key-state cleanup; WS004/WS006 |
| U12, U15 | Deliberately deferred as agreed in report-4. | External hub, READ/WRITE(16), large capacity, UAS; WS004 |

Prioritize the U14 reproduction and U6 finite enumeration recovery when selecting
the next USB phase. Preserve the q086 50-story gate and Q085 Wi-Fi30 regression.
Do not clear a mounted filesystem's readonly state solely because a later driver
flush succeeded. A physically replaced medium must not inherit old caches/claims.
