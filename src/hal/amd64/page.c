/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The amd64 physical-page allocator and fixed physical mapping registry.
 */

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

extern char __kernel_phys_start[];
extern char __kernel_phys_end[];

static uint32_t page_bitmap[BITMAP_WORDS];
static uint32_t reserved_bitmap[BITMAP_WORDS];
static uint32_t phys_pages;
static uint32_t reserved_pages;
static uint32_t allocated_pages;
static struct hal_pmem fixed_claims[16];
static volatile unsigned pmem_lock;

void hal_amd64_space_memory_stats(uint32_t *count, uint32_t *page_tables);

static bool pmem_lock_enter(void);
static void pmem_lock_leave(bool enabled);
static void release_usable_range(uint64_t base, uint64_t size);
static void reserve_range(uintptr_t address, size_t size);
static int alloc_ram(size_t size, size_t alignment, struct hal_pmem *descriptor);
static int free_ram(struct hal_pmem *descriptor);
static void *fixed_vaddr(hal_physaddr_t physical);
static int claim_fixed(const struct hal_pmem_request *request, struct hal_pmem *descriptor);
static int pmem_alloc_unlocked(const struct hal_pmem_request *request, struct hal_pmem *descriptor);
static int pmem_free_unlocked(struct hal_pmem *descriptor);

/*
 * Initializes the amd64 physical-memory allocation maps.
 */
void
amd64_page_init(
	void)
{
	uint64_t total;
	uint64_t base;
	uint64_t size;
	uint32_t index;
	uint32_t type;

	/* Bounds the allocator to the direct physical mapping. */
	total = bsp_mem_probe();
	phys_pages = (uint32_t)(total / PAGE_SIZE);
	if (phys_pages > MAX_PHYS_PAGES)
		phys_pages = MAX_PHYS_PAGES;

	/* Starts with every addressable page reserved. */
	hal_memset(page_bitmap, 0xff, sizeof(page_bitmap));
	hal_memset(reserved_bitmap, 0xff, sizeof(reserved_bitmap));
	reserved_pages = phys_pages;
	allocated_pages = 0;

	/* Releases every firmware range classified as usable RAM. */
	for (index = 0; index < bsp_mem_range_count(); index++) {
		/* Requires each advertised range to remain retrievable. */
		if (!bsp_mem_range(index, &base, &size, &type))
			HAL_FATAL("invalid BSP memory range index");

		/* Releases only ranges explicitly owned by the RAM allocator. */
		if (type == ZBL6_MEMORY_USABLE)
			release_usable_range(base, size);
	}

	/* Reserves firmware space and the loaded kernel image. */
	reserve_range(0, 0x00100000U);
	reserve_range(
		(uintptr_t)__kernel_phys_start,
		(size_t)(__kernel_phys_end - __kernel_phys_start));

	/* Clears the independent fixed-mapping registry. */
	hal_memset(fixed_claims, 0, sizeof(fixed_claims));
}

/*
 * Allocates one physical-memory descriptor under the allocator lock.
 */
int
hal_pmem_alloc(
	const struct hal_pmem_request *request,
	struct hal_pmem *descriptor)
{
	bool enabled;
	int error;

	/* Performs the complete allocation while holding the global lock. */
	enabled = pmem_lock_enter();
	error = pmem_alloc_unlocked(request, descriptor);
	pmem_lock_leave(enabled);

	/* Returns the allocation result unchanged. */
	return error;
}

/*
 * Frees one physical-memory descriptor under the allocator lock.
 */
int
hal_pmem_free(
	struct hal_pmem *descriptor)
{
	bool enabled;
	int error;

	/* Performs the complete release while holding the global lock. */
	enabled = pmem_lock_enter();
	error = pmem_free_unlocked(descriptor);
	pmem_lock_leave(enabled);

	/* Returns the release result unchanged. */
	return error;
}

/*
 * Reports the total physical memory managed by this allocator.
 */
size_t
hal_pmem_get_total_size(
	void)
{
	/* Returns the direct-map-limited page capacity. */
	return (size_t)phys_pages * PAGE_SIZE;
}

/*
 * Supplies zero task statistics when the task module is absent.
 */
void __attribute__((weak))
hal_amd64_task_memory_stats(
	uint32_t *count,
	size_t *stack_bytes)
{
	/* Clears each result requested by the caller. */
	if (count != NULL)
		*count = 0;
	if (stack_bytes != NULL)
		*stack_bytes = 0;
}

/*
 * Reports the current amd64 HAL memory accounting.
 */
void
hal_memory_get_stats(
	struct hal_memory_stats *stats)
{
	bool enabled;

	/* Ignores an absent result buffer. */
	if (stats == NULL)
		return;

	/* Snapshots physical and subsystem accounting under the page lock. */
	enabled = pmem_lock_enter();
	hal_memset(stats, 0, sizeof(*stats));
	stats->physical_total = (size_t)phys_pages * PAGE_SIZE;
	stats->physical_reserved = (size_t)reserved_pages * PAGE_SIZE;
	stats->physical_allocated = (size_t)allocated_pages * PAGE_SIZE;
	stats->physical_free = stats->physical_total -
	    stats->physical_reserved - stats->physical_allocated;
	hal_amd64_task_memory_stats(
		&stats->task_count,
		&stats->task_stack_bytes);
	hal_amd64_space_memory_stats(
		&stats->space_count,
		&stats->page_table_count);
	pmem_lock_leave(enabled);
}

/* Acquires the physical-memory lock with local interrupts disabled. */
static bool
pmem_lock_enter(
	void)
{
	bool enabled;

	/* Preserves interrupt state and acquires the global spin lock. */
	enabled = hal_irq_disable();
	while (__atomic_exchange_n(&pmem_lock, 1U, __ATOMIC_ACQUIRE) != 0)
		__asm__ volatile("pause");

	/* Returns whether interrupts must be restored on release. */
	return enabled;
}

/* Releases the physical-memory lock and restores interrupt state. */
static void
pmem_lock_leave(
	bool enabled)
{
	/* Publishes all protected writes before unlocking. */
	__atomic_store_n(&pmem_lock, 0U, __ATOMIC_RELEASE);

	/* Restores interrupts only when they were originally enabled. */
	if (enabled)
		hal_irq_enable();
}

/* Releases usable pages from the initially reserved bitmap. */
static void
release_usable_range(
	uint64_t base,
	uint64_t size)
{
	uint64_t limit;
	uint64_t end;
	uint32_t first;
	uint32_t last;
	uint32_t index;

	/* Rejects empty ranges and ranges beyond the managed limit. */
	limit = (uint64_t)phys_pages * PAGE_SIZE;
	if (size == 0 || base >= limit)
		return;

	/* Clips and rounds the range inward to complete pages. */
	if (size > limit - base)
		end = limit;
	else
		end = base + size;
	first = (uint32_t)((base + PAGE_SIZE - 1U) / PAGE_SIZE);
	last = (uint32_t)(end / PAGE_SIZE);

	/* Marks each newly usable page free. */
	for (index = first; index < last; index++) {
		/* Releases the page only when it is still reserved. */
		if (BIT_GET(index)) {
			BIT_CLEAR(index);
			reserved_bitmap[index >> 5] &= ~(1U << (index & 31U));
			reserved_pages--;
		}
	}
}

/* Reserves every page touched by a physical range. */
static void
reserve_range(
	uintptr_t address,
	size_t size)
{
	uintptr_t limit;
	uintptr_t end;
	uint32_t first;
	uint32_t last;
	uint32_t index;

	/* Rejects empty ranges and ranges beyond the managed limit. */
	limit = (uintptr_t)phys_pages * PAGE_SIZE;
	if (size == 0 || address >= limit)
		return;

	/* Clips and rounds the range outward to every touched page. */
	if (size > limit - address)
		end = limit;
	else
		end = address + size;
	first = (uint32_t)(address / PAGE_SIZE);
	if (end == limit)
		last = phys_pages;
	else
		last = (uint32_t)((end + PAGE_SIZE - 1U) / PAGE_SIZE);
	if (last > phys_pages)
		last = phys_pages;

	/* Marks each newly reserved page unavailable. */
	for (index = first; index < last; index++) {
		/* Reserves the page only when it is still free. */
		if (!BIT_GET(index)) {
			BIT_SET(index);
			reserved_bitmap[index >> 5] |= 1U << (index & 31U);
			reserved_pages++;
		}
	}
}

/* Allocates a page-aligned run of RAM with interrupts disabled. */
static int
alloc_ram(
	size_t size,
	size_t alignment,
	struct hal_pmem *descriptor)
{
	uint32_t need;
	uint32_t end;
	uint32_t start;
	uint32_t index;
	uint32_t align_pages;
	uint64_t flags;
	uintptr_t limit;

	/* Rejects requests that cannot form a valid page run. */
	limit = (uintptr_t)phys_pages * PAGE_SIZE;
	if (descriptor == NULL ||
	    size == 0 ||
	    size > SIZE_MAX - (PAGE_SIZE - 1U) ||
	    alignment < PAGE_SIZE)
		return HAL_ERR_INVALID;

	/* Converts the request and allocator limit to page units. */
	need = (uint32_t)((size + PAGE_SIZE - 1U) / PAGE_SIZE);
	align_pages = (uint32_t)(alignment / PAGE_SIZE);
	end = (uint32_t)(limit / PAGE_SIZE);

	/* Rejects requests too large for any managed run. */
	if (need == 0 || need >= end)
		return HAL_ERR_NOMEM;

	/* Searches the bitmap atomically with respect to this CPU. */
	flags = asm_get_rflags();
	asm_cli();
	for (start = 1; start + need <= end; start++) {
		/* Skips candidate starts that violate the alignment. */
		if ((start & (align_pages - 1U)) != 0)
			continue;

		/* Counts the free prefix at this candidate start. */
		for (index = 0;
		     index < need && !BIT_GET(start + index);
		     index++) {
			/* The loop condition performs the bitmap scan. */
		}

		/* Stops once the complete requested run is free. */
		if (index == need)
			break;
		start += index;
	}

	/* Restores interrupt state when no suitable run exists. */
	if (start + need > end) {
		/* Restores the caller's enabled interrupt state. */
		if ((flags & 0x200U) != 0)
			asm_sti();

		/* Reports physical-memory exhaustion. */
		return HAL_ERR_NOMEM;
	}

	/* Claims every page in the selected run. */
	for (index = 0; index < need; index++)
		BIT_SET(start + index);
	allocated_pages += need;

	/* Restores the caller's interrupt-enable state. */
	if ((flags & 0x200U) != 0)
		asm_sti();

	/* Publishes the complete direct-mapped RAM descriptor. */
	descriptor->paddr = (hal_physaddr_t)start * PAGE_SIZE;
	descriptor->vaddr = amd64_phys_to_direct((uintptr_t)descriptor->paddr);
	descriptor->size = (size_t)need * PAGE_SIZE;
	descriptor->type = HAL_PMEM_TYPE_RAM;
	descriptor->attr = 0;

	/* Reports a successful allocation. */
	return HAL_OK;
}

/* Releases a direct-mapped RAM allocation. */
static int
free_ram(
	struct hal_pmem *descriptor)
{
	uint32_t first;
	uint32_t count;
	uint32_t index;
	uint64_t flags;

	/* Validates the descriptor's page geometry and direct mapping. */
	if (descriptor == NULL ||
	    descriptor->size == 0 ||
	    ((uintptr_t)descriptor->paddr & (PAGE_SIZE - 1U)) != 0 ||
	    (descriptor->size & (PAGE_SIZE - 1U)) != 0 ||
	    descriptor->vaddr !=
	    amd64_phys_to_direct((uintptr_t)descriptor->paddr))
		return HAL_ERR_INVALID;

	/* Converts and validates the described page range. */
	first = (uint32_t)((uintptr_t)descriptor->paddr / PAGE_SIZE);
	count = (uint32_t)(descriptor->size / PAGE_SIZE);
	if (first >= phys_pages || count > phys_pages - first)
		return HAL_ERR_INVALID;

	/* Verifies every page while local interrupts are disabled. */
	flags = asm_get_rflags();
	asm_cli();
	for (index = 0; index < count; index++) {
		/* Rejects a free or reserved page in the release range. */
		if (!BIT_GET(first + index) ||
		    (reserved_bitmap[(first + index) >> 5] &
		    (1U << ((first + index) & 31U))) != 0) {
			/* Restores the caller's interrupt state before rejecting the range. */
			if ((flags & 0x200U) != 0)
				asm_sti();

			/* Reports inconsistent page ownership. */
			return HAL_ERR_STATE;
		}
	}

	/* Rejects inconsistent allocation accounting. */
	if (allocated_pages < count) {
		/* Restores the caller's interrupt state before rejecting the count. */
		if ((flags & 0x200U) != 0)
			asm_sti();

		/* Reports inconsistent allocation accounting. */
		return HAL_ERR_STATE;
	}

	/* Releases every validated page and updates accounting. */
	for (index = 0; index < count; index++)
		BIT_CLEAR(first + index);
	allocated_pages -= count;

	/* Restores the caller's interrupt-enable state. */
	if ((flags & 0x200U) != 0)
		asm_sti();

	/* Invalidates the caller's released descriptor. */
	hal_memset(descriptor, 0, sizeof(*descriptor));

	/* Reports a successful release. */
	return HAL_OK;
}

/* Resolves a supported fixed physical address to its virtual window. */
static void *
fixed_vaddr(
	hal_physaddr_t physical)
{
	void *address;

	/* Selects the direct legacy-device window. */
	if (physical >= 0x000a0000U && physical < 0x00100000U) {
		address = amd64_phys_to_direct((uintptr_t)physical);

		/* Returns the translated legacy-device address. */
		return address;
	}

	/* Selects the PCI MMIO window. */
	if (physical >= 0xf0000000U && physical < 0xf1000000U) {
		return (void *)(uintptr_t)(0xffffffffc0000000ULL +
		    (physical - 0xf0000000U));
	}

	/* Selects the local APIC MMIO window. */
	if (physical >= 0xfee00000U && physical < 0xff000000U) {
		return (void *)(uintptr_t)(0xffffffffc1000000ULL +
		    (physical - 0xfee00000U));
	}

	/* Selects the I/O APIC MMIO window. */
	if (physical >= 0xfec00000U && physical < 0xfee00000U) {
		return (void *)(uintptr_t)(0xffffffffc1200000ULL +
		    (physical - 0xfec00000U));
	}

	/* Rejects addresses outside every fixed window. */
	return NULL;
}

/* Claims one supported fixed MMIO or VRAM mapping. */
static int
claim_fixed(
	const struct hal_pmem_request *request,
	struct hal_pmem *descriptor)
{
	hal_physaddr_t end;
	hal_physaddr_t claim_end;
	uint64_t flags;
	void *vaddr;
	unsigned index;
	unsigned free_slot;

	/* Resolves both ends of the requested fixed window. */
	free_slot = 16;
	end = request->paddr + request->size;
	flags = asm_get_rflags();
	vaddr = fixed_vaddr(request->paddr);
	if (vaddr == NULL ||
	    request->size > 0x01000000U ||
	    fixed_vaddr(end - 1U) == NULL)
		return HAL_ERR_INVALID;

	/* Finds a free registry slot while rejecting overlaps. */
	asm_cli();
	for (index = 0; index < 16; index++) {
		/* Remembers the first available registry slot. */
		if (fixed_claims[index].size == 0) {
			/* Retains the lowest-numbered available slot. */
			if (free_slot == 16)
				free_slot = index;
			continue;
		}

		/* Computes the exclusive end of this existing claim. */
		claim_end = fixed_claims[index].paddr +
		    fixed_claims[index].size;

		/* Rejects any overlap with an existing fixed claim. */
		if (request->paddr < claim_end &&
		    end > fixed_claims[index].paddr) {
			/* Restores the caller's interrupt state before rejecting overlap. */
			if ((flags & 0x200U) != 0)
				asm_sti();

			/* Reports an overlapping fixed claim. */
			return HAL_ERR_BUSY;
		}
	}

	/* Reports exhaustion of the fixed-claim registry. */
	if (free_slot == 16) {
		/* Restores the caller's interrupt state before reporting exhaustion. */
		if ((flags & 0x200U) != 0)
			asm_sti();

		/* Reports that no fixed-claim slot remains. */
		return HAL_ERR_NOMEM;
	}

	/* Records and returns the complete fixed mapping. */
	fixed_claims[free_slot].vaddr = vaddr;
	fixed_claims[free_slot].paddr = request->paddr;
	fixed_claims[free_slot].size = request->size;
	fixed_claims[free_slot].type = request->type;
	fixed_claims[free_slot].attr = request->attr;
	*descriptor = fixed_claims[free_slot];

	/* Restores the caller's interrupt-enable state. */
	if ((flags & 0x200U) != 0)
		asm_sti();

	/* Reports a successful fixed claim. */
	return HAL_OK;
}

/* Selects the allocation path for a validated physical-memory request. */
static int
pmem_alloc_unlocked(
	const struct hal_pmem_request *request,
	struct hal_pmem *descriptor)
{
	size_t alignment;
	int error;

	/* Validates the common request fields and supported attributes. */
	if (request == NULL ||
	    descriptor == NULL ||
	    request->size == 0 ||
	    (request->attr &
	    ~(HAL_PMEM_ATTR_NOCACHE | HAL_PMEM_ATTR_WRITETHRU)) != 0)
		return HAL_ERR_INVALID;

	/* Applies and validates the requested physical alignment. */
	if (request->alignment == 0)
		alignment = PAGE_SIZE;
	else
		alignment = request->alignment;
	if (alignment < PAGE_SIZE || (alignment & (alignment - 1U)) != 0)
		return HAL_ERR_INVALID;

	/* Allocates ordinary RAM only through the bitmap allocator. */
	if (request->type == HAL_PMEM_TYPE_RAM) {
		/* Rejects fixed-address or attributed requests on the RAM path. */
		if (request->paddr != HAL_PMEM_PADDR_ANY || request->attr != 0)
			return HAL_ERR_INVALID;

		/* Allocates the validated RAM range. */
		error = alloc_ram(request->size, alignment, descriptor);

		/* Returns the RAM allocation result unchanged. */
		return error;
	}

	/* Validates a fixed MMIO or VRAM request. */
	if ((request->type != HAL_PMEM_TYPE_MMIO &&
	    request->type != HAL_PMEM_TYPE_VRAM) ||
	    request->paddr == HAL_PMEM_PADDR_ANY ||
	    request->paddr > (hal_physaddr_t)-1 - request->size ||
	    (request->paddr & (alignment - 1U)) != 0)
		return HAL_ERR_INVALID;

	/* Claims the validated fixed mapping. */
	error = claim_fixed(request, descriptor);

	/* Returns the fixed-claim result unchanged. */
	return error;
}

/* Releases a physical-memory descriptor without taking the outer lock. */
static int
pmem_free_unlocked(
	struct hal_pmem *descriptor)
{
	uint64_t flags;
	unsigned index;
	int error;

	/* Rejects absent and empty descriptors. */
	if (descriptor == NULL || descriptor->size == 0)
		return HAL_ERR_INVALID;

	/* Delegates ordinary RAM to the bitmap allocator. */
	if (descriptor->type == HAL_PMEM_TYPE_RAM) {
		error = free_ram(descriptor);

		/* Returns the RAM release result unchanged. */
		return error;
	}

	/* Finds the exact fixed claim with interrupts disabled. */
	flags = asm_get_rflags();
	asm_cli();
	for (index = 0; index < 16; index++) {
		/* Selects the registry entry which exactly owns this descriptor. */
		if (fixed_claims[index].vaddr == descriptor->vaddr &&
		    fixed_claims[index].paddr == descriptor->paddr &&
		    fixed_claims[index].size == descriptor->size &&
		    fixed_claims[index].type == descriptor->type)
			break;
	}

	/* Rejects descriptors absent from the fixed registry. */
	if (index == 16) {
		/* Restores the caller's interrupt state before rejecting ownership. */
		if ((flags & 0x200U) != 0)
			asm_sti();

		/* Reports a descriptor without a matching fixed claim. */
		return HAL_ERR_STATE;
	}

	/* Clears the matched claim and restores interrupt state. */
	hal_memset(&fixed_claims[index], 0, sizeof(fixed_claims[index]));
	if ((flags & 0x200U) != 0)
		asm_sti();

	/* Invalidates the caller's released descriptor. */
	hal_memset(descriptor, 0, sizeof(*descriptor));

	/* Reports a successful fixed release. */
	return HAL_OK;
}
