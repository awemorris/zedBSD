# WS025 mandatory acceptance mapping

2026-09-08 JST. This maps all 97 mandatory IDs in acceptance.md to final or
explicitly inherited evidence. It does not relabel physical acceptance as an
agent measurement. Conditional p027–p030 are excluded by the recorded adoption
criteria, not silently counted as implemented.

| IDs | Evidence / result |
| --- | --- |
| MEM01–MEM16 | p026-host-2 memory/vmap/DMA groups and 14 BIOS/UEFI USB-root RAM cells in p026-memory-1 and p026-memory-2g-1; current low/high-RAM combined native. Completed p002–p005 retain typed handoff compatibility, exact high PFN/COW, reserved ranges and AP lifetime proofs. p023 specialized mapping proof is retained for unchanged implementation. WLAN/HID physical portion is user-accepted |
| IO01–IO12 | p026-host-2 pool, buffer run, UFS run/allocation, FAT batch/cache, DMA/vector; FS50 S44 current syscall extraction; p026-baseline-1 has 404 checked samples; p025 USB reserve/late-owner gates remain exact production evidence |
| META01–META10 | p026-host-2 UFS allocation/metadata and FAT batch/private/cache groups; completed p010–p013 failure/rollback contracts; p021 operation-specific journal matrices for unchanged UFS implementation |
| FLUSH01–FLUSH06 | p026-host-2 flush-frontier, policy, credits, unmount rollback and journal; p025 finite-reset/proof-only/MODE faults; p026 native off/on/fsync/remount and FS50 wifi-store directory persistence |
| CACHE01–CACHE09 | p025-final-file-cache-1 is exact current production: ordinary/sanitizer lifetime/coherence/pressure; p026-host-2 cache buffer/device/policy and native low/high RAM; p025 native stale-cache media refusal |
| WB01–WB10 | Current file-cache error-cursor/lifetime regression, p026-host-2 credit/policy/unmount/shutdown, current USB/NVMe writeback native; p025 checked clean shutdown and offline image verification |
| ASYNC01–ASYNC08 | p025-final-async-1 and self-reset/USB ownership gates, current FS50 retained-staging/late-completion stories; p026 device/worker gates and current multi-device native |
| READ01–READ04 | p026-host-2 state/worker and file-cache lifetime; current 256 MiB/8 GiB/16 GiB sequential/random native, optional work/errors/retirement counters |
| EXEC01–EXEC05 | p026-host-2 exec-snapshot-vm and actual pool/exec extraction; immutable mapping/edge/COW/rollback tests; current repeated native process execution; completed p022 specialized sharing proof retained |
| SG01–SG05 | Current vmap/DMA/disk-vector host gates; p024 measured discontiguous staging, copy/TRB/CPU/latency comparison retained for unchanged HAL/DMA/xHCI implementation; p026 baseline confirms no redundant HCD copy. Direct user DMA is not adopted |
| REC01–REC06 | Completed p025 final acceptance: actual BOT reset/MODE/capacity/absence classification, lifetime/claim/partition/loop tests and ordinary real same-capacity removable media exchange; new identity only for idle old ownership |
| CRASH01–CRASH06 | p026-host-2 journal crash/replay/snapshot and allocation/metadata tests; p021 deferred operation-specific failure matrices for unchanged UFS core; p026 FS50 two-boot USB persistence and p025 shutdown/offline-content verification |

Fresh integration results:

- `p026-host-2`: all 24 maintained runner families pass, including ordinary and
  sanitizer variants; memory runner also covers its individual HAL groups.
- `p026-memory-1` plus `p026-memory-2g-1`: both firmware modes at 256/512 MiB and
  1/2/4/8/16 GiB pass (14 cells). Every cell uses four CPUs and USB root.
- `p026-native-1`: low-RAM USB, 16 GiB USB and 8 GiB NVMe pass combined ZUJ2,
  writeback and readahead. p025 final ordinary 8 GiB also tests actual exchange.
- `plan/ws018-kernel-architecture/temp/ws025-p026-final-2`: FS50 50/50 plus Wi-Fi30
  ordinary/sanitizer 30/30, including native USB reboot persistence. It covers
  current syscall/q087 transfer semantics; RF behavior is not inferred from it.
- `p026-baseline-1`: 404 samples and both oracle runs pass; see performance.md.

Inherited results are not claimed to have been rerun here. Provenance comparisons
are in p026-provenance-1. The entire p025 production/config snapshot is unchanged.
The selected p023 HAL files and 66 relevant p024 HAL/DMA/xHCI files match. p021
UFS transaction/journal/consistency implementation hashes match; outer file and
writeback policy changed since p021 and have their current p025/p026 tests, rather
than being falsely described as unchanged. The old operation matrices remain
specific evidence for the unchanged core, not all of the outer stack.
