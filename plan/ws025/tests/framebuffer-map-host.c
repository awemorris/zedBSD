/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "src/hal/amd64/framebuffer-map.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int
main(void)
{
	uint64_t directory[256];
	uint64_t edges[2][512];
	uint64_t tables[2] = {0x200000, 0x201000};
	uint64_t base = UINT64_C(0x100001123);
	uint64_t size = UINT64_C(0x400456);
	uint64_t physical;
	uint64_t entry;
	uint64_t first;
	uint64_t end;
	unsigned chunk;
	unsigned slot;
	unsigned edge;

	memset(directory, 0, sizeof(directory));
	assert(amd64_framebuffer_map(directory, 256, edges, tables, base, size, (UINT64_C(1) << 48) - 1));
	first = base & ~UINT64_C(4095);
	end = (base + size + 4095) & ~UINT64_C(4095);
	edge = 0;
	for (chunk = 0; chunk < 3; chunk++) {
		for (slot = 0; slot < 512; slot++) {
			physical = (base & ~UINT64_C(0x1fffff)) + (uint64_t)chunk * 0x200000 + slot * 4096;
			if (directory[chunk] & 128)
				entry = directory[chunk] + slot * 4096;
			else
				entry = edges[edge][slot];
			if (physical < first || physical >= end) {
				assert(entry == 0);
			} else {
				assert((entry & UINT64_C(0x000ffffffffff000)) == physical);
				assert((entry & UINT64_C(0x8000000000000113)) == UINT64_C(0x8000000000000113));
			}
		}
		if (!(directory[chunk] & 128))
			edge++;
	}
	assert(edge == 2 && directory[3] == 0);
	assert(!amd64_framebuffer_map(directory, 2, edges, tables, base, size, UINT64_MAX));
	assert(!amd64_framebuffer_map(directory, 256, edges, tables, base, size, UINT32_MAX));
	assert(!amd64_framebuffer_map(directory, 256, edges, tables, UINT64_MAX - 4, 8, UINT64_MAX));
	assert(amd64_framebuffer_map(directory, 256, edges, tables, base, 1, UINT64_MAX));
	assert(edges[0][0] == 0 && edges[0][1] != 0 && edges[0][2] == 0);
	puts("PASS: high framebuffer PA, two partial edges, holes and overflow");
	return 0;
}
