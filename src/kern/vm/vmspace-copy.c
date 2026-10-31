/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Copies between kernel and user memory: the range checks, the pinning
 * of user pages and the wiring of ranges.
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

/*
 * Test checkpoints.
 *
 * The test build defines these to observe a step that leaves no other
 * trace.  They are weak so that a production kernel links without them and
 * every call site is a null test the compiler removes.
 */
extern void vmspace_pin_page_checkpoint(struct vmspace *vm, size_t index, size_t page_count) __attribute__((weak));

static int vmspace_check_locked(struct vmspace *vm, uintptr_t address, size_t size, uint32_t required);
static int vmspace_pin_mapping_ready(struct vmspace *vm, uintptr_t address, uint32_t required);
static int vmspace_pin_mapping_ready_locked(struct vmspace *vm, uintptr_t address, uint32_t required);
static size_t vmspace_pin_first_unready_locked(struct vmspace *vm, uintptr_t first, size_t page_count, uint32_t required);
static int vmspace_pin_fault_from(struct vmspace *vm, uintptr_t start, uintptr_t last, uint32_t required, uint32_t fault_access);
static int copy_backing(struct vmspace *vm, uintptr_t user_address, void *kernel, size_t size, uint32_t required, int to_user);
static int vmspace_copy_to_locked(struct vmspace *vm, uintptr_t destination, const void *source, size_t size);
static int vmspace_copy_from_locked(struct vmspace *vm, void *destination, uintptr_t source, size_t size);

/*
 * Tests whether a byte range lies entirely inside the user address range.
 */
int
vmspace_user_range_valid(
	uintptr_t start,
	size_t size)
{
	vmspace_layout_init();

	/* Rejects an empty range, which lies nowhere. */
	if (size == 0)
		return 0;

	/* Rejects a range that starts below the user address range. */
	if (start < vm_layout.user_minimum)
		return 0;

	/* Rejects a range that starts at or above its end. */
	if (start >= vm_layout.user_limit)
		return 0;

	/* Rejects a range that reaches past that end. */
	if (size > vm_layout.user_limit - start)
		return 0;

	/* Succeeded: the whole range lies inside the user address range. */
	return 1;
}

/*
 * Releases the pins taken by vmspace_pin_user_pages(), last page first.
 */
void
vmspace_unpin_user_pages(
	struct vmspace_pinned_page *pages,
	size_t page_count)
{
	struct vmspace_pinned_page *page;

	/* Ignores a missing array. */
	if (pages == NULL)
		return;

	/* Releases every pin in reverse order and empties its entry. */
	while (page_count != 0) {
		page_count--;
		page = &pages[page_count];
		if (page->kind == VMSPACE_PINNED_PRIVATE)
			vm_private_page_unpin(page->owner.private_page);
		else if (page->kind == VMSPACE_PINNED_OBJECT)
			vm_object_page_unpin(page->owner.object_page);
		else if (page->kind == VMSPACE_PINNED_DEVICE)
			vm_device_put(page->owner.device);
		kern_memset(page, 0, sizeof(*page));
	}
}

/*
 * Pins every page of a user range for kernel access.
 *
 * Under one hold of both metadata locks the range is checked, its pages
 * are looked at, and when every page is ready the whole range is validated
 * and pinned, so that no unmap, remap, or fork can change page identity in
 * between.  A range that is not mapped with the access fails with EFAULT
 * before any page is faulted.  Pages that are not ready are faulted in with
 * no VM lock held, and the range is then validated and pinned under the
 * locks again; a page that changed underneath is faulted again.
 */
int
vmspace_pin_user_pages(
	struct vmspace *vm,
	uintptr_t address,
	size_t size,
	uint32_t required,
	struct vmspace_pinned_page *pages,
	size_t page_count)
{
	struct vm_private_page *wait_backing;
	uintptr_t first;
	uintptr_t last;
	uintptr_t current;
	uint32_t fault_access;
	size_t expected;
	size_t index;
	size_t acquired;
	int error;
	int refault;
	int wait_fault;
	uint64_t fault_sequence;
	struct vm_region *region;
	struct vm_page *entry;
	int wait_error;
	size_t unready;
	int faulted;

	/* Rejects a call that names no address space. */
	if (vm == NULL)
		return EFAULT;

	/* Rejects the kernel address space, which is never pinned this way. */
	if (vm == &kernel_vmspace)
		return EFAULT;

	/* Rejects a call that names nowhere to report the pages. */
	if (pages == NULL)
		return EFAULT;

	/* Rejects an empty range. */
	if (size == 0)
		return EFAULT;

	/* Rejects an access this kernel does not define. */
	if ((required & ~(HAL_SPACE_READ | HAL_SPACE_WRITE |
	    HAL_SPACE_EXEC)) != 0)
		return EFAULT;

	/* Rejects a request that asks for no access at all. */
	if (required == 0)
		return EFAULT;

	/* Rejects a range that does not lie inside the user address range. */
	if (!vmspace_user_range_valid(address, size))
		return EFAULT;

	/* Rejects a result array that does not match the range. */
	first = address & ~(uintptr_t)(PAGE_SIZE - 1U);
	last = (address + size - 1U) & ~(uintptr_t)(PAGE_SIZE - 1U);
	expected = (size_t)((last - first) / PAGE_SIZE) + 1U;
	if (page_count != expected)
		return EINVAL;
	kern_memset(pages, 0, page_count * sizeof(*pages));

	/* Picks the one concrete access the fault path understands. */
	if ((required & HAL_SPACE_WRITE) != 0)
		fault_access = HAL_SPACE_WRITE;
	else if ((required & HAL_SPACE_READ) != 0)
		fault_access = HAL_SPACE_READ;
	else
		fault_access = HAL_SPACE_EXEC;

	/*
	 * The first pass looks at the pages before pinning; a pass right after
	 * the fault path validates and pins whatever the faults left, as a page
	 * that the fault could not make ready is reported by the pin itself.
	 */
	faulted = 0;

retry_faults:
	/*
	 * Validate the complete range before taking the first backing pin.
	 * Holding both metadata locks until the final pin closes the old
	 * wire/unlock/rewalk gap in which unmap, remap, or fork could
	 * change page identity.
	 */
	wait_backing = NULL;
	acquired = 0;
	refault = 0;
	wait_fault = 0;
	fault_sequence = 0;
	unready = page_count;
	vm_metadata_enter();
	mutex_lock(&vm->lock);

	error = vmspace_check_locked(vm, address, size, required);

	/*
	 * Finds the first page that needs the fault path, which leaves the
	 * locks to fault it in; EAGAIN keeps the walks below from running.
	 */
	if (error == 0 && !faulted) {
		unready = vmspace_pin_first_unready_locked(vm, first, page_count, required);
		if (unready != page_count)
			error = EAGAIN;
	}

	faulted = 0;
	for (index = 0, current = first; error == 0 && index < page_count;
	     index++, current += PAGE_SIZE) {

		/* Looks up the mapping the fault just published. */
		region = vmspace_find_region_locked(vm, current, 1);
		if (region != NULL)
			entry = vmspace_find_page(region, current);
		else
			entry = NULL;

		/* Faults again when the mapping went away under the walk. */
		if (entry == NULL) {
			refault = 1;
			error = EAGAIN;
			break;
		}

		/* Waits out a mapping another pass owns. */
		if ((entry->flags & VM_MAPPING_BUSY) != 0) {
			fault_sequence = waitq_sequence(&vm->fault_waitq);
			wait_fault = 1;
			error = EAGAIN;
			break;
		}

		/*
		 * Ordinary mappings have exactly one private or object backing.
		 * Device mappings instead retain the region's immutable driver extent.
		 */
		if (region->device == NULL &&
		    (entry->private_page == NULL) == (entry->object_page == NULL)) {
			error = EFAULT;
			break;
		}

		if ((required & HAL_SPACE_WRITE) != 0 &&
		    (entry->flags & VM_MAPPING_COW) != 0) {
			/* Private cache pages also require COW before a writable pin. */
			refault = 1;
			error = EAGAIN;
			break;
		}

		/* A private mapping must be resident and past any copy-on-write. */
		if (entry->private_page != NULL) {
			if (!vm_private_page_is_resident(entry)) {
				refault = 1;
				error = EAGAIN;
				break;
			}

			if ((required & HAL_SPACE_WRITE) != 0 &&
			    (entry->flags & VM_MAPPING_COW) != 0) {
				/* A fork raced the fault phase; break the new COW generation. */
				refault = 1;
				error = EAGAIN;
				break;
			}
		}
	}

	/* Pins each page's backing. */
	for (index = 0, current = first; error == 0 && index < page_count;
	     index++, current += PAGE_SIZE) {
		region = vmspace_find_region_locked(vm, current, 1);
		entry = vmspace_find_page(region, current);
		if (region->device != NULL) {
			/* The backing reference survives unmap and descriptor close. */
			vm_device_ref(region->device);
			pages[index].kind = VMSPACE_PINNED_DEVICE;
			pages[index].owner.device = region->device;
			pages[index].device_offset = region->device_offset +
			    (size_t)(current - region->start);
			pages[index].memory.paddr = (hal_physaddr_t)(region->device->physical +
			    pages[index].device_offset);
			pages[index].memory.size = PAGE_SIZE;
		} else if (entry->private_page != NULL) {
			error = vm_private_page_pin(entry->private_page,
			    &pages[index].memory);
			if (error == EBUSY) {
				/* Keep the wait target alive after dropping the VM locks. */
				wait_backing = entry->private_page;
				vm_private_page_ref(wait_backing);
			}

			/* Records which of the two backings this pin holds. */
			if (error == 0) {
				pages[index].kind = VMSPACE_PINNED_PRIVATE;
				pages[index].owner.private_page = entry->private_page;
			}
		} else {
			error = vm_object_page_pin(entry->object_page);
			if (error == 0) {
				pages[index].kind = VMSPACE_PINNED_OBJECT;
				pages[index].owner.object_page = entry->object_page;
				pages[index].memory = entry->object_page->pmem;
			}
		}

		if (error == 0)
			acquired++;
		if (error == 0 && vmspace_pin_page_checkpoint != NULL)
			vmspace_pin_page_checkpoint(vm, index, page_count);
	}

	mutex_unlock(&vm->lock);
	vm_metadata_leave();

	if (error == 0)
		return 0;

	/*
	 * Faults in every page from the first one that was not ready, then
	 * pins under the locks again.  Faulting may sleep and may break COW,
	 * so it is never done under a VM lock.
	 */
	if (unready != page_count) {
		error = vmspace_pin_fault_from(vm, first + unready * PAGE_SIZE, last, required, fault_access);
		if (error != 0)
			return error;

		faulted = 1;
		goto retry_faults;
	}

	/* Backing unpin may finish object teardown, so rollback outside VM locks. */
	vmspace_unpin_user_pages(pages, acquired);
	if (wait_backing != NULL) {
		wait_error = vm_private_page_wait_idle(wait_backing);
		vm_private_page_put(wait_backing);
		if (wait_error != 0)
			return wait_error;
		goto retry_faults;
	}

	if (wait_fault) {
		vmspace_wait_fault_event(vm, fault_sequence);
		goto retry_faults;
	}

	if (refault)
		goto retry_faults;

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Checks that a user range is mapped with the required protection.
 */
int
vmspace_check(
	struct vmspace *vm,
	uintptr_t address,
	size_t size,
	uint32_t required)
{
	int error;

	/* Checks the range under the metadata and space locks. */
	if (vm == NULL || vm == &kernel_vmspace)
		return EFAULT;
	vm_metadata_enter();
	mutex_lock(&vm->lock);

	error = vmspace_check_locked(vm, address, size, required);

	mutex_unlock(&vm->lock);
	vm_metadata_leave();

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Copies kernel memory into a user range.
 */
int
vmspace_copy_to(
	struct vmspace *vm,
	uintptr_t destination,
	const void *source,
	size_t size)
{
	int error;

	if (vm == NULL || vm == &kernel_vmspace)
		return EFAULT;

	/* Reports the failure. */
	error = vmspace_copy_to_locked(vm, destination, source, size);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Copies a user range into kernel memory.
 */
int
vmspace_copy_from(
	struct vmspace *vm,
	void *destination,
	uintptr_t source,
	size_t size)
{
	int error;

	if (vm == NULL || vm == &kernel_vmspace)
		return EFAULT;

	/* Reports the failure. */
	error = vmspace_copy_from_locked(vm, destination, source, size);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Checks that a range is covered by regions with the required protection; the caller holds the VM lock. */
static int
vmspace_check_locked(
	struct vmspace *vm,
	uintptr_t address,
	size_t size,
	uint32_t required)
{
	struct vm_region *region;
	uintptr_t current;
	uintptr_t end;
	uintptr_t region_end;

	if (vm == NULL || !vmspace_user_range_valid(address, size))
		return EFAULT;

	/* Walks the sorted regions, refusing any gap. */
	current = address;
	end = address + size;
	for (region = vm->regions; region != NULL && current < end;
	     region = region->next) {
		region_end = region->start + region->size;
		if (current < region->start)
			return EFAULT;
		if (current >= region_end)
			continue;
		if ((region->prot & required) != required)
			return EFAULT;
		if (region_end < end)
			current = region_end;
		else
			current = end;
	}

	if (current == end)
		return 0;
	return EFAULT;
}

/* Tests whether a page can be pinned without entering the fault path. */
static int
vmspace_pin_mapping_ready(
	struct vmspace *vm,
	uintptr_t address,
	uint32_t required)
{
	int ready;

	/* Looks at the page under the metadata and space locks. */
	vm_metadata_enter();
	mutex_lock(&vm->lock);

	ready = vmspace_pin_mapping_ready_locked(vm, address, required);

	mutex_unlock(&vm->lock);
	vm_metadata_leave();

	/* Reports whether the page is ready. */
	return ready;
}

/*
 * Tests whether a page can be pinned without entering the fault path; the
 * caller holds the metadata and space locks.
 */
static int
vmspace_pin_mapping_ready_locked(
	struct vmspace *vm,
	uintptr_t address,
	uint32_t required)
{
	struct vm_region *region;
	struct vm_page *entry;
	int ready;
	struct vm_object_page *page;
	struct vm_object *object;
	unsigned long irq;

	ready = 0;

	/*
	 * A resident mapping does not need to enter the fault path again
	 * merely to acquire another uaccess pin.  This matters when one
	 * syscall pins multiple ranges which share a user page: the fault
	 * path takes exclusive backing I/O ownership and would otherwise
	 * wait for the syscall's first pin.
	 */
	region = vmspace_find_region_locked(vm, address, 1);
	if (region != NULL)
		entry = vmspace_find_page(region, address);
	else
		entry = NULL;

	/* A mapping is ready when it is present, permitted, and past any copy-on-write. */
	if (entry != NULL &&
	    (region->prot & required) == required &&
	    (entry->flags & (VM_MAPPING_BUSY | VM_MAPPING_MAPPED)) ==
	    VM_MAPPING_MAPPED) {
		if (region->device != NULL) {
			ready = 1;
		} else if (entry->private_page != NULL &&
		    vm_private_page_is_resident(entry) &&
		    ((required & HAL_SPACE_WRITE) == 0 ||
		    (entry->flags & VM_MAPPING_COW) == 0)) {
			ready = 1;
		} else if (entry->object_page != NULL &&
		    ((required & HAL_SPACE_WRITE) == 0 ||
		    (entry->flags & VM_MAPPING_COW) == 0)) {
			/* A cached object page must be complete and not in error. */
			page = entry->object_page;
			object = page->owner;
			if (object != NULL) {
				irq = spin_lock_irqsave(&object->lock);
				ready = 0;
				if (page->owner == object &&
				    page->pmem.size != 0 &&
				    (page->flags & (VM_OBJECT_PAGE_BUSY |
				    VM_OBJECT_PAGE_ERROR)) == 0)
					ready = 1;
				spin_unlock_irqrestore(&object->lock, irq);
			}
		}
	}

	/* Reports whether the page is ready. */
	return ready;
}

/*
 * Finds the first page of a range that cannot be pinned without the fault
 * path, or page_count when every page can; the caller holds the metadata
 * and space locks.
 */
static size_t
vmspace_pin_first_unready_locked(
	struct vmspace *vm,
	uintptr_t first,
	size_t page_count,
	uint32_t required)
{
	uintptr_t current;
	size_t index;
	int ready;

	/* Looks at each page in address order until one is not ready. */
	current = first;
	for (index = 0; index < page_count; index++) {
		ready = vmspace_pin_mapping_ready_locked(vm, current, required);
		if (!ready)
			return index;

		current += PAGE_SIZE;
	}

	/* Every page is ready. */
	return page_count;
}

/*
 * Faults in every page from start to last that is not ready to be pinned.
 *
 * Faulting may sleep and may break COW, so the caller holds no VM lock.
 * Each page is looked at again first, as an earlier fault (or another
 * thread) may have made it ready.
 */
static int
vmspace_pin_fault_from(
	struct vmspace *vm,
	uintptr_t start,
	uintptr_t last,
	uint32_t required,
	uint32_t fault_access)
{
	uintptr_t current;
	int ready;
	int error;

	/* Faults in each page that is still not ready. */
	for (current = start;; current += PAGE_SIZE) {
		ready = vmspace_pin_mapping_ready(vm, current, required);
		if (!ready) {
			error = vmspace_fault(vm, current, fault_access);
			if (error != 0)
				return error;
		}

		/* Stops after the last page of the range. */
		if (current == last)
			break;
	}

	/* Succeeded: every page went through the fault path or was ready. */
	return 0;
}

/* Copies between kernel memory and a pinned user range in either direction. */
static int
copy_backing(
	struct vmspace *vm,
	uintptr_t user_address,
	void *kernel,
	size_t size,
	uint32_t required,
	int to_user)
{
	struct vmspace_pinned_page stack_pages[VM_COPY_STACK_PAGES];
	struct vmspace_pinned_page *pages;
	uint8_t *bytes;
	uintptr_t first;
	uintptr_t last;
	size_t page_count;
	size_t position;
	int error;
	struct vmspace_pinned_page *page;
	struct vm_region *region;
	struct vm_page *entry;
	size_t offset;
	size_t chunk;
	void *mapped;

	bytes = kernel;

	/* Pins every page of the range. */
	if (size == 0)
		return 0;

	if (kernel == NULL)
		return EINVAL;

	if (!vmspace_user_range_valid(user_address, size))
		return EFAULT;

	first = user_address & ~(uintptr_t)(PAGE_SIZE - 1U);
	last = (user_address + size - 1U) & ~(uintptr_t)(PAGE_SIZE - 1U);
	page_count = (size_t)((last - first) / PAGE_SIZE) + 1U;

	/*
	 * A range within one page of one region, on a resident private page
	 * past any copy-on-write (most system call arguments), is checked and
	 * pinned under one hold of the locks with one walk of the regions: the
	 * same checks as the pin below, which takes every other range and a
	 * page that is not ready.
	 */
	if (page_count == 1U) {
		kern_memset(&stack_pages[0], 0, sizeof(stack_pages[0]));
		vm_metadata_enter();
		mutex_lock(&vm->lock);

		/* The region that holds the address: the regions are sorted and do not overlap. */
		for (region = vm->regions; region != NULL; region = region->next) {
			if (user_address < region->start) {
				region = NULL;
				break;
			}
			if (user_address - region->start < region->size)
				break;
		}

		/* The whole range in it with the access, and its page ready, then pinned. */
		entry = NULL;
		if (region != NULL && region->device == NULL && (region->prot & required) == required &&
		    user_address - region->start + size <= region->size)
			entry = vmspace_find_page(region, user_address);
		if (entry != NULL && entry->private_page != NULL && entry->object_page == NULL &&
		    (entry->flags & (VM_MAPPING_BUSY | VM_MAPPING_MAPPED)) == VM_MAPPING_MAPPED &&
		    vm_private_page_is_resident(entry) &&
		    ((required & HAL_SPACE_WRITE) == 0U || (entry->flags & VM_MAPPING_COW) == 0U) &&
		    vm_private_page_pin(entry->private_page, &stack_pages[0].memory) == 0) {
			stack_pages[0].kind = VMSPACE_PINNED_PRIVATE;
			stack_pages[0].owner.private_page = entry->private_page;
			if (vmspace_pin_page_checkpoint != NULL)
				vmspace_pin_page_checkpoint(vm, 0U, 1U);
		}

		mutex_unlock(&vm->lock);
		vm_metadata_leave();

		/* The bytes through the pinned page, then the pin let go. */
		if (stack_pages[0].kind == VMSPACE_PINNED_PRIVATE) {
			mapped = (uint8_t *)hal_pmem_to_kernel(stack_pages[0].memory.paddr) +
			    (user_address & (PAGE_SIZE - 1U));
			if (to_user) {
				kern_memcpy(mapped, bytes, size);
				vm_private_page_mark_dirty(stack_pages[0].owner.private_page);
			} else {
				kern_memcpy(bytes, mapped, size);
			}
			vmspace_unpin_user_pages(stack_pages, 1U);
			return 0;
		}
	}

	/*
	 * A short range pins into an array on the stack, and the pin checks
	 * the range under the same hold of the locks it pins with.  A longer
	 * range is checked first, so that a range that cannot be copied
	 * reports EFAULT and does not allocate an array for its pages.
	 */
	if (page_count <= VM_COPY_STACK_PAGES) {
		pages = stack_pages;
	} else {
		error = vmspace_check(vm, user_address, size, required);
		if (error != 0)
			return EFAULT;

		/* Allocates one record per page of the range. */
		if (page_count > SIZE_MAX / sizeof(*pages))
			return ENOMEM;
		pages = kern_calloc(page_count, sizeof(*pages));
		if (pages == NULL)
			return ENOMEM;
	}

	/* Pins every page of the range before any byte moves. */
	error = vmspace_pin_user_pages(vm, user_address, size, required, pages, page_count);
	if (error != 0) {
		if (pages != stack_pages)
			kern_free(pages);
		if (error == ENOMEM)
			return ENOMEM;
		return EFAULT;
	}

	/* Copies one page-bounded chunk at a time. */
	position = user_address - first;
	while (size != 0) {
		page = &pages[position / PAGE_SIZE];
		offset = position & (PAGE_SIZE - 1U);
		chunk = PAGE_SIZE - offset;
		if (chunk > size)
			chunk = size;
		if (page->kind == VMSPACE_PINNED_PRIVATE) {
			mapped = (uint8_t *)hal_pmem_to_kernel(page->memory.paddr) + offset;
			if (to_user) {
				kern_memcpy(mapped, bytes, chunk);
				vm_private_page_mark_dirty(
				    page->owner.private_page);
			} else {
				kern_memcpy(bytes, mapped, chunk);
			}

			/* Copies through whichever backing the pin holds. */
			error = 0;
		} else if (page->kind == VMSPACE_PINNED_DEVICE) {
			/* Device pins keep the ordered kernel alias alive for either copy direction. */
			if (to_user) {
				error = vm_device_write(
					page->owner.device,
					page->device_offset + offset,
					bytes,
					chunk);
			} else {
				error = vm_device_read(
					page->owner.device,
					page->device_offset + offset,
					bytes,
					chunk);
			}
		} else if (page->kind == VMSPACE_PINNED_OBJECT) {
			if (to_user)
				error = vm_object_page_pin_write(
				    page->owner.object_page, offset, bytes, chunk);
			else
				error = vm_object_page_pin_read(
				    page->owner.object_page, offset, bytes, chunk);
		} else {
			error = EFAULT;
		}

		if (error != 0)
			break;

		/* Advances past the bytes this pin carried. */
		bytes += chunk;
		position += chunk;
		size -= chunk;
	}

	/* Releases the pins, and the array when it was allocated. */
	vmspace_unpin_user_pages(pages, page_count);
	if (pages != stack_pages)
		kern_free(pages);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Copies kernel memory into a user range. */
static int
vmspace_copy_to_locked(
	struct vmspace *vm,
	uintptr_t destination,
	const void *source,
	size_t size)
{
	int error;

	/* Reports the failure. */
	error = copy_backing(vm, destination, (void *)source, size, HAL_SPACE_WRITE, 1);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Copies a user range into kernel memory. */
static int
vmspace_copy_from_locked(
	struct vmspace *vm,
	void *destination,
	uintptr_t source,
	size_t size)
{
	int error;

	/* Reports the failure. */
	error = copy_backing(vm, source, destination, size,
	    HAL_SPACE_READ, 0);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}
