/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Demand paging of address spaces: the page fault, its waits, the
 * file, copy-on-write and fault-around fills, and the queries of a
 * page's protection and residency.
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

static void vmspace_fault_around_locked(struct vmspace *vm, struct vm_region *region, uintptr_t address, int copy_on_write);
static int vmspace_fault_around_page_locked(struct vmspace *vm, struct vm_region *region, uintptr_t address, uint32_t prot, int copy_on_write);
static unsigned reclaim_page_map_reserve(struct vm_page *avoid);
static int vmspace_exec_cache_fault(struct vmspace *vm, struct vm_region *region, struct vm_page *page, uint32_t required);
static int allocate_page_frame(struct vm_private_page *backing, struct vm_page *avoid);
static int fill_file_page(struct vm_region *region, struct vm_page *page);

/*
 * Reports the protection a page is mapped with, which excludes write for
 * a copy-on-write page.
 */
uint32_t
vm_page_effective_prot(
	const struct vm_page *page)
{
	uint32_t prot;

	prot = page->region->prot;
	if ((page->flags & VM_MAPPING_COW) != 0)
		prot &= ~HAL_SPACE_WRITE;

	/* A visible device page preserves its backing cache policy and physical identity. */
	if (prot != 0 && page->region->device != NULL)
		prot |= vm_device_page_attributes(page->region->device);

	return prot;
}

/*
 * Tests whether a page's private backing currently holds a physical
 * frame.
 */
int
vm_private_page_is_resident(
	const struct vm_page *page)
{
	struct vm_private_page *backing;
	unsigned long irq;
	int resident;

	/* A page without a private backing is never resident. */
	if (page == NULL)
		return 0;
	backing = page->private_page;
	if (backing == NULL)
		return 0;

	/* Samples the residency of the backing. */
	irq = spin_lock_irqsave(&backing->state_lock);

	resident = (backing->flags & VM_PAGE_RESIDENT) != 0;

	spin_unlock_irqrestore(&backing->state_lock, irq);

	/* Reports the sampled residency. */
	return resident;
}

/*
 * Resolves a page fault by mapping the page an address belongs to.
 *
 * An existing private page is brought back from swap or copied for a
 * write to a copy-on-write mapping.  A missing page is filled from the
 * region's file, its shared object, or zeros.  Map-table pressure is
 * handled by reclaiming a small reserve and retrying the transaction.
 */
int
vmspace_fault(
	struct vmspace *vm,
	uintptr_t address,
	uint32_t required)
{
	struct vm_private_page *reserved_backing;
	struct vm_object_page *object_page;
	struct vm_region *region;
	struct vm_page *page;
	struct vm_page **link;
	hal_physaddr_t prepared_physical;
	uintptr_t page_address;
	uint64_t prepared_backing_generation;
	uint64_t reservation_generation;
	off_t object_offset;
	uint32_t new_reservation_prot;
	unsigned map_pressure_retries;
	int error;
	int mapped;
	int private_io_owned;
	int private_io_hold;
	uint64_t sequence;
	struct vm_private_page *fresh;
	uint32_t reservation_prot;
	unsigned long state_irq;
	int backing_owner;
	int pinned_copy;
	int cow_write;
	unsigned held_flags;
	struct kern_pmem pinned_memory;
	int need_cow;
	int was_mapped;
	int old_unmapped;
	int map_pressure;
	int retry_fault;
	int snapshot_cached;
	int cow_object;
	int swapped;
	uint64_t maximum_offset;
	unsigned long irq;

	/* Starts with nothing reserved, nothing mapped, and no error. */
	reserved_backing = NULL;
	object_page = NULL;
	prepared_physical = 0;
	page_address = address & ~(uintptr_t)(PAGE_SIZE - 1U);
	prepared_backing_generation = 0;
	object_offset = 0;
	new_reservation_prot = 0;
	map_pressure_retries = 0;
	error = 0;
	mapped = 0;
	private_io_owned = 0;
	private_io_hold = 0;

	/* Rejects a kernel space or an access the hardware cannot ask for. */
	if (vm == NULL ||
	    vm == &kernel_vmspace ||
	    (required != HAL_SPACE_READ &&
	    required != HAL_SPACE_WRITE &&
	    required != HAL_SPACE_EXEC))
		return EINVAL;

	/* Tells reclaim that a fault is in progress. */
	vm_reclaim_note_fault();

retry:
	/* Finds the region and any existing page, waiting out a BUSY one. */
	error = 0;
	vm_metadata_enter();
	mutex_lock(&vm->lock);

	/*
	 * Waits out a fork that is copying this address space.  Every
	 * fault advances the generation, which the fork checks after each
	 * batch of pages, and a new page would be linked where the fork
	 * has already passed; a thread that kept writing to the pages the
	 * fork made copy-on-write would make it start over without end.
	 */
	if (vm->forking) {
		sequence = waitq_sequence(&vm->fault_waitq);
		mutex_unlock(&vm->lock);
		vm_metadata_leave();
		vmspace_wait_fault_event(vm, sequence);
		goto retry;
	}

	/* Rejects an address no region covers with the access asked for. */
	region = vmspace_find_region_locked(vm, address, 1);
	if (region == NULL || (region->prot & required) == 0) {
		mutex_unlock(&vm->lock);
		vm_metadata_leave();
		return EFAULT;
	}

	/* Waits out a mapping another fault already owns. */
	page = vmspace_find_page(region, page_address);
	if (page != NULL && (page->flags & VM_MAPPING_BUSY) != 0) {
		sequence = waitq_sequence(&vm->fault_waitq);
		mutex_unlock(&vm->lock);
		vm_metadata_leave();
		vmspace_wait_fault_event(vm, sequence);
		goto retry;
	}

	/* Device storage remains resident; an absent PTE only needs remapping. */
	if (page != NULL && region->device != NULL) {
		if ((page->flags & VM_MAPPING_MAPPED) != 0) {
			mutex_unlock(&vm->lock);
			vm_metadata_leave();
			return 0;
		}

		/* The region hold prevents unmap or split while the HAL changes its PTE. */
		page->flags |= VM_MAPPING_BUSY;
		region->hold_count++;
		mutex_unlock(&vm->lock);
		vm_metadata_leave();
		/* Completes the held device mapping after all VM locks were released. */
		error = vmspace_device_fault(vm, region, page);
		if (error != 0)
			return error;

		/* Succeeded: the retained device page is now mapped with its original cache policy. */
		return 0;
	}

	/* An existing object mapping is served by the object cache. */
	if (page != NULL && page->object_page != NULL) {
		cow_object = region->snapshot != NULL ||
		    (region->flags & VM_REGION_PRIVATE_OBJECT) != 0;
		if (cow_object && (required == HAL_SPACE_WRITE ||
		    (page->flags & VM_MAPPING_MAPPED) == 0)) {
			page->flags |= VM_MAPPING_BUSY;
			region->hold_count++;
			mutex_unlock(&vm->lock);
			vm_metadata_leave();
			error = vmspace_exec_cache_fault(vm, region, page, required);
			if (error == EAGAIN)
				goto retry;
			return error;
		}

		mutex_unlock(&vm->lock);
		vm_metadata_leave();
		return 0;
	}

	/* An existing private page is revived, copied, or simply mapped. */
	if (page != NULL) {
		fresh = NULL;
		backing_owner = 0;
		pinned_copy = 0;
		cow_write = 0;
		need_cow = 0;
		was_mapped = 0;
		old_unmapped = 0;
		map_pressure = 0;
		retry_fault = 0;

		if (page->private_page == NULL) {
			mutex_unlock(&vm->lock);
			vm_metadata_leave();
			return EFAULT;
		}

		region->hold_count++;

		page->flags |= VM_MAPPING_BUSY;
		reserved_backing = page->private_page;

		vm_private_page_ref(reserved_backing);

		reservation_generation = vm->generation;
		reservation_prot = region->prot;
		cow_write = required == HAL_SPACE_WRITE &&
		    (page->flags & VM_MAPPING_COW) != 0;

		mutex_unlock(&vm->lock);
		vm_metadata_leave();

		/*
		 * Takes exclusive I/O ownership of the backing.
		 *
		 * A copy-on-write break of a page someone else has pinned is
		 * the exception.  Ownership waits for every pin to go, and the
		 * pin may belong to another process's system call blocked for
		 * as long as it likes (a write to a socket waiting for its
		 * peer, which may be this very process).  Copying needs only
		 * to read the page, and a shared copy-on-write page carries
		 * read pins alone, so the copy is made under a pin of this
		 * fault's own, which keeps the page resident and reclaim away
		 * from it, as fork() does for such a page.
		 */
		if (cow_write && vmspace_private_page_pinned_resident(reserved_backing) &&
		    vm_private_page_pin(reserved_backing, &pinned_memory) == 0) {
			pinned_copy = 1;
			error = 0;
		} else {
			error = vm_private_page_io_acquire(reserved_backing);
			if (error == 0)
				backing_owner = 1;
		}

		/* Waiting for another vmspace's owner may have changed this backing. */
		vm_metadata_enter();
		mutex_lock(&vm->lock);

		if (error == 0) {
			if (vmspace_find_region_locked(vm, address, 1) != region) {
				/* The region this reservation named is gone. */
				error = EAGAIN;
			} else if (vmspace_find_page(region, page_address) != page) {
				/* The region no longer holds this mapping. */
				error = EAGAIN;
			} else if (page->region != region) {
				/* The mapping now belongs to another region. */
				error = EAGAIN;
			} else if (page->address != page_address) {
				/* The mapping now covers another address. */
				error = EAGAIN;
			} else if (page->private_page != reserved_backing) {
				/* The mapping took a different backing. */
				error = EAGAIN;
			} else if (region->prot != reservation_prot) {
				/* The region's protection changed under the wait. */
				error = EAGAIN;
			} else if ((region->prot & required) == 0) {
				/* The region no longer permits this access. */
				error = EAGAIN;
			}
		}

		if (error == 0) {
			/* Full validation acknowledges unrelated generation changes. */
			reservation_generation = vm->generation;
			need_cow = required == HAL_SPACE_WRITE && (page->flags & VM_MAPPING_COW) != 0;
			was_mapped = (page->flags & VM_MAPPING_MAPPED) != 0;

			/* Without ownership, only the copy may be made; else start over. */
			if (pinned_copy && !need_cow)
				error = EAGAIN;
		}

		mutex_unlock(&vm->lock);
		vm_metadata_leave();

		/* Brings a swapped page back and prepares a COW copy. */
		if (error == 0 && backing_owner) {

			/* Samples the backing the copy will map, while it is still owned. */
			state_irq = spin_lock_irqsave(&reserved_backing->state_lock);
			swapped = (reserved_backing->flags & VM_PAGE_SWAPPED) != 0;
			spin_unlock_irqrestore(&reserved_backing->state_lock, state_irq);
			if (swapped)
				error = vm_private_page_in_owned(reserved_backing, page);
		}

		if (error == 0 && need_cow)
			error = vmspace_prepare_cow_copy(page, reserved_backing, &fresh);

		/*
		 * Samples what the copy must still match when it is installed.
		 * The owner holds the page busy; a pinned copy holds it
		 * resident.
		 */
		held_flags = backing_owner ? VM_PAGE_BUSY | VM_PAGE_RESIDENT :
		    VM_PAGE_RESIDENT;
		if (error == 0) {
			state_irq = spin_lock_irqsave(&reserved_backing->state_lock);
			if ((reserved_backing->flags &
			    (VM_PAGE_BUSY | VM_PAGE_RESIDENT)) != held_flags) {
				error = EIO;
			} else {
				prepared_backing_generation =
				    reserved_backing->generation;
				prepared_physical = reserved_backing->pmem.paddr;
			}

			spin_unlock_irqrestore(&reserved_backing->state_lock,
			    state_irq);
		}

		/* A replacement needs a shootdown, performed with no VM lock held. */
		if (error == 0 && fresh != NULL && was_mapped) {
			if (hal_space_unmap(vm->space, (void *)page_address,
			    PAGE_SIZE) != HAL_OK)
				error = EIO;
			else
				old_unmapped = 1;
		}

		/* Revalidates everything and installs the mapping. */
		vm_metadata_enter();
		mutex_lock(&vm->lock);

		if (old_unmapped)
			page->flags &= ~VM_MAPPING_MAPPED;

		/* Refuses the mapping if the backing moved since it was sampled. */
		if (error == 0) {
			state_irq = spin_lock_irqsave(&reserved_backing->state_lock);
			if ((reserved_backing->flags &
			    (VM_PAGE_BUSY | VM_PAGE_RESIDENT)) != held_flags ||
			    reserved_backing->generation !=
			    prepared_backing_generation ||
			    reserved_backing->pmem.paddr != prepared_physical)
				error = EAGAIN;
			spin_unlock_irqrestore(&reserved_backing->state_lock,
			    state_irq);
		}

		/* Refuses the mapping if anything the reservation named has changed. */
		if (error == 0 &&
		    (vm->generation != reservation_generation ||
		    vmspace_find_region_locked(vm, address, 1) != region ||
		    vmspace_find_page(region, page_address) != page ||
		    page->region != region ||
		    page->address != page_address ||
		    page->private_page != reserved_backing ||
		    region->prot != reservation_prot ||
		    (region->prot & required) == 0))
			error = EAGAIN;

		/* Installs the copy, giving up on map pressure so a reclaim can run. */
		if (error == 0 && fresh != NULL) {
			if (hal_space_map(vm->space, (void *)page_address,
			    fresh->pmem.paddr, PAGE_SIZE, region->prot) != HAL_OK) {
				error = ENOMEM;
				map_pressure = 1;
			} else {
				/* The mapping owns the creator reference. */
				vm_page_replace_private(page, fresh);
				fresh = NULL;
				page->flags &= ~VM_MAPPING_COW;
				page->flags |= VM_MAPPING_MAPPED;
			}
		} else if (error == 0 &&
		    (page->flags & VM_MAPPING_MAPPED) == 0) {
			if (hal_space_map(vm->space, (void *)page_address,
			    prepared_physical, PAGE_SIZE,
			    vm_page_effective_prot(page)) != HAL_OK) {
				error = ENOMEM;
				map_pressure = 1;
			} else {
				page->flags |= VM_MAPPING_MAPPED;
			}
		}

		/* Map-table pressure reclaims a reserve and retries. */
		if (map_pressure) {
			mutex_unlock(&vm->lock);
			vm_metadata_leave();
			if (map_pressure_retries < VM_PAGE_MAP_RECLAIM_RETRIES &&
			    reclaim_page_map_reserve(page) != 0) {
				map_pressure_retries++;
				error = EAGAIN;
			}

			/* Retakes the locks and traps if the reservation did not survive. */
			vm_metadata_enter();
			mutex_lock(&vm->lock);
			if (vmspace_find_region_locked(vm, address, 1) != region ||
			    vmspace_find_page(region, page_address) != page ||
			    page->private_page != reserved_backing ||
			    (page->flags & VM_MAPPING_BUSY) == 0 ||
			    region->hold_count == 0)
				HAL_FATAL("VM map-pressure reservation lost");
		}

		/* Releases the reservation and the backing. */
		page->flags &= ~VM_MAPPING_BUSY;
		if (region->hold_count == 0)
			HAL_FATAL("VM fault region hold underflow");

		region->hold_count--;
		if (error == 0)
			vmspace_generation_advance_locked(vm);

		retry_fault = error == EAGAIN;
		vmspace_fault_wake_locked(vm);
		mutex_unlock(&vm->lock);
		vm_metadata_leave();

		if (backing_owner)
			vm_private_page_io_release(reserved_backing);
		if (pinned_copy)
			vm_private_page_unpin(reserved_backing);

		vm_private_page_put(reserved_backing);

		if (fresh != NULL)
			vm_private_page_put(fresh);

		if (retry_fault)
			goto retry;

		return error;
	}

	/* Pin the region, then allocate a placeholder without holding the lock. */
	region->hold_count++;
	reservation_generation = vm->generation;

	mutex_unlock(&vm->lock);
	vm_metadata_leave();

	page = vm_page_alloc_metadata();
	if (page == NULL) {
		error = ENOMEM;
		goto release_region;
	}

	page->vm = vm;
	page->region = region;
	page->address = page_address;
	page->flags = VM_MAPPING_BUSY;

	/* Links the placeholder unless the page appeared meanwhile. */
	vm_metadata_enter();
	mutex_lock(&vm->lock);

	if (vmspace_find_region_locked(vm, address, 1) != region ||
	    (region->prot & required) == 0) {
		error = EFAULT;
		goto unlink_locked;
	}

	/* Starts over when another fault published this page first. */
	if (vmspace_find_page(region, page_address) != NULL) {
		region->hold_count--;
		vmspace_fault_wake_locked(vm);
		mutex_unlock(&vm->lock);
		vm_metadata_leave();
		vm_page_free_metadata(page);
		goto retry;
	}

	/* A generation change is allowed only after full region revalidation. */
	if (vm->generation != reservation_generation &&
	    vmspace_find_region_locked(vm, address, 1) != region) {
		error = EFAULT;
		goto unlink_locked;
	}

	/* Publishes the placeholder and samples what it is measured against. */
	page->next = region->pages;
	region->pages = page;
	vmspace_region_page_index_insert(region, page);
	reservation_generation = vm->generation;
	new_reservation_prot = region->prot;

	mutex_unlock(&vm->lock);
	vm_metadata_leave();

	/* The driver has already materialized and retained every device page. */
	if (region->device != NULL) {
		/* Completes the held device mapping after all VM locks were released. */
		error = vmspace_device_fault(vm, region, page);
		if (error != 0)
			return error;

		/* Succeeded: the retained device page is now mapped with its original cache policy. */
		return 0;
	}

	/*
	 * A private region maps its object's page only where the page is all
	 * file data; a page the data ends in, or past it, is filled below.
	 */
	cow_object = 0;
	if ((region->flags & VM_REGION_PRIVATE_OBJECT) != 0 &&
	    page_address >= region->data_start &&
	    page_address - region->data_start + PAGE_SIZE <= region->data_size)
		cow_object = 1;

	/* A shared object page is mapped straight from the object cache. */
	if ((region->object != NULL &&
	    ((region->flags & VM_REGION_PRIVATE_OBJECT) == 0 || cow_object)) ||
	    region->snapshot != NULL) {
		snapshot_cached = region->snapshot != NULL || cow_object;
		if (sizeof(off_t) == 8)
			maximum_offset = (uint64_t)INT64_MAX;
		else
			maximum_offset = (uint64_t)INT32_MAX;

		/* Refuses an offset the file cannot address. */
		if (region->file_offset < 0 ||
		    (uint64_t)region->file_offset +
		    (uint64_t)(page_address - region->start) > maximum_offset) {
			error = EOVERFLOW;
			goto remove_placeholder;
		}

		/* Refuses an offset outside the snapshot this region maps. */
		object_offset = region->file_offset + (off_t)(page_address - region->start);
		if (region->snapshot != NULL) {
			if (object_offset < region->snapshot->offset ||
			    (uint64_t)(object_offset - region->snapshot->offset) >= region->snapshot->length) {
				error = EFAULT;
				goto remove_placeholder;
			}
		}

		/* Reads the page in through the object, or through the snapshot. */
		error = vm_object_fault(region->snapshot != NULL ?
					region->snapshot->input.read_object :
					region->object,
					object_offset,
					&object_page);
		if (error != 0)
			goto remove_placeholder;

		/* A snapshot mapping must get the page the snapshot pinned. */
		if (region->snapshot != NULL) {
			if (object_page != region->snapshot->pages[
			    (size_t)(object_offset - region->snapshot->offset) / PAGE_SIZE]) {
				error = EIO;
				goto remove_placeholder;
			}
		}

		/* A snapshot's or a private region's page is copied on its first write. */
		if (snapshot_cached)
			page->flags |= VM_MAPPING_COW;

		/* Maps the object page, read-only when it is copied on write. */
		page->object_page = object_page;
		mapped = hal_space_map(vm->space,
				      (void *)page_address,
				      object_page->pmem.paddr,
				      PAGE_SIZE,
				      snapshot_cached ?
				      region->prot & ~HAL_SPACE_WRITE :
				      region->prot) == HAL_OK;
		if (!mapped) {
			/*
			 * The fault hold keeps this cache page out of object
			 * reclaim, while the BUSY placeholder and region hold
			 * keep its VM metadata stable.  Reclaim other pages
			 * with no VM lock held, then retire the placeholder
			 * and retry the full transaction.
			 */
			if (map_pressure_retries < VM_PAGE_MAP_RECLAIM_RETRIES &&
			    reclaim_page_map_reserve(page) != 0) {
				map_pressure_retries++;
				error = EAGAIN;
			} else {
				error = ENOMEM;
			}

			goto remove_placeholder;
		}

		page->flags |= VM_PAGE_RESIDENT | VM_MAPPING_MAPPED;

		vm_metadata_enter();
		mutex_lock(&vm->lock);

		if (vmspace_find_region_locked(vm, address, 1) != region ||
		    vmspace_find_page(region, page_address) != page)
			HAL_FATAL("lost VM fault reservation");

		vm_object_mapping_add(object_page, page);

		object_page = NULL;
		page->flags &= ~VM_MAPPING_BUSY;
		region->hold_count--;

		/* Maps the neighbours the object already holds, for a read. */
		if (required != HAL_SPACE_WRITE) {
			vmspace_fault_around_locked(vm, region, page_address,
			    snapshot_cached);
		}

		vmspace_generation_advance_locked(vm);
		vmspace_fault_wake_locked(vm);
		mutex_unlock(&vm->lock);
		vm_metadata_leave();

		if (required == HAL_SPACE_WRITE && snapshot_cached)
			goto retry;

		return 0;
	}

	/* A private page gets a new backing, a frame, and its contents. */
	page->private_page = vmspace_private_page_alloc();
	if (page->private_page == NULL) {
		error = ENOMEM;
		goto remove_placeholder;
	}

	vmspace_private_page_attach_new(page, page->private_page);
	reserved_backing = page->private_page;
	vm_private_page_ref(reserved_backing);
	private_io_hold = 1;

	error = vm_private_page_io_acquire(reserved_backing);
	if (error != 0)
		goto remove_placeholder;

	private_io_owned = 1;
	if (allocate_page_frame(page->private_page, page) != 0) {
		error = ENOMEM;
		goto remove_placeholder;
	}

	/* Fills the fresh page from the file, or with zeroes. */
	if (region->backing == VM_BACKING_FILE) {
		error = fill_file_page(region, page);
	} else {
		kern_memset((void *)hal_pmem_to_kernel(page->private_page->pmem.paddr), 0, PAGE_SIZE);
		error = 0;
	}

	/* Publishes the filled page as resident and samples what it became. */
	if (error == 0) {
		irq = spin_lock_irqsave(&page->private_page->state_lock);

		page->private_page->flags |= VM_PAGE_RESIDENT;

		page->private_page->generation++;
		if (page->private_page->generation == 0)
			page->private_page->generation++;

		prepared_backing_generation = page->private_page->generation;

		prepared_physical = page->private_page->pmem.paddr;

		spin_unlock_irqrestore(&page->private_page->state_lock, irq);
	}

	if (error != 0)
		goto remove_placeholder;

	/* Revalidates the reservation before mapping the frame. */
	vm_metadata_enter();
	mutex_lock(&vm->lock);

	if (error == 0) {
		irq = spin_lock_irqsave(&reserved_backing->state_lock);

		if ((reserved_backing->flags & (VM_PAGE_BUSY | VM_PAGE_RESIDENT)) != (VM_PAGE_BUSY | VM_PAGE_RESIDENT) ||
		    reserved_backing->generation != prepared_backing_generation ||
		    reserved_backing->pmem.paddr != prepared_physical)
			error = EAGAIN;

		spin_unlock_irqrestore(&reserved_backing->state_lock, irq);
	}

	if (error != 0)
		goto unlink_locked;

	/* Refuses the mapping if anything the reservation named has changed. */
	if (vm->generation != reservation_generation ||
	    vmspace_find_region_locked(vm, address, 1) != region ||
	    vmspace_find_page(region, page_address) != page ||
	    page->region != region ||
	    page->address != page_address ||
	    page->private_page != reserved_backing ||
	    region->prot != new_reservation_prot ||
	    (region->prot & required) == 0) {
		error = EAGAIN;
		goto unlink_locked;
	}

	if (hal_space_map(vm->space, (void *)page_address,
	    prepared_physical, PAGE_SIZE, region->prot) != HAL_OK) {
		/*
		 * The frame allocator already retries after reclaim, but the
		 * HAL can need another physical page for a new page-table
		 * level.  Drop the VM locks before reclaim, retire this
		 * incomplete fault reservation, and retry the complete
		 * transaction.  Releasing the prepared frame during rollback
		 * leaves both it and the reclaimed frame available to a HAL
		 * which needs separate allocation metadata and table storage.
		 */
		mutex_unlock(&vm->lock);
		vm_metadata_leave();
		if (map_pressure_retries < VM_PAGE_MAP_RECLAIM_RETRIES &&
		    reclaim_page_map_reserve(page) != 0) {
			map_pressure_retries++;
			error = EAGAIN;
		} else {
			error = ENOMEM;
		}

		goto remove_placeholder;
	}

	/* Publishes the mapped page and releases the backing. */
	mapped = 1;
	page->flags |= VM_MAPPING_MAPPED;
	vm_page_track(page);

	page->flags &= ~VM_MAPPING_BUSY;
	region->hold_count--;

	vmspace_generation_advance_locked(vm);
	vmspace_fault_wake_locked(vm);

	mutex_unlock(&vm->lock);
	vm_metadata_leave();

	vm_private_page_io_release(reserved_backing);
	private_io_owned = 0;
	vm_private_page_put(reserved_backing);
	private_io_hold = 0;

	return 0;

remove_placeholder:
	/* Undoes a partial mapping and drops an object fault hold. */
	if (mapped)
		(void)hal_space_unmap(vm->space, (void *)page_address, PAGE_SIZE);

	if (object_page != NULL) {
		vm_object_fault_release(object_page);
		object_page = NULL;
	}

	vm_metadata_enter();
	mutex_lock(&vm->lock);

unlink_locked:
	/* Unlinks the placeholder and releases the region hold. */
	link = &region->pages;
	while (*link != NULL && *link != page)
		link = &(*link)->next;
	if (*link == page) {
		*link = page->next;
		vmspace_region_page_index_remove(region, page);
	}

	if (region->hold_count == 0)
		HAL_FATAL("VM fault region hold underflow");
	region->hold_count--;

	vmspace_fault_wake_locked(vm);

	mutex_unlock(&vm->lock);
	vm_metadata_leave();

	if (page->private_page != NULL)
		vm_page_untrack(page);

	if (private_io_owned) {
		vm_private_page_io_release(reserved_backing);
		private_io_owned = 0;
	}

	if (private_io_hold) {
		vm_private_page_put(reserved_backing);
		private_io_hold = 0;
	}

	vm_page_free_metadata(page);

	if (error == EAGAIN)
		goto retry;

	return error;

release_region:
	/* Releases the region hold this fault took and wakes the waiters. */
	vm_metadata_enter();
	mutex_lock(&vm->lock);

	if (region->hold_count == 0)
		HAL_FATAL("VM fault region hold underflow");
	region->hold_count--;

	vmspace_fault_wake_locked(vm);

	mutex_unlock(&vm->lock);
	vm_metadata_leave();

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Reports whether a backing is held only by pins, with its page in memory.
 *
 * Such a page cannot be shared copy-on-write, because whoever pinned it
 * may still write into it; but it can be copied outright, since a pin
 * keeps the page in memory and keeps reclaim away from it.
 */
int
vmspace_private_page_pinned_resident(
	struct vm_private_page *backing)
{
	unsigned long irq;
	int result;

	irq = spin_lock_irqsave(&backing->state_lock);
	result = backing->pin_count != 0 &&
	    backing->active_operations == 0 &&
	    (backing->flags & (VM_PAGE_BUSY | VM_PAGE_RESIDENT)) ==
	    VM_PAGE_RESIDENT;
	spin_unlock_irqrestore(&backing->state_lock, irq);
	return result;
}

/*
 * Sleeps until the fault wait queue advances past an observed sequence.
 */
void
vmspace_wait_fault_event(
	struct vmspace *vm,
	uint64_t sequence)
{
	unsigned long irq;

	irq = spin_lock_irqsave(&vm->lock.guard);

	(void)waitq_sleep(&vm->fault_waitq, &vm->lock.guard, sequence, 0, 0);

	spin_unlock_irqrestore(&vm->lock.guard, irq);
}

/*
 * Waits until no region is held and no page is BUSY; the caller holds the VM
 * locks.
 */
void
vmspace_wait_faults_locked(
	struct vmspace *vm)
{
	struct vm_region *region;
	int busy;
	uint64_t sequence;

	for (;;) {
		/*
		 * Scans for any fault in progress.  Every mapping marked BUSY
		 * also holds its region, so the region holds alone say whether
		 * one is; the pages need not be walked.
		 */
		busy = 0;
		for (region = vm->regions; region != NULL; region = region->next) {
			if (region->hold_count != 0) {
				busy = 1;
				break;
			}
		}

		if (!busy)
			return;

		/* Fault completion needs the outer cross-vm metadata lock. */
		sequence = waitq_sequence(&vm->fault_waitq);
		mutex_unlock(&vm->lock);
		vm_metadata_leave();
		vmspace_wait_fault_event(vm, sequence);
		vm_metadata_enter();
		mutex_lock(&vm->lock);
	}
}

/*
 * Builds an unlinked copy of a private page while the fault owns the old one.
 */
int
vmspace_prepare_cow_copy(
	struct vm_page *page,
	struct vm_private_page *old,
	struct vm_private_page **result)
{
	struct vm_private_page *fresh;
	unsigned long irq;

	if (page == NULL || old == NULL || result == NULL)
		return EINVAL;
	/* Allocates a backing and a frame for the private copy. */
	fresh = vmspace_private_page_alloc();
	if (fresh == NULL)
		return ENOMEM;

	if (allocate_page_frame(fresh, page) != 0) {
		vm_private_page_put(fresh);
		return ENOMEM;
	}

	/* Copies the shared page and publishes the copy as resident and dirty. */
	kern_memcpy((void *)hal_pmem_to_kernel(fresh->pmem.paddr), (const void *)hal_pmem_to_kernel(old->pmem.paddr),
	    PAGE_SIZE);
	irq = spin_lock_irqsave(&fresh->state_lock);

	fresh->flags = VM_PAGE_RESIDENT | VM_PAGE_DIRTY;
	fresh->generation++;
	if (fresh->generation == 0)
		fresh->generation++;

	spin_unlock_irqrestore(&fresh->state_lock, irq);

	*result = fresh;

	/* Reports the prepared copy. */
	return 0;
}

/*
 * Wakes every waiter on the fault queue; takes only the guard spinlock.
 */
void
vmspace_fault_wake_locked(
	struct vmspace *vm)
{
	unsigned long irq;

	irq = spin_lock_irqsave(&vm->lock.guard);

	waitq_wake_all(&vm->fault_waitq);

	spin_unlock_irqrestore(&vm->lock.guard, irq);
}

/*
 * Reclaims a small reserve of private pages for page-table growth.
 *
 * Reports nonzero when the fault should try again.  A reclaim that finds
 * every candidate busy, typically with swap writes of its own, waits a
 * little instead of failing the fault: commit is charged before a page is
 * faulted, so the memory exists and only has to be written out.  The
 * callers bound the retries.
 */
static unsigned
reclaim_page_map_reserve(
	struct vm_page *avoid)
{
	unsigned reclaimed;
	int owned;

	/*
	 * A page-table miss can require more than the one page returned by
	 * a normal frame-allocation retry.  In particular, once the fixed
	 * kernel heap is full, the i386 HAL needs one page-backed descriptor
	 * and one PTE page at a new PDE boundary.  Reclaim a small bounded
	 * reserve while the fault's own mapping is BUSY, then retry the
	 * complete fault transaction.
	 */
	for (reclaimed = 0; reclaimed < VM_PAGE_MAP_RESERVE_PAGES; reclaimed++) {
		if (vm_reclaim_private_one(avoid) != 0)
			break;
	}

	/* Waits for busy candidates when nothing could be taken at once. */
	if (reclaimed == 0) {
		owned = vm_metadata_owned();
		if (owned)
			return 0;
		vm_reclaim_wait_free();
		return 1;
	}

	/* Reports the reserve taken. */
	return reclaimed;
}

/* Allocates a frame for a private backing, reclaiming one page on failure. */
static int
allocate_page_frame(
	struct vm_private_page *backing,
	struct vm_page *avoid)
{
	int error;

	/* Takes a frame, reclaiming until one is free. */
	error = vm_reclaim_frame(&backing->pmem, avoid);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Fills a private page from the region's file, zeroing the rest. */
static int
fill_file_page(
	struct vm_region *region,
	struct vm_page *page)
{
	uintptr_t page_end;
	uintptr_t data_end;
	uintptr_t read_start;
	uintptr_t read_end;
	uint64_t offset_max;
	size_t length;
	off_t offset;
	ssize_t count;
	uint8_t *frame;

	/* Intersects the page with the region's data range. */
	page_end = page->address + PAGE_SIZE;
	data_end = region->data_start + region->data_size;
	if (page->address > region->data_start)
		read_start = page->address;
	else
		read_start = region->data_start;
	if (page_end < data_end)
		read_end = page_end;
	else
		read_end = data_end;

	/* A page outside the data is zero only for an ELF zero tail. */
	frame = (uint8_t *)hal_pmem_to_kernel(page->private_page->pmem.paddr);
	if (read_start >= read_end) {
		kern_memset(frame, 0, PAGE_SIZE);
		if ((region->flags & VM_REGION_ELF_ZERO_TAIL) != 0)
			return 0;
		return ENXIO;
	}

	/* Zeroes only the bytes before and after the data the read will fill. */
	kern_memset(frame, 0, read_start - page->address);
	kern_memset(frame + (read_end - page->address), 0, page_end - read_end);

	/* Reads the overlapping bytes at their file offset. */
	length = read_end - read_start;
	if (sizeof(off_t) == sizeof(int64_t))
		offset_max = (uint64_t)INT64_MAX;
	else
		offset_max = (uint64_t)INT32_MAX;

	if (region->file_offset < 0 ||
	    (uint64_t)(read_start - region->data_start) >
	    offset_max - (uint64_t)region->file_offset) {
		kern_memset(frame, 0, PAGE_SIZE);
		return EOVERFLOW;
	}

	offset = region->file_offset +
		(off_t)(read_start - region->data_start);

	/* Reads the data into the middle of the page. */
	count = file_pread(region->file, frame + (read_start - page->address), length, offset);
	if (count == (ssize_t)length)
		return 0;

	/* A short read is a device error and leaves a zero page. */
	kern_memset(frame, 0, PAGE_SIZE);
	return EIO;
}

/* Caller reserved BUSY page/region ownership and released both VM locks. */
static int
vmspace_exec_cache_fault(
	struct vmspace *vm,
	struct vm_region *region,
	struct vm_page *page,
	uint32_t required)
{
	struct vm_object_page *source;
	struct vm_private_page *fresh;
	uintptr_t address;
	uint32_t prot;
	unsigned long irq;
	int error, writing, unmapped;
	int pinned;
	int waiting;
	int pin_error;

	/* The region's snapshot pins the immutable source during allocation and copy. */
	source = page->object_page;
	address = page->address;
	prot = region->prot;
	writing = required == HAL_SPACE_WRITE;
	fresh = NULL;
	error = 0;
	unmapped = 0;
	waiting = 0;
	if (writing) {
		fresh = vmspace_private_page_alloc();
		if (fresh == NULL)
			error = ENOMEM;
		if (error == 0)
			error = allocate_page_frame(fresh, page);
		if (error == 0) {
			irq = spin_lock_irqsave(&fresh->state_lock);
			fresh->flags = VM_PAGE_RESIDENT;
			fresh->generation++;
			if (fresh->generation == 0)
				fresh->generation++;
			spin_unlock_irqrestore(&fresh->state_lock, irq);

			/*
			 * A snapshot pins its pages; a private region's page is
			 * only mapped, so it is pinned for the copy.
			 */
			pinned = 0;
			if (region->snapshot == NULL) {
				error = vm_object_page_pin(source);
				if (error == 0)
					pinned = 1;
			}

			/*
			 * Copies the page into the private one.  A sync that owns
			 * the page may be revoking its mappings, and waits for this
			 * reserved one (BUG-082): the reservation is given up
			 * first, and the page is waited out with a pin of its own.
			 */
			if (error == 0)
				error = vm_object_page_pin_read_nowait(source, 0, hal_pmem_to_kernel(fresh->pmem.paddr), PAGE_SIZE);
			if (error == EBUSY) {
				pin_error = vm_object_page_pin(source);
				if (pin_error == 0)
					waiting = 1;
				error = EAGAIN;
			}
			if (pinned)
				vm_object_page_unpin(source);
			if (error == 0)
				vm_private_page_mark_dirty(fresh);
		}
	}

	/* Replaces a PTE only after the complete private copy is ready. */
	if (error == 0 && writing && (page->flags & VM_MAPPING_MAPPED) != 0) {
		if (hal_space_unmap(vm->space, (void *)address, PAGE_SIZE) != HAL_OK)
			error = EIO;
		else
			unmapped = 1;
	}

	/* Revalidates everything and installs the mapping. */
	vm_metadata_enter();
	mutex_lock(&vm->lock);

	if ((page->flags & VM_MAPPING_BUSY) == 0 || region->hold_count == 0)
		HAL_FATAL("exec cache fault lost reservation");

	if (unmapped)
		page->flags &= ~VM_MAPPING_MAPPED;

	if (error == 0 && (vmspace_find_region_locked(vm, address, 1) != region ||
	    vmspace_find_page(region, address) != page || page->object_page != source ||
	    (region->snapshot == NULL && (region->flags & VM_REGION_PRIVATE_OBJECT) == 0) ||
	    region->prot != prot || (prot & required) == 0))
		error = EAGAIN;

	if (error == 0 && hal_space_map(vm->space, (void *)address,
	    writing ? fresh->pmem.paddr : source->pmem.paddr, PAGE_SIZE,
	    writing ? prot : prot & ~HAL_SPACE_WRITE) != HAL_OK)
		error = ENOMEM;

	/* Publishes the mapping, taking a private copy when this was a write. */
	if (error == 0) {
		if (writing) {
			vm_object_mapping_remove(source, page);
			page->object_page = NULL;
			page->object_next = NULL;
			vmspace_private_page_attach_new(page, fresh);
			fresh = NULL;
			vm_page_track(page);
			page->flags &= ~VM_MAPPING_COW;
		}

		page->flags |= VM_MAPPING_MAPPED;
		vmspace_generation_advance_locked(vm);
	}

	/* Releases the reservation and wakes the faults that waited. */
	page->flags &= ~VM_MAPPING_BUSY;
	region->hold_count--;
	vmspace_fault_wake_locked(vm);

	mutex_unlock(&vm->lock);
	vm_metadata_leave();

	if (fresh != NULL)
		vm_private_page_put(fresh);

	/* Waits out the write-back with no reservation held, then retries. */
	if (waiting) {
		(void)vm_object_page_pin_wait(source);
		vm_object_page_unpin(source);
	}

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Maps the neighbours of a faulted object page that the object already
 * caches; the caller holds the VM locks.
 *
 * A process that touches one page of a file mapping touches its
 * neighbours soon after, and each would otherwise take a fault of its own.
 * The window is the aligned group of VM_FAULT_AROUND_PAGES around the
 * fault, cut to the region and to what the region maps from its object.
 * Only pages that are cached and idle are mapped, read-only when the region
 * copies on write; nothing is read and nothing waits, so a neighbour that is
 * not ready simply takes its own fault later.
 *
 * A shared writable region is left alone: its pages are mapped writable at
 * their fault, and a read-only neighbour would never be upgraded.
 */
static void
vmspace_fault_around_locked(
	struct vmspace *vm,
	struct vm_region *region,
	uintptr_t address,
	int copy_on_write)
{
	uintptr_t first;
	uintptr_t last;
	uintptr_t window;
	uintptr_t data_end;
	uintptr_t current;
	uint32_t prot;
	int mapped;

	/* Leaves a shared writable region and an inaccessible one alone. */
	prot = region->prot;
	if (!copy_on_write && (prot & HAL_SPACE_WRITE) != 0)
		return;
	if (copy_on_write)
		prot &= ~HAL_SPACE_WRITE;
	if ((prot & (HAL_SPACE_READ | HAL_SPACE_EXEC)) == 0)
		return;

	/* Takes the aligned window around the fault, cut to the region. */
	window = (uintptr_t)VM_FAULT_AROUND_PAGES * PAGE_SIZE;
	first = address & ~(window - 1U);
	last = first + window;
	if (first < region->start)
		first = region->start;
	if (last > region->start + region->size)
		last = region->start + region->size;

	/* A private region maps from its object only whole pages of file data. */
	if ((region->flags & VM_REGION_PRIVATE_OBJECT) != 0) {
		data_end = region->data_start +
		    (region->data_size & ~(size_t)(PAGE_SIZE - 1U));
		if (first < region->data_start)
			first = region->data_start;
		if (last > data_end)
			last = data_end;
	}

	/* Maps each neighbour until one cannot be mapped. */
	for (current = first; current < last; current += PAGE_SIZE) {
		if (current == address)
			continue;
		mapped = vmspace_fault_around_page_locked(vm, region, current,
		    prot, copy_on_write);
		if (mapped < 0)
			break;
	}
}

/*
 * Maps one neighbour for fault-around; the caller holds the VM locks.
 *
 * Reports 1 for a page mapped, 0 for one skipped, and -1 when metadata
 * or page-table memory ran out, which ends the window.
 */
static int
vmspace_fault_around_page_locked(
	struct vmspace *vm,
	struct vm_region *region,
	uintptr_t address,
	uint32_t prot,
	int copy_on_write)
{
	struct vm_object_page *object_page;
	struct vm_object *object;
	struct vm_page *page;
	off_t offset;
	size_t index;
	int error;

	/* Skips an address that already has a mapping of its own. */
	page = vmspace_find_page(region, address);
	if (page != NULL)
		return 0;

	/* Names the object offset of the address. */
	offset = region->file_offset + (off_t)(address - region->start);
	if (offset < 0)
		return 0;

	/* A snapshot maps only the pages it pinned, from its read object. */
	index = 0;
	if (region->snapshot != NULL) {
		if (offset < region->snapshot->offset ||
		    (uint64_t)(offset - region->snapshot->offset) >=
		    region->snapshot->length)
			return 0;
		index = (size_t)(offset - region->snapshot->offset) / PAGE_SIZE;
		object = region->snapshot->input.read_object;
	} else {
		object = region->object;
	}

	/* Skips a region with no object to map from. */
	if (object == NULL)
		return 0;

	/* Holds the cached page, or leaves the address to its own fault. */
	object_page = NULL;
	error = vm_object_fault_resident(object, offset, &object_page);
	if (error != 0)
		return 0;
	if (region->snapshot != NULL &&
	    object_page != region->snapshot->pages[index]) {
		vm_object_fault_release(object_page);
		return 0;
	}

	/* Builds the mapping descriptor. */
	page = vm_page_alloc_metadata();
	if (page == NULL) {
		vm_object_fault_release(object_page);
		return -1;
	}

	/* Names the region, address and object page the descriptor maps. */
	page->vm = vm;
	page->region = region;
	page->address = address;
	page->object_page = object_page;
	page->flags = VM_PAGE_RESIDENT;
	if (copy_on_write)
		page->flags |= VM_MAPPING_COW;

	/* Installs the translation. */
	error = hal_space_map(vm->space, (void *)address,
	    object_page->pmem.paddr, PAGE_SIZE, prot);
	if (error != HAL_OK) {
		vm_page_free_metadata(page);
		vm_object_fault_release(object_page);
		return -1;
	}

	/* Records the translation. */
	page->flags |= VM_MAPPING_MAPPED;

	/* Publishes the mapping in the region and in the page's reverse map. */
	page->next = region->pages;
	region->pages = page;
	vmspace_region_page_index_insert(region, page);
	vm_object_mapping_add(object_page, page);

	/* Reports the page mapped. */
	return 1;
}
