/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The page index of one tmpfs file.
 *
 * A file's data pages are physical pages, found by their page number
 * through a radix tree whose tables are pages too, so a file of any size
 * costs one lookup per level and no kernel heap at all.
 */

#ifndef KERN_TMPFS_PAGES_H
#define KERN_TMPFS_PAGES_H

#include <hal/types.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The data pages of one regular file, by page number.
 *
 * root is the top table's entry (zero when the file holds no page) and
 * height the number of table levels below it, so the tree covers the page
 * numbers below the fan-out raised to the height.  count is the number of
 * data pages the tree holds.  The owner serializes every operation.
 */
struct tmpfs_pages {
	hal_physaddr_t root;
	unsigned height;
	size_t count;
};

void
tmpfs_pages_init(
	struct tmpfs_pages *pages);

void *
tmpfs_pages_lookup(
	const struct tmpfs_pages *pages,
	uint64_t index);

int
tmpfs_pages_insert(
	struct tmpfs_pages *pages,
	uint64_t index,
	hal_physaddr_t data);

size_t
tmpfs_pages_truncate(
	struct tmpfs_pages *pages,
	uint64_t first);

#endif
