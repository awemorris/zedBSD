# Unified PC BIOS/UEFI loader

This directory contains a single-disk BIOS boot path shared by IBM PC/AT
compatibles and NEC PC-98.  LBA 0 detects the firmware family before using a
machine-specific BIOS interrupt.  It then dispatches to a one-sector Stage 1.5
and a separate ZBL2 slot for that machine.

The active 128-MiB FAT16 partition starts at LBA 2048. PC-98 loads
`VMUNIX.98`; PC/AT selects `VMUNIX.AT` or `VMUNIX.X64` after checking CPUID
long-mode and NX support. LBA 1 contains a PC-98 H=8/S=17 CHS mirror of this
partition only.

The remaining space is a non-active MBR type `0xEF` FAT32 ESP. Its fallback
path `EFI/BOOT/BOOTX64.EFI` opens the ESP copy of `VMUNIX.X64`, exits boot
services, and enters the same amd64 kernel used by BIOS. The kernel then
mounts FAT16 MBR partition 1 with the native PIIX IDE driver; the ESP is not
the root filesystem. GPT is deliberately not used because its primary header
and entry array conflict with the PC-98 table and loader slots at LBA 1 onward.

Build and test with:

```sh
./build.sh hdd-image unified
./build.sh unified-loader-host-check unified
./build.sh unified-loader-qemu-test unified
./build.sh uefi-loader-host-check unified
./build.sh uefi-entry-qemu-test unified
```

The first command is the normal user-facing image build and writes
`build/unified/hdd-image.img`. The `unified-*` targets remain available for
low-level loader development. The UEFI test
uses non-Secure-Boot OVMF, QEMU `-M pc`, and one PIIX IDE disk at 64, 128, and
256 MiB. AHCI, NVMe, Secure Boot, and GPT are outside the initial target.

The individual loaders in `bootloader/pcat` and `bootloader/pc98` remain
separate supported targets.
