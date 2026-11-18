/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The MIPS physical page allocator.
 *
 * Memory is the board's RAM, one bit per page, set when the page is in
 * use.  The loader's tables, the memory below the image and the image
 * itself, and the initial ramdisk are set at boot and never freed.
 *
 * The malta board shows its first 256 MiB of RAM at physical address 0
 * (the low alias) and all of it again at 0x80000000 (the high alias).
 * Each page has one physical address here, so that no page is handed out
 * twice under two names: the low alias for the first 256 MiB, where the
 * image, the loader's tables and the ramdisk are, and the high alias for
 * the RAM past it.  Page number i, counted from the start of RAM, is at
 * physical address i * 4 KiB below 256 MiB and at 0x80000000 + i * 4 KiB
 * from there; a block never spans the two.  The 32-bit processor reaches
 * only the low alias (through KSEG0) and manages that alone; the 64-bit
 * one reaches both through xkphys.  A device sees a page at its physical
 * address.
 */

#include <hal/hal.h>
#include "asm.h"
#include "bsp.h"
#include "cache.h"
#include "io.h"
#include "page.h"
#include "space.h"
#include "spin.h"
#include "task.h"

/*
 * The size of the low RAM alias, and the most RAM the bitmap describes:
 * the low alias alone on the 32-bit processor, which reaches it through
 * KSEG0, and the board's largest RAM, 2 GiB, on the 64-bit one.  The high
 * alias holds RAM offset o at physical address PAGE_HIGH_ALIAS + o.
 *
 * Values referenced: QEMU v10.0.11 hw/mips/malta.c (the RAM region at
 * 0x80000000, the low alias of at most 256 MiB, the environment's memsize
 * and ememsize, and the 2 GiB limit of RAM).
 */
#define PAGE_LOW_BYTES		(256ULL * 1024ULL * 1024ULL)
#define PAGE_LOW_PAGES		((uint32_t)(PAGE_LOW_BYTES / MIPS_PAGE_SIZE))
#define PAGE_HIGH_ALIAS		0x80000000ULL
#if defined(HAL_ARCH_MIPS64)
#define PAGE_MAX_BYTES		(2048ULL * 1024ULL * 1024ULL)
#else
#define PAGE_MAX_BYTES		PAGE_LOW_BYTES
#endif
#define PAGE_MAX_PAGES		(PAGE_MAX_BYTES / MIPS_PAGE_SIZE)
#define PAGE_BITMAP_WORDS	(PAGE_MAX_PAGES / 32U)

/* The end of the kernel image, from the linker script. */
extern char __kernel_vma_end[];

/*
 * One bit per page of the span: set in page_bitmap when the page is in use
 * or not available, and in reserved_bitmap when it was never available.
 * Kept under pmem_lock after boot.
 */
static uint32_t page_bitmap[PAGE_BITMAP_WORDS];
static uint32_t reserved_bitmap[PAGE_BITMAP_WORDS];

/*
 * The pages of RAM, and those of them in the low alias: the first
 * low_pages are at their offset in RAM, the rest in the high alias.  Set
 * once by mips_page_init() before any allocation and constant afterwards.
 */
static uint32_t ram_pages;
static uint32_t low_pages;

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
static uint64_t page_physical(uint32_t page);
static bool page_of(uint64_t physical, uint32_t *page);

/*
 * Builds the page bitmap from the board's memory.
 *
 * Every page starts in use; the RAM is released, then everything below
 * the image's end and the ramdisk are taken again.  Those are in the low
 * alias, where a physical address is the offset in RAM.
 */
void
mips_page_init(
	void)
{
	uint32_t ramdisk_phys;
	uint32_t ramdisk_size;
	uint64_t highest;
	uint64_t kernel_end;

	/* The RAM, kept page-aligned and within the bitmap. */
	highest = (uint64_t)mips_board_memory_total() & ~((uint64_t)MIPS_PAGE_SIZE - 1U);
	if (highest == 0)
		HAL_FATAL("mips board reports no memory");
	if (highest > PAGE_MAX_BYTES)
		highest = PAGE_MAX_BYTES;
	ram_pages = (uint32_t)(highest / MIPS_PAGE_SIZE);
	low_pages = ram_pages;
	if (low_pages > PAGE_LOW_PAGES)
		low_pages = PAGE_LOW_PAGES;

	/* Starts with every page unavailable, then releases the RAM. */
	hal_memset(page_bitmap, 0xff, sizeof(page_bitmap));
	hal_memset(reserved_bitmap, 0xff, sizeof(reserved_bitmap));
	reserved_pages = ram_pages;
	allocated_pages = 0;
	release_range(0, highest);

	/* Takes back the loader's tables, the memory below the image, the image and the ramdisk. */
	kernel_end = mips_image_physical((uintptr_t)__kernel_vma_end);
	reserve_range(0, kernel_end);
	mips_board_ramdisk(&ramdisk_phys, &ramdisk_size);
	if (ramdisk_size != 0)
		reserve_range(ramdisk_phys, ramdisk_size);

	/* The first allocation looks from the span's start. */
	first_free_candidate = 0;
	hal_printf(MIPS_HAL_NAME " MEMORY low=%llu MiB high=%llu MiB total=%llu MiB free=%llu MiB\n",
		   (uint64_t)low_pages * MIPS_PAGE_SIZE / (1024U * 1024U),
		   (uint64_t)(ram_pages - low_pages) * MIPS_PAGE_SIZE / (1024U * 1024U),
		   (uint64_t)ram_pages * MIPS_PAGE_SIZE / (1024U * 1024U),
		   ((uint64_t)ram_pages - reserved_pages) * MIPS_PAGE_SIZE / (1024U * 1024U));
}

/*
 * Reports whether a physical range lies entirely inside one RAM alias's
 * managed part.
 */
bool
mips_range_is_ram(
	uint64_t physical,
	uint64_t size)
{
	uint32_t first;
	uint32_t last;
	bool found;

	/* Both ends are managed pages, of the same alias. */
	if (size == 0 || physical + size < physical)
		return false;
	found = page_of(physical, &first);
	if (!found)
		return false;
	found = page_of(physical + size - 1U, &last);
	if (!found)
		return false;
	if ((first < low_pages) != (last < low_pages))
		return false;
	return true;
}

/*
 * Reports whether a physical range overlaps managed RAM.
 */
bool
mips_range_touches_ram(
	uint64_t physical,
	uint64_t size)
{
	uint64_t end;
	uint64_t high_start;
	uint64_t high_end;

	/* The range against the low alias's part, then the high alias's. */
	end = physical + size;
	if (size == 0)
		return false;
	if (physical < (uint64_t)low_pages * MIPS_PAGE_SIZE)
		return true;
	high_start = PAGE_HIGH_ALIAS + (uint64_t)low_pages * MIPS_PAGE_SIZE;
	high_end = PAGE_HIGH_ALIAS + (uint64_t)ram_pages * MIPS_PAGE_SIZE;
	if (high_start < high_end && physical < high_end && end > high_start)
		return true;
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
		alignment = MIPS_PAGE_SIZE;

	/* Allocates with no reachability or boundary constraint. */
	enabled = mips_spin_lock(&pmem_lock);

	error = alloc_ram(req_size, alignment, ~(hal_physaddr_t)0, 0, block);

	mips_spin_unlock(&pmem_lock, enabled);

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
		alignment = MIPS_PAGE_SIZE;

	/* Applies the caller's reachability and boundary constraints. */
	enabled = mips_spin_lock(&pmem_lock);

	error = alloc_ram(req_size, alignment, max_paddr, boundary, block);

	mips_spin_unlock(&pmem_lock, enabled);

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
	enabled = mips_spin_lock(&pmem_lock);

	error = free_ram(block, size);

	mips_spin_unlock(&pmem_lock, enabled);

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
	uint32_t page;
	bool found;

	/* Reports no alias for an address outside managed RAM. */
	found = page_of(paddr, &page);
	if (!found)
		return NULL;

	/* The kernel reaches RAM cached: through KSEG0, or xkphys. */
	address = (void *)mips_cached_address(paddr);

	/* Succeeded: the alias. */
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
	return (size_t)ram_pages * MIPS_PAGE_SIZE;
}

/*
 * Reports an uncached view of a RAM range: its KSEG1 alias, or on the
 * 64-bit processor its uncached xkphys alias.
 *
 * The range's lines are written back and dropped first, so that no line
 * of the cached view later overwrites what is written through this one.
 */
int
hal_pmem_map_uncached(
	hal_physaddr_t paddr,
	size_t size,
	void **vaddr)
{
	bool ram;

	/* Requires a destination and a range of managed RAM. */
	if (vaddr == NULL || size == 0)
		return HAL_ERR_INVALID;
	ram = mips_range_is_ram(paddr, size);
	if (!ram)
		return HAL_ERR_INVALID;

	/* Empties the cached view's lines of the range. */
	hal_dcache_clean_invalidate_range(mips_cached_address(paddr), size);

	/* Succeeded: the uncached segment's alias of the range. */
	*vaddr = (void *)mips_uncached_address(paddr);
	return HAL_OK;
}

/*
 * Ends an uncached view, which needs no undoing: the segment is fixed.
 */
int
hal_pmem_unmap_uncached(
	void *vaddr,
	size_t size)
{
	uintptr_t address;

	/* Refuses an empty range or one outside the uncached segment. */
	address = (uintptr_t)vaddr;
	if (size == 0)
		return HAL_ERR_INVALID;
#if defined(HAL_ARCH_MIPS64)
	if (address < (uintptr_t)MIPS_XKPHYS_UNCACHED || address - (uintptr_t)MIPS_XKPHYS_UNCACHED >= PAGE_HIGH_ALIAS + PAGE_MAX_BYTES)
		return HAL_ERR_INVALID;
#else
	if (address < MIPS_KSEG1_BASE || address - MIPS_KSEG1_BASE >= MIPS_KSEG_SIZE)
		return HAL_ERR_INVALID;
#endif

	/* Succeeded: nothing was mapped for the view. */
	return HAL_OK;
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
	enabled = mips_spin_lock(&pmem_lock);

	stat->physical_total = (size_t)ram_pages * MIPS_PAGE_SIZE;
	stat->physical_reserved = (size_t)reserved_pages * MIPS_PAGE_SIZE;
	stat->physical_allocated = (size_t)allocated_pages * MIPS_PAGE_SIZE;

	mips_spin_unlock(&pmem_lock, enabled);

	/* The free pages are what is neither reserved nor allocated. */
	stat->physical_free = stat->physical_total - stat->physical_reserved - stat->physical_allocated;

	/* Adds the task stacks and the page tables. */
	mips_task_memory_stats(&stat->task_count, &stat->task_stack_bytes);
	mips_space_memory_stats(&stat->space_count, &stat->page_table_count);
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

	/* Clips the range to the RAM, by offset. */
	span_end = (uint64_t)ram_pages * MIPS_PAGE_SIZE;
	end = base + size;
	if (end > span_end)
		end = span_end;
	if (size == 0 || base >= end)
		return;

	/* Releases each page wholly inside the range. */
	first = (base + MIPS_PAGE_SIZE - 1U) / MIPS_PAGE_SIZE;
	last = end / MIPS_PAGE_SIZE;
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

	/* Clips the range to the RAM, by offset. */
	span_end = (uint64_t)ram_pages * MIPS_PAGE_SIZE;
	end = base + size;
	if (end > span_end)
		end = span_end;
	if (size == 0 || base >= end)
		return;

	/* Reserves each page the range touches. */
	first = base / MIPS_PAGE_SIZE;
	last = (end + MIPS_PAGE_SIZE - 1U) / MIPS_PAGE_SIZE;
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
	uint64_t limit;
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
	if (size > SIZE_MAX - (MIPS_PAGE_SIZE - 1))
		return HAL_ERR_INVALID;
	if (alignment < MIPS_PAGE_SIZE)
		return HAL_ERR_INVALID;
	if ((alignment & (alignment - 1U)) != 0)
		return HAL_ERR_INVALID;

	/* Refuses a boundary that is not a power of two. */
	if (boundary != 0 && (boundary & (boundary - 1U)) != 0)
		return HAL_ERR_INVALID;

	/* Rounds the request up to pages, as the free repeats it. */
	need = (uint32_t)((size + MIPS_PAGE_SIZE - 1) / MIPS_PAGE_SIZE);
	align_pages = (uint32_t)(alignment / MIPS_PAGE_SIZE);

	/*
	 * Limits the search to the pages the caller can address: a limit
	 * below the high alias's part leaves the low alias's pages up to it.
	 */
	end = ram_pages;
	limit = (uint64_t)max_paddr;
	if (limit < (uint64_t)low_pages * MIPS_PAGE_SIZE) {
		end = (uint32_t)((limit + 1U) / MIPS_PAGE_SIZE);
	} else if (limit < PAGE_HIGH_ALIAS + (uint64_t)low_pages * MIPS_PAGE_SIZE) {
		end = low_pages;
	} else if (limit - PAGE_HIGH_ALIAS + 1U < (uint64_t)ram_pages * MIPS_PAGE_SIZE) {
		end = (uint32_t)((limit - PAGE_HIGH_ALIAS + 1U) / MIPS_PAGE_SIZE);
	}

	/* Refuses a run longer than the addressable pages. */
	if (need > end)
		return HAL_ERR_NOMEM;

	/* A run longer than the boundary block crosses it wherever it starts. */
	boundary_pages = 0;
	if (boundary != 0) {
		if ((uint64_t)need * MIPS_PAGE_SIZE > boundary)
			return HAL_ERR_NOMEM;
		boundary_pages = (uint32_t)(boundary / MIPS_PAGE_SIZE);
	}

	/* Reuses the known prefix in use rather than scanning it each time. */
	first_free_candidate = next_free(first_free_candidate, ram_pages);

	/*
	 * Finds the lowest fitting run.  The alignment and the boundary are
	 * of the physical address; a page's number and its address in
	 * either alias agree in every bit below the high alias's 2 GiB, so
	 * the number is aligned and bounded instead.
	 */
	start = first_free_candidate;
	for (;;) {
		/* The lowest free page, moved up to the alignment of its physical address. */
		start = next_free(start, end);
		aligned = ((uint64_t)start + align_pages - 1U) & ~((uint64_t)align_pages - 1U);
		if (aligned >= end)
			break;
		start = (uint32_t)aligned;
		if (need > end - start)
			break;

		/* A run does not go from the low alias's pages into the high alias's. */
		if (start < low_pages && need > low_pages - start && end > low_pages) {
			start = low_pages;
			continue;
		}

		/* A run crossing a boundary moves to the next boundary block. */
		if (boundary_pages != 0) {
			first_byte = (uint64_t)start * MIPS_PAGE_SIZE;
			last_byte = first_byte + (uint64_t)need * MIPS_PAGE_SIZE - 1U;
			if (first_byte / boundary != last_byte / boundary) {
				start = (uint32_t)(((first_byte / boundary + 1U) * boundary) / MIPS_PAGE_SIZE);
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
	if (aligned >= end)
		return HAL_ERR_NOMEM;
	if (need > end - start)
		return HAL_ERR_NOMEM;

	/* Succeeded: the run is the caller's. */
	claim_run(start, need);
	allocated_pages += need;
	*block = (hal_physaddr_t)page_physical(start);
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
	uint32_t first;
	uint32_t count;
	uint32_t index;
	int in_use;
	int reserved;
	bool found;

	/* Refuses an empty, unaligned or foreign release. */
	if (block == NULL || size == 0)
		return HAL_ERR_INVALID;
	if (((uint64_t)*block & (MIPS_PAGE_SIZE - 1U)) != 0)
		return HAL_ERR_INVALID;
	found = page_of(*block, &first);
	if (!found)
		return HAL_ERR_INVALID;

	/* Repeats the rounding the allocation applied; a block stays in its alias. */
	count = (uint32_t)((size + MIPS_PAGE_SIZE - 1) / MIPS_PAGE_SIZE);
	if (count > ram_pages - first)
		return HAL_ERR_INVALID;
	if (first < low_pages && count > low_pages - first)
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

/* Reports the physical address of a page by its number from the start of RAM. */
static uint64_t
page_physical(
	uint32_t page)
{
	uint64_t offset;

	/* The low alias's pages are at their offset, the rest in the high alias. */
	offset = (uint64_t)page * MIPS_PAGE_SIZE;
	if (page < low_pages)
		return offset;

	/* Succeeded: the page in the high alias. */
	return PAGE_HIGH_ALIAS + offset;
}

/*
 * Finds the number of the managed page a physical address is in.
 * Reports false for an address of no managed page, including the high
 * alias of the low alias's pages.
 */
static bool
page_of(
	uint64_t physical,
	uint32_t *page)
{
	uint64_t offset;

	/* The low alias's part. */
	if (physical < (uint64_t)low_pages * MIPS_PAGE_SIZE) {
		*page = (uint32_t)(physical / MIPS_PAGE_SIZE);
		return true;
	}

	/* The high alias's part, past the pages the low alias shows. */
	if (physical < PAGE_HIGH_ALIAS)
		return false;
	offset = physical - PAGE_HIGH_ALIAS;
	if (offset < (uint64_t)low_pages * MIPS_PAGE_SIZE || offset >= (uint64_t)ram_pages * MIPS_PAGE_SIZE)
		return false;

	/* Succeeded: the page's number. */
	*page = (uint32_t)(offset / MIPS_PAGE_SIZE);
	return true;
}
