/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Removal and change of mappings: unmap, protect, brk, the address,
 * data and stack limits, and the revocation of object pages.
 */

#include "kern/vmspace.h"
#include "kern/vm-device.h"
#include "kern/file.h"
#include "kern/kmem.h"
#include "kern/lock.h"
#include "kern/page.h"
#include "kern/sched.h"
#include "kern/swap.h"
#include "kern/vm-commit.h"
#include "kern/vm-lock.h"
#include "kern/vm-object.h"
#include "kern/vm-reclaim.h"
#include <uapi/errno.h>
#include <uapi/mman.h>
#include <kern/kcrt.h>
#include "kern/clock.h"
#include "vmspace-internal.h"

static void free_vm_page(struct vmspace *vm, struct vm_page *page, int unmap);
static void detach_vm_page_for_unmap(struct vmspace *vm, struct vm_page *page);
static void object_mapping_note_dirty(struct vm_page *page);
static void detach_region_pages_for_unmap(struct vmspace *vm, struct vm_region *region);
static int split_region_prepared(struct vm_region *region, uintptr_t address, struct vm_region *right);
static int split_region(struct vm_region *region, uintptr_t address);
static void release_retired_regions(struct vm_region *retired);
static int vmspace_set_brk_start_locked(struct vmspace *vm, uintptr_t start, uint64_t static_data_bytes);
static int vmspace_brk_locked(struct vmspace *vm, uintptr_t requested, uintptr_t *result);
static int vmspace_unmap_locked(struct vmspace *vm, uintptr_t start, size_t size, struct vm_region **retired);
static int vmspace_protect_locked(struct vmspace *vm, uintptr_t start, size_t size, uint32_t prot);
static int vmspace_set_address_limit_locked(struct vmspace *vm, uint64_t limit);
static void vmspace_set_stack_limit_locked(struct vmspace *vm, uint64_t limit);
static int vmspace_set_data_limit_locked(struct vmspace *vm, uint64_t limit);

/*
 * Revokes every process mapping of a BUSY shared object page and reports
 * the accessed and dirty state observed while doing so.
 *
 * A shared-file content writer first makes its object page BUSY.
 * Existing fault holds drain before this routine is called, so no new
 * reverse mapping can appear until the writer publishes the new cache
 * bytes.  Each mapping is then pinned in metadata, revoked with no
 * VM/object lock held, and finally detached.  hal_space_prot_query()
 * returns A/D state only after the remote TLB acknowledgement, closing
 * the late-store window between a dirty snapshot and unmap.  A
 * write-only mapping cannot be represented read-only by the current HAL
 * contract; conservatively treating it as dirty is safe.
 */
int
vmspace_object_page_revoke(
	struct vm_object_page *object_page,
	uint32_t *observed_flags)
{
	struct vm_object *object;
	uint32_t observed;
	struct vm_page *mapping;
	struct vm_region *region;
	struct vmspace *vm;
	uint32_t readonly;
	unsigned long irq;
	int error;
	int was_mapped;
	int acquired;
	uint64_t sequence;
	struct vm_page **link;
	uint32_t flags;

	observed = 0;

	/* Rejects a page without an owning object. */
	if (object_page == NULL)
		return EINVAL;

	object = object_page->owner;
	if (object == NULL)
		return EINVAL;

	/* Revokes one mapping of the page at a time. */
	for (;;) {
		/* Takes the next mapping while the page is still BUSY. */
		vm_metadata_enter();

		irq = spin_lock_irqsave(&object->lock);

		if ((object_page->flags & VM_OBJECT_PAGE_BUSY) == 0) {
			spin_unlock_irqrestore(&object->lock, irq);
			vm_metadata_leave();
			return EBUSY;
		}

		/* Stops once no mapping of the page is left to revoke. */
		mapping = object_page->mappings;
		if (mapping == NULL) {
			spin_unlock_irqrestore(&object->lock, irq);
			vm_metadata_leave();
			break;
		}

		vm = mapping->vm;
		region = mapping->region;
		if (vm == NULL || region == NULL)
			HAL_FATAL("invalid shared VM reverse mapping");

		/*
		 * A space whose last reference is gone is being torn down: its
		 * threads are gone, and vmspace_destroy() takes its mappings
		 * off the page region by region, between which it leaves the
		 * metadata lock.  The mapping is waited out rather than revoked.
		 */
		acquired = vmspace_tryref(vm);
		if (!acquired) {
			spin_unlock_irqrestore(&object->lock, irq);
			vm_metadata_leave();
			sched_yield();
			continue;
		}

		spin_unlock_irqrestore(&object->lock, irq);

		/* Pins the mapping in metadata. */
		mutex_lock(&vm->lock);
		irq = spin_lock_irqsave(&object->lock);

		if (mapping->object_page != object_page ||
		    mapping->vm != vm ||
		    mapping->region != region)
			HAL_FATAL("shared VM reverse mapping changed while pinned");

		/*
		 * A copy-on-write fault holds the mapping reserved.  It gives the
		 * reservation up rather than wait for this page, so it is waited
		 * out (BUG-082).
		 */
		if ((mapping->flags & VM_MAPPING_BUSY) != 0) {
			sequence = waitq_sequence(&vm->fault_waitq);
			spin_unlock_irqrestore(&object->lock, irq);
			mutex_unlock(&vm->lock);
			vm_metadata_leave();
			vmspace_wait_fault_event(vm, sequence);
			vmspace_put_deferred(vm);
			continue;
		}

		mapping->flags |= VM_MAPPING_BUSY;
		region->hold_count++;
		readonly = region->prot & ~HAL_SPACE_WRITE;
		was_mapped = (mapping->flags & VM_MAPPING_MAPPED) != 0;

		spin_unlock_irqrestore(&object->lock, irq);
		mutex_unlock(&vm->lock);
		vm_metadata_leave();

		/* Revokes the hardware mapping with no lock held. */
		if (was_mapped) {
			flags = 0;
			if (readonly != 0) {
				error = hal_space_prot_query(vm->space,
							    (void *)mapping->address,
							    PAGE_SIZE,
							    readonly,
							    &flags);
				if (error != HAL_OK)
					HAL_FATAL("shared VM write revoke failed");
			} else {
				/* The synchronous unmap is the revoke operation. */
				flags |= HAL_SPACE_PAGE_DIRTY;
			}

			if (hal_space_unmap(vm->space,
					   (void *)mapping->address,
					   PAGE_SIZE) != HAL_OK)
				HAL_FATAL("shared VM content unmap failed");

			observed |= flags;
		}

		/* Detaches the mapping from the region and the object page. */
		vm_metadata_enter();
		mutex_lock(&vm->lock);
		irq = spin_lock_irqsave(&object->lock);

		if (mapping->object_page != object_page ||
		    (mapping->flags & VM_MAPPING_BUSY) == 0)
			HAL_FATAL("shared VM revoke reservation lost");

		/*
		 * Walks the region's mapping list to the link that refers to
		 * this mapping, keeping the address of the link so it can be
		 * unlinked without a second search.
		 */
		link = &region->pages;
		while (*link != NULL && *link != mapping)
			link = &(*link)->next;
		if (*link != mapping)
			HAL_FATAL("shared VM mapping left region");
		*link = mapping->next;
		vmspace_region_page_index_remove(region, mapping);

		/* Retires the mapping and gives back the region hold it took. */
		mapping->flags &= ~(VM_MAPPING_MAPPED | VM_MAPPING_BUSY);
		vm_object_mapping_remove_locked(object_page, mapping);
		mapping->object_page = NULL;
		if (region->hold_count == 0)
			HAL_FATAL("shared VM revoke region hold underflow");
		region->hold_count--;

		vmspace_generation_advance_locked(vm);

		waitq_wake_all(&object->page_waitq);
		spin_unlock_irqrestore(&object->lock, irq);
		vmspace_fault_wake_locked(vm);
		mutex_unlock(&vm->lock);
		vm_metadata_leave();

		/*
		 * The space may have ended meanwhile.  Its teardown syncs the
		 * objects it maps, which waits for the pages this caller holds
		 * BUSY, so the last reference goes to the reaper (BUG-082).
		 */
		vm_page_free_metadata(mapping);
		vmspace_put_deferred(vm);
	}

	if (observed_flags != NULL)
		*observed_flags = observed;

	return 0;
}

/*
 * Records where the program break starts and how much static data
 * counts against the data limit.
 */
int
vmspace_set_brk_start(
	struct vmspace *vm,
	uintptr_t start,
	uint64_t static_data_bytes)
{
	int error;

	if (vm == NULL || vm == &kernel_vmspace)
		return EINVAL;

	/* Runs the mapping under the metadata and space locks. */
	vm_metadata_enter();
	mutex_lock(&vm->lock);

	error = vmspace_set_brk_start_locked(vm, start, static_data_bytes);
	if (error == 0)
		vmspace_generation_advance_locked(vm);

	mutex_unlock(&vm->lock);
	vm_metadata_leave();

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Moves the program break, or reports it when the request is zero.
 */
int
vmspace_brk(
	struct vmspace *vm,
	uintptr_t requested,
	uintptr_t *result)
{
	int error;

	if (vm == NULL || vm == &kernel_vmspace)
		return EINVAL;

	/* Runs the mapping under the metadata and space locks. */
	vm_metadata_enter();
	mutex_lock(&vm->lock);

	error = vmspace_brk_locked(vm, requested, result);
	if (error == 0 && requested != 0)
		vmspace_generation_advance_locked(vm);

	mutex_unlock(&vm->lock);
	vm_metadata_leave();

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Unmaps a user range, retiring its regions outside the locks.
 */
int
vmspace_unmap(
	struct vmspace *vm,
	uintptr_t start,
	size_t size)
{
	struct vm_region *retired;
	int error;

	retired = NULL;

	if (vm == NULL || vm == &kernel_vmspace)
		return EINVAL;

	/* Runs the mapping under the metadata and space locks. */
	vm_metadata_enter();
	mutex_lock(&vm->lock);

	error = vmspace_unmap_locked(vm, start, size, &retired);
	if (error == 0)
		vmspace_generation_advance_locked(vm);

	mutex_unlock(&vm->lock);
	vm_metadata_leave();

	release_retired_regions(retired);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Changes the protection of a user range.
 */
int
vmspace_protect(
	struct vmspace *vm,
	uintptr_t start,
	size_t size,
	uint32_t prot)
{
	int error;

	if (vm == NULL || vm == &kernel_vmspace)
		return EINVAL;

	/* Runs the mapping under the metadata and space locks. */
	vm_metadata_enter();
	mutex_lock(&vm->lock);

	error = vmspace_protect_locked(vm, start, size, prot);
	if (error == 0)
		vmspace_generation_advance_locked(vm);

	mutex_unlock(&vm->lock);
	vm_metadata_leave();

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Sets the limit on mapped virtual bytes.
 */
int
vmspace_set_address_limit(
	struct vmspace *vm,
	uint64_t limit)
{
	int error;

	if (vm == NULL || vm == &kernel_vmspace)
		return EINVAL;

	/* Runs the mapping under the metadata and space locks. */
	vm_metadata_enter();
	mutex_lock(&vm->lock);

	error = vmspace_set_address_limit_locked(vm, limit);
	if (error == 0)
		vmspace_generation_advance_locked(vm);

	mutex_unlock(&vm->lock);
	vm_metadata_leave();

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Sets the limit on data segment growth.
 */
int
vmspace_set_data_limit(
	struct vmspace *vm,
	uint64_t limit)
{
	int error;

	if (vm == NULL || vm == &kernel_vmspace)
		return EINVAL;

	/* Runs the mapping under the metadata and space locks. */
	vm_metadata_enter();
	mutex_lock(&vm->lock);

	error = vmspace_set_data_limit_locked(vm, limit);
	if (error == 0)
		vmspace_generation_advance_locked(vm);

	mutex_unlock(&vm->lock);
	vm_metadata_leave();

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Sets the limit on stack size.
 */
void
vmspace_set_stack_limit(
	struct vmspace *vm,
	uint64_t limit)
{
	if (vm == NULL || vm == &kernel_vmspace)
		return;

	vm_metadata_enter();
	mutex_lock(&vm->lock);

	vmspace_set_stack_limit_locked(vm, limit);
	vmspace_generation_advance_locked(vm);

	mutex_unlock(&vm->lock);
	vm_metadata_leave();
}

/*
 * Removes a page's reverse mappings, keeping a reference on its private
 * backing as a retire token for vmspace_release_detached_region_pages(); the
 * caller holds the VM locks.
 */
void
vmspace_detach_page(
	struct vm_page *page)
{
	struct vm_private_page *backing;

	/*
	 * An object mapping only leaves the object's reverse mapping list,
	 * once a writable one has left its stores to be written back (a space
	 * torn down at exit comes here with its hardware mappings kept).
	 */
	if (page->object_page != NULL) {
		object_mapping_note_dirty(page);
		vm_object_mapping_remove(page->object_page, page);
		page->object_page = NULL;
		page->object_next = NULL;
		return;
	}

	/*
	 * A private backing can own physical memory or a swap slot.  Take a
	 * temporary reference before vm_page_untrack() so that the
	 * reverse-map detach cannot be the operation which frees those
	 * resources.  The detached vm_page retains that reference solely as
	 * a retire token; vmspace_release_detached_region_pages() drops it after
	 * the address has been unpublished and both locks are gone.
	 */
	backing = page->private_page;
	if (backing == NULL)
		return;
	vm_private_page_ref(backing);
	vm_page_untrack(page);
	page->private_page = backing;
	page->private_next = NULL;
}

/*
 * Frees the detached pages of a retired region outside the locks.
 */
void
vmspace_release_detached_region_pages(
	struct vm_region *region)
{
	struct vm_page *page;

	/* Drops the index; the pages go one by one. */
	vmspace_region_page_index_free(region);
	region->page_count = 0;
	page = region->pages;
	while (page != NULL) {
		region->pages = page->next;
		/* Drops the private-backing retire token, if there is one. */
		if (page->private_page != NULL)
			vm_private_page_put(page->private_page);
		vm_page_free_metadata(page);
		page = region->pages;
	}
}

/*
 * Publishes a prepared region at an exact address, retiring what it overlaps.
 */
int
vmspace_replace_prepared(
	struct vmspace *vm,
	struct vm_region *prepared,
	struct vm_region **result)
{
	struct vm_region *split_first;
	struct vm_region *split_second;
	struct vm_region *retired;
	struct vm_region *region;
	struct vm_region **link;
	uintptr_t start;
	uintptr_t end;
	size_t removed;
	int error;
	struct vm_page *page;
	uintptr_t overlap_start;
	uintptr_t overlap_end;
	struct vm_region *split;
	uint64_t mapped_bytes;

	retired = NULL;
	removed = 0;
	error = 0;

	/*
	 * Two preallocated split records make the metadata commit
	 * failure-free after the first old region is changed.
	 */
	if (vm == NULL ||
	    vm == &kernel_vmspace ||
	    prepared == NULL ||
	    !vmspace_range_valid(prepared->start, prepared->size))
		return EINVAL;

	/* Preallocates both possible splits, because the commit may not fail. */
	split_first = kern_calloc(1, sizeof(*split_first));
	split_second = kern_calloc(1, sizeof(*split_second));
	if (split_first == NULL || split_second == NULL) {
		kern_free(split_first);
		kern_free(split_second);
		return ENOMEM;
	}

	start = prepared->start;
	end = start + prepared->size;
	vm_metadata_enter();
	mutex_lock(&vm->lock);

	vmspace_wait_faults_locked(vm);

	/* Validates every overlapped region and sums the bytes removed. */
	for (region = vm->regions; region != NULL; region = region->next) {
		if (region->start >= end || region->start + region->size <= start)
			continue;
		if ((region->flags & VM_REGION_IMMUTABLE) != 0) {
			error = EINVAL;
			goto out_locked;
		}

		if (region->hold_count != 0) {
			error = EAGAIN;
			goto out_locked;
		}

		/* Refuses the replacement while any page of the range is wired. */
		for (page = region->pages; page != NULL; page = page->next) {
			if (page->address < end &&
			    page->address + PAGE_SIZE > start &&
			    page->wire_count != 0) {
				error = EAGAIN;
				goto out_locked;
			}
		}

		/* Counts how much mapped address space the replacement frees. */
		if (region->start > start)
			overlap_start = region->start;
		else
			overlap_start = start;
		if (region->start + region->size < end)
			overlap_end = region->start + region->size;
		else
			overlap_end = end;
		removed += overlap_end - overlap_start;
	}

	if (removed > vm->mapped_virtual_bytes) {
		error = ENOMEM;
		goto out_locked;
	}

	/* Refuses a replacement that would exceed the address-space limit. */
	mapped_bytes = vm->mapped_virtual_bytes - removed;
	if (mapped_bytes > vm->address_limit)
		mapped_bytes = vm->address_limit;
	if ((uint64_t)prepared->size > vm->address_limit - mapped_bytes) {
		error = ENOMEM;
		goto out_locked;
	}

	/* Splits the regions that straddle either end of the range. */
	region = vmspace_find_region_locked(vm, end - 1U, 1);
	if (region != NULL && end < region->start + region->size) {
		error = split_region_prepared(region, end, split_first);
		if (error != 0)
			goto out_locked;
		split_first = NULL;
	}

	/* Splits the region that straddles the start of the range. */
	region = vmspace_find_region_locked(vm, start, 1);
	if (region != NULL && start > region->start) {
		if (split_first != NULL)
			split = split_first;
		else
			split = split_second;
		error = split_region_prepared(region, start, split);
		if (error != 0)
			HAL_FATAL("prepared VM split commit failed");
		if (split == split_first)
			split_first = NULL;
		else
			split_second = NULL;
	}

	/* Detaches the old mappings, unlinks the old regions, and links the new one. */
	for (region = vm->regions; region != NULL && region->start < end;
	     region = region->next) {
		if (region->start >= start)
			detach_region_pages_for_unmap(vm, region);
	}

	for (link = &vm->regions; *link != NULL; link = &(*link)->next) {
		if ((*link)->start >= start)
			break;
	}

	region = *link;
	while (region != NULL && region->start < end) {
		*link = region->next;
		vm->mapped_virtual_bytes -= region->size;
		region->next = retired;
		retired = region;
		region = *link;
	}

	vmspace_insert_region(vm, prepared);
	vm->mapped_virtual_bytes += prepared->size;
	vmspace_generation_advance_locked(vm);
	if (result != NULL)
		*result = prepared;

out_locked:
	/* Leaves the locks and frees the split records the pass reserved. */

	mutex_unlock(&vm->lock);
	vm_metadata_leave();

	kern_free(split_first);
	kern_free(split_second);

	/* Retires the replaced regions outside every lock. */
	release_retired_regions(retired);

	/* Reports why the replacement failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Frees one page record during teardown, unmapping it when asked.
 *
 * A space about to be destroyed keeps its hardware mappings: the HAL
 * releases every page table of the space at once and flushes the
 * translations then, so unmapping each page first would only walk the
 * tables once more for every page.
 */
static void
free_vm_page(
	struct vmspace *vm,
	struct vm_page *page,
	int unmap)
{
	/* An object page only drops its reverse mapping. */
	if (page->object_page != NULL) {
		object_mapping_note_dirty(page);
		if (unmap && (page->flags & VM_MAPPING_MAPPED) != 0)
			(void)hal_space_unmap(vm->space, (void *)page->address, PAGE_SIZE);
		vm_object_mapping_remove(page->object_page, page);
		vm_page_free_metadata(page);
		return;
	}

	/* A private page leaves the hardware and its backing's mapping list. */
	if (unmap && (page->flags & VM_MAPPING_MAPPED) != 0)
		(void)hal_space_unmap(vm->space, (void *)page->address, PAGE_SIZE);
	vm_page_untrack(page);
	vm_page_free_metadata(page);
}

/* Removes a page's hardware and reverse mappings for unmap; the caller holds the VM locks. */
static void
detach_vm_page_for_unmap(
	struct vmspace *vm,
	struct vm_page *page)
{
	int unmapped;

	/*
	 * vmspace_unmap() has two teardown phases.  This first phase runs
	 * while both the global VM metadata lock and the vmspace lock are
	 * held.  It must remove every hardware and reverse mapping before
	 * the region list publishes the virtual address as free.
	 */
	object_mapping_note_dirty(page);
	if ((page->flags & VM_MAPPING_MAPPED) != 0) {
		unmapped = hal_space_unmap(vm->space, (void *)page->address, PAGE_SIZE);
		if (unmapped != HAL_OK)
			HAL_FATAL("VM unmap commit failed");

		/* MAPPED tells later teardown that the hardware mapping is gone. */
		page->flags &= ~VM_MAPPING_MAPPED;
	}

	/* The reverse mappings. */
	vmspace_detach_page(page);
}

/*
 * Records that a writable shared mapping may have changed its object page,
 * before the mapping goes away.
 *
 * A store through a shared file mapping sets only the hardware dirty bit;
 * the object page learns of it when a sync revokes the mapping.  Unmapping
 * the page, or tearing the whole space down at exit, used to drop the
 * mapping without looking, so a file written through a mapping and then
 * unmapped without msync() kept its new contents only in the cache and
 * never reached the filesystem (BUG-026: ld.lld's output, empty on disk).
 * The page is taken as dirty whenever the mapping could write it; a page
 * that was only read is written back once more, which is harmless.
 */
static void
object_mapping_note_dirty(
	struct vm_page *page)
{
	struct vm_region *region;

	/* Only a mapping of an object page can dirty the file's cache. */
	if (page->object_page == NULL)
		return;
	if ((page->flags & VM_MAPPING_MAPPED) == 0)
		return;

	/* Only a shared writable mapping stores into the object page itself. */
	region = page->region;
	if (region == NULL)
		return;
	if ((region->flags & VM_REGION_SHARED) == 0)
		return;
	if ((region->prot & HAL_SPACE_WRITE) == 0)
		return;
	if ((page->flags & VM_MAPPING_COW) != 0)
		return;

	/* Leaves the page for the next write-back. */
	vm_object_mark_dirty(page->object_page);
}

/* Detaches every page of a region for unmap; the caller holds the VM locks. */
static void
detach_region_pages_for_unmap(
	struct vmspace *vm,
	struct vm_region *region)
{
	struct vm_page *page;

	/* Detaches each page of the region. */
	for (page = region->pages;
	     page != NULL;
	     page = page->next) {
		detach_vm_page_for_unmap(vm, page);
	}
}

/* Splits a region at an address into itself and a preallocated right half. */
static int
split_region_prepared(
	struct vm_region *region,
	uintptr_t address,
	struct vm_region *right)
{
	struct vm_page **link;
	size_t left_size;
	size_t right_size;
	uintptr_t original_data_start;
	uintptr_t original_data_end;
	uintptr_t left_data_start;
	uintptr_t left_data_end;
	uintptr_t right_data_start;
	uintptr_t right_data_end;
	uintptr_t region_end;
	struct vm_page *page;

	/* Rejects a split outside the region or of a busy or immutable one. */
	if (region == NULL ||
	    right == NULL ||
	    address <= region->start ||
	    address >= region->start + region->size ||
	    (address & (PAGE_SIZE - 1U)) != 0)
		return EINVAL;
	if (region->hold_count != 0)
		return EBUSY;
	if (region->flags & VM_REGION_IMMUTABLE)
		return EACCES;

	/* The right half inherits the region and clips the data window. */
	left_size = address - region->start;
	right_size = region->size - left_size;
	original_data_start = region->data_start;
	original_data_end = region->data_start + region->data_size;
	region_end = region->start + region->size;
	*right = *region;

	/* The right half starts with no pages and no index of its own. */
	right->pages = NULL;
	right->page_index = NULL;
	right->page_index_size = 0;
	right->page_index_retry = 0;
	right->page_count = 0;
	right->start = address;
	right->size = right_size;

	if (original_data_start > address)
		right_data_start = original_data_start;
	else
		right_data_start = address;

	if (original_data_end < region_end)
		right_data_end = original_data_end;
	else
		right_data_end = region_end;

	if (right_data_end < right_data_start)
		right_data_end = right_data_start;

	right->data_start = right_data_start;
	right->data_size = right_data_end - right_data_start;

	if (region->backing == VM_BACKING_FILE &&
	    right_data_start > original_data_start)
		right->file_offset += (off_t)(right_data_start - original_data_start);

	if (region->commit_size != 0)
		right->commit_size = right_size;
	else
		right->commit_size = 0;

	right->pages = NULL;

	if (right->file != NULL)
		file_ref(right->file);

	if (right->object != NULL)
		vm_object_ref(right->object);

	if (right->snapshot != NULL)
		file_exec_snapshot_ref(right->snapshot);

	/* A split keeps both aliases on the same backing with distinct byte offsets. */
	if (right->device != NULL) {
		vm_device_ref(right->device);
		right->device_offset += left_size;
	}

	/* The left half keeps whatever data lies before the split. */
	region->size = left_size;
	if (original_data_start < address &&
	    original_data_end > region->start) {
		left_data_start = original_data_start;
		if (original_data_end < address)
			left_data_end = original_data_end;
		else
			left_data_end = address;
	} else {
		left_data_start = region->start;
		left_data_end = left_data_start;
	}

	/* Gives the left half its share of the data range and the commitment. */
	region->data_start = left_data_start;
	region->data_size = left_data_end - left_data_start;

	if (region->commit_size != 0)
		region->commit_size = left_size;
	else
		region->commit_size = 0;

	/* Moves the pages at or above the split to the right half. */
	link = &region->pages;
	while (*link != NULL) {
		page = *link;
		if (page->address < address) {
			link = &page->next;
			continue;
		}

		*link = page->next;
		page->region = right;
		page->next = right->pages;
		right->pages = page;
	}

	/* Indexes each half's pages afresh (the old index held both halves' pages). */
	region->page_index_retry = 0;
	(void)vmspace_region_page_index_rebuild(region, 0);
	(void)vmspace_region_page_index_rebuild(right, 0);

	/* Links the right half after the left. */
	right->next = region->next;
	region->next = right;

	return 0;
}

/* Splits a region at an address, allocating the right half. */
static int
split_region(
	struct vm_region *region,
	uintptr_t address)
{
	struct vm_region *right;
	int error;

	/* Allocates the record the right half of the split will use. */
	right = kern_calloc(1, sizeof(*right));
	if (right == NULL)
		return ENOMEM;

	/* Splits the region, returning the record a failure did not take. */
	error = split_region_prepared(region, address, right);
	if (error != 0)
		kern_free(right);

	/* Reports why the split failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Frees a list of retired regions and their resources outside the locks. */
static void
release_retired_regions(
	struct vm_region *retired)
{
	struct vm_region *region;

	/* Frees each detached region with everything it owned. */
	region = retired;
	while (region != NULL) {
		retired = region->next;
		vmspace_release_detached_region_pages(region);
		if (region->file != NULL)
			(void)file_close(region->file);
		if (region->object != NULL)
			vm_object_put(region->object);
		if (region->snapshot != NULL)
			file_exec_snapshot_put(region->snapshot);
		if (region->device != NULL)
			vm_device_put(region->device);
		if (region->commit_size != 0)
			vm_commit_release(region->commit_size);
		vmspace_region_page_index_free(region);
		kern_free(region);
		region = retired;
	}
}

/* Records the program break start; the caller holds the VM locks. */
static int
vmspace_set_brk_start_locked(
	struct vmspace *vm,
	uintptr_t start,
	uint64_t static_data_bytes)
{
	if (vm == NULL ||
	    vm == &kernel_vmspace ||
	    vm->brk_start != 0 ||
	    (start & (PAGE_SIZE - 1U)) != 0 ||
	    start < vm_layout.user_minimum ||
	    start >= vm_layout.brk_limit ||
	    vmspace_overlaps(vm, start, PAGE_SIZE))
		return EINVAL;

	if (static_data_bytes > vm->data_limit)
		return ENOMEM;

	vm->brk_start = start;
	vm->brk_current = start;
	vm->static_data_bytes = static_data_bytes;

	return 0;
}

/* Grows or shrinks the program break region; the caller holds the VM locks. */
static int
vmspace_brk_locked(
	struct vmspace *vm,
	uintptr_t requested,
	uintptr_t *result)
{
	struct vm_region **link;
	struct vm_region *region;
	uintptr_t old_end;
	uintptr_t new_end;
	uint64_t mapped_bytes;
	size_t difference;
	int error;
	struct vm_page **page_link;
	struct vm_page *page;

	region = NULL;

	/* Requires a user space that has a program break. */
	if (vm == NULL ||
	    vm == &kernel_vmspace ||
	    result == NULL ||
	    vm->brk_start == 0)
		return EINVAL;

	/* Waits for the faults in flight before changing the layout. */
	vmspace_wait_faults_locked(vm);
	if (requested == 0) {
		*result = vm->brk_current;
		return 0;
	}

	/* The request must stay within the break area and the data limit. */
	if (requested < vm->brk_start || requested >= vm_layout.brk_limit)
		return ENOMEM;

	if (vm->static_data_bytes > vm->data_limit ||
	    (uint64_t)(requested - vm->brk_start) >
	    vm->data_limit - vm->static_data_bytes)
		return ENOMEM;

	old_end = (vm->brk_current + PAGE_SIZE - 1U) & ~(uintptr_t)(PAGE_SIZE - 1U);
	new_end = (requested + PAGE_SIZE - 1U) & ~(uintptr_t)(PAGE_SIZE - 1U);

	/* Finds the break region, if one has been created. */
	for (link = &vm->regions; *link != NULL; link = &(*link)->next) {
		if (((*link)->flags & VM_REGION_BRK) != 0) {
			region = *link;
			break;
		}
	}

	/* Growth extends or creates the break region; shrinking frees pages. */
	if (new_end > old_end) {
		difference = new_end - old_end;
		if (region == NULL) {
			error = vmspace_map_region(vm, vm->brk_start,
				new_end - vm->brk_start,
				HAL_SPACE_READ | HAL_SPACE_WRITE, VM_BACKING_ANON,
				NULL, 0, vm->brk_start, 0,
				VM_REGION_BRK | VM_REGION_IMMUTABLE,
				new_end - vm->brk_start, &region);
			if (error != 0)
				return error;
		} else {
			if (region->start + region->size != old_end ||
			    vmspace_overlaps(vm, old_end, difference))
				return ENOMEM;
			mapped_bytes = vm->mapped_virtual_bytes;
			if (mapped_bytes > vm->address_limit)
				mapped_bytes = vm->address_limit;
			if ((uint64_t)difference > vm->address_limit - mapped_bytes)
				return ENOMEM;
			error = vm_commit_reserve(difference);
			if (error != 0)
				return error;
			region->size += difference;
			region->commit_size += difference;
			vm->mapped_virtual_bytes += difference;
		}
	} else if (new_end < old_end) {
		if (region == NULL || region->start + region->size != old_end)
			return EINVAL;
		page_link = &region->pages;
		while (*page_link != NULL) {
			page = *page_link;
			if (page->address < new_end) {
				page_link = &page->next;
				continue;
			}

			/* Takes the page off the list and the index, and frees it. */
			*page_link = page->next;
			vmspace_region_page_index_remove(region, page);
			free_vm_page(vm, page, 1);
		}

		/* Gives back the commitment and the address space the break released. */
		difference = old_end - new_end;
		vm_commit_release(difference);
		region->size -= difference;
		region->commit_size -= difference;
		vm->mapped_virtual_bytes -= difference;
		if (region->size == 0) {
			*link = region->next;
			vmspace_region_page_index_free(region);
			kern_free(region);
		}
	}

	vm->brk_current = requested;
	*result = requested;

	return 0;
}

/* Detaches and unlinks the regions of a range; the caller holds the VM locks. */
static int
vmspace_unmap_locked(
	struct vmspace *vm,
	uintptr_t start,
	size_t size,
	struct vm_region **retired)
{
	struct vm_region **link;
	struct vm_region *region;
	uintptr_t end;
	int error;
	struct vm_page *page;

	/* Rejects a kernel space, a missing output, or a malformed range. */
	if (vm == NULL ||
	    vm == &kernel_vmspace ||
	    retired == NULL ||
	    !vmspace_range_valid(start, size))
		return EINVAL;

	/* Waits for the faults in flight before changing the layout. */
	vmspace_wait_faults_locked(vm);
	end = start + size;

	/* Validate the complete transaction before split_region mutates metadata. */
	for (region = vm->regions; region != NULL; region = region->next) {
		if (region->start >= end || region->start + region->size <= start)
			continue;
		if ((region->flags & VM_REGION_IMMUTABLE) != 0)
			return EACCES;
		for (page = region->pages; page != NULL; page = page->next) {
			if (page->address < end &&
			    page->address + PAGE_SIZE > start &&
			    page->wire_count != 0)
				return EBUSY;
		}
	}

	/* Splits the region that straddles the end of the range. */
	region = vmspace_find_region_locked(vm, end - 1U, 1);
	if (region != NULL && end < region->start + region->size) {
		error = split_region(region, end);
		if (error != 0)
			return error;
	}

	/* Splits the region that straddles the start of the range. */
	region = vmspace_find_region_locked(vm, start, 1);
	if (region != NULL && start > region->start) {
		error = split_region(region, start);
		if (error != 0)
			return error;
	}

	/*
	 * All validation, allocation, and splitting is complete.  From this
	 * point the commit cannot report failure: a HAL failure would leave
	 * only part of the old range detached, so it is a kernel invariant
	 * violation.  Detach every old PTE/reverse map before unlinking any
	 * region.
	 */
	for (region = vm->regions; region != NULL && region->start < end;
	     region = region->next) {
		if (region->start >= start)
			detach_region_pages_for_unmap(vm, region);
	}

	for (link = &vm->regions; *link != NULL; link = &(*link)->next) {
		if ((*link)->start >= start)
			break;
	}

	region = *link;
	while (region != NULL && region->start < end) {
		*link = region->next;
		vm->mapped_virtual_bytes -= region->size;
		region->next = *retired;
		*retired = region;
		region = *link;
	}

	return 0;
}

/* Changes the protection of a range, rolling the HAL back on failure; the caller holds the VM locks. */
static int
vmspace_protect_locked(
	struct vmspace *vm,
	uintptr_t start,
	size_t size,
	uint32_t prot)
{
	struct vm_region *region;
	struct vm_region *first;
	struct vm_region *end_region;
	struct vm_page *page;
	struct vm_page *failed_page;
	struct vm_region *failed_region;
	uintptr_t end;
	uintptr_t covered;
	size_t new_commit;
	int error;
	uint32_t page_prot;
	int hal_error;
	struct vm_page *rollback;
	hal_physaddr_t physical;

	failed_page = NULL;
	failed_region = NULL;
	new_commit = 0;

	/* Rejects a malformed range or a protection the hardware has no bit for. */
	if (vm == NULL ||
	    vm == &kernel_vmspace ||
	    !vmspace_range_valid(start, size) ||
	    (prot & ~(HAL_SPACE_READ | HAL_SPACE_WRITE | HAL_SPACE_EXEC)) != 0)
		return EINVAL;

	/* Waits for the faults in flight before changing the protections. */
	vmspace_wait_faults_locked(vm);

	/* The range must be fully mapped by mutable regions that allow prot. */
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
		if ((prot & ~region->max_prot) != 0)
			return EACCES;
		covered = region->start + region->size;
		if (covered > end)
			covered = end;
	}

	if (covered != end)
		return EINVAL;

	/* Splits the regions that straddle either end of the range. */
	end_region = vmspace_find_region_locked(vm, end - 1U, 1);
	if (end_region != NULL && end < end_region->start + end_region->size) {
		error = split_region(end_region, end);
		if (error != 0)
			return error;
	}

	/* Splits the region that straddles the start of the range. */
	region = vmspace_find_region_locked(vm, start, 1);
	if (start > region->start) {
		error = split_region(region, start);
		if (error != 0)
			return error;
		region = region->next;
	}

	first = region;

	/*
	 * A region becoming accessible or writable needs commit first.  A
	 * shared region writes into its object and needs none; a private
	 * region that maps its object's pages copies them on write, so it
	 * does.
	 */
	for (; region != NULL && region->start < end; region = region->next) {
		if (region->commit_size == 0 &&
		    (region->flags & VM_REGION_SHARED) == 0 &&
		    (region->object == NULL ||
		    (region->flags & VM_REGION_PRIVATE_OBJECT) != 0) &&
		    ((region->backing == VM_BACKING_ANON && prot != 0) ||
		     (region->backing == VM_BACKING_FILE &&
		      (prot & HAL_SPACE_WRITE) != 0))) {
			if (new_commit > SIZE_MAX - region->size)
				return EOVERFLOW;
			new_commit += region->size;
		}
	}

	if (new_commit != 0) {
		error = vm_commit_reserve(new_commit);
		if (error != 0)
			return error;
	}

	/* Applies the new protection to every resident page. */
	for (region = first; region != NULL && region->start < end;
	     region = region->next) {
		for (page = region->pages; page != NULL; page = page->next) {
			if (page->flags & VM_MAPPING_COW)
				page_prot = prot & ~HAL_SPACE_WRITE;
			else
				page_prot = prot;
			/* Device identity and cache policy survive every nonempty protection change. */
			if (prot != 0 && region->device != NULL)
				page_prot |= vm_device_page_attributes(region->device);
			hal_error = HAL_OK;
			if (prot == 0 && (page->flags & VM_MAPPING_MAPPED)) {
				hal_error = hal_space_unmap(vm->space,
				    (void *)page->address, PAGE_SIZE);
				if (hal_error == HAL_OK) {
					page->flags &= ~VM_MAPPING_MAPPED;
					page->flags |= VM_MAPPING_PROTECT_REMOVED;
				}
			} else if (prot != 0 &&
			    (page->flags & VM_MAPPING_MAPPED)) {
				hal_error = hal_space_prot(vm->space,
				    (void *)page->address, PAGE_SIZE, page_prot);
			} else if (prot != 0 && region->device != NULL) {
				/* Restores a previously inaccessible device page from its immutable physical extent. */
				physical = (hal_physaddr_t)(region->device->physical +
				    region->device_offset + page->address - region->start);
				hal_error = hal_space_map(
					vm->space,
					(void *)page->address,
					physical,
					PAGE_SIZE,
					page_prot);
				if (hal_error == HAL_OK) {
					/* Rollback must remove this newly restored PTE if a later page fails. */
					page->flags |= VM_MAPPING_MAPPED | VM_MAPPING_PROTECT_ADDED;
				}
			} else if (prot != 0 && page->object_page != NULL) {
				hal_error = hal_space_map(vm->space,
				    (void *)page->address,
				    page->object_page->pmem.paddr, PAGE_SIZE, page_prot);
				if (hal_error == HAL_OK)
					page->flags |= VM_MAPPING_MAPPED |
					    VM_MAPPING_PROTECT_ADDED;
			} else if (prot != 0 &&
			    vm_private_page_is_resident(page)) {
				hal_error = hal_space_map(vm->space,
				    (void *)page->address,
				    page->private_page->pmem.paddr, PAGE_SIZE, page_prot);
				if (hal_error == HAL_OK)
					page->flags |= VM_MAPPING_MAPPED |
					    VM_MAPPING_PROTECT_ADDED;
			}

			/* Remembers where the failure happened so it can be rolled back. */
			if (hal_error != HAL_OK) {
				failed_region = region;
				failed_page = page;
				goto rollback;
			}
		}
	}

	/* Commits the new protection and the commit charge. */
	for (region = first; region != NULL && region->start < end;
	     region = region->next) {
		region->prot = prot;
		for (page = region->pages; page != NULL; page = page->next)
			page->flags &= ~(VM_MAPPING_PROTECT_REMOVED |
			    VM_MAPPING_PROTECT_ADDED);
		if (region->commit_size == 0 &&
		    (region->flags & VM_REGION_SHARED) == 0 &&
		    (region->object == NULL ||
		    (region->flags & VM_REGION_PRIVATE_OBJECT) != 0) &&
		    ((region->backing == VM_BACKING_ANON && prot != 0) ||
		     (region->backing == VM_BACKING_FILE &&
		      (prot & HAL_SPACE_WRITE) != 0)))
			region->commit_size = region->size;
	}

	return 0;

rollback:
	/* Restores every page changed before the failure. */
	for (region = first; region != NULL && region->start < end;
	     region = region->next) {
		for (rollback = region->pages; rollback != NULL;
		     rollback = rollback->next) {
			if (region == failed_region && rollback == failed_page)
				break;
			if (rollback->flags & VM_MAPPING_PROTECT_ADDED) {
				if (hal_space_unmap(vm->space,
				    (void *)rollback->address, PAGE_SIZE) != HAL_OK)
					HAL_FATAL("VM protection rollback unmap failed");
				rollback->flags &= ~(VM_MAPPING_MAPPED |
				    VM_MAPPING_PROTECT_ADDED);
			} else if (rollback->flags & VM_MAPPING_PROTECT_REMOVED) {
				/* Reconstructs the original physical page from the backing which survived rollback. */
				if (region->device != NULL) {
					physical = (hal_physaddr_t)(region->device->physical +
					    region->device_offset + rollback->address - region->start);
				} else if (rollback->object_page != NULL) {
					physical = rollback->object_page->pmem.paddr;
				} else {
					physical = rollback->private_page->pmem.paddr;
				}

				/* Restores the original protection before releasing the rollback marker. */
				if (hal_space_map(vm->space,
				    (void *)rollback->address, physical, PAGE_SIZE,
				    vm_page_effective_prot(rollback)) != HAL_OK)
					HAL_FATAL("VM protection rollback map failed");
				rollback->flags &= ~VM_MAPPING_PROTECT_REMOVED;
				rollback->flags |= VM_MAPPING_MAPPED;
			} else if ((rollback->flags & VM_MAPPING_MAPPED) &&
			    hal_space_prot(vm->space, (void *)rollback->address,
				PAGE_SIZE, vm_page_effective_prot(rollback)) != HAL_OK) {
				HAL_FATAL("VM protection rollback failed");
			}
		}

		if (region == failed_region)
			break;
	}

	if (new_commit != 0)
		vm_commit_release(new_commit);
	return EINVAL;
}

/* Sets the limit on mapped virtual bytes; the caller holds the VM locks. */
static int
vmspace_set_address_limit_locked(
	struct vmspace *vm,
	uint64_t limit)
{
	if (vm == NULL || vm == &kernel_vmspace || limit > vmspace_address_cap())
		return EINVAL;
	vm->address_limit = limit;
	return 0;
}

/* Sets the stack size limit; the caller holds the VM locks. */
static void
vmspace_set_stack_limit_locked(
	struct vmspace *vm,
	uint64_t limit)
{
	if (vm != NULL && vm != &kernel_vmspace)
		vm->stack_limit = limit;
}

/* Sets the data size limit; the caller holds the VM locks. */
static int
vmspace_set_data_limit_locked(
	struct vmspace *vm,
	uint64_t limit)
{
	uint64_t cap;

	cap = vmspace_address_cap();
	if (vm == NULL || vm == &kernel_vmspace)
		return EINVAL;

	/*
	 * RLIM_INFINITY is the public resource-limit representation.  The
	 * VM still needs a concrete bound for overflow-safe brk arithmetic,
	 * so map it to the largest addressable user range at this boundary.
	 */
	if (limit == UINT64_MAX)
		limit = cap;
	if (limit > cap)
		return EINVAL;
	vm->data_limit = limit;
	return 0;
}
