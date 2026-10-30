zedBSD bootloaders
==================

Not every port boots through a zedBSD boot loader.  Where the machine's
firmware or the emulator can load the kernel itself, zedBSD uses that path
and has no loader of its own.  Where it cannot, the loaders in this
directory find the kernel, build a boot record for it and enter it.

In both cases the kernel never reads a firmware- or loader-specific
structure.  The board's HAL code (`src/hal/<arch>/bsp-<board>/` or
`src/hal/<arch>/`) checks what it was given, keeps a copy, and builds the
common kernel handoff and the named objects of `hal_get_arch_handoff()`
from it.

| Port | Firmware | zedBSD loader | Record the HAL reads |
| --- | --- | --- | --- |
| amd64 PC/AT, UEFI | UEFI (OVMF in QEMU) | `uefi/` (`BOOTX64.EFI`) | ZBL6, from the loader |
| amd64 PC/AT, BIOS | PC BIOS (SeaBIOS) | `pcat/` (four stages) | ZBL6, from the loader |
| i386 PC/AT | PC BIOS | `pcat/` (four stages) | Multiboot v1, from the loader |
| PC-98 (i386) | NEC BIOS | `pc98/` (four stages) | PC-98 handoff, from the loader |
| Raspberry Pi 4 (arm64) | Pi boot firmware | none | the device tree |
| riscv64 virt | OpenSBI | none | the device tree |
| riscv64 VisionFive 2 | (planned) | (planned) | (planned) |
| riscv32 virt | OpenSBI | none | the device tree |
| ppc32 mac99 | OpenBIOS | none | Open Firmware client calls |
| ppc64 pseries | SLOF | none | Open Firmware client calls |
| m68k virt | QEMU's kernel loader | none | QEMU boot information |
| SPARC V9 sun4u | OpenBoot (OpenBIOS in QEMU) | `sparcv9/` (two stages) | sun4u handoff, from the loader |
| X68000 (m68k) | SCSI IPL ROM and IOCS | `x68k/` (two stages) | X68000 handoff, from the loader |

`common/`, `include/` and `bios/` hold the parts the x86 loaders share: the
boot-key override, the memory map, the FAT directory search, the logo, and
the disk and handoff layouts.  The BIOS loaders also build the
configuration parser of `uefi/` (`zedbsd-config.c`).  `tests/` holds small
ELF test kernels that the BIOS loader tests boot instead of `vmunix`.

amd64 PC/AT
-----------

The amd64 image layout is chosen with `ZEDBSD_VARIANT`:

- `native` (the default): UEFI only.  A GPT disk with an EFI System
  Partition that carries only `/EFI/BOOT/BOOTX64.EFI`, a FAT32 partition
  labelled `ZEDBSD` with the kernel, `zedbsd.cfg` and the logo, a UFS root
  partition and a swap partition.
- `uefi`: UEFI only.  A GPT disk with the EFI System Partition and one
  payload FAT carrying the kernel, `zedbsd.cfg` and the root, data and swap
  files.  The Intel Mac configuration uses it.
- `hybrid`: a GPT disk with a hybrid MBR, bootable both through UEFI
  (`BOOTX64.EFI`) and through the BIOS (the four stages below).
- `bios`: an MBR disk with the BIOS stages only.

Under UEFI the firmware starts `BOOTX64.EFI` from the fallback path.  It
finds `zedbsd.cfg`, loads the ELF64 kernel, writes the ZBL6 handoff
(`include/amd64-handoff.h`), exits boot services and enters the kernel.
`uefi/README.md` describes it.  Under the BIOS, `BOOTZBSD.EXE` writes the
same ZBL6 handoff and enters long mode itself.  On both paths
`src/hal/amd64/bsp-pcat/boot.c` checks the ZBL6 record and builds the
kernel handoff.

i386 PC/AT and PC-98
--------------------

Both boot only from the BIOS, through the four stages below.  On PC/AT,
`BOOTZBSD.EXE` enters an ELF32 kernel in protected mode with Multiboot v1
information, which `src/hal/i386/bsp-pcat/boot.c` turns into the kernel
handoff.  On PC-98 the loader writes its own handoff, which
`src/hal/i386/bsp-pc98/boot.c` checks.

The i386 kernel also carries a Multiboot v1 header, so a Multiboot loader
can start it without the zedBSD stages: the PC/AT build has a
`zedbsd-grub.iso` target for that (it needs `grub-mkrescue`).  It is not
the image that `make disk-image` builds.

PC/AT and PC-98 BIOS loaders
----------------------------

The PC/AT and PC-98 BIOS loaders now share the same four-stage
contract while retaining machine-specific firmware I/O:

1. Stage 1 is the MBR/disk IPL.  It loads the reserved-area Stage 2.
2. Stage 2 selects the active FAT12/16/32 partition and chain-loads its PBR.
3. Stage 3 is the FAT PBR plus three reserved continuation sectors.  It loads
   the contiguous root-directory file `BOOTZBSD.EXE`.
4. Stage 4 is a DOS MZ system loader.  On PC/AT it reads `/zedbsd.cfg`; on
   PC-98 it reads `/BOOTZBSD.CFG`.  It loads the configured kernel, constructs
   the architecture handoff, changes CPU mode, and enters the kernel.

The BIOS loaders do not read the boot keys (Ctrl for the kernel messages on
the console, Shift for the console login) that the amd64 UEFI loader reads;
see `docs/reference/kernel-boot-parameters.md` Section 7c.

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

Direct DOS invocation is a retained legacy entry stub, not a supported
production path.  PC/AT currently derives a BIOS disk number arithmetically
from the DOS drive letter, which is not a valid mapping on arbitrary DOS
systems; PC-98 likewise cannot yet map every DOS volume back to the matching
native partition.  Production images enter through MBR/partition PBR.  A
later compatibility change must either define an explicit DOS-volume mapping
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
from a two-sector cache.  The PC-98 production PBR currently remains FAT16.
Production image FAT types are fixed by each platform
layout; the current Noct/C image interface does not expose an arbitrary
`--fat-type` selector and rejects that unsupported option.

Raspberry Pi 4
--------------

The Pi does not use a zedBSD boot loader.  The SD card's FAT32 boot
partition carries the Pi's own boot firmware (`start4.elf`, `fixup4.dat`),
the board's device tree, the `disable-bt` overlay, `config.txt`,
`cmdline.txt` and the kernel `vmunix`.  The firmware reads
`config.txt` (`platform/arm64/config.txt`), loads `vmunix` at `0x80000`
(`kernel=vmunix`, `kernel_address=0x80000`, `arm_64bit=1`) together with the
device tree, and enters the kernel with the device tree's address in `x0`.
`vmunix` is a raw binary with the arm64 Image header, not an ELF file.
The firmware puts `cmdline.txt` into the device tree's `/chosen/bootargs`;
it is the kernel's parameter line, and there is no `zedbsd.cfg`.
`src/hal/arm64/boot.c` reads the device tree and the command line
and builds the kernel handoff.

QEMU's `raspi4b` machine does not run the Pi's firmware, so `make run` and
the QEMU boot test give it the kernel and the device tree directly and
use the same image as the SD card.

RISC-V
------

On the `virt` machine (riscv64 and riscv32) there is no zedBSD loader.
QEMU loads the kernel ELF (`-kernel vmunix`) at its physical load address
(`0x80200000` on riscv64, `0x80400000` on riscv32), and its
default firmware, OpenSBI, enters it in supervisor mode with translation
off, the boot hart's id in `a0` and the device tree in `a1`.  The kernel
parameters come from the device tree's command line (`-append`), an
initial ramdisk from `-initrd`.  `src/hal/riscv64/bootinfo.c` and
`src/hal/riscv32/bootinfo.c` read the tree and build the kernel handoff.

The StarFive VisionFive 2 is not supported yet.  The planned path is the
board's SPL, OpenSBI and U-Boot, whose UEFI support then starts a riscv64
build of the UEFI loader in `uefi/`, which passes the kernel a ZBL6
handoff and the device tree from U-Boot.  That loader is not in this tree
yet.

PowerPC
-------

Neither PowerPC port has a zedBSD loader.  On `mac99` (ppc32) QEMU loads the
flat image `vmunix.bin` and OpenBIOS enters it at its load address; on
`pseries` (ppc64) QEMU loads the big-endian ELF `vmunix` and SLOF enters it.
In both, the firmware passes its Open Firmware client interface, and the
HAL (`src/hal/ppc32/bsp-mac99/firmware.c`,
`src/hal/ppc64/bsp-pseries/firmware.c`) asks it once, before taking the
machine, for the memory, the time base's rate, the display and the command
line, and builds the kernel handoff from the answers.

m68k virt
---------

There is no zedBSD loader.  QEMU's own kernel loader loads the ELF
`vmunix` and leaves its list of tagged boot-information records (memory,
ramdisk, command line, device addresses) after the image.
`src/hal/m68k/bsp-virt/bootinfo.c` reads them once and builds the kernel
handoff.

The PowerPC, RISC-V and m68k `virt` platform Makefiles have no loader or
disk-image step; `make run` builds the kernel and gives it to QEMU.

SPARC V9 sun4u
--------------

`sparcv9/` is a two-stage loader for OpenBoot (OpenBIOS in QEMU).  The disk
has a Sun disklabel.  The firmware loads Stage 1 from the boot block
(sectors 1 through 15) at virtual address `0x4000` and passes it the client
interface.  Stage 1 reads the Stage 2 header (sector 32) and payload,
checks them and enters Stage 2.  Stage 2 reads `VMUNIX.S9` from the FAT16
slice, loads the ELF64 kernel, builds the sun4u handoff (`sparcv9/handoff.h`)
from the firmware's device tree and enters the kernel.
`src/hal/sparcv9/bsp-sun4u/boot.c` checks the handoff and builds the kernel
handoff.  The UFS root is a slice of its own.

X68000
------

`x68k/` is a two-stage loader for a SCSI disk.  The IPL ROM loads the 1 KiB
Stage 1 from the disk at `0x2400`.  Stage 1 reads, through IOCS, the boot
manifest at sector 8 and the Stage 2 it names, and checks it.  Stage 2 reads
the ELF kernel the manifest names by programmed I/O on the SCSI controller,
checks it, builds the X68000 handoff (`x68k/handoff.h`), and enters the
kernel; IOCS is not called after that.  `src/hal/m68k/bsp-x68k/` checks the
handoff and builds the kernel handoff.  QEMU has no X68000 machine;
`make run` starts MAME instead.

Building
--------

Select the target with the repository menu, then use the public make
interface.  The selected platform Makefile owns the platform-specific loader
and image assembly steps:

```text
make menuconfig
make bootloader
make disk-image
```
