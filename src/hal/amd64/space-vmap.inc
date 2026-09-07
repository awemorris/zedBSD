/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <limits.h>

/* Owned scratch mappings in the shared, supervisor-only upper half. */
#define VMAP_BASE UINT64_C(0xffffc00000000000)
#define VMAP_SLOTS 128U
#define VMAP_PAGES (HAL_VMAP_MAX_SIZE / PAGE_SIZE)
#define VMAP_STRIDE (2U * HAL_VMAP_MAX_SIZE)

enum vmap_state { VMAP_FREE, VMAP_RESERVED, VMAP_POPULATING, VMAP_LIVE, VMAP_RETIRING };

struct hal_vmap {
	enum vmap_state state;
	unsigned pins;
	size_t size;
	struct hal_pmem pages[VMAP_PAGES];
};

static struct hal_vmap vmap_slots[VMAP_SLOTS];
static struct amd64_space vmap_space;

static int vmap_valid(struct hal_vmap *mapping);
static uintptr_t vmap_address(struct hal_vmap *mapping);
static void vmap_free_pages(struct hal_vmap *mapping);
static void vmap_clear_locked(struct hal_vmap *mapping);

unsigned
hal_vmap_capabilities(void)
{
	return ram_active ? 1U : 0U;
}

int
hal_vmap_reserve(
	size_t size,
	struct hal_vmap **result)
{
	unsigned index;
	bool enabled;

	if (result == NULL)
		return HAL_ERR_INVALID;
	*result = NULL;
	if (size == 0 || size > HAL_VMAP_MAX_SIZE || size % PAGE_SIZE != 0)
		return HAL_ERR_INVALID;
	if (!ram_active)
		return HAL_ERR_UNSUPPORTED;
	enabled = space_lock_enter(&vmap_space);
	vmap_space.pml4 = system_pml4;
	for (index = 0; index < VMAP_SLOTS; index++) {
		if (vmap_slots[index].state != VMAP_FREE)
			continue;
		vmap_slots[index].size = size;
		vmap_slots[index].state = VMAP_RESERVED;
		*result = &vmap_slots[index];
		break;
	}
	space_lock_leave(&vmap_space, enabled);
	return *result != NULL ? HAL_OK : HAL_ERR_NOMEM;
}

int
hal_vmap_populate(
	struct hal_vmap *mapping,
	uint64_t minimum,
	uint64_t maximum)
{
	struct hal_pmem_request request;
	struct hal_pmem *memory;
	uint64_t entry;
	uint64_t *leaf;
	uintptr_t address;
	unsigned index, count;
	bool enabled;
	int error;

	if (!vmap_valid(mapping) || minimum > maximum)
		return HAL_ERR_INVALID;
	enabled = space_lock_enter(&vmap_space);
	if (mapping->state != VMAP_RESERVED) {
		space_lock_leave(&vmap_space, enabled);
		return HAL_ERR_BUSY;
	}
	mapping->state = VMAP_POPULATING;
	count = (unsigned)(mapping->size / PAGE_SIZE);
	space_lock_leave(&vmap_space, enabled);
	hal_memset(&request, 0, sizeof(request));
	request.paddr = HAL_PMEM_PADDR_ANY;
	request.size = request.alignment = PAGE_SIZE;
	request.type = HAL_PMEM_TYPE_RAM;
	error = HAL_OK;
	for (index = 0; index < count; index++) {
		memory = &mapping->pages[index];
		error = hal_pmem_alloc_range(&request, minimum, maximum, 0, memory);
		if (error != HAL_OK)
			break;
		if (memory->size != PAGE_SIZE || memory->vaddr == NULL ||
		    memory->paddr % PAGE_SIZE != 0 ||
		    !amd64_ram_lookup(&ram_builder, memory->paddr, &entry) ||
		    (entry & AMD64_PTE_WRITE) == 0) {
			error = HAL_ERR_INVALID;
			break;
		}
		hal_memset(memory->vaddr, 0, PAGE_SIZE);
	}

	/* No borrower can observe the partially installed vector. */
	enabled = space_lock_enter(&vmap_space);
	address = vmap_address(mapping);
	for (index = 0; error == HAL_OK && index < count; index++) {
		leaf = walk_leaf(&vmap_space, address + index * PAGE_SIZE, 1);
		if (leaf == NULL) {
			error = HAL_ERR_NOMEM;
			break;
		}
		if (*leaf & AMD64_PTE_PRESENT)
			HAL_FATAL("vmap reserved range overlaps a mapping");
		*leaf = mapping->pages[index].paddr | AMD64_PTE_PRESENT |
		    AMD64_PTE_WRITE | AMD64_PTE_NX;
	}
	if (error == HAL_OK) {
		shootdown(HAL_SPACE_SYS, (void *)address, mapping->size);
		mapping->state = VMAP_LIVE;
	} else {
		vmap_clear_locked(mapping);
	}
	space_lock_leave(&vmap_space, enabled);
	if (error != HAL_OK) {
		vmap_free_pages(mapping);
		enabled = space_lock_enter(&vmap_space);
		mapping->state = VMAP_RESERVED;
		space_lock_leave(&vmap_space, enabled);
	}
	return error;
}

int
hal_vmap_pin(
	struct hal_vmap *mapping,
	void **address)
{
	bool enabled;
	int error;

	if (!vmap_valid(mapping) || address == NULL)
		return HAL_ERR_INVALID;
	*address = NULL;
	enabled = space_lock_enter(&vmap_space);
	error = HAL_ERR_STATE;
	if (mapping->state == VMAP_LIVE && mapping->pins != UINT_MAX) {
		mapping->pins++;
		*address = (void *)vmap_address(mapping);
		error = HAL_OK;
	}
	space_lock_leave(&vmap_space, enabled);
	return error;
}

void
hal_vmap_unpin(
	struct hal_vmap *mapping)
{
	bool enabled;

	if (!vmap_valid(mapping))
		HAL_FATAL("invalid vmap unpin");
	enabled = space_lock_enter(&vmap_space);
	if (mapping->state != VMAP_LIVE || mapping->pins == 0)
		HAL_FATAL("unbalanced vmap unpin");
	mapping->pins--;
	space_lock_leave(&vmap_space, enabled);
}

int
hal_vmap_release(
	struct hal_vmap *mapping)
{
	bool enabled;

	if (!vmap_valid(mapping))
		return HAL_ERR_INVALID;
	enabled = space_lock_enter(&vmap_space);
	if (mapping->pins != 0 || (mapping->state != VMAP_LIVE &&
	    mapping->state != VMAP_RESERVED)) {
		space_lock_leave(&vmap_space, enabled);
		return HAL_ERR_BUSY;
	}
	if (mapping->state == VMAP_LIVE)
		vmap_clear_locked(mapping);
	mapping->state = VMAP_RETIRING;
	space_lock_leave(&vmap_space, enabled);
	vmap_free_pages(mapping);
	enabled = space_lock_enter(&vmap_space);
	mapping->size = 0;
	mapping->state = VMAP_FREE;
	space_lock_leave(&vmap_space, enabled);
	return HAL_OK;
}

/* Caller retains the mapping's original owner/pin across lookup and use. */
int
hal_kernel_page_lookup(
	const void *pointer,
	hal_physaddr_t *physical)
{
	uintptr_t address, translated;
	uint64_t *leaf;
	bool enabled;
	int error;

	if (physical == NULL)
		return HAL_ERR_INVALID;
	translated = amd64_image_to_phys(pointer);
	if (translated == UINTPTR_MAX)
		translated = amd64_direct_to_phys(pointer);
	if (translated != UINTPTR_MAX) {
		*physical = translated;
		return HAL_OK;
	}
	address = (uintptr_t)pointer;
	if (address < VMAP_BASE || address - VMAP_BASE >= VMAP_SLOTS * VMAP_STRIDE)
		return HAL_ERR_INVALID;
	enabled = space_lock_enter(&vmap_space);
	error = HAL_ERR_INVALID;
	if (vmap_space.pml4 != NULL) {
		leaf = walk_leaf(&vmap_space, address, 0);
		if (leaf != NULL && (*leaf & AMD64_PTE_PRESENT)) {
			*physical = (*leaf & AMD64_PTE_ADDR_MASK) | (address & (PAGE_SIZE - 1U));
			error = HAL_OK;
		}
	}
	space_lock_leave(&vmap_space, enabled);
	return error;
}

static int
vmap_valid(
	struct hal_vmap *mapping)
{
	uintptr_t address, start;

	address = (uintptr_t)mapping;
	start = (uintptr_t)vmap_slots;
	return address >= start && address - start < sizeof(vmap_slots) &&
	    (address - start) % sizeof(vmap_slots[0]) == 0;
}

static uintptr_t
vmap_address(
	struct hal_vmap *mapping)
{
	return VMAP_BASE + (size_t)(mapping - vmap_slots) * VMAP_STRIDE;
}

static void
vmap_free_pages(
	struct hal_vmap *mapping)
{
	unsigned index;

	for (index = 0; index < VMAP_PAGES; index++) {
		if (mapping->pages[index].size == 0)
			continue;
		if (hal_pmem_free(&mapping->pages[index]) != HAL_OK)
			HAL_FATAL("vmap backing retirement failed");
		hal_memset(&mapping->pages[index], 0, sizeof(mapping->pages[index]));
	}
}

/* Only vmap-owned subordinate tables are detached; upper roots stay shared. */
static void
vmap_clear_locked(
	struct hal_vmap *mapping)
{
	struct amd64_table_page *detached;
	uint64_t *leaf;
	uintptr_t address;
	size_t offset;

	address = vmap_address(mapping);
	for (offset = 0; offset < mapping->size; offset += PAGE_SIZE) {
		leaf = walk_leaf(&vmap_space, address + offset, 0);
		if (leaf != NULL)
			*leaf = 0;
	}
	detached = detach_empty_tables(&vmap_space);
	shootdown(HAL_SPACE_SYS, detached != NULL ? NULL : (void *)address,
	    detached != NULL ? 0 : mapping->size);
	free_detached_tables(detached);
}
