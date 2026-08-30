# zedBSD BIOS bootloaders

The PC/AT and PC-98 BIOS loaders now share the same four-stage contract while
retaining machine-specific firmware I/O:

1. Stage 1 is the MBR/disk IPL.  It loads the reserved-area Stage 2.
2. Stage 2 selects the active FAT12/16/32 partition and chain-loads its PBR.
3. Stage 3 is the FAT PBR plus three reserved continuation sectors.  It loads
   the contiguous root-directory file `BOOTZBSD.EXE`.
4. Stage 4 is a DOS MZ system loader.  On PC/AT it reads `/zedbsd.cfg`; on
   PC-98 it reads `/BOOTZBSD.CFG`.  It loads the configured kernel, constructs
   the architecture handoff, changes CPU mode, and enters the kernel.

On non-GPT PC/AT images, Stage 2 begins at LBA 1; the amd64 hybrid GPT image
places it in the BIOS boot partition beginning at LBA 34.  On PC-98, LBA 1
remains the native NEC partition table and the 14-sector Stage 2 area occupies
LBA 2 through 15.  The first active FAT partition begins at LBA 2048 in
non-GPT repository images.  PC-98 Stage 2 may also be replaced by a compatible
NEC fixed-disk menu because Stage 3 observes the ordinary partition-IPL entry
contract.

`BOOTZBSD.EXE` has a conventional MZ header.  Stage 3 strips the 64-byte
header by arranging for the loader body to remain at physical `0x10000`.
When DOS loads the executable elsewhere, its entry stub records the current
DOS drive and relocates the body to the same address before taking over the
machine.  The PC/AT loader accepts ELF32/i386 and ELF64/x86-64 kernels; the
PC-98 loader accepts ELF32/i386.

Direct DOS invocation is a retained legacy entry stub, not a supported q032
production path.  PC/AT currently derives a BIOS disk number arithmetically
from the DOS drive letter, which is not a valid mapping on arbitrary DOS
systems; PC-98 likewise cannot yet map every DOS volume back to the matching
native partition.  Production images enter through MBR/partition PBR.  A
later compatibility Phase must either define an explicit DOS-volume mapping
or remove both direct-launch stubs rather than treating those guesses as a
boot contract.

Both configuration names use the same bounded line-oriented format as the
UEFI loader.  `kernel=` selects a safe relative FAT path (including LFN
subdirectories), while the remaining lines form the kernel-parameter record.
When `boot0=` is omitted, the loader supplies the source FAT UUID and
normalizes relative overlay/data/swap files to `boot0:`.  Missing or malformed
configuration, an invalid path or ELF, and bounded-I/O/FAT failures stop
visibly; there is no fixed-`VMUNIX` or embedded-parameter fallback.

The amd64 hybrid image keeps its EFI System Partition separate from the
zedBSD payload FAT.  Its active hybrid-MBR entry points to that payload, so
SeaBIOS reaches the same `zedbsd.cfg`, configured kernel, and filesystem
images that `BOOTX64.EFI` selects through UEFI.

The PC/AT Stage 3 and both Stage 4 loaders classify FAT12, FAT16, and FAT32
from the BPB and cluster count.  FAT32 root directories and file data use
their cluster chains; FAT12 entries that straddle physical sectors are decoded
from a two-sector cache.  The PC-98 production PBR currently remains FAT16 as
recorded in WS013 p006.  Production image FAT types are fixed by each platform
layout; the current Noct/C image interface does not expose an arbitrary
`--fat-type` selector and rejects that unsupported option.

Select the target with the repository menu, then use the public make
interface.  The selected platform Makefile owns the platform-specific loader
and image assembly steps:

```text
make menuconfig
make bootloader
make disk-image
```
