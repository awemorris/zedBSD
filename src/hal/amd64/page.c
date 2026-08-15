/* amd64 physical page allocator for the initial sub-1-GiB port. */
#include <hal/hal.h>
#include "defs.h"
#include "asm.h"
#include "space.h"
#include "bsp.h"
#include "bootloader/include/amd64-handoff.h"

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
static struct hal_pmem fixed_claims[16];
static volatile unsigned pmem_lock;

static bool
pmem_lock_enter(void)
{
	bool enabled = hal_irq_disable();
	while (__atomic_exchange_n(&pmem_lock, 1U, __ATOMIC_ACQUIRE) != 0)
		__asm__ volatile("pause");
	return enabled;
}

static void
pmem_lock_leave(bool enabled)
{
	__atomic_store_n(&pmem_lock, 0U, __ATOMIC_RELEASE);
	if (enabled)
		hal_irq_enable();
}

static void
release_usable_range(uint64 base, uint64 size)
{
	uint64 limit = (uint64)phys_pages * PAGE_SIZE;
	uint64 end;
	uint32 first, last, index;

	if (size == 0 || base >= limit)
		return;
	end = size > limit - base ? limit : base + size;
	first = (uint32)((base + PAGE_SIZE - 1U) / PAGE_SIZE);
	last = (uint32)(end / PAGE_SIZE);
	for (index = first; index < last; index++)
		if (BIT_GET(index)) {
			BIT_CLEAR(index);
			reserved_bitmap[index >> 5] &= ~(1U << (index & 31U));
			reserved_pages--;
		}
}

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
	uint32 index;

	phys_pages = (uint32)(total / PAGE_SIZE);
	if (phys_pages > MAX_PHYS_PAGES) phys_pages = MAX_PHYS_PAGES;
	hal_memset(page_bitmap, 0xff, sizeof(page_bitmap));
	hal_memset(reserved_bitmap, 0xff, sizeof(reserved_bitmap));
	reserved_pages = phys_pages;
	allocated_pages = 0;
	for (index = 0; index < bsp_mem_range_count(); index++) {
		uint64 base, size;
		uint32 type;
		if (!bsp_mem_range(index, &base, &size, &type))
			HAL_FATAL("invalid BSP memory range index");
		if (type == ZBL6_MEMORY_USABLE)
			release_usable_range(base, size);
	}
	reserve_range(0, 0x00100000U);
	reserve_range((uintptr_t)__kernel_phys_start,
	    (size_t)(__kernel_phys_end - __kernel_phys_start));
	hal_memset(fixed_claims, 0, sizeof(fixed_claims));
	hal_printf("boot: amd64 physical memory: %u KiB\n",
	    (uint32)((uint64)phys_pages * PAGE_SIZE / 1024U));
}

static int
alloc_ram(size_t size, size_t alignment, struct hal_pmem *desc)
{
	uint32 need, end, start, index, align_pages;
	uint64 flags;
	uintptr_t limit = (uintptr_t)phys_pages * PAGE_SIZE;

	if (desc == NULL || size == 0 || size > SIZE_MAX - (PAGE_SIZE - 1U) ||
	    alignment < PAGE_SIZE)
		return HAL_ERR_INVALID;
	need = (uint32)((size + PAGE_SIZE - 1U) / PAGE_SIZE);
	align_pages = (uint32)(alignment / PAGE_SIZE);
	end = (uint32)(limit / PAGE_SIZE);
	if (need == 0 || need >= end)
		return HAL_ERR_NOMEM;
	flags = asm_get_rflags();
	asm_cli();
	for (start = 1; start + need <= end; start++) {
		if ((start & (align_pages - 1U)) != 0)
			continue;
		for (index = 0; index < need && !BIT_GET(start + index); index++) ;
		if (index == need) break;
		start += index;
	}
	if (start + need > end) {
		if (flags & 0x200U) asm_sti();
		return HAL_ERR_NOMEM;
	}
	for (index = 0; index < need; index++) BIT_SET(start + index);
	allocated_pages += need;
	if (flags & 0x200U) asm_sti();
	desc->paddr = (hal_physaddr_t)start * PAGE_SIZE;
	desc->vaddr = amd64_phys_to_direct((uintptr_t)desc->paddr);
	desc->size = (size_t)need * PAGE_SIZE;
	desc->type = HAL_PMEM_TYPE_RAM;
	desc->attr = 0;
	return HAL_OK;
}

static int
free_ram(struct hal_pmem *desc)
{
	uint32 first, count, index;
	uint64 flags;

	if (desc == NULL || desc->size == 0 ||
	    ((uintptr_t)desc->paddr & (PAGE_SIZE - 1U)) != 0 ||
	    (desc->size & (PAGE_SIZE - 1U)) != 0 ||
	    desc->vaddr != amd64_phys_to_direct((uintptr_t)desc->paddr))
		return HAL_ERR_INVALID;
	first = (uint32)((uintptr_t)desc->paddr / PAGE_SIZE);
	count = (uint32)(desc->size / PAGE_SIZE);
	if (first >= phys_pages || count > phys_pages - first)
		return HAL_ERR_INVALID;
	flags = asm_get_rflags();
	asm_cli();
	for (index = 0; index < count; index++)
		if (!BIT_GET(first + index) ||
		    (reserved_bitmap[(first + index) >> 5] &
		    (1U << ((first + index) & 31U))) != 0) {
			if (flags & 0x200U) asm_sti();
			return HAL_ERR_STATE;
		}
	if (allocated_pages < count) {
		if (flags & 0x200U) asm_sti();
		return HAL_ERR_STATE;
	}
	for (index = 0; index < count; index++) BIT_CLEAR(first + index);
	allocated_pages -= count;
	if (flags & 0x200U) asm_sti();
	hal_memset(desc, 0, sizeof(*desc));
	return HAL_OK;
}

static void *
fixed_vaddr(hal_physaddr_t paddr)
{
	if (paddr >= 0x000a0000U && paddr < 0x00100000U)
		return amd64_phys_to_direct((uintptr_t)paddr);
	if (paddr >= 0xf0000000U && paddr < 0xf1000000U)
		return (void *)(uintptr_t)(0xffffffffc0000000ULL +
		    (paddr - 0xf0000000U));
	if (paddr >= 0xfee00000U && paddr < 0xff000000U)
		return (void *)(uintptr_t)(0xffffffffc1000000ULL +
		    (paddr - 0xfee00000U));
	if (paddr >= 0xfec00000U && paddr < 0xfee00000U)
		return (void *)(uintptr_t)(0xffffffffc1200000ULL +
		    (paddr - 0xfec00000U));
	return NULL;
}

static int
claim_fixed(const struct hal_pmem_request *request, struct hal_pmem *desc)
{
	unsigned i, free_slot = 16;
	hal_physaddr_t end = request->paddr + request->size;
	uint64 flags = asm_get_rflags();
	void *vaddr = fixed_vaddr(request->paddr);
	if (vaddr == NULL || request->size > 0x01000000U ||
	    fixed_vaddr(end - 1U) == NULL)
		return HAL_ERR_INVALID;
	asm_cli();
	for (i = 0; i < 16; i++) {
		hal_physaddr_t claim_end;
		if (fixed_claims[i].size == 0) {
			if (free_slot == 16) free_slot = i;
			continue;
		}
		claim_end = fixed_claims[i].paddr + fixed_claims[i].size;
		if (request->paddr < claim_end && end > fixed_claims[i].paddr) {
			if (flags & 0x200U) asm_sti();
			return HAL_ERR_BUSY;
		}
	}
	if (free_slot == 16) {
		if (flags & 0x200U) asm_sti();
		return HAL_ERR_NOMEM;
	}
	fixed_claims[free_slot].vaddr = vaddr;
	fixed_claims[free_slot].paddr = request->paddr;
	fixed_claims[free_slot].size = request->size;
	fixed_claims[free_slot].type = request->type;
	fixed_claims[free_slot].attr = request->attr;
	*desc = fixed_claims[free_slot];
	if (flags & 0x200U) asm_sti();
	return HAL_OK;
}

static int
pmem_alloc_unlocked(const struct hal_pmem_request *request,
	struct hal_pmem *desc)
{
	size_t alignment;
	if (request == NULL || desc == NULL || request->size == 0 ||
	    (request->attr & ~(HAL_PMEM_ATTR_NOCACHE |
	    HAL_PMEM_ATTR_WRITETHRU)) != 0)
		return HAL_ERR_INVALID;
	alignment = request->alignment == 0 ? PAGE_SIZE : request->alignment;
	if (alignment < PAGE_SIZE || (alignment & (alignment - 1U)) != 0)
		return HAL_ERR_INVALID;
	if (request->type == HAL_PMEM_TYPE_RAM) {
		if (request->paddr != HAL_PMEM_PADDR_ANY || request->attr != 0)
			return HAL_ERR_INVALID;
		return alloc_ram(request->size, alignment, desc);
	}
	if ((request->type != HAL_PMEM_TYPE_MMIO &&
	    request->type != HAL_PMEM_TYPE_VRAM) ||
	    request->paddr == HAL_PMEM_PADDR_ANY ||
	    request->paddr > (hal_physaddr_t)-1 - request->size ||
	    (request->paddr & (alignment - 1U)) != 0)
		return HAL_ERR_INVALID;
	return claim_fixed(request, desc);
}

int
hal_pmem_alloc(const struct hal_pmem_request *request, struct hal_pmem *desc)
{
	bool enabled = pmem_lock_enter();
	int error = pmem_alloc_unlocked(request, desc);
	pmem_lock_leave(enabled);
	return error;
}

static int
pmem_free_unlocked(struct hal_pmem *desc)
{
	unsigned i;
	uint64 flags;
	if (desc == NULL || desc->size == 0) return HAL_ERR_INVALID;
	if (desc->type == HAL_PMEM_TYPE_RAM) return free_ram(desc);
	flags = asm_get_rflags(); asm_cli();
	for (i = 0; i < 16; i++)
		if (fixed_claims[i].vaddr == desc->vaddr &&
		    fixed_claims[i].paddr == desc->paddr &&
		    fixed_claims[i].size == desc->size &&
		    fixed_claims[i].type == desc->type)
			break;
	if (i == 16) {
		if (flags & 0x200U) asm_sti();
		return HAL_ERR_STATE;
	}
	hal_memset(&fixed_claims[i], 0, sizeof(fixed_claims[i]));
	if (flags & 0x200U) asm_sti();
	hal_memset(desc, 0, sizeof(*desc));
	return HAL_OK;
}

int
hal_pmem_free(struct hal_pmem *desc)
{
	bool enabled = pmem_lock_enter();
	int error = pmem_free_unlocked(desc);
	pmem_lock_leave(enabled);
	return error;
}

size_t hal_pmem_get_total_size(void)
{
	return (size_t)phys_pages * PAGE_SIZE;
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
	bool enabled;
	if (stats == NULL) return;
	enabled = pmem_lock_enter();
	hal_memset(stats, 0, sizeof(*stats));
	stats->physical_total = (size_t)phys_pages * PAGE_SIZE;
	stats->physical_reserved = (size_t)reserved_pages * PAGE_SIZE;
	stats->physical_allocated = (size_t)allocated_pages * PAGE_SIZE;
	stats->physical_free = stats->physical_total - stats->physical_reserved -
	    stats->physical_allocated;
	hal_amd64_task_memory_stats(&stats->task_count, &stats->task_stack_bytes);
	hal_amd64_space_memory_stats(&stats->space_count,
	    &stats->page_table_count);
	pmem_lock_leave(enabled);
}
