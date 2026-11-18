/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The kernel image placement contract shared by the loaders and the amd64
 * HAL.
 */

#ifndef KERN_BOOTLOADER_AMD64_KERNEL_IMAGE_H
#define KERN_BOOTLOADER_AMD64_KERNEL_IMAGE_H

/*
 * The kernel is linked at one fixed virtual address (platform/amd64/vmunix.ld)
 * inside the one-GiB bootstrap window that the loader and the HAL both map at
 * AMD64_KERNEL_LINK_VIRT_BASE.  The physical address it is linked for is only
 * the preferred placement: a loader may put the image at any other physical
 * address that satisfies AMD64_KERNEL_PHYS_ALIGN/LIMIT and must report the
 * actual placement through kernel_phys_start/kernel_phys_end of the handoff.
 * The HAL maps the linked virtual range onto the reported physical range, so
 *
 *     physical = virtual - AMD64_KERNEL_LINK_VIRT_BASE
 *                        - AMD64_KERNEL_LINK_PHYS_START + kernel_phys_start
 *
 * holds for every image address.  With the linked placement the expression
 * degenerates to the historical identity window.
 *
 * The bootstrap window doubles as the early physical-memory window (VGA text,
 * the handoff block, early page-table pages).  Behind a relocated image the
 * linked physical range has no window address any more: the HAL reserves it
 * ("shadow") so that nothing is ever handed out there before the direct map.
 */
#define AMD64_KERNEL_LINK_VIRT_BASE	0xffffffff80000000ULL	/* AMD64_IMAGE_BASE */
#define AMD64_KERNEL_LINK_PHYS_START	0x00200000ULL		/* vmunix.ld __kernel_phys_start */

/*
 * Largest image any placement supports: eight 2 MiB W^X leaf tables in
 * src/hal/amd64/space.c.  platform/amd64/vmunix.ld asserts the same literal.
 */
#define AMD64_KERNEL_MAX_BYTES		0x01000000ULL

/* The loader maps the placed image with 2 MiB pages in its bootstrap tables. */
#define AMD64_KERNEL_PHYS_ALIGN		0x00200000ULL

/* Every placement must stay inside the one-GiB bootstrap window. */
#define AMD64_KERNEL_PHYS_LIMIT		0x40000000ULL

#endif
