/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The copy of an address space for fork, sharing private pages
 * copy-on-write in batches.
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
 * One private page of a fork batch: the parent's mapping, the child's,
 * the backing they share, and how the child is mapped.
 *
 * An entry lives in a fork's batch on the stack from the page's share
 * until the batch is released.
 */
struct vmspace_fork_entry {
	struct vm_page *source_page;
	struct vm_page *copy_page;
	struct vm_private_page *backing;
	hal_physaddr_t physical;	/* where the page is, when it is resident */
	uintptr_t page_address;
	uint32_t cow_prot;		/* the protection both are mapped with */
	int eager;			/* the child got its own copy now */
	int resident;			/* the page is in memory, so it is mapped now */
	int source_mapped;		/* the parent had it mapped */
	int child_mapped;		/* the child's PTE was made */
};

static int vmspace_fork_locked(struct vmspace *source, struct vmspace **result, struct vm_private_page **wait_backing, struct vmspace **failed_copy);
static int fork_region(struct vmspace *source, struct vmspace *copy, struct vm_region *source_region, struct vm_private_page **wait_backing);
static int fork_batch_share(struct vmspace *source, struct vmspace *copy, struct vm_region *source_region, struct vm_region *copy_region, struct vm_page **cursor, struct vmspace_fork_entry *batch, unsigned *count, struct vm_private_page **wait_backing);
static int fork_share_page(struct vmspace *source, struct vm_region *source_region, struct vm_region *copy_region, struct vm_page *source_page, struct vm_page *copy_page, struct vmspace_fork_entry *entry, struct vm_private_page **wait_backing);
static int fork_batch_map(struct vmspace *source, struct vmspace *copy, struct vmspace_fork_entry *batch, unsigned count);
static void fork_batch_release(struct vmspace *source, struct vm_region *source_region, struct vmspace_fork_entry *batch, unsigned count);

/*
 * Copies a vmspace for fork, sharing private pages copy-on-write.
 *
 * A page whose backing is busy is waited for outside the locks and the
 * whole copy is retried.  While a copy runs, the source's faults wait
 * for it, so only a change of the layout itself (a mapping made, a
 * page reclaimed) makes it start over.
 */
int
vmspace_fork(
	struct vmspace *source,
	struct vmspace **result)
{
	struct vm_private_page *wait_backing;
	struct vmspace *failed_copy;
	uint64_t sequence;
	int error;
	int wait_error;

	/* Rejects the kernel's address space and a missing output. */
	if (source == NULL || source == &kernel_vmspace || result == NULL)
		return EINVAL;

	/* No copy exists yet. */
	*result = NULL;

	/* Copies, again after a busy backing or a change of the source. */
	for (;;) {
		/* Copies under the source locks. */
		wait_backing = NULL;
		failed_copy = NULL;
		vm_metadata_enter();
		mutex_lock(&source->lock);

		/*
		 * Waits for another fork of the same space to end: each
		 * advances the generation after every batch, so two at once
		 * would keep making each other start over.
		 */
		while (source->forking) {
			sequence = waitq_sequence(&source->fault_waitq);
			mutex_unlock(&source->lock);
			vm_metadata_leave();
			vmspace_wait_fault_event(source, sequence);
			vm_metadata_enter();
			mutex_lock(&source->lock);
		}

		/* Holds the source's faults off until this copy ends. */
		source->forking = 1;

		/* A copy made changes the source's layout: its PTEs are read-only. */
		error = vmspace_fork_locked(source, result, &wait_backing, &failed_copy);
		if (error == 0)
			vmspace_generation_advance_locked(source);

		/* Lets the faults that waited for the copy go on. */
		source->forking = 0;
		vmspace_fault_wake_locked(source);

		/* Leaves the source locks. */
		mutex_unlock(&source->lock);
		vm_metadata_leave();

		/* Retires a failed copy. */
		if (failed_copy != NULL)
			vmspace_put(failed_copy);

		/* Waits out a busy backing, then copies again. */
		if (error == EBUSY && wait_backing != NULL) {
			wait_error = vm_private_page_wait_idle(wait_backing);
			vm_private_page_put(wait_backing);
			if (wait_error == 0 || wait_error == EAGAIN)
				continue;
			return wait_error;
		}

		/* A source that changed while it was unlocked is copied again. */
		if (error != EAGAIN)
			break;
	}

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded: *result is the child's address space. */
	return 0;
}

/* Copies a vmspace's regions and shares its private pages; the caller holds the source locks. */
static int
vmspace_fork_locked(
	struct vmspace *source,
	struct vmspace **result,
	struct vm_private_page **wait_backing,
	struct vmspace **failed_copy)
{
	struct vmspace *copy;
	struct vm_region *source_region;
	int error;

	/* Rejects a kernel source or a missing output. */
	if (source == NULL ||
	    source == &kernel_vmspace ||
	    result == NULL ||
	    wait_backing == NULL ||
	    failed_copy == NULL)
		return EINVAL;

	/* Starts with nothing to wait for and no failed copy. */
	*wait_backing = NULL;
	*failed_copy = NULL;

	/* Waits for every fault in the source to settle, then makes the copy. */
	vmspace_wait_faults_locked(source);
	copy = vmspace_create();
	if (copy == NULL)
		return ENOMEM;

	/* The copy has the source's limits. */
	copy->address_limit = source->address_limit;
	copy->data_limit = source->data_limit;
	copy->stack_limit = source->stack_limit;

	/* Copies each region, then shares each of its private pages. */
	for (source_region = source->regions;
	     source_region != NULL;
	     source_region = source_region->next) {
		error = fork_region(source, copy, source_region, wait_backing);
		if (error != 0) {
			/* Child teardown may free pages or sync objects, so retire it locklessly. */
			*failed_copy = copy;
			return error;
		}
	}

	/* Copies the layout bookkeeping. */
	copy->entry = source->entry;
	copy->brk_start = source->brk_start;
	copy->brk_current = source->brk_current;
	copy->static_data_bytes = source->static_data_bytes;
	copy->stack_guard_bottom = source->stack_guard_bottom;
	copy->stack_bottom = source->stack_bottom;
	copy->stack_top = source->stack_top;
	*result = copy;

	/* Succeeded: the copy is complete. */
	return 0;
}

/*
 * Copies one region of a fork into the child and shares its private
 * pages; the caller holds the source locks, which are let go and taken
 * again for each batch of pages.  Returns 0 or the error that ends the
 * fork.
 */
static int
fork_region(
	struct vmspace *source,
	struct vmspace *copy,
	struct vm_region *source_region,
	struct vm_private_page **wait_backing)
{
	struct vmspace_fork_entry batch[VM_FORK_BATCH];
	struct vm_region *copy_region;
	struct vm_page *source_page;
	uint64_t reservation_generation;
	unsigned count;
	int batch_error;
	int map_error;
	int error;

	/* Copies the region's layout into the child. */
	error = vmspace_map_region(copy,
			   source_region->start,
			   source_region->size,
			   source_region->prot,
			   source_region->backing,
			   source_region->file,
			   source_region->file_offset,
			   source_region->data_start,
			   source_region->data_size,
			   source_region->flags,
			   source_region->commit_size,
			   &copy_region);
	if (error != 0)
		return error;

	/* The child may never raise the protection further than the parent may. */
	copy_region->max_prot = source_region->max_prot;

	/* The child shares the program snapshot the region maps. */
	if (source_region->snapshot != NULL) {
		file_exec_snapshot_ref(source_region->snapshot);
		copy_region->snapshot = source_region->snapshot;
	}

	/* Forked device regions share the retained extent and fault their own PTEs lazily. */
	if (source_region->device != NULL) {
		vm_device_ref(source_region->device);
		copy_region->device = source_region->device;
		copy_region->device_offset = source_region->device_offset;
		return 0;
	}

	/* The child shares the region's object. */
	if (source_region->object != NULL) {
		vm_object_ref(source_region->object);
		copy_region->object = source_region->object;

		/*
		 * Shared object pages are mapped lazily in the child.  A
		 * private region also has pages it copied on write, which
		 * the loop below shares with the child.
		 */
		if ((source_region->flags & VM_REGION_PRIVATE_OBJECT) == 0)
			return 0;
	}

	/*
	 * Gives the child a mapping descriptor for every one the parent
	 * has, a batch of pages at a time: each batch is shared under
	 * the VM locks, the parent's PTEs are made read-only and the
	 * child's mapped with no VM lock held, and the batch is
	 * released under the locks again.  The pages are held BUSY and
	 * the region held meanwhile, as one page was before.
	 */
	source_page = source_region->pages;
	while (source_page != NULL) {
		/* Shares a batch of pages under the locks. */
		count = 0;
		reservation_generation = source->generation;
		batch_error = fork_batch_share(source,
					       copy,
					       source_region,
					       copy_region,
					       &source_page,
					       batch,
					       &count,
					       wait_backing);

		/* Nothing was shared in this batch. */
		if (count == 0) {
			if (batch_error != 0)
				return batch_error;
			continue;
		}

		/* Downgrades the source and maps the child with no VM lock held. */
		mutex_unlock(&source->lock);
		vm_metadata_leave();
		map_error = fork_batch_map(source, copy, batch, count);

		/* Takes the VM locks back. */
		vm_metadata_enter();
		mutex_lock(&source->lock);

		/* Releases each pin after checking that nothing moved. */
		fork_batch_release(source, source_region, batch, count);

		/* The next batch starts after the last page of this one. */
		source_page = batch[count - 1U].source_page->next;

		/* The page after the batch was where it failed; the batch itself is released. */
		if (batch_error != 0)
			return batch_error;

		/* A change of the source's layout while it was unlocked fails the fork. */
		if (map_error == 0 && source->generation != reservation_generation)
			map_error = EAGAIN;
		if (map_error != 0)
			return map_error;

		/* The source's layout changed: its PTEs are read-only now. */
		vmspace_generation_advance_locked(source);
	}

	/* Succeeded: the region and its pages are the child's too. */
	return 0;
}

/*
 * Shares the next batch of a region's private pages with the child under
 * the VM locks, from *cursor on: up to VM_FORK_BATCH pages go into batch
 * (*count of them), and *cursor moves past them.  Returns 0, or the error
 * of the page it stopped at (the pages before it stay in the batch).
 */
static int
fork_batch_share(
	struct vmspace *source,
	struct vmspace *copy,
	struct vm_region *source_region,
	struct vm_region *copy_region,
	struct vm_page **cursor,
	struct vmspace_fork_entry *batch,
	unsigned *count,
	struct vm_private_page **wait_backing)
{
	struct vm_page *source_page;
	struct vm_page *copy_page;
	struct vm_page *next_page;
	int error;

	/* Shares each page until the batch is full or the region ends. */
	while (*cursor != NULL && *count < VM_FORK_BATCH) {
		/* Makes the child's mapping descriptor. */
		source_page = *cursor;
		next_page = source_page->next;
		copy_page = vm_page_alloc_metadata();
		if (copy_page == NULL)
			return ENOMEM;
		copy_page->vm = copy;
		copy_page->region = copy_region;
		copy_page->address = source_page->address;

		/* MAP_SHARED pages remain lazy in the child. */
		if (source_page->object_page != NULL) {
			vm_page_free_metadata(copy_page);
			*cursor = next_page;
			continue;
		}

		/* A private mapping without its backing cannot be shared. */
		if (source_page->private_page == NULL) {
			vm_page_free_metadata(copy_page);
			return EFAULT;
		}

		/* Shares the page; a failure has released what the page took. */
		error = fork_share_page(source,
					source_region,
					copy_region,
					source_page,
					copy_page,
					&batch[*count],
					wait_backing);
		if (error != 0)
			return error;

		/* The page is in the batch. */
		(*count)++;
		*cursor = next_page;
	}

	/* Succeeded: the batch is full, or the region has no more pages. */
	return 0;
}

/*
 * Shares one private page with the child under the VM locks and fills its
 * batch entry.  The source mapping is held BUSY and its region held until
 * the batch is released.  A page that fails is released here: with EBUSY
 * the backing to wait for goes to *wait_backing.
 */
static int
fork_share_page(
	struct vmspace *source,
	struct vm_region *source_region,
	struct vm_region *copy_region,
	struct vm_page *source_page,
	struct vm_page *copy_page,
	struct vmspace_fork_entry *entry,
	struct vm_private_page **wait_backing)
{
	struct vm_private_page *backing;
	struct vm_private_page *fresh;
	unsigned long state_irq;
	int pinned;
	int error;

	/*
	 * Pin the source metadata before the backing share.  The backing
	 * operation excludes reclaim/pins, while the local BUSY marker makes
	 * unmap/protect/fault wait when the VM locks are dropped for the
	 * source PTE shootdown.  The region's hold keeps it while the page
	 * is BUSY.
	 */
	if (source_region->hold_count == (unsigned)-1)
		HAL_FATAL("VM fork region hold overflow");
	source_region->hold_count++;

	/* Takes the source mapping and samples what the child will share. */
	source_page->flags |= VM_MAPPING_BUSY;
	backing = source_page->private_page;
	entry->source_page = source_page;
	entry->copy_page = copy_page;
	entry->backing = backing;
	entry->page_address = source_page->address;
	entry->cow_prot = source_region->prot & ~HAL_SPACE_WRITE;
	entry->source_mapped = 0;
	if ((source_page->flags & VM_MAPPING_MAPPED) != 0)
		entry->source_mapped = 1;
	entry->child_mapped = 0;
	entry->physical = 0;

	/* Read-only private mappings also need COW for later mprotect. */
	entry->eager = 0;
	error = vm_page_share_private(source_page, copy_page);

	/*
	 * A page another thread of the parent has pinned, for a read that may
	 * not finish until long after this fork, is copied now rather than
	 * waited for: the child gets its own page holding what the parent's
	 * held at this moment, which is all a fork ever promises.
	 */
	if (error == EBUSY) {
		pinned = vmspace_private_page_pinned_resident(backing);
		if (pinned) {
			error = vmspace_prepare_cow_copy(source_page, backing, &fresh);
			if (error == 0) {
				vmspace_private_page_attach_new(copy_page, fresh);
				vm_page_track(copy_page);
				entry->eager = 1;
			}
		}
	}

	/* Releases what the failed page took; a busy backing is waited for. */
	if (error != 0) {
		if (error == EBUSY) {
			*wait_backing = backing;
			vm_private_page_ref(*wait_backing);
		}

		/* The source mapping is no longer the fork's, nor is the region. */
		source_page->flags &= ~VM_MAPPING_BUSY;
		if (source_region->hold_count == 0)
			HAL_FATAL("VM fork region hold underflow");
		source_region->hold_count--;
		vmspace_fault_wake_locked(source);
		vm_page_free_metadata(copy_page);
		return error;
	}

	/* Links the copy into the child's region. */
	copy_page->next = copy_region->pages;
	copy_region->pages = copy_page;
	vmspace_region_page_index_insert(copy_region, copy_page);

	/* An outright copy is the child's own, writable page. */
	if (entry->eager) {
		entry->resident = 1;
		entry->physical = fresh->pmem.paddr;
		entry->cow_prot = source_region->prot;
		return 0;
	}

	/* Samples whether the shared backing is resident, and where. */
	state_irq = spin_lock_irqsave(&backing->state_lock);

	/* A backing on swap is mapped by the child's first fault instead. */
	entry->resident = 0;
	if ((backing->flags & VM_PAGE_RESIDENT) != 0)
		entry->resident = 1;
	if (entry->resident)
		entry->physical = backing->pmem.paddr;

	/* Leaves the backing's state lock. */
	spin_unlock_irqrestore(&backing->state_lock, state_irq);

	/* Succeeded: the page is shared copy-on-write. */
	return 0;
}

/*
 * Makes the parent's PTEs of a batch read-only and maps the child's, with
 * no VM lock held.  The pages are BUSY, so nothing moves them meanwhile.
 * Returns 0, or ENOMEM when a PTE could not be changed (the pages after it
 * are left unmapped).
 */
static int
fork_batch_map(
	struct vmspace *source,
	struct vmspace *copy,
	struct vmspace_fork_entry *batch,
	unsigned count)
{
	struct vmspace_fork_entry *entry;
	unsigned index;
	int status;
	int map_error;

	/* Maps each resident page the parent had mapped. */
	map_error = 0;
	for (index = 0; index < count; index++) {
		entry = &batch[index];
		if (map_error != 0 ||
		    !entry->resident ||
		    !entry->source_mapped)
			continue;

		/* Makes the parent's PTE read-only, unless the child got its own copy. */
		if (!entry->eager) {
			status = hal_space_prot(source->space,
						(void *)entry->page_address,
						PAGE_SIZE,
						entry->cow_prot);
			if (status != HAL_OK) {
				map_error = ENOMEM;
				continue;
			}
		}

		/* Maps the child read-only so the first write faults. */
		status = hal_space_map(copy->space,
				       (void *)entry->page_address,
				       entry->physical,
				       PAGE_SIZE,
				       entry->cow_prot);
		if (status != HAL_OK)
			map_error = ENOMEM;
		else
			entry->child_mapped = 1;
	}

	/* Reports a PTE that could not be changed. */
	if (map_error != 0)
		return map_error;

	/* Succeeded: every page is mapped in both. */
	return 0;
}

/*
 * Releases a fork batch under the VM locks: checks that no page moved
 * while they were let go, marks the child's mapped pages, takes BUSY and
 * the region's holds off, and ends the backing operations.
 */
static void
fork_batch_release(
	struct vmspace *source,
	struct vm_region *source_region,
	struct vmspace_fork_entry *batch,
	unsigned count)
{
	struct vmspace_fork_entry *entry;
	struct vm_page *source_page;
	struct vm_page *found;
	unsigned index;
	int moved;

	/* Releases each page of the batch. */
	for (index = 0; index < count; index++) {
		/* A pinned mapping that moved is a broken invariant. */
		entry = &batch[index];
		source_page = entry->source_page;
		moved = 0;
		if (source_page->region != source_region ||
		    source_page->address != entry->page_address ||
		    source_page->private_page != entry->backing) {
			moved = 1;
		} else {
			found = vmspace_find_page(source_region, entry->page_address);
			if (found != source_page)
				moved = 1;
		}

		/* Stops the kernel on a moved mapping. */
		if (moved)
			HAL_FATAL("VM fork lost pinned source mapping");

		/* MAPPED tells unmap and reclaim that the child's PTE exists. */
		if (entry->child_mapped)
			entry->copy_page->flags |= VM_MAPPING_MAPPED;

		/* The source mapping leaves the fork, and so does its region's hold. */
		source_page->flags &= ~VM_MAPPING_BUSY;
		if (source_region->hold_count == 0)
			HAL_FATAL("VM fork region hold underflow");
		source_region->hold_count--;

		/* A shared page's backing operation ends; an outright copy took none. */
		if (!entry->eager)
			vm_private_page_operation_end(entry->backing);
	}

	/* Wakes the faults that waited for the BUSY pages. */
	vmspace_fault_wake_locked(source);
}
