/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The SPARC V9 HAL's address-space constants.
 */

#ifndef KERN_HAL_SPARCV9_DEFS_H
#define KERN_HAL_SPARCV9_DEFS_H

/* Marks a parameter the function does not use. */
#define UNUSED_PARAMETER(parameter) ((void)(parameter))

/* The base page: 8 KiB, the smallest page the UltraSPARC MMU maps. */
#define SPARCV9_PAGE_SIZE	8192UL
#define SPARCV9_PAGE_MASK	(SPARCV9_PAGE_SIZE - 1UL)

/*
 * The kernel's direct map of physical memory: physical address P is seen at
 * SPARCV9_DIRECT_BASE + P.  The fast miss handlers fill it in 4 MiB pages
 * up to SPARCV9_DIRECT_LIMIT.
 */
#define SPARCV9_DIRECT_BASE	0xfffff80000000000UL
#define SPARCV9_DIRECT_LIMIT	0x20000000UL

/* The kernel window of the mappings made by hal_space_map_device(). */
#define SPARCV9_DEVICE_VA	0xfffffa0000000000UL
#define SPARCV9_DEVICE_VA_END	0xfffffb0000000000UL

/* The end of the user half of an address space. */
#define SPARCV9_USER_LIMIT	0x0000080000000000UL

/* The V9 ABI's stack bias and the smallest frame a call needs. */
#define SPARCV9_STACK_BIAS	2047UL
#define SPARCV9_STACK_FRAME_SIZE	176UL

#endif
