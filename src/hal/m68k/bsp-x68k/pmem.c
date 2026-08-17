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
#define X68K_FIXED_CLAIMS 16U
static struct hal_pmem fixed_claims[X68K_FIXED_CLAIMS];

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
	hal_memset(fixed_claims, 0, sizeof(fixed_claims));

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

static int
allocate_pages(size_t size, size_t alignment, struct hal_pmem *descriptor)
{
	uintptr_t limit = (uintptr_t)physical_pages * M68K030_PAGE_SIZE;
	uint32_t needed, first, end, start, index, align_pages;
	bool enabled;

	if (descriptor == NULL || size == 0 ||
	    size > SIZE_MAX - M68K030_PAGE_MASK ||
	    alignment < M68K030_PAGE_SIZE ||
	    (alignment & (alignment - 1U)) != 0)
		return HAL_ERR_INVALID;
	needed = (uint32_t)((size + M68K030_PAGE_MASK) /
		M68K030_PAGE_SIZE);
	align_pages = (uint32_t)(alignment / M68K030_PAGE_SIZE);
	first = 1U;
	end = (uint32_t)(limit / M68K030_PAGE_SIZE);
	if (needed == 0 || first >= end || needed > end - first)
		return HAL_ERR_NOMEM;

	enabled = hal_irq_disable();
	for (start = first; start + needed <= end; start++) {
		if ((start & (align_pages - 1U)) != 0)
			continue;
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
		return HAL_ERR_NOMEM;
	}
	for (index = 0; index < needed; index++)
		BIT_SET(used, start + index);
	allocated_pages += needed;
	if (enabled)
		hal_irq_enable();

	descriptor->paddr = (hal_physaddr_t)start * M68K030_PAGE_SIZE;
	descriptor->vaddr = m68k030_phys_to_direct(descriptor->paddr);
	descriptor->size = (size_t)needed * M68K030_PAGE_SIZE;
	descriptor->type = HAL_PMEM_TYPE_RAM;
	descriptor->attr = 0;
	return HAL_OK;
}

static int
free_ram(struct hal_pmem *descriptor)
{
	uint32_t first, count, index;
	bool enabled;

	if (descriptor == NULL || descriptor->size == 0 ||
	    descriptor->type != HAL_PMEM_TYPE_RAM ||
	    !m68k030_page_aligned(descriptor->paddr) ||
	    (descriptor->size & M68K030_PAGE_MASK) != 0 ||
	    descriptor->vaddr != m68k030_phys_to_direct(
	    descriptor->paddr))
		return HAL_ERR_INVALID;
	first = (uint32_t)(descriptor->paddr /
		M68K030_PAGE_SIZE);
	count = (uint32_t)(descriptor->size / M68K030_PAGE_SIZE);
	if (first >= physical_pages || count > physical_pages - first)
		return HAL_ERR_INVALID;
	enabled = hal_irq_disable();
	for (index = 0; index < count; index++) {
		if (!BIT_GET(used, first + index) ||
		    BIT_GET(reserved, first + index)) {
			if (enabled)
				hal_irq_enable();
			return HAL_ERR_STATE;
		}
	}
	for (index = 0; index < count; index++)
		BIT_CLEAR(used, first + index);
	allocated_pages -= count;
	if (enabled)
		hal_irq_enable();
	hal_memset(descriptor, 0, sizeof(*descriptor));
	return HAL_OK;
}

int
hal_pmem_alloc(const struct hal_pmem_request *request,
	       struct hal_pmem *descriptor)
{
	size_t alignment;
	unsigned i, free_slot = X68K_FIXED_CLAIMS;
	hal_physaddr_t end;
	bool enabled;

	if (request == NULL || descriptor == NULL || request->size == 0 ||
	    (request->attr & ~(HAL_PMEM_ATTR_NOCACHE |
	    HAL_PMEM_ATTR_WRITETHRU)) != 0)
		return HAL_ERR_INVALID;
	alignment = request->alignment == 0 ? M68K030_PAGE_SIZE :
		request->alignment;
	if (alignment < M68K030_PAGE_SIZE ||
	    (alignment & (alignment - 1U)) != 0)
		return HAL_ERR_INVALID;
	if (request->type == HAL_PMEM_TYPE_RAM) {
		if (request->paddr != HAL_PMEM_PADDR_ANY || request->attr != 0)
			return HAL_ERR_INVALID;
		return allocate_pages(request->size, alignment, descriptor);
	}
	if ((request->type != HAL_PMEM_TYPE_MMIO &&
	    request->type != HAL_PMEM_TYPE_VRAM) ||
	    request->paddr == HAL_PMEM_PADDR_ANY ||
	    request->paddr > UINT32_MAX - request->size ||
	    (request->paddr & (alignment - 1U)) != 0)
		return HAL_ERR_INVALID;
	end = request->paddr + request->size;
	enabled = hal_irq_disable();
	for (i = 0; i < X68K_FIXED_CLAIMS; i++) {
		hal_physaddr_t claim_end;
		if (fixed_claims[i].size == 0) {
			if (free_slot == X68K_FIXED_CLAIMS)
				free_slot = i;
			continue;
		}
		claim_end = fixed_claims[i].paddr + fixed_claims[i].size;
		if (request->paddr < claim_end && end > fixed_claims[i].paddr) {
			if (enabled)
				hal_irq_enable();
			return HAL_ERR_BUSY;
		}
	}
	if (free_slot == X68K_FIXED_CLAIMS) {
		if (enabled)
			hal_irq_enable();
		return HAL_ERR_NOMEM;
	}
	fixed_claims[free_slot].vaddr = (void *)(uintptr_t)request->paddr;
	fixed_claims[free_slot].paddr = request->paddr;
	fixed_claims[free_slot].size = request->size;
	fixed_claims[free_slot].type = request->type;
	fixed_claims[free_slot].attr = request->attr;
	*descriptor = fixed_claims[free_slot];
	if (enabled)
		hal_irq_enable();
	return HAL_OK;
}

int
hal_pmem_free(struct hal_pmem *descriptor)
{
	unsigned i;
	bool enabled;
	if (descriptor == NULL || descriptor->size == 0)
		return HAL_ERR_INVALID;
	if (descriptor->type == HAL_PMEM_TYPE_RAM)
		return free_ram(descriptor);
	enabled = hal_irq_disable();
	for (i = 0; i < X68K_FIXED_CLAIMS; i++)
		if (fixed_claims[i].vaddr == descriptor->vaddr &&
		    fixed_claims[i].paddr == descriptor->paddr &&
		    fixed_claims[i].size == descriptor->size &&
		    fixed_claims[i].type == descriptor->type)
			break;
	if (i == X68K_FIXED_CLAIMS) {
		if (enabled)
			hal_irq_enable();
		return HAL_ERR_STATE;
	}
	hal_memset(&fixed_claims[i], 0, sizeof(fixed_claims[i]));
	if (enabled)
		hal_irq_enable();
	hal_memset(descriptor, 0, sizeof(*descriptor));
	return HAL_OK;
}

size_t
hal_pmem_get_total_size(void)
{
	return (size_t)physical_pages * M68K030_PAGE_SIZE;
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
