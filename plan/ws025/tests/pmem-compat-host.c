/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <hal/hal.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static struct hal_pmem supplied;
static size_t alignment;
static int allocations, releases, release_error;

int
hal_pmem_alloc(hal_physaddr_t request_paddr, size_t request_size, size_t request_alignment, uint32_t request_type, uint32_t request_attr, struct hal_pmem *memory)
{
	(void)request_paddr; (void)request_size; (void)request_type; (void)request_attr;

	allocations++;
	alignment = request_alignment;
	*memory = supplied;
	return HAL_OK;
}

int
hal_pmem_free(struct hal_pmem *memory)
{
	assert(memory->vaddr == supplied.vaddr);
	releases++;
	return release_error;
}

int
main(void)
{
	hal_physaddr_t request_paddr = 0;
	size_t request_size = 0;
	size_t request_alignment = 0;
	uint32_t request_type = 0;
	uint32_t request_attr = 0;
	struct hal_pmem result = {0};
	int before;

	request_type = HAL_PMEM_TYPE_RAM;
	request_paddr = HAL_PMEM_PADDR_ANY;
	request_size = 4096;
	request_alignment = 4096;
	supplied.type = HAL_PMEM_TYPE_RAM;
	supplied.vaddr = &supplied;
	supplied.paddr = 0x10000;
	supplied.size = 4096;
	assert(hal_pmem_alloc_range(request_paddr, request_size, request_alignment, request_type, request_attr, 0x10000, 0x10fff, 0, &result) == HAL_OK);
	assert(releases == 0 && result.paddr == 0x10000);
	assert(hal_pmem_alloc_range(request_paddr, request_size, request_alignment, request_type, request_attr, 0, UINT32_MAX, 65536, &result) == HAL_OK);
	assert(alignment == 65536);

	/* Reject a rounded backing run that crosses the inclusive maximum. */
	supplied.size = 8192;
	assert(hal_pmem_alloc_range(request_paddr, request_size, request_alignment, request_type, request_attr, 0, 0x10fff, 0, &result) == HAL_ERR_NOMEM);
	assert(result.size == 0 && releases == 1);
	supplied.size = 4096;
	supplied.paddr = UINT64_C(0x100000000);
	assert(hal_pmem_alloc_range(request_paddr, request_size, request_alignment, request_type, request_attr, 0, UINT32_MAX, 0, &result) == HAL_ERR_NOMEM);
	assert(result.size == 0 && releases == 2);

	/* Preserve ownership when the underlying HAL refuses rollback. */
	release_error = HAL_ERR_STATE;
	assert(hal_pmem_alloc_range(request_paddr, request_size, request_alignment, request_type, request_attr, 0, UINT32_MAX, 0, &result) == HAL_ERR_STATE);
	assert(result.vaddr == supplied.vaddr && result.paddr == supplied.paddr);
	release_error = HAL_OK;

	/* Validate input before invoking a legacy allocator. */
	before = allocations;
	request_alignment = 3;
	assert(hal_pmem_alloc_range(request_paddr, request_size, request_alignment, request_type, request_attr, 0, UINT64_MAX, 0, &result) == HAL_ERR_INVALID);
	request_alignment = 4096;
	assert(hal_pmem_alloc_range(request_paddr, request_size, request_alignment, request_type, request_attr, 2, 1, 0, &result) == HAL_ERR_INVALID);
	assert(hal_pmem_alloc_range(request_paddr, request_size, request_alignment, request_type, request_attr, 0, UINT64_MAX, 6000, &result) == HAL_ERR_INVALID);
	assert(allocations == before);

	/* Reject legacy results that violate alignment or payload ownership. */
	supplied.paddr = 0x10001;
	assert(hal_pmem_alloc_range(request_paddr, request_size, request_alignment, request_type, request_attr, 0, UINT64_MAX, 0, &result) == HAL_ERR_NOMEM);
	supplied.paddr = 0x10000;
	supplied.size = 2048;
	assert(hal_pmem_alloc_range(request_paddr, request_size, request_alignment, request_type, request_attr, 0, UINT64_MAX, 0, &result) == HAL_ERR_NOMEM);
	supplied.size = 4096;
	supplied.attr = 1;
	assert(hal_pmem_alloc_range(request_paddr, request_size, request_alignment, request_type, request_attr, 0, UINT64_MAX, 0, &result) == HAL_ERR_NOMEM);
	puts("PASS: constrained HAL compatibility, rejection and rollback ownership");
	return 0;
}
