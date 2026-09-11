# WS017 Phase 002: amd64 boot-framebuffer LFB backend

Last updated: 2026-08-27

WSID: `ws017`

Phase ID: `p002`

Combined ID: `ws017-p002`

Status: Planned; Queue-ready after `ws017-p001`

Parent: [WS017](../ws.md)

Tests: [WS017 test index](../tests/README.md)

## Objective

Expose the already validated amd64 GOP/VBE boot framebuffer through the p001
optional mapping contract while retaining all current kernel drawing and
fallback behavior.

## Work packages

1. Extend the internal PC/AT graphics-driver descriptor with the physical base,
   checked physical size, page-aligned mapping base/length, visible data
   offset, full-page aperture-ownership evidence, and direct-RGB shift/mask
   layout derived from the accepted handoff.
2. For current `RGBX8888` and `BGRX8888` formats, report 32 bpp and the exact
   little-endian red/green/blue shifts and `0xff` masks; do not infer a format
   from dimensions alone.
3. Require the entered fixed mode to equal the handoff geometry/stride and
   require the visible final byte to fit both handoff size and the architecture
   mappable framebuffer window.
4. Map only pages wholly covered by a framebuffer-owned handoff/MMIO aperture.
   A page-aligned GOP/VBE base and a reported aperture large enough for the
   page-rounded visible extent are sufficient; an unaligned base or partial
   final page without equivalent architecture ownership evidence makes mmap
   unavailable rather than exposing neighboring bytes.
5. Supply the optional LFB callback only for `DISPLAY_LINEAR`. Cirrus and planar
   VGA remain unavailable and continue through existing operations.
6. Ensure the kernel's framebuffer mapping and user mappings refer to the same
   physical aperture with consistent cache/device attributes and no allocator
   ownership transfer.
7. Preserve `leave`, console resume, panic/diagnostic output, and subsequent
   graphics reopen after the final user mapping is released.

The generic UAPI accepts future 8/16/24 layouts, but the current amd64 UEFI
GOP handoff advertises only its real 32-bit RGBX/BGRX mode. This Phase does not
fabricate lower-depth modes or change firmware mode selection.

## Verification and completion conditions

LFB-T005--T007 must cover both RGBX/BGRX mask orders, offset/extent boundaries,
mapping read/write coherence with the kernel view, rejection of an unproven
unaligned/partial-page aperture, console ownership, final unmap/reopen, and
Cirrus/VGA unavailability. QEMU amd64 BIOS and UEFI smoke
must retain current graphics mode entry. The Phase is complete when those
tests, affected handoff/graphics/VM regressions, `make -j16`, and
`git diff --check` pass without PC-98 graphics changes.

## Reconsideration boundary

Stop if firmware reports `PixelBltOnly`, an unsupported pixel layout, or an
aperture outside the validated physical/MMIO window, including a rounded edge
page whose complete ownership is not proven. Keep the ioctl fallback; do not
guess masks, expose adjacent bytes, or expand this Phase into firmware
modesetting.
