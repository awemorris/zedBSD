# WS004 Phase 022: NVMe controller and namespace discovery

Last updated: 2026-08-29

Phase ID: `ws004-p022`

Status: completed (`q030`)

Parent: [WS004 hardware expansion](../ws.md)

Tests: [WS004 test index](../tests/README.md)

## Objective

Implement the first native NVMe path on the existing PCI, DMA, and message-
interrupt foundations.  A QEMU NVMe controller must reset, create its admin
queues, identify itself and one 512-byte-LBA namespace, and publish that
namespace as `/dev/nvme0n1` without permitting data writes yet.

## Frozen implementation boundary

- Match the standard PCI mass-storage/NVM/NVMe class tuple, not a vendor ID.
- Map and validate BAR0, CAP, VS, CC, CSTS, doorbell stride, page size, queue
  depth, timeout, and controller-ready transitions with bounded waits.
- Use coherent DMA allocations for the admin submission/completion queues and
  identify buffers. Prefer MSI-X and permit the existing single-message MSI
  fallback; this Phase does not expand the public MSI API or add polling as a
  silent substitute.
- Create and delete the admin queue ownership transactionally. A failed probe,
  timeout, or detach must not leave an enabled controller, registered IRQ, or
  live DMA buffer.
- Support the minimum initial profile: one controller, one active namespace,
  one admin queue, 512-byte logical blocks, and no namespace hotplug.
- Add stable `nvme<controller>n<namespace>` disk naming. Partition naming stays
  one-based, so the first GPT partition is `/dev/nvme0n1p1`.
- Publish the namespace as discovery-only in this Phase. The generic block
  layer rejects writes with `EROFS`; reads return `EOPNOTSUPP` until p023 adds
  the NVM I/O queue. NVM read/write/flush commands and the usable block-data
  contract belong to p023.

## Verification plan

1. Add focused register/CAP/identify/queue fixtures covering malformed limits,
   timeout, stale phase tags, and cleanup after every injected failure.
2. Run ordinary, sanitizer, and analyzer variants where the fixture supports
   them.
3. Run `make -j16` and boot a disposable amd64 UEFI QEMU guest with a standard
   NVMe device. Confirm one `/dev/nvme0n1`, the expected capacity, and no
   writable command path.
4. Do not use `make check` or `.internal/` material.

## Completion conditions

- Standard QEMU NVMe identifies through bounded admin-queue operation and
  publishes exactly one truthful read-only namespace.
- Invalid capabilities and all injected timeout/probe failures either release
  IRQ, DMA, queue, PCI, and disk-registry ownership exactly once, or retain the
  still-owned graph in an explicit retryable quarantine when hardware
  quiescence cannot be proved.
- The existing IDE and USB-storage boot paths remain passing.

## Result

Completed on 2026-08-29. The standard PCI class driver now performs bounded
reset and admin Identify over coherent queues, uses MSI-X with single-message
MSI fallback, validates CQ phase/SQ/CID ownership, accepts a truthful sparse
active NSID, and publishes exactly one discovery-only `/dev/nvme0n1` with
stable one-based partition naming.

The production attach path and the host failure fixture share the same strict
cleanup ledger. Probe/detach admission is serialized, aborted detach restores
normal admission, and a failure to prove controller/IRQ/PCI quiescence retains
ownership for retry instead of freeing live DMA. QEMU exposed and the Phase
fixed two interrupt-ordering defects: NVMe INTMS/INTMC cannot mask MSI-X after
MSI-X enable, and an admin waiter must yield with CPU interrupts restored so a
CPU0-targeted message can be delivered.

Final static review moved PCI bus-master quiescence ahead of BAR mapping,
checks the mapped BAR before the first MMIO access, uses the tick deadline once
the clock advances, and keeps only the bounded spin fallback needed before the
first timer tick. Checked PCI message teardown now restores the prior MSI or
MSI-X capability/table image transactionally; single-message MSI clears MME
while active, and firmware-inherited MSI/MSI-X is disabled before the first
controller reset and restored only after safe teardown. The disk-name storage
also represents the complete non-reserved 32-bit NSID range.

[HW-T20 p022 evidence](../tests/q030-nvme-admin-evidence.md) records the focused
ordinary/sanitizer/analyzer passes, amd64 and i386 PC/AT builds, exact 32 MiB
namespace/non-mutation QEMU proof, and passing IDE plus xHCI USB-root
regressions. The expected `geometry error=21` scan diagnostic remains because
p022 deliberately has no NVM read command; p023 removes it by adding the I/O
queue.

## Reconsideration boundary

Return to planning if the existing single-message MSI contract cannot service
one admin and one I/O queue, the target requires an IOMMU before safe DMA, or a
512-byte namespace cannot be selected without changing the public block ABI.
