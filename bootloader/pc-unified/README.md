# Unified PC BIOS loader

This directory contains a single-disk BIOS boot path shared by IBM PC/AT
compatibles and NEC PC-98.  LBA 0 detects the firmware family before using a
machine-specific BIOS interrupt.  It then dispatches to a one-sector Stage 1.5
and a separate ZBL2 slot for that machine.

The first active FAT16 partition starts at LBA 2048.  PC/AT loads
`VMUNIX.AT`; PC-98 loads `VMUNIX.98` with fixed H=8/S=17 disk translation.
LBA 1 contains a PC-98 CHS mirror of the authoritative MBR partition.

Build and test with:

```sh
./build.sh hdd-image unified-pcat-pc98
./build.sh pc-unified-hdd-image pcat
./build.sh pc-unified-loader-host-check pcat
./build.sh pc-unified-loader-qemu-test pcat
```

The first command is the normal user-facing image build and writes
`build/unified-pcat-pc98/hdd-image.img`.  The `pc-unified-*` targets remain
available for low-level loader development and compatibility.

The individual loaders in `bootloader/pcat` and `bootloader/pc98` remain
separate supported targets.
