#include <hal/hal.h>
#include "asm.h"
#include "bsp.h"
#include "defs.h"
#include "space.h"
#include "spin.h"

#define ARM64_MAX_PHYS (8ULL * 1024ULL * 1024ULL * 1024ULL)
#define MAX_PHYS_PAGES (ARM64_MAX_PHYS / ARM64_PAGE_SIZE)
#define BITMAP_WORDS (MAX_PHYS_PAGES / 32U)
#define BIT_GET(map, n) ((map)[(n) >> 5] & (1U << ((n) & 31U)))
#define BIT_SET(map, n) ((map)[(n) >> 5] |= 1U << ((n) & 31U))
#define BIT_CLEAR(map, n) ((map)[(n) >> 5] &= ~(1U << ((n) & 31U)))

extern char __kernel_vma_start[], __kernel_vma_end[];
static uint32_t page_bitmap[BITMAP_WORDS];
static uint32_t reserved_bitmap[BITMAP_WORDS];
static uint32_t phys_pages;
static uint32_t reserved_pages;
static uint32_t allocated_pages;

/*
 * All pages below this candidate are occupied.  Allocation advances it only
 * across occupied bits; freeing an earlier run rewinds it.  Kept under
 * pmem_lock with the bitmaps and counters.
 */
static uint32_t first_free_candidate;

/*
 * Guards the bitmaps, the counters and the cursor after boot, when any CPU
 * may allocate or free.  Zero when free.
 */
static volatile unsigned pmem_lock;

static uint32_t next_free(uint32_t page, uint32_t end);
static uint32_t next_occupied(uint32_t page, uint32_t end);
static void claim_run(uint32_t first, uint32_t count);

static void
release_range(uint64_t base, uint64_t size)
{
	uint64_t limit = (uint64_t)phys_pages * ARM64_PAGE_SIZE;
	uint64_t end;
	uint32_t first, last, page;
	if (size == 0 || base >= limit) return;
	end = size > limit - base ? limit : base + size;
	first = (uint32_t)((base + ARM64_PAGE_SIZE - 1) / ARM64_PAGE_SIZE);
	last = (uint32_t)(end / ARM64_PAGE_SIZE);
	for (page = first; page < last; page++) {
		if (BIT_GET(page_bitmap, page)) {
			BIT_CLEAR(page_bitmap, page);
			BIT_CLEAR(reserved_bitmap, page);
			reserved_pages--;
		}
	}
}

static void
reserve_range(uint64_t base, uint64_t size)
{
	uint64_t limit = (uint64_t)phys_pages * ARM64_PAGE_SIZE, end;
	uint32_t first, last, page;
	if (size == 0 || base >= limit) return;
	end = size > limit - base ? limit : base + size;
	first = (uint32_t)(base / ARM64_PAGE_SIZE);
	last = (uint32_t)((end + ARM64_PAGE_SIZE - 1) / ARM64_PAGE_SIZE);
	if (last > phys_pages) last = phys_pages;
	for (page = first; page < last; page++) {
		if (!BIT_GET(page_bitmap, page)) {
			BIT_SET(page_bitmap, page);
			BIT_SET(reserved_bitmap, page);
			reserved_pages++;
		}
	}
}

void
arm64_page_init(void)
{
	const struct arm64_fdt_info *info;
	uint64_t top;
	unsigned i;

	/* Finds the top of RAM the device tree lists. */
	info = arm64_boot_info();
	top = 0;
	for (i = 0; i < info->memory_count; i++) {
		uint64_t end = info->memory[i].base + info->memory[i].size;
		if (end > top) top = end;
	}
	/* An unpatched standalone DTB has a zero-sized memory node in QEMU. */
	if (top == 0) {
		top = 2ULL * 1024ULL * 1024ULL * 1024ULL;
		hal_puts(ARM64_BOARD_TAG " FDT memory unavailable; QEMU 2 GiB fallback\n");
	}
	if (top > ARM64_MAX_PHYS) top = ARM64_MAX_PHYS;
	phys_pages = (uint32_t)(top / ARM64_PAGE_SIZE);
	hal_memset(page_bitmap, 0xff, sizeof(page_bitmap));
	hal_memset(reserved_bitmap, 0xff, sizeof(reserved_bitmap));
	reserved_pages = phys_pages;
	allocated_pages = 0;
	if (info->memory_count == 0) release_range(0, top);
	for (i = 0; i < info->memory_count; i++)
		release_range(info->memory[i].base, info->memory[i].size);
	reserve_range(0, 0x100000);
	reserve_range(arm64_direct_to_phys(__kernel_vma_start),
	    (uintptr_t)(__kernel_vma_end - __kernel_vma_start));
	reserve_range(arm64_boot_fdt_phys(), info->totalsize);
	for (i = 0; i < info->reserved_count; i++)
		reserve_range(info->reserved[i].base, info->reserved[i].size);

	/* The first allocation skips the reserved prefix once, after boot reservations. */
	first_free_candidate = 1;
	hal_printf("ARM64 MEMORY MAP PASS total=%llu MiB free=%llu MiB\n",
	    (uint64_t)phys_pages * ARM64_PAGE_SIZE / (1024 * 1024),
	    ((uint64_t)phys_pages - reserved_pages) * ARM64_PAGE_SIZE / (1024 * 1024));
}

/*
 * Finds and claims an aligned run of free pages.
 *
 * max_paddr is the highest byte the caller can address, and boundary,
 * when not zero, is a power-of-two block the run must not cross.
 */
static int
alloc_ram(
	size_t size,
	size_t alignment,
	hal_physaddr_t max_paddr,
	size_t boundary,
	hal_physaddr_t *block)
{
	uint32_t need, end, start, align_pages, boundary_pages, occupied;
	uint64_t state, first_byte, last_byte;
	uintptr_t limit = (uintptr_t)phys_pages * ARM64_PAGE_SIZE;

	/* Requires a destination and a representable request. */
	if (block == NULL || size == 0 ||
	    size > SIZE_MAX - (ARM64_PAGE_SIZE - 1) ||
	    alignment < ARM64_PAGE_SIZE)
		return HAL_ERR_INVALID;

	/* Rejects a boundary which is not a power of two. */
	if (boundary != 0 && (boundary & (boundary - 1U)) != 0)
		return HAL_ERR_INVALID;

	/* Rounds the request up so the free path can repeat this. */
	need = (uint32_t)((size + ARM64_PAGE_SIZE - 1) / ARM64_PAGE_SIZE);
	align_pages = (uint32_t)(alignment / ARM64_PAGE_SIZE);

	/* Limits the search to pages the caller can address. */
	end = (uint32_t)(limit / ARM64_PAGE_SIZE);
	if (max_paddr < (hal_physaddr_t)limit)
		end = (uint32_t)((max_paddr + 1U) / ARM64_PAGE_SIZE);
	if (need == 0 || need >= end)
		return HAL_ERR_NOMEM;

	/* A run longer than the boundary block crosses it wherever it starts. */
	boundary_pages = 0;
	if (boundary != 0) {
		if ((uint64_t)need * ARM64_PAGE_SIZE > boundary)
			return HAL_ERR_NOMEM;
		boundary_pages = (uint32_t)(boundary / ARM64_PAGE_SIZE);
	}

	state = arm64_irq_save();

	/* Reuses the known occupied prefix rather than rescanning it for every small allocation. */
	first_free_candidate = next_free(first_free_candidate, phys_pages);

	/*
	 * Finds the lowest fitting run, skipping only candidates that cannot
	 * fit: occupied pages a word at a time, unaligned starts, starts whose
	 * run crosses a boundary (every later start in the same block does
	 * too), and every start whose run holds the occupied page found.
	 */
	start = first_free_candidate;
	for (;;) {
		/* The lowest free page at or above the candidate, then the alignment (a power of two rounds up at once). */
		start = next_free(start, end);
		if (align_pages != 0 && (align_pages & (align_pages - 1U)) == 0) {
			start = (start + align_pages - 1U) & ~(align_pages - 1U);
		} else {
			while (start < end && (start & (align_pages - 1U)) != 0)
				start++;
		}
		if (start >= end || need > end - start)
			break;

		/* A run crossing a boundary moves to the next boundary block. */
		if (boundary_pages != 0) {
			first_byte = (uint64_t)start * ARM64_PAGE_SIZE;
			last_byte = first_byte + (uint64_t)need * ARM64_PAGE_SIZE - 1U;
			if (first_byte / boundary != last_byte / boundary) {
				start = (start / boundary_pages + 1U) * boundary_pages;
				continue;
			}
		}

		/* The run fits when it holds no occupied page; otherwise the next candidate follows that page. */
		occupied = next_occupied(start, start + need);
		if (occupied == start + need)
			break;
		start = occupied + 1U;
	}

	/* Reports exhaustion when no candidate run remains. */
	if (start >= end || need > end - start) {
		arm64_irq_restore(state);
		return HAL_ERR_NOMEM;
	}

	/* Claims and accounts for every page in the selected run. */
	claim_run(start, need);
	allocated_pages += need;
	arm64_irq_restore(state);

	/* Publishes the allocated physical address. */
	*block = (hal_physaddr_t)start * ARM64_PAGE_SIZE;
	return HAL_OK;
}

/* Finds the lowest free page in [page, end), or end; whole occupied words are skipped at once. */
static uint32_t
next_free(
	uint32_t page,
	uint32_t end)
{
	uint32_t word;
	uint32_t found;

	/* Bits below the page in its first word count as occupied. */
	if (page >= end)
		return end;
	word = page_bitmap[page >> 5] | ((1U << (page & 31U)) - 1U);
	page &= ~31U;

	/* Skips whole occupied words up to the end. */
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

	/* Succeeded: found is free. */
	return found;
}

/* Finds the lowest occupied page in [page, end), or end; whole free words are skipped at once. */
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

	/* Succeeded: found is occupied. */
	return found;
}

/* Marks a free run occupied, whole words at a time; the caller holds the IRQ exclusion. */
static void
claim_run(
	uint32_t first,
	uint32_t count)
{
	uint32_t end;

	/* Single pages up to the first word boundary, whole words, then the tail's pages. */
	end = first + count;
	while (first < end && (first & 31U) != 0) {
		BIT_SET(page_bitmap, first);
		first++;
	}
	while (end - first >= 32U) {
		page_bitmap[first >> 5] = UINT32_MAX;
		first += 32U;
	}
	while (first < end) {
		BIT_SET(page_bitmap, first);
		first++;
	}

	/* Succeeded: every page of the run is occupied. */
	return;
}

/*
 * Releases the pages of one allocation.
 *
 * size is the size the matching allocation requested, so the same
 * rounding is repeated here and a smaller size splits the block.
 */
static int
free_ram(
	hal_physaddr_t *block,
	size_t size)
{
	uint32_t first, count, i;
	uint64_t state;

	/* Rejects an empty or misaligned release. */
	if (block == NULL || size == 0 ||
	    ((uintptr_t)*block & (ARM64_PAGE_SIZE - 1)) != 0)
		return HAL_ERR_INVALID;

	/* Repeats the rounding the allocation applied. */
	first = (uint32_t)((uintptr_t)*block / ARM64_PAGE_SIZE);
	count = (uint32_t)((size + ARM64_PAGE_SIZE - 1) / ARM64_PAGE_SIZE);
	if (first >= phys_pages || count > phys_pages - first)
		return HAL_ERR_INVALID;
	state = arm64_irq_save();

	/* Verifies ownership of every page before changing the bitmap. */
	for (i = 0; i < count; i++) {
		if (!BIT_GET(page_bitmap, first + i) ||
		    BIT_GET(reserved_bitmap, first + i)) {
			arm64_irq_restore(state);
			return HAL_ERR_STATE;
		}
	}

	/* Releases and accounts for every page in the interval. */
	for (i = 0; i < count; i++)
		BIT_CLEAR(page_bitmap, first + i);
	allocated_pages -= count;

	/* A later allocation must revisit newly released low pages, including constrained DMA requests. */
	if (first < first_free_candidate)
		first_free_candidate = first;
	arm64_irq_restore(state);

	/* Clears the caller address so a double free is visible. */
	*block = 0;
	return HAL_OK;
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
		alignment = ARM64_PAGE_SIZE;

	/* Allocates with no reachability or boundary constraint. */
	enabled = arm64_spin_lock(&pmem_lock);

	error = alloc_ram(req_size, alignment, UINT64_MAX, 0, block);

	arm64_spin_unlock(&pmem_lock, enabled);

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
		alignment = ARM64_PAGE_SIZE;

	/* Applies the caller's reachability and boundary constraints. */
	enabled = arm64_spin_lock(&pmem_lock);

	error = alloc_ram(req_size, alignment, max_paddr, boundary, block);

	arm64_spin_unlock(&pmem_lock, enabled);

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
	enabled = arm64_spin_lock(&pmem_lock);

	error = free_ram(block, size);

	arm64_spin_unlock(&pmem_lock, enabled);

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
	/* Reports no alias for an address outside managed RAM. */
	if (paddr >= (hal_physaddr_t)phys_pages * ARM64_PAGE_SIZE)
		return NULL;

	/* Managed RAM is direct-mapped into the kernel half. */
	return arm64_phys_to_direct((uintptr_t)paddr);
}

/*
 * Reports the total managed physical-memory size.
 */
size_t
hal_pmem_get_total_size(void)
{
	/* Returns the managed page count in bytes. */
	return (size_t)phys_pages * ARM64_PAGE_SIZE;
}

void __attribute__((weak)) hal_arm64_task_memory_stats(uint32_t *c,size_t *s)
{ if(c)*c=0; if(s)*s=0; }
void hal_arm64_space_memory_stats(uint32_t *,uint32_t *);

/*
 * Collects arm64 physical, task, and address-space statistics.
 */
void
hal_get_memstat(
	struct hal_memstat *s)
{
	bool enabled;

	/* Ignores requests without result storage. */
	if (!s)
		return;

	/* Samples the physical-page counters together. */
	hal_memset(s, 0, sizeof(*s));
	enabled = arm64_spin_lock(&pmem_lock);

	s->physical_total = (size_t)phys_pages * ARM64_PAGE_SIZE;
	s->physical_reserved = (size_t)reserved_pages * ARM64_PAGE_SIZE;
	s->physical_allocated = (size_t)allocated_pages * ARM64_PAGE_SIZE;

	arm64_spin_unlock(&pmem_lock, enabled);

	s->physical_free = s->physical_total - s->physical_reserved -
	    s->physical_allocated;

	/* Adds task-stack and page-table ownership counters. */
	hal_arm64_task_memory_stats(&s->task_count, &s->task_stack_bytes);
	hal_arm64_space_memory_stats(&s->space_count, &s->page_table_count);
}
