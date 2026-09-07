# WS025 effective policy and conditional adoption

2026-09-08 JST. Selected policy for the completed mandatory implementation;
final integration evidence is recorded in results.md. No new runtime switch or
unmeasured tuning value is introduced by this document.

| Area | Selected behavior | Reason / boundary |
| --- | --- | --- |
| amd64 RAM | Use validated reported usable RAM beyond 1 GiB, with sparse mapping and explicit reserved holes | The old test cap is removed from loader/HAL admission; physical acceptance is user-accepted |
| Cache budget | Shared soft target of one quarter of managed RAM, plus a bounded physical free-memory floor | Track file/block metadata/data, pools and DMA together; reclaim eligible clean owners under pressure |
| Syscall / exec copy | Borrow bounded shared scratch, up to 64 KiB per batch; stack/small reserve fallback | No ordinary per-call large heap allocation; streaming files and resource exhaustion retain correct partial-I/O behavior |
| File and block cache | Enabled for eligible ordinary storage with coherent lifetimes, bounded run fills and reclaim | Dirty/inflight/pinned owners cannot be evicted as clean; disk and loop media admission applies to hits |
| Physical backing | Contiguous frames first where appropriate; owned kernel vmap fallback for supported page-backed cache/workers | Virtual reservation, physical commitment, pinning and DMA mapping remain separate lifetimes |
| FAT / UFS | Batch contiguous data and allocation/metadata operations within their supported transaction contracts | FAT metadata remains synchronous; unrelated persistence boundaries never merge merely for fewer calls |
| UFS journal | ZUJ2 version 2 only, with replay, immutable transaction snapshots and ordered home installation | Journal v1 compatibility is retired; no mixed v1/v2 publication |
| Writeback | Explicit opt-in per eligible mount through vfs.writeback.control | Supported UFS operations and persistence-capable backing can defer writes; automatic global enablement would change ordinary write-error/crash exposure without an explicit mount-policy request. O_SYNC/O_DSYNC, fsync, disable/drain/unmount/shutdown retain their stronger boundaries |
| Read ahead | Enabled for eligible sequential ordinary-file reads; 64 KiB request, 64–128 KiB lookahead | Useful consumption controls growth; random access, generation changes, pressure and queue refusal abandon optional speculation |
| Exec sharing | Immutable read-only backing may share cache-owned full pages | Writable input and unsupported shapes keep private snapshot/COW handling; writable file data is not shared as immutable text |
| USB BOT / xHCI | Owned reserved staging shared by core and HCD; bounded SG TDs, 64 KiB eligible transfers | One caller-isolation copy remains; unsupported HCD/large-sector cases retain the checked smaller adapter. Existing conservative PCI DMA mask remains |
| Recovery | Finite same-medium command retry; separate periodic control owner for media retirement/reprobe | New media requires new disk identity. Old mounted/claimed/busy owners remain failed; no forced old-cache write into replacement media |
| Shutdown | Drain filesystem obligations, close device admission, check HCD quiesce, retain still-owned memory | Retention does not masquerade as running DMA or permit unsafe free |

Writeback opt-in is a deliberate final default choice, not a missing worker,
journal or lifecycle implementation. The on/off/drain paths are part of mandatory
acceptance. Expanding supported deferred operations or changing mount defaults
requires its own workload and persistence review.

## Conditional phases

| Phase | WS025 decision | Concrete resume condition |
| --- | --- | --- |
| p027 NVMe queue depth | Not adopted; keep planned | Measure a depth-1 bottleneck on the target NVMe device and demonstrate a bounded CID/PRP/timeout ownership design with useful depth scaling |
| p028 direct user I/O | Not adopted; retain owned staging/copy | Demonstrate that remaining user-copy CPU is a workload bottleneck and validate pin/COW/unmap/truncate/cancel lifetimes for a specific direct path |
| p029 UAS | Not adopted; keep planned | Obtain target device descriptors and usable stream/pipe capabilities, then accept a separate UAS owner before multiplexing |
| p030 IMOD | Not adopted; keep current 4000 and planned | Compare 0/160/4000 on real supported topology with storage plus WLAN/HID, measuring correctness, CPU and latency |

p024 measured removal of the redundant HCD copy; its controlled native comparison
did not demonstrate CPU/latency improvement. That does not justify direct user
DMA or new queue-depth/interrupt defaults. User acceptance of the physical gate
is not substitute numeric evidence for these optional optimizations. These four
phases are intentionally not claimed as implemented or tested.
