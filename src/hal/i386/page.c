/*
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 *
 * i386 physical memory management (pmem): range descriptors over a
 * page-usage bitmap.  Range bookkeeping only -- page tables are managed
 * by the space API.
 */

#include <hal/hal.h>
#include "asm.h"

#define PAGEMAP_GET(n)		(pagemap_tbl[(n) >> 5] & (1U << ((n) & 31)))
#define PAGEMAP_SET(n)		(pagemap_tbl[(n) >> 5] |= (1U << ((n) & 31)))
#define PAGEMAP_RESET(n)	(pagemap_tbl[(n) >> 5] &= ~(1U << ((n) & 31)))

/* Enough bitmap for PHYSICAL_MEGS of RAM. */
#define PAGEMAP_WORDS	(PHYSICAL_MEGS * (1024U * 1024U / PAGE_SIZE) / 32U)

/* Number of physical pages present. */
static uint32 phys_pages;
static uint32 reserved_pages;
static uint32 allocated_pages;

/* Page usage bitmap. */
static uint32 pagemap_tbl[PAGEMAP_WORDS];

/* Provided by the BSP: total RAM in bytes, and device-window reserves. */
uint32 bsp_mem_probe(void);
void bsp_mem_reserve(void);

static void init_pagemap_tbl(void);

/*
 * Initialize the memory management module.
 */
void
i386_page_init(void)
{
	init_pagemap_tbl();
	bsp_mem_reserve();
}

/* Build the physical page map from the BSP's memory probe. */
static void
init_pagemap_tbl(void)
{
	uint32 total;
	uint32 reserved_top;
	uint32 i;

	total = bsp_mem_probe();
	if (total == 0)
		HAL_FATAL("can't detect memory size");
	phys_pages = total / PAGE_SIZE;
	if (phys_pages > PAGEMAP_WORDS * 32U)
		phys_pages = PAGEMAP_WORDS * 32U;
	hal_printf("boot: physical memory: %u KiB\n", total / 1024U);
	if (total < 0x400000)
		HAL_FATAL("too few physical memory");

	hal_memset(pagemap_tbl, 0, sizeof(pagemap_tbl));
	reserved_pages = allocated_pages = 0;

	/*
	 * The fixed work areas below ADDR_FREE_TOP (IDT, boot info,
	 * startup stack) stay out of the allocator.
	 */
	reserved_top = (ADDR_FREE_TOP + PAGE_SIZE - 1) / PAGE_SIZE;
	if (reserved_top > phys_pages)
		reserved_top = phys_pages;
	for (i = 0; i < reserved_top; i++) {
		PAGEMAP_SET(i);
		reserved_pages++;
	}
}

/*
 * Exclude [paddr, paddr+size) from allocation.
 */
void
pmem_reserve(hal_physaddr_t paddr, size_t size)
{
	uint32 first = paddr / PAGE_SIZE;
	uint32 last;
	uint32 i;

	if (size == 0)
		return;
	if (paddr > UINT32_MAX - size)
		last = PAGEMAP_WORDS * 32U;
	else
		last = (paddr + size + PAGE_SIZE - 1) / PAGE_SIZE;

	if (last > phys_pages)
		last = phys_pages;
	for (i = first; i < last; i++)
		if (!PAGEMAP_GET(i)) {
			PAGEMAP_SET(i);
			reserved_pages++;
		}
}

/*
 * Allocate contiguous physical pages from the direct-mapped low area
 * (< 1GB); accessible without pmem_lock().
 */
int
pmem_alloc_lo(size_t size, struct pmem_desc *desc)
{
	uint32 need_pages;
	uint32 start_index;
	uint32 page_end;
	uint32 i;
	irqlock_t irqlock;

	need_pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
	if (need_pages == 0 || need_pages > phys_pages)
		return PMEM_NOSPACE;
	page_end = phys_pages - need_pages;

	ENTER_IRQLOCK(irqlock)
	{
		start_index = 0;
		for (;;) {
			for (; start_index <= page_end; start_index++)
				if (PAGEMAP_GET(start_index) == 0)
					break;
			if (start_index > page_end)
				break;
			for (i = 0; i < need_pages; i++)
				if (PAGEMAP_GET(start_index + i) != 0)
					break;
			if (i == need_pages)
				break;
			start_index += i + 1;
		}
		if (start_index > page_end) {
			LEAVE_IRQLOCK(irqlock);
			return PMEM_NOSPACE;
		}
		for (i = 0; i < need_pages; i++)
			PAGEMAP_SET(start_index + i);
		allocated_pages += need_pages;
	}
	LEAVE_IRQLOCK(irqlock);

	desc->paddr = (void *)(start_index << 12);
	desc->vaddr = (void *)((start_index << 12) | SYS_START);
	desc->size = need_pages << 12;
	return PMEM_SUCCESS;
}

int
hal_pmem_alloc(size_t size, struct hal_pmem *desc, uint32_t flags)
{
	struct pmem_desc memory;
	int error;

	if (desc == NULL || (flags & ~(HAL_PMEM_ATTR_NOCACHE |
	    HAL_PMEM_ATTR_WRITETHRU)) != 0)
		return HAL_PMEM_BADDESC;
	error = pmem_alloc_lo(size, &memory);
	if (error != PMEM_SUCCESS)
		return error == PMEM_NOSPACE ? HAL_PMEM_NOSPACE : HAL_PMEM_BADDESC;
	desc->vaddr = (uintptr_t)memory.vaddr;
	desc->paddr = (uintptr_t)memory.paddr;
	desc->size = memory.size;
	return HAL_PMEM_SUCCESS;
}

int
hal_pmem_alloc_limited(size_t size, uintptr_t above, uintptr_t below,
		       struct hal_pmem *desc)
{
	uint32 need_pages, first_page, end_page, start, i;
	irqlock_t irqlock;

	if (desc == NULL || above >= below ||
	    above > UINTPTR_MAX - (PAGE_SIZE - 1U))
		return HAL_PMEM_BADDESC;
	need_pages = (size + PAGE_SIZE - 1U) / PAGE_SIZE;
	first_page = (uint32)((above + PAGE_SIZE - 1U) / PAGE_SIZE);
	end_page = (uint32)(below / PAGE_SIZE);
	if (end_page > phys_pages)
		end_page = phys_pages;
	if (need_pages == 0 || first_page >= end_page ||
	    need_pages > end_page - first_page)
		return HAL_PMEM_NOSPACE;
	ENTER_IRQLOCK(irqlock)
	{
		start = first_page;
		for (;;) {
			for (; start + need_pages <= end_page; start++)
				if (!PAGEMAP_GET(start))
					break;
			if (start + need_pages > end_page)
				break;
			for (i = 0; i < need_pages; i++)
				if (PAGEMAP_GET(start + i))
					break;
			if (i == need_pages)
				break;
			start += i + 1U;
		}
		if (start + need_pages > end_page) {
			LEAVE_IRQLOCK(irqlock);
			return HAL_PMEM_NOSPACE;
		}
		for (i = 0; i < need_pages; i++)
			PAGEMAP_SET(start + i);
		allocated_pages += need_pages;
	}
	LEAVE_IRQLOCK(irqlock);
	desc->paddr = (uintptr_t)start * PAGE_SIZE;
	desc->vaddr = desc->paddr | SYS_START;
	desc->size = (size_t)need_pages * PAGE_SIZE;
	return HAL_PMEM_SUCCESS;
}

int
hal_pmem_free(struct hal_pmem *desc)
{
	struct pmem_desc memory;
	int error;

	if (desc == NULL)
		return HAL_PMEM_BADDESC;
	memory.vaddr = (void *)desc->vaddr;
	memory.paddr = (void *)desc->paddr;
	memory.size = desc->size;
	error = pmem_free(&memory);
	if (error != PMEM_SUCCESS)
		return HAL_PMEM_BADDESC;
	desc->vaddr = desc->paddr = 0;
	desc->size = 0;
	return HAL_PMEM_SUCCESS;
}

size_t
hal_pmem_get_total_size(void)
{
	return (size_t)phys_pages * PAGE_SIZE;
}

void hal_i386_task_memory_stats(uint32_t *, size_t *);
void hal_i386_space_memory_stats(uint32_t *, uint32_t *);

void
hal_memory_get_stats(struct hal_memory_stats *stats)
{
	if (stats == NULL)
		return;
	hal_memset(stats, 0, sizeof(*stats));
	stats->physical_total = (size_t)phys_pages * PAGE_SIZE;
	stats->physical_reserved = (size_t)reserved_pages * PAGE_SIZE;
	stats->physical_allocated = (size_t)allocated_pages * PAGE_SIZE;
	stats->physical_free = stats->physical_total -
		stats->physical_reserved - stats->physical_allocated;
	hal_i386_task_memory_stats(&stats->task_count,
				   &stats->task_stack_bytes);
	hal_i386_space_memory_stats(&stats->space_count,
				    &stats->page_table_count);
}

void
hal_mem_get_memory_map(int *blocks, struct hal_memory_map_entry *entries,
		       size_t buf_count)
{
	if (blocks != NULL)
		*blocks = 1;
	if (entries != NULL && buf_count != 0) {
		entries[0].base = 0;
		entries[0].size = hal_pmem_get_total_size();
		entries[0].flags = HAL_PAGE_ENTRY_RAM;
	}
}

/*
 * Free pages allocated with pmem_alloc_lo().
 */
int
pmem_free(struct pmem_desc *desc)
{
	uint32 start_page, end_page, i;
	irqlock_t irqlock;

	start_page = (uint32)desc->paddr >> 12;
	end_page = start_page + (desc->size >> 12);

	ENTER_IRQLOCK(irqlock)
	{
		for (i = start_page; i < end_page; i++) {
			if (PAGEMAP_GET(i) == 0) {
				LEAVE_IRQLOCK(irqlock);
				return PMEM_BADDESC;
			}
		}
		if (allocated_pages < end_page - start_page) {
			LEAVE_IRQLOCK(irqlock);
			return PMEM_BADDESC;
		}
		for (i = start_page; i < end_page; i++)
			PAGEMAP_RESET(i);
		allocated_pages -= end_page - start_page;
	}
	LEAVE_IRQLOCK(irqlock);
	return PMEM_SUCCESS;
}
