/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The riscv32 physical page allocator.
 *
 * Memory is a span from the lowest RAM bank's start to the highest one's
 * end, one bit per page, set when the page is in use or not RAM at all.
 * The kernel image, the device tree, the initial ramdisk and every
 * reservation the tree names are set at boot and never freed.
 */

#include <hal/hal.h>
#include "asm.h"
#include "bootinfo.h"
#include "defs.h"
#include "page.h"
#include "space.h"
#include "spin.h"

/* The most RAM the bitmap can describe: 1 GiB, what the direct map holds. */
#define PAGE_MAX_BYTES		(1024ULL * 1024ULL * 1024ULL)
#define PAGE_MAX_PAGES		(PAGE_MAX_BYTES / RISCV32_PAGE_SIZE)
#define PAGE_BITMAP_WORDS	(PAGE_MAX_PAGES / 32U)

/* The bounds of the kernel image, from the linker script. */
extern char __kernel_vma_start[];
extern char __kernel_vma_end[];

/*
 * One bit per page of the span: set in page_bitmap when the page is in use
 * or not available, and in reserved_bitmap when it was never available.
 * Kept under pmem_lock after boot.
 */
static uint32_t page_bitmap[PAGE_BITMAP_WORDS];
static uint32_t reserved_bitmap[PAGE_BITMAP_WORDS];

/*
 * The span's first physical address and its length in pages.  Set once by
 * riscv32_page_init() before any allocation and constant afterwards.
 */
static uint64_t ram_base;
static uint32_t ram_pages;

/*
 * The pages that were never available and the pages allocated now, for
 * the memory statistics.  Kept under pmem_lock.
 */
static uint32_t reserved_pages;
static uint32_t allocated_pages;

/*
 * All pages below this one are in use.  An allocation moves it only across
 * pages in use, and a free below it moves it back.  Kept under pmem_lock.
 */
static uint32_t first_free_candidate;

/* Guards the bitmaps, the counters and the cursor; zero when free. */
static volatile unsigned pmem_lock;

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
 * Builds the page bitmap from the device tree's memory and reservations.
 *
 * Every page starts in use; the RAM banks are released, then the kernel
 * image, the tree, the ramdisk and the reservations are taken again.
 */
void
riscv32_page_init(
	void)
{
	const struct riscv32_fdt_info *info;
	uint64_t lowest;
	uint64_t highest;
	uint64_t end;
	uint64_t kernel_start;
	uint64_t kernel_size;
	unsigned index;

	/* Finds the span the RAM banks cover. */
	info = riscv32_boot_fdt();
	if (info->memory_count == 0)
		HAL_FATAL("riscv32 device tree describes no memory");
	lowest = ~0ULL;
	highest = 0;
	for (index = 0; index < info->memory_count; index++) {
		end = info->memory[index].base + info->memory[index].size;
		if (info->memory[index].base < lowest)
			lowest = info->memory[index].base;
		if (end > highest)
			highest = end;
	}

	/* Keeps the span page-aligned and within what the bitmap can describe. */
	lowest &= ~((uint64_t)RISCV32_PAGE_SIZE - 1U);
	highest &= ~((uint64_t)RISCV32_PAGE_SIZE - 1U);
	if (highest - lowest > PAGE_MAX_BYTES)
		highest = lowest + PAGE_MAX_BYTES;
	if (lowest < RISCV32_DIRECT_BASE)
		HAL_FATAL("riscv32 memory below the direct map");
	if (highest > RISCV32_DIRECT_LIMIT)
		highest = RISCV32_DIRECT_LIMIT;
	ram_base = lowest;
	ram_pages = (uint32_t)((highest - lowest) / RISCV32_PAGE_SIZE);

	/* Starts with every page unavailable, then releases the banks. */
	hal_memset(page_bitmap, 0xff, sizeof(page_bitmap));
	hal_memset(reserved_bitmap, 0xff, sizeof(reserved_bitmap));
	reserved_pages = ram_pages;
	allocated_pages = 0;
	for (index = 0; index < info->memory_count; index++)
		release_range(info->memory[index].base, info->memory[index].size);

	/* Takes back the kernel image, which holds the boot tables and stacks. */
	kernel_start = riscv32_kernel_phys_start();
	kernel_size = (uint64_t)(uintptr_t)(__kernel_vma_end - __kernel_vma_start);
	reserve_range(kernel_start, kernel_size);

	/* Takes back the device tree and the initial ramdisk the firmware loaded. */
	reserve_range(riscv32_boot_fdt_phys(), info->totalsize);
	if (info->initrd_end > info->initrd_start)
		reserve_range(info->initrd_start, info->initrd_end - info->initrd_start);

	/* Takes back every reservation, the firmware's own memory among them. */
	for (index = 0; index < info->reserved_count; index++)
		reserve_range(info->reserved[index].base, info->reserved[index].size);
	for (index = 0; index < info->nomap_count; index++)
		reserve_range(info->nomap[index].base, info->nomap[index].size);

	/* The first allocation looks from the span's start. */
	first_free_candidate = 0;
	hal_printf("RISCV32 MEMORY base=%llx total=%llu MiB free=%llu MiB\n",
		   ram_base,
		   (uint64_t)ram_pages * RISCV32_PAGE_SIZE / (1024U * 1024U),
		   ((uint64_t)ram_pages - reserved_pages) * RISCV32_PAGE_SIZE / (1024U * 1024U));
}

/*
 * Reports the first physical address of managed RAM.
 */
uint64_t
riscv32_ram_base(
	void)
{
	/* Succeeded: the span's start, fixed at boot. */
	return ram_base;
}

/*
 * Reports the end of managed RAM: the first address past the span.
 */
uint64_t
riscv32_ram_end(
	void)
{
	/* Succeeded: the span's end, fixed at boot. */
	return ram_base + (uint64_t)ram_pages * RISCV32_PAGE_SIZE;
}

/*
 * Reports whether a physical range lies entirely inside one RAM bank.
 */
bool
riscv32_range_is_ram(
	uint64_t physical,
	uint64_t size)
{
	const struct riscv32_fdt_info *info;
	uint64_t bank_end;
	unsigned index;

	/* Looks for a bank that holds the whole range. */
	info = riscv32_boot_fdt();
	for (index = 0; index < info->memory_count; index++) {
		/* Skips a bank that starts after the range. */
		if (info->memory[index].base > physical)
			continue;

		/* Skips a bank that ends at or before the range's start. */
		bank_end = info->memory[index].base + info->memory[index].size;
		if (physical >= bank_end)
			continue;

		/* Skips a bank that holds the start but not the rest. */
		if (bank_end - physical < size)
			continue;
		return true;
	}

	/* No bank holds the range. */
	return false;
}

/*
 * Reports whether a physical range overlaps any RAM bank.
 */
bool
riscv32_range_touches_ram(
	uint64_t physical,
	uint64_t size)
{
	const struct riscv32_fdt_info *info;
	uint64_t bank_end;
	uint64_t end;
	unsigned index;

	/* Compares the range with every bank. */
	info = riscv32_boot_fdt();
	end = physical + size;
	for (index = 0; index < info->memory_count; index++) {
		/* Skips a bank that ends before the range starts. */
		bank_end = info->memory[index].base + info->memory[index].size;
		if (bank_end <= physical)
			continue;

		/* Skips a bank that starts after the range ends. */
		if (info->memory[index].base >= end)
			continue;
		return true;
	}

	/* The range is outside RAM. */
	return false;
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
		alignment = RISCV32_PAGE_SIZE;

	/* Allocates with no reachability or boundary constraint. */
	enabled = riscv32_spin_lock(&pmem_lock);

	error = alloc_ram(req_size, alignment, ~(hal_physaddr_t)0, 0, block);

	riscv32_spin_unlock(&pmem_lock, enabled);

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
		alignment = RISCV32_PAGE_SIZE;

	/* Applies the caller's reachability and boundary constraints. */
	enabled = riscv32_spin_lock(&pmem_lock);

	error = alloc_ram(req_size, alignment, max_paddr, boundary, block);

	riscv32_spin_unlock(&pmem_lock, enabled);

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
	enabled = riscv32_spin_lock(&pmem_lock);

	error = free_ram(block, size);

	riscv32_spin_unlock(&pmem_lock, enabled);

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
	if ((uint64_t)paddr - ram_base >= (uint64_t)ram_pages * RISCV32_PAGE_SIZE)
		return NULL;

	/* Managed RAM is direct-mapped into the kernel half. */
	address = riscv32_phys_to_direct((uint64_t)paddr);

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
	return (size_t)ram_pages * RISCV32_PAGE_SIZE;
}

/*
 * Refuses an uncached view of RAM: the cache behaviour of memory is fixed
 * by the platform here, not by the page tables.
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
	enabled = riscv32_spin_lock(&pmem_lock);

	stat->physical_total = (size_t)ram_pages * RISCV32_PAGE_SIZE;
	stat->physical_reserved = (size_t)reserved_pages * RISCV32_PAGE_SIZE;
	stat->physical_allocated = (size_t)allocated_pages * RISCV32_PAGE_SIZE;

	riscv32_spin_unlock(&pmem_lock, enabled);

	/* The free pages are what is neither reserved nor allocated. */
	stat->physical_free = stat->physical_total - stat->physical_reserved - stat->physical_allocated;

	/* Adds the task stacks and the page tables. */
	riscv32_task_memory_stats(&stat->task_count, &stat->task_stack_bytes);
	riscv32_space_memory_stats(&stat->space_count, &stat->page_table_count);
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
	span_end = ram_base + (uint64_t)ram_pages * RISCV32_PAGE_SIZE;
	end = base + size;
	if (base < ram_base)
		base = ram_base;
	if (end > span_end)
		end = span_end;
	if (size == 0 || base >= end)
		return;

	/* Releases each page wholly inside the range. */
	first = (base - ram_base + RISCV32_PAGE_SIZE - 1U) / RISCV32_PAGE_SIZE;
	last = (end - ram_base) / RISCV32_PAGE_SIZE;
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
	span_end = ram_base + (uint64_t)ram_pages * RISCV32_PAGE_SIZE;
	end = base + size;
	if (base < ram_base)
		base = ram_base;
	if (end > span_end)
		end = span_end;
	if (size == 0 || base >= end)
		return;

	/* Reserves each page the range touches. */
	first = (base - ram_base) / RISCV32_PAGE_SIZE;
	last = (end - ram_base + RISCV32_PAGE_SIZE - 1U) / RISCV32_PAGE_SIZE;
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
	if (size > SIZE_MAX - (RISCV32_PAGE_SIZE - 1))
		return HAL_ERR_INVALID;
	if (alignment < RISCV32_PAGE_SIZE)
		return HAL_ERR_INVALID;
	if ((alignment & (alignment - 1U)) != 0)
		return HAL_ERR_INVALID;

	/* Refuses a boundary that is not a power of two. */
	if (boundary != 0 && (boundary & (boundary - 1U)) != 0)
		return HAL_ERR_INVALID;

	/* Rounds the request up to pages, as the free repeats it. */
	need = (uint32_t)((size + RISCV32_PAGE_SIZE - 1) / RISCV32_PAGE_SIZE);
	align_pages = (uint32_t)(alignment / RISCV32_PAGE_SIZE);

	/* Limits the search to the pages the caller can address. */
	end = ram_pages;
	if ((uint64_t)max_paddr < ram_base)
		return HAL_ERR_NOMEM;
	if ((uint64_t)max_paddr - ram_base + 1U < (uint64_t)ram_pages * RISCV32_PAGE_SIZE)
		end = (uint32_t)(((uint64_t)max_paddr - ram_base + 1U) / RISCV32_PAGE_SIZE);
	if (need > end)
		return HAL_ERR_NOMEM;

	/* A run longer than the boundary block crosses it wherever it starts. */
	boundary_pages = 0;
	if (boundary != 0) {
		if ((uint64_t)need * RISCV32_PAGE_SIZE > boundary)
			return HAL_ERR_NOMEM;
		boundary_pages = (uint32_t)(boundary / RISCV32_PAGE_SIZE);
	}

	/* Reuses the known prefix in use rather than scanning it each time. */
	first_free_candidate = next_free(first_free_candidate, ram_pages);

	/*
	 * Finds the lowest fitting run.  The alignment is of the physical
	 * address, so the span's first page is taken into account.
	 */
	base_page = ram_base / RISCV32_PAGE_SIZE;
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
			first_byte = ram_base + (uint64_t)start * RISCV32_PAGE_SIZE;
			last_byte = first_byte + (uint64_t)need * RISCV32_PAGE_SIZE - 1U;
			if (first_byte / boundary != last_byte / boundary) {
				start = (uint32_t)(((first_byte / boundary + 1U) * boundary - ram_base) / RISCV32_PAGE_SIZE);
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
	*block = (hal_physaddr_t)(ram_base + (uint64_t)start * RISCV32_PAGE_SIZE);
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
	if (((uint64_t)*block & (RISCV32_PAGE_SIZE - 1U)) != 0)
		return HAL_ERR_INVALID;
	if ((uint64_t)*block < ram_base)
		return HAL_ERR_INVALID;

	/* Repeats the rounding the allocation applied. */
	offset = (uint64_t)*block - ram_base;
	if (offset / RISCV32_PAGE_SIZE >= ram_pages)
		return HAL_ERR_INVALID;
	first = (uint32_t)(offset / RISCV32_PAGE_SIZE);
	count = (uint32_t)((size + RISCV32_PAGE_SIZE - 1) / RISCV32_PAGE_SIZE);
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
