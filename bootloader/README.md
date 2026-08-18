# zedBSD BIOS bootloaders

The native BIOS loaders use the same on-disk layout on PC/AT and PC-98:

- LBA 0: machine-specific Stage 1 and a PC/AT MBR partition table
- LBA 1..N: machine-specific ZBL2 Stage 2
- LBA N+1..2047: reserved and zero filled
- LBA 2048: the first active primary FAT16 partition

Stage 2 loads the root-directory file `VMUNIX` through its FAT16 cluster
chain.  The PC/AT loader accepts ELF32/i386 and ELF64/x86-64.  The PC-98
loader accepts ELF32/i386 only.

Build and test the loaders with the repository build driver:

```text
./build.sh bios-bootloader pcat
./build.sh bios-loader-qemu-test pcat
./build.sh bios-bootloader pc98
./build.sh bios-loader-qemu-test pc98
./build.sh hdd-image unified
./build.sh unified-loader-qemu-test unified
./build.sh uefi-loader-host-check unified
./build.sh uefi-entry-qemu-test unified
```

`hdd-image` now selects the native MBR/FAT16 loader on both platforms.  The
PC-98 QEMU test covers normal H=8 geometry, a sub-20-MiB H=4 image, and a
fragmented `VMUNIX` cluster chain.  The PC/AT test covers ELF32 and ELF64,
including fragmented variants.

The unified image additionally contains an MBR type `0xEF` FAT32 ESP after a
fixed 128-MiB FAT16 root partition. PC/AT BIOS chooses the i386 or amd64 kernel
from CPUID capabilities, while x64 UEFI loads the amd64 kernel through
`EFI/BOOT/BOOTX64.EFI`. PC-98 continues to see only the FAT16 mirror in its
LBA 1 partition table.
