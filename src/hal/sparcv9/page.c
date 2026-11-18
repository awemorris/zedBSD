/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The SPARC V9 physical page allocator.
 *
 * Memory is a span from physical address zero to the highest installed
 * byte, one bit per 8 KiB page, set when the page is in use or not
 * available at all.  The firmware's available ranges are released at boot;
 * the low 16 MiB, which hold the firmware, the loader and the kernel image,
 * are taken back and never freed.
 */

#include <hal/hal.h>
#include "bsp.h"
#include "defs.h"
#include "page.h"
#include "space.h"
#include "task.h"

/* The most RAM the bitmap can describe: the direct map's span. */
#define PAGE_MAX_BYTES		((uint64_t)SPARCV9_DIRECT_LIMIT)
#define PAGE_MAX_PAGES		(PAGE_MAX_BYTES / SPARCV9_PAGE_SIZE)
#define PAGE_BITMAP_WORDS	(PAGE_MAX_PAGES / 32U)

/* The low memory the firmware, the loader and the kernel's 12 MiB window occupy. */
#define PAGE_LOW_RESERVED_BYTES	0x01000000UL

/*
 * One bit per page of the span: set in page_bitmap when the page is in use
 * or not available, and in reserved_bitmap when it was never available.
 * Changed with interrupts off after boot.
 */
static uint32_t page_bitmap[PAGE_BITMAP_WORDS];
static uint32_t reserved_bitmap[PAGE_BITMAP_WORDS];

/*
 * The span's first physical address and its length in pages.  Set once by
 * sparcv9_page_init() before any allocation and constant afterwards.
 */
static uint64_t ram_base;
static uint32_t ram_pages;

/*
 * The pages that were never available and the pages allocated now, for
 * the memory statistics.  Changed with interrupts off.
 */
static uint32_t reserved_pages;
static uint32_t allocated_pages;

/*
 * All pages below this one are in use.  An allocation moves it only across
 * pages in use, and a free below it moves it back.  Changed with
 * interrupts off; the port runs on one processor, so that is enough to
 * keep the bitmaps, the counters and the cursor consistent.
 */
static uint32_t first_free_candidate;

static void release_range(uint64_t base, uint64_t size);
static void reserve_range(uint64_t base, uint64_t size);
static int page_is_set(const uint32_t *bitmap, uint32_t page);
static void page_set(uint32_t *bitmap, uint32_t page);
static void page_clear(uint32_t *bitmap, uint32_t page);
static uint32_t next_free(uint32_t page, uint32_t end);
static uint32_t next_occupied(uint32_t page, uint32_t end);
static void claim_run(uint32_t first, uint32_t count);
static int alloc_ram(size_t size, size_t alignment, hal_physaddr_t max_paddr, size_t boundary, hal_physaddr_t *block);
static int free_ram(hal_physaddr_t *block, size_t size);

/*
 * Builds the page bitmap from the firmware's memory ranges.
 *
 * Every page starts in use; the available ranges are released, then the
 * low memory the firmware, the loader and the kernel image hold is taken
 * again.
 */
void
sparcv9_page_init(
	void)
{
	const struct sun4u_boot_handoff *handoff;
	uint64_t highest;
	uint64_t end;
	unsigned index;

	/* Finds the end of the installed memory. */
	handoff = sun4u_boot_handoff();
	highest = 0;
	for (index = 0; index < handoff->installed_count; index++) {
		end = handoff->installed[index].base + handoff->installed[index].size;
		if (end > highest)
			highest = end;
	}

	/* Keeps the span page-aligned and within the direct map. */
	highest &= ~((uint64_t)SPARCV9_PAGE_SIZE - 1U);
	if (highest > PAGE_MAX_BYTES)
		highest = PAGE_MAX_BYTES;
	ram_base = 0;
	ram_pages = (uint32_t)(highest / SPARCV9_PAGE_SIZE);

	/* Starts with every page unavailable, then releases the available ranges. */
	hal_memset(page_bitmap, 0xff, sizeof(page_bitmap));
	hal_memset(reserved_bitmap, 0xff, sizeof(reserved_bitmap));
	reserved_pages = ram_pages;
	allocated_pages = 0;
	for (index = 0; index < handoff->available_count; index++)
		release_range(handoff->available[index].base, handoff->available[index].size);

	/* Takes back the firmware, the loader and the kernel image. */
	reserve_range(0, PAGE_LOW_RESERVED_BYTES);

	/* The first allocation looks from the span's start. */
	first_free_candidate = 0;
	hal_printf("SPARCV9 MEMORY total=%lu MiB free=%lu MiB\n",
		   (uint64_t)ram_pages * SPARCV9_PAGE_SIZE / (1024U * 1024U),
		   ((uint64_t)ram_pages - reserved_pages) * SPARCV9_PAGE_SIZE / (1024U * 1024U));
}

/*
 * Reports the physical address a direct-map address stands for.
 */
uintptr_t
sparcv9_direct_to_phys(
	const void *address)
{
	/* Succeeded: the offset into the direct map is the physical address. */
	return (uintptr_t)address - SPARCV9_DIRECT_BASE;
}

/*
 * Reports the direct-map address of a physical address.
 */
void *
sparcv9_phys_to_direct(
	uintptr_t address)
{
	/* Succeeded: the direct map holds physical memory at a fixed offset. */
	return (void *)(SPARCV9_DIRECT_BASE + address);
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
	size_t alignment;
	bool enabled;
	int error;

	/* No alignment asked means page alignment. */
	alignment = req_align;
	if (alignment == 0)
		alignment = SPARCV9_PAGE_SIZE;

	/* Allocates with no reachability or boundary constraint. */
	enabled = hal_irq_disable();

	error = alloc_ram(req_size, alignment, ~(hal_physaddr_t)0, 0, block);

	if (enabled)
		hal_irq_enable();

	/* Reports why the block could not be allocated. */
	if (error != HAL_OK)
		return error;

	/* Succeeded: the block is the caller's. */
	return HAL_OK;
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
	size_t alignment;
	bool enabled;
	int error;

	/* No alignment asked means page alignment. */
	alignment = req_align;
	if (alignment == 0)
		alignment = SPARCV9_PAGE_SIZE;

	/* Applies the caller's reachability and boundary constraints. */
	enabled = hal_irq_disable();

	error = alloc_ram(req_size, alignment, max_paddr, boundary, block);

	if (enabled)
		hal_irq_enable();

	/* Reports why the block could not be allocated. */
	if (error != HAL_OK)
		return error;

	/* Succeeded: the block is the caller's. */
	return HAL_OK;
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

	/* Releases the pages the matching allocation claimed. */
	enabled = hal_irq_disable();

	error = free_ram(block, size);

	if (enabled)
		hal_irq_enable();

	/* Reports a block that was not the caller's to free. */
	if (error != HAL_OK)
		return error;

	/* Succeeded: the pages are free again. */
	return HAL_OK;
}

/*
 * Translates a physical RAM address to its direct-map address.
 */
void *
hal_pmem_to_kernel(
	hal_physaddr_t paddr)
{
	void *address;

	/* Reports no alias for an address outside managed RAM. */
	if ((uint64_t)paddr < ram_base)
		return NULL;
	if ((uint64_t)paddr - ram_base >= (uint64_t)ram_pages * SPARCV9_PAGE_SIZE)
		return NULL;

	/* Managed RAM is direct-mapped into the kernel half. */
	address = sparcv9_phys_to_direct((uintptr_t)paddr);

	/* Succeeded: the direct-map alias. */
	return address;
}

/*
 * Reports the size of managed RAM.
 */
size_t
hal_pmem_get_total_size(
	void)
{
	/* Succeeded: the span in bytes. */
	return (size_t)ram_pages * SPARCV9_PAGE_SIZE;
}

/*
 * Refuses an uncached view of RAM: the board's devices snoop the caches,
 * so none is needed.
 */
int
hal_pmem_map_uncached(
	hal_physaddr_t paddr,
	size_t size,
	void **vaddr)
{
	UNUSED_PARAMETER(paddr);
	UNUSED_PARAMETER(size);
	UNUSED_PARAMETER(vaddr);

	/* Failed: no such view exists on this port. */
	return HAL_ERR_UNSUPPORTED;
}

/*
 * Refuses to remove an uncached view, which this port never makes.
 */
int
hal_pmem_unmap_uncached(
	void *vaddr,
	size_t size)
{
	UNUSED_PARAMETER(vaddr);
	UNUSED_PARAMETER(size);

	/* Failed: no such view exists on this port. */
	return HAL_ERR_UNSUPPORTED;
}

/*
 * Collects the physical, task and address-space statistics.
 */
void
hal_get_memstat(
	struct hal_memstat *stat)
{
	bool enabled;

	/* A caller with nowhere to put the answer asked for nothing. */
	if (stat == NULL)
		return;
	hal_memset(stat, 0, sizeof(*stat));

	/* Samples the page counters together. */
	enabled = hal_irq_disable();

	stat->physical_total = (size_t)ram_pages * SPARCV9_PAGE_SIZE;
	stat->physical_reserved = (size_t)reserved_pages * SPARCV9_PAGE_SIZE;
	stat->physical_allocated = (size_t)allocated_pages * SPARCV9_PAGE_SIZE;

	if (enabled)
		hal_irq_enable();

	/* The free pages are what is neither reserved nor allocated. */
	stat->physical_free = stat->physical_total - stat->physical_reserved - stat->physical_allocated;

	/* Adds the task stacks and the page tables. */
	sparcv9_task_memory_stats(&stat->task_count, &stat->task_stack_bytes);
	sparcv9_space_memory_stats(&stat->space_count, &stat->page_table_count);
}

/* Makes the whole pages inside a range available. */
static void
release_range(
	uint64_t base,
	uint64_t size)
{
	uint64_t span_end;
	uint64_t end;
	uint64_t first;
	uint64_t last;
	uint64_t page;
	int reserved;

	/* Clips the range to the span. */
	span_end = ram_base + (uint64_t)ram_pages * SPARCV9_PAGE_SIZE;
	end = base + size;
	if (base < ram_base)
		base = ram_base;
	if (end > span_end)
		end = span_end;
	if (size == 0 || base >= end)
		return;

	/* Releases each page wholly inside the range. */
	first = (base - ram_base + SPARCV9_PAGE_SIZE - 1U) / SPARCV9_PAGE_SIZE;
	last = (end - ram_base) / SPARCV9_PAGE_SIZE;
	for (page = first; page < last; page++) {
		reserved = page_is_set(reserved_bitmap, (uint32_t)page);
		if (!reserved)
			continue;
		page_clear(page_bitmap, (uint32_t)page);
		page_clear(reserved_bitmap, (uint32_t)page);
		reserved_pages--;
	}
}

/* Takes every page a range touches out of use for good. */
static void
reserve_range(
	uint64_t base,
	uint64_t size)
{
	uint64_t span_end;
	uint64_t end;
	uint64_t first;
	uint64_t last;
	uint64_t page;
	int in_use;

	/* Clips the range to the span. */
	span_end = ram_base + (uint64_t)ram_pages * SPARCV9_PAGE_SIZE;
	end = base + size;
	if (base < ram_base)
		base = ram_base;
	if (end > span_end)
		end = span_end;
	if (size == 0 || base >= end)
		return;

	/* Reserves each page the range touches. */
	first = (base - ram_base) / SPARCV9_PAGE_SIZE;
	last = (end - ram_base + SPARCV9_PAGE_SIZE - 1U) / SPARCV9_PAGE_SIZE;
	for (page = first; page < last; page++) {
		in_use = page_is_set(page_bitmap, (uint32_t)page);
		if (in_use)
			continue;
		page_set(page_bitmap, (uint32_t)page);
		page_set(reserved_bitmap, (uint32_t)page);
		reserved_pages++;
	}
}

/* Reports whether a page's bit is set. */
static int
page_is_set(
	const uint32_t *bitmap,
	uint32_t page)
{
	/* The bit of the page in its word. */
	if ((bitmap[page >> 5] & (1U << (page & 31U))) != 0)
		return 1;
	return 0;
}

/* Sets a page's bit. */
static void
page_set(
	uint32_t *bitmap,
	uint32_t page)
{
	/* The bit of the page in its word. */
	bitmap[page >> 5] |= 1U << (page & 31U);
}

/* Clears a page's bit. */
static void
page_clear(
	uint32_t *bitmap,
	uint32_t page)
{
	/* The bit of the page in its word. */
	bitmap[page >> 5] &= ~(1U << (page & 31U));
}

/* Finds the lowest free page in [page, end), or end; whole words in use are skipped at once. */
static uint32_t
next_free(
	uint32_t page,
	uint32_t end)
{
	uint32_t word;
	uint32_t found;

	/* Bits below the page in its first word count as in use. */
	if (page >= end)
		return end;
	word = page_bitmap[page >> 5] | ((1U << (page & 31U)) - 1U);
	page &= ~31U;

	/* Skips whole words in use up to the end. */
	while (word == UINT32_MAX) {
		page += 32U;
		if (page >= end)
			return end;
		word = page_bitmap[page >> 5];
	}

	/* The lowest clear bit, unless it lies at or past the end. */
	found = page + (uint32_t)__builtin_ctz(~word);
	if (found >= end)
		return end;

	/* Succeeded: the page is free. */
	return found;
}

/* Finds the lowest page in use in [page, end), or end; whole free words are skipped at once. */
static uint32_t
next_occupied(
	uint32_t page,
	uint32_t end)
{
	uint32_t word;
	uint32_t found;

	/* Bits below the page in its first word count as free. */
	if (page >= end)
		return end;
	word = page_bitmap[page >> 5] & ~((1U << (page & 31U)) - 1U);
	page &= ~31U;

	/* Skips whole free words up to the end. */
	while (word == 0) {
		page += 32U;
		if (page >= end)
			return end;
		word = page_bitmap[page >> 5];
	}

	/* The lowest set bit, unless it lies at or past the end. */
	found = page + (uint32_t)__builtin_ctz(word);
	if (found >= end)
		return end;

	/* Succeeded: the page is in use. */
	return found;
}

/* Marks a free run in use; the caller holds the lock. */
static void
claim_run(
	uint32_t first,
	uint32_t count)
{
	uint32_t end;

	/* Single pages up to a word boundary, whole words, then the tail's pages. */
	end = first + count;
	while (first < end && (first & 31U) != 0) {
		page_set(page_bitmap, first);
		first++;
	}
	while (end - first >= 32U) {
		page_bitmap[first >> 5] = UINT32_MAX;
		first += 32U;
	}
	while (first < end) {
		page_set(page_bitmap, first);
		first++;
	}
}

/*
 * Finds and claims an aligned run of free pages.
 *
 * max_paddr is the highest byte the caller can address, and boundary,
 * when not zero, is a power-of-two block the run must not cross.  The
 * caller holds the lock.
 */
static int
alloc_ram(
	size_t size,
	size_t alignment,
	hal_physaddr_t max_paddr,
	size_t boundary,
	hal_physaddr_t *block)
{
	uint64_t first_byte;
	uint64_t last_byte;
	uint64_t base_page;
	uint64_t aligned;
	uint32_t need;
	uint32_t end;
	uint32_t start;
	uint32_t align_pages;
	uint32_t boundary_pages;
	uint32_t occupied;

	/* Requires a destination and a representable request. */
	if (block == NULL || size == 0)
		return HAL_ERR_INVALID;
	if (size > SIZE_MAX - (SPARCV9_PAGE_SIZE - 1))
		return HAL_ERR_INVALID;
	if (alignment < SPARCV9_PAGE_SIZE)
		return HAL_ERR_INVALID;
	if ((alignment & (alignment - 1U)) != 0)
		return HAL_ERR_INVALID;

	/* Refuses a boundary that is not a power of two. */
	if (boundary != 0 && (boundary & (boundary - 1U)) != 0)
		return HAL_ERR_INVALID;

	/* Rounds the request up to pages, as the free repeats it. */
	need = (uint32_t)((size + SPARCV9_PAGE_SIZE - 1) / SPARCV9_PAGE_SIZE);
	align_pages = (uint32_t)(alignment / SPARCV9_PAGE_SIZE);

	/*
	 * Limits the search to the pages the caller can address.  The span
	 * starts at zero, so the highest address is compared before one is
	 * added to it.
	 */
	end = ram_pages;
	if ((uint64_t)max_paddr < ram_base)
		return HAL_ERR_NOMEM;
	if ((uint64_t)max_paddr - ram_base < (uint64_t)ram_pages * SPARCV9_PAGE_SIZE - 1U)
		end = (uint32_t)(((uint64_t)max_paddr - ram_base + 1U) / SPARCV9_PAGE_SIZE);
	if (need > end)
		return HAL_ERR_NOMEM;

	/* A run longer than the boundary block crosses it wherever it starts. */
	boundary_pages = 0;
	if (boundary != 0) {
		if ((uint64_t)need * SPARCV9_PAGE_SIZE > boundary)
			return HAL_ERR_NOMEM;
		boundary_pages = (uint32_t)(boundary / SPARCV9_PAGE_SIZE);
	}

	/* Reuses the known prefix in use rather than scanning it each time. */
	first_free_candidate = next_free(first_free_candidate, ram_pages);

	/*
	 * Finds the lowest fitting run.  The alignment is of the physical
	 * address, so the span's first page is taken into account.
	 */
	base_page = ram_base / SPARCV9_PAGE_SIZE;
	start = first_free_candidate;
	for (;;) {
		/* The lowest free page, moved up to the alignment of its physical address. */
		start = next_free(start, end);
		aligned = (base_page + start + align_pages - 1U) & ~((uint64_t)align_pages - 1U);
		if (aligned - base_page >= end)
			break;
		start = (uint32_t)(aligned - base_page);
		if (need > end - start)
			break;

		/* A run crossing a boundary moves to the next boundary block. */
		if (boundary_pages != 0) {
			first_byte = ram_base + (uint64_t)start * SPARCV9_PAGE_SIZE;
			last_byte = first_byte + (uint64_t)need * SPARCV9_PAGE_SIZE - 1U;
			if (first_byte / boundary != last_byte / boundary) {
				start = (uint32_t)(((first_byte / boundary + 1U) * boundary - ram_base) / SPARCV9_PAGE_SIZE);
				continue;
			}
		}

		/* The run fits when nothing in it is in use; else the search goes past what is. */
		occupied = next_occupied(start, start + need);
		if (occupied == start + need)
			break;
		start = occupied + 1U;
	}

	/* Reports that no run fits. */
	if (aligned - base_page >= end)
		return HAL_ERR_NOMEM;
	if (need > end - start)
		return HAL_ERR_NOMEM;

	/* Succeeded: the run is the caller's. */
	claim_run(start, need);
	allocated_pages += need;
	*block = (hal_physaddr_t)(ram_base + (uint64_t)start * SPARCV9_PAGE_SIZE);
	return HAL_OK;
}

/*
 * Releases the pages of one allocation; the caller holds the lock.
 *
 * size is the size the matching allocation asked for, so the same
 * rounding is repeated here and a smaller size splits the block.
 */
static int
free_ram(
	hal_physaddr_t *block,
	size_t size)
{
	uint64_t offset;
	uint32_t first;
	uint32_t count;
	uint32_t index;
	int in_use;
	int reserved;

	/* Refuses an empty, unaligned or foreign release. */
	if (block == NULL || size == 0)
		return HAL_ERR_INVALID;
	if (((uint64_t)*block & (SPARCV9_PAGE_SIZE - 1U)) != 0)
		return HAL_ERR_INVALID;
	if ((uint64_t)*block < ram_base)
		return HAL_ERR_INVALID;

	/* Repeats the rounding the allocation applied. */
	offset = (uint64_t)*block - ram_base;
	if (offset / SPARCV9_PAGE_SIZE >= ram_pages)
		return HAL_ERR_INVALID;
	first = (uint32_t)(offset / SPARCV9_PAGE_SIZE);
	count = (uint32_t)((size + SPARCV9_PAGE_SIZE - 1) / SPARCV9_PAGE_SIZE);
	if (count > ram_pages - first)
		return HAL_ERR_INVALID;

	/* Checks that every page was allocated, before any changes. */
	for (index = 0; index < count; index++) {
		in_use = page_is_set(page_bitmap, first + index);
		if (!in_use)
			return HAL_ERR_STATE;
		reserved = page_is_set(reserved_bitmap, first + index);
		if (reserved)
			return HAL_ERR_STATE;
	}

	/* Releases the pages. */
	for (index = 0; index < count; index++)
		page_clear(page_bitmap, first + index);
	allocated_pages -= count;

	/* A later allocation looks at the released pages again. */
	if (first < first_free_candidate)
		first_free_candidate = first;

	/* Succeeded: clears the caller's address so a double free shows. */
	*block = 0;
	return HAL_OK;
}
