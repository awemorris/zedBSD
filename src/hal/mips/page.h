/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The mips physical memory: where RAM is, for the rest of the HAL.
 */

#ifndef KERN_HAL_MIPS_PAGE_H
#define KERN_HAL_MIPS_PAGE_H

#include <hal/types.h>

void
mips_page_init(
	void);

bool
mips_range_is_ram(
	uint64_t physical,
	uint64_t size);

bool
mips_range_touches_ram(
	uint64_t physical,
	uint64_t size);

#endif
