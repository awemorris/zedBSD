/* MC68030 two-level user page-table implementation. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <hal/hal.h>
#include "mmu030.h"
#include "space.h"

static hal_space_t current_space;
static struct m68k030_root_pointer system_crp;
static uint32_t next_space_id = 1;
static uint32_t space_count;
static uint32_t page_table_count;

static int
valid_space(hal_space_t handle)
{
	struct m68k030_space *space = handle;
	return handle == HAL_SPACE_SYS ||
		(space != NULL && space->magic == M68K030_SPACE_MAGIC);
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
	if (pmem_alloc_lo(M68K030_PAGE_SIZE, &table->memory) != PMEM_SUCCESS) {
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

static void
reclaim_empty_tables(struct m68k030_space *space)
{
	struct m68k030_table_page **link = &space->tables;

	while (*link != NULL) {
		struct m68k030_table_page *table = *link;
		if (!table_empty(table)) {
			link = &table->next;
			continue;
		}
		space->root[m68k030_root_index(table->virtual_base)] =
			M68K030_DT_INVALID;
		*link = table->next;
		(void)pmem_free(&table->memory);
		hal_free(table);
		if (page_table_count != 0)
			page_table_count--;
	}
	hal_compiler_barrier();
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
	system_crp.attr = M68K030_ROOT_ATTR;
	system_crp.address = (uint32_t)empty_root_physical;
	current_space = HAL_SPACE_SYS;
	space_count = page_table_count = 0;
	m68k030_load_crp(&system_crp);
	m68k030_flush_atc();
}

hal_space_t
hal_mem_create_space(void)
{
	struct m68k030_space *space = hal_malloc(sizeof(*space));

	if (space == NULL)
		return NULL;
	hal_memset(space, 0, sizeof(*space));
	if (pmem_alloc_lo(M68K030_PAGE_SIZE, &space->root_memory) !=
	    PMEM_SUCCESS) {
		hal_free(space);
		return NULL;
	}
	space->root = space->root_memory.vaddr;
	hal_memset(space->root, 0, M68K030_PAGE_SIZE);
	space->root_pointer.attr = M68K030_ROOT_ATTR;
	space->root_pointer.address = (uint32_t)(uintptr_t)
		space->root_memory.paddr;
	space->space_id = next_space_id++;
	space->magic = M68K030_SPACE_MAGIC;
	space_count++;
	return space;
}

void
hal_page_destroy_space(hal_space_t handle)
{
	struct m68k030_space *space = handle;
	struct m68k030_table_page *table;

	if (space == NULL)
		return;
	if (!valid_space(space))
		HAL_FATAL("invalid MC68030 space destroy");
	if (current_space == space)
		hal_page_switch_space(HAL_SPACE_SYS);
	while ((table = space->tables) != NULL) {
		space->tables = table->next;
		(void)pmem_free(&table->memory);
		hal_free(table);
		if (page_table_count != 0)
			page_table_count--;
	}
	space->magic = 0;
	(void)pmem_free(&space->root_memory);
	hal_free(space);
	if (space_count != 0)
		space_count--;
}

void
hal_page_switch_space(hal_space_t handle)
{
	const struct m68k030_root_pointer *root;

	if (!valid_space(handle))
		HAL_FATAL("invalid MC68030 space switch");
	if (handle == current_space)
		return;
	root = handle == HAL_SPACE_SYS ? &system_crp :
		&((struct m68k030_space *)handle)->root_pointer;
	hal_compiler_barrier();
	m68k030_load_crp(root);
	m68k030_flush_atc();
	current_space = handle;
}

int
hal_page_map(hal_space_t handle, void *virtual_address, uintptr_t physical,
	     size_t size, uint32_t attributes)
{
	struct m68k030_space *space = handle;
	uintptr_t address = (uintptr_t)virtual_address;
	uintptr_t offset;

	if (space == NULL || !valid_space(space) ||
	    !valid_user_range(address, size) ||
	    !m68k030_page_aligned(physical) ||
	    physical >= hal_pmem_get_total_size() ||
	    size > hal_pmem_get_total_size() - physical ||
	    (attributes & (HAL_SPACE_READ | HAL_SPACE_WRITE |
	     HAL_SPACE_EXEC)) == 0 ||
	    ((attributes & HAL_SPACE_WRITE) && (attributes & HAL_SPACE_EXEC)))
		return HAL_PMEM_BADDESC;
	for (offset = 0; offset < size; offset += M68K030_PAGE_SIZE) {
		struct m68k030_table_page *table = find_table(space,
			address + offset);
		unsigned index = m68k030_leaf_index(address + offset);
		if (table != NULL && m68k030_descriptor_is_page(
		    table->entries[index]))
			return HAL_PMEM_BADDESC;
	}
	for (offset = 0; offset < size; offset += M68K030_PAGE_SIZE) {
		struct m68k030_table_page *table = find_table(space,
			address + offset);
		unsigned index = m68k030_leaf_index(address + offset);
		if (table == NULL)
			table = create_table(space, address + offset);
		if (table == NULL) {
			(void)hal_page_unmap(space, virtual_address, offset);
			reclaim_empty_tables(space);
			return HAL_PMEM_NOSPACE;
		}
		table->entries[index] = m68k030_page_descriptor(
			physical + offset, page_attributes(attributes));
	}
	hal_compiler_barrier();
	if (current_space == space)
		hal_page_flush_tlb(space);
	return HAL_PMEM_SUCCESS;
}

int
hal_page_prot(hal_space_t handle, void *virtual_address, size_t size,
	      uint32_t attributes)
{
	struct m68k030_space *space = handle;
	uintptr_t address = (uintptr_t)virtual_address;
	uintptr_t offset;

	if (space == NULL || !valid_space(space) ||
	    !valid_user_range(address, size) ||
	    (attributes & (HAL_SPACE_READ | HAL_SPACE_WRITE |
	     HAL_SPACE_EXEC)) == 0 ||
	    ((attributes & HAL_SPACE_WRITE) && (attributes & HAL_SPACE_EXEC)))
		return HAL_PMEM_BADDESC;
	for (offset = 0; offset < size; offset += M68K030_PAGE_SIZE) {
		struct m68k030_table_page *table = find_table(space,
			address + offset);
		unsigned index = m68k030_leaf_index(address + offset);
		uint32_t old;
		if (table == NULL || !m68k030_descriptor_is_page(
		    table->entries[index]))
			return HAL_PMEM_BADDESC;
		old = table->entries[index];
		table->entries[index] = m68k030_page_descriptor(
			m68k030_page_address(old), page_attributes(attributes) |
			(old & (M68K030_DESC_USED | M68K030_PAGE_MODIFIED)));
	}
	hal_compiler_barrier();
	if (current_space == space)
		hal_page_flush_tlb(space);
	return HAL_PMEM_SUCCESS;
}

int
hal_page_unmap(hal_space_t handle, void *virtual_address, size_t size)
{
	struct m68k030_space *space = handle;
	uintptr_t address = (uintptr_t)virtual_address;
	uintptr_t offset;

	if (size == 0)
		return HAL_PMEM_SUCCESS;
	if (space == NULL || !valid_space(space) ||
	    !valid_user_range(address, size))
		return HAL_PMEM_BADDESC;
	for (offset = 0; offset < size; offset += M68K030_PAGE_SIZE) {
		struct m68k030_table_page *table = find_table(space,
			address + offset);
		if (table != NULL)
			table->entries[m68k030_leaf_index(address + offset)] =
				M68K030_DT_INVALID;
	}
	reclaim_empty_tables(space);
	if (current_space == space)
		hal_page_flush_tlb(space);
	return HAL_PMEM_SUCCESS;
}

int
hal_page_query(hal_space_t handle, void *virtual_address, uint32_t *flags)
{
	struct m68k030_space *space = handle;
	uintptr_t address = (uintptr_t)virtual_address;
	struct m68k030_table_page *table;
	uint32_t descriptor;

	if (space == NULL || !valid_space(space) || flags == NULL ||
	    !valid_user_range(address, M68K030_PAGE_SIZE))
		return HAL_PMEM_BADDESC;
	table = find_table(space, address);
	descriptor = table == NULL ? M68K030_DT_INVALID :
		table->entries[m68k030_leaf_index(address)];
	*flags = m68k030_descriptor_is_page(descriptor) ? HAL_PAGE_PRESENT : 0;
	if (descriptor & M68K030_DESC_USED)
		*flags |= HAL_PAGE_ACCESSED;
	if (descriptor & M68K030_PAGE_MODIFIED)
		*flags |= HAL_PAGE_DIRTY;
	return HAL_PMEM_SUCCESS;
}

int
hal_page_clear_flags(hal_space_t handle, void *virtual_address,
		     uint32_t flags)
{
	struct m68k030_space *space = handle;
	uintptr_t address = (uintptr_t)virtual_address;
	struct m68k030_table_page *table;
	uint32_t mask = 0;

	if (space == NULL || !valid_space(space) ||
	    !valid_user_range(address, M68K030_PAGE_SIZE) ||
	    (flags & ~(HAL_PAGE_ACCESSED | HAL_PAGE_DIRTY)) != 0)
		return HAL_PMEM_BADDESC;
	table = find_table(space, address);
	if (table == NULL || !m68k030_descriptor_is_page(
	    table->entries[m68k030_leaf_index(address)]))
		return HAL_PMEM_BADDESC;
	if (flags & HAL_PAGE_ACCESSED)
		mask |= M68K030_DESC_USED;
	if (flags & HAL_PAGE_DIRTY)
		mask |= M68K030_PAGE_MODIFIED;
	table->entries[m68k030_leaf_index(address)] &= ~mask;
	hal_compiler_barrier();
	if (current_space == space)
		hal_page_flush_tlb(space);
	return HAL_PMEM_SUCCESS;
}

void
hal_page_flush_tlb(hal_space_t handle)
{
	if (handle == HAL_SPACE_SYS || handle == current_space) {
		hal_compiler_barrier();
		m68k030_flush_atc();
		hal_compiler_barrier();
	}
}

size_t
hal_page_get_page_size(int level)
{
	return level == 1 ? M68K030_PAGE_SIZE : 0;
}

void
hal_m68k_space_memory_stats(uint32_t *spaces, uint32_t *tables)
{
	if (spaces != NULL)
		*spaces = space_count;
	if (tables != NULL)
		*tables = page_table_count;
}
