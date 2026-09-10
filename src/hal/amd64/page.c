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
#include "pmem-range.h"
#include "bsp.h"
#include "bootloader/include/amd64-handoff.h"

#define MAX_PHYS_PAGES (AMD64_BOOTSTRAP_LIMIT / PAGE_SIZE)
#define BITMAP_WORDS   (MAX_PHYS_PAGES / 32U)
#define BIT_GET(n)     (page_bitmap[(n) >> 5] & (1U << ((n) & 31U)))
#define BIT_SET(n)     (page_bitmap[(n) >> 5] |= (1U << ((n) & 31U)))
#define BIT_CLEAR(n)   (page_bitmap[(n) >> 5] &= ~(1U << ((n) & 31U)))

extern char __kernel_phys_start[];
extern char __kernel_phys_end[];

static uint32_t page_bitmap[BITMAP_WORDS];
static uint32_t reserved_bitmap[BITMAP_WORDS];
struct early_reservation {
	uint64_t base;
	uint64_t size;
};

/* A normalized range and each boot owner can split the monotonic arena. */
static struct early_reservation early_reservations[1024];
static uint32_t early_reservation_count;
static uint32_t early_low_cursor = 0x100000U / PAGE_SIZE;
static uint64_t early_high_cursor = AMD64_BOOTSTRAP_LIMIT;
static uint32_t early_low_pages;
#ifndef WS025_EARLY_LOW_PAGES
#define WS025_EARLY_LOW_PAGES UINT32_MAX
#endif

static struct amd64_pmem_extent ram_extents[ZBL6_MAX_MEMORY_RANGES];
static uint32_t ram_extent_count;
static uint32_t ram_extent_type[ZBL6_MAX_MEMORY_RANGES];
static int boot_memory_released;
static uint64_t managed_pages;
static uint64_t metadata_bytes;
static int range_ready;
static uint32_t phys_pages;
static uint64_t boot_usable_bytes;
static uint64_t boot_reclaim_bytes;
static uint64_t boot_highest_end;
static uint64_t boot_usable_highest_end;
static uint64_t allocator_initial_bytes;
static uint32_t reserved_pages;
static uint32_t allocated_pages;
static struct hal_pmem fixed_claims[16];
static volatile unsigned pmem_lock;
static uint64_t pmem_entered_cycles;
static uint64_t pmem_max_irqoff_cycles;

void hal_amd64_space_memory_stats(uint32_t *count, uint32_t *page_tables);

static uint64_t pmem_cycles(void);
static bool pmem_lock_enter(void);
static void pmem_lock_leave(bool enabled);
static void release_usable_range(uint64_t base, uint64_t size);
static void reserve_range(uintptr_t address, size_t size);
static int record_early_page(uint64_t physical);
static int record_early_run(uint64_t physical, uint64_t size);
static void *early_metadata(uint64_t size);
static void reserve_managed(uint64_t base, uint64_t size);
static int alloc_range(size_t size, size_t alignment, uint64_t minimum, uint64_t maximum, uint64_t boundary, struct hal_pmem *descriptor);
static int boot_page_retained(uint64_t physical);
static int boot_page_retired(uint64_t physical);
static uint64_t next_retired_page(uint64_t physical, uint64_t end);
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
	const struct zbl6_framebuffer *framebuffer;
	uint64_t total;
	uint64_t base;
	uint64_t size;
	uint32_t index;
	uint32_t type;

	/* Distinguishes legacy geometry from complete typed firmware reporting. */
	if (bsp_memory_source() == 0)
		hal_puts("A64 MEMORY legacy degraded: boot ownership/attributes unavailable\n");

	/* Bounds the bootstrap allocator to its initial direct physical mapping. */
	total = bsp_mem_probe();
	if (total / PAGE_SIZE > MAX_PHYS_PAGES)
		phys_pages = MAX_PHYS_PAGES;
	else
		phys_pages = (uint32_t)(total / PAGE_SIZE);

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
		if (size > UINT64_MAX - base)
			HAL_FATAL("overflowing BSP memory range");
		if (base + size > boot_highest_end)
			boot_highest_end = base + size;
		if (type == ZBL6_MEMORY_USABLE) {
			if (size > UINT64_MAX - boot_usable_bytes)
				HAL_FATAL("overflowing BSP usable RAM sum");
			boot_usable_bytes += size;
			if (base + size > boot_usable_highest_end)
				boot_usable_highest_end = base + size;
		}
		if (type == ZBL6_MEMORY_BOOT_RECLAIM) {
			if (size > UINT64_MAX - boot_reclaim_bytes)
				HAL_FATAL("overflowing BSP boot reclaim sum");
			boot_reclaim_bytes += size;
		}

		/* Releases only ranges explicitly owned by the RAM allocator. */
		if (type == ZBL6_MEMORY_USABLE)
			release_usable_range(base, size);
	}

	/* Reserves firmware space and the loaded kernel image. */
	reserve_range(0, 0x00100000U);
	reserve_range(
		(uintptr_t)__kernel_phys_start,
		(size_t)(__kernel_phys_end - __kernel_phys_start));
	for (index = 0; bsp_boot_allocation(index, &base, &size); index++)
		reserve_range((uintptr_t)base, (size_t)size);

	framebuffer = hal_get_arch_handoff("pcat.framebuffer");
	if (framebuffer != NULL)
		reserve_range((uintptr_t)framebuffer->physical_base, (size_t)framebuffer->size);

	/* Clears the independent fixed-mapping registry. */
	allocator_initial_bytes = (uint64_t)(phys_pages - reserved_pages) * PAGE_SIZE;
	hal_printf("A64 MEMORY source=%u usable=%llu boot_reclaim=%llu highest_usable=%llu allocator=%llu\n",
	    bsp_memory_source(), (unsigned long long)boot_usable_bytes,
	    (unsigned long long)boot_reclaim_bytes,
	    (unsigned long long)boot_usable_highest_end,
	    (unsigned long long)allocator_initial_bytes);
	hal_memset(fixed_claims, 0, sizeof(fixed_claims));
}

/*
 * Reserves a bootstrap-reachable table page before normal allocation starts.
 * This transfers a free bit directly to reserved ownership and uses no heap.
 */
int
amd64_early_table_page(
	uint64_t *physical,
	int mapped)
{
	uint64_t base;
	uint64_t size;
	uint64_t end;
	uint64_t owner_base;
	uint64_t owner_size;
	uint64_t candidate;
	uint32_t index;
	uint32_t owner;
	uint32_t type;
	int changed;

	if (physical == NULL)
		return 0;
	while (early_low_cursor < phys_pages && early_low_pages < WS025_EARLY_LOW_PAGES) {
		index = early_low_cursor++;
		if (BIT_GET(index))
			continue;
		*physical = (uint64_t)index * PAGE_SIZE;
		if (!record_early_page(*physical))
			return 0;
		BIT_SET(index);
		reserved_bitmap[index >> 5] |= 1U << (index & 31U);
		reserved_pages++;
		early_low_pages++;
		allocator_initial_bytes -= PAGE_SIZE;
		return 1;
	}
	/* A high arena is available only after an explicit partial CR3 switch. */
	if (!mapped)
		return 0;
	for (index = 0; index < bsp_mem_range_count(); index++) {
		if (!bsp_mem_range(index, &base, &size, &type) || type != ZBL6_MEMORY_USABLE)
			continue;
		end = base + size;
		candidate = base > early_high_cursor ? base : early_high_cursor;
		candidate = (candidate + PAGE_SIZE - 1U) & ~(uint64_t)(PAGE_SIZE - 1U);
		while (candidate < end && PAGE_SIZE <= end - candidate) {
			changed = 0;
			for (owner = 0; bsp_boot_allocation(owner, &owner_base, &owner_size); owner++) {
				if (candidate >= owner_base && candidate - owner_base < owner_size) {
					candidate = owner_base + owner_size;
					changed = 1;
					break;
				}
			}
			if (changed)
				continue;
			/* The translation checks a present leaf in the active RAM map. */
			if (amd64_phys_to_direct((uintptr_t)candidate) == NULL)
				return 0;
			if (!record_early_page(candidate))
				return 0;
			*physical = candidate;
			early_high_cursor = candidate + PAGE_SIZE;
			return 1;
		}
	}
	return 0;
}

/* Returns arena ownership for transfer to the range-based allocator. */
int
amd64_early_reservation(uint32_t index, uint64_t *physical, uint64_t *size)
{
	if (index >= early_reservation_count || physical == NULL || size == NULL)
		return 0;
	*physical = early_reservations[index].base;
	*size = early_reservations[index].size;
	return 1;
}

/* Records monotonic arena runs without allocating bookkeeping memory. */
static int
record_early_page(uint64_t physical)
{
	return record_early_run(physical, PAGE_SIZE);
}

/* Records one complete metadata or table run with explicit ownership. */
static int
record_early_run(uint64_t physical, uint64_t size)
{
	struct early_reservation *last;

	if (early_reservation_count != 0) {
		last = &early_reservations[early_reservation_count - 1U];
		if (last->base + last->size == physical &&
		    (last->base < AMD64_BOOTSTRAP_LIMIT) == (physical < AMD64_BOOTSTRAP_LIMIT)) {
			last->size += size;
			return 1;
		}
	}
	if (early_reservation_count == sizeof(early_reservations) / sizeof(early_reservations[0]))
		return 0;
	last = &early_reservations[early_reservation_count++];
	last->base = physical;
	last->size = size;
	return 1;
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
	return (size_t)managed_pages * PAGE_SIZE;
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
hal_pmem_get_stats(
	struct hal_pmem_stats *stats)
{
	bool enabled;
	uint32_t index;

	/* Ignores an absent result buffer. */
	if (stats == NULL)
		return;

	/* Snapshots physical and subsystem accounting under the page lock. */
	enabled = pmem_lock_enter();
	hal_memset(stats, 0, sizeof(*stats));
	stats->boot_ranges_valid = 1;
	stats->boot_range_count = bsp_mem_range_count();
	stats->boot_usable_bytes = boot_usable_bytes;
	stats->boot_highest_end = boot_highest_end;
	stats->boot_usable_highest_end = boot_usable_highest_end;
	stats->direct_mapped_bytes = amd64_direct_mapped_bytes();
	stats->allocator_initial_bytes = allocator_initial_bytes;
	stats->boot_reclaim_bytes = boot_reclaim_bytes;
	stats->boot_memory_source = bsp_memory_source();
	stats->allocator_metadata_bytes = metadata_bytes;
	stats->allocator_max_irqoff_cycles = pmem_max_irqoff_cycles;
	stats->physical_total = (size_t)managed_pages * PAGE_SIZE;
	for (index = 0; index < ram_extent_count; index++) {
		stats->physical_reserved += (size_t)ram_extents[index].reserved_pages * PAGE_SIZE;
		stats->physical_allocated += (size_t)ram_extents[index].allocated_pages * PAGE_SIZE;
		stats->physical_free += (size_t)ram_extents[index].free_pages * PAGE_SIZE;
		stats->allocator_scan_words += ram_extents[index].scanned_words;
		if (ram_extents[index].max_scan_words > stats->allocator_max_extent_scan_words)
			stats->allocator_max_extent_scan_words = ram_extents[index].max_scan_words;
	}

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
	uint64_t entered;

	/* Preserves interrupt state and acquires the global spin lock. */
	enabled = hal_irq_disable();
	entered = pmem_cycles();
	while (__atomic_exchange_n(&pmem_lock, 1U, __ATOMIC_ACQUIRE) != 0)
		__asm__ volatile("pause");

	pmem_entered_cycles = entered;

	/* Returns whether interrupts must be restored on release. */
	return enabled;
}

/* Releases the physical-memory lock and restores interrupt state. */
static void
pmem_lock_leave(
	bool enabled)
{
	uint64_t elapsed;

	elapsed = pmem_cycles() - pmem_entered_cycles;
	if (elapsed > pmem_max_irqoff_cycles)
		pmem_max_irqoff_cycles = elapsed;

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

/* Allocates RAM through the published range geometry. */
static int
alloc_ram(size_t size, size_t alignment, struct hal_pmem *descriptor)
{
	int result;

	/* Preserves DMA32 capacity even when one RAM extent crosses 4 GiB. */
	result = alloc_range(size, alignment, UINT64_C(0x100000000),
	    AMD64_ALLOCATOR_LIMIT - 1U, 0, descriptor);
	if (result != HAL_ERR_NOMEM)
		return result;

	/* Uses lower RAM when the high pool cannot satisfy the whole run. */
	result = alloc_range(size, alignment, 0, UINT32_MAX, 0, descriptor);
	return result;
}

/* Frees exactly one range-owned allocation under the outer allocator lock. */
static int
free_ram(struct hal_pmem *descriptor)
{
	struct amd64_pmem_extent *extent;
	uint32_t index;
	enum amd64_pmem_result result;

	if (!range_ready || descriptor == NULL || descriptor->size == 0 ||
	    descriptor->attr != 0 ||
	    descriptor->vaddr != amd64_phys_to_direct((uintptr_t)descriptor->paddr))
		return HAL_ERR_INVALID;
	for (index = 0; index < ram_extent_count; index++) {
		extent = &ram_extents[index];
		if (descriptor->paddr < extent->base ||
		    descriptor->paddr - extent->base >= extent->pages * PAGE_SIZE)
			continue;
		result = amd64_pmem_extent_free(extent, descriptor->paddr, descriptor->size);
		if (result != AMD64_PMEM_OK)
			return result == AMD64_PMEM_STATE ? HAL_ERR_STATE : HAL_ERR_INVALID;
		hal_memset(descriptor, 0, sizeof(*descriptor));
		return HAL_OK;
	}
	return HAL_ERR_INVALID;
}

/* Resolves a supported fixed physical address to its virtual window. */
static void *
fixed_vaddr(
	hal_physaddr_t physical)
{
	void *address;

	/* Selects the direct legacy-device window. */
	if (physical >= 0x000a0000U && physical < 0x00100000U) {
		address = (void *)((uintptr_t)AMD64_LEGACY_MMIO_BASE + physical - 0xa0000U);

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

/*
 * Transfers typed RAM and early ownership to extent-sized runtime metadata.
 */
void
amd64_range_page_init(void)
{
	uint64_t base;
	uint64_t size;
	uint64_t bytes;
	uint64_t end;
	uint64_t free_pages;
	uint32_t index;
	uint32_t type;
	void *metadata;
	const struct zbl6_framebuffer *framebuffer;

	if (range_ready || allocated_pages != 0)
		HAL_FATAL("amd64 range allocator transition repeated or too late");
	for (index = 0; index < bsp_mem_range_count(); index++) {
		if (!bsp_mem_range(index, &base, &size, &type))
			HAL_FATAL("amd64 allocator range disappeared");
		if ((type != ZBL6_MEMORY_USABLE && type != ZBL6_MEMORY_BOOT_RECLAIM) ||
		    base >= AMD64_ALLOCATOR_LIMIT)
			continue;
		end = base + size;
		if (end > AMD64_ALLOCATOR_LIMIT)
			end = AMD64_ALLOCATOR_LIMIT;
		base = (base + PAGE_SIZE - 1U) & ~(uint64_t)(PAGE_SIZE - 1U);
		end &= ~(uint64_t)(PAGE_SIZE - 1U);
		if (base >= end)
			continue;
		size = end - base;
		bytes = amd64_pmem_metadata_size(size / PAGE_SIZE);
		metadata = early_metadata(bytes);
		if (metadata == NULL || ram_extent_count == ZBL6_MAX_MEMORY_RANGES ||
		    amd64_pmem_extent_init(&ram_extents[ram_extent_count], base, size, metadata, bytes) != AMD64_PMEM_OK)
			HAL_FATAL("amd64 RAM extent metadata allocation failed");
		metadata_bytes += (bytes + PAGE_SIZE - 1U) & ~(uint64_t)(PAGE_SIZE - 1U);
		managed_pages += size / PAGE_SIZE;
		ram_extent_type[ram_extent_count] = type;
		if (type == ZBL6_MEMORY_BOOT_RECLAIM &&
		    amd64_pmem_reserve(&ram_extents[ram_extent_count], base, size) != AMD64_PMEM_OK)
			HAL_FATAL("amd64 boot-reclaim reservation failed");
		ram_extent_count++;
	}
	reserve_managed(0, 0x100000U);
	reserve_managed((uintptr_t)__kernel_phys_start,
	    (uintptr_t)__kernel_phys_end - (uintptr_t)__kernel_phys_start);
	for (index = 0; bsp_boot_allocation(index, &base, &size); index++)
		reserve_managed(base, size);
	for (index = 0; amd64_early_reservation(index, &base, &size); index++)
		reserve_managed(base, size);
	framebuffer = hal_get_arch_handoff("pcat.framebuffer");
	if (framebuffer != NULL)
		reserve_managed(framebuffer->physical_base, framebuffer->size);
	free_pages = 0;
	for (index = 0; index < ram_extent_count; index++)
		free_pages += ram_extents[index].free_pages;
	allocator_initial_bytes = free_pages * PAGE_SIZE;
	range_ready = 1;
	hal_printf("A64 RAM ALLOC extents=%u managed=%llu metadata=%llu free=%llu publication_limit=%llu\n",
	    ram_extent_count, (unsigned long long)(managed_pages * PAGE_SIZE),
	    (unsigned long long)metadata_bytes, (unsigned long long)allocator_initial_bytes,
	    (unsigned long long)AMD64_ALLOCATOR_LIMIT);
}

/* Allocates contiguous bootstrap metadata from already mapped, unowned RAM. */
static void *
early_metadata(uint64_t size)
{
	uint64_t base;
	uint64_t bytes;
	uint64_t end;
	uint64_t candidate;
	uint64_t owner_base;
	uint64_t owner_size;
	uint64_t collision_end;
	uint32_t index;
	uint32_t owner;
	uint32_t type;
	unsigned pass;
	const struct zbl6_framebuffer *framebuffer;

	if (size == 0 || size > UINT64_MAX - (PAGE_SIZE - 1U))
		return NULL;
	size = (size + PAGE_SIZE - 1U) & ~(uint64_t)(PAGE_SIZE - 1U);
	framebuffer = hal_get_arch_handoff("pcat.framebuffer");
	/* Metadata prefers high RAM so it does not consume low DMA capacity. */
	for (pass = 0; pass < 2; pass++) {
		for (index = 0; index < bsp_mem_range_count(); index++) {
			if (!bsp_mem_range(index, &base, &bytes, &type) || type != ZBL6_MEMORY_USABLE)
				continue;
			end = base + bytes;
			candidate = base < 0x100000U ? 0x100000U : base;
			if (pass == 0 && candidate < AMD64_BOOTSTRAP_LIMIT)
				candidate = AMD64_BOOTSTRAP_LIMIT;
			if (pass != 0 && end > AMD64_BOOTSTRAP_LIMIT)
				end = AMD64_BOOTSTRAP_LIMIT;
			candidate = (candidate + PAGE_SIZE - 1U) & ~(uint64_t)(PAGE_SIZE - 1U);
			while (candidate < end && size <= end - candidate) {
				collision_end = candidate;
				if (candidate < (uintptr_t)__kernel_phys_end && candidate + size > (uintptr_t)__kernel_phys_start)
					collision_end = (uintptr_t)__kernel_phys_end;
				if (framebuffer != NULL && candidate < framebuffer->physical_base + framebuffer->size &&
				    candidate + size > framebuffer->physical_base)
					collision_end = framebuffer->physical_base + framebuffer->size;
				for (owner = 0; bsp_boot_allocation(owner, &owner_base, &owner_size); owner++)
					if (candidate < owner_base + owner_size && candidate + size > owner_base &&
					    collision_end < owner_base + owner_size)
						collision_end = owner_base + owner_size;
				for (owner = 0; amd64_early_reservation(owner, &owner_base, &owner_size); owner++)
					if (candidate < owner_base + owner_size && candidate + size > owner_base &&
					    collision_end < owner_base + owner_size)
						collision_end = owner_base + owner_size;
				if (collision_end != candidate) {
					if (collision_end > UINT64_MAX - (PAGE_SIZE - 1U))
						return NULL;
					candidate = (collision_end + PAGE_SIZE - 1U) & ~(uint64_t)(PAGE_SIZE - 1U);
					continue;
				}
				if (amd64_phys_to_direct((uintptr_t)candidate) == NULL ||
				    amd64_phys_to_direct((uintptr_t)(candidate + size - 1U)) == NULL ||
				    !record_early_run(candidate, size))
					return NULL;
				reserve_range((uintptr_t)candidate, (size_t)size);
				return amd64_phys_to_direct((uintptr_t)candidate);
			}
		}
	}
	return NULL;
}

/* Intersects a boot reservation with each actual managed extent. */
static void
reserve_managed(uint64_t base, uint64_t size)
{
	struct amd64_pmem_extent *extent;
	uint64_t end;
	uint64_t first;
	uint64_t last;
	uint32_t index;

	if (size == 0 || size > UINT64_MAX - base || base + size > UINT64_MAX - (PAGE_SIZE - 1U))
		HAL_FATAL("amd64 allocator reservation overflow");
	end = (base + size + PAGE_SIZE - 1U) & ~(uint64_t)(PAGE_SIZE - 1U);
	base &= ~(uint64_t)(PAGE_SIZE - 1U);
	for (index = 0; index < ram_extent_count; index++) {
		extent = &ram_extents[index];
		first = base > extent->base ? base : extent->base;
		last = end < extent->base + extent->pages * PAGE_SIZE ? end : extent->base + extent->pages * PAGE_SIZE;
		if (first < last && amd64_pmem_reserve(extent, first, last - first) != AMD64_PMEM_OK)
			HAL_FATAL("amd64 allocator ownership conflict");
	}
}

/* Searches managed RAM inside the caller's physical constraints. */
static int
alloc_range(size_t size, size_t alignment, uint64_t minimum, uint64_t maximum,
    uint64_t boundary, struct hal_pmem *descriptor)
{
	uint64_t physical;
	uint64_t allocated;
	uint32_t index;
	enum amd64_pmem_result result;

	if (!range_ready || descriptor == NULL)
		return HAL_ERR_STATE;
	for (index = ram_extent_count; index != 0; index--) {
		result = amd64_pmem_extent_alloc(&ram_extents[index - 1U], size, alignment,
		    minimum, maximum, boundary, &physical, &allocated);
		if (result == AMD64_PMEM_NOMEM)
			continue;
		if (result != AMD64_PMEM_OK)
			return HAL_ERR_INVALID;
		descriptor->paddr = physical;
		descriptor->vaddr = amd64_phys_to_direct((uintptr_t)physical);
		descriptor->size = (size_t)allocated;
		descriptor->type = HAL_PMEM_TYPE_RAM;
		descriptor->attr = 0;
		return HAL_OK;
	}
	return HAL_ERR_NOMEM;
}

/*
 * Allocates directly inside a device's physical address and boundary limits.
 */
int
hal_pmem_alloc_range(
	const struct hal_pmem_request *request,
	uint64_t minimum,
	uint64_t maximum,
	uint64_t boundary,
	struct hal_pmem *descriptor)
{
	size_t alignment;
	bool enabled;
	int result;

	if (request == NULL || descriptor == NULL || request->type != HAL_PMEM_TYPE_RAM ||
	    request->paddr != HAL_PMEM_PADDR_ANY || request->attr != 0)
		return HAL_ERR_INVALID;
	alignment = request->alignment == 0 ? PAGE_SIZE : request->alignment;
	if (alignment < PAGE_SIZE || (alignment & (alignment - 1U)) != 0)
		return HAL_ERR_INVALID;
	enabled = pmem_lock_enter();
	result = alloc_range(request->size, alignment, minimum, maximum, boundary, descriptor);
	pmem_lock_leave(enabled);
	return result;
}

/* Samples local cycles; this metric includes lock waiting and is not wall time. */
static uint64_t
pmem_cycles(void)
{
	uint32_t low;
	uint32_t high;

	__asm__ volatile("lfence; rdtsc; lfence" : "=a"(low), "=d"(high) : : "memory");
	return ((uint64_t)high << 32) | low;
}

/*
 * Retires boot-owned RAM after ACPI discovery and CPU topology setup have completed.
 * Future AP startup uses the retained low trampoline and kernel-owned root.
 * Permanent owners override release lifetimes, including overlapping records.
 */
void
amd64_boot_memory_release(void)
{
	struct amd64_pmem_extent *extent;
	uint64_t physical;
	uint64_t end;
	uint64_t first;
	uint64_t released;
	uint32_t index;
	int eligible;
	bool enabled;

	if (!range_ready || boot_memory_released)
		HAL_FATAL("amd64 boot memory retirement out of order");
	amd64_acpi_finish_discovery();
	released = 0;

	/* Releases only contiguous retired runs, preserving all current owners. */
	for (index = 0; index < ram_extent_count; index++) {
		extent = &ram_extents[index];
		end = extent->base + extent->pages * PAGE_SIZE;
		first = end;
		for (physical = extent->base; physical <= end; physical += PAGE_SIZE) {
			eligible = 0;
			if (physical < end) {
				eligible = ram_extent_type[index] == ZBL6_MEMORY_BOOT_RECLAIM ||
				    boot_page_retired(physical);
				if (eligible && boot_page_retained(physical))
					eligible = 0;
			}
			/* Skips normal RAM between the few obsolete boot allocations. */
			if (!eligible && first == end && physical < end &&
			    ram_extent_type[index] != ZBL6_MEMORY_BOOT_RECLAIM) {
				physical = next_retired_page(physical, end) - PAGE_SIZE;
				continue;
			}
			if (eligible && first == end)
				first = physical;
			if (!eligible && first != end) {
				enabled = pmem_lock_enter();
				if (amd64_pmem_release_reserved(extent, first, physical - first) != AMD64_PMEM_OK)
					HAL_FATAL("amd64 boot retirement ownership conflict");
				pmem_lock_leave(enabled);
				released += physical - first;
				first = end;
			}
		}
	}
	boot_memory_released = 1;
	allocator_initial_bytes += released;
	hal_printf("A64 BOOT RECLAIM released=%llu bytes ACPI/AP owners retained\n",
	    (unsigned long long)released);
}

/* Determines whether an obsolete loader claim covers this whole page. */
static int
boot_page_retired(uint64_t physical)
{
	uint64_t base;
	uint64_t size;
	uint32_t index;
	uint32_t lifetime;

	/* Searches validated and page-aligned boot claims. */
	for (index = 0; bsp_boot_allocation(index, &base, &size); index++) {
		if (bsp_boot_allocation_lifetime(index, &lifetime) &&
		    lifetime != ZBL6_BOOT_KEEP && physical >= base && physical - base < size)
			return 1;
	}
	return 0;
}

/* Gives permanent owners precedence over an overlapping retired claim. */
static int
boot_page_retained(uint64_t physical)
{
	uint64_t base;
	uint64_t size;
	uint32_t index;
	uint32_t lifetime;
	const struct zbl6_framebuffer *framebuffer;

	if (physical < 0x100000U ||
	    (physical >= (uintptr_t)__kernel_phys_start && physical < (uintptr_t)__kernel_phys_end) ||
	    amd64_acpi_page_reserved(physical))
		return 1;
	framebuffer = hal_get_arch_handoff("pcat.framebuffer");
	if (framebuffer != NULL && physical < framebuffer->physical_base + framebuffer->size &&
	    physical + PAGE_SIZE > framebuffer->physical_base)
		return 1;

	/* Keeps the table arena and allocator metadata regardless of boot type. */
	for (index = 0; amd64_early_reservation(index, &base, &size); index++) {
		if (physical >= base && physical - base < size)
			return 1;
	}

	/* Keeps every explicit permanent loader record. */
	for (index = 0; bsp_boot_allocation(index, &base, &size); index++) {
		if (bsp_boot_allocation_lifetime(index, &lifetime) &&
		    lifetime == ZBL6_BOOT_KEEP && physical >= base && physical - base < size)
			return 1;
	}
	return 0;
}

/* Skips pages without a retired boot claim; called only outside a release run. */
static uint64_t
next_retired_page(uint64_t physical, uint64_t end)
{
	uint64_t base;
	uint64_t size;
	uint64_t next;
	uint32_t index;
	uint32_t lifetime;

	next = end;
	/* Finds the earliest later page that an obsolete claim could own. */
	for (index = 0; bsp_boot_allocation(index, &base, &size); index++) {
		if (!bsp_boot_allocation_lifetime(index, &lifetime) || lifetime == ZBL6_BOOT_KEEP)
			continue;
		if (physical >= base && physical - base < size)
			return physical + PAGE_SIZE;
		if (base > physical && base < next)
			next = base;
	}
	return next;
}
