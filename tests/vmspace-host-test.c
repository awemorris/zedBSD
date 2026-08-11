#include "kern/kmem.h"
#include "kern/vmspace.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct fake_space { unsigned maps; };
static unsigned spaces_created, spaces_destroyed;
static unsigned pages_allocated, pages_freed, pages_unmapped, pages_protected;
static int fail_space, fail_pages, fail_map, fail_protect;

void *kern_malloc(size_t size) { return malloc(size); }
void *kern_calloc(size_t count, size_t size) { return calloc(count, size); }
void kern_free(void *pointer) { free(pointer); }

hal_space_t
hal_mem_create_space(void)
{
	struct fake_space *space;
	if (fail_space)
		return NULL;
	space = calloc(1, sizeof(*space));
	if (space != NULL)
		spaces_created++;
	return space;
}

void
hal_page_destroy_space(hal_space_t handle)
{
	spaces_destroyed++;
	free(handle);
}

int
hal_pmem_alloc(size_t size, struct hal_pmem *memory, uint32_t flags)
{
	(void)flags;
	if (fail_pages)
		return HAL_PMEM_NOSPACE;
	memory->vaddr = (uintptr_t)calloc(1, size);
	if (memory->vaddr == 0)
		return HAL_PMEM_NOSPACE;
	memory->paddr = (uintptr_t)memory->vaddr & ~(uintptr_t)4095U;
	memory->size = size;
	pages_allocated++;
	return HAL_PMEM_SUCCESS;
}

int
hal_pmem_free(struct hal_pmem *memory)
{
	free((void *)memory->vaddr);
	pages_freed++;
	return HAL_PMEM_SUCCESS;
}

int
hal_page_map(hal_space_t handle, void *address, uintptr_t paddr, size_t size,
	     uint32_t attr)
{
	struct fake_space *space = handle;
	(void)address;
	(void)paddr;
	(void)size;
	(void)attr;
	if (fail_map)
		return HAL_PMEM_BADDESC;
	space->maps++;
	return HAL_PMEM_SUCCESS;
}

int
hal_page_unmap(hal_space_t handle, void *address, size_t size)
{
	(void)handle;
	(void)address;
	(void)size;
	pages_unmapped++;
	return HAL_PMEM_SUCCESS;
}

int
hal_page_prot(hal_space_t handle, void *address, size_t size, uint32_t attr)
{
	(void)handle;
	(void)address;
	(void)size;
	(void)attr;
	if (fail_protect)
		return HAL_PMEM_BADDESC;
	pages_protected++;
	return HAL_PMEM_SUCCESS;
}

int
main(void)
{
	struct vmspace *vm;
	struct vm_region *region;

	vm = vmspace_create();
	assert(vm != NULL && spaces_created == 1);
	assert(vmspace_map_anon(vm, 0x400000, 4096,
				HAL_SPACE_READ | HAL_SPACE_EXEC, &region) == 0);
	assert(region->start == 0x400000 && region->size == 4096);
	assert(vmspace_find_region(vm, 0x400000, 16) == region);
	assert(vmspace_protect(vm, 0x400000, 4096,
			       HAL_SPACE_READ | HAL_SPACE_WRITE) == 0);
	assert(region->prot == (HAL_SPACE_READ | HAL_SPACE_WRITE));
	assert(pages_protected == 1);
	fail_protect = 1;
	assert(vmspace_protect(vm, 0x400000, 4096, HAL_SPACE_READ) == EINVAL);
	assert(region->prot == (HAL_SPACE_READ | HAL_SPACE_WRITE));
	fail_protect = 0;
	assert(vmspace_map_anon(vm, 0x400000, 4096, HAL_SPACE_READ,
				NULL) == EINVAL);
	assert(vmspace_map_anon(vm, 0x400001, 4096, HAL_SPACE_READ,
				NULL) == EINVAL);
	vmspace_free(vm);
	assert(spaces_destroyed == 1 && pages_allocated == 1);
	assert(pages_unmapped == 1 && pages_freed == 1);

	fail_space = 1;
	assert(vmspace_create() == NULL);
	fail_space = 0;
	vm = vmspace_create();
	assert(vm != NULL);
	fail_pages = 1;
	assert(vmspace_map_anon(vm, 0x500000, 4096, HAL_SPACE_READ,
				NULL) == ENOMEM);
	fail_pages = 0;
	fail_map = 1;
	assert(vmspace_map_anon(vm, 0x500000, 4096, HAL_SPACE_READ,
				NULL) == EINVAL);
	fail_map = 0;
	vmspace_free(vm);
	assert(spaces_created == spaces_destroyed);
	assert(pages_allocated == pages_freed);

	puts("Boots vmspace host tests: PASS");
	return 0;
}
