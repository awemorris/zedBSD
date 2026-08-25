# ws003-p001: Latitude 5320 hardware inventory

WSID: `ws003`  
Phase ID: `p001`  
Combined ID: `ws003-p001`  
Status: carried forward  
Last attempted: 2026-08-25  
Parent WS: [WS003](../ws.md)

## Objective

Record the exact Dell Latitude 5320 firmware mode and CPU, GPU, xHCI, NVMe,
WLAN, Ethernet/USB adapter identities before choosing physical drivers.

## Attempt result

The available execution host is WSL2, not the target laptop:

```text
Linux agent-1 6.18.33.2-microsoft-standard-WSL2 x86_64 GNU/Linux
```

Target DMI fields, `/sys/firmware/efi`, PCI inventory, and USB inventory are
unavailable. `lspci`, `lsusb`, `mokutil`, and `efibootmgr` are also absent.
Installing tools cannot expose hardware hidden by WSL2.

## Work packages

- [x] Identify whether the active host is the target laptop.
- [x] Check availability of DMI, firmware-mode, PCI, and USB evidence.
- [ ] Capture Latitude BIOS/UEFI and Secure Boot state.
- [ ] Capture exact CPU/GPU/xHCI/NVMe/WLAN/network device IDs.
- [ ] Record cold-boot and removable-media observations.

## Completion conditions

Not met. No hardware identity has been inferred without evidence.

## Carry-forward reason and resume point

Physical Latitude 5320 access or a user-supplied inventory captured there is
required. Resume with DMI/firmware information and PCI/USB numeric IDs. Driver
family choices remain provisional.
