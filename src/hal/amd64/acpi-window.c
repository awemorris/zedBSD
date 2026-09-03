/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Sparse persistent-mapping slots for amd64 ACPI table discovery.
 */

#include "acpi-window.h"

/*
 * Initializes an empty ACPI mapping window.
 */
void
amd64_acpi_window_init(
	struct amd64_acpi_window *window)
{
	/* Clears the allocation cursor when a window was supplied. */
	if (window != NULL)
		window->used = 0;
}

/*
 * Finds or reserves the slots covering one physical ACPI table range.
 */
int
amd64_acpi_window_reserve(
	struct amd64_acpi_window *window,
	paddr_t physical,
	size_t size,
	unsigned *first_slot,
	size_t *page_offset,
	unsigned *new_first_slot,
	unsigned *new_slot_count)
{
	const paddr_t page_mask = AMD64_ACPI_WINDOW_PAGE_SIZE - 1U;
	paddr_t aligned;
	paddr_t end;
	size_t offset;
	size_t span;
	size_t page_count;
	unsigned pages;
	unsigned start;
	unsigned index;
	unsigned available;
	unsigned missing;

	/* Requires complete outputs and a nonempty representable range. */
	if (window == NULL || first_slot == NULL || page_offset == NULL ||
	    new_first_slot == NULL || new_slot_count == NULL || size == 0 ||
	    physical > UINTPTR_MAX - (size - 1U))
		return 0;

	/* Aligns the range and rejects overflow while including its offset. */
	aligned = physical & ~page_mask;
	offset = (size_t)(physical - aligned);
	if (size > SIZE_MAX - offset)
		return 0;
	span = size + offset;

	/* Rounds the covered span up to complete mapping pages. */
	page_count = span / AMD64_ACPI_WINDOW_PAGE_SIZE;
	if (span % AMD64_ACPI_WINDOW_PAGE_SIZE != 0)
		page_count++;
	if (page_count == 0 || page_count > AMD64_ACPI_WINDOW_SLOTS)
		return 0;
	pages = (unsigned)page_count;

	/* Rejects physical page-sequence arithmetic which wraps. */
	end = aligned + (paddr_t)(pages - 1U) *
	    AMD64_ACPI_WINDOW_PAGE_SIZE;
	if (end < aligned)
		return 0;

	/* Searches for an already mapped copy of the complete page sequence. */
	for (start = 0; start + pages <= window->used; start++) {
		/* Compares every requested physical page in order. */
		for (index = 0; index < pages; index++) {
			/* Stops at the first physical page that differs. */
			if (window->slot_physical[start + index] !=
			    aligned + (paddr_t)index *
			    AMD64_ACPI_WINDOW_PAGE_SIZE)
				break;
		}

		/* Reuses a complete match without creating mappings. */
		if (index == pages) {
			*first_slot = start;
			*page_offset = offset;
			*new_first_slot = 0;
			*new_slot_count = 0;
			return 1;
		}
	}

	/*
	 * A header probe is immediately followed by a full-table request.
	 * Extends a matching terminal prefix so the header page is not duplicated.
	 */
	for (start = 0; start < window->used; start++) {
		available = window->used - start;

		/* Only an incomplete terminal suffix can be extended. */
		if (available >= pages)
			continue;

		/* Compares every page already present in the terminal suffix. */
		for (index = 0; index < available; index++) {
			/* Stops when the suffix no longer matches the request. */
			if (window->slot_physical[start + index] !=
			    aligned + (paddr_t)index *
			    AMD64_ACPI_WINDOW_PAGE_SIZE)
				break;
		}

		/* Ignores a suffix which does not match the requested prefix. */
		if (index != available)
			continue;

		/* Requires enough free slots for the missing terminal pages. */
		missing = pages - available;
		if (missing > AMD64_ACPI_WINDOW_SLOTS - window->used)
			return 0;

		/* Reports and appends only the newly required mappings. */
		*new_first_slot = window->used;
		*new_slot_count = missing;
		for (index = available; index < pages; index++) {
			window->slot_physical[window->used++] =
			    aligned + (paddr_t)index *
			    AMD64_ACPI_WINDOW_PAGE_SIZE;
		}

		/* Returns the location of the complete extended sequence. */
		*first_slot = start;
		*page_offset = offset;
		return 1;
	}

	/* Requires room for a completely new page sequence. */
	if (pages > AMD64_ACPI_WINDOW_SLOTS - window->used)
		return 0;

	/* Appends every physical page in the new sequence. */
	start = window->used;
	for (index = 0; index < pages; index++) {
		window->slot_physical[start + index] =
		    aligned + (paddr_t)index * AMD64_ACPI_WINDOW_PAGE_SIZE;
	}
	window->used += pages;

	/* Reports the location and full extent of the newly mapped sequence. */
	*first_slot = start;
	*page_offset = offset;
	*new_first_slot = start;
	*new_slot_count = pages;
	return 1;
}
