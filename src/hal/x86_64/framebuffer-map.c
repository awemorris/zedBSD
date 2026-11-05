/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib. */
#include "framebuffer-map.h"

#define FB_PAGE UINT64_C(4096)
#define FB_LARGE UINT64_C(0x200000)
#define FB_FLAGS (UINT64_C(0x8000000000000113))

/*
 * Builds uncached leaves without aliasing adjacent write-back RAM pages.
 * The caller supplies unpublished directories and two owned edge tables.
 */
int
amd64_framebuffer_map(
	uint64_t *directory,
	unsigned capacity,
	uint64_t edges[2][512],
	const uint64_t edge_physical[2],
	uint64_t base,
	uint64_t size,
	uint64_t physical_max)
{
	uint64_t first;
	uint64_t end;
	uint64_t aligned;
	uint64_t count;
	uint64_t chunk;
	uint64_t page;
	unsigned index;
	unsigned slot;
	unsigned edge;

	/* Rejects overflow and unrepresentable table ownership before mutation. */
	if (directory == 0 || edges == 0 || edge_physical == 0 || size == 0 ||
	    base > physical_max || size - 1U > physical_max - base ||
	    size > UINT64_MAX - base || base + size > UINT64_MAX - (FB_PAGE - 1U))
		return 0;
	first = base & ~(FB_PAGE - 1U);
	end = (base + size + FB_PAGE - 1U) & ~(FB_PAGE - 1U);
	aligned = base & ~(FB_LARGE - 1U);
	count = (end - aligned) / FB_LARGE + ((end - aligned) % FB_LARGE != 0);
	if (count == 0 || count > capacity)
		return 0;
	for (index = 0; index < 2; index++) {
		if ((edge_physical[index] & (FB_PAGE - 1U)) != 0 ||
		    edge_physical[index] > physical_max || FB_PAGE - 1U > physical_max - edge_physical[index])
			return 0;
	}

	/* Clears both edge tables before publishing any present table entry. */
	for (index = 0; index < 2; index++) {
		for (slot = 0; slot < 512; slot++)
			edges[index][slot] = 0;
	}
	edge = 0;
	for (index = 0; index < count; index++) {
		chunk = aligned + (uint64_t)index * FB_LARGE;
		if (chunk >= first && end - chunk >= FB_LARGE) {
			directory[index] = chunk | FB_FLAGS | UINT64_C(0x80);
			continue;
		}
		for (slot = 0; slot < 512; slot++) {
			page = chunk + (uint64_t)slot * FB_PAGE;
			if (page >= first && page < end)
				edges[edge][slot] = page | FB_FLAGS;
		}
		directory[index] = edge_physical[edge] | UINT64_C(3);
		edge++;
	}
	return 1;
}
