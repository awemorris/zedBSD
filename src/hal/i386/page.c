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
#define FIXED_CLAIMS 8U

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
static struct hal_pmem fixed_claims[FIXED_CLAIMS];

uint32_t bsp_mem_probe(void);
void hal_i386_task_memory_stats(uint32_t *, size_t *);
void hal_i386_space_memory_stats(uint32_t *, uint32_t *);

static bool pmem_lock_enter(void);
static void pmem_lock_leave(bool enabled);
static void init_pagemap_tbl(void);
static void reserve_range(hal_physaddr_t paddr, size_t size);
static int alloc_ram(size_t size, size_t alignment, struct hal_pmem *desc);
static int valid_fixed_window(hal_physaddr_t paddr, size_t size);
static int claim_fixed(const struct hal_pmem_request *request, struct hal_pmem *desc);
static int pmem_alloc_unlocked(const struct hal_pmem_request *request, struct hal_pmem *desc);
static int pmem_free_unlocked(struct hal_pmem *desc);

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

	/* Clears the fixed MMIO and video-memory claim registry. */
	hal_memset(fixed_claims, 0, sizeof(fixed_claims));
}

/*
 * Allocates one physical-memory range.
 */
int
hal_pmem_alloc(
	const struct hal_pmem_request *request,
	struct hal_pmem *desc)
{
	bool enabled;
	int error;

	/* Serializes allocation and records its result. */
	enabled = pmem_lock_enter();
	error = pmem_alloc_unlocked(request, desc);
	pmem_lock_leave(enabled);

	/* Returns the allocation result. */
	return error;
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
hal_memory_get_stats(
	struct hal_memory_stats *stats)
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

/*
 * Releases one physical-memory range.
 */
int
hal_pmem_free(
	struct hal_pmem *desc)
{
	bool enabled;
	int error;

	/* Serializes release and records its result. */
	enabled = pmem_lock_enter();
	error = pmem_free_unlocked(desc);
	pmem_lock_leave(enabled);

	/* Returns the release result. */
	return error;
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

/* Allocates contiguous pages from direct-mapped RAM. */
static int
alloc_ram(
	size_t size,
	size_t alignment,
	struct hal_pmem *desc)
{
	uint32_t need_pages;
	uint32_t align_pages;
	uint32_t start_index;
	uint32_t page_end;
	uint32_t i;
	bool irq_enabled;

	/* Converts the request size and alignment to page counts. */
	need_pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
	align_pages = (uint32_t)(alignment / PAGE_SIZE);

	/* Rejects requests larger than the managed page inventory. */
	if (need_pages == 0 || need_pages > phys_pages)
		return HAL_ERR_NOMEM;
	page_end = phys_pages - need_pages;

	/* Keeps allocation search and bitmap publication interrupt-atomic. */
	irq_enabled = hal_irq_disable();
	start_index = 0;

	/* Searches for an aligned run of unused physical pages. */
	for (;;) {
		/* Finds the next aligned free candidate page. */
		for (; start_index <= page_end; start_index++) {
			/* Skips candidates which do not meet the requested alignment. */
			if ((start_index & (align_pages - 1U)) != 0)
				continue;

			/* Stops at the first aligned unclaimed page. */
			if (PAGEMAP_GET(start_index) == 0)
				break;
		}

		/* Stops when no candidate remains. */
		if (start_index > page_end)
			break;

		/* Measures the free run beginning at this candidate. */
		for (i = 0; i < need_pages; i++) {
			/* Stops at the first claimed page in the candidate run. */
			if (PAGEMAP_GET(start_index + i) != 0)
				break;
		}

		/* Stops when the candidate spans the complete request. */
		if (i == need_pages)
			break;
		start_index += i + 1;
	}

	/* Reports exhaustion after restoring the prior interrupt state. */
	if (start_index > page_end) {
		/* Restores interrupts only when they were previously enabled. */
		if (irq_enabled)
			hal_irq_enable();
		return HAL_ERR_NOMEM;
	}

	/* Claims and accounts for every page in the selected run. */
	for (i = 0; i < need_pages; i++)
		PAGEMAP_SET(start_index + i);
	allocated_pages += need_pages;

	/* Restores interrupts only when they were previously enabled. */
	if (irq_enabled)
		hal_irq_enable();

	/* Describes the direct-mapped allocated range. */
	desc->paddr = (hal_physaddr_t)(start_index << 12);
	desc->vaddr = (void *)((start_index << 12) | SYS_START);
	desc->size = need_pages << 12;
	desc->type = HAL_PMEM_TYPE_RAM;
	desc->attr = 0;

	/* Reports a successful RAM allocation. */
	return HAL_OK;
}

/* Tests whether a fixed claim lies in an allowed device window. */
static int
valid_fixed_window(
	hal_physaddr_t paddr,
	size_t size)
{
	/* Accepts the conventional-memory device aperture. */
	if (paddr >= 0x000a0000U && paddr <= 0x00100000U &&
	    size <= 0x00100000U - paddr) {
		return 1;
	}

	/* Accepts the high PCI-style device aperture. */
	if (paddr >= 0xf0000000U && paddr <= 0xf1000000U &&
	    size <= 0xf1000000U - paddr) {
		return 1;
	}

	/* Rejects fixed claims outside both apertures. */
	return 0;
}

/* Claims one fixed MMIO or video-memory interval. */
static int
claim_fixed(
	const struct hal_pmem_request *request,
	struct hal_pmem *desc)
{
	hal_physaddr_t end;
	hal_physaddr_t claim_end;
	unsigned i;
	unsigned free_slot;
	bool irq_enabled;

	/* Initializes the request extent and empty-slot sentinel. */
	free_slot = FIXED_CLAIMS;
	end = request->paddr + request->size;

	/* Keeps claim inspection and publication interrupt-atomic. */
	irq_enabled = hal_irq_disable();

	/* Finds an empty registry slot while rejecting overlap. */
	for (i = 0; i < FIXED_CLAIMS; i++) {
		/* Remembers the first unused registry slot. */
		if (fixed_claims[i].size == 0) {
			/* Preserves the earliest unused slot. */
			if (free_slot == FIXED_CLAIMS)
				free_slot = i;
			continue;
		}

		/* Rejects overlap with this existing fixed claim. */
		claim_end = fixed_claims[i].paddr + fixed_claims[i].size;

		/* Reports an interval intersection with the existing claim. */
		if (request->paddr < claim_end &&
		    end > fixed_claims[i].paddr) {
			/* Restores interrupts before reporting the overlap. */
			if (irq_enabled)
				hal_irq_enable();
			return HAL_ERR_BUSY;
		}
	}

	/* Reports exhaustion of the fixed-size claim registry. */
	if (free_slot == FIXED_CLAIMS) {
		/* Restores interrupts before reporting registry exhaustion. */
		if (irq_enabled)
			hal_irq_enable();
		return HAL_ERR_NOMEM;
	}

	/* Publishes the fixed direct-mapped descriptor and returns its copy. */
	fixed_claims[free_slot].vaddr =
	    (void *)((uintptr_t)request->paddr | SYS_START);
	fixed_claims[free_slot].paddr = request->paddr;
	fixed_claims[free_slot].size = request->size;
	fixed_claims[free_slot].type = request->type;
	fixed_claims[free_slot].attr = request->attr;
	*desc = fixed_claims[free_slot];

	/* Restores interrupts only when they were previously enabled. */
	if (irq_enabled)
		hal_irq_enable();

	/* Reports a successful fixed-range claim. */
	return HAL_OK;
}

/* Validates and dispatches one physical-memory allocation request. */
static int
pmem_alloc_unlocked(
	const struct hal_pmem_request *request,
	struct hal_pmem *desc)
{
	struct hal_pmem result;
	size_t alignment;
	int error;

	/* Rejects missing, empty, or unsupported allocation requests. */
	if (request == NULL || desc == NULL || request->size == 0 ||
	    (request->attr & ~(HAL_PMEM_ATTR_NOCACHE |
	    HAL_PMEM_ATTR_WRITETHRU)) != 0) {
		return HAL_ERR_INVALID;
	}

	/* Selects and validates the requested page alignment. */
	alignment = request->alignment == 0 ? PAGE_SIZE : request->alignment;

	/* Rejects a sub-page or non-power-of-two alignment. */
	if (alignment < PAGE_SIZE || (alignment & (alignment - 1U)) != 0)
		return HAL_ERR_INVALID;

	/* Dispatches ordinary RAM through the contiguous page allocator. */
	if (request->type == HAL_PMEM_TYPE_RAM) {
		/* Requires unspecified placement and cacheable RAM attributes. */
		if (request->paddr != HAL_PMEM_PADDR_ANY || request->attr != 0)
			return HAL_ERR_INVALID;
		error = alloc_ram(request->size, alignment, desc);
		return error;
	}

	/* Validates a fixed MMIO or video-memory claim. */
	if ((request->type != HAL_PMEM_TYPE_MMIO &&
	    request->type != HAL_PMEM_TYPE_VRAM) ||
	    request->paddr == HAL_PMEM_PADDR_ANY ||
	    request->paddr > UINT32_MAX - request->size ||
	    (request->paddr & (alignment - 1U)) != 0 ||
	    !valid_fixed_window(request->paddr, request->size)) {
		return HAL_ERR_INVALID;
	}

	/* Retains the existing descriptor read before claiming fixed memory. */
	result = *desc;
	(void)result;

	/* Claims and reports the fixed device interval. */
	error = claim_fixed(request, desc);

	/* Returns the fixed-claim result. */
	return error;
}

/* Validates and releases one physical-memory descriptor. */
static int
pmem_free_unlocked(
	struct hal_pmem *desc)
{
	uint32_t start_page;
	uint32_t end_page;
	uint32_t i;
	unsigned slot;
	bool irq_enabled;

	/* Rejects an empty descriptor. */
	if (desc == NULL || desc->size == 0)
		return HAL_ERR_INVALID;

	/* Releases a fixed MMIO or video-memory claim by exact identity. */
	if (desc->type != HAL_PMEM_TYPE_RAM) {
		irq_enabled = hal_irq_disable();

		/* Finds the exact registered fixed claim. */
		for (slot = 0; slot < FIXED_CLAIMS; slot++) {
			/* Stops at a descriptor with identical ownership fields. */
			if (fixed_claims[slot].vaddr == desc->vaddr &&
			    fixed_claims[slot].paddr == desc->paddr &&
			    fixed_claims[slot].size == desc->size &&
			    fixed_claims[slot].type == desc->type) {
				break;
			}
		}

		/* Rejects a descriptor absent from the claim registry. */
		if (slot == FIXED_CLAIMS) {
			/* Restores interrupts before reporting the stale descriptor. */
			if (irq_enabled)
				hal_irq_enable();
			return HAL_ERR_STATE;
		}

		/* Clears the registered fixed claim. */
		hal_memset(
			&fixed_claims[slot],
			0,
			sizeof(fixed_claims[slot]));

		/* Restores prior interrupts before clearing the caller descriptor. */
		if (irq_enabled)
			hal_irq_enable();
		hal_memset(desc, 0, sizeof(*desc));

		/* Reports a released fixed claim. */
		return HAL_OK;
	}

	/* Validates a direct-mapped, page-aligned RAM descriptor. */
	if (desc->vaddr != (void *)((uintptr_t)desc->paddr | SYS_START) ||
	    (desc->paddr & (PAGE_SIZE - 1U)) != 0 ||
	    (desc->size & (PAGE_SIZE - 1U)) != 0) {
		return HAL_ERR_INVALID;
	}
	start_page = (uint32_t)desc->paddr >> 12;
	end_page = start_page + (desc->size >> 12);

	/* Rejects a page interval outside managed physical memory. */
	if (start_page >= phys_pages || end_page > phys_pages)
		return HAL_ERR_INVALID;

	/* Keeps RAM validation and bitmap release interrupt-atomic. */
	irq_enabled = hal_irq_disable();

	/* Verifies ownership of every page before changing the bitmap. */
	for (i = start_page; i < end_page; i++) {
		/* Rejects the first page absent from the allocation bitmap. */
		if (PAGEMAP_GET(i) == 0) {
			/* Restores interrupts before reporting inconsistent ownership. */
			if (irq_enabled)
				hal_irq_enable();
			return HAL_ERR_STATE;
		}
	}

	/* Rejects an allocation-accounting underflow. */
	if (allocated_pages < end_page - start_page) {
		/* Restores interrupts before reporting inconsistent accounting. */
		if (irq_enabled)
			hal_irq_enable();
		return HAL_ERR_STATE;
	}

	/* Releases and accounts for every page in the descriptor. */
	for (i = start_page; i < end_page; i++)
		PAGEMAP_RESET(i);
	allocated_pages -= end_page - start_page;

	/* Restores prior interrupts before clearing the caller descriptor. */
	if (irq_enabled)
		hal_irq_enable();
	hal_memset(desc, 0, sizeof(*desc));

	/* Reports a released RAM allocation. */
	return HAL_OK;
}
