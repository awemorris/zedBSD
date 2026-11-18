/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The page index of one tmpfs file.
 *
 * A regular tmpfs file keeps its data in whole physical pages, found by
 * page number through a radix tree.  Every table of the tree is one
 * physical page of entries, and an entry is the physical address of the
 * table or data page below it with the present bit set, or zero for a hole.
 * Nothing comes from the kernel heap, so a large file neither fills the
 * fixed heap nor makes a release walk a list (BUG-052), and finding a page
 * costs one step for each level of the tree.
 *
 * The caller owns the data pages it inserts until they are in the tree;
 * from then on the tree frees them when it is truncated past them.
 */

#include "tmpfs-pages.h"
#include "kern/page.h"
#include "kern/pmem.h"
#include <kern/kcrt.h>

#include <uapi/errno.h>
#include <hal/hal.h>

/* The entries one table holds: one page of physical addresses. */
#define TMPFS_PAGES_FANOUT ((uint64_t)(KERN_PAGE_SIZE / sizeof(hal_physaddr_t)))

/*
 * The bit that marks an entry as present.  Tables and data pages are page
 * aligned, so the low bit of their address is free.
 */
#define TMPFS_PAGES_PRESENT ((hal_physaddr_t)1U)

/*
 * The tallest tree.  Six levels of a 512-entry fan-out already cover every
 * page a 64-bit file offset can name; the limit only bounds the walk.
 */
#define TMPFS_PAGES_HEIGHT_MAX 8U

static hal_physaddr_t * table_of(hal_physaddr_t entry);
static uint64_t span_of(unsigned level);
static int covers(unsigned height, uint64_t index);
static int allocate_table(hal_physaddr_t *entry);
static void free_page(hal_physaddr_t entry);
static size_t free_subtree(hal_physaddr_t entry, unsigned level);
static size_t truncate_table(hal_physaddr_t entry, unsigned level, uint64_t base, uint64_t first, int *emptied);

/*
 * Initializes an empty page index.
 */
void
tmpfs_pages_init(
	struct tmpfs_pages *pages)
{
	pages->root = 0;
	pages->height = 0;
	pages->count = 0;
}

/*
 * Finds the data page that holds a page number.
 *
 * Reports the page through its kernel address, or NULL for a hole.
 */
void *
tmpfs_pages_lookup(
	const struct tmpfs_pages *pages,
	uint64_t index)
{
	hal_physaddr_t entry;
	hal_physaddr_t *table;
	uint64_t span;
	uint64_t slot;
	unsigned level;
	int covered;
	void *data;

	/* An empty tree holds no page. */
	if (pages->height == 0)
		return NULL;

	/* A page number past what the tree covers is a hole. */
	covered = covers(pages->height, index);
	if (!covered)
		return NULL;

	/* Walks down one table a level, the page number's digit choosing the slot. */
	entry = pages->root;
	for (level = pages->height; level > 0; level--) {
		table = table_of(entry);
		span = span_of(level);
		slot = index / span;
		index = index % span;
		entry = table[slot];
		if (entry == 0)
			return NULL;
	}

	/* Succeeded: the last entry is the data page. */
	data = table_of(entry);
	return data;
}

/*
 * Inserts a data page at a page number.
 *
 * The tree grows taller and allocates the tables on the way as needed.  On
 * failure the data page stays the caller's, and any table already
 * allocated stays empty in the tree until it is truncated.
 */
int
tmpfs_pages_insert(
	struct tmpfs_pages *pages,
	uint64_t index,
	hal_physaddr_t data)
{
	hal_physaddr_t *link;
	hal_physaddr_t *table;
	hal_physaddr_t entry;
	uint64_t span;
	uint64_t slot;
	unsigned level;
	int covered;
	int error;

	/* Grows the tree a level at a time until it covers the page number. */
	for (;;) {
		/* Stops growing once the page number is inside the tree. */
		if (pages->height != 0) {
			covered = covers(pages->height, index);
			if (covered)
				break;
		}

		/* Refuses a page number beyond the tallest tree. */
		if (pages->height == TMPFS_PAGES_HEIGHT_MAX)
			return EFBIG;

		/* A new top table, with the old tree as its first child. */
		error = allocate_table(&entry);
		if (error != 0)
			return error;
		if (pages->height != 0) {
			table = table_of(entry);
			table[0] = pages->root;
		}

		/* The new table is the top, one level taller. */
		pages->root = entry;
		pages->height++;
	}

	/* Walks down to the lowest table, allocating the missing ones. */
	link = &pages->root;
	for (level = pages->height; level > 1; level--) {
		table = table_of(*link);
		span = span_of(level);
		slot = index / span;
		index = index % span;
		link = &table[slot];

		/* A missing table below is allocated empty. */
		if (*link == 0) {
			error = allocate_table(link);
			if (error != 0)
				return error;
		}
	}

	/* Refuses a page number that already has a page. */
	table = table_of(*link);
	if (table[index] != 0)
		return EEXIST;

	/* The page now belongs to the tree, which counts it. */
	table[index] = data | TMPFS_PAGES_PRESENT;
	pages->count++;

	/* Succeeded: the page is found by its number. */
	return 0;
}

/*
 * Frees every data page at or after a page number.
 *
 * Tables left empty are freed too, and a tree left with no page at all
 * becomes empty.  Reports the number of data pages freed, which the caller
 * gives back to its quota.
 */
size_t
tmpfs_pages_truncate(
	struct tmpfs_pages *pages,
	uint64_t first)
{
	size_t freed;
	int emptied;

	/* An empty tree has nothing to free. */
	if (pages->height == 0)
		return 0;

	/* Frees the pages from the top table down. */
	emptied = 0;
	freed = truncate_table(pages->root, pages->height, 0, first, &emptied);

	/* A tree with nothing left in its top table is empty again. */
	if (emptied) {
		free_page(pages->root);
		pages->root = 0;
		pages->height = 0;
	}

	/* The freed pages are no longer counted. */
	pages->count -= freed;

	/* Reports how many data pages were freed. */
	return freed;
}

/* Returns the kernel address of the page an entry names. */
static hal_physaddr_t *
table_of(
	hal_physaddr_t entry)
{
	hal_physaddr_t address;
	hal_physaddr_t *table;

	/* Strips the present bit and translates through the direct map. */
	address = entry & ~TMPFS_PAGES_PRESENT;
	table = kern_pmem_to_kernel(address);

	/* Reports the page's kernel address. */
	return table;
}

/* Returns how many page numbers one entry of a table at a level covers. */
static uint64_t
span_of(
	unsigned level)
{
	uint64_t span;
	unsigned step;

	/*
	 * An entry of the lowest table (level one) covers one page, and each
	 * level up multiplies by the fan-out.  The tree is only ever as tall
	 * as its largest page number needs, so this never overflows.
	 */
	span = 1U;
	for (step = 1U; step < level; step++)
		span = span * TMPFS_PAGES_FANOUT;

	/* Reports the span. */
	return span;
}

/* Reports whether a tree of a height covers a page number. */
static int
covers(
	unsigned height,
	uint64_t index)
{
	uint64_t remaining;
	unsigned level;

	/* Divides the page number by the fan-out once per level. */
	remaining = index;
	for (level = 0; level < height; level++) {
		remaining = remaining / TMPFS_PAGES_FANOUT;

		/* Nothing left means the levels so far already cover it. */
		if (remaining == 0)
			return 1;
	}

	/* The page number needs more levels than the tree has. */
	return 0;
}

/* Allocates one zeroed table page and stores its present entry. */
static int
allocate_table(
	hal_physaddr_t *entry)
{
	struct kern_pmem run;
	void *table;
	int error;

	/* Takes one physical page for the table. */
	error = kern_pmem_alloc(KERN_PAGE_SIZE, KERN_PAGE_SIZE, &run);
	if (error != 0)
		return ENOMEM;

	/* Every entry starts as a hole. */
	table = kern_pmem_to_kernel(run.paddr);
	kern_memset(table, 0, KERN_PAGE_SIZE);

	/* Succeeded: the entry names the new table. */
	*entry = run.paddr | TMPFS_PAGES_PRESENT;
	return 0;
}

/* Gives one table or data page back to the physical allocator. */
static void
free_page(
	hal_physaddr_t entry)
{
	struct kern_pmem run;
	int error;

	/* The run is the page the entry names. */
	run.paddr = entry & ~TMPFS_PAGES_PRESENT;
	run.size = KERN_PAGE_SIZE;

	/* A page this tree allocated always goes back. */
	error = kern_pmem_free(&run);
	if (error != 0)
		HAL_FATAL("tmpfs page free failed");
}

/* Frees a whole table and everything below it, reporting the data pages freed. */
static size_t
free_subtree(
	hal_physaddr_t entry,
	unsigned level)
{
	hal_physaddr_t *table;
	uint64_t slot;
	size_t freed;

	/* Finds the table and starts the count. */
	table = table_of(entry);
	freed = 0;

	/* Frees each present child: data pages below level one, tables above. */
	for (slot = 0; slot < TMPFS_PAGES_FANOUT; slot++) {
		/* A hole has nothing to free. */
		if (table[slot] == 0)
			continue;

		/* A data page is one page freed; a table frees its own subtree. */
		if (level == 1U) {
			free_page(table[slot]);
			freed++;
		} else {
			freed += free_subtree(table[slot], level - 1U);
		}

		/* The slot is a hole now. */
		table[slot] = 0;
	}

	/* The table itself goes last. */
	free_page(entry);

	/* Reports the data pages freed. */
	return freed;
}

/*
 * Frees the pages of one table at or after a page number.
 *
 * base is the first page number the table covers.  Reports the data pages
 * freed, and through emptied whether the table has no entry left, which
 * the caller then frees.
 */
static size_t
truncate_table(
	hal_physaddr_t entry,
	unsigned level,
	uint64_t base,
	uint64_t first,
	int *emptied)
{
	hal_physaddr_t *table;
	hal_physaddr_t child;
	uint64_t span;
	uint64_t slot;
	uint64_t child_base;
	size_t freed;
	size_t kept;
	int child_emptied;

	/* Finds the table, the span of one of its entries, and starts the counts. */
	table = table_of(entry);
	span = span_of(level);
	freed = 0;
	kept = 0;

	/*
	 * Visits each present child.  A present child's page numbers exist,
	 * so its first page number does not overflow.
	 */
	for (slot = 0; slot < TMPFS_PAGES_FANOUT; slot++) {
		child = table[slot];

		/* A hole has nothing to free or keep. */
		if (child == 0)
			continue;

		/* The first page number the child covers. */
		child_base = base + slot * span;

		/* A child wholly at or after the cut is freed. */
		if (child_base >= first) {
			if (level == 1U) {
				free_page(child);
				freed++;
			} else {
				freed += free_subtree(child, level - 1U);
			}

			/* The slot is a hole now. */
			table[slot] = 0;
			continue;
		}

		/* A child wholly before the cut is kept. */
		if (level == 1U || first - child_base >= span) {
			kept++;
			continue;
		}

		/* A table the cut passes through is truncated, and freed when emptied. */
		child_emptied = 0;
		freed += truncate_table(child, level - 1U, child_base, first, &child_emptied);
		if (child_emptied) {
			free_page(child);
			table[slot] = 0;
		} else {
			kept++;
		}
	}

	/* A table with no child left is the caller's to free. */
	*emptied = 0;
	if (kept == 0)
		*emptied = 1;

	/* Succeeded: reports the data pages freed. */
	return freed;
}
