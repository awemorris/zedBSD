# WS003 Phase 007: shared DMA allocation synchronization

Last updated: 2026-08-26

WSID: `ws003`

Phase ID: `p007`

Combined ID: `ws003-p007`

Status: Complete (`q013`)

Parent: [WS003](../ws.md)

Tests: [WS003 test index](../tests/README.md)

Evidence: [Latitude xHCI evidence](../tests/latitude-xhci-evidence.md)

## Objective

Make the shared PCI-root DMA allocation registry safe when both Latitude xHCI
controllers allocate and complete URB bounce buffers concurrently on separate
CPUs or in IRQ context.

## Confirmed defect

All PCI functions below one root bus currently share one `drv_dma_device`.
Its allocation list is updated without synchronization. Concurrent prepend,
unlink, free, map inspection, or device destruction can republish a freed list
node or dereference it, causing heap corruption which a one-controller QEMU
configuration does not expose.

## Scope

- Add one IRQ-safe lock to the DMA-device allocation metadata.
- Publish a completed allocation and unlink a selected allocation under the
  lock; perform page and heap release after unlocking.
- Synchronize mapping lookup/snapshot and device destruction/list inspection.
- Add BR-T36 host contention/lifecycle coverage where practical.
- Run the existing heap, PCI, USB, xHCI, build, and QEMU USB-root gates.
- Share the one final BR-T34 hardware boot with every q013 Phase.

## Non-goals

- Redesigning IOMMU domains or DMA address translation.
- Giving each PCI function a private DMA allocator merely to avoid locking.
- Requesting an independent physical test.

## Ordered work packages

- [x] Protect DMA allocation-list ownership with an IRQ-safe lock.
- [x] Add and pass BR-T36 focused concurrency/lifecycle coverage.
- [x] Pass existing DMA/PCI/USB/xHCI/heap regressions and `make -j16`.
- [x] Pass legacy USB root, BR-T24, and BR-T29 on the frozen candidate.
- [x] Feed the one shared BR-T34 observation into this Phase.

## Completion conditions

- No allocation-list operation races another list reader or writer.
- No physical-page or heap release occurs while holding the metadata lock.
- Focused and integrated automatic gates pass.
- The single shared BR-T34 observation is recorded.

## Actual results and evidence

- `drivers/dma.c` now initializes one `LOCK_RANK_DEVICE` IRQ-safe spinlock per
  DMA device. Allocation publication, unlink, map lookup, the in-flight
  operation counter, and the fail-closed `destroying` state use that lock.
  `hal_malloc`, `hal_free`, `hal_pmem_alloc`, and `hal_pmem_free` all remain
  outside its critical section.
- `drv_dma_device_destroy()` closes new allocation/map work on its first call,
  returns `EBUSY` while operations or coherent allocations remain, permits
  coherent frees to drain, and succeeds only on a later empty retry. The
  public lifetime contract is recorded in `include/drivers/dma.h`.
- BR-T36 (`tests/dma-allocation-lock-test.c`) passed concurrent allocation,
  map/list lookup, unlink/free, and destroy-close contention. The existing
  WS004 DMA constraint fixture and the applicable PCI/USB/xHCI/heap host
  regressions also passed.
- `make -j16` and `git diff --check` passed. Candidate SHA-256
  `bd3aa801ac890deabb5f0ad4b6f3388e5137992e9f6f81e8d912af4abad7585f`
  passed the legacy BIOS q35/xHCI USB-only root gate, BR-T29 remove/re-add, and
  BR-T24 at 4, 8, and 16 GiB (3/3).
- The one shared BR-T34 Latitude run exercised both attached physical xHCI
  controllers, configured devices, and reached `usb-storage: sda` and BOT I/O
  without a DMA-registry, heap, retained-buffer, or controller-lifetime
  failure. This completes the Phase. The later SCSI cache-capability stop is
  isolated in `ws003-p010`.

## Remaining debt and handoff

`drv_dma_mapping` currently snapshots a segment but does not pin its backing
allocation against a same-buffer coherent free. That pre-existing generic API
debt is not used by the current xHCI path; a future IOMMU implementation must
add mapping-to-allocation pin/reference ownership before supporting live
mapping/free concurrency. IOMMU isolation and scalable per-device allocation
accounting remain separate hardware work.
