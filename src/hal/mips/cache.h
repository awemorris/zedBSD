/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The MIPS caches: their shape, read from Config1 and Config2, and the
 * operations of the rest of the HAL on them.
 */

#ifndef KERN_HAL_MIPS_CACHE_H
#define KERN_HAL_MIPS_CACHE_H

#include <hal/types.h>

void
mips_cache_init(
	void);

void
mips_cache_flush_all_early(
	void);

bool
mips_cache_aliases(
	void);

bool
mips_cache_virtual_tags(
	void);

void
mips_cache_invalidate_instructions(
	void);

void
mips_cache_flush_page_colors(
	hal_physaddr_t page,
	bool instructions);

uint32_t
mips_cache_color_flushes(
	void);

#endif
