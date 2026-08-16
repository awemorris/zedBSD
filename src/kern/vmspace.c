/*
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 *
 * Process virtual-memory ownership and demand paging.
 */

#include "kern/vmspace.h"
#include "kern/file.h"
#include "kern/kmem.h"
#include "kern/page.h"
#include "kern/swap.h"
#include "kern/vm-commit.h"
#include "kern/vm-object.h"
#include "kern/vm-reclaim.h"

#include <errno.h>
#include <string.h>
#include <sys/mman.h>

#define PAGE_SIZE ZEDBSD_PAGE_SIZE
struct vmspace kernel_vmspace = {
	.space = HAL_SPACE_SYS,
	.refs = { 1 },
};

struct vm_layout vm_layout;
static int vm_layout_initialized;
static atomic_uint_t vmspace_live;

static int
alloc_vm_page(struct hal_pmem *memory)
{
	const struct hal_pmem_request request = {
		HAL_PMEM_PADDR_ANY, PAGE_SIZE, PAGE_SIZE,
		HAL_PMEM_TYPE_RAM, 0
	};
	return hal_pmem_alloc(&request, memory);
}

void
vmspace_layout_init(void)
{
	uintptr_t minimum, limit;
	if (vm_layout_initialized)
		return;
	hal_page_get_user_range(&minimum, &limit);
	if (minimum < PAGE_SIZE || (minimum & (PAGE_SIZE - 1U)) != 0 ||
	    limit <= minimum || (limit & (PAGE_SIZE - 1U)) != 0)
		HAL_FATAL("invalid HAL user address range");
	vm_layout.user_minimum = minimum;
	vm_layout.user_limit = limit;
#ifdef ZEDBSD_USER_ABI_LP64
	vm_layout.brk_limit = 0x0000000100000000ULL;
	vm_layout.mmap_base = 0x0000000100000000ULL;
#else
	vm_layout.brk_limit = 0x10000000U;
	vm_layout.mmap_base = 0x10000000U;
#endif
	if (vm_layout.brk_limit >= limit || vm_layout.mmap_base >= limit ||
	    limit - minimum <= PAGE_SIZE)
		HAL_FATAL("user address range too small");
	vm_layout.stack_top = limit - PAGE_SIZE;
	vm_layout_initialized = 1;
}

static int
range_valid(uintptr_t start, size_t size)
{
	vmspace_layout_init();
	return size != 0 && (start & (PAGE_SIZE - 1U)) == 0 &&
		(size & (PAGE_SIZE - 1U)) == 0 &&
		start >= vm_layout.user_minimum && start < vm_layout.user_limit &&
		size <= vm_layout.user_limit - start;
}

int vmspace_user_range_valid(uintptr_t start, size_t size)
{
	vmspace_layout_init();
	return size != 0 && start >= vm_layout.user_minimum &&
	    start < vm_layout.user_limit && size <= vm_layout.user_limit - start;
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

static int map_region(struct vmspace *, uintptr_t, size_t, uint32_t,
		      enum vm_region_backing, struct file *, off_t, uintptr_t,
		      size_t, unsigned, size_t, struct vm_region **);
static int allocate_page_frame(struct vm_page *);
static int page_in(struct vm_page *);

struct vmspace *
vmspace_create(void)
{
	struct vmspace *vm = kern_calloc(1, sizeof(*vm));
	vmspace_layout_init();

	if (vm == NULL)
		return NULL;
	vm->space = hal_mem_create_space();
	if (vm->space == NULL) {
		kern_free(vm);
		return NULL;
	}
	refcount_init(&vm->refs, 1);
	(void)atomic_fetch_add_relaxed(&vmspace_live, 1U);
	return vm;
}

int
vmspace_tryref(struct vmspace *vm)
{
	return vm != NULL && refcount_tryget(&vm->refs);
}

void
vmspace_ref(struct vmspace *vm)
{
	if (vm != NULL)
		refcount_get(&vm->refs);
}

int
vmspace_fork(struct vmspace *source, struct vmspace **result)
{
	struct vmspace *copy;
	struct vm_region *source_region;
	int error = 0;

	if (source == NULL || source == &kernel_vmspace || result == NULL)
		return EINVAL;
	copy = vmspace_create();
	if (copy == NULL)
		return ENOMEM;
	for (source_region = source->regions; source_region != NULL;
	     source_region = source_region->next) {
		struct vm_region *copy_region;
		struct vm_page *source_page;

		error = map_region(copy, source_region->start, source_region->size,
		    source_region->prot, source_region->backing,
		    source_region->file, source_region->file_offset,
		    source_region->data_start, source_region->data_size,
		    source_region->flags, source_region->commit_size,
		    &copy_region);
		if (error != 0)
			goto fail;
		if (source_region->object != NULL) {
			vm_object_ref(source_region->object);
			copy_region->object = source_region->object;
			/* Shared object pages are mapped lazily in the child. */
			continue;
		}
		for (source_page = source_region->pages; source_page != NULL;
		     source_page = source_page->next) {
			struct vm_page *copy_page;
			uint32_t page_flags;
			int mapped;

			if (source_page->flags & VM_PAGE_SWAPPED) {
				error = page_in(source_page);
				if (error != 0)
					goto fail;
			}
			copy_page = kern_calloc(1, sizeof(*copy_page));
			if (copy_page == NULL) {
				error = ENOMEM;
				goto fail;
			}
			copy_page->swap_slot = SWAP_SLOT_NONE;
			copy_page->vm = copy;
			copy_page->region = copy_region;
			copy_page->address = source_page->address;
			copy_page->flags = VM_PAGE_BUSY;
			error = allocate_page_frame(copy_page);
			if (error != 0) {
				kern_free(copy_page);
				goto fail;
			}
			memcpy((void *)copy_page->pmem.vaddr,
			    (const void *)source_page->pmem.vaddr, PAGE_SIZE);
			page_flags = 0;
			if (hal_page_query(source->space,
			    (void *)source_page->address, &page_flags) ==
			    HAL_OK && (page_flags & HAL_PAGE_DIRTY) != 0)
				copy_page->flags |= VM_PAGE_DIRTY;
			if (source_page->flags & VM_PAGE_DIRTY)
				copy_page->flags |= VM_PAGE_DIRTY;
			mapped = hal_page_map(copy->space,
			    (void *)copy_page->address, copy_page->pmem.paddr,
			    PAGE_SIZE, copy_region->prot);
			if (mapped != HAL_OK) {
				(void)hal_pmem_free(&copy_page->pmem);
				kern_free(copy_page);
				error = ENOMEM;
				goto fail;
			}
			copy_page->flags &= ~VM_PAGE_BUSY;
			copy_page->flags |= VM_PAGE_RESIDENT;
			copy_page->next = copy_region->pages;
			copy_region->pages = copy_page;
			vm_page_track(copy_page);
		}
	}
	copy->entry = source->entry;
	copy->brk_start = source->brk_start;
	copy->brk_current = source->brk_current;
	copy->stack_guard_bottom = source->stack_guard_bottom;
	copy->stack_bottom = source->stack_bottom;
	copy->stack_top = source->stack_top;
	*result = copy;
	return 0;

fail:
	vmspace_free(copy);
	return error;
}

static int
map_region(struct vmspace *vm, uintptr_t start, size_t size, uint32_t prot,
	   enum vm_region_backing backing, struct file *file,
	   off_t file_offset, uintptr_t data_start, size_t data_size,
	   unsigned flags, size_t commit_size, struct vm_region **result)
{
	struct vm_region *region;
	int error;

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
	if (commit_size != 0) {
		error = vm_commit_reserve(commit_size);
		if (error != 0) {
			kern_free(region);
			return error;
		}
	}
	region->start = start;
	region->size = size;
	region->prot = prot;
	region->flags = flags;
	region->commit_size = commit_size;
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
			  0, start, 0, 0, prot != 0 ? size : 0, result);
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
			  file_offset, data_start, data_size, 0,
			  (prot & HAL_SPACE_WRITE) != 0 ? size : 0, result);
}

int
vmspace_map_file_shared(struct vmspace *vm, uintptr_t start, size_t size,
			uint32_t prot, struct file *file, off_t file_offset,
			size_t data_size, struct vm_region **result)
{
	struct vm_object *object;
	struct vm_region *region;
	int error;

	if (file_offset < 0 || (file_offset & (PAGE_SIZE - 1U)) != 0)
		return EINVAL;
	error = vm_object_get_shared(file, &object);
	if (error != 0)
		return error;
	error = map_region(vm, start, size, prot, VM_BACKING_FILE, file,
	    file_offset, start, data_size, VM_REGION_SHARED, 0, &region);
	if (error != 0) {
		vm_object_put(object);
		return error;
	}
	region->object = object;
	if (result != NULL)
		*result = region;
	return 0;
}

int
vmspace_map_stack(struct vmspace *vm, uintptr_t top, size_t size,
		  size_t guard_size)
{
	struct vm_region *guard, *stack;
	uintptr_t bottom, guard_bottom;
	int error;

	if (vm == NULL || vm == &kernel_vmspace || size == 0 ||
	    guard_size == 0 || (top & (PAGE_SIZE - 1U)) != 0 ||
	    (size & (PAGE_SIZE - 1U)) != 0 ||
	    (guard_size & (PAGE_SIZE - 1U)) != 0 || top > vm_layout.user_limit ||
	    size > top || guard_size > top - size)
		return EINVAL;
	bottom = top - size;
	guard_bottom = bottom - guard_size;
	if (!range_valid(guard_bottom, size + guard_size) ||
	    overlaps(vm, guard_bottom, size + guard_size))
		return EINVAL;
	guard = kern_calloc(1, sizeof(*guard));
	if (guard == NULL)
		return ENOMEM;
	stack = kern_calloc(1, sizeof(*stack));
	if (stack == NULL) {
		kern_free(guard);
		return ENOMEM;
	}
	error = vm_commit_reserve(size);
	if (error != 0) {
		kern_free(stack);
		kern_free(guard);
		return error;
	}
	guard->start = guard_bottom;
	guard->size = guard_size;
	guard->backing = VM_BACKING_ANON;
	guard->flags = VM_REGION_GUARD | VM_REGION_IMMUTABLE;
	guard->data_start = guard_bottom;
	stack->start = bottom;
	stack->size = size;
	stack->prot = HAL_SPACE_READ | HAL_SPACE_WRITE;
	stack->flags = VM_REGION_STACK | VM_REGION_IMMUTABLE;
	stack->commit_size = size;
	stack->backing = VM_BACKING_ANON;
	stack->data_start = bottom;
	insert_region(vm, guard);
	insert_region(vm, stack);
	vm->stack_guard_bottom = guard_bottom;
	vm->stack_bottom = bottom;
	vm->stack_top = top;
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
	if (alloc_vm_page(&page->pmem) == HAL_OK)
		return 0;
	if (vm_reclaim_one(page) != 0 ||
	    alloc_vm_page(&page->pmem) != HAL_OK)
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
		if (mapped == HAL_ERR_NOMEM && vm_reclaim_one(page) == 0)
			mapped = hal_page_map(page->vm->space,
				(void *)page->address, page->pmem.paddr, PAGE_SIZE,
				page->region->prot);
		if (mapped != HAL_OK)
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
		return (region->flags & VM_REGION_ELF_ZERO_TAIL) != 0 ? 0 : ENXIO;
	length = read_end - read_start;
	if (region->file_offset < 0 ||
	    (uint64_t)(read_start - region->data_start) >
	    (sizeof(off_t) == sizeof(int64_t) ? (uint64_t)INT64_MAX :
	    (uint64_t)INT32_MAX) - (uint64_t)region->file_offset)
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
	if (region->object != NULL) {
		struct vm_object_page *object_page;
		off_t object_offset;
		int mapped;
		uint64_t maximum_offset = sizeof(off_t) == 8 ?
		    (uint64_t)INT64_MAX : (uint64_t)INT32_MAX;
		if (region->file_offset < 0 ||
		    (uint64_t)region->file_offset +
		    (uint64_t)(page_address - region->start) > maximum_offset)
			return EOVERFLOW;
		object_offset = region->file_offset +
		    (off_t)(page_address - region->start);
		error = vm_object_fault(region->object, object_offset,
		    &object_page);
		if (error != 0)
			return error;
		page = kern_calloc(1, sizeof(*page));
		if (page == NULL) {
			vm_object_fault_release(object_page);
			return ENOMEM;
		}
		page->swap_slot = SWAP_SLOT_NONE;
		page->vm = vm;
		page->region = region;
		page->address = page_address;
		page->object_page = object_page;
		page->pmem = object_page->pmem;
		mapped = hal_page_map(vm->space, (void *)page_address,
		    object_page->pmem.paddr, PAGE_SIZE, region->prot);
		if (mapped != HAL_OK) {
			vm_object_fault_release(object_page);
			kern_free(page);
			return ENOMEM;
		}
		page->flags = VM_PAGE_RESIDENT;
		page->next = region->pages;
		region->pages = page;
		vm_object_mapping_add(object_page, page);
		return 0;
	}
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
		if (mapped == HAL_ERR_NOMEM && vm_reclaim_one(page) == 0)
			mapped = hal_page_map(vm->space, (void *)page_address,
				page->pmem.paddr, PAGE_SIZE, region->prot);
		if (mapped != HAL_OK)
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

	if (vm == NULL || !vmspace_user_range_valid(address, size))
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
			if (page->object_page != NULL)
				vm_object_mark_dirty(page->object_page);
			else
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

static int
find_free_range(struct vmspace *vm, uintptr_t hint, size_t size,
		uintptr_t *mapped)
{
	uintptr_t start, limit;
	struct vm_region *region;

	if (vm == NULL || mapped == NULL || size == 0 ||
	    size > (size_t)(vm_layout.user_limit - vm_layout.mmap_base))
		return EINVAL;
	if (size > SIZE_MAX - (PAGE_SIZE - 1U))
		return EOVERFLOW;
	size = (size + PAGE_SIZE - 1U) & ~(PAGE_SIZE - 1U);
	limit = vm->stack_guard_bottom != 0 ?
		vm->stack_guard_bottom : vm_layout.user_limit;
	start = hint >= vm_layout.mmap_base && (hint & (PAGE_SIZE - 1U)) == 0 ?
		hint : vm_layout.mmap_base;
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
	*mapped = start;
	return 0;
}

int
vmspace_map_find(struct vmspace *vm, uintptr_t hint, size_t size,
		 uint32_t prot, uintptr_t *mapped)
{
	uintptr_t start;
	int error = find_free_range(vm, hint, size, &start);
	if (error == 0)
		error = vmspace_map_anon(vm, start,
		    (size + PAGE_SIZE - 1U) & ~(PAGE_SIZE - 1U), prot, NULL);
	if (error == 0)
		*mapped = start;
	return error;
}

int
vmspace_map_file_find(struct vmspace *vm, uintptr_t hint, size_t size,
		      uint32_t prot, struct file *file, off_t offset,
		      size_t data_size, uintptr_t *mapped)
{
	uintptr_t start;
	size_t rounded;
	int error = find_free_range(vm, hint, size, &start);
	if (error != 0)
		return error;
	rounded = (size + PAGE_SIZE - 1U) & ~(PAGE_SIZE - 1U);
	if (data_size > size)
		data_size = size;
	error = vmspace_map_file(vm, start, rounded, prot, file, offset,
	    start, data_size, NULL);
	if (error == 0)
		*mapped = start;
	return error;
}

int
vmspace_map_file_shared_find(struct vmspace *vm, uintptr_t hint, size_t size,
			     uint32_t prot, struct file *file, off_t offset,
			     size_t data_size, uintptr_t *mapped)
{
	uintptr_t start;
	size_t rounded;
	int error = find_free_range(vm, hint, size, &start);
	if (error != 0)
		return error;
	rounded = (size + PAGE_SIZE - 1U) & ~(PAGE_SIZE - 1U);
	if (data_size > size)
		data_size = size;
	error = vmspace_map_file_shared(vm, start, rounded, prot, file, offset,
	    data_size, NULL);
	if (error == 0)
		*mapped = start;
	return error;
}

static void
free_vm_page(struct vmspace *vm, struct vm_page *page)
{
	if (page->object_page != NULL) {
		(void)hal_page_unmap(vm->space, (void *)page->address, PAGE_SIZE);
		vm_object_mapping_remove(page->object_page, page);
		kern_free(page);
		return;
	}
	vm_page_untrack(page);
	if (page->flags & VM_PAGE_RESIDENT) {
		(void)hal_page_unmap(vm->space, (void *)page->address, PAGE_SIZE);
		(void)hal_pmem_free(&page->pmem);
	}
	if ((page->flags & VM_PAGE_SWAPPED) && swap_system_backend() != NULL)
		swap_free_slot(swap_system_backend(), page->swap_slot);
	kern_free(page);
}

static void
free_region_pages(struct vmspace *vm, struct vm_region *region)
{
	struct vm_page *page;

	while ((page = region->pages) != NULL) {
		region->pages = page->next;
		free_vm_page(vm, page);
	}
}

static int
split_region(struct vm_region *region, uintptr_t address)
{
	struct vm_region *right;
	struct vm_page **link;
	size_t left_size, right_size, data_skip;

	if (region == NULL || address <= region->start ||
	    address >= region->start + region->size ||
	    (address & (PAGE_SIZE - 1U)) != 0)
		return EINVAL;
	/* ELF mappings may have an unaligned data subrange, but they are immutable
	 * and never reach this helper.  User mmap regions use data_start=start. */
	if (region->flags & VM_REGION_IMMUTABLE ||
	    region->data_start != region->start)
		return EACCES;
	right = kern_calloc(1, sizeof(*right));
	if (right == NULL)
		return ENOMEM;
	left_size = address - region->start;
	right_size = region->size - left_size;
	*right = *region;
	right->start = address;
	right->size = right_size;
	right->data_start = address;
	data_skip = left_size < region->data_size ? left_size : region->data_size;
	right->data_size = region->data_size - data_skip;
	if (right->data_size > right_size)
		right->data_size = right_size;
	if (region->backing == VM_BACKING_FILE)
		right->file_offset += (off_t)data_skip;
	right->commit_size = region->commit_size != 0 ? right_size : 0;
	right->pages = NULL;
	if (right->file != NULL)
		file_ref(right->file);
	if (right->object != NULL)
		vm_object_ref(right->object);
	region->size = left_size;
	if (region->data_size > left_size)
		region->data_size = left_size;
	region->commit_size = region->commit_size != 0 ? left_size : 0;
	for (link = &region->pages; *link != NULL; ) {
		struct vm_page *page = *link;
		if (page->address < address) {
			link = &page->next;
			continue;
		}
		*link = page->next;
		page->region = right;
		page->next = right->pages;
		right->pages = page;
	}
	right->next = region->next;
	region->next = right;
	return 0;
}

int
vmspace_set_brk_start(struct vmspace *vm, uintptr_t start)
{
	if (vm == NULL || vm == &kernel_vmspace || vm->brk_start != 0 ||
	    (start & (PAGE_SIZE - 1U)) != 0 ||
	    start < vm_layout.user_minimum || start >= vm_layout.brk_limit ||
	    overlaps(vm, start, PAGE_SIZE))
		return EINVAL;
	vm->brk_start = start;
	vm->brk_current = start;
	return 0;
}

int
vmspace_brk(struct vmspace *vm, uintptr_t requested, uintptr_t *result)
{
	struct vm_region **link, *region = NULL;
	uintptr_t old_end, new_end;
	size_t difference;
	int error;

	if (vm == NULL || vm == &kernel_vmspace || result == NULL ||
	    vm->brk_start == 0)
		return EINVAL;
	if (requested == 0) {
		*result = vm->brk_current;
		return 0;
	}
	if (requested < vm->brk_start || requested >= vm_layout.brk_limit)
		return ENOMEM;
	old_end = (vm->brk_current + PAGE_SIZE - 1U) &
		~(uintptr_t)(PAGE_SIZE - 1U);
	new_end = (requested + PAGE_SIZE - 1U) &
		~(uintptr_t)(PAGE_SIZE - 1U);
	for (link = &vm->regions; *link != NULL; link = &(*link)->next)
		if (((*link)->flags & VM_REGION_BRK) != 0) {
			region = *link;
			break;
		}
	if (new_end > old_end) {
		difference = new_end - old_end;
		if (region == NULL) {
			error = map_region(vm, vm->brk_start,
				new_end - vm->brk_start,
				HAL_SPACE_READ | HAL_SPACE_WRITE, VM_BACKING_ANON,
				NULL, 0, vm->brk_start, 0,
				VM_REGION_BRK | VM_REGION_IMMUTABLE,
				new_end - vm->brk_start, &region);
			if (error != 0)
				return error;
		} else {
			if (region->start + region->size != old_end ||
			    overlaps(vm, old_end, difference))
				return ENOMEM;
			error = vm_commit_reserve(difference);
			if (error != 0)
				return error;
			region->size += difference;
			region->commit_size += difference;
		}
	} else if (new_end < old_end) {
		struct vm_page **page_link;
		if (region == NULL || region->start + region->size != old_end)
			return EINVAL;
		for (page_link = &region->pages; *page_link != NULL; ) {
			struct vm_page *page = *page_link;
			if (page->address < new_end) {
				page_link = &page->next;
				continue;
			}
			*page_link = page->next;
			free_vm_page(vm, page);
		}
		difference = old_end - new_end;
		vm_commit_release(difference);
		region->size -= difference;
		region->commit_size -= difference;
		if (region->size == 0) {
			*link = region->next;
			kern_free(region);
		}
	}
	vm->brk_current = requested;
	*result = requested;
	return 0;
}

int
vmspace_unmap(struct vmspace *vm, uintptr_t start, size_t size)
{
	struct vm_region **link, *region;
	uintptr_t end;
	int error;

	if (vm == NULL || vm == &kernel_vmspace || !range_valid(start, size))
		return EINVAL;
	end = start + size;
	/* Validate the complete transaction before split_region mutates metadata. */
	for (region = vm->regions; region != NULL; region = region->next) {
		struct vm_page *page;
		if (region->start >= end || region->start + region->size <= start)
			continue;
		if ((region->flags & VM_REGION_IMMUTABLE) != 0)
			return EACCES;
		for (page = region->pages; page != NULL; page = page->next)
			if (page->address < end && page->address + PAGE_SIZE > start &&
			    page->wire_count != 0)
				return EBUSY;
	}
	region = vmspace_find_region(vm, end - 1U, 1);
	if (region != NULL && end < region->start + region->size) {
		error = split_region(region, end);
		if (error != 0)
			return error;
	}
	region = vmspace_find_region(vm, start, 1);
	if (region != NULL && start > region->start) {
		error = split_region(region, start);
		if (error != 0)
			return error;
	}
	for (link = &vm->regions; *link != NULL; link = &(*link)->next)
		if ((*link)->start >= start)
			break;
	while ((region = *link) != NULL && region->start < end) {
		*link = region->next;
		free_region_pages(vm, region);
		if (region->file != NULL)
			(void)file_close(region->file);
		if (region->object != NULL)
			vm_object_put(region->object);
		if (region->commit_size != 0)
			vm_commit_release(region->commit_size);
		kern_free(region);
	}
	return 0;
}

int
vmspace_protect(struct vmspace *vm, uintptr_t start, size_t size,
		uint32_t prot)
{
	struct vm_region *region, *first, *end_region;
	struct vm_page *page, *failed_page = NULL;
	struct vm_region *failed_region = NULL;
	uintptr_t end, covered;
	size_t new_commit = 0;
	int error;

	if (vm == NULL || vm == &kernel_vmspace || !range_valid(start, size) ||
	    (prot & ~(HAL_SPACE_READ | HAL_SPACE_WRITE | HAL_SPACE_EXEC)) != 0)
		return EINVAL;
	end = start + size;
	covered = start;
	for (region = vm->regions; region != NULL && covered < end;
	     region = region->next) {
		if (covered < region->start)
			return EINVAL;
		if (covered >= region->start + region->size)
			continue;
		if (region->flags & VM_REGION_IMMUTABLE)
			return EACCES;
		covered = region->start + region->size;
		if (covered > end)
			covered = end;
	}
	if (covered != end)
		return EINVAL;
	end_region = vmspace_find_region(vm, end - 1U, 1);
	if (end_region != NULL && end < end_region->start + end_region->size) {
		error = split_region(end_region, end);
		if (error != 0)
			return error;
	}
	region = vmspace_find_region(vm, start, 1);
	if (start > region->start) {
		error = split_region(region, start);
		if (error != 0)
			return error;
		region = region->next;
	}
	first = region;
	for (; region != NULL && region->start < end; region = region->next)
		if (region->commit_size == 0 && region->object == NULL &&
		    ((region->backing == VM_BACKING_ANON && prot != 0) ||
		     (region->backing == VM_BACKING_FILE &&
		      (prot & HAL_SPACE_WRITE) != 0))) {
			if (new_commit > SIZE_MAX - region->size)
				return EOVERFLOW;
			new_commit += region->size;
		}
	if (new_commit != 0) {
		error = vm_commit_reserve(new_commit);
		if (error != 0)
			return error;
	}
	for (region = first; region != NULL && region->start < end;
	     region = region->next) {
		for (page = region->pages; page != NULL; page = page->next)
			if ((page->flags & VM_PAGE_RESIDENT) &&
			    hal_page_prot(vm->space, (void *)page->address,
				PAGE_SIZE, prot) != HAL_OK) {
				failed_region = region;
				failed_page = page;
				goto rollback;
			}
	}
	for (region = first; region != NULL && region->start < end;
	     region = region->next) {
		region->prot = prot;
		if (region->commit_size == 0 && region->object == NULL &&
		    ((region->backing == VM_BACKING_ANON && prot != 0) ||
		     (region->backing == VM_BACKING_FILE &&
		      (prot & HAL_SPACE_WRITE) != 0)))
			region->commit_size = region->size;
	}
	return 0;

rollback:
	for (region = first; region != NULL && region->start < end;
	     region = region->next) {
		struct vm_page *rollback;
		for (rollback = region->pages; rollback != NULL;
		     rollback = rollback->next) {
			if (region == failed_region && rollback == failed_page)
				break;
			if ((rollback->flags & VM_PAGE_RESIDENT) &&
			    hal_page_prot(vm->space, (void *)rollback->address,
				PAGE_SIZE, region->prot) != HAL_OK)
				HAL_FATAL("VM protection rollback failed");
		}
		if (region == failed_region)
			break;
	}
	if (new_commit != 0)
		vm_commit_release(new_commit);
	return EINVAL;
}

int
vmspace_sync(struct vmspace *vm, uintptr_t start, size_t size, int flags)
{
	uintptr_t current, end;
	struct vm_region *region;
	if (vm == NULL || vm == &kernel_vmspace || !range_valid(start, size) ||
	    (flags & ~(MS_ASYNC | MS_INVALIDATE | MS_SYNC)) != 0 ||
	    (flags & (MS_ASYNC | MS_SYNC)) == 0 ||
	    (flags & (MS_ASYNC | MS_SYNC)) == (MS_ASYNC | MS_SYNC))
		return EINVAL;
	current = start;
	end = start + size;
	for (region = vm->regions; region != NULL && current < end;
	     region = region->next) {
		if (current < region->start)
			return ENOMEM;
		if (current >= region->start + region->size)
			continue;
		{
			uintptr_t region_end = region->start + region->size;
			uintptr_t overlap_end = region_end < end ? region_end : end;
			if (region->object != NULL) {
				off_t offset = region->file_offset +
				    (off_t)(current - region->start);
				int error = vm_object_sync_range(region->object, offset,
				    overlap_end - current, flags);
				if (error != 0)
					return error;
			}
			current = overlap_end;
		}
	}
	return current == end ? 0 : ENOMEM;
}

void
vmspace_free(struct vmspace *vm)
{
	struct vm_region *region;

	if (vm == NULL || vm == &kernel_vmspace)
		return;
	if (!refcount_put(&vm->refs))
		return;
	while ((region = vm->regions) != NULL) {
		vm->regions = region->next;
		free_region_pages(vm, region);
		if (region->file != NULL)
			(void)file_close(region->file);
		if (region->object != NULL)
			vm_object_put(region->object);
		if (region->commit_size != 0)
			vm_commit_release(region->commit_size);
		kern_free(region);
	}
	hal_page_destroy_space(vm->space);
	(void)atomic_raw_fetch_add_relaxed(&vmspace_live.value, (unsigned)-1);
	kern_free(vm);
}

unsigned vmspace_count(void)
{ return atomic_load_acquire(&vmspace_live); }
