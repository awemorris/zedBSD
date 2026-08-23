# zedBSD BIOS bootloaders

The PC/AT and PC-98 BIOS loaders now share the same four-stage contract while
retaining machine-specific firmware I/O:

1. Stage 1 is the MBR/disk IPL.  It loads the reserved-area Stage 2.
2. Stage 2 selects the active FAT12/16/32 partition and chain-loads its PBR.
3. Stage 3 is the FAT PBR plus three reserved continuation sectors.  It loads
   the contiguous root-directory file `BOOTZBSD.EXE`.
4. Stage 4 is a DOS MZ system loader.  It loads `VMUNIX`, constructs the
   architecture handoff, changes CPU mode, and enters the kernel.

On PC/AT, Stage 2 begins at LBA 1.  On PC-98, LBA 1 remains the native NEC
partition table and the 14-sector Stage 2 area occupies LBA 2 through 15.
The first active FAT partition begins at LBA 2048 in repository-generated
images.  PC-98 Stage 2 may also be replaced by a compatible NEC fixed-disk
menu because Stage 3 observes the ordinary partition-IPL entry contract.

`BOOTZBSD.EXE` has a conventional MZ header.  Stage 3 strips the 64-byte
header by arranging for the loader body to remain at physical `0x10000`.
When DOS loads the executable elsewhere, its entry stub records the current
DOS drive and relocates the body to the same address before taking over the
machine.  The PC/AT loader accepts ELF32/i386 and ELF64/x86-64 kernels; the
PC-98 loader accepts ELF32/i386.

Stage 3 and Stage 4 classify FAT12, FAT16, and FAT32 from the BPB and cluster
count.  FAT32 root directories and file data use their cluster chains; FAT12
entries that straddle physical sectors are decoded from a two-sector cache.
The image builder accepts `--fat-type fat12`, `fat16`, or `fat32`, and its
checker rejects an image whose computed FAT type differs from the request.

Select the target with the repository menu, then use the public make
interface.  The selected platform Makefile owns the platform-specific loader
and image assembly steps:

```text
make menuconfig
make bootloader
make disk-image
```
