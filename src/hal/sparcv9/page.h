/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The SPARC V9 HAL's physical pages and the direct map.
 */

#ifndef KERN_HAL_SPARCV9_PAGE_H
#define KERN_HAL_SPARCV9_PAGE_H

#include <hal/types.h>

/*
 * Builds the page bitmap from the firmware's memory ranges.
 */
void
sparcv9_page_init(
	void);

/*
 * Reports the physical address a direct-map address stands for.
 */
uintptr_t
sparcv9_direct_to_phys(
	const void *address);

/*
 * Reports the direct-map address of a physical address.
 */
void *
sparcv9_phys_to_direct(
	uintptr_t address);

#endif
