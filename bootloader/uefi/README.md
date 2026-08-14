# zedBSD x64 UEFI loader

`BOOTX64.EFI` is a freestanding fallback-path application. It opens
`\\VMUNIX.X64` on its own ESP through UEFI Simple File System, validates the
restricted zedBSD ELF64 layout, loads it at physical 2 MiB, captures a ZBL6 v2
memory map, exits boot services, installs private four-level bootstrap page
tables, and enters the kernel using the System V AMD64 ABI.

The first implementation deliberately targets non-Secure-Boot OVMF on QEMU
`-M pc` with one PIIX IDE disk. Firmware disk services are not used after
`ExitBootServices()`; the kernel mounts MBR partition 1 with its native IDE
driver.
