/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The i386 protected user-address-space implementation.
 */

#include <hal/hal.h>

#include "apic-topology.h"
#include "defs.h"
#include "irq.h"
#include "smp.h"
#include "space.h"

#define I386_SHOOTDOWN_REQUESTS (I386_APIC_MAX_CPUS * 4U)
#define current_space current_spaces[hal_cpu_current()]

struct i386_shootdown_request {
	volatile unsigned active;
	hal_space_t space;
	void *vaddr;
	size_t size;
	volatile unsigned pending;
	volatile unsigned lock;
};

typedef char i386_user_pointer_range_must_fit[
	(uintptr_t)(SYS_START - 1U) <= (uintptr_t)INTPTR_MAX ? 1 : -1];
typedef char i386_shootdown_mask_must_fit[
	I386_APIC_MAX_CPUS <= 32U ? 1 : -1];

static hal_space_t current_spaces[HAL_CPU_MAX];
static uint32_t system_cr3;
static int next_space_id;
static uint32_t space_count;
static uint32_t page_table_count;
static volatile unsigned count_lock;
static volatile unsigned shootdown_pool_lock;
static volatile unsigned space_registry_lock;
static struct i386_space *space_registry;
static struct i386_shootdown_request shootdowns[I386_SHOOTDOWN_REQUESTS];

static bool raw_lock_enter(volatile unsigned *lock);
static void raw_lock_leave(volatile unsigned *lock, bool enabled);
static int space_op_enter(struct i386_space *space);
static void space_op_leave(struct i386_space *space);
static void table_count_drop(void);
static int alloc_page(struct hal_pmem *memory);
static bool space_lock_enter(struct i386_space *space);
static void space_lock_leave(struct i386_space *space, bool enabled);
static struct i386_page_table *find_table(struct i386_space *space, uintptr_t vaddr);
static struct i386_page_table *create_table(struct i386_space *space, uintptr_t vaddr);
static struct i386_page_table *detach_empty_tables(struct i386_space *space);
static void free_detached_tables(struct i386_page_table *table);
static int valid_user_range(uintptr_t vaddr, size_t size);
static uint32_t pte_flags(uint32_t attr);
static uint32_t pte_replace_preserve_ad(uint32_t *entry, uint32_t replacement);
static void shootdown(hal_space_t handle, void *vaddr, size_t size);
static void service_shootdowns(hal_cpu_id_t cpu);

/*
 * Initializes bootstrap-CPU address-space and shootdown state.
 */
void
i386_space_init(
	void)
{
	/* Captures the kernel page directory and clears per-CPU state. */
	system_cr3 = asm_get_cr3();
	hal_memset(current_spaces, 0, sizeof(current_spaces));
	hal_memset(shootdowns, 0, sizeof(shootdowns));

	/* Initializes locks, registries, and the bootstrap current space. */
	count_lock = 0;
	shootdown_pool_lock = 0;
	space_registry_lock = 0;
	space_registry = NULL;
	__atomic_store_n(&current_space, HAL_SPACE_SYS, __ATOMIC_RELEASE);

	/* Initializes address-space and page-table accounting. */
	next_space_id = 1;
	page_table_count = 0;
	space_count = 0;
}

/*
 * Initializes address-space state for one secondary CPU.
 */
void
i386_space_init_secondary(
	void)
{
	/* Installs the shared kernel directory before publishing current space. */
	asm_load_cr3(system_cr3);
	__atomic_store_n(&current_space, HAL_SPACE_SYS, __ATOMIC_RELEASE);
}

/*
 * Creates one empty i386 user address space.
 */
hal_space_t
hal_mem_create_space(
	void)
{
	struct i386_space *space;
	uint32_t *system_pdt;
	unsigned i;
	bool enabled;
	int result;

	/* Allocates and clears the software address-space record. */
	space = hal_malloc(sizeof(*space));

	/* Reports allocation failure before initializing the record. */
	if (space == NULL)
		return NULL;
	hal_memset(space, 0, sizeof(*space));

	/* Allocates the physical page-directory page. */
	result = alloc_page(&space->directory_memory);

	/* Releases the software record when directory allocation fails. */
	if (result != HAL_OK) {
		hal_free(space);
		return NULL;
	}

	/* Clears the directory and copies the kernel half without user access. */
	space->pdt = space->directory_memory.vaddr;
	hal_memset(space->pdt, 0, PAGE_SIZE);
	system_pdt = (uint32_t *)(system_cr3 | SYS_START);
	for (i = 512; i < 1024; i++)
		space->pdt[i] = system_pdt[i] & ~PTE_USER;
	space->magic = I386_SPACE_MAGIC;

	/* Assigns an identity and accounts for the new address space. */
	enabled = raw_lock_enter(&count_lock);
	space->space_id = next_space_id;
	next_space_id++;
	space_count++;
	raw_lock_leave(&count_lock, enabled);

	/* Publishes the initialized space in the lifetime registry. */
	enabled = raw_lock_enter(&space_registry_lock);
	space->registry_next = space_registry;
	space_registry = space;
	raw_lock_leave(&space_registry_lock, enabled);

	/* Returns the registered address-space handle. */
	return space;
}

/*
 * Destroys one inactive i386 user address space.
 */
void
hal_page_destroy_space(
	hal_space_t handle)
{
	struct i386_space *space;
	struct i386_space **link;
	struct i386_page_table *table;
	struct hal_cpu_mask ready;
	hal_cpu_id_t cpu;
	unsigned active;
	bool enabled;

	/* Ignores an empty address-space handle. */
	space = handle;
	if (space == NULL)
		return;

	/* Removes the space from the registry and prevents new operations. */
	enabled = raw_lock_enter(&space_registry_lock);
	link = &space_registry;
	while (*link != NULL && *link != space)
		link = &(*link)->registry_next;

	/* Rejects an absent handle or repeated destruction. */
	if (*link == NULL || space->destroying)
		HAL_FATAL("invalid i386 space destroy");
	space->destroying = 1U;
	*link = space->registry_next;
	raw_lock_leave(&space_registry_lock, enabled);

	/* Waits for every admitted address-space operation to leave. */
	for (;;) {
		enabled = raw_lock_enter(&space_registry_lock);
		active = space->active_ops;
		raw_lock_leave(&space_registry_lock, enabled);

		/* Stops after the final admitted operation leaves. */
		if (active == 0)
			break;

		/* Services reciprocal invalidations before retrying the lifetime wait. */
		service_shootdowns(hal_cpu_current());
		__asm__ volatile("pause");
	}

	/* Closes remote hardware page-walk windows for the destroyed space. */
	shootdown(space, NULL, 0);

	/* Rejects destruction while any ready CPU still publishes the space. */
	hal_cpu_ready_mask(&ready);
	for (cpu = 0; cpu < hal_cpu_count(); cpu++) {
		/* Rejects a ready CPU still executing in the destroyed space. */
		if (hal_cpu_mask_test(&ready, cpu) &&
		    __atomic_load_n(
		    &current_spaces[cpu],
		    __ATOMIC_ACQUIRE) == space) {
			HAL_FATAL("destroying an active i386 space");
		}
	}

	/* Releases every page table and its physical backing page. */
	while ((table = space->page_tables) != NULL) {
		space->page_tables = table->next;
		(void)hal_pmem_free(&table->memory);
		hal_free(table);
		table_count_drop();
	}

	/* Invalidates and releases the directory and software record. */
	space->magic = 0;
	(void)hal_pmem_free(&space->directory_memory);
	hal_free(space);

	/* Decrements the global address-space count under its lock. */
	enabled = raw_lock_enter(&count_lock);

	/* Rejects an address-space accounting underflow. */
	if (space_count == 0)
		HAL_FATAL("i386 space counter underflow");
	space_count--;
	raw_lock_leave(&count_lock, enabled);
}

/*
 * Reports i386 address-space and page-table memory usage.
 */
void
hal_i386_space_memory_stats(
	uint32_t *spaces,
	uint32_t *tables)
{
	bool enabled;

	/* Samples both counters while holding their raw lock. */
	enabled = raw_lock_enter(&count_lock);

	/* Publishes the address-space count when requested. */
	if (spaces != NULL)
		*spaces = space_count;

	/* Publishes the page-table count when requested. */
	if (tables != NULL)
		*tables = page_table_count;
	raw_lock_leave(&count_lock, enabled);
}

/*
 * Switches the current CPU to one i386 address space.
 */
void
hal_page_switch_space(
	hal_space_t handle)
{
	struct i386_space *space;
	hal_space_t old;
	uint32_t cr3;
	bool enabled;

	/* Avoids a redundant page-directory reload. */
	old = __atomic_load_n(&current_space, __ATOMIC_ACQUIRE);

	/* Returns without reloading an already current directory. */
	if (handle == old)
		return;

	/* Switches directly to the immortal system address space. */
	if (handle == HAL_SPACE_SYS) {
		enabled = hal_irq_disable();
		asm_load_cr3(system_cr3);
		__atomic_store_n(&current_space, handle, __ATOMIC_RELEASE);

		/* Restores interrupts only when they were previously enabled. */
		if (enabled)
			hal_irq_enable();
		return;
	}

	/* Admits and serializes an operation on the requested user space. */
	space = handle;

	/* Rejects an invalid or destroying user-space handle. */
	if (!space_op_enter(space))
		HAL_FATAL("invalid i386 space switch");
	enabled = space_lock_enter(space);

	/* Loads the user page directory before publishing current space. */
	cr3 = (uint32_t)space->directory_memory.paddr;
	asm_load_cr3(cr3);
	__atomic_store_n(&current_space, handle, __ATOMIC_RELEASE);

	/* Releases the space lock and lifetime admission. */
	space_lock_leave(space, enabled);
	space_op_leave(space);
}

/*
 * Maps a physical interval into one i386 user address space.
 */
int
hal_page_map(
	hal_space_t handle,
	void *address,
	hal_physaddr_t paddr,
	size_t size,
	uint32_t attr)
{
	struct i386_space *space;
	struct i386_page_table *table;
	struct i386_page_table *detached;
	uintptr_t vaddr;
	uintptr_t offset;
	uintptr_t rollback;
	unsigned index;
	bool enabled;

	/* Resolves the requested virtual address and user-space handle. */
	space = handle;
	vaddr = (uintptr_t)address;

	/* Validates the space and virtual interval before physical arithmetic. */
	if (space == NULL)
		return HAL_ERR_INVALID;

	/* Rejects an invalid or unaligned user virtual interval. */
	if (!valid_user_range(vaddr, size))
		return HAL_ERR_INVALID;

	/* Validates physical alignment, extent, and requested access. */
	if ((paddr & (PAGE_SIZE - 1U)) != 0 ||
	    paddr > UINT32_MAX - (size - PAGE_SIZE) ||
	    (attr & (HAL_SPACE_READ | HAL_SPACE_WRITE | HAL_SPACE_EXEC)) == 0) {
		return HAL_ERR_INVALID;
	}

	/* Admits and serializes the mapping operation. */
	if (!space_op_enter(space))
		return HAL_ERR_STATE;
	enabled = space_lock_enter(space);

	/* Validates the complete interval before changing any PTE. */
	for (offset = 0; offset < size; offset += PAGE_SIZE) {
		table = find_table(space, vaddr + offset);
		index = (unsigned)((vaddr + offset) >> 12) & 1023U;

		/* Rejects a page which is already mapped. */
		if (table != NULL && (table->pte[index] & PTE_PRESENT) != 0U) {
			space_lock_leave(space, enabled);
			space_op_leave(space);
			return HAL_ERR_INVALID;
		}
	}

	/* Creates missing page tables and publishes every requested PTE. */
	for (offset = 0; offset < size; offset += PAGE_SIZE) {
		table = find_table(space, vaddr + offset);
		index = (unsigned)((vaddr + offset) >> 12) & 1023U;

		/* Allocates a missing page table for this virtual address. */
		if (table == NULL)
			table = create_table(space, vaddr + offset);

		/* Rolls back every earlier PTE after a table-allocation failure. */
		if (table == NULL) {
			/* Clears every mapping installed earlier in this operation. */
			for (rollback = 0;
			     rollback < offset;
			     rollback += PAGE_SIZE) {
				table = find_table(space, vaddr + rollback);

				/* Clears a prior mapping only when its table still exists. */
				if (table != NULL) {
					table->pte[
					    ((vaddr + rollback) >> 12) & 1023U] = 0;
				}
			}

			/* Detaches empty parents before choosing the invalidation scope. */
			detached = detach_empty_tables(space);

			/* Flushes all translations if parent tables were detached. */
			if (detached != NULL) {
				shootdown(space, NULL, 0);
			} else if (offset != 0) {
				shootdown(space, address, offset);
			}

			/* Releases rollback storage and operation ownership. */
			free_detached_tables(detached);
			space_lock_leave(space, enabled);
			space_op_leave(space);
			return HAL_ERR_NOMEM;
		}

		/* Publishes this page's physical address and requested PTE flags. */
		table->pte[index] =
		    (uint32_t)(paddr + offset) | pte_flags(attr);
	}

	/* Invalidates translations before releasing operation ownership. */
	shootdown(space, address, size);
	space_lock_leave(space, enabled);
	space_op_leave(space);

	/* Reports a completely mapped interval. */
	return HAL_OK;
}

/*
 * Changes protection across one i386 user mapping.
 */
int
hal_page_prot(
	hal_space_t handle,
	void *address,
	size_t size,
	uint32_t attr)
{
	int result;

	/* Changes protection without requesting observed PTE flags. */
	result = hal_page_prot_query(handle, address, size, attr, NULL);

	/* Returns the protection-change result. */
	return result;
}

/*
 * Changes protection and reports observed flags across one i386 mapping.
 */
int
hal_page_prot_query(
	hal_space_t handle,
	void *address,
	size_t size,
	uint32_t attr,
	uint32_t *flags)
{
	struct i386_space *space;
	struct i386_page_table *table;
	uintptr_t vaddr;
	uintptr_t offset;
	uint32_t observed;
	uint32_t snapshot;
	uint32_t old;
	uint32_t entry;
	unsigned index;
	bool enabled;

	/* Resolves the requested virtual address and user-space handle. */
	space = handle;
	vaddr = (uintptr_t)address;
	observed = 0;

	/* Validates the space, interval, and requested access. */
	if (space == NULL)
		return HAL_ERR_INVALID;

	/* Rejects an invalid or unaligned user virtual interval. */
	if (!valid_user_range(vaddr, size))
		return HAL_ERR_INVALID;

	/* Requires at least one access permission. */
	if ((attr & (HAL_SPACE_READ | HAL_SPACE_WRITE | HAL_SPACE_EXEC)) == 0)
		return HAL_ERR_INVALID;

	/* Admits and serializes the protection operation. */
	if (!space_op_enter(space))
		return HAL_ERR_STATE;
	enabled = space_lock_enter(space);

	/* Validates the complete range before changing the first PTE. */
	for (offset = 0; offset < size; offset += PAGE_SIZE) {
		table = find_table(space, vaddr + offset);
		index = (unsigned)((vaddr + offset) >> 12) & 1023U;

		/* Rejects the first absent mapping in the requested interval. */
		if (table == NULL ||
		    (table->pte[index] & PTE_PRESENT) == 0U) {
			space_lock_leave(space, enabled);
			space_op_leave(space);
			return HAL_ERR_INVALID;
		}
	}

	/* Replaces each PTE while preserving hardware accessed and dirty bits. */
	for (offset = 0; offset < size; offset += PAGE_SIZE) {
		table = find_table(space, vaddr + offset);
		index = (unsigned)((vaddr + offset) >> 12) & 1023U;
		snapshot = table->pte[index];
		old = pte_replace_preserve_ad(
			&table->pte[index],
			(snapshot & 0xfffff000U) | pte_flags(attr));

		/* Accumulates an accessed bit observed during replacement. */
		if ((old & PTE_ACCESS) != 0U)
			observed |= HAL_PAGE_ACCESSED;

		/* Accumulates a dirty bit observed during replacement. */
		if ((old & PTE_DIRTY) != 0U)
			observed |= HAL_PAGE_DIRTY;
	}

	/* Invalidates every changed translation before verifying hardware state. */
	shootdown(space, address, size);

	/* Re-reads the complete range and accumulates observed PTE flags. */
	for (offset = 0; offset < size; offset += PAGE_SIZE) {
		table = find_table(space, vaddr + offset);
		index = (unsigned)((vaddr + offset) >> 12) & 1023U;

		/* Rejects a table which disappeared while the space lock was held. */
		if (table == NULL) {
			space_lock_leave(space, enabled);
			space_op_leave(space);
			return HAL_ERR_STATE;
		}
		entry = table->pte[index];

		/* Rejects a PTE which disappeared while the space lock was held. */
		if ((entry & PTE_PRESENT) == 0U) {
			space_lock_leave(space, enabled);
			space_op_leave(space);
			return HAL_ERR_STATE;
		}

		/* Accumulates current presence and hardware-maintained state. */
		observed |= HAL_PAGE_PRESENT;

		/* Accumulates the current accessed bit. */
		if ((entry & PTE_ACCESS) != 0U)
			observed |= HAL_PAGE_ACCESSED;

		/* Accumulates the current dirty bit. */
		if ((entry & PTE_DIRTY) != 0U)
			observed |= HAL_PAGE_DIRTY;
	}

	/* Releases operation ownership before publishing optional flags. */
	space_lock_leave(space, enabled);
	space_op_leave(space);

	/* Publishes accumulated flags when result storage was requested. */
	if (flags != NULL)
		*flags = observed;

	/* Reports a completely changed and verified interval. */
	return HAL_OK;
}

/*
 * Removes mappings across one i386 user interval.
 */
int
hal_page_unmap(
	hal_space_t handle,
	void *address,
	size_t size)
{
	struct i386_space *space;
	struct i386_page_table *table;
	struct i386_page_table *detached;
	uintptr_t vaddr;
	uintptr_t offset;
	unsigned index;
	bool enabled;

	/* Treats an empty unmap request as already complete. */
	if (size == 0)
		return HAL_OK;

	/* Resolves and validates the requested user interval. */
	space = handle;
	vaddr = (uintptr_t)address;

	/* Rejects a missing user-space handle. */
	if (space == NULL)
		return HAL_ERR_INVALID;

	/* Rejects an invalid or unaligned user virtual interval. */
	if (!valid_user_range(vaddr, size))
		return HAL_ERR_INVALID;

	/* Admits and serializes the unmap operation. */
	if (!space_op_enter(space))
		return HAL_ERR_STATE;
	enabled = space_lock_enter(space);

	/* Clears every PTE currently backed by a page table. */
	for (offset = 0; offset < size; offset += PAGE_SIZE) {
		table = find_table(space, vaddr + offset);
		index = (unsigned)((vaddr + offset) >> 12) & 1023U;

		/* Clears this PTE only when its parent table exists. */
		if (table != NULL)
			table->pte[index] = 0;
	}

	/* Detaches empty parent tables before acknowledging invalidation. */
	detached = detach_empty_tables(space);

	/* Flushes all translations if a parent table was detached. */
	if (detached != NULL) {
		shootdown(space, NULL, 0);
	} else {
		shootdown(space, address, size);
	}

	/* Releases detached storage and operation ownership. */
	free_detached_tables(detached);
	space_lock_leave(space, enabled);
	space_op_leave(space);

	/* Reports a completely unmapped interval. */
	return HAL_OK;
}

/*
 * Reports hardware-maintained flags for one i386 user page.
 */
int
hal_page_query(
	hal_space_t handle,
	void *address,
	uint32_t *flags)
{
	struct i386_space *space;
	struct i386_page_table *table;
	uintptr_t vaddr;
	uint32_t pte;
	unsigned index;
	bool enabled;

	/* Resolves and validates the requested page and result storage. */
	space = handle;
	vaddr = (uintptr_t)address;

	/* Requires both a user-space handle and result storage. */
	if (space == NULL || flags == NULL)
		return HAL_ERR_INVALID;

	/* Rejects an invalid or unaligned user page. */
	if (!valid_user_range(vaddr, PAGE_SIZE))
		return HAL_ERR_INVALID;

	/* Admits and serializes the page query. */
	if (!space_op_enter(space))
		return HAL_ERR_STATE;
	enabled = space_lock_enter(space);

	/* Reads the PTE or treats a missing table as an absent mapping. */
	table = find_table(space, vaddr);
	index = (unsigned)(vaddr >> 12) & 1023U;

	/* Selects the stored PTE only when its parent table exists. */
	if (table != NULL) {
		pte = table->pte[index];
	} else {
		pte = 0;
	}

	/* Converts the hardware PTE state to public page flags. */
	*flags = (pte & PTE_PRESENT ? HAL_PAGE_PRESENT : 0) |
	    (pte & PTE_ACCESS ? HAL_PAGE_ACCESSED : 0) |
	    (pte & PTE_DIRTY ? HAL_PAGE_DIRTY : 0);

	/* Releases operation ownership after the PTE snapshot. */
	space_lock_leave(space, enabled);
	space_op_leave(space);

	/* Reports a completed page query. */
	return HAL_OK;
}

/*
 * Clears selected hardware-maintained flags from one i386 user page.
 */
int
hal_page_clear_flags(
	hal_space_t handle,
	void *address,
	uint32_t flags)
{
	struct i386_space *space;
	struct i386_page_table *table;
	uintptr_t vaddr;
	unsigned index;
	uint32_t mask;
	uint32_t keep;
	bool enabled;

	/* Resolves the requested virtual page and user-space handle. */
	space = handle;
	vaddr = (uintptr_t)address;
	mask = 0;

	/* Validates the space, page, and clearable flag set. */
	if (space == NULL)
		return HAL_ERR_INVALID;

	/* Rejects an invalid or unaligned user page. */
	if (!valid_user_range(vaddr, PAGE_SIZE))
		return HAL_ERR_INVALID;

	/* Rejects flags outside the hardware-maintained clearable set. */
	if ((flags & ~(HAL_PAGE_ACCESSED | HAL_PAGE_DIRTY)) != 0)
		return HAL_ERR_INVALID;

	/* Admits and serializes the flag-clear operation. */
	if (!space_op_enter(space))
		return HAL_ERR_STATE;
	enabled = space_lock_enter(space);

	/* Requires a present PTE for the requested page. */
	table = find_table(space, vaddr);
	index = (unsigned)(vaddr >> 12) & 1023U;
	if (table == NULL || (table->pte[index] & PTE_PRESENT) == 0U) {
		space_lock_leave(space, enabled);
		space_op_leave(space);
		return HAL_ERR_INVALID;
	}

	/* Includes the hardware accessed bit when requested. */
	if ((flags & HAL_PAGE_ACCESSED) != 0U)
		mask |= PTE_ACCESS;

	/* Includes the hardware dirty bit when requested. */
	if ((flags & HAL_PAGE_DIRTY) != 0U)
		mask |= PTE_DIRTY;
	keep = ~mask;

	/* Atomically clears the bits before invalidating the translation. */
	__asm__ volatile("lock; andl %1, %0"
	    : "+m"(table->pte[index])
	    : "r"(keep)
	    : "memory", "cc");
	shootdown(space, address, PAGE_SIZE);

	/* Releases operation ownership after invalidation. */
	space_lock_leave(space, enabled);
	space_op_leave(space);

	/* Reports a completed flag clear. */
	return HAL_OK;
}

/*
 * Services pending TLB shootdowns on the current CPU.
 */
void
i386_tlb_interrupt(
	void)
{
	/* Invalidates pending requests before completing the IPI. */
	service_shootdowns(hal_cpu_current());
	hal_irq_send_eoi(IRQ_MAX + 2U);
}

/*
 * Flushes all translations for one address space.
 */
void
hal_page_flush_tlb(
	hal_space_t handle)
{
	struct i386_space *space;
	bool enabled;

	/* Flushes the immortal system space without lifetime admission. */
	space = handle;

	/* Flushes the shared directory directly. */
	if (handle == HAL_SPACE_SYS) {
		shootdown(handle, NULL, 0);
		return;
	}

	/* Admits, serializes, and flushes one user address space. */
	if (!space_op_enter(space))
		HAL_FATAL("invalid i386 space flush");
	enabled = space_lock_enter(space);
	shootdown(space, NULL, 0);
	space_lock_leave(space, enabled);
	space_op_leave(space);
}

/*
 * Flushes one translation interval for an address space.
 */
void
hal_page_flush_tlb_range(
	hal_space_t handle,
	void *vaddr,
	size_t size)
{
	struct i386_space *space;
	bool enabled;

	/* Ignores an empty translation interval. */
	if (size == 0)
		return;

	/* Flushes the immortal system space without lifetime admission. */
	space = handle;

	/* Flushes the shared directory interval directly. */
	if (handle == HAL_SPACE_SYS) {
		shootdown(handle, vaddr, size);
		return;
	}

	/* Admits, serializes, and flushes one user-space interval. */
	if (!space_op_enter(space))
		HAL_FATAL("invalid i386 space range flush");
	enabled = space_lock_enter(space);
	shootdown(space, vaddr, size);
	space_lock_leave(space, enabled);
	space_op_leave(space);
}

/*
 * Reports the page size for one i386 translation level.
 */
size_t
hal_page_get_page_size(
	int level)
{
	/* Supports only the ordinary first page-table level. */
	if (level == 1)
		return PAGE_SIZE;

	/* Reports an unsupported translation level. */
	return 0;
}

/*
 * Reports the valid i386 user virtual-address interval.
 */
void
hal_page_get_user_range(
	uintptr_t *minimum,
	uintptr_t *limit)
{
	/* Publishes each requested bound independently. */
	if (minimum != NULL)
		*minimum = PAGE_SIZE;

	/* Publishes the kernel split as the user limit when requested. */
	if (limit != NULL)
		*limit = SYS_START;
}

/* Acquires one raw spin lock with interrupts disabled. */
static bool
raw_lock_enter(
	volatile unsigned *lock)
{
	unsigned previous;
	bool enabled;

	/* Preserves interrupt state before attempting the lock. */
	enabled = hal_irq_disable();

	/* Exchanges until this CPU observes and acquires an unlocked value. */
	for (;;) {
		previous = 1U;
		__asm__ volatile("xchgl %0, %1"
		    : "+r"(previous), "+m"(*lock)
		    :
		    : "memory");

		/* Returns after the exchange acquires an unlocked value. */
		if (previous == 0U)
			return enabled;

		/* Reduces contention before retrying the raw lock exchange. */
		__asm__ volatile("pause");
	}
}

/* Releases one raw spin lock and restores interrupt state. */
static void
raw_lock_leave(
	volatile unsigned *lock,
	bool enabled)
{
	unsigned previous;

	/* Exchanges the lock to zero and records its prior ownership state. */
	previous = 0U;
	__asm__ volatile("xchgl %0, %1"
	    : "+r"(previous), "+m"(*lock)
	    :
	    : "memory");

	/* Rejects a release of a lock which was not held. */
	if (previous != 1U)
		HAL_FATAL("invalid i386 raw lock release");

	/* Restores interrupts only when they were previously enabled. */
	if (enabled)
		hal_irq_enable();
}

/* Admits one operation against a live address space. */
static int
space_op_enter(
	struct i386_space *space)
{
	struct i386_space *item;
	bool enabled;

	/* Serializes the lifetime registry before searching for the handle. */
	enabled = raw_lock_enter(&space_registry_lock);

	/* Searches the registry for the exact address-space handle. */
	for (item = space_registry;
	     item != NULL;
	     item = item->registry_next) {
		/* Stops at the exact address-space handle. */
		if (item == space)
			break;
	}

	/* Rejects an absent or already-destroying space. */
	if (item == NULL || item->destroying) {
		raw_lock_leave(&space_registry_lock, enabled);
		return 0;
	}

	/* Counts the admitted operation before releasing the registry. */
	item->active_ops++;
	raw_lock_leave(&space_registry_lock, enabled);

	/* Reports a live admitted operation. */
	return 1;
}

/* Releases one admitted address-space operation. */
static void
space_op_leave(
	struct i386_space *space)
{
	bool enabled;

	/* Serializes and validates the address-space operation counter. */
	enabled = raw_lock_enter(&space_registry_lock);

	/* Rejects an address-space operation counter underflow. */
	if (space->active_ops == 0)
		HAL_FATAL("i386 space operation counter underflow");

	/* Releases this operation count and the registry serializer. */
	space->active_ops--;
	raw_lock_leave(&space_registry_lock, enabled);
}

/* Decrements the allocated page-table count. */
static void
table_count_drop(
	void)
{
	bool enabled;

	/* Serializes and validates the page-table counter. */
	enabled = raw_lock_enter(&count_lock);

	/* Rejects a page-table accounting underflow. */
	if (page_table_count == 0)
		HAL_FATAL("i386 page-table counter underflow");

	/* Releases this page-table count and the accounting serializer. */
	page_table_count--;
	raw_lock_leave(&count_lock, enabled);
}

/* Allocates one zero-owner physical page for page-table use. */
static int
alloc_page(
	struct hal_pmem *memory)
{
	const struct hal_pmem_request request = {
		HAL_PMEM_PADDR_ANY,
		PAGE_SIZE,
		PAGE_SIZE,
		HAL_PMEM_TYPE_RAM,
		0
	};
	int result;

	/* Allocates the fixed-size aligned RAM page. */
	result = hal_pmem_alloc(&request, memory);

	/* Returns the physical allocator result. */
	return result;
}

/* Acquires one address-space serializer with interrupts disabled. */
static bool
space_lock_enter(
	struct i386_space *space)
{
	unsigned previous;
	bool enabled;

	/* Preserves interrupt state before attempting the serializer. */
	enabled = hal_irq_disable();

	/* Waits while servicing IPIs needed by the current lock owner. */
	for (;;) {
		previous = 1U;
		__asm__ volatile("xchgl %0, %1"
		    : "+r"(previous), "+m"(space->lock)
		    :
		    : "memory");

		/* Returns after the exchange acquires an unlocked value. */
		if (previous == 0U)
			return enabled;

		/* Services reciprocal invalidations before retrying the space lock. */
		service_shootdowns(hal_cpu_current());
		__asm__ volatile("pause");
	}
}

/* Releases one address-space serializer and restores interrupts. */
static void
space_lock_leave(
	struct i386_space *space,
	bool enabled)
{
	/* Releases the embedded raw lock through the common path. */
	raw_lock_leave(&space->lock, enabled);
}

/* Finds the page table containing one user virtual address. */
static struct i386_page_table *
find_table(
	struct i386_space *space,
	uintptr_t vaddr)
{
	struct i386_page_table *table;
	uintptr_t base_address;

	/* Derives the four-MiB virtual range containing the address. */
	base_address = vaddr & 0xffc00000U;

	/* Searches the address space's page-table list. */
	for (table = space->page_tables;
	     table != NULL;
	     table = table->next) {
		/* Returns the table covering the requested virtual range. */
		if (table->vaddr == base_address)
			return table;
	}

	/* Reports that the address has no allocated page table. */
	return NULL;
}

/* Creates and attaches the page table containing one virtual address. */
static struct i386_page_table *
create_table(
	struct i386_space *space,
	uintptr_t vaddr)
{
	struct i386_page_table *table;
	unsigned pde;
	bool enabled;
	int result;

	/* Resolves the page-directory entry for the requested address. */
	pde = (unsigned)(vaddr >> 22);

	/* Allocates and clears the software page-table record. */
	table = hal_malloc(sizeof(*table));

	/* Reports allocation failure before initializing the record. */
	if (table == NULL)
		return NULL;
	hal_memset(table, 0, sizeof(*table));

	/* Allocates its physical PTE page or releases the record on failure. */
	result = alloc_page(&table->memory);

	/* Releases the software record when page allocation fails. */
	if (result != HAL_OK) {
		hal_free(table);
		return NULL;
	}

	/* Initializes and attaches the empty page table. */
	table->vaddr = vaddr & 0xffc00000U;
	table->pte = table->memory.vaddr;
	hal_memset(table->pte, 0, PAGE_SIZE);
	table->next = space->page_tables;
	space->page_tables = table;

	/* Accounts for the attached page table. */
	enabled = raw_lock_enter(&count_lock);
	page_table_count++;
	raw_lock_leave(&count_lock, enabled);

	/* Publishes the user-writable page-directory entry last. */
	space->pdt[pde] =
	    ((uint32_t)table->memory.paddr & 0xfffff000U) |
	    PTE_PRESENT | PTE_WRITE | PTE_USER;

	/* Returns the attached page table. */
	return table;
}

/* Detaches empty page tables while retaining storage for shootdown. */
static struct i386_page_table *
detach_empty_tables(
	struct i386_space *space)
{
	struct i386_page_table **link;
	struct i386_page_table *detached;
	struct i386_page_table *table;
	unsigned index;

	/* Initializes the live-list cursor and detached-list result. */
	link = &space->page_tables;
	detached = NULL;

	/* Examines every attached page table for present PTEs. */
	while (*link != NULL) {
		table = *link;

		/* Searches this table for its first present PTE. */
		for (index = 0; index < 1024U; index++) {
			/* Stops after finding a present mapping. */
			if ((table->pte[index] & PTE_PRESENT) != 0U)
				break;
		}

		/* Retains a nonempty table and advances its live-list link. */
		if (index != 1024U) {
			link = &table->next;
			continue;
		}

		/* Clears the parent PDE and prepends the table for later release. */
		space->pdt[table->vaddr >> 22] = 0;
		*link = table->next;
		table->next = detached;
		detached = table;
	}

	/* Returns the detached tables in reverse discovery order. */
	return detached;
}

/* Releases a list of detached page tables. */
static void
free_detached_tables(
	struct i386_page_table *table)
{
	struct i386_page_table *next;

	/* Releases every physical page and software record in list order. */
	while (table != NULL) {
		next = table->next;
		(void)hal_pmem_free(&table->memory);
		hal_free(table);
		table_count_drop();
		table = next;
	}
}

/* Tests one page-aligned interval against the user virtual range. */
static int
valid_user_range(
	uintptr_t vaddr,
	size_t size)
{
	/* Rejects empty or unaligned virtual intervals. */
	if (size == 0 || (vaddr & (PAGE_SIZE - 1U)) != 0 ||
	    (size & (PAGE_SIZE - 1U)) != 0) {
		return 0;
	}

	/* Rejects the null page and addresses at or above the kernel split. */
	if (vaddr < PAGE_SIZE || vaddr >= SYS_START)
		return 0;

	/* Rejects an interval which crosses the kernel split. */
	if (size > SYS_START - vaddr)
		return 0;

	/* Reports a valid user interval. */
	return 1;
}

/* Converts public mapping attributes to i386 PTE flags. */
static uint32_t
pte_flags(
	uint32_t attr)
{
	uint32_t flags;

	/* Starts every user mapping present and user-accessible. */
	flags = PTE_PRESENT | PTE_USER;

	/* Adds each optional hardware mapping attribute. */
	if ((attr & HAL_SPACE_WRITE) != 0U)
		flags |= PTE_WRITE;

	/* Adds cache-disable behavior when requested. */
	if ((attr & HAL_SPACE_NOCACHE) != 0U)
		flags |= PTE_NOCACHE;

	/* Adds write-through behavior when requested. */
	if ((attr & HAL_SPACE_WRITETHRU) != 0U)
		flags |= PTE_WRITEBACK;

	/* Returns the complete hardware flag word. */
	return flags;
}

/* Replaces one PTE while retaining concurrently observed A/D bits. */
static uint32_t
pte_replace_preserve_ad(
	uint32_t *entry,
	uint32_t replacement)
{
	uint32_t snapshot;
	uint32_t previous;
	uint32_t accessed_dirty;

	/* Includes currently observed hardware bits in the first replacement. */
	snapshot = *entry;
	previous = replacement | (snapshot & (PTE_ACCESS | PTE_DIRTY));

	/* Atomically publishes the replacement and captures the exact old PTE. */
	__asm__ volatile("xchgl %0, %1"
	    : "+r"(previous), "+m"(*entry)
	    :
	    : "memory");

	/* Carries hardware bits observed during exchange into the new PTE. */
	accessed_dirty = previous & (PTE_ACCESS | PTE_DIRTY);

	/* Republishes any accessed or dirty bit observed during exchange. */
	if (accessed_dirty != 0) {
		__asm__ volatile("lock; orl %1, %0"
		    : "+m"(*entry)
		    : "r"(accessed_dirty)
		    : "memory", "cc");
	}

	/* Returns the exact pre-publication PTE. */
	return previous;
}

/* Invalidates one address-space interval on all relevant CPUs. */
static void
shootdown(
	hal_space_t handle,
	void *vaddr,
	size_t size)
{
	struct i386_shootdown_request *request;
	struct hal_cpu_mask ready;
	hal_cpu_id_t sender;
	hal_cpu_id_t cpu;
	unsigned slot;
	unsigned pending;
	bool pool_enabled;
	int result;

	/* Captures the sender and initializes request-allocation state. */
	sender = hal_cpu_current();
	request = NULL;
	pending = 0;

	/* Reserves one fixed shootdown request slot under the pool lock. */
	pool_enabled = raw_lock_enter(&shootdown_pool_lock);
	for (slot = 0; slot < I386_SHOOTDOWN_REQUESTS; slot++) {
		/* Reserves the first inactive request slot. */
		if (shootdowns[slot].active == 0U) {
			request = &shootdowns[slot];
			request->active = 2U;
			break;
		}
	}
	raw_lock_leave(&shootdown_pool_lock, pool_enabled);

	/* Rejects exhaustion of the fixed shootdown request pool. */
	if (request == NULL)
		HAL_FATAL("i386 TLB shootdown request pool exhausted");

	/* Builds the ready remote-CPU mask which currently uses the space. */
	hal_cpu_ready_mask(&ready);
	for (cpu = 0; cpu < hal_cpu_count(); cpu++) {
		/* Skips the sender, excess IDs, and CPUs which are not ready. */
		if (cpu == sender || cpu >= I386_APIC_MAX_CPUS ||
		    !hal_cpu_mask_test(&ready, cpu)) {
			continue;
		}

		/* Selects CPUs using the shared or requested page directory. */
		if (handle == HAL_SPACE_SYS ||
		    __atomic_load_n(
		    &current_spaces[cpu],
		    __ATOMIC_ACQUIRE) == handle) {
			pending |= 1U << cpu;
		}
	}

	/* Publishes the complete request before marking it active. */
	request->space = handle;
	request->vaddr = vaddr;
	request->size = size;
	request->pending = pending;
	hal_wmb();
	request->active = 1U;
	hal_wmb();

	/* Sends a TLB IPI to every selected remote CPU. */
	for (cpu = 0; cpu < hal_cpu_count(); cpu++) {
		/* Skips CPUs absent from the published pending mask. */
		if ((pending & (1U << cpu)) == 0U)
			continue;

		/* Sends and validates this CPU's shootdown notification. */
		result = i386_smp_send_tlb(cpu);
		if (result != HAL_OK) {
			HAL_FATAL("i386 TLB shootdown delivery failed");
		}
	}

	/* Invalidates locally when the current CPU uses the target space. */
	if (handle == HAL_SPACE_SYS ||
	    __atomic_load_n(&current_space, __ATOMIC_ACQUIRE) == handle) {
		asm_flash_tlb();
	}

	/* Waits while servicing reciprocal requests to avoid deadlock. */
	while (request->pending != 0) {
		service_shootdowns(sender);
		__asm__ volatile("pause");
	}

	/* Retires the fully acknowledged request after a memory barrier. */
	hal_mb();
	request->active = 0U;
}

/* Services every pending shootdown request for one CPU. */
static void
service_shootdowns(
	hal_cpu_id_t cpu)
{
	struct i386_shootdown_request *request;
	unsigned bit;
	unsigned slot;
	bool enabled;

	/* Derives this CPU's bit in the fixed 32-bit pending mask. */
	bit = 1U << cpu;

	/* Examines every fixed request slot for work assigned to this CPU. */
	for (slot = 0; slot < I386_SHOOTDOWN_REQUESTS; slot++) {
		request = &shootdowns[slot];

		/* Skips slots which are not fully published and active. */
		if (request->active != 1U)
			continue;

		/* Orders request fields before serializing its pending mask. */
		hal_rmb();
		enabled = raw_lock_enter(&request->lock);

		/* Services this CPU only while the request remains active. */
		if (request->active == 1U &&
		    (request->pending & bit) != 0U) {
			asm_flash_tlb();
			request->pending &= ~bit;
		}
		raw_lock_leave(&request->lock, enabled);
	}
}
