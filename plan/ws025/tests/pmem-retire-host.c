/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "src/hal/amd64/pmem-range.h"
#include <assert.h>
#include <stdio.h>

int
main(void)
{
	struct amd64_pmem_extent extent;
	uint64_t storage[128];
	uint64_t physical;
	uint64_t size;
	uint64_t base = UINT64_C(0x100000000);

	assert(amd64_pmem_extent_init(&extent, base, 130 * 4096, storage, sizeof(storage)) == AMD64_PMEM_OK);
	assert(amd64_pmem_reserve(&extent, base, 130 * 4096) == AMD64_PMEM_OK);
	assert(amd64_pmem_release_reserved(&extent, base + 4096, 128 * 4096) == AMD64_PMEM_OK);
	assert(extent.reserved_pages == 2 && extent.free_pages == 128);
	assert(amd64_pmem_extent_alloc(&extent, 128 * 4096, 4096, base, UINT64_MAX, 0, &physical, &size) == AMD64_PMEM_OK);
	assert(physical == base + 4096 && extent.allocated_pages == 128);

	/* Mixed permanent and allocated pages fail without releasing the prefix. */
	assert(amd64_pmem_release_reserved(&extent, base, 2 * 4096) == AMD64_PMEM_STATE);
	assert(extent.reserved_pages == 2 && extent.allocated_pages == 128 && extent.free_pages == 0);
	assert(amd64_pmem_extent_free(&extent, physical, size) == AMD64_PMEM_OK);
	assert(amd64_pmem_release_reserved(&extent, base + 4096, 4096) == AMD64_PMEM_STATE);
	assert(amd64_pmem_release_reserved(&extent, base + 1, 4096) == AMD64_PMEM_INVALID);
	assert(extent.reserved_pages == 2 && extent.free_pages == 128);
	puts("PASS: retired reservation accounting, permanent edges and atomic rejection");
	return 0;
}
