# zedBSD BIOS bootloaders

The native BIOS loaders use the same on-disk layout on PC/AT and PC-98:

- LBA 0: machine-specific Stage 1 and a PC/AT MBR partition table
- LBA 1..N: machine-specific ZBL2 Stage 2
- LBA N+1..2047: reserved and zero filled
- LBA 2048: the first active primary FAT16 partition

Stage 2 loads the root-directory file `VMUNIX` through its FAT16 cluster
chain.  The PC/AT loader accepts ELF32/i386 and ELF64/x86-64.  The PC-98
loader accepts ELF32/i386 only.

The old native NEC98 image path remains under `bootsectors/pc98`.  Its LBA 0
does not carry the PC/AT `55 aa` signature; its partition table is at LBA 1.

Build and test the loaders with the repository build driver:

```text
./build.sh bios-bootloader pcat
./build.sh bios-loader-qemu-test pcat
./build.sh bios-bootloader pc98
./build.sh bios-loader-qemu-test pc98
./build.sh pc-unified-hdd-image pcat
./build.sh pc-unified-loader-qemu-test pcat
./build.sh legacy-pc98-hdd-image pc98
```

`hdd-image` now selects the native MBR/FAT16 loader on both platforms.  The
PC-98 QEMU test covers normal H=8 geometry, a sub-20-MiB H=4 image, and a
fragmented `VMUNIX` cluster chain.  The PC/AT test covers ELF32 and ELF64,
including fragmented variants.
