/* Sparse persistent mapping slots for amd64 ACPI table discovery. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "acpi-window.h"

void
amd64_acpi_window_init(struct amd64_acpi_window *window)
{
	if (window != NULL)
		window->used = 0;
}

int
amd64_acpi_window_reserve(struct amd64_acpi_window *window,
			  paddr_t physical, size_t size,
			  unsigned *first_slot, size_t *page_offset,
			  unsigned *new_first_slot,
			  unsigned *new_slot_count)
{
	const paddr_t page_mask = AMD64_ACPI_WINDOW_PAGE_SIZE - 1U;
	paddr_t aligned, end;
	size_t offset, span, page_count;
	unsigned pages, start, index;

	if (window == NULL || first_slot == NULL || page_offset == NULL ||
	    new_first_slot == NULL || new_slot_count == NULL || size == 0 ||
	    physical > UINTPTR_MAX - (size - 1U))
		return 0;
	aligned = physical & ~page_mask;
	offset = (size_t)(physical - aligned);
	if (size > SIZE_MAX - offset)
		return 0;
	span = size + offset;
	page_count = span / AMD64_ACPI_WINDOW_PAGE_SIZE;
	if (span % AMD64_ACPI_WINDOW_PAGE_SIZE != 0)
		page_count++;
	if (page_count == 0 || page_count > AMD64_ACPI_WINDOW_SLOTS)
		return 0;
	pages = (unsigned)page_count;
	end = aligned + (paddr_t)(pages - 1U) *
			AMD64_ACPI_WINDOW_PAGE_SIZE;
	if (end < aligned)
		return 0;

	for (start = 0; start + pages <= window->used; start++) {
		for (index = 0; index < pages; index++)
			if (window->slot_physical[start + index] !=
			    aligned + (paddr_t)index *
					  AMD64_ACPI_WINDOW_PAGE_SIZE)
				break;
		if (index == pages) {
			*first_slot = start;
			*page_offset = offset;
			*new_first_slot = 0;
			*new_slot_count = 0;
			return 1;
		}
	}
	/* A header probe is immediately followed by a full-table request.  Extend
	 * a matching terminal prefix so that the header page is not duplicated. */
	for (start = 0; start < window->used; start++) {
		unsigned available = window->used - start;
		unsigned missing;

		if (available >= pages)
			continue;
		for (index = 0; index < available; index++)
			if (window->slot_physical[start + index] !=
			    aligned + (paddr_t)index *
					  AMD64_ACPI_WINDOW_PAGE_SIZE)
				break;
		if (index != available)
			continue;
		missing = pages - available;
		if (missing > AMD64_ACPI_WINDOW_SLOTS - window->used)
			return 0;
		*new_first_slot = window->used;
		*new_slot_count = missing;
		for (index = available; index < pages; index++)
			window->slot_physical[window->used++] =
			    aligned + (paddr_t)index *
					  AMD64_ACPI_WINDOW_PAGE_SIZE;
		*first_slot = start;
		*page_offset = offset;
		return 1;
	}
	if (pages > AMD64_ACPI_WINDOW_SLOTS - window->used)
		return 0;
	start = window->used;
	for (index = 0; index < pages; index++)
		window->slot_physical[start + index] =
		    aligned + (paddr_t)index * AMD64_ACPI_WINDOW_PAGE_SIZE;
	window->used += pages;
	*first_slot = start;
	*page_offset = offset;
	*new_first_slot = start;
	*new_slot_count = pages;
	return 1;
}
