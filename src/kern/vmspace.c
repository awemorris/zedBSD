/*
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 *
 * Process virtual-memory ownership and demand paging.
 */

#include "kern/vmspace.h"
#include "kern/file.h"
#include "kern/kmem.h"
#include "kern/swap.h"
#include "kern/vm-reclaim.h"

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

static void
insert_region(struct vmspace *vm, struct vm_region *region)
{
	struct vm_region **link = &vm->regions;
	while (*link != NULL && (*link)->start < region->start)
		link = &(*link)->next;
	region->next = *link;
	*link = region;
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

static int
map_region(struct vmspace *vm, uintptr_t start, size_t size, uint32_t prot,
	   enum vm_region_backing backing, struct file *file,
	   off_t file_offset, uintptr_t data_start, size_t data_size,
	   struct vm_region **result)
{
	struct vm_region *region;

	if (vm == NULL || vm == &kernel_vmspace || !range_valid(start, size) ||
	    overlaps(vm, start, size) ||
	    (prot & ~(HAL_SPACE_READ | HAL_SPACE_WRITE | HAL_SPACE_EXEC)) != 0)
		return EINVAL;
	if (backing == VM_BACKING_FILE &&
	    (file == NULL || file_offset < 0 || data_start < start ||
	     data_start >= start + size || data_size > start + size - data_start))
		return EINVAL;
	region = kern_calloc(1, sizeof(*region));
	if (region == NULL)
		return ENOMEM;
	region->start = start;
	region->size = size;
	region->prot = prot;
	region->backing = backing;
	region->file = file;
	region->file_offset = file_offset;
	region->data_start = data_start;
	region->data_size = data_size;
	if (file != NULL)
		file_ref(file);
	insert_region(vm, region);
	if (result != NULL)
		*result = region;
	return 0;
}

int
vmspace_map_anon(struct vmspace *vm, uintptr_t start, size_t size,
		 uint32_t prot, struct vm_region **result)
{
	return map_region(vm, start, size, prot, VM_BACKING_ANON, NULL,
			  0, start, 0, result);
}

int
vmspace_map_anon_fixed_noreplace(struct vmspace *vm, uintptr_t start,
				 size_t size, uint32_t prot,
				 struct vm_region **result)
{
	if (vm == NULL || !range_valid(start, size))
		return EINVAL;
	if (overlaps(vm, start, size))
		return EEXIST;
	return vmspace_map_anon(vm, start, size, prot, result);
}

int
vmspace_map_file(struct vmspace *vm, uintptr_t start, size_t size,
		 uint32_t prot, struct file *file, off_t file_offset,
		 uintptr_t data_start, size_t data_size,
		 struct vm_region **result)
{
	return map_region(vm, start, size, prot, VM_BACKING_FILE, file,
			  file_offset, data_start, data_size, result);
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

static struct vm_page *
find_page(struct vm_region *region, uintptr_t address)
{
	struct vm_page *page;
	address &= ~(uintptr_t)(PAGE_SIZE - 1U);
	for (page = region->pages; page != NULL; page = page->next)
		if (page->address == address)
			return page;
	return NULL;
}

static int
allocate_page_frame(struct vm_page *page)
{
	if (hal_pmem_alloc(PAGE_SIZE, &page->pmem, 0) == HAL_PMEM_SUCCESS)
		return 0;
	if (vm_reclaim_one(page) != 0 ||
	    hal_pmem_alloc(PAGE_SIZE, &page->pmem, 0) != HAL_PMEM_SUCCESS)
		return ENOMEM;
	return 0;
}

static int
page_in(struct vm_page *page)
{
	struct swap_backend *backend = swap_system_backend();
	int error;

	if (backend == NULL || !(page->flags & VM_PAGE_SWAPPED))
		return EIO;
	page->flags |= VM_PAGE_BUSY;
	error = allocate_page_frame(page);
	if (error == 0)
		error = swap_read_page(backend, page->swap_slot,
				       (void *)page->pmem.vaddr);
	if (error == 0) {
		int mapped = hal_page_map(page->vm->space,
			(void *)page->address, page->pmem.paddr, PAGE_SIZE,
			page->region->prot);
		if (mapped == HAL_PMEM_NOSPACE && vm_reclaim_one(page) == 0)
			mapped = hal_page_map(page->vm->space,
				(void *)page->address, page->pmem.paddr, PAGE_SIZE,
				page->region->prot);
		if (mapped != HAL_PMEM_SUCCESS)
			error = ENOMEM;
	}
	if (error != 0) {
		if (page->pmem.size != 0)
			(void)hal_pmem_free(&page->pmem);
		page->flags &= ~VM_PAGE_BUSY;
		return error;
	}
	swap_free_slot(backend, page->swap_slot);
	page->swap_slot = SWAP_SLOT_NONE;
	page->flags &= ~(VM_PAGE_SWAPPED | VM_PAGE_BUSY);
	/*
	 * The swap slot was the only backing store for this page.  Once it is
	 * released, a clean-looking resident page must not be discarded: CPU
	 * reads do not set the PTE dirty bit and a later reclaim would recreate
	 * anonymous memory as zero-filled data.  Treat the page as dirty until
	 * it has been written to swap again.
	 */
	page->flags |= VM_PAGE_RESIDENT | VM_PAGE_DIRTY;
	vm_page_note_in(page);
	return 0;
}

static int
fill_file_page(struct vm_region *region, struct vm_page *page)
{
	uintptr_t page_end = page->address + PAGE_SIZE;
	uintptr_t data_end = region->data_start + region->data_size;
	uintptr_t read_start = page->address > region->data_start ?
		page->address : region->data_start;
	uintptr_t read_end = page_end < data_end ? page_end : data_end;
	size_t length;
	off_t offset;
	ssize_t count;

	memset((void *)page->pmem.vaddr, 0, PAGE_SIZE);
	if (read_start >= read_end)
		return 0;
	length = read_end - read_start;
	if ((uint64_t)region->file_offset +
	    (uint64_t)(read_start - region->data_start) > INT32_MAX)
		return EOVERFLOW;
	offset = region->file_offset +
		(off_t)(read_start - region->data_start);
	count = file_pread(region->file,
		(void *)(page->pmem.vaddr + read_start - page->address),
		length, offset);
	return count == (ssize_t)length ? 0 : EIO;
}

int
vmspace_fault(struct vmspace *vm, uintptr_t address, uint32_t required)
{
	struct vm_region *region;
	struct vm_page *page;
	uintptr_t page_address = address & ~(uintptr_t)(PAGE_SIZE - 1U);
	int error;
	vm_reclaim_note_fault();

	if (required != HAL_SPACE_READ && required != HAL_SPACE_WRITE &&
	    required != HAL_SPACE_EXEC)
		return EINVAL;
	region = vmspace_find_region(vm, address, 1);
	if (region == NULL || (region->prot & required) == 0)
		return EFAULT;
	page = find_page(region, page_address);
	if (page != NULL)
		return page->flags & VM_PAGE_SWAPPED ? page_in(page) : 0;
	page = kern_calloc(1, sizeof(*page));
	if (page == NULL)
		return ENOMEM;
	page->swap_slot = SWAP_SLOT_NONE;
	page->vm = vm;
	page->region = region;
	page->flags = VM_PAGE_BUSY;
	if (allocate_page_frame(page) != 0) {
		kern_free(page);
		return ENOMEM;
	}
	page->address = page_address;
	if (region->backing == VM_BACKING_FILE)
		error = fill_file_page(region, page);
	else {
		memset((void *)page->pmem.vaddr, 0, PAGE_SIZE);
		error = 0;
	}
	if (error == 0) {
		int mapped = hal_page_map(vm->space, (void *)page_address,
			page->pmem.paddr, PAGE_SIZE, region->prot);
		if (mapped == HAL_PMEM_NOSPACE && vm_reclaim_one(page) == 0)
			mapped = hal_page_map(vm->space, (void *)page_address,
				page->pmem.paddr, PAGE_SIZE, region->prot);
		if (mapped != HAL_PMEM_SUCCESS)
			error = ENOMEM;
	}
	if (error != 0) {
		(void)hal_pmem_free(&page->pmem);
		kern_free(page);
		return error;
	}
	page->flags = VM_PAGE_RESIDENT;
	page->next = region->pages;
	region->pages = page;
	vm_page_track(page);
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
vmspace_wire_range(struct vmspace *vm, uintptr_t address, size_t size,
		   uint32_t required)
{
	uintptr_t page, first, last;
	int error = vmspace_check(vm, address, size, required);

	if (error != 0)
		return error;
	first = page = address & ~(uintptr_t)(PAGE_SIZE - 1U);
	last = (address + size - 1U) & ~(uintptr_t)(PAGE_SIZE - 1U);
	for (;;) {
		struct vm_region *region;
		struct vm_page *entry;

		error = vmspace_fault(vm, page, required);
		if (error != 0)
			break;
		region = vmspace_find_region(vm, page, 1);
		entry = region != NULL ? find_page(region, page) : NULL;
		if (entry == NULL || !(entry->flags & VM_PAGE_RESIDENT)) {
			error = EFAULT;
			break;
		}
		entry->wire_count++;
		if (page == last)
			return 0;
		page += PAGE_SIZE;
	}
	while (page != first) {
		struct vm_region *region;
		struct vm_page *entry;
		page -= PAGE_SIZE;
		region = vmspace_find_region(vm, page, 1);
		entry = region != NULL ? find_page(region, page) : NULL;
		if (entry != NULL && entry->wire_count != 0)
			entry->wire_count--;
	}
	return error;
}

void
vmspace_unwire_range(struct vmspace *vm, uintptr_t address, size_t size)
{
	uintptr_t page, last;

	if (vm == NULL || size == 0 || address > UINTPTR_MAX - size)
		return;
	page = address & ~(uintptr_t)(PAGE_SIZE - 1U);
	last = (address + size - 1U) & ~(uintptr_t)(PAGE_SIZE - 1U);
	for (;;) {
		struct vm_region *region = vmspace_find_region(vm, page, 1);
		struct vm_page *entry = region != NULL ? find_page(region, page) : NULL;
		if (entry != NULL && entry->wire_count != 0)
			entry->wire_count--;
		if (page == last)
			break;
		page += PAGE_SIZE;
	}
}

static int
copy_backing(struct vmspace *vm, uintptr_t user_address, void *kernel,
	     size_t size, uint32_t required, int to_user)
{
	uint8_t *bytes = kernel;
	uintptr_t original_address = user_address;
	size_t original_size = size;

	if (size == 0)
		return 0;
	if (kernel == NULL)
		return EINVAL;
	if (vmspace_wire_range(vm, user_address, size, required) != 0)
		return EFAULT;
	while (size != 0) {
		struct vm_region *region = vmspace_find_region(vm, user_address, 1);
		struct vm_page *page = region != NULL ?
			find_page(region, user_address) : NULL;
		size_t offset = user_address & (PAGE_SIZE - 1U);
		size_t chunk = PAGE_SIZE - offset;
		void *mapped;

		if (page == NULL) {
			vmspace_unwire_range(vm, original_address, original_size);
			return EFAULT;
		}
		if (chunk > size)
			chunk = size;
		mapped = (void *)(page->pmem.vaddr + offset);
		if (to_user)
		{
			memcpy(mapped, bytes, chunk);
			page->flags |= VM_PAGE_DIRTY;
		} else
			memcpy(bytes, mapped, chunk);
		bytes += chunk;
		user_address += chunk;
		size -= chunk;
	}
	vmspace_unwire_range(vm, original_address, original_size);
	return 0;
}

int
vmspace_copy_to(struct vmspace *vm, uintptr_t destination,
		const void *source, size_t size)
{
	return copy_backing(vm, destination, (void *)source, size,
			    HAL_SPACE_WRITE, 1);
}

int
vmspace_copy_from(struct vmspace *vm, void *destination,
		  uintptr_t source, size_t size)
{
	return copy_backing(vm, source, destination, size,
			    HAL_SPACE_READ, 0);
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

static void
free_region_pages(struct vmspace *vm, struct vm_region *region)
{
	struct vm_page *page;

	while ((page = region->pages) != NULL) {
		region->pages = page->next;
		vm_page_untrack(page);
		if (page->flags & VM_PAGE_RESIDENT) {
			(void)hal_page_unmap(vm->space, (void *)page->address,
					     PAGE_SIZE);
			(void)hal_pmem_free(&page->pmem);
		}
		if ((page->flags & VM_PAGE_SWAPPED) &&
		    swap_system_backend() != NULL)
			swap_free_slot(swap_system_backend(), page->swap_slot);
		kern_free(page);
	}
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
	*link = region->next;
	free_region_pages(vm, region);
	if (region->file != NULL)
		(void)file_close(region->file);
	kern_free(region);
	return 0;
}

int
vmspace_protect(struct vmspace *vm, uintptr_t start, size_t size,
		uint32_t prot)
{
	struct vm_region *region;
	struct vm_page *page;

	if (vm == NULL || vm == &kernel_vmspace ||
	    (prot & ~(HAL_SPACE_READ | HAL_SPACE_WRITE | HAL_SPACE_EXEC)) != 0)
		return EINVAL;
	for (region = vm->regions; region != NULL; region = region->next)
		if (region->start == start && region->size == size)
			break;
	if (region == NULL)
		return EINVAL;
	for (page = region->pages; page != NULL; page = page->next)
		if ((page->flags & VM_PAGE_RESIDENT) &&
		    hal_page_prot(vm->space, (void *)page->address, PAGE_SIZE,
				  prot) != HAL_PMEM_SUCCESS)
			return EINVAL;
	region->prot = prot;
	return 0;
}

void
vmspace_free(struct vmspace *vm)
{
	struct vm_region *region;

	if (vm == NULL || vm == &kernel_vmspace)
		return;
	while ((region = vm->regions) != NULL) {
		vm->regions = region->next;
		free_region_pages(vm, region);
		if (region->file != NULL)
			(void)file_close(region->file);
		kern_free(region);
	}
	hal_page_destroy_space(vm->space);
	kern_free(vm);
}
