/* MC68030 two-level user page-table implementation. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <hal/hal.h>
#include "mmu030.h"
#include "space.h"

_Static_assert((uintptr_t)(M68K030_USER_LIMIT - 1U) <=
    (uintptr_t)INTPTR_MAX,
    "user pointers must not overlap the negative syscall errno window");

static hal_space_t current_space;
static struct m68k030_root_pointer system_crp;
static uint32_t next_space_id = 1;
static uint32_t space_count;
static uint32_t page_table_count;
static struct m68k030_space *space_registry;

static int
alloc_page(struct hal_pmem *memory)
{
	const struct hal_pmem_request request = {
		HAL_PMEM_PADDR_ANY, M68K030_PAGE_SIZE, M68K030_PAGE_SIZE,
		HAL_PMEM_TYPE_RAM, 0
	};
	return hal_pmem_alloc(&request, memory);
}

static int
space_lock_handle(hal_space_t handle, struct m68k030_space **result,
    bool *irq_enabled)
{
	struct m68k030_space *space;
	bool enabled = hal_irq_disable();

	for (space = space_registry; space != NULL; space = space->registry_next)
		if ((hal_space_t)space == handle)
			break;
	if (space == NULL || space->destroying) {
		if (enabled)
			hal_irq_enable();
		return 0;
	}
	if (space->lock != 0)
		HAL_FATAL("recursive MC68030 space operation");
	space->lock = 1U;
	*result = space;
	*irq_enabled = enabled;
	return 1;
}

static void
space_unlock(struct m68k030_space *space, bool enabled)
{
	if (space->lock != 1U)
		HAL_FATAL("invalid MC68030 space unlock");
	space->lock = 0;
	if (enabled)
		hal_irq_enable();
}

static void
flush_locked(hal_space_t handle)
{
	if (handle == HAL_SPACE_SYS || handle == current_space) {
		hal_compiler_barrier();
		m68k030_flush_atc();
		hal_compiler_barrier();
	}
}

static int
valid_user_range(uintptr_t address, size_t size)
{
	return size != 0 && m68k030_page_aligned(address) &&
		(size & M68K030_PAGE_MASK) == 0 &&
		address >= M68K030_PAGE_SIZE && address < M68K030_USER_LIMIT &&
		size <= M68K030_USER_LIMIT - address;
}

static struct m68k030_table_page *
find_table(struct m68k030_space *space, uintptr_t address)
{
	struct m68k030_table_page *table;
	uintptr_t base = address & ~((uintptr_t)0x003fffffU);

	for (table = space->tables; table != NULL; table = table->next)
		if (table->virtual_base == base)
			return table;
	return NULL;
}

static struct m68k030_table_page *
create_table(struct m68k030_space *space, uintptr_t address)
{
	struct m68k030_table_page *table;
	unsigned root_index = m68k030_root_index(address);

	table = hal_malloc(sizeof(*table));
	if (table == NULL)
		return NULL;
	hal_memset(table, 0, sizeof(*table));
	if (alloc_page(&table->memory) != HAL_OK) {
		hal_free(table);
		return NULL;
	}
	table->virtual_base = address & ~((uintptr_t)0x003fffffU);
	table->entries = table->memory.vaddr;
	hal_memset(table->entries, 0, M68K030_PAGE_SIZE);
	table->next = space->tables;
	space->tables = table;
	space->root[root_index] =
		m68k030_table_descriptor((uintptr_t)table->memory.paddr);
	hal_compiler_barrier();
	page_table_count++;
	return table;
}

static int
table_empty(const struct m68k030_table_page *table)
{
	unsigned index;
	for (index = 0; index < M68K030_LEAF_ENTRIES; index++)
		if ((table->entries[index] & M68K030_DT_MASK) !=
		    M68K030_DT_INVALID)
			return 0;
	return 1;
}

static struct m68k030_table_page *
detach_empty_tables(struct m68k030_space *space)
{
	struct m68k030_table_page **link = &space->tables;
	struct m68k030_table_page *detached = NULL;

	while (*link != NULL) {
		struct m68k030_table_page *table = *link;
		if (!table_empty(table)) {
			link = &table->next;
			continue;
		}
		space->root[m68k030_root_index(table->virtual_base)] =
			M68K030_DT_INVALID;
		*link = table->next;
		table->next = detached;
		detached = table;
	}
	hal_compiler_barrier();
	return detached;
}

static void
free_detached_tables(struct m68k030_table_page *table)
{
	while (table != NULL) {
		struct m68k030_table_page *next = table->next;

		(void)hal_pmem_free(&table->memory);
		hal_free(table);
		if (page_table_count != 0)
			page_table_count--;
		table = next;
	}
}

static uint32_t
page_attributes(uint32_t attributes)
{
	uint32_t descriptor = 0;
	if ((attributes & HAL_SPACE_WRITE) == 0)
		descriptor |= M68K030_DESC_WRITE_PROTECT;
	if (attributes & (HAL_SPACE_NOCACHE | HAL_SPACE_DEVICE))
		descriptor |= M68K030_PAGE_CACHE_INHIBIT;
	return descriptor;
}

void
m68k030_space_init(uintptr_t empty_root_physical)
{
	if (!m68k030_page_aligned(empty_root_physical))
		HAL_FATAL("unaligned empty MC68030 CRP root");
	if (hal_cpu_count() != 1)
		HAL_FATAL("MC68030 space implementation is UP-only");
	system_crp.attr = M68K030_ROOT_ATTR;
	system_crp.address = (uint32_t)empty_root_physical;
	current_space = HAL_SPACE_SYS;
	space_registry = NULL;
	space_count = page_table_count = 0;
	m68k030_load_crp(&system_crp);
	m68k030_flush_atc();
}

hal_space_t
hal_mem_create_space(void)
{
	struct m68k030_space *space = hal_malloc(sizeof(*space));
	bool enabled;

	if (space == NULL)
		return NULL;
	hal_memset(space, 0, sizeof(*space));
	if (alloc_page(&space->root_memory) != HAL_OK) {
		hal_free(space);
		return NULL;
	}
	space->root = space->root_memory.vaddr;
	hal_memset(space->root, 0, M68K030_PAGE_SIZE);
	space->root_pointer.attr = M68K030_ROOT_ATTR;
	space->root_pointer.address = (uint32_t)(uintptr_t)
		space->root_memory.paddr;
	space->magic = M68K030_SPACE_MAGIC;
	enabled = hal_irq_disable();
	space->space_id = next_space_id++;
	space->registry_next = space_registry;
	space_registry = space;
	space_count++;
	if (enabled)
		hal_irq_enable();
	return space;
}

void
hal_page_destroy_space(hal_space_t handle)
{
	struct m68k030_space *space = handle;
	struct m68k030_space **link;
	struct m68k030_table_page *table;
	bool enabled;

	if (space == NULL)
		return;
	enabled = hal_irq_disable();
	for (link = &space_registry; *link != NULL && *link != space;
	    link = &(*link)->registry_next)
		;
	if (*link == NULL || space->destroying)
		HAL_FATAL("invalid MC68030 space destroy");
	if (current_space == space)
		HAL_FATAL("destroying an active MC68030 space");
	if (space->lock != 0)
		HAL_FATAL("destroying a busy MC68030 space");
	space->destroying = 1U;
	*link = space->registry_next;
	while ((table = space->tables) != NULL) {
		space->tables = table->next;
		(void)hal_pmem_free(&table->memory);
		hal_free(table);
		if (page_table_count != 0)
			page_table_count--;
	}
	space->magic = 0;
	(void)hal_pmem_free(&space->root_memory);
	hal_free(space);
	if (space_count != 0)
		space_count--;
	if (enabled)
		hal_irq_enable();
}

void
hal_page_switch_space(hal_space_t handle)
{
	struct m68k030_space *space;
	const struct m68k030_root_pointer *root;
	bool enabled;

	if (handle == current_space)
		return;
	if (handle == HAL_SPACE_SYS) {
		enabled = hal_irq_disable();
		m68k030_load_crp(&system_crp);
		m68k030_flush_atc();
		current_space = handle;
		if (enabled)
			hal_irq_enable();
		return;
	}
	if (!space_lock_handle(handle, &space, &enabled))
		HAL_FATAL("invalid MC68030 space switch");
	root = &space->root_pointer;
	hal_compiler_barrier();
	m68k030_load_crp(root);
	m68k030_flush_atc();
	current_space = handle;
	space_unlock(space, enabled);
}

int
hal_page_map(hal_space_t handle, void *virtual_address,
	     hal_physaddr_t physical,
	     size_t size, uint32_t attributes)
{
	struct m68k030_space *space = handle;
	uintptr_t address = (uintptr_t)virtual_address;
	uintptr_t offset;
	bool enabled;

	if (space == NULL ||
	    !valid_user_range(address, size) ||
	    !m68k030_page_aligned(physical) ||
	    physical >= hal_pmem_get_total_size() ||
	    size > hal_pmem_get_total_size() - physical ||
	    (attributes & (HAL_SPACE_READ | HAL_SPACE_WRITE |
	     HAL_SPACE_EXEC)) == 0 ||
	    ((attributes & HAL_SPACE_WRITE) && (attributes & HAL_SPACE_EXEC)))
		return HAL_ERR_INVALID;
	if (!space_lock_handle(handle, &space, &enabled))
		return HAL_ERR_STATE;
	for (offset = 0; offset < size; offset += M68K030_PAGE_SIZE) {
		struct m68k030_table_page *table = find_table(space,
			address + offset);
		unsigned index = m68k030_leaf_index(address + offset);
		if (table != NULL && m68k030_descriptor_is_page(
		    table->entries[index])) {
			space_unlock(space, enabled);
			return HAL_ERR_INVALID;
		}
	}
	for (offset = 0; offset < size; offset += M68K030_PAGE_SIZE) {
		struct m68k030_table_page *table = find_table(space,
			address + offset);
		unsigned index = m68k030_leaf_index(address + offset);
		if (table == NULL)
			table = create_table(space, address + offset);
		if (table == NULL) {
			struct m68k030_table_page *detached;
			uintptr_t rollback;

			for (rollback = 0; rollback < offset;
			    rollback += M68K030_PAGE_SIZE) {
				table = find_table(space, address + rollback);
				if (table != NULL)
					table->entries[m68k030_leaf_index(
					    address + rollback)] = M68K030_DT_INVALID;
			}
			detached = detach_empty_tables(space);
			flush_locked(space);
			free_detached_tables(detached);
			space_unlock(space, enabled);
			return HAL_ERR_NOMEM;
		}
		table->entries[index] = m68k030_page_descriptor(
			physical + offset, page_attributes(attributes));
	}
	hal_compiler_barrier();
	flush_locked(space);
	space_unlock(space, enabled);
	return HAL_OK;
}

int
hal_page_prot(hal_space_t handle, void *virtual_address, size_t size,
	      uint32_t attributes)
{
	return hal_page_prot_query(handle, virtual_address, size, attributes, NULL);
}

int
hal_page_prot_query(hal_space_t handle, void *virtual_address, size_t size,
		    uint32_t attributes, uint32_t *flags)
{
	struct m68k030_space *space = handle;
	uintptr_t address = (uintptr_t)virtual_address;
	uintptr_t offset;
	uint32_t observed = 0;
	bool enabled;

	if (space == NULL ||
	    !valid_user_range(address, size) ||
	    (attributes & (HAL_SPACE_READ | HAL_SPACE_WRITE |
	     HAL_SPACE_EXEC)) == 0 ||
	    ((attributes & HAL_SPACE_WRITE) && (attributes & HAL_SPACE_EXEC)))
		return HAL_ERR_INVALID;
	if (!space_lock_handle(handle, &space, &enabled))
		return HAL_ERR_STATE;
	/* A failed range operation must not change an earlier descriptor. */
	for (offset = 0; offset < size; offset += M68K030_PAGE_SIZE) {
		struct m68k030_table_page *table = find_table(space,
			address + offset);
		unsigned index = m68k030_leaf_index(address + offset);

		if (table == NULL || !m68k030_descriptor_is_page(
		    table->entries[index])) {
			space_unlock(space, enabled);
			return HAL_ERR_INVALID;
		}
	}
	for (offset = 0; offset < size; offset += M68K030_PAGE_SIZE) {
		struct m68k030_table_page *table = find_table(space,
			address + offset);
		unsigned index = m68k030_leaf_index(address + offset);
		uint32_t old;
		old = table->entries[index];
		observed |= HAL_PAGE_PRESENT;
		if (old & M68K030_DESC_USED)
			observed |= HAL_PAGE_ACCESSED;
		if (old & M68K030_PAGE_MODIFIED)
			observed |= HAL_PAGE_DIRTY;
		table->entries[index] = m68k030_page_descriptor(
			m68k030_page_address(old), page_attributes(attributes) |
			(old & (M68K030_DESC_USED | M68K030_PAGE_MODIFIED)));
	}
	hal_compiler_barrier();
	flush_locked(space);
	/* The MC68030 updates U/M in memory.  Once the ATC is flushed, the final
	 * descriptor is stable with respect to the old translation. */
	for (offset = 0; offset < size; offset += M68K030_PAGE_SIZE) {
		struct m68k030_table_page *table = find_table(space,
			address + offset);
		uint32_t entry = table->entries[
			m68k030_leaf_index(address + offset)];
		if (entry & M68K030_DESC_USED)
			observed |= HAL_PAGE_ACCESSED;
		if (entry & M68K030_PAGE_MODIFIED)
			observed |= HAL_PAGE_DIRTY;
	}
	if (flags != NULL)
		*flags = observed;
	space_unlock(space, enabled);
	return HAL_OK;
}

int
hal_page_unmap(hal_space_t handle, void *virtual_address, size_t size)
{
	struct m68k030_space *space = handle;
	struct m68k030_table_page *detached;
	uintptr_t address = (uintptr_t)virtual_address;
	uintptr_t offset;
	bool enabled;

	if (size == 0)
		return HAL_OK;
	if (space == NULL ||
	    !valid_user_range(address, size))
		return HAL_ERR_INVALID;
	if (!space_lock_handle(handle, &space, &enabled))
		return HAL_ERR_STATE;
	for (offset = 0; offset < size; offset += M68K030_PAGE_SIZE) {
		struct m68k030_table_page *table = find_table(space,
			address + offset);
		if (table != NULL)
			table->entries[m68k030_leaf_index(address + offset)] =
				M68K030_DT_INVALID;
	}
	detached = detach_empty_tables(space);
	flush_locked(space);
	free_detached_tables(detached);
	space_unlock(space, enabled);
	return HAL_OK;
}

int
hal_page_query(hal_space_t handle, void *virtual_address, uint32_t *flags)
{
	struct m68k030_space *space = handle;
	uintptr_t address = (uintptr_t)virtual_address;
	struct m68k030_table_page *table;
	uint32_t descriptor;
	bool enabled;

	if (space == NULL || flags == NULL ||
	    !valid_user_range(address, M68K030_PAGE_SIZE))
		return HAL_ERR_INVALID;
	if (!space_lock_handle(handle, &space, &enabled))
		return HAL_ERR_STATE;
	table = find_table(space, address);
	descriptor = table == NULL ? M68K030_DT_INVALID :
		table->entries[m68k030_leaf_index(address)];
	*flags = m68k030_descriptor_is_page(descriptor) ? HAL_PAGE_PRESENT : 0;
	if (descriptor & M68K030_DESC_USED)
		*flags |= HAL_PAGE_ACCESSED;
	if (descriptor & M68K030_PAGE_MODIFIED)
		*flags |= HAL_PAGE_DIRTY;
	space_unlock(space, enabled);
	return HAL_OK;
}

int
hal_page_clear_flags(hal_space_t handle, void *virtual_address,
		     uint32_t flags)
{
	struct m68k030_space *space = handle;
	uintptr_t address = (uintptr_t)virtual_address;
	struct m68k030_table_page *table;
	uint32_t mask = 0;
	bool enabled;

	if (space == NULL ||
	    !valid_user_range(address, M68K030_PAGE_SIZE) ||
	    (flags & ~(HAL_PAGE_ACCESSED | HAL_PAGE_DIRTY)) != 0)
		return HAL_ERR_INVALID;
	if (!space_lock_handle(handle, &space, &enabled))
		return HAL_ERR_STATE;
	table = find_table(space, address);
	if (table == NULL || !m68k030_descriptor_is_page(
	    table->entries[m68k030_leaf_index(address)])) {
		space_unlock(space, enabled);
		return HAL_ERR_INVALID;
	}
	if (flags & HAL_PAGE_ACCESSED)
		mask |= M68K030_DESC_USED;
	if (flags & HAL_PAGE_DIRTY)
		mask |= M68K030_PAGE_MODIFIED;
	table->entries[m68k030_leaf_index(address)] &= ~mask;
	hal_compiler_barrier();
	flush_locked(space);
	space_unlock(space, enabled);
	return HAL_OK;
}

void
hal_page_flush_tlb(hal_space_t handle)
{
	struct m68k030_space *space;
	bool enabled;

	if (handle == HAL_SPACE_SYS) {
		enabled = hal_irq_disable();
		flush_locked(handle);
		if (enabled)
			hal_irq_enable();
		return;
	}
	if (!space_lock_handle(handle, &space, &enabled))
		HAL_FATAL("invalid MC68030 space flush");
	flush_locked(space);
	space_unlock(space, enabled);
}

void
hal_page_flush_tlb_range(hal_space_t handle, void *virtual_address,
			 size_t size)
{
	(void)virtual_address;
	if (size != 0)
		hal_page_flush_tlb(handle);
}

size_t
hal_page_get_page_size(int level)
{
	return level == 1 ? M68K030_PAGE_SIZE : 0;
}

void
hal_page_get_user_range(uintptr_t *minimum, uintptr_t *limit)
{
	if (minimum != NULL)
		*minimum = M68K030_PAGE_SIZE;
	if (limit != NULL)
		*limit = M68K030_USER_LIMIT;
}

void
hal_m68k_space_memory_stats(uint32_t *spaces, uint32_t *tables)
{
	bool enabled = hal_irq_disable();

	if (spaces != NULL)
		*spaces = space_count;
	if (tables != NULL)
		*tables = page_table_count;
	if (enabled)
		hal_irq_enable();
}
