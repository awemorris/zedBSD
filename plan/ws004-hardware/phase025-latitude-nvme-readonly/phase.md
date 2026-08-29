# WS004 Phase 025: Latitude 5320 NVMe read-only acceptance

Last updated: 2026-08-29

Phase ID: `ws004-p025`

Status: planned; physical acceptance; depends on `ws004-p024`

Parent: [WS004 hardware expansion](../ws.md)

Tests: [WS004 test index](../tests/README.md)

## Objective

Prove that the native driver recognizes the Latitude 5320's SanDisk SN740
controller (`15b7:5015`) and reads its namespace and existing partition table
without modifying internal storage.

## Safety boundary

- Use the normal class driver; do not add a device-ID-only success path.
- This Phase issues Identify and bounded reads only. It does not run a write,
  format, partition, firmware, sanitize, namespace-management, or destructive
  self-test command.
- Present one image and one explicit physical action at a time. Repeated boots
  are deferred to the final frozen installer acceptance rather than blocking
  intermediate implementation.

## Completion conditions

- One Latitude USB boot reaches a shell while the NVMe controller and namespace
  are identified with truthful model, capacity, 512-byte LBA, and partition
  diagnostics.
- Reading selected beginning, middle, and ending in-range blocks completes
  without timeout, reset, corruption, or modification.
- A failure records the exact PCI/admin/I/O boundary and returns to the owning
  driver Phase; it is not worked around in the installer.

## Reconsideration boundary

Stop before writing if the reported controller, namespace geometry, logical
block format, or PCI interrupt/DMA behavior differs from the accepted QEMU
profile.

