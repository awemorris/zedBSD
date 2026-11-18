/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The riscv64 physical page allocator.
 *
 * Memory is a span from the lowest RAM bank's start to the highest one's
 * end, one bit per page, set when the page is in use or not RAM at all.
 * The kernel image, the device tree, the initial ramdisk, every
 * reservation the tree names and the RAM the UEFI firmware keeps are set
 * at boot and never freed.
 */

#include <hal/hal.h>
#include "asm.h"
#include "bootinfo.h"
#include "bsp.h"
#include "defs.h"
#include "page.h"
#include "space.h"
#include "spin.h"

/* The most RAM the bitmap can describe: 8 GiB, the largest board's. */
#define PAGE_MAX_BYTES		(8ULL * 1024ULL * 1024ULL * 1024ULL)
#define PAGE_MAX_PAGES		(PAGE_MAX_BYTES / RISCV64_PAGE_SIZE)
#define PAGE_BITMAP_WORDS	(PAGE_MAX_PAGES / 32U)

/* The bounds of the kernel image, from the linker script. */
extern char __kernel_vma_start[];
extern char __kernel_vma_end[];

/*
 * One bit per page of the span, set when the page is in use or not
 * available.  Kept under pmem_lock after boot.
 */
static uint32_t page_bitmap[PAGE_BITMAP_WORDS];

/*
 * One bit per page of the span, set when the page was never available, so
 * a free cannot return it.  Kept under pmem_lock after boot.
 */
static uint32_t reserved_bitmap[PAGE_BITMAP_WORDS];

/*
 * The span's first physical address.  Set once by riscv64_page_init()
 * before any allocation and constant afterwards, like ram_pages.
 */
static uint64_t ram_base;

/* The span's length in pages, set with ram_base. */
static uint32_t ram_pages;

/* The pages that were never available, for the memory statistics.  Kept under pmem_lock. */
static uint32_t reserved_pages;

/* The pages allocated now, for the memory statistics.  Kept under pmem_lock. */
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
static bool range_is_managed(uint64_t physical, uint64_t size);

/*
 * Builds the page bitmap from the device tree's memory and reservations.
 *
 * Every page starts in use; the RAM banks are released, then the kernel
 * image, the tree, the ramdisk and the reservations are taken again.
 */
void
riscv64_page_init(
	void)
{
	const struct riscv64_fdt_info *info;
	const struct riscv64_fdt_range *firmware;
	unsigned firmware_count;
	uint64_t lowest;
	uint64_t highest;
	uint64_t end;
	uint64_t kernel_start;
	uint64_t kernel_size;
	uint64_t fdt_phys;
	unsigned index;

	/* The HAL cannot run on a machine the tree gives no memory. */
	info = riscv64_boot_fdt();
	if (info->memory_count == 0)
		HAL_FATAL("riscv64 device tree describes no memory");

	/* Finds the span the RAM banks cover, widening an empty span to each bank. */
	lowest = ~0ULL;
	highest = 0;
	for (index = 0; index < info->memory_count; index++) {
		/* A bank that starts lower moves the span's start down. */
		end = info->memory[index].base + info->memory[index].size;
		if (info->memory[index].base < lowest)
			lowest = info->memory[index].base;

		/* A bank that ends higher moves the span's end up. */
		if (end > highest)
			highest = end;
	}

	/* Keeps the span page-aligned. */
	lowest &= ~((uint64_t)RISCV64_PAGE_SIZE - 1U);
	highest &= ~((uint64_t)RISCV64_PAGE_SIZE - 1U);

	/* Keeps it within what the bitmap can describe. */
	if (highest - lowest > PAGE_MAX_BYTES)
		highest = lowest + PAGE_MAX_BYTES;

	/* The kernel reaches managed RAM only through the direct map. */
	if (highest > RISCV64_DIRECT_LIMIT)
		HAL_FATAL("riscv64 memory beyond the direct map");

	/* Records the span. */
	ram_base = lowest;
	ram_pages = (uint32_t)((highest - lowest) / RISCV64_PAGE_SIZE);

	/* Starts with every page unavailable and none allocated. */
	hal_memset(page_bitmap, 0xff, sizeof(page_bitmap));
	hal_memset(reserved_bitmap, 0xff, sizeof(reserved_bitmap));
	reserved_pages = ram_pages;
	allocated_pages = 0;

	/* Releases the banks. */
	for (index = 0; index < info->memory_count; index++)
		release_range(info->memory[index].base, info->memory[index].size);

	/* Takes back the kernel image, which holds the boot tables and stacks. */
	kernel_start = riscv64_kernel_phys_start();
	kernel_size = (uint64_t)(uintptr_t)(__kernel_vma_end - __kernel_vma_start);
	reserve_range(kernel_start, kernel_size);

	/* Takes back the device tree. */
	fdt_phys = riscv64_boot_fdt_phys();
	reserve_range(fdt_phys, info->totalsize);

	/* Takes back the initial ramdisk the firmware loaded, when there is one. */
	if (info->initrd_end > info->initrd_start)
		reserve_range(info->initrd_start, info->initrd_end - info->initrd_start);

	/* Takes back every reservation, the firmware's own memory among them. */
	for (index = 0; index < info->reserved_count; index++)
		reserve_range(info->reserved[index].base, info->reserved[index].size);

	/* Takes back every no-map reservation too. */
	for (index = 0; index < info->nomap_count; index++)
		reserve_range(info->nomap[index].base, info->nomap[index].size);

	/* Takes back the RAM the UEFI firmware still uses, the screen it left on among it. */
	firmware = riscv64_boot_firmware_reserved(&firmware_count);
	for (index = 0; index < firmware_count; index++)
		reserve_range(firmware[index].base, firmware[index].size);

	/* The first allocation looks from the span's start. */
	first_free_candidate = 0;

	/* Reports the span and what of it is free. */
	hal_printf("RISCV64 MEMORY base=%lx total=%lu MiB free=%lu MiB\n",
		   ram_base,
		   (uint64_t)ram_pages * RISCV64_PAGE_SIZE / (1024U * 1024U),
		   ((uint64_t)ram_pages - reserved_pages) * RISCV64_PAGE_SIZE / (1024U * 1024U));
}

/*
 * Reports the first physical address of managed RAM.
 */
uint64_t
riscv64_ram_base(
	void)
{
	/* Succeeded: the span's start, fixed at boot. */
	return ram_base;
}

/*
 * Reports the end of managed RAM: the first address past the span.
 */
uint64_t
riscv64_ram_end(
	void)
{
	/* Succeeded: the span's end, fixed at boot. */
	return ram_base + (uint64_t)ram_pages * RISCV64_PAGE_SIZE;
}

/*
 * Reports whether a physical range lies entirely inside one RAM bank.
 */
bool
riscv64_range_is_ram(
	uint64_t physical,
	uint64_t size)
{
	const struct riscv64_fdt_info *info;
	uint64_t bank_end;
	unsigned index;

	/* Looks for a bank that holds the whole range. */
	info = riscv64_boot_fdt();
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

		/* This bank holds the whole range. */
		return true;
	}

	/* No bank holds the range. */
	return false;
}

/*
 * Reports whether a physical range overlaps any RAM bank.
 */
bool
riscv64_range_touches_ram(
	uint64_t physical,
	uint64_t size)
{
	const struct riscv64_fdt_info *info;
	uint64_t bank_end;
	uint64_t end;
	unsigned index;

	/* Compares the range with every bank. */
	info = riscv64_boot_fdt();
	end = physical + size;
	for (index = 0; index < info->memory_count; index++) {
		/* Skips a bank that ends before the range starts. */
		bank_end = info->memory[index].base + info->memory[index].size;
		if (bank_end <= physical)
			continue;

		/* Skips a bank that starts after the range ends. */
		if (info->memory[index].base >= end)
			continue;

		/* This bank overlaps the range. */
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
		alignment = RISCV64_PAGE_SIZE;

	/* Allocates with no reachability or boundary constraint. */
	enabled = riscv64_spin_lock(&pmem_lock);

	error = alloc_ram(req_size, alignment, ~(hal_physaddr_t)0, 0, block);

	riscv64_spin_unlock(&pmem_lock, enabled);

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
		alignment = RISCV64_PAGE_SIZE;

	/* Applies the caller's reachability and boundary constraints. */
	enabled = riscv64_spin_lock(&pmem_lock);

	error = alloc_ram(req_size, alignment, max_paddr, boundary, block);

	riscv64_spin_unlock(&pmem_lock, enabled);

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
	enabled = riscv64_spin_lock(&pmem_lock);

	error = free_ram(block, size);

	riscv64_spin_unlock(&pmem_lock, enabled);

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
	if ((uint64_t)paddr - ram_base >= (uint64_t)ram_pages * RISCV64_PAGE_SIZE)
		return NULL;

	/* Managed RAM is direct-mapped into the kernel half. */
	address = riscv64_phys_to_direct((uint64_t)paddr);

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
	return (size_t)ram_pages * RISCV64_PAGE_SIZE;
}

/*
 * Maps managed RAM a second time, uncached, through the board's alias.
 *
 * A board whose memory is also reached uncached at a fixed offset (the
 * JH7110's system port) offers the view: the range's cached lines are
 * written back and discarded first, then the alias of each of its pages
 * is made reachable in the device window, page by page, so no other
 * memory's alias becomes reachable with it.  A board without such an
 * alias refuses the view.
 *
 * The view is uncached memory, not ordinary memory: a caller uses plain
 * loads and stores on it, never LR/SC or AMO, whose behaviour on the
 * alias the platform does not promise.
 */
int
hal_pmem_map_uncached(
	hal_physaddr_t paddr,
	size_t size,
	void **vaddr)
{
	uint64_t offset;
	uint64_t alias;
	bool managed;
	int error;

	/* Refuses the view on a board without an uncached alias. */
	offset = riscv64_board_uncached_offset();
	if (offset == 0)
		return HAL_ERR_UNSUPPORTED;

	/* Requires a destination and a non-empty page-aligned range. */
	if (vaddr == NULL || size == 0)
		return HAL_ERR_INVALID;
	if (((uint64_t)paddr & (RISCV64_PAGE_SIZE - 1U)) != 0)
		return HAL_ERR_INVALID;
	if (((uint64_t)size & (RISCV64_PAGE_SIZE - 1U)) != 0)
		return HAL_ERR_INVALID;

	/* Refuses a range that is not managed RAM. */
	managed = range_is_managed((uint64_t)paddr, (uint64_t)size);
	if (!managed)
		return HAL_ERR_INVALID;

	/* Refuses an alias the device window does not reach. */
	alias = (uint64_t)paddr + offset;
	if (alias >= RISCV64_DEVICE_LIMIT || (uint64_t)size > RISCV64_DEVICE_LIMIT - alias)
		return HAL_ERR_INVALID;

	/* Writes back and discards the range's lines, so none can overwrite the alias's stores. */
	riscv64_board_flush_physical((uint64_t)paddr, (uint64_t)size);

	/* Makes the alias of each page of the range reachable. */
	error = riscv64_space_map_alias(alias, (uint64_t)size);
	if (error != HAL_OK)
		return error;

	/* Succeeded: the range is reachable uncached in the device window. */
	*vaddr = (void *)(uintptr_t)(RISCV64_DEVICE_BASE + alias);
	return HAL_OK;
}

/*
 * Ends the use of an uncached view made by hal_pmem_map_uncached().
 *
 * The range's lines are written back and discarded, so a line the direct
 * map brought in speculatively while the view was used is gone and the
 * direct map reads what is in RAM.  The alias's page leaves stay: they
 * reach only this RAM, which holds no cache state through them, so no
 * TLB shootdown is needed.
 */
int
hal_pmem_unmap_uncached(
	void *vaddr,
	size_t size)
{
	uint64_t address;
	uint64_t offset;
	uint64_t physical;
	bool managed;

	/* Refuses the call on a board that never makes the view. */
	offset = riscv64_board_uncached_offset();
	if (offset == 0)
		return HAL_ERR_UNSUPPORTED;

	/* Requires a non-empty range. */
	address = (uint64_t)(uintptr_t)vaddr;
	if (size == 0)
		return HAL_ERR_INVALID;

	/* Refuses an address outside the device window. */
	if (address < RISCV64_DEVICE_BASE || address - RISCV64_DEVICE_BASE >= RISCV64_DEVICE_LIMIT)
		return HAL_ERR_INVALID;

	/* Turns the alias back into the RAM it shows. */
	if (address - RISCV64_DEVICE_BASE < offset)
		return HAL_ERR_INVALID;
	physical = address - RISCV64_DEVICE_BASE - offset;

	/* Refuses an address that is not the alias of managed RAM. */
	managed = range_is_managed(physical, (uint64_t)size);
	if (!managed)
		return HAL_ERR_INVALID;

	/* Writes back and discards the range's lines. */
	riscv64_board_flush_physical(physical, (uint64_t)size);

	/* Succeeded: the range is reachable through the direct map alone. */
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

	/* Starts from an empty report. */
	hal_memset(stat, 0, sizeof(*stat));

	/* Samples the page counters together. */
	enabled = riscv64_spin_lock(&pmem_lock);

	stat->physical_total = (size_t)ram_pages * RISCV64_PAGE_SIZE;
	stat->physical_reserved = (size_t)reserved_pages * RISCV64_PAGE_SIZE;
	stat->physical_allocated = (size_t)allocated_pages * RISCV64_PAGE_SIZE;

	riscv64_spin_unlock(&pmem_lock, enabled);

	/* The free pages are what is neither reserved nor allocated. */
	stat->physical_free = stat->physical_total - stat->physical_reserved - stat->physical_allocated;

	/* Adds the task stacks and the page tables. */
	riscv64_task_memory_stats(&stat->task_count, &stat->task_stack_bytes);
	riscv64_space_memory_stats(&stat->space_count, &stat->page_table_count);
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
	span_end = ram_base + (uint64_t)ram_pages * RISCV64_PAGE_SIZE;
	end = base + size;
	if (base < ram_base)
		base = ram_base;
	if (end > span_end)
		end = span_end;

	/* A range with nothing inside the span releases nothing. */
	if (size == 0 || base >= end)
		return;

	/* Releases each page wholly inside the range. */
	first = (base - ram_base + RISCV64_PAGE_SIZE - 1U) / RISCV64_PAGE_SIZE;
	last = (end - ram_base) / RISCV64_PAGE_SIZE;
	for (page = first; page < last; page++) {
		/* A page released already, by an overlapping bank, stays as it is. */
		reserved = page_is_set(reserved_bitmap, (uint32_t)page);
		if (!reserved)
			continue;

		/* Makes the page free and stops counting it as reserved. */
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
	span_end = ram_base + (uint64_t)ram_pages * RISCV64_PAGE_SIZE;
	end = base + size;
	if (base < ram_base)
		base = ram_base;
	if (end > span_end)
		end = span_end;

	/* A range with nothing inside the span reserves nothing. */
	if (size == 0 || base >= end)
		return;

	/* Reserves each page the range touches. */
	first = (base - ram_base) / RISCV64_PAGE_SIZE;
	last = (end - ram_base + RISCV64_PAGE_SIZE - 1U) / RISCV64_PAGE_SIZE;
	for (page = first; page < last; page++) {
		/* A page already out of use stays as it is. */
		in_use = page_is_set(page_bitmap, (uint32_t)page);
		if (in_use)
			continue;

		/* Takes the page out of use for good and counts it as reserved. */
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
	/* A clear bit is a page that is not set. */
	if ((bitmap[page >> 5] & (1U << (page & 31U))) == 0)
		return 0;

	/* Succeeded: the page's bit is set. */
	return 1;
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

	/* An empty interval has no free page. */
	if (page >= end)
		return end;

	/* Bits below the page in its first word count as in use. */
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

	/* An empty interval has no page in use. */
	if (page >= end)
		return end;

	/* Bits below the page in its first word count as free. */
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

	/* Single pages up to a word boundary. */
	end = first + count;
	while (first < end && (first & 31U) != 0) {
		page_set(page_bitmap, first);
		first++;
	}

	/* Whole words. */
	while (end - first >= 32U) {
		page_bitmap[first >> 5] = UINT32_MAX;
		first += 32U;
	}

	/* The tail's pages. */
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
	if (size > SIZE_MAX - (RISCV64_PAGE_SIZE - 1))
		return HAL_ERR_INVALID;
	if (alignment < RISCV64_PAGE_SIZE)
		return HAL_ERR_INVALID;
	if ((alignment & (alignment - 1U)) != 0)
		return HAL_ERR_INVALID;

	/* Refuses a boundary that is not a power of two. */
	if (boundary != 0 && (boundary & (boundary - 1U)) != 0)
		return HAL_ERR_INVALID;

	/* Rounds the request up to pages, as the free repeats it. */
	need = (uint32_t)((size + RISCV64_PAGE_SIZE - 1) / RISCV64_PAGE_SIZE);
	align_pages = (uint32_t)(alignment / RISCV64_PAGE_SIZE);

	/* A caller that cannot address the span's start gets nothing. */
	if ((uint64_t)max_paddr < ram_base)
		return HAL_ERR_NOMEM;

	/* Limits the search to the pages the caller can address. */
	end = ram_pages;
	if ((uint64_t)max_paddr - ram_base + 1U < (uint64_t)ram_pages * RISCV64_PAGE_SIZE)
		end = (uint32_t)(((uint64_t)max_paddr - ram_base + 1U) / RISCV64_PAGE_SIZE);

	/* Refuses a run longer than the pages left to search. */
	if (need > end)
		return HAL_ERR_NOMEM;

	/* A run longer than the boundary block crosses it wherever it starts. */
	boundary_pages = 0;
	if (boundary != 0) {
		if ((uint64_t)need * RISCV64_PAGE_SIZE > boundary)
			return HAL_ERR_NOMEM;

		/* The boundary block in pages, for the search. */
		boundary_pages = (uint32_t)(boundary / RISCV64_PAGE_SIZE);
	}

	/* Reuses the known prefix in use rather than scanning it each time. */
	first_free_candidate = next_free(first_free_candidate, ram_pages);

	/*
	 * Finds the lowest fitting run.  The alignment is of the physical
	 * address, so the span's first page is taken into account.
	 */
	base_page = ram_base / RISCV64_PAGE_SIZE;
	start = first_free_candidate;
	for (;;) {
		/* The lowest free page, moved up to the alignment of its physical address. */
		start = next_free(start, end);
		aligned = (base_page + start + align_pages - 1U) & ~((uint64_t)align_pages - 1U);
		if (aligned - base_page >= end)
			break;

		/* Stops when the run no longer fits before the end. */
		start = (uint32_t)(aligned - base_page);
		if (need > end - start)
			break;

		/* A run crossing a boundary moves to the next boundary block. */
		if (boundary_pages != 0) {
			first_byte = ram_base + (uint64_t)start * RISCV64_PAGE_SIZE;
			last_byte = first_byte + (uint64_t)need * RISCV64_PAGE_SIZE - 1U;
			if (first_byte / boundary != last_byte / boundary) {
				start = (uint32_t)(((first_byte / boundary + 1U) * boundary - ram_base) / RISCV64_PAGE_SIZE);
				continue;
			}
		}

		/* The run fits when nothing in it is in use; else the search goes past what is. */
		occupied = next_occupied(start, start + need);
		if (occupied == start + need)
			break;

		/* Looks again past the page in use. */
		start = occupied + 1U;
	}

	/* Reports that no run fits. */
	if (aligned - base_page >= end)
		return HAL_ERR_NOMEM;
	if (need > end - start)
		return HAL_ERR_NOMEM;

	/* Claims the run and counts its pages. */
	claim_run(start, need);
	allocated_pages += need;

	/* Reports the run's physical address. */
	*block = (hal_physaddr_t)(ram_base + (uint64_t)start * RISCV64_PAGE_SIZE);

	/* Succeeded: the run is the caller's. */
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
	if (((uint64_t)*block & (RISCV64_PAGE_SIZE - 1U)) != 0)
		return HAL_ERR_INVALID;
	if ((uint64_t)*block < ram_base)
		return HAL_ERR_INVALID;

	/* Refuses a block past the span. */
	offset = (uint64_t)*block - ram_base;
	if (offset / RISCV64_PAGE_SIZE >= ram_pages)
		return HAL_ERR_INVALID;

	/* Repeats the rounding the allocation applied; the pages must lie in the span. */
	first = (uint32_t)(offset / RISCV64_PAGE_SIZE);
	count = (uint32_t)((size + RISCV64_PAGE_SIZE - 1) / RISCV64_PAGE_SIZE);
	if (count > ram_pages - first)
		return HAL_ERR_INVALID;

	/* Checks that every page was allocated, before any changes. */
	for (index = 0; index < count; index++) {
		/* A free page was not allocated. */
		in_use = page_is_set(page_bitmap, first + index);
		if (!in_use)
			return HAL_ERR_STATE;

		/* A reserved page was never allocated either. */
		reserved = page_is_set(reserved_bitmap, first + index);
		if (reserved)
			return HAL_ERR_STATE;
	}

	/* Releases the pages. */
	for (index = 0; index < count; index++)
		page_clear(page_bitmap, first + index);

	/* Stops counting them as allocated. */
	allocated_pages -= count;

	/* A later allocation looks at the released pages again. */
	if (first < first_free_candidate)
		first_free_candidate = first;

	/* Clears the caller's address so a double free shows. */
	*block = 0;

	/* Succeeded: the pages are free. */
	return HAL_OK;
}

/* Reports whether a range lies wholly in the span of managed RAM. */
static bool
range_is_managed(
	uint64_t physical,
	uint64_t size)
{
	uint64_t span;

	/* Refuses a range below the span or one that wraps. */
	if (physical < ram_base || physical > ~0ULL - size)
		return false;

	/* Refuses a range that runs past the span's end. */
	span = (uint64_t)ram_pages * RISCV64_PAGE_SIZE;
	if (physical - ram_base > span || size > span - (physical - ram_base))
		return false;

	/* Succeeded: the range is managed RAM. */
	return true;
}
