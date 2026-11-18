# Kernel platform ownership

This directory contains exactly one C translation unit for each supported
platform: `pcat.c`, `pc98.c`, `rpi4.c`, `riscv-virt.c`, `armv7-virt.c`,
`zybo.c`, `mac99.c`, `hal-console.c`, `sun4u.c`, and `x68k.c`.  The PC/AT
translation unit serves both i386 PC/AT and amd64, `riscv-virt.c` the
64-bit RISC-V virt board (a serial console and the initial ramdisk),
`armv7-virt.c` the ARMv7 virt board and `zybo.c` the Digilent ZYBO and
the emulator's xilinx-zynq-a9 machine (each a serial console and the
initial ramdisk), `mac99.c` the PowerPC Mac (the HAL's console, the
PCI host bridge and the initial ramdisk), and
`hal-console.c` the boards that publish no boot device and whose console is
the HAL's (the 32-bit RISC-V and m68k virt boards and pseries).

Each file independently defines `kern_platform_init()` and the complete
`kern_platform_*` hook set declared by `<kern/platform.h>`.  Common platform
implementation, helper C files, and helper headers do not belong here.

Reusable device, bus, graphics, input, filesystem, and disk-label
implementations belong to their owners below `src/drivers/`.  Platform files
may select and initialize those drivers, but must not absorb their reusable
implementation.  This README is the only non-C exception in this directory.
