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

#define FIXED_CLAIMS 8U
static struct hal_pmem fixed_claims[FIXED_CLAIMS];

/* Provided by the BSP: total RAM in bytes. */
uint32 bsp_mem_probe(void);

#ifdef HAL_BOARD_PC98
extern char __kernel_phys_start[], __kernel_phys_end[];
#else
extern char __low_start[], __low_end[], __high_start[], __high_end[];
#endif

static void init_pagemap_tbl(void);
static void reserve_range(hal_physaddr_t, size_t);

/*
 * Initialize the memory management module.
 */
void
i386_page_init(void)
{
	init_pagemap_tbl();
	reserve_range(0x000a0000U, 0x00060000U);
#ifdef HAL_BOARD_PC98
	reserve_range(0x00f00000U, 0x00100000U);
#endif
#ifdef HAL_BOARD_PC98
	reserve_range((hal_physaddr_t)(uintptr_t)__kernel_phys_start,
	    (size_t)(__kernel_phys_end - __kernel_phys_start));
#else
	reserve_range((hal_physaddr_t)((uintptr_t)__low_start & ~SYS_START),
	    (size_t)(__low_end - __low_start));
	reserve_range((hal_physaddr_t)((uintptr_t)__high_start & ~SYS_START),
	    (size_t)(__high_end - __high_start));
#endif
	hal_memset(fixed_claims, 0, sizeof(fixed_claims));
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
static void
reserve_range(hal_physaddr_t paddr, size_t size)
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
static int
alloc_ram(size_t size, size_t alignment, struct hal_pmem *desc)
{
	uint32 need_pages;
	uint32 start_index;
	uint32 page_end;
	uint32 i;
	bool irq_enabled;

	uint32 align_pages;

	need_pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
	align_pages = (uint32)(alignment / PAGE_SIZE);
	if (need_pages == 0 || need_pages > phys_pages)
		return HAL_ERR_NOMEM;
	page_end = phys_pages - need_pages;

	irq_enabled = hal_irq_disable();
	{
		start_index = 0;
		for (;;) {
			for (; start_index <= page_end; start_index++) {
				if ((start_index & (align_pages - 1U)) != 0)
					continue;
				if (PAGEMAP_GET(start_index) == 0)
					break;
			}
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
			if (irq_enabled) hal_irq_enable();
			return HAL_ERR_NOMEM;
		}
		for (i = 0; i < need_pages; i++)
			PAGEMAP_SET(start_index + i);
		allocated_pages += need_pages;
	}
	if (irq_enabled) hal_irq_enable();

	desc->paddr = (hal_physaddr_t)(start_index << 12);
	desc->vaddr = (void *)((start_index << 12) | SYS_START);
	desc->size = need_pages << 12;
	desc->type = HAL_PMEM_TYPE_RAM;
	desc->attr = 0;
	return HAL_OK;
}

static int
valid_fixed_window(hal_physaddr_t paddr, size_t size)
{
	return (paddr >= 0x000a0000U && paddr <= 0x00100000U &&
	    size <= 0x00100000U - paddr) ||
	    (paddr >= 0xf0000000U && paddr <= 0xf1000000U &&
	    size <= 0xf1000000U - paddr);
}

static int
claim_fixed(const struct hal_pmem_request *request, struct hal_pmem *desc)
{
	unsigned i, free_slot = FIXED_CLAIMS;
	hal_physaddr_t end = request->paddr + request->size;
	bool irq_enabled;

	irq_enabled = hal_irq_disable();
	{
		for (i = 0; i < FIXED_CLAIMS; i++) {
			hal_physaddr_t claim_end;
			if (fixed_claims[i].size == 0) {
				if (free_slot == FIXED_CLAIMS)
					free_slot = i;
				continue;
			}
			claim_end = fixed_claims[i].paddr + fixed_claims[i].size;
			if (request->paddr < claim_end && end > fixed_claims[i].paddr) {
				if (irq_enabled) hal_irq_enable();
				return HAL_ERR_BUSY;
			}
		}
		if (free_slot == FIXED_CLAIMS) {
			if (irq_enabled) hal_irq_enable();
			return HAL_ERR_NOMEM;
		}
		fixed_claims[free_slot].vaddr =
		    (void *)((uintptr_t)request->paddr | SYS_START);
		fixed_claims[free_slot].paddr = request->paddr;
		fixed_claims[free_slot].size = request->size;
		fixed_claims[free_slot].type = request->type;
		fixed_claims[free_slot].attr = request->attr;
		*desc = fixed_claims[free_slot];
	}
	if (irq_enabled) hal_irq_enable();
	return HAL_OK;
}

int
hal_pmem_alloc(const struct hal_pmem_request *request, struct hal_pmem *desc)
{
	struct hal_pmem result;
	size_t alignment;

	if (request == NULL || desc == NULL || request->size == 0 ||
	    (request->attr & ~(HAL_PMEM_ATTR_NOCACHE |
	    HAL_PMEM_ATTR_WRITETHRU)) != 0)
		return HAL_ERR_INVALID;
	alignment = request->alignment == 0 ? PAGE_SIZE : request->alignment;
	if (alignment < PAGE_SIZE || (alignment & (alignment - 1U)) != 0)
		return HAL_ERR_INVALID;
	if (request->type == HAL_PMEM_TYPE_RAM) {
		if (request->paddr != HAL_PMEM_PADDR_ANY || request->attr != 0)
			return HAL_ERR_INVALID;
		return alloc_ram(request->size, alignment, desc);
	}
	if ((request->type != HAL_PMEM_TYPE_MMIO &&
	    request->type != HAL_PMEM_TYPE_VRAM) ||
	    request->paddr == HAL_PMEM_PADDR_ANY ||
	    request->paddr > UINT32_MAX - request->size ||
	    (request->paddr & (alignment - 1U)) != 0 ||
	    !valid_fixed_window(request->paddr, request->size))
		return HAL_ERR_INVALID;
	result = *desc;
	(void)result;
	return claim_fixed(request, desc);
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

int
hal_pmem_free(struct hal_pmem *desc)
{
	uint32 start_page, end_page, i;
	unsigned slot;
	bool irq_enabled;

	if (desc == NULL || desc->size == 0)
		return HAL_ERR_INVALID;
	if (desc->type != HAL_PMEM_TYPE_RAM) {
		irq_enabled = hal_irq_disable();
		{
			for (slot = 0; slot < FIXED_CLAIMS; slot++)
				if (fixed_claims[slot].vaddr == desc->vaddr &&
				    fixed_claims[slot].paddr == desc->paddr &&
				    fixed_claims[slot].size == desc->size &&
				    fixed_claims[slot].type == desc->type)
					break;
			if (slot == FIXED_CLAIMS) {
				if (irq_enabled) hal_irq_enable();
				return HAL_ERR_STATE;
			}
			hal_memset(&fixed_claims[slot], 0,
			    sizeof(fixed_claims[slot]));
		}
		if (irq_enabled) hal_irq_enable();
		hal_memset(desc, 0, sizeof(*desc));
		return HAL_OK;
	}
	if (desc->vaddr != (void *)((uintptr_t)desc->paddr | SYS_START) ||
	    (desc->paddr & (PAGE_SIZE - 1U)) != 0 ||
	    (desc->size & (PAGE_SIZE - 1U)) != 0)
		return HAL_ERR_INVALID;
	start_page = (uint32)desc->paddr >> 12;
	end_page = start_page + (desc->size >> 12);
	if (start_page >= phys_pages || end_page > phys_pages)
		return HAL_ERR_INVALID;

	irq_enabled = hal_irq_disable();
	{
		for (i = start_page; i < end_page; i++) {
			if (PAGEMAP_GET(i) == 0) {
				if (irq_enabled) hal_irq_enable();
				return HAL_ERR_STATE;
			}
		}
		if (allocated_pages < end_page - start_page) {
			if (irq_enabled) hal_irq_enable();
			return HAL_ERR_STATE;
		}
		for (i = start_page; i < end_page; i++)
			PAGEMAP_RESET(i);
		allocated_pages -= end_page - start_page;
	}
	if (irq_enabled) hal_irq_enable();
	hal_memset(desc, 0, sizeof(*desc));
	return HAL_OK;
}
