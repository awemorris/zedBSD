# WS003 Phase 002: Latitude UEFI memory-map normalization

Last updated: 2026-08-25

WSID: `ws003`

Phase ID: `p002`

Combined ID: `ws003-p002`

Status: uncleared; ExitBootServices sequencing correction awaits physical validation

Acceptance disposition: **Uncleared**

Parent: [WS003](../ws.md)

Tests: [WS003 test index](../tests/README.md)

## Objective

Make the production amd64 `BOOTX64.EFI` accept and safely normalize the Dell
Latitude 5320 firmware memory map, exit UEFI boot services, and enter the
kernel with a valid, bounded handoff.

## Physical finding

The target loads the zedBSD USB image and displays:

```text
A64 UEFI ENTRY
A64 UEFI ELF
A64 UEFI READY
Normalize memory map: 0x8000000000000001
```

The visible status is the loader's locally selected `EFI_LOAD_ERROR`, not a raw
status returned by firmware. Source inspection shows that normalization rejects
at least these conditions: a short/misaligned descriptor stream, page-count or
address overflow, descriptors not monotonically ordered, overlapping ranges,
more than `MAX_MEMORY_RANGES`, or an empty result. The photo alone does not
distinguish them.

The next physical run advanced past that rejection but displayed only
`A64 UEFI READY`. Source review found that the final map was normalized between
`GetMemoryMap()` and `ExitBootServices()`. On firmware which changes its map
asynchronously, the sort/merge delay can invalidate the key on every attempt;
the old three-failure path then halted without a visible post-READY message.
The final normalization is now deferred until after a successful exit, leaving
only the required two firmware calls in the retry interval.

The highest proven physical tier is U0: firmware loads and runs the UEFI loader.
U1 kernel entry and M2 Latitude USB shell acceptance are Uncleared. The static
screen is a deliberate loader halt after the diagnostic, not evidence that the
machine reached the kernel.

## Scope

- Capture descriptor size/version/count and a bounded diagnostic identifying
  the exact normalization rejection without dumping sensitive firmware data.
- Compare the Latitude map shape with the UEFI contract and QEMU/OVMF control.
- Accept valid maps regardless of descriptor ordering by sorting/merging a
  loader-owned representation when required, while rejecting real overlaps,
  integer overflow, malformed descriptor sizing, and capacity exhaustion.
- Keep `GetMemoryMap`/`ExitBootServices` retry semantics valid: do not allocate
  memory after obtaining the final map key.
- Verify that normalized ranges retain reserved/runtime/MMIO distinctions
  required by the amd64 HAL and do not expose unavailable memory.

## Non-goals

- Continuing past malformed or overlapping firmware maps by silently dropping
  descriptors.
- Secure Boot enablement, kernel xHCI bring-up, or root-filesystem repair.
- Treating the attached photo as a complete hardware inventory.

## Expected files and subsystems

- `bootloader/uefi/bootx64.c`
- amd64 handoff memory-range definitions and validation, if evidence requires
- WS003 host fixtures and QEMU/Latitude evidence

## Ordered work packages

- [x] Add bounded reason diagnostics; a fresh Latitude result is still needed
      to identify any remaining rejection.
- [x] Add host fixtures for sorted, unsorted-valid, adjacent, overlapping,
      malformed-size, overflow, and range-capacity maps.
- [x] Implement safe canonical ordering/merging at the loader boundary.
- [x] Verify QEMU/OVMF still emits `A64 UEFI EXIT` and reaches kernel/login.
- [x] Re-verify QEMU/OVMF after moving final normalization past
      `ExitBootServices`.
- [ ] Rebuild the USB image and verify the Latitude passes normalization,
      `ExitBootServices`, and kernel entry.
- [ ] Record the new highest U-tier and hand off any next physical failure
      without claiming shell acceptance prematurely.
- [ ] Run focused tests, `make -j16`, and `git diff --check`; do not use
      `make check` or `.internal/` material.

## Completion conditions

- Valid descriptor maps in arbitrary order normalize deterministically; invalid
  size, overflow, overlap, and capacity cases fail with distinct diagnostics.
- QEMU/OVMF retains its current UEFI boot behavior.
- The Latitude passes `Normalize memory map`, exits boot services, and reaches
  an unambiguous kernel-entry marker on three cold boots from the USB image.
- The handoff contains non-overlapping, ordered, correctly typed ranges within
  the fixed handoff capacity.
- Focused tests, `make -j16`, and `git diff --check` pass.

## Interruption / resumption

The BR-T23 fixture and post-sequencing OVMF USB run pass. OVMF emits `A64 UEFI
EXIT`, reaches the amd64 kernel, mounts the overlay, starts init, and reaches
`login:`. Resume with the rebuilt USB image on the Latitude and run BR-T32
three times. Do not infer the physical result from OVMF.
