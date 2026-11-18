/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The page entries of one megapage of the device window.
 *
 * A megapage of the window is normally one leaf.  Where only some of its
 * pages may be reached, as for the uncached alias of RAM that a driver
 * owns, the megapage points at a table of page leaves instead.  The calls
 * here write those leaves into a table the caller holds, so they touch no
 * hardware and can be checked on a host.
 */

#ifndef KERN_HAL_RISCV64_WINDOW_H
#define KERN_HAL_RISCV64_WINDOW_H

#include <hal/types.h>

/* The page leaves of one megapage: one table. */
#define RISCV64_WINDOW_TABLE_ENTRIES	512U

unsigned
riscv64_window_fill_pages(
	uint64_t *table,
	uint64_t block,
	uint64_t physical,
	uint64_t size,
	uint64_t flags);

#endif
