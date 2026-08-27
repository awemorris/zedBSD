# WS017 Phase 001: device-mmap and graphics LFB UAPI

Last updated: 2026-08-27

WSID: `ws017`

Phase ID: `p001`

Combined ID: `ws017-p001`

Status: Planned; Queue-ready

Parent: [WS017](../ws.md)

Tests: [WS017 test index](../tests/README.md)

## Objective

Add the generic VM/file lifetime needed for safe character-device mappings and
freeze the optional `/dev/graphics` LFB query/mmap contract without yet
enabling a production graphics backend.

## Scope

1. Add an optional mmap operation to file/cdev dispatch and route non-regular
   descriptors from `mmap(2)` only after normal flags, offset, length,
   protection, and descriptor access checks.
2. Add a device-backed VM-region type whose driver-supplied physical pages and
   HAL cache/device attributes are mapped directly rather than faulted from an
   inode or anonymous page.
3. Retain the mapping's file/device lease across fork, region split, partial
   unmap, descriptor close, and process exit. Final release calls the device
   operation exactly once; device regions are not swapped, reclaimed, written
   back, or counted as anonymous commitment.
4. Reject `MAP_PRIVATE`, nonzero offsets, execute permission, permission
   escalation through `mprotect`, out-of-user-range addresses, and extent/
   arithmetic overflow for graphics mappings.
5. Add `ZEDBSD_GRAPHICS_CAP_LFB_MMAP`, versioned
   `ZEDBSD_GRAPHICS_GET_LFB_INFO`, indexed/direct-RGB format constants, mapping
   flags, and the fields fixed by the WS contract.
6. Permit 16 bpp in central `GRAPHICS_ENTER` validation only when the selected
   driver actually advertises such a mode; retain all existing 4/8/24/32
   behavior.
7. Add an optional graphics-driver callback that supplies a kernel-only,
   page-aligned/page-multiple aperture ownership span plus public layout.
   Validate bpp, format, masks, stride, data offset, visible length, and full
   containment centrally. A non-page-aligned visible base is accepted only
   when the driver proves that the complete containing first/last pages belong
   to the framebuffer; otherwise advertise the LFB as unavailable.
8. Keep drivers without that callback fully functional with the existing
   ioctl path and an unavailable LFB query result.

## Verification

- LFB-T001--T004 cover UAPI structure layout on 32/64-bit targets, every valid
  8/16/24/32 layout, overlapping/noncontiguous/out-of-range masks, short or
  overflowing extents, wrong flags/offset/length/protection, and no-driver
  fallback.
- Device-VM fixtures cover fork, split/partial unmap, `mprotect`, descriptor
  close before unmap, process-exit cleanup, second-open exclusion, and exact
  one-time final release.
- Run affected mmap/VM/object/resource and graphics-device host regressions,
  `make -j16`, and `git diff --check`; do not run `make check` or use
  `.internal/`.

## Completion conditions

- valid device pages map with the requested device/non-cacheable attributes
  and cannot become executable;
- no page is mapped unless the driver-owned span proves that all prefix,
  visible, padding, and suffix bytes belong to the framebuffer aperture;
- all region derivatives retain the device lease and final release occurs once
  after the last derivative;
- `/dev/graphics` validates and returns the fixed public layout without a
  physical address;
- an absent or indexed/nonlinear fast path does not break legacy ioctl use;
- LFB-T001--T004 and affected regressions/builds pass.

## Reconsideration boundary

Stop if the existing VM-region model cannot represent a device lease without
pretending it is an inode or anonymous commitment. Extract that general VM
foundation rather than adding graphics-only PTEs outside VM ownership. Never
round an unproven visible physical range outward into adjacent RAM or MMIO.
