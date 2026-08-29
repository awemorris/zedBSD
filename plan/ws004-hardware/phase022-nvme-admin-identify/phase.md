# WS004 Phase 022: NVMe controller and namespace discovery

Last updated: 2026-08-29

Phase ID: `ws004-p022`

Status: planned; ready for a future Queue proposal

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
- Publish the namespace read-only in this Phase. NVM read/write/flush commands
  and the writable block contract belong to p023.

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
- Invalid capabilities and all injected timeout/probe failures release IRQ,
  DMA, queue, PCI, and disk-registry ownership.
- The existing IDE and USB-storage boot paths remain passing.

## Reconsideration boundary

Return to planning if the existing single-message MSI contract cannot service
one admin and one I/O queue, the target requires an IOMMU before safe DMA, or a
512-byte namespace cannot be selected without changing the public block ABI.
