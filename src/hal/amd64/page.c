/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The amd64 physical-page allocator and fixed physical mapping registry.
 */

#include <hal/hal.h>
#include "defs.h"
#include "asm.h"
#include "space.h"
#include "image.h"
#include "pmem-range.h"
#include "bsp.h"
#include "bootloader/include/amd64-handoff.h"

#define MAX_PHYS_PAGES (AMD64_BOOTSTRAP_LIMIT / PAGE_SIZE)
#define BITMAP_WORDS   (MAX_PHYS_PAGES / 32U)
#define BIT_GET(n)     (page_bitmap[(n) >> 5] & (1U << ((n) & 31U)))
#define BIT_SET(n)     (page_bitmap[(n) >> 5] |= (1U << ((n) & 31U)))
#define BIT_CLEAR(n)   (page_bitmap[(n) >> 5] &= ~(1U << ((n) & 31U)))

static uint32_t page_bitmap[BITMAP_WORDS];
static uint32_t reserved_bitmap[BITMAP_WORDS];
struct early_reservation {
	uint64_t base;
	uint64_t size;
};

/* A normalized range and each boot owner can split the monotonic arena. */
static struct early_reservation early_reservations[1024];
static uint32_t early_reservation_count;
static uint32_t early_low_cursor = 0x100000U / PAGE_SIZE;
static uint64_t early_high_cursor = AMD64_BOOTSTRAP_LIMIT;
static uint32_t early_low_pages;
#ifndef WS025_EARLY_LOW_PAGES
#define WS025_EARLY_LOW_PAGES UINT32_MAX
#endif

static struct amd64_pmem_extent ram_extents[ZBL6_MAX_MEMORY_RANGES];
static uint32_t ram_extent_count;
static uint32_t ram_extent_type[ZBL6_MAX_MEMORY_RANGES];
static int boot_memory_released;
static uint64_t managed_pages;
static uint64_t metadata_bytes;
static int range_ready;

/*
 * Set when every managed RAM extent was found in the direct map at boot.
 *
 * hal_pmem_to_kernel() then converts an address inside an extent by
 * offset alone instead of walking the RAM map for every call.
 */
static int extents_direct_mapped;
static uint32_t phys_pages;
static uint64_t boot_usable_bytes;
static uint64_t boot_reclaim_bytes;
static uint64_t boot_highest_end;
static uint64_t boot_usable_highest_end;
static uint64_t allocator_initial_bytes;
static uint32_t reserved_pages;
static uint32_t allocated_pages;
static volatile unsigned pmem_lock;
static uint64_t pmem_entered_cycles;
static uint64_t pmem_max_irqoff_cycles;

/*
 * A stack of single pages freed most recently, handed out again first.
 *
 * The extent allocator is next-fit and walks forward through free memory,
 * so without this a stream of page allocations and frees touches pages
 * that were never used since boot: cold in every cache and, under a
 * hypervisor, not yet backed by host memory.  The stack keeps the pages
 * last freed, which are the warmest.  It holds only unconstrained
 * single-page runs, is filled by hal_pmem_free() and drained by
 * hal_pmem_alloc(), and is protected by pmem_lock.  Its pages count as
 * allocated in the extents and as free in hal_get_memstat().
 */
#define PMEM_PAGE_STACK_MAX 8192U
static hal_physaddr_t pmem_page_stack[PMEM_PAGE_STACK_MAX];
static uint32_t pmem_page_stack_count;

void hal_amd64_space_memory_stats(uint32_t *count, uint32_t *page_tables);

static uint64_t pmem_cycles(void);
static bool pmem_lock_enter(void);
static void pmem_lock_leave(bool enabled);
static void release_usable_range(uint64_t base, uint64_t size);
static void reserve_range(uintptr_t address, size_t size);
static int record_early_page(uint64_t physical);
static int record_early_run(uint64_t physical, uint64_t size);
static void *early_metadata(uint64_t size);
static void reserve_managed(uint64_t base, uint64_t size);
static int alloc_range(size_t size, size_t alignment, uint64_t minimum,
    uint64_t maximum, uint64_t boundary, hal_physaddr_t *paddr);
static int boot_page_retained(uint64_t physical);
static int boot_page_retired(uint64_t physical);
static uint64_t next_retired_page(uint64_t physical, uint64_t end);
static int alloc_ram(size_t size, size_t alignment, hal_physaddr_t *paddr);
static int free_ram(hal_physaddr_t paddr, size_t size);
static int pmem_alloc_unlocked(size_t size, size_t alignment,
    uint64_t maximum, uint64_t boundary, hal_physaddr_t *paddr);
static int pmem_free_unlocked(hal_physaddr_t *block, size_t size);
static int extents_in_direct_map(void);
static struct amd64_pmem_extent *extent_of_page(hal_physaddr_t paddr, uint64_t *index);
static int page_stack_push(hal_physaddr_t paddr);
static int page_stack_pop(hal_physaddr_t *paddr);
static void page_stack_drain(void);
static int pmem_alloc_or_drain(size_t size, size_t alignment, uint64_t maximum, uint64_t boundary, hal_physaddr_t *paddr);

/*
 * Initializes the amd64 physical-memory allocation maps.
 */
void
prekern_amd64_page_init(
	void)
{
	const struct zbl6_framebuffer *framebuffer;
	uint64_t total;
	uint64_t base;
	uint64_t size;
	uint32_t index;
	uint32_t type;

	/* Distinguishes legacy geometry from complete typed firmware reporting. */
	if (bsp_memory_source() == 0)
		hal_puts("A64 MEMORY legacy degraded: boot ownership/attributes unavailable\n");

	/* Bounds the bootstrap allocator to its initial direct physical mapping. */
	total = bsp_mem_probe();
	if (total / PAGE_SIZE > MAX_PHYS_PAGES)
		phys_pages = MAX_PHYS_PAGES;
	else
		phys_pages = (uint32_t)(total / PAGE_SIZE);

	/* Starts with every addressable page reserved. */
	hal_memset(page_bitmap, 0xff, sizeof(page_bitmap));
	hal_memset(reserved_bitmap, 0xff, sizeof(reserved_bitmap));
	reserved_pages = phys_pages;
	allocated_pages = 0;

	/* Releases every firmware range classified as usable RAM. */
	for (index = 0; index < bsp_mem_range_count(); index++) {
		/* Requires each advertised range to remain retrievable. */
		if (!bsp_mem_range(index, &base, &size, &type))
			HAL_FATAL("invalid BSP memory range index");
		if (size > UINT64_MAX - base)
			HAL_FATAL("overflowing BSP memory range");
		if (base + size > boot_highest_end)
			boot_highest_end = base + size;
		if (type == ZBL6_MEMORY_USABLE) {
			if (size > UINT64_MAX - boot_usable_bytes)
				HAL_FATAL("overflowing BSP usable RAM sum");
			boot_usable_bytes += size;
			if (base + size > boot_usable_highest_end)
				boot_usable_highest_end = base + size;
		}
		if (type == ZBL6_MEMORY_BOOT_RECLAIM) {
			if (size > UINT64_MAX - boot_reclaim_bytes)
				HAL_FATAL("overflowing BSP boot reclaim sum");
			boot_reclaim_bytes += size;
		}

		/* Releases only ranges explicitly owned by the RAM allocator. */
		if (type == ZBL6_MEMORY_USABLE)
			release_usable_range(base, size);
	}

	/* Reserves firmware space, the loaded kernel image and, behind a
	 * relocated image, the linked range that the bootstrap window no
	 * longer reaches (early table pages are used through that window). */
	reserve_range(0, 0x00100000U);
	reserve_range((uintptr_t)amd64_kernel_image()->phys_start,
	    (size_t)(amd64_kernel_image()->phys_end - amd64_kernel_image()->phys_start));
	if (amd64_kernel_image()->relocated)
		reserve_range((uintptr_t)amd64_kernel_image()->shadow_start,
		    (size_t)(amd64_kernel_image()->shadow_end - amd64_kernel_image()->shadow_start));
	for (index = 0; bsp_boot_allocation(index, &base, &size); index++)
		reserve_range((uintptr_t)base, (size_t)size);

	framebuffer = hal_get_arch_handoff("pcat.framebuffer");
	if (framebuffer != NULL)
		reserve_range((uintptr_t)framebuffer->physical_base, (size_t)framebuffer->size);

	allocator_initial_bytes = (uint64_t)(phys_pages - reserved_pages) * PAGE_SIZE;
	hal_printf("A64 MEMORY source=%u usable=%llu boot_reclaim=%llu highest_usable=%llu allocator=%llu\n",
	    bsp_memory_source(), (unsigned long long)boot_usable_bytes,
	    (unsigned long long)boot_reclaim_bytes,
	    (unsigned long long)boot_usable_highest_end,
	    (unsigned long long)allocator_initial_bytes);
}

/*
 * Reserves a bootstrap-reachable table page before normal allocation starts.
 * This transfers a free bit directly to reserved ownership and uses no heap.
 */
int
amd64_early_table_page(
	uint64_t *physical,
	int mapped)
{
	uint64_t base;
	uint64_t size;
	uint64_t end;
	uint64_t owner_base;
	uint64_t owner_size;
	uint64_t candidate;
	uint32_t index;
	uint32_t owner;
	uint32_t type;
	int changed;

	if (physical == NULL)
		return 0;
	while (early_low_cursor < phys_pages && early_low_pages < WS025_EARLY_LOW_PAGES) {
		index = early_low_cursor++;
		if (BIT_GET(index))
			continue;
		*physical = (uint64_t)index * PAGE_SIZE;
		if (!record_early_page(*physical))
			return 0;
		BIT_SET(index);
		reserved_bitmap[index >> 5] |= 1U << (index & 31U);
		reserved_pages++;
		early_low_pages++;
		allocator_initial_bytes -= PAGE_SIZE;
		return 1;
	}
	/* A high arena is available only after an explicit partial CR3 switch. */
	if (!mapped)
		return 0;
	for (index = 0; index < bsp_mem_range_count(); index++) {
		if (!bsp_mem_range(index, &base, &size, &type) || type != ZBL6_MEMORY_USABLE)
			continue;
		end = base + size;
		candidate = base > early_high_cursor ? base : early_high_cursor;
		candidate = (candidate + PAGE_SIZE - 1U) & ~(uint64_t)(PAGE_SIZE - 1U);
		while (candidate < end && PAGE_SIZE <= end - candidate) {
			changed = 0;
			for (owner = 0; bsp_boot_allocation(owner, &owner_base, &owner_size); owner++) {
				if (candidate >= owner_base && candidate - owner_base < owner_size) {
					candidate = owner_base + owner_size;
					changed = 1;
					break;
				}
			}
			if (changed)
				continue;
			/* The translation checks a present leaf in the active RAM map. */
			if (amd64_phys_to_direct((uintptr_t)candidate) == NULL)
				return 0;
			if (!record_early_page(candidate))
				return 0;
			*physical = candidate;
			early_high_cursor = candidate + PAGE_SIZE;
			return 1;
		}
	}
	return 0;
}

/* Returns arena ownership for transfer to the range-based allocator. */
int
amd64_early_reservation(uint32_t index, uint64_t *physical, uint64_t *size)
{
	if (index >= early_reservation_count || physical == NULL || size == NULL)
		return 0;
	*physical = early_reservations[index].base;
	*size = early_reservations[index].size;
	return 1;
}

/* Records monotonic arena runs without allocating bookkeeping memory. */
static int
record_early_page(uint64_t physical)
{
	return record_early_run(physical, PAGE_SIZE);
}

/* Records one complete metadata or table run with explicit ownership. */
static int
record_early_run(uint64_t physical, uint64_t size)
{
	struct early_reservation *last;

	if (early_reservation_count != 0) {
		last = &early_reservations[early_reservation_count - 1U];
		if (last->base + last->size == physical &&
		    (last->base < AMD64_BOOTSTRAP_LIMIT) == (physical < AMD64_BOOTSTRAP_LIMIT)) {
			last->size += size;
			return 1;
		}
	}
	if (early_reservation_count == sizeof(early_reservations) / sizeof(early_reservations[0]))
		return 0;
	last = &early_reservations[early_reservation_count++];
	last->base = physical;
	last->size = size;
	return 1;
}

/*
 * Allocates one physical-memory descriptor under the allocator lock.
 */
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
	int single;

	/* A single page with no alignment beyond a page may come from the stack. */
	single = 0;
	if (block != NULL && req_size != 0 && req_size <= PAGE_SIZE &&
	    req_align <= PAGE_SIZE)
		single = 1;

	/* Takes the page freed most recently when the stack has one. */
	enabled = pmem_lock_enter();
	error = HAL_ERR_NOMEM;
	if (single)
		error = page_stack_pop(block);
	if (error != HAL_OK)
		error = pmem_alloc_or_drain(req_size, req_align, UINT64_MAX, 0, block);
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
	error = pmem_alloc_or_drain(
		req_size,
		req_align,
		(uint64_t)max_paddr,
		(uint64_t)boundary,
		block);
	pmem_lock_leave(enabled);

	/* Returns the allocation result unchanged. */
	return error;
}

/*
 * Frees one physical-memory descriptor under the allocator lock.
 */
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
	int single;
	int stacked;

	/* A whole single page is kept on the stack for the next allocation. */
	single = 0;
	if (block != NULL && size != 0 && size <= PAGE_SIZE)
		single = 1;

	/* Stacks the page, or gives the run back to its extent. */
	enabled = pmem_lock_enter();
	stacked = 0;
	if (single)
		stacked = page_stack_push(*block);
	if (stacked) {
		*block = 0;
		error = HAL_OK;
	} else {
		error = pmem_free_unlocked(block, size);
	}

	/* Leaves the allocator with the page placed. */
	pmem_lock_leave(enabled);

	/* Returns the release result unchanged. */
	return error;
}

/*
 * Translates a physical RAM address to its direct-map address.
 */
void *
hal_pmem_to_kernel(
	hal_physaddr_t paddr)
{
	struct amd64_pmem_extent *extent;
	uint32_t index;

	/* Converts an address inside a verified managed extent by offset. */
	if (extents_direct_mapped) {
		for (index = 0; index < ram_extent_count; index++) {
			extent = &ram_extents[index];
			if (paddr < extent->base ||
			    paddr - extent->base >= extent->pages * PAGE_SIZE)
				continue;
			return (void *)((uintptr_t)AMD64_DIRECT_BASE + (uintptr_t)paddr);
		}
	}

	/* RAM is direct-mapped; anything else has no kernel alias. */
	return amd64_phys_to_direct((uintptr_t)paddr);
}

/*
 * Tests whether the direct map aliases every managed extent.
 *
 * Runs once at boot, before the allocator is published; an extent with any
 * page outside the direct map leaves every conversion on the checked path.
 */
static int
extents_in_direct_map(
	void)
{
	struct amd64_pmem_extent *extent;
	uint64_t end;
	uint32_t index;
	int covered;

	/* Checks every extent page by page. */
	for (index = 0; index < ram_extent_count; index++) {
		extent = &ram_extents[index];
		end = extent->base + extent->pages * PAGE_SIZE;
		covered = amd64_direct_covers(extent->base, end);
		if (!covered)
			return 0;
	}

	/* Reports that the offset conversion is safe for every extent. */
	return 1;
}

/*
 * Reports the total physical memory managed by this allocator.
 */
size_t
hal_pmem_get_total_size(
	void)
{
	/* Returns the direct-map-limited page capacity. */
	return (size_t)managed_pages * PAGE_SIZE;
}

/*
 * Supplies zero task statistics when the task module is absent.
 */
void __attribute__((weak))
hal_amd64_task_memory_stats(
	uint32_t *count,
	size_t *stack_bytes)
{
	/* Clears each result requested by the caller. */
	if (count != NULL)
		*count = 0;
	if (stack_bytes != NULL)
		*stack_bytes = 0;
}

/*
 * Reports the current amd64 HAL memory accounting.
 */
void
hal_get_memstat(
	struct hal_memstat *stats)
{
	bool enabled;
	uint32_t index;

	/* Ignores an absent result buffer. */
	if (stats == NULL)
		return;

	/* Snapshots physical and subsystem accounting under the page lock. */
	enabled = pmem_lock_enter();
	hal_memset(stats, 0, sizeof(*stats));
	stats->boot_ranges_valid = 1;
	stats->boot_range_count = bsp_mem_range_count();
	stats->boot_usable_bytes = boot_usable_bytes;
	stats->boot_highest_end = boot_highest_end;
	stats->boot_usable_highest_end = boot_usable_highest_end;
	stats->direct_mapped_bytes = amd64_direct_mapped_bytes();
	stats->allocator_initial_bytes = allocator_initial_bytes;
	stats->boot_reclaim_bytes = boot_reclaim_bytes;
	stats->boot_memory_source = bsp_memory_source();
	stats->allocator_metadata_bytes = metadata_bytes;
	stats->allocator_max_irqoff_cycles = pmem_max_irqoff_cycles;
	stats->physical_total = (size_t)managed_pages * PAGE_SIZE;
	for (index = 0; index < ram_extent_count; index++) {
		stats->physical_reserved += (size_t)ram_extents[index].reserved_pages * PAGE_SIZE;
		stats->physical_allocated += (size_t)ram_extents[index].allocated_pages * PAGE_SIZE;
		stats->physical_free += (size_t)ram_extents[index].free_pages * PAGE_SIZE;
		stats->allocator_scan_words += ram_extents[index].scanned_words;
		if (ram_extents[index].max_scan_words > stats->allocator_max_extent_scan_words)
			stats->allocator_max_extent_scan_words = ram_extents[index].max_scan_words;
	}

	/* Pages waiting on the stack are free to the caller, not allocated. */
	stats->physical_free += (size_t)pmem_page_stack_count * PAGE_SIZE;
	stats->physical_allocated -= (size_t)pmem_page_stack_count * PAGE_SIZE;

	hal_amd64_task_memory_stats(
		&stats->task_count,
		&stats->task_stack_bytes);
	hal_amd64_space_memory_stats(
		&stats->space_count,
		&stats->page_table_count);
	pmem_lock_leave(enabled);
}

/* Acquires the physical-memory lock with local interrupts disabled. */
static bool
pmem_lock_enter(
	void)
{
	bool enabled;
	uint64_t entered;

	/* Preserves interrupt state and acquires the global spin lock. */
	enabled = hal_irq_disable();
	entered = pmem_cycles();
	while (__atomic_exchange_n(&pmem_lock, 1U, __ATOMIC_ACQUIRE) != 0)
		__asm__ volatile("pause");

	pmem_entered_cycles = entered;

	/* Returns whether interrupts must be restored on release. */
	return enabled;
}

/* Releases the physical-memory lock and restores interrupt state. */
static void
pmem_lock_leave(
	bool enabled)
{
	uint64_t elapsed;

	elapsed = pmem_cycles() - pmem_entered_cycles;
	if (elapsed > pmem_max_irqoff_cycles)
		pmem_max_irqoff_cycles = elapsed;

	/* Publishes all protected writes before unlocking. */
	__atomic_store_n(&pmem_lock, 0U, __ATOMIC_RELEASE);

	/* Restores interrupts only when they were originally enabled. */
	if (enabled)
		hal_irq_enable();
}

/* Releases usable pages from the initially reserved bitmap. */
static void
release_usable_range(
	uint64_t base,
	uint64_t size)
{
	uint64_t limit;
	uint64_t end;
	uint32_t first;
	uint32_t last;
	uint32_t index;

	/* Rejects empty ranges and ranges beyond the managed limit. */
	limit = (uint64_t)phys_pages * PAGE_SIZE;
	if (size == 0 || base >= limit)
		return;

	/* Clips and rounds the range inward to complete pages. */
	if (size > limit - base)
		end = limit;
	else
		end = base + size;
	first = (uint32_t)((base + PAGE_SIZE - 1U) / PAGE_SIZE);
	last = (uint32_t)(end / PAGE_SIZE);

	/* Marks each newly usable page free. */
	for (index = first; index < last; index++) {
		/* Releases the page only when it is still reserved. */
		if (BIT_GET(index)) {
			BIT_CLEAR(index);
			reserved_bitmap[index >> 5] &= ~(1U << (index & 31U));
			reserved_pages--;
		}
	}
}

/* Reserves every page touched by a physical range. */
static void
reserve_range(
	uintptr_t address,
	size_t size)
{
	uintptr_t limit;
	uintptr_t end;
	uint32_t first;
	uint32_t last;
	uint32_t index;

	/* Rejects empty ranges and ranges beyond the managed limit. */
	limit = (uintptr_t)phys_pages * PAGE_SIZE;
	if (size == 0 || address >= limit)
		return;

	/* Clips and rounds the range outward to every touched page. */
	if (size > limit - address)
		end = limit;
	else
		end = address + size;
	first = (uint32_t)(address / PAGE_SIZE);
	if (end == limit)
		last = phys_pages;
	else
		last = (uint32_t)((end + PAGE_SIZE - 1U) / PAGE_SIZE);
	if (last > phys_pages)
		last = phys_pages;

	/* Marks each newly reserved page unavailable. */
	for (index = first; index < last; index++) {
		/* Reserves the page only when it is still free. */
		if (!BIT_GET(index)) {
			BIT_SET(index);
			reserved_bitmap[index >> 5] |= 1U << (index & 31U);
			reserved_pages++;
		}
	}
}

/* Allocates RAM through the published range geometry. */
/* Allocates ordinary RAM, preferring memory above the DMA32 pool. */
static int
alloc_ram(
	size_t size,
	size_t alignment,
	hal_physaddr_t *paddr)
{
	int result;

	/* Preserves DMA32 capacity even when one RAM extent crosses 4 GiB. */
	result = alloc_range(size, alignment, UINT64_C(0x100000000),
	    AMD64_ALLOCATOR_LIMIT - 1U, 0, paddr);
	if (result != HAL_ERR_NOMEM)
		return result;

	/* Uses lower RAM when the high pool cannot satisfy the whole run. */
	return alloc_range(size, alignment, 0, UINT32_MAX, 0, paddr);
}

/* Frees exactly one range-owned allocation under the outer allocator lock. */
/* Releases one RAM run back to the extent which owns it. */
static int
free_ram(
	hal_physaddr_t paddr,
	size_t size)
{
	struct amd64_pmem_extent *extent;
	uint32_t index;
	enum amd64_pmem_result result;

	/* Requires a published extent set and a non-empty run. */
	if (!range_ready || size == 0)
		return HAL_ERR_INVALID;

	/* Finds the extent which contains this run. */
	for (index = 0; index < ram_extent_count; index++) {
		extent = &ram_extents[index];
		if (paddr < extent->base ||
		    paddr - extent->base >= extent->pages * PAGE_SIZE)
			continue;

		/* Releases the run, splitting the owning block as needed. */
		result = amd64_pmem_extent_free(extent, paddr, size);
		if (result != AMD64_PMEM_OK)
			return result == AMD64_PMEM_STATE ? HAL_ERR_STATE
							  : HAL_ERR_INVALID;
		return HAL_OK;
	}

	/* Reports a run outside every managed extent. */
	return HAL_ERR_INVALID;
}

/* Resolves a supported fixed physical address to its virtual window. */
/* Claims one supported fixed MMIO or VRAM mapping. */
/* Selects the allocation path for a validated physical-memory request. */
/* Validates and performs one RAM allocation under the allocator lock. */
static int
pmem_alloc_unlocked(
	size_t size,
	size_t alignment,
	uint64_t maximum,
	uint64_t boundary,
	hal_physaddr_t *paddr)
{
	/* Rejects an empty request or a missing destination. */
	if (size == 0 || paddr == NULL)
		return HAL_ERR_INVALID;

	/* Rounds to whole pages so release can repeat the same arithmetic. */
	if (size > SIZE_MAX - (PAGE_SIZE - 1U))
		return HAL_ERR_INVALID;
	size = (size + PAGE_SIZE - 1U) & ~(size_t)(PAGE_SIZE - 1U);

	/* Applies and validates the requested physical alignment. */
	if (alignment == 0)
		alignment = PAGE_SIZE;
	if (alignment < PAGE_SIZE || (alignment & (alignment - 1U)) != 0)
		return HAL_ERR_INVALID;

	/* Requires a power-of-two segment boundary when one is given. */
	if (boundary != 0 && (boundary & (boundary - 1U)) != 0)
		return HAL_ERR_INVALID;

	/* Uses the unconstrained path when the device can reach all RAM. */
	if (maximum == UINT64_MAX && boundary == 0)
		return alloc_ram(size, alignment, paddr);

	/* Searches only the range the device can reach. */
	return alloc_range(size, alignment, 0, maximum, boundary, paddr);
}

/* Releases a physical-memory descriptor without taking the outer lock. */
/* Releases one RAM run under the allocator lock. */
static int
pmem_free_unlocked(
	hal_physaddr_t *block,
	size_t size)
{
	int error;

	/* Rejects an absent handle or an empty run. */
	if (block == NULL || size == 0)
		return HAL_ERR_INVALID;

	/* Repeats the rounding hal_pmem_alloc() applied. */
	if (size > SIZE_MAX - (PAGE_SIZE - 1U))
		return HAL_ERR_INVALID;
	size = (size + PAGE_SIZE - 1U) & ~(size_t)(PAGE_SIZE - 1U);

	/* Releases the run and retires the caller's handle. */
	error = free_ram(*block, size);
	if (error != HAL_OK)
		return error;
	*block = 0;
	return HAL_OK;
}

/*
 * Transfers typed RAM and early ownership to extent-sized runtime metadata.
 */
void
prekern_amd64_range_page_init(void)
{
	uint64_t base;
	uint64_t size;
	uint64_t bytes;
	uint64_t end;
	uint64_t free_pages;
	uint32_t index;
	uint32_t type;
	void *metadata;
	const struct zbl6_framebuffer *framebuffer;

	if (range_ready || allocated_pages != 0)
		HAL_FATAL("amd64 range allocator transition repeated or too late");
	for (index = 0; index < bsp_mem_range_count(); index++) {
		if (!bsp_mem_range(index, &base, &size, &type))
			HAL_FATAL("amd64 allocator range disappeared");
		if ((type != ZBL6_MEMORY_USABLE && type != ZBL6_MEMORY_BOOT_RECLAIM) ||
		    base >= AMD64_ALLOCATOR_LIMIT)
			continue;
		end = base + size;
		if (end > AMD64_ALLOCATOR_LIMIT)
			end = AMD64_ALLOCATOR_LIMIT;
		base = (base + PAGE_SIZE - 1U) & ~(uint64_t)(PAGE_SIZE - 1U);
		end &= ~(uint64_t)(PAGE_SIZE - 1U);
		if (base >= end)
			continue;
		size = end - base;
		bytes = amd64_pmem_metadata_size(size / PAGE_SIZE);
		metadata = early_metadata(bytes);
		if (metadata == NULL || ram_extent_count == ZBL6_MAX_MEMORY_RANGES ||
		    amd64_pmem_extent_init(&ram_extents[ram_extent_count], base, size, metadata, bytes) != AMD64_PMEM_OK)
			HAL_FATAL("amd64 RAM extent metadata allocation failed");
		metadata_bytes += (bytes + PAGE_SIZE - 1U) & ~(uint64_t)(PAGE_SIZE - 1U);
		managed_pages += size / PAGE_SIZE;
		ram_extent_type[ram_extent_count] = type;
		if (type == ZBL6_MEMORY_BOOT_RECLAIM &&
		    amd64_pmem_reserve(&ram_extents[ram_extent_count], base, size) != AMD64_PMEM_OK)
			HAL_FATAL("amd64 boot-reclaim reservation failed");
		ram_extent_count++;
	}
	reserve_managed(0, 0x100000U);
	reserve_managed(amd64_kernel_image()->phys_start,
	    amd64_kernel_image()->phys_end - amd64_kernel_image()->phys_start);
	if (amd64_kernel_image()->relocated)
		reserve_managed(amd64_kernel_image()->shadow_start,
		    amd64_kernel_image()->shadow_end - amd64_kernel_image()->shadow_start);
	for (index = 0; bsp_boot_allocation(index, &base, &size); index++)
		reserve_managed(base, size);
	for (index = 0; amd64_early_reservation(index, &base, &size); index++)
		reserve_managed(base, size);
	framebuffer = hal_get_arch_handoff("pcat.framebuffer");
	if (framebuffer != NULL)
		reserve_managed(framebuffer->physical_base, framebuffer->size);
	free_pages = 0;
	for (index = 0; index < ram_extent_count; index++)
		free_pages += ram_extents[index].free_pages;
	allocator_initial_bytes = free_pages * PAGE_SIZE;
	extents_direct_mapped = extents_in_direct_map();
	range_ready = 1;
	hal_printf("A64 RAM ALLOC extents=%u managed=%llu metadata=%llu free=%llu publication_limit=%llu\n",
	    ram_extent_count, (unsigned long long)(managed_pages * PAGE_SIZE),
	    (unsigned long long)metadata_bytes, (unsigned long long)allocator_initial_bytes,
	    (unsigned long long)AMD64_ALLOCATOR_LIMIT);
}

/* Allocates contiguous bootstrap metadata from already mapped, unowned RAM. */
static void *
early_metadata(uint64_t size)
{
	uint64_t base;
	uint64_t bytes;
	uint64_t end;
	uint64_t candidate;
	uint64_t owner_base;
	uint64_t owner_size;
	uint64_t collision_end;
	uint32_t index;
	uint32_t owner;
	uint32_t type;
	unsigned pass;
	const struct zbl6_framebuffer *framebuffer;

	if (size == 0 || size > UINT64_MAX - (PAGE_SIZE - 1U))
		return NULL;
	size = (size + PAGE_SIZE - 1U) & ~(uint64_t)(PAGE_SIZE - 1U);
	framebuffer = hal_get_arch_handoff("pcat.framebuffer");
	/* Metadata prefers high RAM so it does not consume low DMA capacity. */
	for (pass = 0; pass < 2; pass++) {
		for (index = 0; index < bsp_mem_range_count(); index++) {
			if (!bsp_mem_range(index, &base, &bytes, &type) || type != ZBL6_MEMORY_USABLE)
				continue;
			end = base + bytes;
			candidate = base < 0x100000U ? 0x100000U : base;
			if (pass == 0 && candidate < AMD64_BOOTSTRAP_LIMIT)
				candidate = AMD64_BOOTSTRAP_LIMIT;
			if (pass != 0 && end > AMD64_BOOTSTRAP_LIMIT)
				end = AMD64_BOOTSTRAP_LIMIT;
			candidate = (candidate + PAGE_SIZE - 1U) & ~(uint64_t)(PAGE_SIZE - 1U);
			while (candidate < end && size <= end - candidate) {
				collision_end = candidate;
				if (candidate < amd64_kernel_image()->phys_end && candidate + size > amd64_kernel_image()->phys_start)
					collision_end = amd64_kernel_image()->phys_end;
				if (amd64_kernel_image()->relocated &&
				    candidate < amd64_kernel_image()->shadow_end && candidate + size > amd64_kernel_image()->shadow_start &&
				    collision_end < amd64_kernel_image()->shadow_end)
					collision_end = amd64_kernel_image()->shadow_end;
				if (framebuffer != NULL && candidate < framebuffer->physical_base + framebuffer->size &&
				    candidate + size > framebuffer->physical_base)
					collision_end = framebuffer->physical_base + framebuffer->size;
				for (owner = 0; bsp_boot_allocation(owner, &owner_base, &owner_size); owner++)
					if (candidate < owner_base + owner_size && candidate + size > owner_base &&
					    collision_end < owner_base + owner_size)
						collision_end = owner_base + owner_size;
				for (owner = 0; amd64_early_reservation(owner, &owner_base, &owner_size); owner++)
					if (candidate < owner_base + owner_size && candidate + size > owner_base &&
					    collision_end < owner_base + owner_size)
						collision_end = owner_base + owner_size;
				if (collision_end != candidate) {
					if (collision_end > UINT64_MAX - (PAGE_SIZE - 1U))
						return NULL;
					candidate = (collision_end + PAGE_SIZE - 1U) & ~(uint64_t)(PAGE_SIZE - 1U);
					continue;
				}
				if (amd64_phys_to_direct((uintptr_t)candidate) == NULL ||
				    amd64_phys_to_direct((uintptr_t)(candidate + size - 1U)) == NULL ||
				    !record_early_run(candidate, size))
					return NULL;
				reserve_range((uintptr_t)candidate, (size_t)size);
				return amd64_phys_to_direct((uintptr_t)candidate);
			}
		}
	}
	return NULL;
}

/* Intersects a boot reservation with each actual managed extent. */
static void
reserve_managed(uint64_t base, uint64_t size)
{
	struct amd64_pmem_extent *extent;
	uint64_t end;
	uint64_t first;
	uint64_t last;
	uint32_t index;

	if (size == 0 || size > UINT64_MAX - base || base + size > UINT64_MAX - (PAGE_SIZE - 1U))
		HAL_FATAL("amd64 allocator reservation overflow");
	end = (base + size + PAGE_SIZE - 1U) & ~(uint64_t)(PAGE_SIZE - 1U);
	base &= ~(uint64_t)(PAGE_SIZE - 1U);
	for (index = 0; index < ram_extent_count; index++) {
		extent = &ram_extents[index];
		first = base > extent->base ? base : extent->base;
		last = end < extent->base + extent->pages * PAGE_SIZE ? end : extent->base + extent->pages * PAGE_SIZE;
		if (first < last && amd64_pmem_reserve(extent, first, last - first) != AMD64_PMEM_OK)
			HAL_FATAL("amd64 allocator ownership conflict");
	}
}

/* Searches managed RAM inside the caller's physical constraints. */
/* Allocates one RAM run honouring the device reach and boundary limits. */
static int
alloc_range(
	size_t size,
	size_t alignment,
	uint64_t minimum,
	uint64_t maximum,
	uint64_t boundary,
	hal_physaddr_t *paddr)
{
	uint64_t physical;
	uint64_t allocated;
	uint32_t index;
	enum amd64_pmem_result result;

	/* Requires a published extent set and a destination. */
	if (!range_ready || paddr == NULL)
		return HAL_ERR_STATE;

	/* Searches every extent from the highest downwards. */
	for (index = ram_extent_count; index != 0; index--) {
		result = amd64_pmem_extent_alloc(
			&ram_extents[index - 1U],
			size,
			alignment,
			minimum,
			maximum,
			boundary,
			&physical,
			&allocated);
		if (result == AMD64_PMEM_NOMEM)
			continue;
		if (result != AMD64_PMEM_OK)
			return HAL_ERR_INVALID;

		/* Publishes the allocated physical run. */
		*paddr = (hal_physaddr_t)physical;
		return HAL_OK;
	}

	/* Reports an exhausted pool. */
	return HAL_ERR_NOMEM;
}


/* Samples local cycles; this metric includes lock waiting and is not wall time. */
static uint64_t
pmem_cycles(void)
{
	uint32_t low;
	uint32_t high;

	__asm__ volatile("lfence; rdtsc; lfence" : "=a"(low), "=d"(high) : : "memory");
	return ((uint64_t)high << 32) | low;
}

/*
 * Retires boot-owned RAM after ACPI discovery and CPU topology setup have completed.
 * Future AP startup uses the retained low trampoline and kernel-owned root.
 * Permanent owners override release lifetimes, including overlapping records.
 */
void
prekern_amd64_boot_memory_release(void)
{
	struct amd64_pmem_extent *extent;
	uint64_t physical;
	uint64_t end;
	uint64_t first;
	uint64_t released;
	uint32_t index;
	int eligible;
	bool enabled;

	if (!range_ready || boot_memory_released)
		HAL_FATAL("amd64 boot memory retirement out of order");
	amd64_acpi_finish_discovery();
	released = 0;

	/* Releases only contiguous retired runs, preserving all current owners. */
	for (index = 0; index < ram_extent_count; index++) {
		extent = &ram_extents[index];
		end = extent->base + extent->pages * PAGE_SIZE;
		first = end;
		for (physical = extent->base; physical <= end; physical += PAGE_SIZE) {
			eligible = 0;
			if (physical < end) {
				eligible = ram_extent_type[index] == ZBL6_MEMORY_BOOT_RECLAIM ||
				    boot_page_retired(physical);
				if (eligible && boot_page_retained(physical))
					eligible = 0;
			}
			/* Skips normal RAM between the few obsolete boot allocations. */
			if (!eligible && first == end && physical < end &&
			    ram_extent_type[index] != ZBL6_MEMORY_BOOT_RECLAIM) {
				physical = next_retired_page(physical, end) - PAGE_SIZE;
				continue;
			}
			if (eligible && first == end)
				first = physical;
			if (!eligible && first != end) {
				enabled = pmem_lock_enter();
				if (amd64_pmem_release_reserved(extent, first, physical - first) != AMD64_PMEM_OK)
					HAL_FATAL("amd64 boot retirement ownership conflict");
				pmem_lock_leave(enabled);
				released += physical - first;
				first = end;
			}
		}
	}
	boot_memory_released = 1;
	allocator_initial_bytes += released;
	hal_printf("A64 BOOT RECLAIM released=%llu bytes ACPI/AP owners retained\n",
	    (unsigned long long)released);
}

/* Determines whether an obsolete loader claim covers this whole page. */
static int
boot_page_retired(uint64_t physical)
{
	uint64_t base;
	uint64_t size;
	uint32_t index;
	uint32_t lifetime;

	/* Searches validated and page-aligned boot claims. */
	for (index = 0; bsp_boot_allocation(index, &base, &size); index++) {
		if (bsp_boot_allocation_lifetime(index, &lifetime) &&
		    lifetime != ZBL6_BOOT_KEEP && physical >= base && physical - base < size)
			return 1;
	}
	return 0;
}

/* Gives permanent owners precedence over an overlapping retired claim. */
static int
boot_page_retained(uint64_t physical)
{
	uint64_t base;
	uint64_t size;
	uint32_t index;
	uint32_t lifetime;
	const struct zbl6_framebuffer *framebuffer;

	if (physical < 0x100000U || amd64_kernel_image_owns(physical) ||
	    amd64_acpi_page_reserved(physical))
		return 1;
	framebuffer = hal_get_arch_handoff("pcat.framebuffer");
	if (framebuffer != NULL && physical < framebuffer->physical_base + framebuffer->size &&
	    physical + PAGE_SIZE > framebuffer->physical_base)
		return 1;

	/* Keeps the table arena and allocator metadata regardless of boot type. */
	for (index = 0; amd64_early_reservation(index, &base, &size); index++) {
		if (physical >= base && physical - base < size)
			return 1;
	}

	/* Keeps every explicit permanent loader record. */
	for (index = 0; bsp_boot_allocation(index, &base, &size); index++) {
		if (bsp_boot_allocation_lifetime(index, &lifetime) &&
		    lifetime == ZBL6_BOOT_KEEP && physical >= base && physical - base < size)
			return 1;
	}
	return 0;
}

/* Skips pages without a retired boot claim; called only outside a release run. */
static uint64_t
next_retired_page(uint64_t physical, uint64_t end)
{
	uint64_t base;
	uint64_t size;
	uint64_t next;
	uint32_t index;
	uint32_t lifetime;

	next = end;
	/* Finds the earliest later page that an obsolete claim could own. */
	for (index = 0; bsp_boot_allocation(index, &base, &size); index++) {
		if (!bsp_boot_allocation_lifetime(index, &lifetime) || lifetime == ZBL6_BOOT_KEEP)
			continue;
		if (physical >= base && physical - base < size)
			return physical + PAGE_SIZE;
		if (base > physical && base < next)
			next = base;
	}
	return next;
}

/* Finds the extent that manages a page and the page's index in it. */
static struct amd64_pmem_extent *
extent_of_page(
	hal_physaddr_t paddr,
	uint64_t *index)
{
	struct amd64_pmem_extent *extent;
	uint32_t position;

	/* Rejects an address that is not the start of a page. */
	if ((paddr & (PAGE_SIZE - 1U)) != 0)
		return NULL;

	/* Searches every published extent for the one holding the page. */
	for (position = 0; position < ram_extent_count; position++) {
		extent = &ram_extents[position];
		if (paddr < extent->base)
			continue;
		if ((paddr - extent->base) / PAGE_SIZE >= extent->pages)
			continue;

		/* Reports the extent and the page's position in its bitmaps. */
		*index = (paddr - extent->base) / PAGE_SIZE;
		return extent;
	}

	/* Reports a page outside managed RAM. */
	return NULL;
}

/*
 * Keeps one freed single page on the stack; the caller holds pmem_lock.
 *
 * The page must be exactly one live single-page allocation of an extent:
 * its used, head and tail bits are set and it is not reserved.  While the
 * page waits on the stack its head bit is cleared, so a second free of the
 * same page fails in the extent as a double free would today, and the bit
 * is set again when the page is handed out.
 */
static int
page_stack_push(
	hal_physaddr_t paddr)
{
	struct amd64_pmem_extent *extent;
	uint64_t index;
	uint64_t word;
	uint64_t bit;

	/* Leaves a full stack alone; the extent takes the page back. */
	if (pmem_page_stack_count >= PMEM_PAGE_STACK_MAX)
		return 0;

	/* Resolves the owning extent, or leaves an unknown page to the extent path. */
	extent = extent_of_page(paddr, &index);
	if (extent == NULL)
		return 0;

	/* Requires a live, unreserved, single-page allocation. */
	word = index / 64U;
	bit = UINT64_C(1) << (index % 64U);
	if ((extent->used[word] & bit) == 0)
		return 0;
	if ((extent->reserved[word] & bit) != 0)
		return 0;
	if ((extent->heads[word] & bit) == 0)
		return 0;
	if ((extent->tails[word] & bit) == 0)
		return 0;

	/* A cleared head bit marks the page as waiting on the stack. */
	extent->heads[word] &= ~bit;
	pmem_page_stack[pmem_page_stack_count] = paddr;
	pmem_page_stack_count++;

	/* Reports that the stack took the page. */
	return 1;
}

/* Hands out the page freed most recently; the caller holds pmem_lock. */
static int
page_stack_pop(
	hal_physaddr_t *paddr)
{
	struct amd64_pmem_extent *extent;
	uint64_t index;

	/* Reports an empty stack. */
	if (pmem_page_stack_count == 0)
		return HAL_ERR_NOMEM;

	/* Takes the top page and makes it a live allocation again. */
	pmem_page_stack_count--;
	*paddr = pmem_page_stack[pmem_page_stack_count];
	extent = extent_of_page(*paddr, &index);
	if (extent == NULL)
		HAL_FATAL("amd64 page stack holds a page outside managed RAM");
	extent->heads[index / 64U] |= UINT64_C(1) << (index % 64U);

	/* Succeeded: the caller owns the page. */
	return HAL_OK;
}

/*
 * Gives every stacked page back to its extent; the caller holds pmem_lock.
 *
 * The stacked pages count as free, but only a single unconstrained page
 * can be taken from the stack.  A larger or constrained allocation, such
 * as a device's DMA buffer, needs them back in the extents, or it fails
 * under memory pressure while the memory statistics report room.
 */
static void
page_stack_drain(
	void)
{
	struct amd64_pmem_extent *extent;
	hal_physaddr_t paddr;
	uint64_t index;
	int error;

	/* Makes each page a live allocation again and frees it to its extent. */
	while (pmem_page_stack_count != 0) {
		pmem_page_stack_count--;
		paddr = pmem_page_stack[pmem_page_stack_count];
		extent = extent_of_page(paddr, &index);
		if (extent == NULL)
			HAL_FATAL("amd64 page stack holds a page outside managed RAM");
		extent->heads[index / 64U] |= UINT64_C(1) << (index % 64U);
		error = pmem_free_unlocked(&paddr, PAGE_SIZE);
		if (error != HAL_OK)
			HAL_FATAL("amd64 page stack drain failed");
	}
}

/*
 * Allocates from the extents, giving the stacked pages back and trying
 * again when the extents alone cannot satisfy the request; the caller
 * holds pmem_lock.
 */
static int
pmem_alloc_or_drain(
	size_t size,
	size_t alignment,
	uint64_t maximum,
	uint64_t boundary,
	hal_physaddr_t *paddr)
{
	int error;

	/* Tries the extents as they are. */
	error = pmem_alloc_unlocked(size, alignment, maximum, boundary, paddr);
	if (error == HAL_OK || pmem_page_stack_count == 0)
		return error;

	/* Tries again with every stacked page back in its extent. */
	page_stack_drain();
	return pmem_alloc_unlocked(size, alignment, maximum, boundary, paddr);
}
