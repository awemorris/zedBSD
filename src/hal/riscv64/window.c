/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The page entries of one megapage of the device window.
 *
 * Only the pages a range covers get a leaf; the others of the megapage
 * stay invalid, so a range of RAM a driver owns does not make the
 * neighbouring pages (the firmware's, or reserved memory) reachable.
 */

#include "defs.h"
#include "window.h"

/*
 * Writes a page leaf for every page of a range that lies in one megapage.
 *
 * Block is the megapage's first physical address in the window; the
 * table's entry n then maps block plus n pages.  Pages of the range
 * outside the megapage are left to the caller's other megapages.  Reports
 * how many leaves were written.
 */
unsigned
riscv64_window_fill_pages(
	uint64_t *table,
	uint64_t block,
	uint64_t physical,
	uint64_t size,
	uint64_t flags)
{
	uint64_t first;
	uint64_t end;
	uint64_t page;
	unsigned index;
	unsigned written;

	/* An empty range or one that wraps writes nothing. */
	if (table == NULL || size == 0)
		return 0;
	if (physical > ~0ULL - size)
		return 0;

	/* Clips the range to the megapage, page by page. */
	first = physical & ~((uint64_t)RISCV64_PAGE_SIZE - 1U);
	end = physical + size;
	if (first < block)
		first = block;
	if (end > block + RISCV64_MEGAPAGE_SIZE)
		end = block + RISCV64_MEGAPAGE_SIZE;

	/* Writes a leaf for each page of the clipped range. */
	written = 0;
	for (page = first; page < end; page += RISCV64_PAGE_SIZE) {
		index = (unsigned)((page - block) >> RISCV64_PAGE_SHIFT);
		table[index] = (page >> RISCV64_PAGE_SHIFT << RISCV64_PTE_PPN_SHIFT) | flags;
		written++;
	}

	/* Succeeded: the pages of the range in the megapage are reachable. */
	return written;
}
