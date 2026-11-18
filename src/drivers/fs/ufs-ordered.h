/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The batched journal's ordered runs: the new blocks of a transaction
 * whose data has to be on the disk, or zeroes in its place, before the
 * commit that makes the blocks reachable is sealed, so that a crash never
 * shows a file the old contents of a block another file freed.
 *
 * Only the bookkeeping is here, without locks or I/O: the journal keeps
 * the runs under its lock and does the writes.  A run is either a page
 * run, a run of whole blocks whose bytes the page cache holds and writes
 * back itself, or a cache run, a range whose new contents went to the
 * buffer cache.  A page run keeps a bit per page for the page cache's
 * write in flight (writing) and one for a page whose bytes have left the
 * page cache or whose block needs nothing more (settled); a cache run has
 * only its range, which the buffer cache writes before the seal.
 */

#ifndef DRIVERS_FS_UFS_ORDERED_H
#define DRIVERS_FS_UFS_ORDERED_H

#include <stdint.h>

/* The most bytes one page run covers: one allocation run of the file system. */
#define UFS_ORDERED_BYTES	(1024U * 1024U)

/* The most pages one page run covers. */
#define UFS_ORDERED_PAGES	(UFS_ORDERED_BYTES / KERN_PAGE_SIZE)

/* The 64-bit words of one page run's bit map. */
#define UFS_ORDERED_WORDS	((UFS_ORDERED_PAGES + 63U) / 64U)

/* The 512-byte sectors of one page. */
#define UFS_ORDERED_PAGE_SECTORS	(KERN_PAGE_SIZE / 512U)

/*
 * One ordered run of a transaction.
 *
 * It lives in the transaction's table from the allocation that made its
 * blocks until the commit of that transaction is sealed.  ino is zero for
 * a cache run.
 */
struct ufs_ordered {
	/* The first 512-byte sector of the run and how many it covers. */
	uint64_t sector;
	uint64_t sectors;
	/* The file of a page run, its generation, and the offset of its first page. */
	uint64_t ino;
	uint32_t generation;
	uint64_t offset;
	/* The pages of a page run. */
	unsigned pages;
	/* Set while zeroes are being written over some of the run's pages. */
	unsigned zeroing;
	/* The pages the page cache is writing, and the pages that need nothing more. */
	uint64_t writing[UFS_ORDERED_WORDS];
	uint64_t settled[UFS_ORDERED_WORDS];
};

/* Tests one page's bit. */
static __inline int
ufs_ordered_bit(
	const uint64_t *map,
	unsigned page)
{
	/* The page's word, then its bit in the word. */
	return (map[page / 64U] >> (page % 64U)) & 1U;
}

/* Sets one page's bit. */
static __inline void
ufs_ordered_bit_set(
	uint64_t *map,
	unsigned page)
{
	map[page / 64U] |= (uint64_t)1U << (page % 64U);
}

/* Clears one page's bit. */
static __inline void
ufs_ordered_bit_clear(
	uint64_t *map,
	unsigned page)
{
	map[page / 64U] &= ~((uint64_t)1U << (page % 64U));
}

/*
 * Finds the pages of a page run that a range of sectors touches: from
 * *first up to, not including, *end.  0 when the range misses the run or
 * the run is a cache run.
 */
static __inline int
ufs_ordered_pages_touched(
	const struct ufs_ordered *run,
	uint64_t sector,
	uint64_t count,
	unsigned *first,
	unsigned *end)
{
	uint64_t from;
	uint64_t to;

	/* A cache run has no pages, and an empty range touches nothing. */
	if (run->ino == 0 || count == 0)
		return 0;

	/* A range wholly before or wholly after the run misses it. */
	if (sector + count <= run->sector)
		return 0;
	if (sector >= run->sector + run->sectors)
		return 0;

	/* Clips the range to the run. */
	from = sector;
	if (from < run->sector)
		from = run->sector;
	to = sector + count;
	if (to > run->sector + run->sectors)
		to = run->sector + run->sectors;

	/* Every page any sector of the clipped range lies in. */
	*first = (unsigned)((from - run->sector) / UFS_ORDERED_PAGE_SECTORS);
	*end = (unsigned)((to - run->sector + UFS_ORDERED_PAGE_SECTORS - 1U) / UFS_ORDERED_PAGE_SECTORS);
	if (*end > run->pages)
		*end = run->pages;

	/* Succeeded: the range touches the pages found. */
	return 1;
}

/* Asks whether zeroes are being written over a run a range of sectors touches. */
static __inline int
ufs_ordered_zeroing(
	const struct ufs_ordered *runs,
	unsigned count,
	uint64_t sector,
	uint64_t sectors)
{
	unsigned first;
	unsigned end;
	unsigned n;

	/* Looks at every page run the range touches. */
	for (n = 0; n < count; n++) {
		if (!runs[n].zeroing)
			continue;
		if (ufs_ordered_pages_touched(&runs[n], sector, sectors, &first, &end))
			return 1;
	}

	/* No run the range touches is being zeroed. */
	return 0;
}

/*
 * Settles the pages a range of sectors touches: their bytes went another
 * way (the buffer cache, the journal) or their blocks were freed.
 */
static __inline void
ufs_ordered_settle(
	struct ufs_ordered *runs,
	unsigned count,
	uint64_t sector,
	uint64_t sectors)
{
	unsigned first;
	unsigned end;
	unsigned page;
	unsigned n;

	/* Marks every touched page of every page run. */
	for (n = 0; n < count; n++) {
		if (!ufs_ordered_pages_touched(&runs[n], sector, sectors, &first, &end))
			continue;
		for (page = first; page < end; page++)
			ufs_ordered_bit_set(runs[n].settled, page);
	}
}

/*
 * Marks the pages a range of sectors touches that are not settled as
 * written by the page cache; reports how many pages it marked.
 */
static __inline unsigned
ufs_ordered_write_begin(
	struct ufs_ordered *runs,
	unsigned count,
	uint64_t sector,
	uint64_t sectors)
{
	unsigned marked;
	unsigned first;
	unsigned end;
	unsigned page;
	unsigned n;

	/* Marks every touched page that still needs its bytes. */
	marked = 0;
	for (n = 0; n < count; n++) {
		if (!ufs_ordered_pages_touched(&runs[n], sector, sectors, &first, &end))
			continue;
		for (page = first; page < end; page++) {
			if (ufs_ordered_bit(runs[n].settled, page))
				continue;
			ufs_ordered_bit_set(runs[n].writing, page);
			marked++;
		}
	}

	/* Reports the pages now being written. */
	return marked;
}

/*
 * Ends the page cache's write of a range of sectors: the pages it marked
 * are settled when the write reached the disk, and need their bytes still
 * when it did not.
 */
static __inline void
ufs_ordered_write_end(
	struct ufs_ordered *runs,
	unsigned count,
	uint64_t sector,
	uint64_t sectors,
	int written)
{
	unsigned first;
	unsigned end;
	unsigned page;
	unsigned n;

	/* Ends every touched page's write. */
	for (n = 0; n < count; n++) {
		if (!ufs_ordered_pages_touched(&runs[n], sector, sectors, &first, &end))
			continue;
		for (page = first; page < end; page++) {
			if (!ufs_ordered_bit(runs[n].writing, page))
				continue;
			ufs_ordered_bit_clear(runs[n].writing, page);
			if (written)
				ufs_ordered_bit_set(runs[n].settled, page);
		}
	}
}

/* Asks whether the page cache is writing any page of a run. */
static __inline int
ufs_ordered_writing(
	const struct ufs_ordered *run)
{
	unsigned word;

	/* Any bit of the map. */
	for (word = 0; word < UFS_ORDERED_WORDS; word++) {
		if (run->writing[word] != 0)
			return 1;
	}

	/* No page is being written. */
	return 0;
}

/*
 * Finds the next pages of a page run, from page from on, that still need
 * their bytes: *first and *pages describe them.  0 when there are none.
 */
static __inline int
ufs_ordered_unsettled(
	const struct ufs_ordered *run,
	unsigned from,
	unsigned *first,
	unsigned *pages)
{
	unsigned page;

	/* A cache run has no pages to wait for. */
	if (run->ino == 0)
		return 0;

	/* Skips the settled pages. */
	page = from;
	while (page < run->pages && ufs_ordered_bit(run->settled, page))
		page++;
	if (page >= run->pages)
		return 0;

	/* Takes the unsettled pages that follow. */
	*first = page;
	while (page < run->pages && !ufs_ordered_bit(run->settled, page))
		page++;
	*pages = page - *first;

	/* Succeeded: the pages are found. */
	return 1;
}

/*
 * Adds a cache run of sectors to a table, into the last run when the range
 * continues it; 0 when it was taken, -1 when the table is full.
 */
static __inline int
ufs_ordered_add_cached(
	struct ufs_ordered *runs,
	unsigned *count,
	unsigned max,
	uint64_t sector,
	uint64_t sectors)
{
	struct ufs_ordered *last;
	struct ufs_ordered *run;

	/* Lengthens the last run when the range starts where it ends. */
	if (*count != 0) {
		last = &runs[*count - 1U];
		if (last->ino == 0 && last->sector + last->sectors == sector) {
			last->sectors += sectors;
			return 0;
		}
	}

	/* A full table takes no new run. */
	if (*count >= max)
		return -1;

	/* Describes the new cache run. */
	run = &runs[*count];
	kern_memset(run, 0, sizeof(*run));
	run->sector = sector;
	run->sectors = sectors;
	(*count)++;

	/* Succeeded: the run is in the table. */
	return 0;
}

/*
 * Adds a page run to a table, with no page settled; 0 when it was taken,
 * -1 when the table is full or the run is larger than a page run holds.
 */
static __inline int
ufs_ordered_add_pages(
	struct ufs_ordered *runs,
	unsigned *count,
	unsigned max,
	uint64_t ino,
	uint32_t generation,
	uint64_t offset,
	uint64_t sector,
	unsigned pages)
{
	struct ufs_ordered *run;

	/* Refuses a run the bit maps cannot hold, and a full table. */
	if (ino == 0 || pages == 0 || pages > UFS_ORDERED_PAGES)
		return -1;
	if (*count >= max)
		return -1;

	/* Describes the new page run. */
	run = &runs[*count];
	kern_memset(run, 0, sizeof(*run));
	run->sector = sector;
	run->sectors = (uint64_t)pages * UFS_ORDERED_PAGE_SECTORS;
	run->ino = ino;
	run->generation = generation;
	run->offset = offset;
	run->pages = pages;
	(*count)++;

	/* Succeeded: the run is in the table. */
	return 0;
}

#endif
