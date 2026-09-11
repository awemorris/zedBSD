# WS004 Phase 023: NVMe I/O and controller lifecycle

Last updated: 2026-08-30

Phase ID: `ws004-p023`

Status: completed (`q030`)

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

## Result

Completed on 2026-08-30. The namespace published by p022 now owns one I/O
submission/completion queue pair with depth 64 and 63 independently owned
4-KiB coherent bounce buffers. The common block path provides checked 64-bit
LBA read/write splitting and a truthful flush barrier; raw devfs block access
also retains 64-bit byte offsets instead of truncating accesses above 4 GiB.

The command ledger tracks slot, CID, epoch, submission ownership, CQ phase and
terminal state separately. Multiple synchronous callers may have commands in
flight, completions may arrive out of order, and read data is copied from the
private bounce buffer before that slot can be reused. IRQ completion never
finishes a caller-owned stack BIO directly. A flush excludes new data commands,
waits for admitted data commands, submits NVM Flush, and reports success only
after its completion.

Timeout and fatal-queue recovery stop admission, fail each owned BIO exactly
once, mask and drain interrupt delivery, disable the controller and bus
mastering, recreate the admin and I/O queues, and reopen admission only after
the whole transaction succeeds. A timed-out write is not retried. Any failure
to prove hardware/DMA quiescence retains the controller in an unavailable
quarantine rather than releasing live queue or payload memory. Detach first
makes the disk unreachable, preserves its final controller-cache FLUSH as a
retryable obligation, and refuses to release resources if that obligation
cannot be completed safely. Terminal PCI shutdown stops admission, requests
normal `CC.SHN`, waits boundedly for `CSTS.SHST`, and attempts both independent
controller-disable and PCI bus-master-disable boundaries even when an earlier
step fails. System-wide filesystem sync remains owned by the generic shutdown
path; the PCI shutdown callback does not invent a second filesystem-sync
transaction.

The first QEMU data run exposed a boot-context scheduler defect: initial VFS
partition reads run on CPU 0's idle thread, which cannot block on the ordinary
wait queue while its interrupts remain disabled. The final implementation
polls that special pre-user context with interrupts restored and scheduler
yields; ordinary threads retain the wait-queue path. The deterministic first
read then completed normally.

[HW-T20 p023 evidence](../tests/q030-nvme-io-evidence.md) records all focused
ordinary/sanitizer/analyzer passes, the production amd64 and configured i386
PC/AT builds, QEMU run 6, and passing IDE plus xHCI USB-root regressions. The
QEMU gate uses a disposable 5-GiB namespace: write/descriptor-`fsync`/readback
passes at 8 MiB and above 4 GiB, 96-command phase-wrap and four-worker
128-command concurrency loads pass, and a second controller/guest boot reads
back every pattern. QEMU's NVMe trace proves SQ1/CQ1 wrap and actual NVM
read/write/flush commands; the source system image digest is unchanged.

The completed Phase remains intentionally limited to one controller, one
512-byte active namespace, one I/O queue, one message interrupt, and bounded
single-page coherent bounce buffers. It does not claim scatter/gather,
arbitrary user-page DMA, IOMMU isolation, namespace hotplug, multipath,
multiple I/O queues, power-state management, physical SN740 operation, or GPT
partition discovery. Strict GPT and the final disposable acceptance matrix are
next in `ws004-p024`; the Latitude remains the separate read-only `p025`
checkpoint.

## Reconsideration boundary

Return to planning if safe operation requires arbitrary user-page DMA,
multiple interrupt vectors, namespace-management commands, or a block-layer
ordering change outside this Phase.
