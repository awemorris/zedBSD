/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The MC68040 address spaces.
 *
 * The supervisor reaches RAM and the device space through the transparent
 * translation registers, so the kernel has no tables of its own: the
 * tables hold the user half alone, and a space's root goes into both root
 * pointers, so that the kernel reaches the user half of the space it runs
 * in.  The kernel alone selects an empty root.
 *
 * Each table takes a page of its own (a root and a pointer table need 512
 * bytes, a page table 256), which keeps the bookkeeping simple.  The port
 * is uniprocessor, so a space's operation runs with interrupts off and
 * flushes the one translation cache.
 */

#include <hal/hal.h>
#include "asm.h"
#include "defs.h"
#include "page.h"
#include "space.h"
#include "spin.h"

/* The flags of a user page: resident, copyback-cached. */
#define USER_PAGE	(M68040_DESC_PAGE | M68040_DESC_CM_COPYBACK)

/* The transparent windows: RAM up to 1 GiB, copyback; the device space, serialized. */
#define RAM_WINDOW	(0x003f0000U | M68040_TT_ENABLE | M68040_TT_SUPERVISOR | M68040_TT_CM_COPYBACK)
#define DEVICE_WINDOW	(0xff000000U | M68040_TT_ENABLE | M68040_TT_SUPERVISOR | M68040_TT_CM_SERIALIZED)

/* The root the kernel alone selects: every entry invalid. */
static uint32_t system_root[M68040_ROOT_ENTRIES] __attribute__((aligned(M68040_ROOT_BYTES)));

/* The space selected now. */
static hal_space_t current_space;

/*
 * Every user space that exists, the number the next one gets, how many
 * there are and how many table pages they hold.  Kept with interrupts off.
 */
static struct m68040_space *space_registry;
static int next_space_id = 1;
static uint32_t space_count;
static uint32_t page_table_count;

static int space_lock_handle(hal_space_t handle, struct m68040_space **result, bool *irq_enabled);
static int valid_user(uintptr_t address, size_t size);
static int valid_attributes(uint32_t attr);
static struct m68040_table_page *allocate_table(struct m68040_space *space, uint32_t *parent, unsigned index);
static uint32_t *walk_leaf(struct m68040_space *space, uintptr_t address, int create);
static int table_empty(const uint32_t *table, unsigned entries);
static struct m68040_table_page *detach_empty_tables(struct m68040_space *space);
static void free_detached_tables(struct m68040_table_page *page);
static uint32_t user_leaf(hal_physaddr_t physical, uint32_t attr);
static void flush_space(hal_space_t handle);

/*
 * Turns translation on with the kernel alone selected.
 */
void
m68040_space_init(
	void)
{
	/* The empty root, then the windows and the table walk. */
	hal_memset(system_root, 0, sizeof(system_root));
	space_registry = NULL;
	current_space = HAL_SPACE_SYS;
	m68040_mmu_enable(RAM_WINDOW, DEVICE_WINDOW, (uint32_t)(uintptr_t)system_root);
	hal_puts("M68040 PAGING PASS\n");
}

/*
 * Creates an empty user space.
 */
hal_space_t
hal_space_create(
	void)
{
	struct m68040_space *space;
	bool enabled;
	int error;

	/* Allocates the record and the root. */
	space = kernel_alloc(sizeof(*space));
	if (space == NULL)
		return NULL;
	hal_memset(space, 0, sizeof(*space));
	error = hal_pmem_alloc(M68040_PAGE_SIZE, M68040_PAGE_SIZE, &space->root_memory);
	if (error != HAL_OK) {
		kernel_free(space);
		return NULL;
	}

	/* Clears the root through the kernel's view of it. */
	space->root = hal_pmem_to_kernel(space->root_memory);
	hal_memset(space->root, 0, M68040_ROOT_BYTES);
	space->magic = M68040_SPACE_MAGIC;

	/* Numbers the space and makes it live. */
	enabled = m68040_irq_save();
	space->space_id = next_space_id++;
	space->registry_next = space_registry;
	space_registry = space;
	space_count++;
	m68040_irq_restore(enabled);

	/* Succeeded: the space, with nothing mapped. */
	return space;
}

/*
 * Destroys a user space that is not selected.
 */
void
hal_space_destroy(
	hal_space_t handle)
{
	struct m68040_space *space;
	struct m68040_space **link;
	struct m68040_table_page *page;
	bool enabled;

	/* A missing space is nothing to destroy. */
	space = handle;
	if (space == NULL)
		return;

	/* Takes the space out of the registry. */
	enabled = m68040_irq_save();
	link = &space_registry;
	while (*link != NULL && *link != space)
		link = &(*link)->registry_next;
	if (*link == NULL || space->destroying)
		HAL_FATAL("invalid m68040 space destroy");
	if (current_space == handle)
		HAL_FATAL("destroying an active m68040 space");
	space->destroying = 1U;
	*link = space->registry_next;
	if (space_count != 0)
		space_count--;
	m68040_irq_restore(enabled);

	/* Frees every table, then the root and the record. */
	while (space->tables != NULL) {
		page = space->tables;
		space->tables = page->next;
		(void)hal_pmem_free(&page->memory, M68040_PAGE_SIZE);
		kernel_free(page);
		page_table_count--;
	}

	/* Frees the root and the record. */
	space->magic = 0;
	(void)hal_pmem_free(&space->root_memory, M68040_PAGE_SIZE);
	kernel_free(space);
}

/*
 * Selects a user space, or the kernel alone.
 */
void
hal_space_switch(
	hal_space_t handle)
{
	struct m68040_space *space;
	bool enabled;
	int locked;

	/* The space already selected needs nothing. */
	if (handle == current_space)
		return;

	/* The kernel alone: the empty root. */
	if (handle == HAL_SPACE_SYS) {
		enabled = m68040_irq_save();
		m68040_load_root((uint32_t)(uintptr_t)system_root);
		current_space = handle;
		m68040_irq_restore(enabled);
		return;
	}

	/* A user space: its root in both pointers. */
	locked = space_lock_handle(handle, &space, &enabled);
	if (!locked)
		HAL_FATAL("invalid m68040 space switch");
	m68040_load_root((uint32_t)space->root_memory);
	current_space = handle;
	m68040_irq_restore(enabled);
}

/*
 * Maps managed RAM into a user space.
 */
int
hal_space_map(
	hal_space_t handle,
	void *vaddr,
	hal_physaddr_t paddr,
	size_t size,
	uint32_t attr)
{
	struct m68040_space *space;
	struct m68040_table_page *detached;
	uint32_t *leaf;
	uintptr_t address;
	uintptr_t offset;
	uintptr_t rollback;
	bool enabled;
	int valid;
	int locked;
	bool ram;

	/* Requires a user range, a page-aligned range of RAM and attributes the tables can hold. */
	address = (uintptr_t)vaddr;
	if (handle == HAL_SPACE_SYS)
		return HAL_ERR_INVALID;
	valid = valid_user(address, size);
	if (!valid)
		return HAL_ERR_INVALID;
	if ((paddr & (M68040_PAGE_SIZE - 1U)) != 0)
		return HAL_ERR_INVALID;
	ram = m68040_range_is_ram(paddr, size);
	if (!ram)
		return HAL_ERR_INVALID;
	valid = valid_attributes(attr);
	if (valid != HAL_OK)
		return valid;

	/* Holds the space for the whole change. */
	locked = space_lock_handle(handle, &space, &enabled);
	if (!locked)
		return HAL_ERR_STATE;

	/* Refuses a range of which any page is mapped already. */
	for (offset = 0; offset < size; offset += M68040_PAGE_SIZE) {
		leaf = walk_leaf(space, address + offset, 0);
		if (leaf != NULL && (*leaf & M68040_DESC_TYPE_MASK) != 0) {
			m68040_irq_restore(enabled);
			return HAL_ERR_INVALID;
		}
	}

	/* Writes a leaf for each page, unwinding them all if a table cannot be made. */
	for (offset = 0; offset < size; offset += M68040_PAGE_SIZE) {
		leaf = walk_leaf(space, address + offset, 1);
		if (leaf == NULL) {
			for (rollback = 0; rollback < offset; rollback += M68040_PAGE_SIZE) {
				leaf = walk_leaf(space, address + rollback, 0);
				if (leaf != NULL)
					*leaf = 0;
			}

			/* Unlinks the tables left empty, then drops the stale translations and frees them. */
			detached = detach_empty_tables(space);
			flush_space(handle);
			free_detached_tables(detached);
			m68040_irq_restore(enabled);
			return HAL_ERR_NOMEM;
		}

		/* Writes the page's leaf. */
		*leaf = user_leaf(paddr + offset, attr);
	}

	/* Code in the pages is fetched from memory, not from stale cache lines. */
	if ((attr & HAL_SPACE_EXEC) != 0)
		m68040_cache_push_all();
	flush_space(handle);
	m68040_irq_restore(enabled);

	/* Succeeded: the range is mapped. */
	return HAL_OK;
}

/*
 * Removes a user range's mappings.
 */
int
hal_space_unmap(
	hal_space_t handle,
	void *vaddr,
	size_t size)
{
	struct m68040_space *space;
	struct m68040_table_page *detached;
	uint32_t *leaf;
	uintptr_t address;
	uintptr_t offset;
	bool enabled;
	int locked;
	int valid;

	/* An empty range is nothing to remove. */
	if (size == 0)
		return HAL_OK;
	address = (uintptr_t)vaddr;
	if (handle == HAL_SPACE_SYS)
		return HAL_ERR_INVALID;
	valid = valid_user(address, size);
	if (!valid)
		return HAL_ERR_INVALID;
	locked = space_lock_handle(handle, &space, &enabled);
	if (!locked)
		return HAL_ERR_STATE;

	/* Clears each leaf, then unlinks the tables that became empty. */
	for (offset = 0; offset < size; offset += M68040_PAGE_SIZE) {
		leaf = walk_leaf(space, address + offset, 0);
		if (leaf != NULL)
			*leaf = 0;
	}

	/* Unlinks the tables left empty, then drops the stale translations and frees them. */
	detached = detach_empty_tables(space);
	flush_space(handle);
	free_detached_tables(detached);
	m68040_irq_restore(enabled);

	/* Succeeded: the range is unmapped. */
	return HAL_OK;
}

/*
 * Changes a mapped user range's protection and reports what the old
 * translations recorded: the processor sets the used and modified bits of
 * the page descriptors itself.
 */
int
hal_space_prot_query(
	hal_space_t handle,
	void *vaddr,
	size_t size,
	uint32_t attr,
	uint32_t *flags)
{
	struct m68040_space *space;
	uint32_t *leaf;
	uint32_t old;
	uint32_t observed;
	uintptr_t address;
	uintptr_t offset;
	bool enabled;
	int valid;
	int locked;

	/* Requires a user range and attributes the tables can hold. */
	address = (uintptr_t)vaddr;
	if (handle == HAL_SPACE_SYS)
		return HAL_ERR_INVALID;
	valid = valid_user(address, size);
	if (!valid)
		return HAL_ERR_INVALID;
	valid = valid_attributes(attr);
	if (valid != HAL_OK)
		return valid;
	locked = space_lock_handle(handle, &space, &enabled);
	if (!locked)
		return HAL_ERR_STATE;

	/* Refuses a range with any page unmapped. */
	for (offset = 0; offset < size; offset += M68040_PAGE_SIZE) {
		leaf = walk_leaf(space, address + offset, 0);
		if (leaf == NULL || (*leaf & M68040_DESC_TYPE_MASK) == 0) {
			m68040_irq_restore(enabled);
			return HAL_ERR_INVALID;
		}
	}

	/* Rewrites each leaf, keeping its page and its used and modified bits. */
	observed = 0;
	for (offset = 0; offset < size; offset += M68040_PAGE_SIZE) {
		leaf = walk_leaf(space, address + offset, 0);
		old = *leaf;
		observed |= HAL_SPACE_PAGE_PRESENT;
		if ((old & M68040_DESC_USED) != 0)
			observed |= HAL_SPACE_PAGE_ACCESSED;
		if ((old & M68040_DESC_MODIFIED) != 0)
			observed |= HAL_SPACE_PAGE_DIRTY;
		*leaf = user_leaf(old & M68040_PAGE_ADDRESS_MASK, attr) | (old & (M68040_DESC_USED | M68040_DESC_MODIFIED));
	}

	/* Code in the pages is fetched from memory, not from stale cache lines. */
	if ((attr & HAL_SPACE_EXEC) != 0)
		m68040_cache_push_all();
	flush_space(handle);
	m68040_irq_restore(enabled);

	/* Succeeded: reports what the old translations recorded, when asked. */
	if (flags != NULL)
		*flags = observed;
	return HAL_OK;
}

/*
 * Changes a mapped user range's protection.
 */
int
hal_space_prot(
	hal_space_t handle,
	void *vaddr,
	size_t size,
	uint32_t attr)
{
	int error;

	/* The same change without the report. */
	error = hal_space_prot_query(handle, vaddr, size, attr, NULL);
	if (error != HAL_OK)
		return error;

	/* Succeeded: the range has the new protection. */
	return HAL_OK;
}

/*
 * Reports the state of one user page: present, accessed and dirty.
 */
int
hal_space_query(
	hal_space_t handle,
	void *vaddr,
	uint32_t *flags)
{
	struct m68040_space *space;
	uint32_t *leaf;
	uint32_t observed;
	bool enabled;
	int locked;
	int valid;

	/* Requires a user page and somewhere to report. */
	if (handle == HAL_SPACE_SYS || flags == NULL)
		return HAL_ERR_INVALID;
	valid = valid_user((uintptr_t)vaddr, M68040_PAGE_SIZE);
	if (!valid)
		return HAL_ERR_INVALID;
	locked = space_lock_handle(handle, &space, &enabled);
	if (!locked)
		return HAL_ERR_STATE;

	/* Reads the page's leaf. */
	observed = 0;
	leaf = walk_leaf(space, (uintptr_t)vaddr, 0);
	if (leaf != NULL && (*leaf & M68040_DESC_TYPE_MASK) != 0) {
		observed = HAL_SPACE_PAGE_PRESENT;
		if ((*leaf & M68040_DESC_USED) != 0)
			observed |= HAL_SPACE_PAGE_ACCESSED;
		if ((*leaf & M68040_DESC_MODIFIED) != 0)
			observed |= HAL_SPACE_PAGE_DIRTY;
	}

	/* Opens the interrupt mask again as the caller had it. */
	m68040_irq_restore(enabled);

	/* Succeeded: the page's state. */
	*flags = observed;
	return HAL_OK;
}

/*
 * Clears the recorded state of one user page.
 */
int
hal_space_clear_flags(
	hal_space_t handle,
	void *vaddr,
	uint32_t flags)
{
	struct m68040_space *space;
	uint32_t *leaf;
	uint32_t mask;
	bool enabled;
	int locked;
	int valid;

	/* Requires a user page and flags the tables record. */
	if (handle == HAL_SPACE_SYS)
		return HAL_ERR_INVALID;
	valid = valid_user((uintptr_t)vaddr, M68040_PAGE_SIZE);
	if (!valid)
		return HAL_ERR_INVALID;
	if ((flags & ~(uint32_t)(HAL_SPACE_PAGE_ACCESSED | HAL_SPACE_PAGE_DIRTY)) != 0)
		return HAL_ERR_INVALID;
	locked = space_lock_handle(handle, &space, &enabled);
	if (!locked)
		return HAL_ERR_STATE;

	/* Refuses a page that is not mapped. */
	leaf = walk_leaf(space, (uintptr_t)vaddr, 0);
	if (leaf == NULL || (*leaf & M68040_DESC_TYPE_MASK) == 0) {
		m68040_irq_restore(enabled);
		return HAL_ERR_INVALID;
	}

	/* Clears the bits, then the cached translation that would not set them again. */
	mask = 0;
	if ((flags & HAL_SPACE_PAGE_ACCESSED) != 0)
		mask |= M68040_DESC_USED;
	if ((flags & HAL_SPACE_PAGE_DIRTY) != 0)
		mask |= M68040_DESC_MODIFIED;
	*leaf &= ~mask;
	flush_space(handle);
	m68040_irq_restore(enabled);

	/* Succeeded: the page's state is cleared. */
	return HAL_OK;
}

/*
 * Drops the stale translations of a space.
 */
void
hal_space_flush_tlb(
	hal_space_t handle)
{
	bool enabled;

	/* One cache on one processor. */
	enabled = m68040_irq_save();
	flush_space(handle);
	m68040_irq_restore(enabled);
}

/*
 * Drops the stale translations of a range; the whole cache is.
 */
void
hal_space_flush_tlb_range(
	hal_space_t handle,
	void *vaddr,
	size_t size)
{
	UNUSED_PARAMETER(vaddr);

	/* An empty range has nothing stale. */
	if (size != 0)
		hal_space_flush_tlb(handle);
}

/*
 * Reports the size a table level maps: only pages.
 */
size_t
hal_space_get_page_size(
	int level)
{
	/* The tables have no larger leaf. */
	if (level == 1)
		return M68040_PAGE_SIZE;
	return 0;
}

/*
 * Reports the user half's bounds.
 */
void
hal_space_get_user_range(
	uintptr_t *minimum,
	uintptr_t *limit)
{
	/* Between the supervisor's RAM window and the sign bit. */
	if (minimum != NULL)
		*minimum = M68040_USER_MINIMUM;
	if (limit != NULL)
		*limit = M68040_USER_LIMIT;
}

/*
 * Reports where the kernel reaches a device range: the device space and
 * RAM are reached at their own addresses.
 */
int
hal_space_map_device(
	hal_physaddr_t paddr,
	size_t size,
	uint32_t attr,
	void **vaddr)
{
	bool ram;

	/* Requires a destination and a non-empty range. */
	if (vaddr == NULL || size == 0)
		return HAL_ERR_INVALID;
	if ((attr & HAL_SPACE_EXEC) != 0)
		return HAL_ERR_INVALID;

	/* The device space, serialized and never cached. */
	if (paddr >= M68040_DEVICE_BASE && size <= (size_t)(0U - (uint32_t)paddr)) {
		*vaddr = (void *)(uintptr_t)paddr;
		return HAL_OK;
	}

	/* RAM, as it is. */
	ram = m68040_range_is_ram(paddr, size);
	if (ram) {
		*vaddr = (void *)(uintptr_t)paddr;
		return HAL_OK;
	}

	/* Anything else is out of the supervisor's reach. */
	return HAL_ERR_INVALID;
}

/*
 * Releases a device range; nothing was mapped for it.
 */
int
hal_space_unmap_device(
	void *vaddr,
	size_t size)
{
	UNUSED_PARAMETER(vaddr);

	/* An empty range was never handed out. */
	if (size == 0)
		return HAL_ERR_INVALID;
	return HAL_OK;
}

/*
 * Makes code written through the data side visible to instruction fetch.
 */
void
hal_sync_instruction_stream(
	void *addr,
	size_t size)
{
	UNUSED_PARAMETER(addr);

	/* Pushes the data cache and empties the instruction cache. */
	if (size != 0)
		m68040_cache_push_all();
}

/*
 * Makes a range of instructions current; the whole cache is.
 */
void
hal_icache_invalidate_range(
	uintptr_t addr,
	size_t size)
{
	/* The same synchronization. */
	hal_sync_instruction_stream((void *)addr, size);
}

/*
 * Reports how many user spaces and table pages exist.
 */
void
m68040_space_memory_stats(
	uint32_t *spaces,
	uint32_t *tables)
{
	bool enabled;

	/* Samples both counts together. */
	enabled = m68040_irq_save();
	if (spaces != NULL)
		*spaces = space_count;
	if (tables != NULL)
		*tables = page_table_count;
	m68040_irq_restore(enabled);
}

/*
 * Finds a live user space by its handle and closes the interrupt mask for
 * its operation.  Reports 0 for a handle that names no live space.
 */
static int
space_lock_handle(
	hal_space_t handle,
	struct m68040_space **result,
	bool *irq_enabled)
{
	struct m68040_space *space;
	bool enabled;

	/* Looks the handle up among the live spaces. */
	enabled = m68040_irq_save();
	for (space = space_registry; space != NULL; space = space->registry_next) {
		if ((hal_space_t)space == handle)
			break;
	}

	/* A handle that names no live space is refused. */
	if (space == NULL || space->destroying) {
		m68040_irq_restore(enabled);
		return 0;
	}

	/* Succeeded: the caller holds the space until it opens the mask. */
	*result = space;
	*irq_enabled = enabled;
	return 1;
}

/* Reports whether a range is page-aligned, non-empty and inside the user half. */
static int
valid_user(
	uintptr_t address,
	size_t size)
{
	/* An empty or unaligned range is refused. */
	if (size == 0)
		return 0;
	if ((address & (M68040_PAGE_SIZE - 1U)) != 0 || (size & (M68040_PAGE_SIZE - 1U)) != 0)
		return 0;

	/* The range must lie inside the user half. */
	if (address < M68040_USER_MINIMUM || address >= M68040_USER_LIMIT)
		return 0;
	if (size > M68040_USER_LIMIT - address)
		return 0;

	/* Succeeded: the range can be mapped. */
	return 1;
}

/*
 * Checks a user mapping's attributes: some access, never written and
 * executed at once, and no cache behaviour or device class.
 */
static int
valid_attributes(
	uint32_t attr)
{
	/* A mapping must allow some access, and not both writing and executing. */
	if ((attr & (HAL_SPACE_READ | HAL_SPACE_WRITE | HAL_SPACE_EXEC)) == 0)
		return HAL_ERR_INVALID;
	if ((attr & HAL_SPACE_WRITE) != 0 && (attr & HAL_SPACE_EXEC) != 0)
		return HAL_ERR_INVALID;

	/* Device pages and other cache behaviours are not offered to users. */
	if ((attr & (HAL_SPACE_NOCACHE | HAL_SPACE_WRITETHRU | HAL_SPACE_WC | HAL_SPACE_DEVICE)) != 0)
		return HAL_ERR_UNSUPPORTED;

	/* Succeeded: the attributes can be represented. */
	return HAL_OK;
}

/* Allocates a zeroed table page and records it as the space's. */
static struct m68040_table_page *
allocate_table(
	struct m68040_space *space,
	uint32_t *parent,
	unsigned index)
{
	struct m68040_table_page *page;
	int error;

	/* The record, then the page. */
	page = kernel_alloc(sizeof(*page));
	if (page == NULL)
		return NULL;
	error = hal_pmem_alloc(M68040_PAGE_SIZE, M68040_PAGE_SIZE, &page->memory);
	if (error != HAL_OK) {
		kernel_free(page);
		return NULL;
	}

	/* Clears it and remembers the entry that points at it. */
	hal_memset(hal_pmem_to_kernel(page->memory), 0, M68040_PAGE_SIZE);
	page->parent = parent;
	page->parent_index = index;
	page->next = space->tables;
	space->tables = page;
	page_table_count++;

	/* Succeeded: the table, linked by the caller. */
	return page;
}

/*
 * Finds the page descriptor of a user address, making the tables on the
 * way when asked to.  Reports NULL when a table is missing and not to be
 * made, or cannot be made.
 */
static uint32_t *
walk_leaf(
	struct m68040_space *space,
	uintptr_t address,
	int create)
{
	struct m68040_table_page *page;
	uint32_t *table;
	uint32_t entry;
	unsigned index;

	/* The root entry, pointing at a pointer table. */
	table = space->root;
	index = (unsigned)(address >> M68040_ROOT_SHIFT) & (M68040_ROOT_ENTRIES - 1U);
	entry = table[index];
	if ((entry & M68040_DESC_TABLE) == 0) {
		if (!create)
			return NULL;
		page = allocate_table(space, table, index);
		if (page == NULL)
			return NULL;
		entry = (uint32_t)page->memory | M68040_DESC_TABLE;
		table[index] = entry;
	}

	/* The pointer entry, pointing at a page table. */
	table = hal_pmem_to_kernel(entry & M68040_ROOT_ADDRESS_MASK);
	index = (unsigned)(address >> M68040_POINTER_SHIFT) & (M68040_POINTER_ENTRIES - 1U);
	entry = table[index];
	if ((entry & M68040_DESC_TABLE) == 0) {
		if (!create)
			return NULL;
		page = allocate_table(space, table, index);
		if (page == NULL)
			return NULL;
		entry = (uint32_t)page->memory | M68040_DESC_TABLE;
		table[index] = entry;
	}

	/* Succeeded: the page's descriptor. */
	table = hal_pmem_to_kernel(entry & M68040_PAGE_TABLE_ADDRESS_MASK);
	return &table[(address >> M68040_PAGE_SHIFT) & (M68040_PAGE_ENTRIES - 1U)];
}

/* Reports whether a table has no resident entry. */
static int
table_empty(
	const uint32_t *table,
	unsigned entries)
{
	unsigned index;

	/* Looks for any resident entry. */
	for (index = 0; index < entries; index++) {
		if ((table[index] & M68040_DESC_TYPE_MASK) != 0)
			return 0;
	}

	/* The table is empty. */
	return 1;
}

/*
 * Unlinks every empty table of a space, repeating until a pass unlinks
 * none, and reports them for freeing after the flush.
 */
static struct m68040_table_page *
detach_empty_tables(
	struct m68040_space *space)
{
	struct m68040_table_page *detached;
	struct m68040_table_page **link;
	struct m68040_table_page *page;
	int again;
	int empty;

	/* Passes over the tables until nothing more is detached. */
	detached = NULL;
	do {
		again = 0;
		link = &space->tables;
		while (*link != NULL) {
			page = *link;

			/* A table that still maps something stays; 128 entries cover both kinds. */
			empty = table_empty(hal_pmem_to_kernel(page->memory), M68040_POINTER_ENTRIES);
			if (!empty) {
				link = &page->next;
				continue;
			}

			/* Unlinks it from its parent and from the space's list. */
			page->parent[page->parent_index] = 0;
			*link = page->next;
			page->next = detached;
			detached = page;
			again = 1;
		}
	} while (again);

	/* Succeeded: the tables to free. */
	return detached;
}

/* Frees tables detach_empty_tables() unlinked. */
static void
free_detached_tables(
	struct m68040_table_page *page)
{
	struct m68040_table_page *next;

	/* Each table and its record. */
	while (page != NULL) {
		next = page->next;
		(void)hal_pmem_free(&page->memory, M68040_PAGE_SIZE);
		kernel_free(page);
		page_table_count--;
		page = next;
	}
}

/* Builds a user page descriptor: read-only unless the page is writable. */
static uint32_t
user_leaf(
	hal_physaddr_t physical,
	uint32_t attr)
{
	uint32_t descriptor;

	/* The page, resident and cached. */
	descriptor = ((uint32_t)physical & M68040_PAGE_ADDRESS_MASK) | USER_PAGE;
	if ((attr & HAL_SPACE_WRITE) == 0)
		descriptor |= M68040_DESC_WRITE_PROTECT;

	/* Succeeded: the descriptor. */
	return descriptor;
}

/* Drops the cached translations when the space is the one selected. */
static void
flush_space(
	hal_space_t handle)
{
	/* A space not selected has nothing cached: selecting it flushes. */
	if (handle == current_space || handle == HAL_SPACE_SYS)
		m68040_flush_atc();
}
