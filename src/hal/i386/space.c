/*
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 *
 * i386 protected user address spaces.
 */
#include <hal/hal.h>
#include "defs.h"
#include "space.h"

_Static_assert((uintptr_t)(SYS_START - 1U) <= (uintptr_t)INTPTR_MAX,
    "user pointers must not overlap the negative syscall errno window");

static hal_space_t current_spaces[HAL_CPU_MAX];
#define current_space current_spaces[hal_cpu_current()]
static uint32 system_cr3;
static int next_space_id;
static uint32 space_count;
static uint32 page_table_count;

static int
alloc_page(struct hal_pmem *memory)
{
	const struct hal_pmem_request request = {
		HAL_PMEM_PADDR_ANY, PAGE_SIZE, PAGE_SIZE,
		HAL_PMEM_TYPE_RAM, 0
	};
	return hal_pmem_alloc(&request, memory);
}

static int
valid_space(hal_space_t space)
{
	return space == HAL_SPACE_SYS ||
		(space != NULL &&
		 ((struct i386_space *)space)->magic == I386_SPACE_MAGIC);
}

void
i386_space_init(void)
{
	system_cr3 = asm_get_cr3();
	current_space = HAL_SPACE_SYS;
	next_space_id = 1;
	space_count = page_table_count = 0;
}

void
i386_space_init_secondary(void)
{
	current_space = HAL_SPACE_SYS;
	asm_load_cr3(system_cr3);
}

static struct i386_page_table *
find_table(struct i386_space *space, uintptr_t vaddr)
{
	struct i386_page_table *table;
	uintptr_t base = vaddr & 0xffc00000U;

	for (table = space->page_tables; table != NULL; table = table->next)
		if (table->vaddr == base)
			return table;
	return NULL;
}

static struct i386_page_table *
create_table(struct i386_space *space, uintptr_t vaddr)
{
	struct i386_page_table *table;
	unsigned pde = (unsigned)(vaddr >> 22);

	table = hal_malloc(sizeof(*table));
	if (table == NULL)
		return NULL;
	hal_memset(table, 0, sizeof(*table));
	if (alloc_page(&table->memory) != HAL_OK) {
		hal_free(table);
		return NULL;
	}
	table->vaddr = vaddr & 0xffc00000U;
	table->pte = table->memory.vaddr;
	hal_memset(table->pte, 0, PAGE_SIZE);
	table->next = space->page_tables;
	space->page_tables = table;
	page_table_count++;
	space->pdt[pde] = ((uint32)table->memory.paddr & 0xfffff000U) |
		PTE_PRESENT | PTE_WRITE | PTE_USER;
	return table;
}

hal_space_t
hal_mem_create_space(void)
{
	struct i386_space *space;
	uint32 *system_pdt;
	unsigned i;

	space = hal_malloc(sizeof(*space));
	if (space == NULL)
		return NULL;
	hal_memset(space, 0, sizeof(*space));
	if (alloc_page(&space->directory_memory) != HAL_OK) {
		hal_free(space);
		return NULL;
	}
	space->pdt = space->directory_memory.vaddr;
	hal_memset(space->pdt, 0, PAGE_SIZE);
	system_pdt = (uint32 *)(system_cr3 | SYS_START);
	for (i = 512; i < 1024; i++)
		space->pdt[i] = system_pdt[i] & ~PTE_USER;
	space->magic = I386_SPACE_MAGIC;
	space->space_id = next_space_id++;
	space_count++;
	return space;
}

void
hal_page_destroy_space(hal_space_t handle)
{
	struct i386_space *space = handle;
	struct i386_page_table *table;

	if (space == NULL)
		return;
	if (!valid_space(space))
		HAL_FATAL("invalid space destroy");
	if (current_space == space)
		hal_page_switch_space(HAL_SPACE_SYS);
	while ((table = space->page_tables) != NULL) {
		space->page_tables = table->next;
		(void)hal_pmem_free(&table->memory);
		hal_free(table);
		if (page_table_count != 0)
			page_table_count--;
	}
	space->magic = 0;
	(void)hal_pmem_free(&space->directory_memory);
	hal_free(space);
	if (space_count != 0)
		space_count--;
}

void
hal_i386_space_memory_stats(uint32 *spaces, uint32 *tables)
{
	if (spaces != NULL)
		*spaces = space_count;
	if (tables != NULL)
		*tables = page_table_count;
}

void
hal_page_switch_space(hal_space_t handle)
{
	uint32 cr3;

	if (!valid_space(handle))
		HAL_FATAL("invalid space switch");
	if (handle == current_space)
		return;
	cr3 = handle == HAL_SPACE_SYS ? system_cr3 :
		(uint32)((struct i386_space *)handle)->directory_memory.paddr;
	asm_load_cr3(cr3);
	current_space = handle;
}

static int
valid_user_range(uintptr_t vaddr, size_t size)
{
	return size != 0 && (vaddr & (PAGE_SIZE - 1U)) == 0 &&
		(size & (PAGE_SIZE - 1U)) == 0 && vaddr >= PAGE_SIZE &&
		vaddr < SYS_START && size <= SYS_START - vaddr;
}

static uint32
pte_flags(uint32 attr)
{
	uint32 flags = PTE_PRESENT | PTE_USER;

	if (attr & HAL_SPACE_WRITE)
		flags |= PTE_WRITE;
	if (attr & HAL_SPACE_NOCACHE)
		flags |= PTE_NOCACHE;
	if (attr & HAL_SPACE_WRITETHRU)
		flags |= PTE_WRITEBACK;
	return flags;
}

int
hal_page_map(hal_space_t handle, void *address, hal_physaddr_t paddr, size_t size,
	     uint32 attr)
{
	struct i386_space *space = handle;
	uintptr_t vaddr = (uintptr_t)address;
	uintptr_t offset;

	if (space == NULL || !valid_space(space) ||
	    !valid_user_range(vaddr, size) ||
	    (paddr & (PAGE_SIZE - 1U)) != 0 ||
	    (attr & (HAL_SPACE_READ | HAL_SPACE_WRITE | HAL_SPACE_EXEC)) == 0)
		return HAL_ERR_INVALID;
	for (offset = 0; offset < size; offset += PAGE_SIZE) {
		struct i386_page_table *table = find_table(space, vaddr + offset);
		unsigned index = (unsigned)((vaddr + offset) >> 12) & 1023U;

		if (table != NULL && (table->pte[index] & PTE_PRESENT))
			return HAL_ERR_INVALID;
	}
	for (offset = 0; offset < size; offset += PAGE_SIZE) {
		struct i386_page_table *table = find_table(space, vaddr + offset);
		unsigned index = (unsigned)((vaddr + offset) >> 12) & 1023U;

		if (table == NULL)
			table = create_table(space, vaddr + offset);
		if (table == NULL) {
			(void)hal_page_unmap(space, address, offset);
			return HAL_ERR_NOMEM;
		}
		table->pte[index] = (uint32)(paddr + offset) | pte_flags(attr);
	}
	if (current_space == space)
		hal_page_flush_tlb(space);
	return HAL_OK;
}

int
hal_page_prot(hal_space_t handle, void *address, size_t size, uint32 attr)
{
	struct i386_space *space = handle;
	uintptr_t vaddr = (uintptr_t)address;
	uintptr_t offset;

	if (space == NULL || !valid_space(space) ||
	    !valid_user_range(vaddr, size))
		return HAL_ERR_INVALID;
	for (offset = 0; offset < size; offset += PAGE_SIZE) {
		struct i386_page_table *table = find_table(space, vaddr + offset);
		unsigned index = (unsigned)((vaddr + offset) >> 12) & 1023U;
		uint32 physical;

		if (table == NULL || !(table->pte[index] & PTE_PRESENT))
			return HAL_ERR_INVALID;
		physical = table->pte[index] & 0xfffff000U;
		table->pte[index] = physical | pte_flags(attr);
	}
	if (current_space == space)
		hal_page_flush_tlb(space);
	return HAL_OK;
}

int
hal_page_unmap(hal_space_t handle, void *address, size_t size)
{
	struct i386_space *space = handle;
	uintptr_t vaddr = (uintptr_t)address;
	uintptr_t offset;

	if (size == 0)
		return HAL_OK;
	if (space == NULL || !valid_space(space) ||
	    !valid_user_range(vaddr, size))
		return HAL_ERR_INVALID;
	for (offset = 0; offset < size; offset += PAGE_SIZE) {
		struct i386_page_table *table = find_table(space, vaddr + offset);
		unsigned index = (unsigned)((vaddr + offset) >> 12) & 1023U;

		if (table != NULL)
			table->pte[index] = 0;
	}
	{
		struct i386_page_table **link = &space->page_tables;
		while (*link != NULL) {
			struct i386_page_table *table = *link;
			unsigned index;
			for (index = 0; index < 1024U; index++)
				if (table->pte[index] & PTE_PRESENT)
					break;
			if (index != 1024U) {
				link = &table->next;
				continue;
			}
			space->pdt[table->vaddr >> 22] = 0;
			*link = table->next;
			(void)hal_pmem_free(&table->memory);
			hal_free(table);
			if (page_table_count != 0)
				page_table_count--;
		}
	}
	if (current_space == space)
		hal_page_flush_tlb(space);
	return HAL_OK;
}

int
hal_page_query(hal_space_t handle, void *address, uint32_t *flags)
{
	struct i386_space *space = handle;
	uintptr_t vaddr = (uintptr_t)address;
	struct i386_page_table *table;
	uint32 pte;
	unsigned index;

	if (space == NULL || !valid_space(space) || flags == NULL ||
	    !valid_user_range(vaddr, PAGE_SIZE))
		return HAL_ERR_INVALID;
	table = find_table(space, vaddr);
	index = (unsigned)(vaddr >> 12) & 1023U;
	pte = table != NULL ? table->pte[index] : 0;
	*flags = (pte & PTE_PRESENT ? HAL_PAGE_PRESENT : 0) |
		(pte & PTE_ACCESS ? HAL_PAGE_ACCESSED : 0) |
		(pte & PTE_DIRTY ? HAL_PAGE_DIRTY : 0);
	return HAL_OK;
}

int
hal_page_clear_flags(hal_space_t handle, void *address, uint32_t flags)
{
	struct i386_space *space = handle;
	uintptr_t vaddr = (uintptr_t)address;
	struct i386_page_table *table;
	unsigned index;
	uint32 mask = 0;

	if (space == NULL || !valid_space(space) ||
	    !valid_user_range(vaddr, PAGE_SIZE) ||
	    (flags & ~(HAL_PAGE_ACCESSED | HAL_PAGE_DIRTY)) != 0)
		return HAL_ERR_INVALID;
	table = find_table(space, vaddr);
	index = (unsigned)(vaddr >> 12) & 1023U;
	if (table == NULL || !(table->pte[index] & PTE_PRESENT))
		return HAL_ERR_INVALID;
	if (flags & HAL_PAGE_ACCESSED) mask |= PTE_ACCESS;
	if (flags & HAL_PAGE_DIRTY) mask |= PTE_DIRTY;
	table->pte[index] &= ~mask;
	if (current_space == space)
		hal_page_flush_tlb(space);
	return HAL_OK;
}

void hal_page_flush_tlb(hal_space_t handle)
{
	if (handle == HAL_SPACE_SYS || handle == current_space)
		asm_flash_tlb();
}

void hal_page_flush_tlb_range(hal_space_t handle, void *vaddr, size_t size)
{
	(void)vaddr;
	if (size != 0)
		hal_page_flush_tlb(handle);
}

size_t hal_page_get_page_size(int level)
{
	return level == 1 ? PAGE_SIZE : 0;
}

void hal_page_get_user_range(uintptr_t *minimum, uintptr_t *limit)
{
	if (minimum != NULL) *minimum = PAGE_SIZE;
	if (limit != NULL) *limit = SYS_START;
}
