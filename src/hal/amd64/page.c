/* amd64 physical page allocator for the initial sub-1-GiB port. */
#include <hal/hal.h>
#include "defs.h"
#include "asm.h"
#include "space.h"
#include "bsp.h"

#define MAX_PHYS_PAGES (AMD64_DIRECT_LIMIT / PAGE_SIZE)
#define BITMAP_WORDS   (MAX_PHYS_PAGES / 32U)
#define BIT_GET(n)     (page_bitmap[(n) >> 5] & (1U << ((n) & 31U)))
#define BIT_SET(n)     (page_bitmap[(n) >> 5] |= (1U << ((n) & 31U)))
#define BIT_CLEAR(n)   (page_bitmap[(n) >> 5] &= ~(1U << ((n) & 31U)))

extern char __kernel_phys_start[], __kernel_phys_end[];

static uint32 page_bitmap[BITMAP_WORDS];
static uint32 reserved_bitmap[BITMAP_WORDS];
static uint32 phys_pages;
static uint32 reserved_pages;
static uint32 allocated_pages;

static void
reserve_range(uintptr_t address, size_t size)
{
	uintptr_t end, limit = (uintptr_t)phys_pages * PAGE_SIZE;
	uint32 first, last, index;

	if (size == 0 || address >= limit)
		return;
	end = size > limit - address ? limit : address + size;
	first = (uint32)(address / PAGE_SIZE);
	last = end == limit ? phys_pages :
	    (uint32)((end + PAGE_SIZE - 1U) / PAGE_SIZE);
	if (last > phys_pages) last = phys_pages;
	for (index = first; index < last; index++)
		if (!BIT_GET(index)) {
			BIT_SET(index);
			reserved_bitmap[index >> 5] |= 1U << (index & 31U);
			reserved_pages++;
		}
}

void
amd64_page_init(void)
{
	uint64 total = bsp_mem_probe();

	phys_pages = (uint32)(total / PAGE_SIZE);
	if (phys_pages > MAX_PHYS_PAGES) phys_pages = MAX_PHYS_PAGES;
	hal_memset(page_bitmap, 0, sizeof(page_bitmap));
	hal_memset(reserved_bitmap, 0, sizeof(reserved_bitmap));
	reserved_pages = allocated_pages = 0;
	reserve_range(0, 0x00100000U);
	reserve_range((uintptr_t)__kernel_phys_start,
	    (size_t)(__kernel_phys_end - __kernel_phys_start));
	hal_printf("boot: amd64 physical memory: %u KiB\n",
	    (uint32)((uint64)phys_pages * PAGE_SIZE / 1024U));
}

void
pmem_reserve(hal_physaddr_t address, size_t size)
{
	reserve_range(address, size);
}

static int
alloc_range(size_t size, uintptr_t above, uintptr_t below,
	struct pmem_desc *desc)
{
	uint32 need, first, end, start, index;
	uint64 flags;
	uintptr_t limit = (uintptr_t)phys_pages * PAGE_SIZE;

	if (below > limit) below = limit;
	if (desc == NULL || size == 0 || size > SIZE_MAX - (PAGE_SIZE - 1U) ||
	    above >= below)
		return PMEM_BADDESC;
	need = (uint32)((size + PAGE_SIZE - 1U) / PAGE_SIZE);
	first = (uint32)((above + PAGE_SIZE - 1U) / PAGE_SIZE);
	end = (uint32)(below / PAGE_SIZE);
	if (need == 0 || first >= end || need > end - first)
		return PMEM_NOSPACE;
	flags = asm_get_rflags();
	asm_cli();
	for (start = first; start + need <= end; start++) {
		for (index = 0; index < need && !BIT_GET(start + index); index++) ;
		if (index == need) break;
		start += index;
	}
	if (start + need > end) {
		if (flags & 0x200U) asm_sti();
		return PMEM_NOSPACE;
	}
	for (index = 0; index < need; index++) BIT_SET(start + index);
	allocated_pages += need;
	if (flags & 0x200U) asm_sti();
	desc->paddr = (void *)((uintptr_t)start * PAGE_SIZE);
	desc->vaddr = amd64_phys_to_direct((uintptr_t)desc->paddr);
	desc->size = (size_t)need * PAGE_SIZE;
	return PMEM_SUCCESS;
}

int pmem_alloc_lo(size_t size, struct pmem_desc *desc)
{
	return alloc_range(size, PAGE_SIZE, (uintptr_t)phys_pages * PAGE_SIZE,
	    desc);
}

int
pmem_free(struct pmem_desc *desc)
{
	uint32 first, count, index;
	uint64 flags;

	if (desc == NULL || desc->size == 0 ||
	    ((uintptr_t)desc->paddr & (PAGE_SIZE - 1U)) != 0 ||
	    (desc->size & (PAGE_SIZE - 1U)) != 0 ||
	    desc->vaddr != amd64_phys_to_direct((uintptr_t)desc->paddr))
		return PMEM_BADDESC;
	first = (uint32)((uintptr_t)desc->paddr / PAGE_SIZE);
	count = (uint32)(desc->size / PAGE_SIZE);
	if (first >= phys_pages || count > phys_pages - first)
		return PMEM_BADDESC;
	flags = asm_get_rflags();
	asm_cli();
	for (index = 0; index < count; index++)
		if (!BIT_GET(first + index) ||
		    (reserved_bitmap[(first + index) >> 5] &
		    (1U << ((first + index) & 31U))) != 0) {
			if (flags & 0x200U) asm_sti();
			return PMEM_BADDESC;
		}
	if (allocated_pages < count) {
		if (flags & 0x200U) asm_sti();
		return PMEM_BADDESC;
	}
	for (index = 0; index < count; index++) BIT_CLEAR(first + index);
	allocated_pages -= count;
	if (flags & 0x200U) asm_sti();
	desc->vaddr = desc->paddr = NULL;
	desc->size = 0;
	return PMEM_SUCCESS;
}

int
hal_pmem_alloc(size_t size, struct hal_pmem *desc, uint32 flags)
{
	struct pmem_desc memory;
	int error;
	if (desc == NULL || (flags & ~(HAL_PMEM_ATTR_NOCACHE |
	    HAL_PMEM_ATTR_WRITETHRU)) != 0) return HAL_PMEM_BADDESC;
	error = pmem_alloc_lo(size, &memory);
	if (error != PMEM_SUCCESS) return error;
	desc->vaddr = (uintptr_t)memory.vaddr;
	desc->paddr = (uintptr_t)memory.paddr;
	desc->size = memory.size;
	return HAL_PMEM_SUCCESS;
}

int
hal_pmem_alloc_limited(size_t size, uintptr_t above, uintptr_t below,
	struct hal_pmem *desc)
{
	struct pmem_desc memory;
	int error;
	if (desc == NULL) return HAL_PMEM_BADDESC;
	error = alloc_range(size, above, below, &memory);
	if (error != PMEM_SUCCESS) return error;
	desc->vaddr = (uintptr_t)memory.vaddr;
	desc->paddr = (uintptr_t)memory.paddr;
	desc->size = memory.size;
	return HAL_PMEM_SUCCESS;
}

int
hal_pmem_free(struct hal_pmem *desc)
{
	struct pmem_desc memory;
	int error;
	if (desc == NULL) return HAL_PMEM_BADDESC;
	memory.vaddr = (void *)desc->vaddr;
	memory.paddr = (void *)desc->paddr;
	memory.size = desc->size;
	error = pmem_free(&memory);
	if (error == PMEM_SUCCESS) desc->vaddr = desc->paddr = desc->size = 0;
	return error;
}

size_t hal_pmem_get_total_size(void)
{
	return (size_t)phys_pages * PAGE_SIZE;
}

void
hal_mem_get_memory_map(int *blocks, struct hal_memory_map_entry *entries,
	size_t count)
{
	if (blocks != NULL) *blocks = 1;
	if (entries != NULL && count != 0) {
		entries[0].base = 0;
		entries[0].size = hal_pmem_get_total_size();
		entries[0].flags = HAL_PAGE_ENTRY_RAM;
	}
}

void __attribute__((weak))
hal_amd64_task_memory_stats(uint32 *count, size_t *stack_bytes)
{
	if (count != NULL) *count = 0;
	if (stack_bytes != NULL) *stack_bytes = 0;
}
void hal_amd64_space_memory_stats(uint32 *, uint32 *);

void
hal_memory_get_stats(struct hal_memory_stats *stats)
{
	if (stats == NULL) return;
	hal_memset(stats, 0, sizeof(*stats));
	stats->physical_total = (size_t)phys_pages * PAGE_SIZE;
	stats->physical_reserved = (size_t)reserved_pages * PAGE_SIZE;
	stats->physical_allocated = (size_t)allocated_pages * PAGE_SIZE;
	stats->physical_free = stats->physical_total - stats->physical_reserved -
	    stats->physical_allocated;
	hal_amd64_task_memory_stats(&stats->task_count, &stats->task_stack_bytes);
	hal_amd64_space_memory_stats(&stats->space_count,
	    &stats->page_table_count);
}
