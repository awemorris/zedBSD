/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The i386 physical-memory range allocator implementation.
 *
 * It tracks page ownership in a fixed bitmap.  Address-space page tables are
 * managed separately by the space API.
 */

#include <hal/hal.h>

#include "asm.h"

#define PAGEMAP_GET(n) \
	(pagemap_tbl[(n) >> 5] & (1U << ((n) & 31)))
#define PAGEMAP_SET(n) \
	(pagemap_tbl[(n) >> 5] |= (1U << ((n) & 31)))
#define PAGEMAP_RESET(n) \
	(pagemap_tbl[(n) >> 5] &= ~(1U << ((n) & 31)))

#define PAGEMAP_WORDS \
	(PHYSICAL_MEGS * (1024U * 1024U / PAGE_SIZE) / 32U)

#ifdef HAL_BOARD_PC98
extern char __kernel_phys_start[];
extern char __kernel_phys_end[];
#else
extern char __low_start[];
extern char __low_end[];
extern char __high_start[];
extern char __high_end[];
#endif

static uint32_t phys_pages;
static uint32_t reserved_pages;
static uint32_t allocated_pages;
static volatile unsigned pmem_lock;
static uint32_t pagemap_tbl[PAGEMAP_WORDS];

uint32_t bsp_mem_probe(void);
void hal_i386_task_memory_stats(uint32_t *, size_t *);
void hal_i386_space_memory_stats(uint32_t *, uint32_t *);

static bool pmem_lock_enter(void);
static void pmem_lock_leave(bool enabled);
static void init_pagemap_tbl(void);
static void reserve_range(hal_physaddr_t paddr, size_t size);
static int pmem_alloc_unlocked(size_t size, size_t alignment,
			       hal_physaddr_t max_paddr, size_t boundary,
			       hal_physaddr_t *block);
static int pmem_free_unlocked(hal_physaddr_t *block, size_t size);

/*
 * Initializes the i386 physical-page ownership map.
 */
void
i386_page_init(
	void)
{
	/* Builds the available page map and reserves fixed device memory. */
	init_pagemap_tbl();
	reserve_range(0x000a0000U, 0x00060000U);
#ifdef HAL_BOARD_PC98
	reserve_range(0x00f00000U, 0x00100000U);
#endif

	/* Reserves the board-specific kernel image ranges. */
#ifdef HAL_BOARD_PC98
	reserve_range(
		(hal_physaddr_t)(uintptr_t)__kernel_phys_start,
		(size_t)(__kernel_phys_end - __kernel_phys_start));
#else
	reserve_range(
		(hal_physaddr_t)((uintptr_t)__low_start & ~SYS_START),
		(size_t)(__low_end - __low_start));
	reserve_range(
		(hal_physaddr_t)((uintptr_t)__high_start & ~SYS_START),
		(size_t)(__high_end - __high_start));
#endif

}

/*
 * Allocates one physical RAM block.
 */
int
hal_pmem_alloc(
	size_t req_size,
	size_t req_align,
	hal_physaddr_t *block)
{
	bool enabled;
	int error;

	/* Performs the complete allocation while holding the global lock. */
	enabled = pmem_lock_enter();
	error = pmem_alloc_unlocked(req_size, req_align, UINT32_MAX, 0, block);
	pmem_lock_leave(enabled);

	/* Returns the allocation result unchanged. */
	return error;
}

/*
 * Allocates one physical RAM block a device can reach.
 */
int
hal_pmem_alloc_limited(
	size_t req_size,
	size_t req_align,
	hal_physaddr_t max_paddr,
	size_t boundary,
	hal_physaddr_t *block)
{
	bool enabled;
	int error;

	/* Performs the constrained allocation while holding the global lock. */
	enabled = pmem_lock_enter();
	error = pmem_alloc_unlocked(req_size, req_align, max_paddr, boundary,
				    block);
	pmem_lock_leave(enabled);

	/* Returns the allocation result unchanged. */
	return error;
}

/*
 * Frees one physical RAM block.
 */
int
hal_pmem_free(
	hal_physaddr_t *block,
	size_t size)
{
	bool enabled;
	int error;

	/* Performs the complete release while holding the global lock. */
	enabled = pmem_lock_enter();
	error = pmem_free_unlocked(block, size);
	pmem_lock_leave(enabled);

	/* Returns the release result unchanged. */
	return error;
}

/*
 * Translates a physical RAM address to its kernel address.
 */
void *
hal_pmem_to_kernel(
	hal_physaddr_t paddr)
{
	/* Reports no alias for an address outside managed RAM. */
	if (paddr >= (hal_physaddr_t)phys_pages * PAGE_SIZE)
		return NULL;

	/* Managed RAM is direct-mapped into the system half. */
	return (void *)((uintptr_t)paddr | SYS_START);
}

/*
 * Reports the total managed physical-memory size.
 */
size_t
hal_pmem_get_total_size(
	void)
{
	/* Returns the managed page count in bytes. */
	return (size_t)phys_pages * PAGE_SIZE;
}

/*
 * Collects i386 physical, task, and address-space memory statistics.
 */
void
hal_get_memstat(
	struct hal_memstat *stats)
{
	bool enabled;

	/* Ignores requests without result storage. */
	if (stats == NULL)
		return;

	/* Samples physical-page counters under the allocator lock. */
	hal_memset(stats, 0, sizeof(*stats));
	enabled = pmem_lock_enter();
	stats->physical_total = (size_t)phys_pages * PAGE_SIZE;
	stats->physical_reserved = (size_t)reserved_pages * PAGE_SIZE;
	stats->physical_allocated = (size_t)allocated_pages * PAGE_SIZE;
	stats->physical_free = stats->physical_total -
	    stats->physical_reserved - stats->physical_allocated;
	pmem_lock_leave(enabled);

	/* Adds task-stack and page-table ownership counters. */
	hal_i386_task_memory_stats(
		&stats->task_count,
		&stats->task_stack_bytes);
	hal_i386_space_memory_stats(
		&stats->space_count,
		&stats->page_table_count);
}

/* Acquires the physical allocator lock with interrupts disabled. */
static bool
pmem_lock_enter(
	void)
{
	bool enabled;

	/* Preserves interrupt state before waiting for the allocator lock. */
	enabled = hal_irq_disable();

	/* Waits until this CPU owns the allocator lock. */
	while (__atomic_exchange_n(&pmem_lock, 1U, __ATOMIC_ACQUIRE) != 0U)
		__asm__ volatile("pause");

	/* Returns the saved interrupt state. */
	return enabled;
}

/* Releases the physical allocator lock and restores interrupts. */
static void
pmem_lock_leave(
	bool enabled)
{
	/* Publishes the unlocked state before restoring interrupt state. */
	__atomic_store_n(&pmem_lock, 0U, __ATOMIC_RELEASE);

	/* Restores interrupts only when they were previously enabled. */
	if (enabled)
		hal_irq_enable();
}

/* Builds the physical page map from the BSP memory probe. */
static void
init_pagemap_tbl(
	void)
{
	uint32_t total;
	uint32_t reserved_top;
	uint32_t i;

	/* Reads and validates the board's detected memory size. */
	total = bsp_mem_probe();

	/* Rejects a failed board memory probe. */
	if (total == 0)
		HAL_FATAL("can't detect memory size");

	/* Clips the managed page count to the fixed bitmap capacity. */
	phys_pages = total / PAGE_SIZE;

	/* Clips memory larger than the static physical-page bitmap. */
	if (phys_pages > PAGEMAP_WORDS * 32U)
		phys_pages = PAGEMAP_WORDS * 32U;

	/* Rejects a machine below the minimum supported memory size. */
	if (total < 0x400000)
		HAL_FATAL("too few physical memory");

	/* Clears page ownership and its accounting counters. */
	hal_memset(pagemap_tbl, 0, sizeof(pagemap_tbl));
	reserved_pages = 0;
	allocated_pages = 0;

	/* Reserves the IDT, boot-info, stack, and low bootstrap work areas. */
	reserved_top = (ADDR_FREE_TOP + PAGE_SIZE - 1) / PAGE_SIZE;

	/* Clips the bootstrap reservation to detected physical memory. */
	if (reserved_top > phys_pages)
		reserved_top = phys_pages;
	for (i = 0; i < reserved_top; i++) {
		PAGEMAP_SET(i);
		reserved_pages++;
	}
}

/* Excludes one physical address interval from allocation. */
static void
reserve_range(
	hal_physaddr_t paddr,
	size_t size)
{
	uint32_t first;
	uint32_t last;
	uint32_t i;

	/* Ignores an empty reservation. */
	if (size == 0)
		return;

	/* Converts the interval to a clipped page-index range. */
	first = paddr / PAGE_SIZE;

	/* Saturates an address interval which overflows physical arithmetic. */
	if (paddr > UINT32_MAX - size) {
		last = PAGEMAP_WORDS * 32U;
	} else {
		last = (paddr + size + PAGE_SIZE - 1) / PAGE_SIZE;
	}

	/* Clips the reservation to the managed page inventory. */
	if (last > phys_pages)
		last = phys_pages;

	/* Marks every newly reserved page and accounts for it once. */
	for (i = first; i < last; i++) {
		/* Avoids accounting for an already reserved page twice. */
		if (!PAGEMAP_GET(i)) {
			PAGEMAP_SET(i);
			reserved_pages++;
		}
	}
}

/*
 * Finds and claims an aligned run of free pages.
 *
 * max_paddr is the highest byte the caller can address, and boundary,
 * when not zero, is a power-of-two block the run must not cross.
 */
static int
pmem_alloc_unlocked(
	size_t size,
	size_t alignment,
	hal_physaddr_t max_paddr,
	size_t boundary,
	hal_physaddr_t *block)
{
	uint32_t need_pages;
	uint32_t align_pages;
	uint32_t start_index;
	uint32_t limit_pages;
	uint32_t page_end;
	uint32_t i;
	uint64_t first;
	uint64_t last;

	/* Requires a destination and a non-empty request. */
	if (block == NULL || size == 0)
		return HAL_ERR_INVALID;

	/* Selects and validates the requested page alignment. */
	if (alignment == 0)
		alignment = PAGE_SIZE;
	if (alignment < PAGE_SIZE || (alignment & (alignment - 1U)) != 0)
		return HAL_ERR_INVALID;

	/* Rejects a boundary which is not a power of two. */
	if (boundary != 0 && (boundary & (boundary - 1U)) != 0)
		return HAL_ERR_INVALID;

	/* Rounds the request up so the free path can repeat this. */
	need_pages = (uint32_t)((size + PAGE_SIZE - 1) / PAGE_SIZE);
	align_pages = (uint32_t)(alignment / PAGE_SIZE);

	/* Rejects requests larger than the managed page inventory. */
	if (need_pages == 0 || need_pages > phys_pages)
		return HAL_ERR_NOMEM;

	/* Limits the search to pages the caller can address. */
	limit_pages = phys_pages;
	if (max_paddr < (hal_physaddr_t)phys_pages * PAGE_SIZE)
		limit_pages = (uint32_t)((max_paddr + 1U) / PAGE_SIZE);
	if (limit_pages < need_pages)
		return HAL_ERR_NOMEM;
	page_end = limit_pages - need_pages;

	/* Searches for an aligned free run meeting every constraint. */
	for (start_index = 0; start_index <= page_end; start_index++) {
		/* Skips candidates which do not meet the requested alignment. */
		if ((start_index & (align_pages - 1U)) != 0)
			continue;

		/* Skips a run which would cross a forbidden boundary. */
		if (boundary != 0) {
			first = (uint64_t)start_index * PAGE_SIZE;
			last = first +
			    (uint64_t)need_pages * PAGE_SIZE - 1U;

			/* Requires both ends inside one boundary block. */
			if (first / boundary != last / boundary)
				continue;
		}

		/* Measures the free run beginning at this candidate. */
		for (i = 0; i < need_pages; i++) {
			/* Stops at the first claimed page in the run. */
			if (PAGEMAP_GET(start_index + i) != 0)
				break;
		}

		/* Stops when the candidate spans the complete request. */
		if (i == need_pages)
			break;
		start_index += i;
	}

	/* Reports exhaustion when no candidate run remains. */
	if (start_index > page_end)
		return HAL_ERR_NOMEM;

	/* Claims and accounts for every page in the selected run. */
	for (i = 0; i < need_pages; i++)
		PAGEMAP_SET(start_index + i);
	allocated_pages += need_pages;

	/* Publishes the allocated physical address. */
	*block = (hal_physaddr_t)start_index * PAGE_SIZE;

	/* Reports a successful RAM allocation. */
	return HAL_OK;
}

/*
 * Releases the pages of one allocation.
 *
 * size is the size the matching allocation requested, so the same
 * rounding is repeated here and a smaller size splits the block.
 */
static int
pmem_free_unlocked(
	hal_physaddr_t *block,
	size_t size)
{
	uint32_t start_page;
	uint32_t end_page;
	uint32_t i;

	/* Rejects an empty release. */
	if (block == NULL || size == 0)
		return HAL_ERR_INVALID;

	/* Requires a page-aligned start. */
	if ((*block & (PAGE_SIZE - 1U)) != 0)
		return HAL_ERR_INVALID;

	/* Repeats the rounding the allocation applied. */
	start_page = (uint32_t)(*block / PAGE_SIZE);
	end_page = start_page +
	    (uint32_t)((size + PAGE_SIZE - 1) / PAGE_SIZE);

	/* Rejects a page interval outside managed physical memory. */
	if (start_page >= phys_pages || end_page > phys_pages)
		return HAL_ERR_INVALID;

	/* Verifies ownership of every page before changing the bitmap. */
	for (i = start_page; i < end_page; i++) {
		/* Rejects the first page absent from the bitmap. */
		if (PAGEMAP_GET(i) == 0)
			return HAL_ERR_STATE;
	}

	/* Rejects an allocation-accounting underflow. */
	if (allocated_pages < end_page - start_page)
		return HAL_ERR_STATE;

	/* Releases and accounts for every page in the interval. */
	for (i = start_page; i < end_page; i++)
		PAGEMAP_RESET(i);
	allocated_pages -= end_page - start_page;

	/* Clears the caller address so a double free is visible. */
	*block = 0;

	/* Reports a released RAM allocation. */
	return HAL_OK;
}
