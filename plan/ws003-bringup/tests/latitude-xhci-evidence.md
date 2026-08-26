# ws003-p003 Latitude xHCI capability/MMIO evidence

Last updated: 2026-08-26

Parent Phase: [ws003-p003](../phase003-latitude-xhci-capability-mmio/phase.md)

## 2026-08-26 corrected-image physical run

The user-supplied Latitude 5320 photograph records the first physical run after
the high-RSDP correction. The relevant display is:

```text
A64 ENTRY PASS
A64 PAGING PASS
A64 ACPI RSDP PASS rev=2 rsdt=64F740C4 xsdt=00000000:64F74188
A64 IRQ READY
A64 XMM CONTEXT PASS
boot: HAL initialized successfully. [cpu 8, memory 1024MB, timer 10ms]
usb: URB completion contract q009-release-acquire-v1
pci: ECAM segment 0000 bus 00 at 00000000:c0000000
pci: BAR0 assigned to f0800000 (64 KiB)
xhci: attach failed at capabilities (13)
pci: BAR0 assigned to f0810000 (64 KiB)
xhci: attach failed at capabilities (13)
graphics: built-in VGA 8x16 font selected
A64 TIMER TICK
boot: waiting up to 5 seconds for boot storage
boot: boot-storage wait expired
boot: VFS initialization
vfs: boot BIOS=80 MBR partition=1 devices=0
vfs: native boot disk=none physical disks=0
vfs: boot selector UUID=0711-fd8b
vfs: resolve boot selector failed (error 6)
VFS initialization failed (6); entering idle.
```

The photograph proves the former UEFI/RSDP boundary is crossed and the kernel
initializes ACPI, interrupts, XMM context, eight CPUs, the timer, ECAM, input,
and graphics. It is BR-T32 run 1/3, not yet the three-boot repeatability gate.
The reported 1024-MiB kernel memory is the current bounded general amd64 memory
model and is not evidence of this xHCI failure.

## Exact software boundary

`ENODEV` is value 13. In `drivers/pci-xhci.c`, the `capabilities` stage sets
that value only for the compound check of:

1. zero MaxSlots;
2. zero MaxPorts;
3. CAPLENGTH below `0x20`;
4. HCIVERSION outside the currently accepted `0x100` and `0x110`;
5. the last PORTSC outside BAR mapping extent;
6. RTSOFF outside the mapping;
7. less than `0x40` bytes at RTSOFF;
8. DBOFF outside the mapping; or
9. the required doorbell array outside the mapping.

The current log does not identify which predicate failed. It also reads the
capability MMIO values before calling `drv_pci_device_enable_memory()`. The
ownership walker performs further MMIO before that call. This is a concrete
ordering defect whether or not it is the only physical cause.

Both `BAR0 assigned` messages come from the PC/AT BAR-map fallback. The amd64
HAL currently maps only a fixed `0xf0000000`--`0xf1000000` general PCI MMIO
window. Failure to map the original BAR causes a 32- or 64-bit BAR no larger
than 16 MiB to be reassigned into that window. The current fallback does not
establish from its log whether the original BAR was a high 64-bit address,
whether PCI Memory Space decoding was active, whether the BAR pair read back,
or whether parent bridge/resource windows admit the new address.

## Causal chain

```text
xHCI capability validation fails twice
  -> no xHCI HCD registers
  -> no USB mass-storage block device enumerates
  -> boot-storage wait expires with devices=0
  -> UUID has no candidate device
  -> ENOENT (6), VFS enters idle
```

The photograph therefore does not establish a separate VFS, UUID, or UFS
defect. U2 remains uncleared.

## Evidence required by the first diagnostic image

For each xHCI function, retain one bounded record containing:

- PCI segment/BDF, vendor/device/subsystem/revision and parent bridge;
- PCI COMMAND before and after Memory Space enable;
- original BAR0 low/high, type, size, assigned BAR readback, and mapping size;
- CAPLENGTH, HCIVERSION, HCSPARAMS1/2, HCCPARAMS1, DBOFF, and RTSOFF;
- explicit zero/all-one detection and the failed capability predicate;
- the next named stage if capability validation succeeds.

This data selects among decode ordering, BAR routing/mapping, and compatibility
validation. No raw memory map, serial number, WLAN credential, or unrelated
firmware contents are needed.

## Specification audit findings to verify in the Phase

- xHCI HCIVERSION is a BCD interface revision. The existing driver handles only
  1.0 and 1.1, while the published xHCI specification also defines 1.2.
- HCSPARAMS2 Max Scratchpad Buffers uses bits 25:21 as the high field and bits
  31:27 as the low field; the current composition is reversed.
- an xECP Next pointer is relative to the current extended capability, while
  the current walker treats later pointers as base-relative;
- after HCRST clears, software must wait boundedly for `USBSTS.CNR` to clear
  before programming operational registers.

These findings are in the attach/start path but are not inferred to be the
photographed `(13)` without the raw snapshot.

## Disposition

- `ws003-p002`: former high-RSDP defect cleared in this run; BR-T32 is 1/3.
- `ws003-p003`: Planned, not started, and not authorized by q011.
- Highest proven tier: U1. The next target is U2, beginning with xHCI attach
  and USB mass-storage enumeration.

