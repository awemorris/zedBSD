# WS004 Phase 023: NVMe I/O and controller lifecycle

Last updated: 2026-08-29

Phase ID: `ws004-p023`

Status: planned; depends on `ws004-p022`

Parent: [WS004 hardware expansion](../ws.md)

Tests: [WS004 test index](../tests/README.md)

## Objective

Turn the discovered NVMe namespace into a writable zedBSD block device with
bounded read, write, flush, timeout, reset, shutdown, and detach behavior.

## Frozen implementation boundary

- Create one I/O submission/completion queue pair using coherent queue memory
  and the existing single interrupt mapping.
- Initially use bounded coherent bounce buffers for payload I/O. The current
  DMA layer has neither general scatter/gather mapping nor an IOMMU contract;
  zero-copy and multi-segment PRP optimization are later work.
- Implement namespace read, write, and flush with 64-bit LBAs, checked range
  arithmetic, command-ID ownership, phase-tag handling, maximum-transfer
  limits, and exact completion status translation.
- Never report flush success unless the command completed successfully. Keep
  the block layer's `BIO_FLUSH` ordering contract.
- On timeout or fatal status, stop new submission, complete every owned BIO
  exactly once, reset/recreate queues when safe, and either restore service or
  leave the disk unavailable with a precise diagnostic.
- Detach and shutdown must quiesce submissions and interrupts before freeing
  queue or payload DMA. Namespace hotplug, multiple I/O queues, multipath, and
  power-state management are outside this Phase.

## Verification plan

1. Exercise read/write boundary arithmetic, split transfers, flush ordering,
   queue wrap, phase changes, command-ID reuse, out-of-order completions, and
   injected status failures in focused fixtures.
2. Exercise timeout/reset and detach with in-flight reads and writes, proving
   exactly-once completion and no DMA reuse.
3. Run ordinary, sanitizer, and analyzer gates, followed by `make -j16`.
4. In a disposable QEMU NVMe image, write, flush, reread, reset, and verify a
   pattern outside any source image. Preserve IDE and xHCI USB-root boots.

## Completion conditions

- Read, write, and flush operate through the common block layer with truthful
  errors and 64-bit LBAs.
- Normal wrap/concurrency and every declared failure path preserve queue, BIO,
  IRQ, DMA, and disk lifetime.
- Disposable QEMU data survives flush plus controller/guest restart.

## Reconsideration boundary

Return to planning if safe operation requires arbitrary user-page DMA,
multiple interrupt vectors, namespace-management commands, or a block-layer
ordering change outside this Phase.

