# p004 physical address audit

Date: 2026-09-07. Scope: normal RAM publication prerequisites, not a claim that every driver supports DMA64.

| Owner | Source / finding | Publication rule |
| --- | --- | --- |
| HAL physical address | `include/hal/types.h`: `hal_physaddr_t` is `uintptr_t`, 64-bit on amd64 | Keep intermediate range bounds 64-bit; other HALs validate before returning their narrower descriptor. |
| User pages / reclaim | `src/kern/vm-object.c` allocates HAL-backed pages; `src/kern/vm-reclaim.c` passes `backing->pmem.paddr` to the HAL mapper | No observed PA-to-u32 narrowing; native high-PFN COW/reclaim evidence belongs to p005. |
| amd64 page tables | `src/hal/amd64/space.c`: checked RAM-window lookup, physical leaf permission validation, shared kernel half | Root used by `ap-trampoline.S` remains low; general page-table allocation may become high only after direct-map publication. |
| PCI common policy | `src/drivers/pci-pcat.c`: address_bits=32, segment maximum 16 MiB | Preserve the conservative DMA32 policy while normal RAM becomes high. Device capability promotion is separate. |
| xHCI | `src/drivers/pci-xhci.c`: u64 device addresses; low/high TRB fields and u64 DCBAA | Coherent allocator enforces PCI mask before descriptors are encoded. |
| NVMe | `src/drivers/pci-nvme.c`: u64 device addresses passed to PRP/queue setters with alignment checks | Keep current bounce path; queue depth and SG are later phases. |
| AX211 | `src/drivers/` AX211 DMA path uses coherent buffers, checked end/alignment and explicit 64-bit encoding | Retain PCI DMA32; no normal kernel pointer is treated as a device PA. |
| EHCI / UHCI | `src/drivers/pci-ehci.c`, `src/drivers/pci-uhci.c`: 32-bit descriptor addresses and range guards | These casts require the DMA32 allocation contract; do not enable a blanket DMA64 default. |
| RTL8822BU | USB transport uses HCD staging buffers | The HCD owns DMA constraints; WLAN does not bypass them. |
| Streaming map | `src/drivers/dma.c`: only a live coherent allocation's advertised payload can be mapped | Reject padding, oversized segments and unsupported pointers. Map/free lifetime remains a caller obligation; reservation/retirement work belongs to p009/p024. |

## High-memory gate

p004 keeps the normal 1 GiB publication limit. Its host fixtures exercise actual production extent searches above 4 GiB, including low-pool exhaustion. Those are not native high-memory acceptance.

p005 opens normal RAM only after preserving all bootstrap owners and verifies actual high PFNs through kernel/user/COW paths. Normal allocation must search above the DMA32 ceiling first **even when a single firmware extent crosses that ceiling**; reverse extent ordering alone is insufficient. Low DMA capacity is not an arbitrary permanently excluded RAM block: the minimum reserve must follow measured active-device demand, and free/reserved/allocated statistics must remain disjoint. p009 will add explicit device reservations.

Boot-reclaim memory stays withheld until loader lifetimes and surviving ACPI references are accounted for. Merely completing ExitBootServices does not prove those pages are free. Real-machine acceptance is recorded separately from the QEMU matrix.
