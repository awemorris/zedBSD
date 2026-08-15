/* MC68030/X68000 physical page allocator. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <hal/hal.h>
#include "bsp.h"
#include "../space.h"

#define X68K_MAX_RAM_BYTES 0x00c00000U
#define X68K_MAX_PAGES     (X68K_MAX_RAM_BYTES / M68K030_PAGE_SIZE)
#define X68K_BITMAP_WORDS  (X68K_MAX_PAGES / 32U)
#define BIT_GET(map, bit)  ((map)[(bit) >> 5] & (1U << ((bit) & 31U)))
#define BIT_SET(map, bit)  ((map)[(bit) >> 5] |= (1U << ((bit) & 31U)))
#define BIT_CLEAR(map, bit) ((map)[(bit) >> 5] &= ~(1U << ((bit) & 31U)))

static uint32_t used[X68K_BITMAP_WORDS];
static uint32_t reserved[X68K_BITMAP_WORDS];
static uint32_t physical_pages;
static uint32_t reserved_pages;
static uint32_t allocated_pages;

static void
release_available(uint32_t base, uint32_t size)
{
	uint64_t limit = (uint64_t)physical_pages * M68K030_PAGE_SIZE;
	uint64_t end;
	uint32_t first, last, page;

	if (size == 0 || base >= limit)
		return;
	end = (uint64_t)base + size;
	if (end > limit)
		end = limit;
	first = (base + M68K030_PAGE_MASK) / M68K030_PAGE_SIZE;
	last = (uint32_t)(end / M68K030_PAGE_SIZE);
	for (page = first; page < last; page++) {
		if (BIT_GET(used, page)) {
			BIT_CLEAR(used, page);
			BIT_CLEAR(reserved, page);
			reserved_pages--;
		}
	}
}

static void
reserve_range(uintptr_t base, size_t size)
{
	uint64_t limit = (uint64_t)physical_pages * M68K030_PAGE_SIZE;
	uint64_t end;
	uint32_t first, last, page;

	if (size == 0 || base >= limit)
		return;
	end = (uint64_t)base + size;
	if (end > limit)
		end = limit;
	first = (uint32_t)(base / M68K030_PAGE_SIZE);
	last = (uint32_t)((end + M68K030_PAGE_MASK) /
		M68K030_PAGE_SIZE);
	if (last > physical_pages)
		last = physical_pages;
	for (page = first; page < last; page++) {
		if (!BIT_GET(used, page)) {
			BIT_SET(used, page);
			reserved_pages++;
		}
		BIT_SET(reserved, page);
	}
}

void
m68k030_page_init(void)
{
	const struct zedbsd_x68k_handoff *handoff = x68k_boot_handoff();
	uint32_t bytes = handoff->ram_bytes;
	unsigned index;

	if (bytes > X68K_MAX_RAM_BYTES)
		bytes = X68K_MAX_RAM_BYTES;
	physical_pages = bytes / M68K030_PAGE_SIZE;
	if (physical_pages < 512U)
		HAL_FATAL("X68k requires at least 2 MiB RAM");
	hal_memset(used, 0xff, sizeof(used));
	hal_memset(reserved, 0xff, sizeof(reserved));
	reserved_pages = physical_pages;
	allocated_pages = 0;

	for (index = 0; index < handoff->memory_region_count &&
	    index < ZEDBSD_X68K_MAX_MEMORY_REGIONS; index++) {
		const struct zedbsd_memory_region32 *region =
			&handoff->memory_regions[index];
		if (region->type == ZEDBSD_MEMORY_AVAILABLE &&
		    region->base <= UINT32_MAX - region->size)
			release_available(region->base, region->size);
	}

	/* Never allocate the exception vectors, loader/handoff/bootstrap, or any
	 * part of the ELF load span.  Page zero remains permanently invalid. */
	reserve_range(0, M68K030_PAGE_SIZE);
	/* x68k_boot_init() validated ordering before making this handoff visible;
	 * retain defensive guards so subtraction can never wrap independently. */
	if (handoff->loader_phys_end > handoff->loader_phys_start)
		reserve_range(handoff->loader_phys_start,
			handoff->loader_phys_end - handoff->loader_phys_start);
	if (handoff->kernel_phys_end > handoff->kernel_phys_start)
		reserve_range(handoff->kernel_phys_start,
			handoff->kernel_phys_end - handoff->kernel_phys_start);
	for (index = 0; index < handoff->memory_region_count &&
	    index < ZEDBSD_X68K_MAX_MEMORY_REGIONS; index++) {
		const struct zedbsd_memory_region32 *region =
			&handoff->memory_regions[index];
		if (region->type == ZEDBSD_MEMORY_RESERVED &&
		    region->base <= UINT32_MAX - region->size)
			reserve_range(region->base, region->size);
	}
	hal_printf("X68k RAM: %u KiB, free %u KiB\n", bytes / 1024U,
		(physical_pages - reserved_pages) *
		M68K030_PAGE_SIZE / 1024U);
}

void
pmem_reserve(hal_physaddr_t physical, size_t size)
{
	reserve_range(physical, size);
}

static int
allocate_pages(size_t size, uintptr_t above, uintptr_t below,
	       struct pmem_desc *descriptor)
{
	uintptr_t limit = (uintptr_t)physical_pages * M68K030_PAGE_SIZE;
	uint32_t needed, first, end, start, index;
	bool enabled;

	if (descriptor == NULL || size == 0 ||
	    size > SIZE_MAX - M68K030_PAGE_MASK || above >= below)
		return PMEM_BADDESC;
	if (below > limit)
		below = limit;
	needed = (uint32_t)((size + M68K030_PAGE_MASK) /
		M68K030_PAGE_SIZE);
	first = (uint32_t)((above + M68K030_PAGE_MASK) /
		M68K030_PAGE_SIZE);
	end = (uint32_t)(below / M68K030_PAGE_SIZE);
	if (needed == 0 || first >= end || needed > end - first)
		return PMEM_NOSPACE;

	enabled = hal_irq_disable();
	for (start = first; start + needed <= end; start++) {
		for (index = 0; index < needed &&
		    !BIT_GET(used, start + index); index++)
			;
		if (index == needed)
			break;
		start += index;
	}
	if (start + needed > end) {
		if (enabled)
			hal_irq_enable();
		return PMEM_NOSPACE;
	}
	for (index = 0; index < needed; index++)
		BIT_SET(used, start + index);
	allocated_pages += needed;
	if (enabled)
		hal_irq_enable();

	descriptor->paddr = (void *)((uintptr_t)start * M68K030_PAGE_SIZE);
	descriptor->vaddr = m68k030_phys_to_direct((uintptr_t)
		descriptor->paddr);
	descriptor->size = (size_t)needed * M68K030_PAGE_SIZE;
	return PMEM_SUCCESS;
}

int
pmem_alloc_lo(size_t size, struct pmem_desc *descriptor)
{
	return allocate_pages(size, M68K030_PAGE_SIZE,
		(uintptr_t)physical_pages * M68K030_PAGE_SIZE, descriptor);
}

int
pmem_free(struct pmem_desc *descriptor)
{
	uint32_t first, count, index;
	bool enabled;

	if (descriptor == NULL || descriptor->size == 0 ||
	    !m68k030_page_aligned((uintptr_t)descriptor->paddr) ||
	    (descriptor->size & M68K030_PAGE_MASK) != 0 ||
	    descriptor->vaddr != m68k030_phys_to_direct(
	    (uintptr_t)descriptor->paddr))
		return PMEM_BADDESC;
	first = (uint32_t)((uintptr_t)descriptor->paddr /
		M68K030_PAGE_SIZE);
	count = (uint32_t)(descriptor->size / M68K030_PAGE_SIZE);
	if (first >= physical_pages || count > physical_pages - first)
		return PMEM_BADDESC;
	enabled = hal_irq_disable();
	for (index = 0; index < count; index++) {
		if (!BIT_GET(used, first + index) ||
		    BIT_GET(reserved, first + index)) {
			if (enabled)
				hal_irq_enable();
			return PMEM_BADDESC;
		}
	}
	for (index = 0; index < count; index++)
		BIT_CLEAR(used, first + index);
	allocated_pages -= count;
	if (enabled)
		hal_irq_enable();
	descriptor->vaddr = descriptor->paddr = NULL;
	descriptor->size = 0;
	return PMEM_SUCCESS;
}

int
hal_pmem_alloc(size_t size, struct hal_pmem *descriptor, uint32_t flags)
{
	struct pmem_desc memory;
	int error;
	if (descriptor == NULL || (flags & ~(HAL_PMEM_ATTR_NOCACHE |
	    HAL_PMEM_ATTR_WRITETHRU)) != 0)
		return HAL_PMEM_BADDESC;
	error = pmem_alloc_lo(size, &memory);
	if (error != PMEM_SUCCESS)
		return error;
	descriptor->vaddr = (uintptr_t)memory.vaddr;
	descriptor->paddr = (uintptr_t)memory.paddr;
	descriptor->size = memory.size;
	return HAL_PMEM_SUCCESS;
}

int
hal_pmem_alloc_limited(size_t size, uintptr_t above, uintptr_t below,
		       struct hal_pmem *descriptor)
{
	struct pmem_desc memory;
	int error;
	if (descriptor == NULL)
		return HAL_PMEM_BADDESC;
	error = allocate_pages(size, above, below, &memory);
	if (error != PMEM_SUCCESS)
		return error;
	descriptor->vaddr = (uintptr_t)memory.vaddr;
	descriptor->paddr = (uintptr_t)memory.paddr;
	descriptor->size = memory.size;
	return HAL_PMEM_SUCCESS;
}

int
hal_pmem_free(struct hal_pmem *descriptor)
{
	struct pmem_desc memory;
	int error;
	if (descriptor == NULL)
		return HAL_PMEM_BADDESC;
	memory.vaddr = (void *)descriptor->vaddr;
	memory.paddr = (void *)descriptor->paddr;
	memory.size = descriptor->size;
	error = pmem_free(&memory);
	if (error == PMEM_SUCCESS)
		descriptor->vaddr = descriptor->paddr = descriptor->size = 0;
	return error;
}

size_t
hal_pmem_get_total_size(void)
{
	return (size_t)physical_pages * M68K030_PAGE_SIZE;
}

void
hal_mem_get_memory_map(int *blocks, struct hal_memory_map_entry *entries,
		       size_t capacity)
{
	const struct zedbsd_x68k_handoff *handoff = x68k_boot_handoff();
	unsigned index;
	if (blocks != NULL)
		*blocks = (int)handoff->memory_region_count;
	for (index = 0; entries != NULL && index < capacity &&
	    index < handoff->memory_region_count &&
	    index < ZEDBSD_X68K_MAX_MEMORY_REGIONS; index++) {
		entries[index].base = handoff->memory_regions[index].base;
		entries[index].size = handoff->memory_regions[index].size;
		entries[index].flags = handoff->memory_regions[index].type ==
			ZEDBSD_MEMORY_AVAILABLE ? HAL_PAGE_ENTRY_RAM :
			HAL_PAGE_ENTRY_SPECIAL;
	}
}

void __attribute__((weak))
hal_m68k_task_memory_stats(uint32_t *count, size_t *stack_bytes)
{
	if (count != NULL)
		*count = 0;
	if (stack_bytes != NULL)
		*stack_bytes = 0;
}

void hal_m68k_space_memory_stats(uint32_t *, uint32_t *);

void
hal_memory_get_stats(struct hal_memory_stats *stats)
{
	if (stats == NULL)
		return;
	hal_memset(stats, 0, sizeof(*stats));
	stats->physical_total = (size_t)physical_pages * M68K030_PAGE_SIZE;
	stats->physical_reserved = (size_t)reserved_pages * M68K030_PAGE_SIZE;
	stats->physical_allocated = (size_t)allocated_pages * M68K030_PAGE_SIZE;
	stats->physical_free = stats->physical_total - stats->physical_reserved -
		stats->physical_allocated;
	hal_m68k_task_memory_stats(&stats->task_count, &stats->task_stack_bytes);
	hal_m68k_space_memory_stats(&stats->space_count, &stats->page_table_count);
}
