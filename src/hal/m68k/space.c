/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The MC68030 HAL's user address spaces: a two-level table of short
 * descriptors per space, 1024 root entries of 4 MiB and 1024 page entries
 * of 4 KiB, under the CPU root pointer.  The kernel half is reached through
 * the supervisor root pointer the bootstrap set, which stays.
 *
 * The port runs on one processor; interrupts are off while a space is
 * changed, and the address translation cache is flushed whenever a table
 * of the selected space changes.
 */

#include <hal/hal.h>
#include "mmu030.h"
#include "space.h"

/* The bits of an address within one root entry's 4 MiB. */
#define SPACE_ROOT_SPAN_MASK	0x003fffffU

_Static_assert((uintptr_t)(M68K030_USER_LIMIT - 1U) <= (uintptr_t)INTPTR_MAX,
	       "user pointers must not overlap the negative syscall errno window");

/*
 * The space the processor runs in, or HAL_SPACE_SYS when only the kernel
 * half is selected.  Changed by hal_space_switch() with interrupts off.
 */
static hal_space_t current_space;

/*
 * The CPU root pointer of the kernel half alone: the bootstrap's empty user
 * root.  Set once by m68k030_space_init().
 */
static struct m68k030_root_pointer system_crp;

/*
 * The identity the next space takes, the number of spaces and of page
 * tables, for the memory statistics.  Changed with interrupts off.
 */
static uint32_t next_space_id = 1;
static uint32_t space_count;
static uint32_t page_table_count;

/*
 * Every live space, newest first.  A handle is accepted only when it is on
 * this list, so a destroyed space is refused rather than touched.
 */
static struct m68k030_space *space_registry;

static int alloc_page(hal_physaddr_t *physical, uint32_t **entries);
static void free_page(hal_physaddr_t physical);
static int space_lock_handle(hal_space_t handle, struct m68k030_space **result, bool *irq_enabled);
static void space_unlock(struct m68k030_space *space, bool enabled);
static void flush_locked(hal_space_t handle);
static int valid_user_range(uintptr_t address, size_t size);
static int attributes_are_valid(uint32_t attributes);
static struct m68k030_table_page *find_table(struct m68k030_space *space, uintptr_t address);
static struct m68k030_table_page *create_table(struct m68k030_space *space, uintptr_t address);
static int table_empty(const struct m68k030_table_page *table);
static struct m68k030_table_page *detach_empty_tables(struct m68k030_space *space);
static void free_detached_tables(struct m68k030_table_page *table);
static int page_is_mapped(struct m68k030_space *space, uintptr_t address);
static void clear_page(struct m68k030_space *space, uintptr_t address);
static uint32_t page_attributes(uint32_t attributes);
static uint32_t descriptor_flags(uint32_t descriptor);

/*
 * Starts the MMU's user half empty.
 */
void
m68k030_space_init(
	uintptr_t empty_root_physical)
{
	int aligned;
	unsigned cpus;

	/* The root must be a whole page, and the port runs on one processor. */
	aligned = m68k030_page_aligned(empty_root_physical);
	if (!aligned)
		HAL_FATAL("unaligned empty MC68030 CRP root");
	cpus = hal_cpu_count();
	if (cpus != 1)
		HAL_FATAL("MC68030 space implementation is UP-only");

	/* Keeps the empty root as the kernel half's CPU root pointer. */
	system_crp.attr = M68K030_ROOT_ATTR;
	system_crp.address = (uint32_t)empty_root_physical;

	/* Starts with no space. */
	current_space = HAL_SPACE_SYS;
	space_registry = NULL;
	space_count = 0;
	page_table_count = 0;

	/* Selects the empty user half and drops what the bootstrap translated. */
	m68k030_load_crp(&system_crp);
	m68k030_flush_atc();
}

/*
 * Creates a user space with no mapping.
 */
hal_space_t
hal_space_create(
	void)
{
	struct m68k030_space *space;
	bool enabled;
	int error;

	/* Allocates the space's record. */
	space = kernel_alloc(sizeof(*space));
	if (space == NULL)
		return NULL;
	hal_memset(space, 0, sizeof(*space));

	/* Allocates its root table, empty. */
	error = alloc_page(&space->root_physical, &space->root);
	if (error != HAL_OK) {
		kernel_free(space);
		return NULL;
	}

	/* Points the space's CPU root pointer at it. */
	space->root_pointer.attr = M68K030_ROOT_ATTR;
	space->root_pointer.address = (uint32_t)(uintptr_t)space->root_physical;
	space->magic = M68K030_SPACE_MAGIC;

	/* Gives the space an identity and publishes it. */
	enabled = hal_irq_disable();

	space->space_id = next_space_id;
	next_space_id++;
	space->registry_next = space_registry;
	space_registry = space;
	space_count++;

	if (enabled)
		hal_irq_enable();

	/* Succeeded: the new space. */
	return space;
}

/*
 * Destroys a user space that no processor selects any more.
 */
void
hal_space_destroy(
	hal_space_t handle)
{
	struct m68k030_space *space;
	struct m68k030_space **link;
	struct m68k030_table_page *table;
	bool enabled;

	/* Destroying nothing does nothing. */
	space = handle;
	if (space == NULL)
		return;

	/* Takes the space off the registry, refusing one that is not on it or in use. */
	enabled = hal_irq_disable();

	link = &space_registry;
	while (*link != NULL && *link != space)
		link = &(*link)->registry_next;
	if (*link == NULL || space->destroying)
		HAL_FATAL("invalid MC68030 space destroy");
	if (current_space == space)
		HAL_FATAL("destroying an active MC68030 space");
	if (space->lock != 0)
		HAL_FATAL("destroying a busy MC68030 space");
	space->destroying = 1U;
	*link = space->registry_next;

	/* Frees every page table, the root and the record. */
	while (space->tables != NULL) {
		table = space->tables;
		space->tables = table->next;
		free_page(table->physical);
		kernel_free(table);
		if (page_table_count != 0)
			page_table_count--;
	}
	space->magic = 0;
	free_page(space->root_physical);
	kernel_free(space);
	if (space_count != 0)
		space_count--;

	if (enabled)
		hal_irq_enable();
}

/*
 * Selects a user space on the processor, or only the kernel half.
 */
void
hal_space_switch(
	hal_space_t handle)
{
	struct m68k030_space *space;
	bool enabled;
	int acquired;

	/* Selecting the space already selected changes nothing. */
	if (handle == current_space)
		return;

	/* Selects only the kernel half. */
	if (handle == HAL_SPACE_SYS) {
		enabled = hal_irq_disable();

		m68k030_load_crp(&system_crp);
		m68k030_flush_atc();
		current_space = handle;

		if (enabled)
			hal_irq_enable();
		return;
	}

	/* Refuses a handle that is not a live space. */
	acquired = space_lock_handle(handle, &space, &enabled);
	if (!acquired)
		HAL_FATAL("invalid MC68030 space switch");

	/* Loads the space's root and drops the old translations. */
	hal_compiler_barrier();
	m68k030_load_crp(&space->root_pointer);
	m68k030_flush_atc();
	current_space = handle;

	space_unlock(space, enabled);
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
	struct m68k030_space *space;
	struct m68k030_table_page *table;
	struct m68k030_table_page *detached;
	uintptr_t address;
	uintptr_t offset;
	uintptr_t rollback;
	size_t total;
	bool enabled;
	int user_range;
	int aligned;
	int valid;
	int mapped;
	int acquired;

	/* Refuses a range that is not whole user pages. */
	address = (uintptr_t)vaddr;
	user_range = valid_user_range(address, size);
	if (handle == NULL || !user_range)
		return HAL_ERR_INVALID;

	/* Refuses a physical range that is not whole pages of managed RAM. */
	total = hal_pmem_get_total_size();
	aligned = m68k030_page_aligned(paddr);
	if (!aligned)
		return HAL_ERR_INVALID;
	if (paddr >= total || size > total - paddr)
		return HAL_ERR_INVALID;

	/* Refuses attributes that grant nothing, or writable code. */
	valid = attributes_are_valid(attr);
	if (!valid)
		return HAL_ERR_INVALID;

	/* Refuses a stale handle. */
	acquired = space_lock_handle(handle, &space, &enabled);
	if (!acquired)
		return HAL_ERR_STATE;

	/* Refuses a range that overlaps a page already mapped. */
	for (offset = 0; offset < size; offset += M68K030_PAGE_SIZE) {
		mapped = page_is_mapped(space, address + offset);
		if (mapped) {
			space_unlock(space, enabled);
			return HAL_ERR_INVALID;
		}
	}

	/* Writes each page's descriptor, creating the tables it needs. */
	for (offset = 0; offset < size; offset += M68K030_PAGE_SIZE) {
		/* Finds or creates the page's table. */
		table = find_table(space, address + offset);
		if (table == NULL)
			table = create_table(space, address + offset);

		/* Without a table, takes back the pages written so far and the tables left empty. */
		if (table == NULL) {
			for (rollback = 0; rollback < offset; rollback += M68K030_PAGE_SIZE)
				clear_page(space, address + rollback);
			detached = detach_empty_tables(space);
			flush_locked(space);
			free_detached_tables(detached);
			space_unlock(space, enabled);
			return HAL_ERR_NOMEM;
		}

		table->entries[m68k030_leaf_index(address + offset)] = m68k030_page_descriptor(paddr + offset, page_attributes(attr));
	}

	/* Drops the old translations, so the next access reads the new descriptors. */
	hal_compiler_barrier();
	flush_locked(space);

	space_unlock(space, enabled);

	/* Succeeded: the pages are reachable. */
	return HAL_OK;
}

/*
 * Changes the protection of a user range.
 */
int
hal_space_prot(
	hal_space_t handle,
	void *vaddr,
	size_t size,
	uint32_t attr)
{
	int error;

	/* Changes the protection without asking for the pages' state. */
	error = hal_space_prot_query(handle, vaddr, size, attr, NULL);
	if (error != HAL_OK)
		return error;

	/* Succeeded: the new protection is in effect. */
	return HAL_OK;
}

/*
 * Changes the protection of a user range and reports the access and dirty
 * state its pages had.
 */
int
hal_space_prot_query(
	hal_space_t handle,
	void *vaddr,
	size_t size,
	uint32_t attr,
	uint32_t *flags)
{
	struct m68k030_space *space;
	struct m68k030_table_page *table;
	uintptr_t address;
	uintptr_t offset;
	uint32_t observed;
	uint32_t old;
	unsigned index;
	bool enabled;
	int user_range;
	int valid;
	int mapped;
	int acquired;

	/* Refuses a range that is not whole user pages, or invalid attributes. */
	address = (uintptr_t)vaddr;
	user_range = valid_user_range(address, size);
	if (handle == NULL || !user_range)
		return HAL_ERR_INVALID;
	valid = attributes_are_valid(attr);
	if (!valid)
		return HAL_ERR_INVALID;

	/* Refuses a stale handle. */
	acquired = space_lock_handle(handle, &space, &enabled);
	if (!acquired)
		return HAL_ERR_STATE;

	/* Refuses a range with a page that is not mapped, before changing any. */
	for (offset = 0; offset < size; offset += M68K030_PAGE_SIZE) {
		mapped = page_is_mapped(space, address + offset);
		if (!mapped) {
			space_unlock(space, enabled);
			return HAL_ERR_INVALID;
		}
	}

	/* Gives every page the new protection, keeping its used and modified bits. */
	observed = 0;
	for (offset = 0; offset < size; offset += M68K030_PAGE_SIZE) {
		table = find_table(space, address + offset);
		index = m68k030_leaf_index(address + offset);
		old = table->entries[index];
		observed |= descriptor_flags(old);
		table->entries[index] = m68k030_page_descriptor(m68k030_page_address(old),
								page_attributes(attr) | (old & (M68K030_DESC_USED | M68K030_PAGE_MODIFIED)));
	}

	/* Drops the old translations. */
	hal_compiler_barrier();
	flush_locked(space);

	/*
	 * The MC68030 writes the used and modified bits to memory; once the
	 * translations are dropped, the descriptors hold all the old ones left.
	 */
	for (offset = 0; offset < size; offset += M68K030_PAGE_SIZE) {
		table = find_table(space, address + offset);
		observed |= descriptor_flags(table->entries[m68k030_leaf_index(address + offset)]);
	}

	/* Reports the state the old translations left. */
	if (flags != NULL)
		*flags = observed;

	space_unlock(space, enabled);

	/* Succeeded. */
	return HAL_OK;
}

/*
 * Removes the mappings of a user range.
 */
int
hal_space_unmap(
	hal_space_t handle,
	void *vaddr,
	size_t size)
{
	struct m68k030_space *space;
	struct m68k030_table_page *detached;
	uintptr_t address;
	uintptr_t offset;
	bool enabled;
	int user_range;
	int acquired;

	/* Unmapping nothing succeeds. */
	if (size == 0)
		return HAL_OK;

	/* Refuses a range that is not whole user pages. */
	address = (uintptr_t)vaddr;
	user_range = valid_user_range(address, size);
	if (handle == NULL || !user_range)
		return HAL_ERR_INVALID;

	/* Refuses a stale handle. */
	acquired = space_lock_handle(handle, &space, &enabled);
	if (!acquired)
		return HAL_ERR_STATE;

	/* Clears every page of the range and drops the tables left empty. */
	for (offset = 0; offset < size; offset += M68K030_PAGE_SIZE)
		clear_page(space, address + offset);
	detached = detach_empty_tables(space);
	flush_locked(space);
	free_detached_tables(detached);

	space_unlock(space, enabled);

	/* Succeeded: the range is no longer reachable. */
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
	struct m68k030_space *space;
	struct m68k030_table_page *table;
	uintptr_t address;
	uint32_t descriptor;
	bool enabled;
	int user_range;
	int acquired;

	/* Refuses a missing destination or an address that is not a user page. */
	address = (uintptr_t)vaddr;
	user_range = valid_user_range(address, M68K030_PAGE_SIZE);
	if (handle == NULL || flags == NULL || !user_range)
		return HAL_ERR_INVALID;

	/* Refuses a stale handle. */
	acquired = space_lock_handle(handle, &space, &enabled);
	if (!acquired)
		return HAL_ERR_STATE;

	/* Reports the page's state; an unmapped page has none. */
	descriptor = M68K030_DT_INVALID;
	table = find_table(space, address);
	if (table != NULL)
		descriptor = table->entries[m68k030_leaf_index(address)];
	*flags = descriptor_flags(descriptor);

	space_unlock(space, enabled);

	/* Succeeded. */
	return HAL_OK;
}

/*
 * Clears the accessed or dirty state of one user page.
 */
int
hal_space_clear_flags(
	hal_space_t handle,
	void *vaddr,
	uint32_t flags)
{
	struct m68k030_space *space;
	struct m68k030_table_page *table;
	uintptr_t address;
	uint32_t mask;
	bool enabled;
	int user_range;
	int mapped;
	int acquired;

	/* Refuses an address that is not a user page, or a state that cannot be cleared. */
	address = (uintptr_t)vaddr;
	user_range = valid_user_range(address, M68K030_PAGE_SIZE);
	if (handle == NULL || !user_range)
		return HAL_ERR_INVALID;
	if ((flags & ~(HAL_SPACE_PAGE_ACCESSED | HAL_SPACE_PAGE_DIRTY)) != 0)
		return HAL_ERR_INVALID;

	/* Refuses a stale handle. */
	acquired = space_lock_handle(handle, &space, &enabled);
	if (!acquired)
		return HAL_ERR_STATE;

	/* Refuses a page that is not mapped. */
	mapped = page_is_mapped(space, address);
	if (!mapped) {
		space_unlock(space, enabled);
		return HAL_ERR_INVALID;
	}

	/* Clears the descriptor's used or modified bit. */
	mask = 0;
	if ((flags & HAL_SPACE_PAGE_ACCESSED) != 0)
		mask |= M68K030_DESC_USED;
	if ((flags & HAL_SPACE_PAGE_DIRTY) != 0)
		mask |= M68K030_PAGE_MODIFIED;
	table = find_table(space, address);
	table->entries[m68k030_leaf_index(address)] &= ~mask;

	/* Drops the translations, so the next access sets the bit again. */
	hal_compiler_barrier();
	flush_locked(space);

	space_unlock(space, enabled);

	/* Succeeded. */
	return HAL_OK;
}

/*
 * Drops the translations of a space, or of the kernel half.
 */
void
hal_space_flush_tlb(
	hal_space_t handle)
{
	struct m68k030_space *space;
	bool enabled;
	int acquired;

	/* The kernel half's translations. */
	if (handle == HAL_SPACE_SYS) {
		enabled = hal_irq_disable();

		flush_locked(handle);

		if (enabled)
			hal_irq_enable();
		return;
	}

	/* Refuses a handle that is not a live space. */
	acquired = space_lock_handle(handle, &space, &enabled);
	if (!acquired)
		HAL_FATAL("invalid MC68030 space flush");

	/* Drops the translations when the space is the one selected. */
	flush_locked(space);

	space_unlock(space, enabled);
}

/*
 * Drops the translations of a range; the whole cache is dropped.
 */
void
hal_space_flush_tlb_range(
	hal_space_t handle,
	void *vaddr,
	size_t size)
{
	(void)vaddr;

	/* An empty range drops nothing. */
	if (size == 0)
		return;

	/* The cache holds few entries, so dropping them all costs little. */
	hal_space_flush_tlb(handle);
}

/*
 * Reports the page size of a level; only the base page is used.
 */
size_t
hal_space_get_page_size(
	int level)
{
	/* The base page is the only size the HAL maps for the kernel's use. */
	if (level == 1)
		return M68K030_PAGE_SIZE;

	/* No larger page is offered. */
	return 0;
}

/*
 * Reports the range of user addresses.
 */
void
hal_space_get_user_range(
	uintptr_t *minimum,
	uintptr_t *limit)
{
	/* The first page stays unmapped so a null pointer faults. */
	if (minimum != NULL)
		*minimum = M68K030_PAGE_SIZE;

	/* The user half ends where the kernel's direct map begins. */
	if (limit != NULL)
		*limit = M68K030_USER_LIMIT;
}

/*
 * Reports the number of address spaces and of page tables.
 */
void
m68k030_space_memory_stats(
	uint32_t *spaces,
	uint32_t *tables)
{
	bool enabled;

	/* Samples both counters together. */
	enabled = hal_irq_disable();

	if (spaces != NULL)
		*spaces = space_count;
	if (tables != NULL)
		*tables = page_table_count;

	if (enabled)
		hal_irq_enable();
}

/*
 * Maps a device physical range into kernel space.
 *
 * The X68000's devices lie in its 16 MiB physical space, which the kernel's
 * direct map at 0x80000000 covers from boot: the bootstrap maps the device
 * apertures uncached.  A range there is reached at its direct-map address;
 * no other device memory exists.
 */
int
hal_space_map_device(
	hal_physaddr_t paddr,
	size_t size,
	uint32_t attr,
	void **vaddr)
{
	/* Refuses a missing destination, an empty range, or executable device memory. */
	if (vaddr == NULL || size == 0)
		return HAL_ERR_INVALID;
	if ((attr & HAL_SPACE_EXEC) != 0)
		return HAL_ERR_INVALID;

	/* Refuses a range outside the device apertures of the physical space. */
	if (paddr < M68K030_DEVICE_FIRST || paddr >= M68K030_DIRECT_SIZE)
		return HAL_ERR_UNSUPPORTED;
	if (size > M68K030_DIRECT_SIZE - paddr)
		return HAL_ERR_UNSUPPORTED;

	/* Succeeded: the range is reached through the direct map. */
	*vaddr = m68k030_phys_to_direct((uintptr_t)paddr);
	return HAL_OK;
}

/*
 * Removes a mapping made by hal_space_map_device(); the direct map stays.
 */
int
hal_space_unmap_device(
	void *vaddr,
	size_t size)
{
	uintptr_t physical;

	/* Refuses an address that hal_space_map_device() never gave. */
	physical = m68k030_direct_to_phys(vaddr);
	if (physical < M68K030_DEVICE_FIRST || physical >= M68K030_DIRECT_SIZE)
		return HAL_ERR_INVALID;
	if (size > M68K030_DIRECT_SIZE - physical)
		return HAL_ERR_INVALID;

	/* Succeeded: nothing is torn down, the direct map is permanent. */
	return HAL_OK;
}

/* Allocates one zeroed page for a table and reports its physical and kernel addresses. */
static int
alloc_page(
	hal_physaddr_t *physical,
	uint32_t **entries)
{
	int error;

	/* Allocates the page. */
	error = hal_pmem_alloc(M68K030_PAGE_SIZE, M68K030_PAGE_SIZE, physical);
	if (error != HAL_OK)
		return error;

	/* Clears it through the direct map. */
	*entries = hal_pmem_to_kernel(*physical);
	hal_memset(*entries, 0, M68K030_PAGE_SIZE);

	/* Succeeded: the page is the caller's. */
	return HAL_OK;
}

/* Frees one table page. */
static void
free_page(
	hal_physaddr_t physical)
{
	hal_physaddr_t block;

	/* Releases the page; a page this file allocated is always accepted. */
	block = physical;
	(void)hal_pmem_free(&block, M68K030_PAGE_SIZE);
}

/*
 * Finds a live space by its handle and marks it busy, with interrupts off;
 * returns 1, or 0 for a handle that is not a live space.
 */
static int
space_lock_handle(
	hal_space_t handle,
	struct m68k030_space **result,
	bool *irq_enabled)
{
	struct m68k030_space *space;
	bool enabled;

	/* Looks the handle up in the registry with interrupts off. */
	enabled = hal_irq_disable();
	space = space_registry;
	while (space != NULL && (hal_space_t)space != handle)
		space = space->registry_next;

	/* Refuses a handle that is gone or going. */
	if (space == NULL || space->destroying) {
		if (enabled)
			hal_irq_enable();
		return 0;
	}

	/* An operation never nests inside another on the same space. */
	if (space->lock != 0)
		HAL_FATAL("recursive MC68030 space operation");

	/* Succeeded: the space is the caller's until space_unlock(). */
	space->lock = 1U;
	*result = space;
	*irq_enabled = enabled;
	return 1;
}

/* Ends an operation on a space and restores the interrupts. */
static void
space_unlock(
	struct m68k030_space *space,
	bool enabled)
{
	/* Only a busy space can be released. */
	if (space->lock != 1U)
		HAL_FATAL("invalid MC68030 space unlock");

	/* The space is free for the next operation. */
	space->lock = 0;
	if (enabled)
		hal_irq_enable();
}

/* Drops the translations when a space is the one selected, or for the kernel half. */
static void
flush_locked(
	hal_space_t handle)
{
	/* A space not selected has no translation in the cache. */
	if (handle != HAL_SPACE_SYS && handle != current_space)
		return;

	/* Flushes the address translation cache, ordered against the table writes. */
	hal_compiler_barrier();
	m68k030_flush_atc();
	hal_compiler_barrier();
}

/* Reports whether a range is whole pages of the user half, past page zero. */
static int
valid_user_range(
	uintptr_t address,
	size_t size)
{
	/* Refuses an empty or unaligned range. */
	if (size == 0)
		return 0;
	if ((address & M68K030_PAGE_MASK) != 0 || (size & M68K030_PAGE_MASK) != 0)
		return 0;

	/* Refuses page zero and anything outside the user half. */
	if (address < M68K030_PAGE_SIZE || address >= M68K030_USER_LIMIT)
		return 0;
	if (size > M68K030_USER_LIMIT - address)
		return 0;

	/* The range is user pages. */
	return 1;
}

/* Reports whether attributes grant some access and keep code unwritable. */
static int
attributes_are_valid(
	uint32_t attributes)
{
	/* Refuses attributes that grant no access at all. */
	if ((attributes & (HAL_SPACE_READ | HAL_SPACE_WRITE | HAL_SPACE_EXEC)) == 0)
		return 0;

	/* Refuses a page that is both writable and executable. */
	if ((attributes & HAL_SPACE_WRITE) != 0 && (attributes & HAL_SPACE_EXEC) != 0)
		return 0;

	/* The attributes are usable. */
	return 1;
}

/* Finds the page table that covers an address, or NULL. */
static struct m68k030_table_page *
find_table(
	struct m68k030_space *space,
	uintptr_t address)
{
	struct m68k030_table_page *table;
	uintptr_t base;

	/* Walks the space's tables for the one of the address's 4 MiB. */
	base = address & ~(uintptr_t)SPACE_ROOT_SPAN_MASK;
	for (table = space->tables; table != NULL; table = table->next) {
		if (table->virtual_base == base)
			return table;
	}

	/* No table covers the address. */
	return NULL;
}

/* Creates the empty page table that covers an address and links it into the root. */
static struct m68k030_table_page *
create_table(
	struct m68k030_space *space,
	uintptr_t address)
{
	struct m68k030_table_page *table;
	int error;

	/* Allocates the table's record. */
	table = kernel_alloc(sizeof(*table));
	if (table == NULL)
		return NULL;
	hal_memset(table, 0, sizeof(*table));

	/* Allocates the table's page, empty. */
	error = alloc_page(&table->physical, &table->entries);
	if (error != HAL_OK) {
		kernel_free(table);
		return NULL;
	}

	/* Links the table into the space and into the root. */
	table->virtual_base = address & ~(uintptr_t)SPACE_ROOT_SPAN_MASK;
	table->next = space->tables;
	space->tables = table;
	space->root[m68k030_root_index(address)] = m68k030_table_descriptor((uintptr_t)table->physical);
	hal_compiler_barrier();
	page_table_count++;

	/* Succeeded: the table. */
	return table;
}

/* Reports whether a page table maps no page. */
static int
table_empty(
	const struct m68k030_table_page *table)
{
	unsigned index;

	/* Looks for a valid descriptor. */
	for (index = 0; index < M68K030_LEAF_ENTRIES; index++) {
		if ((table->entries[index] & M68K030_DT_MASK) != M68K030_DT_INVALID)
			return 0;
	}

	/* No page is mapped. */
	return 1;
}

/* Unlinks the empty page tables from a space and its root, and returns them. */
static struct m68k030_table_page *
detach_empty_tables(
	struct m68k030_space *space)
{
	struct m68k030_table_page **link;
	struct m68k030_table_page *detached;
	struct m68k030_table_page *table;
	int empty;

	/* Walks the space's tables, moving the empty ones to the detached list. */
	detached = NULL;
	link = &space->tables;
	while (*link != NULL) {
		/* Keeps a table that still maps a page. */
		table = *link;
		empty = table_empty(table);
		if (!empty) {
			link = &table->next;
			continue;
		}

		/* Invalidates the table's root entry and unlinks it. */
		space->root[m68k030_root_index(table->virtual_base)] = M68K030_DT_INVALID;
		*link = table->next;
		table->next = detached;
		detached = table;
	}
	hal_compiler_barrier();

	/* Succeeded: the tables to free once the translations are dropped. */
	return detached;
}

/* Frees page tables no space refers to any more. */
static void
free_detached_tables(
	struct m68k030_table_page *table)
{
	struct m68k030_table_page *next;

	/* Frees the tables one after another. */
	while (table != NULL) {
		next = table->next;
		free_page(table->physical);
		kernel_free(table);
		if (page_table_count != 0)
			page_table_count--;
		table = next;
	}
}

/* Reports whether a user page has a page descriptor. */
static int
page_is_mapped(
	struct m68k030_space *space,
	uintptr_t address)
{
	struct m68k030_table_page *table;
	int is_page;

	/* A page without a table is not mapped. */
	table = find_table(space, address);
	if (table == NULL)
		return 0;

	/* Reports whether the page's descriptor is a page. */
	is_page = m68k030_descriptor_is_page(table->entries[m68k030_leaf_index(address)]);
	if (is_page)
		return 1;
	return 0;
}

/* Invalidates the descriptor of one user page, if it has a table. */
static void
clear_page(
	struct m68k030_space *space,
	uintptr_t address)
{
	struct m68k030_table_page *table;

	/* A page without a table has nothing to clear. */
	table = find_table(space, address);
	if (table == NULL)
		return;

	/* Invalidates the page's descriptor. */
	table->entries[m68k030_leaf_index(address)] = M68K030_DT_INVALID;
}

/* Builds the descriptor bits of a mapping's attributes. */
static uint32_t
page_attributes(
	uint32_t attributes)
{
	uint32_t descriptor;

	/* A page without write access is write-protected. */
	descriptor = 0;
	if ((attributes & HAL_SPACE_WRITE) == 0)
		descriptor |= M68K030_DESC_WRITE_PROTECT;

	/* An uncached or device page inhibits the cache. */
	if ((attributes & (HAL_SPACE_NOCACHE | HAL_SPACE_DEVICE)) != 0)
		descriptor |= M68K030_PAGE_CACHE_INHIBIT;

	/* Succeeded: the bits. */
	return descriptor;
}

/* Reports the HAL's page state of a descriptor: present, accessed and dirty. */
static uint32_t
descriptor_flags(
	uint32_t descriptor)
{
	uint32_t flags;
	int is_page;

	/* A page descriptor is present; its used and modified bits are its state. */
	flags = 0;
	is_page = m68k030_descriptor_is_page(descriptor);
	if (is_page)
		flags |= HAL_SPACE_PAGE_PRESENT;
	if ((descriptor & M68K030_DESC_USED) != 0)
		flags |= HAL_SPACE_PAGE_ACCESSED;
	if ((descriptor & M68K030_PAGE_MODIFIED) != 0)
		flags |= HAL_SPACE_PAGE_DIRTY;

	/* Succeeded: the state. */
	return flags;
}
