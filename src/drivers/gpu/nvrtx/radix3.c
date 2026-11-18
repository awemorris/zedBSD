/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The three-level table that describes the GSP firmware image to the booter.
 *
 * The GSP reads system memory one 4 KiB page at a time, so a large image
 * need not be contiguous: the booter follows a table.  The top level is one
 * page whose first entry is the bus address of the middle level; the middle
 * level is one page with the bus address of each bottom-level page; the
 * bottom level holds the bus address of each page of the image.  Entries are
 * 64-bit little-endian words, so one table page names 512 pages and the
 * image may be up to 1 GiB.
 */

#include <stddef.h>
#include <stdint.h>

#include <uapi/errno.h>

#include "drivers/gpu/nvrtx/nvrtx-gsp.h"

/* How many 64-bit entries one table page holds. */
#define RADIX3_ENTRIES_PER_PAGE		(NVRTX_GSP_PAGE_SIZE / 8U)

/*
 * The table pages in order: the top level, the middle level, then the bottom
 * level.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/gsp.c (nvkm_gsp_radix3_sg).
 */
#define RADIX3_TOP_PAGE			0U
#define RADIX3_MIDDLE_PAGE		1U
#define RADIX3_BOTTOM_PAGE		2U

/*
 * Reports how many table pages an image of the given number of pages needs.
 */
size_t
nvrtx_radix3_pages(
	size_t image_pages)
{
	size_t bottom;

	/* One bottom-level page names 512 image pages. */
	bottom = (image_pages + RADIX3_ENTRIES_PER_PAGE - 1U) / RADIX3_ENTRIES_PER_PAGE;

	/* Succeeded: the top and middle pages and the bottom pages. */
	return 2U + bottom;
}

/*
 * Fills the table pages for an image.
 *
 * Returns EINVAL when the table has the wrong number of pages or the image
 * is larger than one middle-level page can describe.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/gsp.c (nvkm_gsp_radix3_sg).
 */
int
nvrtx_radix3_fill(
	const struct nvrtx_pages *image,
	const struct nvrtx_pages *table)
{
	size_t needed;
	size_t bottom_pages;
	size_t page;
	size_t index;
	uint8_t *entries;

	/* The table must have exactly the pages the image needs. */
	needed = nvrtx_radix3_pages(image->page_count);
	if (table->page_count != needed)
		return EINVAL;

	/* One middle-level page names at most 512 bottom-level pages. */
	bottom_pages = table->page_count - 2U;
	if (bottom_pages > RADIX3_ENTRIES_PER_PAGE)
		return EINVAL;

	/* Clears the top and middle pages, so their unused entries are zero. */
	for (page = 0U; page < 2U; page++) {
		entries = nvrtx_pages_cpu(table, page);
		for (index = 0U; index < NVRTX_GSP_PAGE_SIZE; index++)
			entries[index] = 0U;
	}

	/* The top level's first entry is the middle level. */
	entries = nvrtx_pages_cpu(table, RADIX3_TOP_PAGE);
	nvrtx_put64(entries, 0U, nvrtx_pages_bus(table, RADIX3_MIDDLE_PAGE));

	/* The middle level names each bottom-level page. */
	entries = nvrtx_pages_cpu(table, RADIX3_MIDDLE_PAGE);
	for (index = 0U; index < bottom_pages; index++)
		nvrtx_put64(entries, index * 8U, nvrtx_pages_bus(table, RADIX3_BOTTOM_PAGE + index));

	/* The bottom level names each image page; the tail of its last page is zero. */
	for (page = 0U; page < bottom_pages; page++) {
		entries = nvrtx_pages_cpu(table, RADIX3_BOTTOM_PAGE + page);
		for (index = 0U; index < RADIX3_ENTRIES_PER_PAGE; index++) {
			/* An entry past the image's last page stays zero. */
			if (page * RADIX3_ENTRIES_PER_PAGE + index < image->page_count) {
				nvrtx_put64(entries,
				    index * 8U,
				    nvrtx_pages_bus(image, page * RADIX3_ENTRIES_PER_PAGE + index));
			} else {
				nvrtx_put64(entries, index * 8U, 0U);
			}
		}
	}

	/* Succeeded: the booter can walk from the top page to every image page. */
	return 0;
}
