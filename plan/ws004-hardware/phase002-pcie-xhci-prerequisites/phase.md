# WS004 Phase 002: PCIe and xHCI prerequisites

Last updated: 2026-08-25

Phase ID: `ws004-p002`

Status: complete software milestone

Parent: [WS004](../ws.md)

Tests: [WS004 test index](../tests/README.md)

## Objective

Separate the QEMU-testable PCIe contract needed before xHCI from facts that can
only be learned on the Latitude 5320, and implement the generic core pieces
that do not require a target-specific interrupt or firmware decision.

## Frozen prerequisite contract

- A host bridge declares 256-byte conventional PCI or 4096-byte PCIe
  configuration space. Extended access is never guessed on mechanism 1.
- Extended capability walking is bounded, aligned, and rejects backward or
  cyclic links.
- Type-1 bridges create child buses from the firmware-programmed secondary bus
  number; tree scan and global iteration include descendants.
- ECAM discovery belongs to an ACPI MCFG host implementation. It must validate
  table checksums, segment/range overlap, address overflow, and mapping bounds.
- MSI/MSI-X requires a HAL allocator for non-ISA interrupt vectors and APIC
  message address/data construction. The current amd64 HAL exposes only IRQ
  1--15, so programming MSI before that allocator exists is forbidden.
- xHCI may use INTx in QEMU for its first transfer implementation only when the
  emulated controller actually exposes a routed INTx line. Latitude completion
  requires MSI/MSI-X and target ACPI evidence.

## Work packages

- [x] Publish the conventional/ECAM and MSI ownership boundaries.
- [x] Add host-declared configuration-space limits.
- [x] Implement bounded PCIe extended-capability walking.
- [x] Implement firmware-numbered bridge discovery and recursive scan/iterate.
- [x] Add a host fixture covering an extended-capability chain and child xHCI.
- [x] Pass the PCI/DMA focused suite and configured amd64 build.
- [x] Record the HAL-vector/MCFG handoff without claiming Latitude support.

## Completion conditions

The new PCIe core fixture, existing rescan/DMA regressions, and amd64 build pass;
the ECAM/MSI implementation boundary is explicit; and target-only facts remain
in `ws003-p001`. This Phase does not claim that xHCI or physical PCIe works.

## Evidence and result

The existing DMA-boundary and PCI-rescan fixtures pass. The new PCIe fixture
walks a two-entry extended-capability chain, discovers a firmware-numbered
child bus, enumerates a modeled xHCI-class child, and observes both devices
through recursive iteration. `make -j16 build/amd64/vmunix` and the complete
amd64 image build pass, followed by a QEMU boot to `login:`.

This is not ECAM/MSI or xHCI completion. The current PC/AT backend remains
explicitly 256-byte mechanism 1. ACPI MCFG mapping and an amd64 allocator for
non-ISA APIC vectors are required before MSI/MSI-X can be programmed; the HAL
currently rejects IRQs above 15. Latitude-specific firmware and routing facts
remain in `ws003-p001`.

## Resume point

Extract the next common-foundation Phase for ACPI MCFG plus dynamic APIC vector
allocation/MSI teardown, or an explicitly QEMU-only xHCI INTx Phase whose
completion does not imply Latitude support.
