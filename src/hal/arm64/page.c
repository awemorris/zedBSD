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
static uint32 page_bitmap[BITMAP_WORDS];
static uint32 reserved_bitmap[BITMAP_WORDS];
static uint32 phys_pages;
static uint32 reserved_pages;
static uint32 allocated_pages;

static void
release_range(uint64 base, uint64 size)
{
	uint64 limit = (uint64)phys_pages * ARM64_PAGE_SIZE;
	uint64 end;
	uint32 first, last, page;
	if (size == 0 || base >= limit) return;
	end = size > limit - base ? limit : base + size;
	first = (uint32)((base + ARM64_PAGE_SIZE - 1) / ARM64_PAGE_SIZE);
	last = (uint32)(end / ARM64_PAGE_SIZE);
	for (page = first; page < last; page++) {
		if (BIT_GET(page_bitmap, page)) {
			BIT_CLEAR(page_bitmap, page);
			BIT_CLEAR(reserved_bitmap, page);
			reserved_pages--;
		}
	}
}

static void
reserve_range(uint64 base, uint64 size)
{
	uint64 limit = (uint64)phys_pages * ARM64_PAGE_SIZE, end;
	uint32 first, last, page;
	if (size == 0 || base >= limit) return;
	end = size > limit - base ? limit : base + size;
	first = (uint32)(base / ARM64_PAGE_SIZE);
	last = (uint32)((end + ARM64_PAGE_SIZE - 1) / ARM64_PAGE_SIZE);
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
	uint64 top = 0;
	unsigned i;
	for (i = 0; i < info->memory_count; i++) {
		uint64 end = info->memory[i].base + info->memory[i].size;
		if (end > top) top = end;
	}
	/* An unpatched standalone DTB has a zero-sized memory node in QEMU. */
	if (top == 0) {
		top = 2ULL * 1024ULL * 1024ULL * 1024ULL;
		hal_puts("RPI4 FDT memory unavailable; QEMU 2 GiB fallback\n");
	}
	if (top > ARM64_MAX_PHYS) top = ARM64_MAX_PHYS;
	phys_pages = (uint32)(top / ARM64_PAGE_SIZE);
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
	    (uint64)phys_pages * ARM64_PAGE_SIZE / (1024 * 1024),
	    ((uint64)phys_pages - reserved_pages) * ARM64_PAGE_SIZE / (1024 * 1024));
}

void pmem_reserve(hal_physaddr_t paddr, size_t size) { reserve_range(paddr, size); }

static int
alloc_range(size_t size, uintptr_t above, uintptr_t below, struct pmem_desc *desc)
{
	uint32 need, first, end, start, index;
	uint64 state;
	uintptr_t limit = (uintptr_t)phys_pages * ARM64_PAGE_SIZE;
	if (below > limit) below = limit;
	if (desc == NULL || size == 0 || size > SIZE_MAX - (ARM64_PAGE_SIZE - 1) ||
	    above >= below) return PMEM_BADDESC;
	need = (uint32)((size + ARM64_PAGE_SIZE - 1) / ARM64_PAGE_SIZE);
	first = (uint32)((above + ARM64_PAGE_SIZE - 1) / ARM64_PAGE_SIZE);
	end = (uint32)(below / ARM64_PAGE_SIZE);
	if (need == 0 || first >= end || need > end - first) return PMEM_NOSPACE;
	state = arm64_irq_save();
	for (start = first; start + need <= end; start++) {
		for (index = 0; index < need && !BIT_GET(page_bitmap, start + index); index++) ;
		if (index == need) break;
		start += index;
	}
	if (start + need > end) { arm64_irq_restore(state); return PMEM_NOSPACE; }
	for (index = 0; index < need; index++) BIT_SET(page_bitmap, start + index);
	allocated_pages += need;
	arm64_irq_restore(state);
	desc->paddr = (void *)((uintptr_t)start * ARM64_PAGE_SIZE);
	desc->vaddr = arm64_phys_to_direct((uintptr_t)desc->paddr);
	desc->size = (size_t)need * ARM64_PAGE_SIZE;
	return PMEM_SUCCESS;
}

int pmem_alloc_lo(size_t size, struct pmem_desc *desc)
{ return alloc_range(size, ARM64_PAGE_SIZE, (uintptr_t)phys_pages * ARM64_PAGE_SIZE, desc); }

int
pmem_free(struct pmem_desc *desc)
{
	uint32 first, count, i;
	uint64 state;
	if (desc == NULL || desc->size == 0 ||
	    ((uintptr_t)desc->paddr & (ARM64_PAGE_SIZE - 1)) != 0 ||
	    (desc->size & (ARM64_PAGE_SIZE - 1)) != 0 ||
	    desc->vaddr != arm64_phys_to_direct((uintptr_t)desc->paddr)) return PMEM_BADDESC;
	first = (uint32)((uintptr_t)desc->paddr / ARM64_PAGE_SIZE);
	count = (uint32)(desc->size / ARM64_PAGE_SIZE);
	if (first >= phys_pages || count > phys_pages - first) return PMEM_BADDESC;
	state = arm64_irq_save();
	for (i = 0; i < count; i++)
		if (!BIT_GET(page_bitmap, first + i) || BIT_GET(reserved_bitmap, first + i)) {
			arm64_irq_restore(state); return PMEM_BADDESC;
		}
	for (i = 0; i < count; i++) BIT_CLEAR(page_bitmap, first + i);
	allocated_pages -= count;
	arm64_irq_restore(state);
	desc->vaddr = desc->paddr = NULL; desc->size = 0;
	return PMEM_SUCCESS;
}

int hal_pmem_alloc(size_t size, struct hal_pmem *desc, uint32 flags)
{
	struct pmem_desc p; int error;
	if (desc == NULL || (flags & ~(HAL_PMEM_ATTR_NOCACHE | HAL_PMEM_ATTR_WRITETHRU))) return HAL_PMEM_BADDESC;
	error = pmem_alloc_lo(size, &p); if (error) return error;
	desc->vaddr = (uintptr_t)p.vaddr; desc->paddr = (uintptr_t)p.paddr; desc->size = p.size;
	return HAL_PMEM_SUCCESS;
}
int hal_pmem_alloc_limited(size_t size, uintptr_t above, uintptr_t below, struct hal_pmem *desc)
{
	struct pmem_desc p; int error;
	if (desc == NULL) return HAL_PMEM_BADDESC;
	error = alloc_range(size, above, below, &p); if (error) return error;
	desc->vaddr = (uintptr_t)p.vaddr; desc->paddr = (uintptr_t)p.paddr; desc->size = p.size;
	return HAL_PMEM_SUCCESS;
}
int hal_pmem_free(struct hal_pmem *desc)
{
	struct pmem_desc p; int error;
	if (desc == NULL) return HAL_PMEM_BADDESC;
	p.vaddr=(void *)desc->vaddr; p.paddr=(void *)desc->paddr; p.size=desc->size;
	error=pmem_free(&p); if (!error) desc->vaddr=desc->paddr=desc->size=0; return error;
}
size_t hal_pmem_get_total_size(void) { return (size_t)phys_pages * ARM64_PAGE_SIZE; }

void hal_mem_get_memory_map(int *blocks, struct hal_memory_map_entry *entries, size_t count)
{
	const struct rpi4_fdt_info *info=rpi4_boot_info(); unsigned i;
	if (blocks) *blocks=(int)info->memory_count;
	for (i=0; entries && i<info->memory_count && i<count; i++) {
		entries[i].base=(uintptr_t)info->memory[i].base;
		entries[i].size=(size_t)info->memory[i].size;
		entries[i].flags=HAL_PAGE_ENTRY_RAM;
	}
}
void __attribute__((weak)) hal_arm64_task_memory_stats(uint32 *c,size_t *s)
{ if(c)*c=0; if(s)*s=0; }
void hal_arm64_space_memory_stats(uint32 *,uint32 *);
void hal_memory_get_stats(struct hal_memory_stats *s)
{
	if (!s) return;
	hal_memset(s,0,sizeof(*s));
	s->physical_total=(size_t)phys_pages*ARM64_PAGE_SIZE;
	s->physical_reserved=(size_t)reserved_pages*ARM64_PAGE_SIZE;
	s->physical_allocated=(size_t)allocated_pages*ARM64_PAGE_SIZE;
	s->physical_free=s->physical_total-s->physical_reserved-s->physical_allocated;
	hal_arm64_task_memory_stats(&s->task_count,&s->task_stack_bytes);
	hal_arm64_space_memory_stats(&s->space_count,&s->page_table_count);
}
