/*
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 *
 * Process virtual-memory ownership.
 */

#include "kern/vmspace.h"
#include "kern/kmem.h"

#include <errno.h>
#include <string.h>

#define PAGE_SIZE 4096U
#define VM_MMAP_BASE 0x10000000U
#define VM_STACK_GUARD (64U * 1024U)

struct vmspace kernel_vmspace = {
	.space = HAL_SPACE_SYS,
	.usecount = 1,
};

static int
range_valid(uintptr_t start, size_t size)
{
	return size != 0 && (start & (PAGE_SIZE - 1U)) == 0 &&
		(size & (PAGE_SIZE - 1U)) == 0 && start >= VM_USER_MIN &&
		start < VM_USER_TOP && size <= VM_USER_TOP - start;
}

static int
overlaps(struct vmspace *vm, uintptr_t start, size_t size)
{
	struct vm_region *region;
	uintptr_t end = start + size;

	for (region = vm->regions; region != NULL; region = region->next)
		if (start < region->start + region->size && region->start < end)
			return 1;
	return 0;
}

struct vmspace *
vmspace_create(void)
{
	struct vmspace *vm = kern_calloc(1, sizeof(*vm));

	if (vm == NULL)
		return NULL;
	vm->space = hal_mem_create_space();
	if (vm->space == NULL) {
		kern_free(vm);
		return NULL;
	}
	vm->usecount = 1;
	return vm;
}

int
vmspace_map_anon(struct vmspace *vm, uintptr_t start, size_t size,
		 uint32_t prot, struct vm_region **result)
{
	struct vm_region *region;
	int error;

	if (vm == NULL || vm == &kernel_vmspace ||
	    !range_valid(start, size) || overlaps(vm, start, size) ||
	    (prot & ~(HAL_SPACE_READ | HAL_SPACE_WRITE | HAL_SPACE_EXEC)) != 0)
		return EINVAL;
	region = kern_calloc(1, sizeof(*region));
	if (region == NULL)
		return ENOMEM;
	error = hal_pmem_alloc(size, &region->pmem, 0);
	if (error != HAL_PMEM_SUCCESS) {
		kern_free(region);
		return ENOMEM;
	}
	memset((void *)region->pmem.vaddr, 0, region->pmem.size);
	error = hal_page_map(vm->space, (void *)start, region->pmem.paddr,
			     size, prot != 0 ? prot : HAL_SPACE_READ);
	if (error != HAL_PMEM_SUCCESS) {
		(void)hal_pmem_free(&region->pmem);
		kern_free(region);
		return error == HAL_PMEM_NOSPACE ? ENOMEM : EINVAL;
	}
	region->start = start;
	region->size = size;
	region->prot = prot;
	{
		struct vm_region **link = &vm->regions;
		while (*link != NULL && (*link)->start < start)
			link = &(*link)->next;
		region->next = *link;
		*link = region;
	}
	if (result != NULL)
		*result = region;
	return 0;
}

int
vmspace_check(struct vmspace *vm, uintptr_t address, size_t size,
	      uint32_t required)
{
	struct vm_region *region;
	uintptr_t current, end;

	if (vm == NULL || size == 0 || address < VM_USER_MIN ||
	    address >= VM_USER_TOP || size > VM_USER_TOP - address)
		return EFAULT;
	current = address;
	end = address + size;
	for (region = vm->regions; region != NULL && current < end;
	     region = region->next) {
		uintptr_t region_end = region->start + region->size;
		if (current < region->start)
			return EFAULT;
		if (current >= region_end)
			continue;
		if ((region->prot & required) != required)
			return EFAULT;
		current = region_end < end ? region_end : end;
	}
	return current == end ? 0 : EFAULT;
}

int
vmspace_map_find(struct vmspace *vm, uintptr_t hint, size_t size,
		 uint32_t prot, uintptr_t *mapped)
{
	uintptr_t start, limit;
	struct vm_region *region;

	if (vm == NULL || mapped == NULL || size == 0 ||
	    size > (size_t)(VM_USER_TOP - VM_MMAP_BASE))
		return EINVAL;
	if (size > SIZE_MAX - (PAGE_SIZE - 1U))
		return EOVERFLOW;
	size = (size + PAGE_SIZE - 1U) & ~(PAGE_SIZE - 1U);
	limit = vm->stack_bottom > VM_STACK_GUARD ?
		vm->stack_bottom - VM_STACK_GUARD : VM_USER_TOP;
	start = hint >= VM_MMAP_BASE && (hint & (PAGE_SIZE - 1U)) == 0 ?
		hint : VM_MMAP_BASE;
	for (;;) {
		int moved = 0;
		if (start >= limit || size > limit - start)
			return ENOMEM;
		for (region = vm->regions; region != NULL; region = region->next) {
			if (start + size <= region->start)
				break;
			if (start < region->start + region->size &&
			    region->start < start + size) {
				start = (region->start + region->size + PAGE_SIZE - 1U) &
					~(uintptr_t)(PAGE_SIZE - 1U);
				moved = 1;
				break;
			}
		}
		if (!moved)
			break;
	}
	{
		int error = vmspace_map_anon(vm, start, size, prot, NULL);
		if (error != 0)
			return error;
	}
	*mapped = start;
	return 0;
}

int
vmspace_unmap(struct vmspace *vm, uintptr_t start, size_t size)
{
	struct vm_region **link, *region;

	if (vm == NULL || vm == &kernel_vmspace)
		return EINVAL;
	for (link = &vm->regions; *link != NULL; link = &(*link)->next)
		if ((*link)->start == start && (*link)->size == size)
			break;
	region = *link;
	if (region == NULL)
		return EINVAL;
	if (hal_page_unmap(vm->space, (void *)start, size) != HAL_PMEM_SUCCESS)
		return EINVAL;
	*link = region->next;
	(void)hal_pmem_free(&region->pmem);
	kern_free(region);
	return 0;
}

int
vmspace_protect(struct vmspace *vm, uintptr_t start, size_t size,
		uint32_t prot)
{
	struct vm_region *region;

	if (vm == NULL || vm == &kernel_vmspace ||
	    (prot & ~(HAL_SPACE_READ | HAL_SPACE_WRITE | HAL_SPACE_EXEC)) != 0)
		return EINVAL;
	for (region = vm->regions; region != NULL; region = region->next)
		if (region->start == start && region->size == size)
			break;
	if (region == NULL)
		return EINVAL;
	if (hal_page_prot(vm->space, (void *)start, size, prot) !=
	    HAL_PMEM_SUCCESS)
		return EINVAL;
	region->prot = prot;
	return 0;
}

struct vm_region *
vmspace_find_region(struct vmspace *vm, uintptr_t address, size_t size)
{
	struct vm_region *region;

	if (vm == NULL || size == 0 || address > UINTPTR_MAX - size)
		return NULL;
	for (region = vm->regions; region != NULL; region = region->next)
		if (address >= region->start &&
		    address + size <= region->start + region->size)
			return region;
	return NULL;
}

void
vmspace_free(struct vmspace *vm)
{
	struct vm_region *region;

	if (vm == NULL)
		return;
	if (vm == &kernel_vmspace)
		return;
	while ((region = vm->regions) != NULL) {
		vm->regions = region->next;
		(void)hal_page_unmap(vm->space, (void *)region->start,
				     region->size);
		(void)hal_pmem_free(&region->pmem);
		kern_free(region);
	}
	hal_page_destroy_space(vm->space);
	kern_free(vm);
}
