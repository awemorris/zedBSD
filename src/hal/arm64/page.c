#include <hal/hal.h>
#include "asm.h"
#include "bsp.h"
#include "defs.h"
#include "space.h"

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
	const struct rpi4_fdt_info *info = rpi4_boot_info();
	uint64_t top = 0;
	unsigned i;
	for (i = 0; i < info->memory_count; i++) {
		uint64_t end = info->memory[i].base + info->memory[i].size;
		if (end > top) top = end;
	}
	/* An unpatched standalone DTB has a zero-sized memory node in QEMU. */
	if (top == 0) {
		top = 2ULL * 1024ULL * 1024ULL * 1024ULL;
		hal_puts("RPI4 FDT memory unavailable; QEMU 2 GiB fallback\n");
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
	reserve_range(rpi4_boot_fdt_phys(), info->totalsize);
	for (i = 0; i < info->reserved_count; i++)
		reserve_range(info->reserved[i].base, info->reserved[i].size);
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
	uint32_t need, end, start, index, align_pages;
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

	state = arm64_irq_save();

	/* Searches for an aligned free run meeting every constraint. */
	for (start = 1; start + need <= end; start++) {
		/* Skips candidates which do not meet the alignment. */
		if ((start & (align_pages - 1U)) != 0)
			continue;

		/* Skips a run which would cross a forbidden boundary. */
		if (boundary != 0) {
			first_byte = (uint64_t)start * ARM64_PAGE_SIZE;
			last_byte = first_byte +
			    (uint64_t)need * ARM64_PAGE_SIZE - 1U;
			if (first_byte / boundary != last_byte / boundary)
				continue;
		}

		/* Measures the free run beginning at this candidate. */
		for (index = 0;
		     index < need && !BIT_GET(page_bitmap, start + index);
		     index++)
			;
		if (index == need)
			break;
		start += index;
	}

	/* Reports exhaustion when no candidate run remains. */
	if (start + need > end) {
		arm64_irq_restore(state);
		return HAL_ERR_NOMEM;
	}

	/* Claims and accounts for every page in the selected run. */
	for (index = 0; index < need; index++)
		BIT_SET(page_bitmap, start + index);
	allocated_pages += need;
	arm64_irq_restore(state);

	/* Publishes the allocated physical address. */
	*block = (hal_physaddr_t)start * ARM64_PAGE_SIZE;
	return HAL_OK;
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
	/* Allocates with no reachability or boundary constraint. */
	return alloc_ram(req_size,
			 req_align == 0 ? ARM64_PAGE_SIZE : req_align,
			 UINT64_MAX, 0, block);
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
	/* Applies the caller's reachability and boundary constraints. */
	return alloc_ram(req_size,
			 req_align == 0 ? ARM64_PAGE_SIZE : req_align,
			 max_paddr, boundary, block);
}

/*
 * Frees one physical RAM block.
 */
int
hal_pmem_free(
	hal_physaddr_t *block,
	size_t size)
{
	/* Releases the pages the matching allocation claimed. */
	return free_ram(block, size);
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
	/* Ignores requests without result storage. */
	if (!s)
		return;

	/* Samples the physical-page counters. */
	hal_memset(s, 0, sizeof(*s));
	s->physical_total = (size_t)phys_pages * ARM64_PAGE_SIZE;
	s->physical_reserved = (size_t)reserved_pages * ARM64_PAGE_SIZE;
	s->physical_allocated = (size_t)allocated_pages * ARM64_PAGE_SIZE;
	s->physical_free = s->physical_total - s->physical_reserved -
	    s->physical_allocated;

	/* Adds task-stack and page-table ownership counters. */
	hal_arm64_task_memory_stats(&s->task_count, &s->task_stack_bytes);
	hal_arm64_space_memory_stats(&s->space_count, &s->page_table_count);
}
