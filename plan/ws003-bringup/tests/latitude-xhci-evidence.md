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
and graphics. It was the first BR-T32 observation; subsequent evidence cleared
that U1 boundary. The reported 1024-MiB kernel memory is the current bounded
general amd64 memory model and is not evidence of this xHCI failure.

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

## 2026-08-26 q012 implementation and software evidence

The q012 implementation makes the physical discriminator part of the normal
driver log and corrects the confirmed ordering defect:

- records BDF, IDs, revision, parent bridge, original/final BAR address and raw
  pair, size, mapping extent, and PCI COMMAND transition;
- maps and validates BAR0, disables Bus Master, enables/read-backs Memory
  Space, and only then performs the first capability MMIO read;
- reports a stable reason mask and primary name for zero, all-one, unsupported
  version, malformed length, slots/ports, PORTSC, runtime, and doorbell cases;
- supports the explicitly verified xHCI 1.0, 1.1, and 1.2 capability contract;
- restores the original PCI enable state and original BAR on attach failure or
  detach;
- programs 32/64-bit BARs transactionally with decode/BME disabled, checked
  readback, rollback, and a fail-closed command state;
- corrects scratchpad Hi/Lo, relative xECP traversal, byte-wide legacy
  ownership and SMI cleanup/rollback; and
- waits boundedly for `USBSTS.CNR` before operational writes and after HCRST;
  and
- retains PCI binding, IRQ, DMA, mapping, and the BAR claim in quarantine if
  xHCI cannot reach `HCHalted`, while USB core blocks new work and permits a
  later checked teardown retry; an all-ones `USBSTS` read is rejected as
  unreachable MMIO rather than accepted as `HCHalted`.

BR-T25, BR-T26, and the existing WS004 xHCI model, PCI rescan, PCIe capability,
and PCI MSI host fixtures pass. `make -j16` and `git diff --check` pass. The
same production image passes:

| Gate | Result |
| --- | --- |
| OVMF q35/xHCI USB root, 4 GiB | `reject=00000000:ok`, xHCI attached, `sda`, `login:` |
| OVMF q35/xHCI USB root, 8 GiB | `reject=00000000:ok`, xHCI attached, `sda`, `login:` |
| OVMF q35/xHCI USB root, 16 GiB | `reject=00000000:ok`, xHCI attached, `sda`, `login:` |
| Legacy BIOS q35/xHCI USB root | `MEM on, MASTER off` before MMIO, xHCI attached, `sda`, `login:` |

Recorded q012 artifact:

- file: [build/amd64/hdd-image.img](../../../build/amd64/hdd-image.img)
- size: 135266304 bytes
- SHA-256:
  `4b346ec9d303c557c4b810f2a5b3ea430964c7e6e9a98fc7a572410f2ba667f4`

BR-T33 requires exactly one cold boot of this raw image at this intermediate
boundary. Its purpose is to determine whether H1 was the physical cause and,
if not, capture the first reason-coded stage needed to select H2/H3. Pass is
the boot-media controller reaching `xhci: PCI controller` followed by USB
mass-storage enumeration, with neither `attach failed at capabilities` nor
`boot-storage wait expired` nor `physical disks=0`. A legacy `devices=0` line
alone is not a failure because the successful USB-only QEMU path prints it
before `physical disks=1`. Both discovered functions must be visible in the
bounded records; a non-boot-media function may remain unsupported only with a
specific BDF and reason. The 1024-MiB memory report is out of scope for this
run.

## 2026-08-26 BR-T33 physical result

The requested single Latitude cold boot was performed with the recorded q012
artifact. The photograph proves both physical functions pass the boundary
which originally failed:

```text
xhci: pci 0000:00:0d.0 caps len=80 version=0120 hcs1=05000840
      hcs2=14200054 hcc1=20007fc1 dboff=00003000 rtsoff=00002000
      reject=00000000:ok
usb0: xHCI, 5 root ports
xhci: PCI controller, version=120 ports=5 slots=64 irq=0 MSI
xhci: pci 0000:00:14.0 id=8086:a0ed sub=1028:0a1f rev=20 parent=root
pci: BAR0 assigned to f0810000 (64 KiB)
xhci: pci 0000:00:14.0 command=0406->0402 (MEM on, MASTER off)
xhci: pci 0000:00:14.0 caps len=80 version=0120 hcs1=10000840
      hcs2=14200054 hcc1=20007fc1 dboff=00003000 rtsoff=00002000
      reject=00000000:ok
usb1: xHCI, 16 root ports
xhci: PCI controller, version=120 ports=16 slots=64 irq=0 MSI
```

The relocated `00:14.0` BAR is readable and returns a valid 64-KiB xHCI 1.2
register layout. Thus the physical evidence closes the old Memory Space,
capability-revision, and BAR-reachability questions without a generic PCI
rebalance or arbitrary high-MMIO branch.

The first subsequent failure is earlier than USB mass-storage probing:

```text
xhci: transfer completion=6 residual=8 length=8 slot=1 endpoint=1 direction=in
usb1: port 6 enumeration failed (44)
xhci: transfer completion=4 residual=8 length=8 slot=2 endpoint=1 direction=in
usb1: port 8 enumeration failed (5)
usb1: port 10 enumeration failed (42)
xhci: command 15 failed, completion=19
xhci: endpoint 1 stop failed during cancel (5); retaining DMA buffer
usb1: port 13 enumeration failed (5)
boot: waiting up to 5 seconds for boot storage
```

Completion code 6 is a stalled EP0 transfer and becomes `EPIPE (44)`; code 4
is a USB Transaction Error and becomes `EIO (5)`. Error 42 is `ETIMEDOUT`.
The Stop Endpoint completion 19 is a Context State Error reached while
cancelling a timed-out request, so it is secondary to the first EP0 failures.
The `retaining DMA buffer` text does not prove safe ownership: the current
cancel path drops the active/HCD references before Stop, and later device
teardown can free endpoint resources after an unchecked Disable Slot failure.
This is a P1 live-DMA safety handoff for p004. No `usb-storage: sda` marker is
present, and this q012 artifact must not be used for further physical testing.

Result: BR-T33 **FAIL**, q012 `uncleared`, and `ws003-p003` **Partial**. The
controller capability/MMIO portion is physically accepted, but U2 remains
unproven. Control-transfer formatting, port reset completion, EP0 context, and
cancellation recovery are handed to
[ws003-p004](../phase004-latitude-xhci-device-enumeration/phase.md).

## Disposition

- `ws003-p002`: former high-RSDP defect cleared; BR-T32 is complete at 3/3.
- `ws003-p003`: Partial; both physical xHCI functions attach, but BR-T33 failed
  during EP0 enumeration before USB mass storage.
- Highest physically proven tier: U1. `ws003-p004` owns the next U2 boundary.
