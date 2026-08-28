# Kernel platform ownership

This directory contains exactly one C translation unit for each supported
platform: `pcat.c`, `pc98.c`, `rpi4.c`, `sun4u.c`, and `x68k.c`.  The PC/AT
translation unit serves both i386 PC/AT and amd64.

Each file independently defines `kern_platform_init()` and the complete
`kern_platform_*` hook set declared by `<kern/platform.h>`.  Common platform
implementation, helper C files, and helper headers do not belong here.

Reusable device, bus, graphics, input, filesystem, and disk-label
implementations belong to their owners below `src/drivers/`.  Platform files
may select and initialize those drivers, but must not absorb their reusable
implementation.  This README is the only non-C exception in this directory.
