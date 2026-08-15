/* 8 KiB physical page allocator from the OpenBoot memory map. */
#include <hal/hal.h>
#include "bsp.h"
#include "defs.h"
#include "space.h"

#define MAX_PAGES (0x20000000UL / SPARCV9_PAGE_SIZE)
#define WORDS (MAX_PAGES / 32U)
#define GET(m,n) ((m)[(n) >> 5] & (1U << ((n) & 31U)))
#define SET(m,n) ((m)[(n) >> 5] |= 1U << ((n) & 31U))
#define CLEAR(m,n) ((m)[(n) >> 5] &= ~(1U << ((n) & 31U)))

static uint32 used[WORDS], reserved[WORDS];
static uint32 phys_pages, reserved_pages, allocated_pages;

uintptr_t sparcv9_direct_to_phys(const void *p)
{
	return (uintptr_t)p - SPARCV9_DIRECT_BASE;
}

void *sparcv9_phys_to_direct(uintptr_t p)
{
	return (void *)(SPARCV9_DIRECT_BASE + p);
}

static void
release(uint64 base, uint64 size)
{
	uint64 limit = (uint64)phys_pages * SPARCV9_PAGE_SIZE, end;
	uint32 first, last, page;
	if (size == 0 || base >= limit) return;
	end = size > limit - base ? limit : base + size;
	first = (uint32)((base + SPARCV9_PAGE_MASK) / SPARCV9_PAGE_SIZE);
	last = (uint32)(end / SPARCV9_PAGE_SIZE);
	for (page = first; page < last; page++)
		if (GET(used, page)) {
			CLEAR(used, page); CLEAR(reserved, page); reserved_pages--;
		}
}

static void
reserve_range(uint64 base, uint64 size)
{
	uint64 limit = (uint64)phys_pages * SPARCV9_PAGE_SIZE, end;
	uint32 first, last, page;
	if (size == 0 || base >= limit) return;
	end = size > limit - base ? limit : base + size;
	first = (uint32)(base / SPARCV9_PAGE_SIZE);
	last = (uint32)((end + SPARCV9_PAGE_MASK) / SPARCV9_PAGE_SIZE);
	if (last > phys_pages) last = phys_pages;
	for (page = first; page < last; page++)
		if (!GET(used, page)) {
			SET(used, page); SET(reserved, page); reserved_pages++;
		}
}

void
sparcv9_page_init(void)
{
	const struct zedbsd_sun4u_handoff *h = sun4u_boot_handoff();
	uint64 top = 0;
	unsigned i;
	for (i = 0; i < h->installed_count; i++) {
		uint64 end = h->installed[i].base + h->installed[i].size;
		if (end > top) top = end;
	}
	if (top > 0x20000000ULL) top = 0x20000000ULL;
	phys_pages = (uint32)(top / SPARCV9_PAGE_SIZE);
	hal_memset(used, 0xff, sizeof(used));
	hal_memset(reserved, 0xff, sizeof(reserved));
	reserved_pages = phys_pages; allocated_pages = 0;
	for (i = 0; i < h->available_count; i++)
		release(h->available[i].base, h->available[i].size);
	reserve_range(0, 0x00800000UL);
	hal_printf("SPARCV9 MEMORY MAP PASS total=%llu MiB free=%llu MiB\n",
	    (uint64)phys_pages * SPARCV9_PAGE_SIZE / (1024U * 1024U),
	    ((uint64)phys_pages - reserved_pages) * SPARCV9_PAGE_SIZE /
	    (1024U * 1024U));
}

static int
alloc_ram(size_t size, size_t alignment, struct hal_pmem *desc)
{
	uint32 need, end, start, i, align_pages;
	bool enabled;
	if (desc == NULL || size == 0 || size > SIZE_MAX - SPARCV9_PAGE_MASK)
		return HAL_ERR_INVALID;
	need = (uint32)((size + SPARCV9_PAGE_MASK) / SPARCV9_PAGE_SIZE);
	align_pages = (uint32)(alignment / SPARCV9_PAGE_SIZE);
	end = phys_pages;
	if (need == 0 || need >= end) return HAL_ERR_NOMEM;
	enabled = hal_irq_disable();
	for (start = 1; start + need <= end; start++) {
		if ((start & (align_pages - 1U)) != 0) continue;
		for (i = 0; i < need && !GET(used, start + i); i++) ;
		if (i == need) break;
		start += i;
	}
	if (start + need > end) {
		if (enabled) hal_irq_enable();
		return HAL_ERR_NOMEM;
	}
	for (i = 0; i < need; i++) SET(used, start + i);
	allocated_pages += need;
	if (enabled) hal_irq_enable();
	desc->paddr = (hal_physaddr_t)start * SPARCV9_PAGE_SIZE;
	desc->vaddr = sparcv9_phys_to_direct((uintptr_t)desc->paddr);
	desc->size = (size_t)need * SPARCV9_PAGE_SIZE;
	desc->type = HAL_PMEM_TYPE_RAM; desc->attr = 0;
	return HAL_OK;
}

int
hal_pmem_alloc(const struct hal_pmem_request *request, struct hal_pmem *desc)
{
	size_t alignment;
	if (request == NULL || desc == NULL || request->size == 0 ||
	    request->type != HAL_PMEM_TYPE_RAM ||
	    request->paddr != HAL_PMEM_PADDR_ANY || request->attr != 0)
		return request != NULL && request->type != HAL_PMEM_TYPE_RAM ?
		    HAL_ERR_UNSUPPORTED : HAL_ERR_INVALID;
	alignment = request->alignment == 0 ? SPARCV9_PAGE_SIZE :
	    request->alignment;
	if (alignment < SPARCV9_PAGE_SIZE ||
	    (alignment & (alignment - 1U)) != 0)
		return HAL_ERR_INVALID;
	return alloc_ram(request->size, alignment, desc);
}

int
hal_pmem_free(struct hal_pmem *desc)
{
	uint32 first, count, i;
	bool enabled;
	if (desc == NULL || desc->type != HAL_PMEM_TYPE_RAM ||
	    desc->size == 0 || (desc->paddr & SPARCV9_PAGE_MASK) != 0 ||
	    (desc->size & SPARCV9_PAGE_MASK) != 0 ||
	    desc->vaddr != sparcv9_phys_to_direct((uintptr_t)desc->paddr))
		return HAL_ERR_INVALID;
	first = (uint32)(desc->paddr / SPARCV9_PAGE_SIZE);
	count = (uint32)(desc->size / SPARCV9_PAGE_SIZE);
	if (first >= phys_pages || count > phys_pages - first)
		return HAL_ERR_INVALID;
	enabled = hal_irq_disable();
	for (i = 0; i < count; i++)
		if (!GET(used, first + i) || GET(reserved, first + i)) {
			if (enabled) hal_irq_enable();
			return HAL_ERR_STATE;
		}
	for (i = 0; i < count; i++) CLEAR(used, first + i);
	allocated_pages -= count;
	if (enabled) hal_irq_enable();
	hal_memset(desc, 0, sizeof(*desc));
	return HAL_OK;
}

size_t hal_pmem_get_total_size(void)
{
	return (size_t)phys_pages * SPARCV9_PAGE_SIZE;
}

void __attribute__((weak)) hal_sparcv9_task_memory_stats(uint32 *c, size_t *s)
{
	if (c) *c = 0;
	if (s) *s = 0;
}
void hal_sparcv9_space_memory_stats(uint32 *, uint32 *);

void
hal_memory_get_stats(struct hal_memory_stats *s)
{
	if (!s) return;
	hal_memset(s, 0, sizeof(*s));
	s->physical_total = (size_t)phys_pages * SPARCV9_PAGE_SIZE;
	s->physical_reserved = (size_t)reserved_pages * SPARCV9_PAGE_SIZE;
	s->physical_allocated = (size_t)allocated_pages * SPARCV9_PAGE_SIZE;
	s->physical_free = s->physical_total - s->physical_reserved -
	    s->physical_allocated;
	hal_sparcv9_task_memory_stats(&s->task_count, &s->task_stack_bytes);
	hal_sparcv9_space_memory_stats(&s->space_count, &s->page_table_count);
}
