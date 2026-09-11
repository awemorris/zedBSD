# HW-00 foundation audit

Last updated: 2026-08-25

## Capability matrix

| Area | Present implementation | Confirmed gap | Disposition |
| --- | --- | --- | --- |
| PCI configuration | PC/AT mechanism 1 at I/O ports `0xcf8/0xcfc`; segment 0; 256-byte configuration space | No ACPI MCFG/ECAM or PCIe extended configuration space | Required before a general Latitude PCIe claim |
| PCI topology | Root-bus slot/function enumeration | Bridge child-bus discovery and recursive traversal are not implemented | Design with ECAM work; current enumeration is not laptop-complete |
| PCI rescan | Reuses an existing bus object | Previously allocated duplicate device objects on every rescan | Fixed and covered by `pci-rescan-test.c` |
| PCI capabilities | Conventional capability list support | Extended capabilities return `ENOTSUP` | Needed for modern PCIe device facilities |
| BARs | 32/64-bit BAR sizing and mapping; small PC/AT MMIO assignment window | No firmware resource allocator and no general PCIe window management | Keep current path for QEMU legacy devices only |
| Interrupts | Legacy INTx through PIC/I/O APIC; amd64 affinity and teardown primitives exist | PCI MSI and MSI-X allocation/teardown are unsupported | Common prerequisite for xHCI/NVMe and modern hardware |
| DMA allocation | Coherent, physically contiguous allocation with address-width and maximum-size checks | Arbitrary mappings, scatter/gather, bounce buffering, noncoherent cache work, and IOMMU are absent | Later common-foundation implementation |
| DMA boundary | Constraint field existed but had no semantics or enforcement | A segment could cross its declared boundary | Defined as a power-of-two byte boundary and enforced by allocation alignment |
| USB host | UHCI and EHCI PCI drivers plus USB storage | No xHCI driver | HW-01 remains the next controller Phase |
| Target evidence | QEMU PC/AT build path is available | Latitude DMI/PCI/USB, ACPI routing, IOMMU, and controller IDs unavailable under WSL2 | Carried by `ws003-p001` |

## Regression evidence

The commands documented in the shared test index pass with host `cc` using
`-Wall -Wextra -Werror`. The PCI fixture scans the same bus twice and observes
one device. The DMA fixture verifies boundary alignment and rejects invalid or
too-small boundary constraints. `make -j16` then rebuilt the configured amd64
kernel and image, including the repository's amd64 ELF/image validators.

## Implementation cautions for HW-01

An xHCI driver must not be presented as a self-contained controller addition.
On the Latitude it is likely to require at least PCIe configuration access,
MSI/MSI-X, 64-bit-aware DMA, controller reset/timeout rules, and firmware/ACPI
resource correctness. Exact requirements remain an inference until the target
inventory is captured.
