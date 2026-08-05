/*
 * i386 physical memory management (pmem): range descriptors over a
 * page-usage bitmap.  Range bookkeeping only — page tables are managed
 * by univ.c and, later, the space API.
 *
 * Restored from the working kt snapshot with two mechanical changes:
 * the bitmap is a static array instead of a fixed physical address (the
 * old home at 2MB sat inside memory the kernel now hands to the script
 * arena), and reserved ranges — VRAM, ROM windows, the PC-98 15-16MB
 * hole, the kernel's own image — are excluded through pmem_reserve()
 * as declared by the BSP and the embedding kernel.
 */

#include <sys/hal/pmem.h>
#include <sys/hal/irq.h>
#include <sys/kcrt/kcrt.h>
#include "asm.h"

#define PAGEMAP_GET(n)		(pagemap_tbl[(n) >> 5] & (1U << ((n) & 31)))
#define PAGEMAP_SET(n)		(pagemap_tbl[(n) >> 5] |= (1U << ((n) & 31)))
#define PAGEMAP_RESET(n)	(pagemap_tbl[(n) >> 5] &= ~(1U << ((n) & 31)))

/* Enough bitmap for PHYSICAL_MEGS of RAM. */
#define PAGEMAP_WORDS	(PHYSICAL_MEGS * (1024U * 1024U / PAGE_SIZE) / 32U)

/* Number of physical pages present. */
static uint32 phys_pages;

/* Page usage bitmap. */
static uint32 pagemap_tbl[PAGEMAP_WORDS];

/* Provided by the BSP: total RAM in bytes, and device-window reserves. */
uint32 bsp_mem_probe(void);
void bsp_mem_reserve(void);

static void init_pagemap_tbl(void);

/*
 * Initialize the memory management module.
 */
void
i386_page_init(void)
{
	init_pagemap_tbl();
	bsp_mem_reserve();
}

/* Build the physical page map from the BSP's memory probe. */
static void
init_pagemap_tbl(void)
{
	uint32 total;
	uint32 reserved_top;
	uint32 i;

	total = bsp_mem_probe();
	if (total == 0)
		HAL_FATAL("can't detect memory size");
	phys_pages = total / PAGE_SIZE;
	if (phys_pages > PAGEMAP_WORDS * 32U)
		phys_pages = PAGEMAP_WORDS * 32U;
	hal_printf("mem: %d kb detected.\n", total / 1024);
	if (total < 0x400000)
		HAL_FATAL("too few physical memory");

	hal_memset(pagemap_tbl, 0, sizeof(pagemap_tbl));

	/*
	 * The fixed work areas below ADDR_FREE_TOP (IDT, boot info,
	 * startup stack) stay out of the allocator.
	 */
	reserved_top = (ADDR_FREE_TOP + PAGE_SIZE - 1) / PAGE_SIZE;
	for (i = 0; i < reserved_top; i++)
		PAGEMAP_SET(i);
}

/*
 * Exclude [paddr, paddr+size) from allocation.
 */
void
pmem_reserve(physaddr_t paddr, size_t size)
{
	uint32 first = paddr / PAGE_SIZE;
	uint32 last = (paddr + size + PAGE_SIZE - 1) / PAGE_SIZE;
	uint32 i;

	if (last > PAGEMAP_WORDS * 32U)
		last = PAGEMAP_WORDS * 32U;
	for (i = first; i < last; i++)
		PAGEMAP_SET(i);
}

/*
 * Allocate contiguous physical pages from the direct-mapped low area
 * (< 1GB); accessible without pmem_lock().
 */
int
pmem_alloc_lo(size_t size, struct pmem_desc *desc)
{
	uint32 need_pages;
	uint32 start_index;
	uint32 page_end;
	uint32 i;
	irqlock_t irqlock;

	need_pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
	if (need_pages == 0 || need_pages > phys_pages)
		return PMEM_NOSPACE;
	page_end = phys_pages - need_pages;

	ENTER_IRQLOCK(irqlock)
	{
		start_index = 0;
		for (;;) {
			for (; start_index <= page_end; start_index++)
				if (PAGEMAP_GET(start_index) == 0)
					break;
			if (start_index > page_end)
				break;
			for (i = 0; i < need_pages; i++)
				if (PAGEMAP_GET(start_index + i) != 0)
					break;
			if (i == need_pages)
				break;
			start_index += i + 1;
		}
		if (start_index > page_end) {
			LEAVE_IRQLOCK(irqlock);
			return PMEM_NOSPACE;
		}
		for (i = 0; i < need_pages; i++)
			PAGEMAP_SET(start_index + i);
	}
	LEAVE_IRQLOCK(irqlock);

	desc->paddr = (void *)(start_index << 12);
	desc->vaddr = (void *)((start_index << 12) | SYS_START);
	desc->size = need_pages << 12;
	return PMEM_SUCCESS;
}

/*
 * Free pages allocated with pmem_alloc_lo().
 */
int
pmem_free(struct pmem_desc *desc)
{
	uint32 start_page, end_page, i;
	irqlock_t irqlock;

	start_page = (uint32)desc->paddr >> 12;
	end_page = start_page + (desc->size >> 12);

	ENTER_IRQLOCK(irqlock)
	{
		for (i = start_page; i < end_page; i++) {
			if (PAGEMAP_GET(i) == 0) {
				LEAVE_IRQLOCK(irqlock);
				return PMEM_BADDESC;
			}
		}
		for (i = start_page; i < end_page; i++)
			PAGEMAP_RESET(i);
	}
	LEAVE_IRQLOCK(irqlock);
	return PMEM_SUCCESS;
}
