/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The kernel image placement contract shared by the riscv64 UEFI loader
 * and the riscv64 HAL.
 */

#ifndef KERN_BOOTLOADER_RISCV64_KERNEL_IMAGE_H
#define KERN_BOOTLOADER_RISCV64_KERNEL_IMAGE_H

/*
 * The kernel is linked at one fixed virtual address (platform/riscv64/
 * vmunix.ld) for the physical address RISCV64_KERNEL_LINK_PHYS_START, the
 * first megapage after the firmware's on the emulator's board.  Every
 * section keeps the same distance from the virtual base as from the
 * physical start:
 *
 *     virtual = RISCV64_KERNEL_LINK_VIRT_BASE
 *               + (linked physical - RISCV64_KERNEL_LINK_PHYS_START)
 *
 * The linked physical address is only the preferred placement.  A loader
 * may put the image at any other physical address that satisfies
 * RISCV64_KERNEL_PHYS_ALIGN and RISCV64_KERNEL_PHYS_LIMIT and enters it at
 * the physical address of its entry, with translation off; the image finds
 * where it was put from its own first instruction and maps its linked
 * range there.
 */
#define RISCV64_KERNEL_LINK_VIRT_BASE	0xffffffff80000000ULL
#define RISCV64_KERNEL_LINK_PHYS_START	0x80200000ULL

/*
 * Largest image any placement supports: the HAL's image window maps the
 * first 16 MiB of the image in megapages.  platform/riscv64/vmunix.ld
 * asserts the same size.
 */
#define RISCV64_KERNEL_MAX_BYTES	0x01000000ULL

/* The image starts on a megapage, which the HAL's image window maps. */
#define RISCV64_KERNEL_PHYS_ALIGN	0x00200000ULL

/*
 * The image lies inside the low 256 GiB of physical space, which the HAL's
 * boot table maps as it is while the image moves to its linked address.
 */
#define RISCV64_KERNEL_PHYS_LIMIT	0x0000004000000000ULL

#endif
