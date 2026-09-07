/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Private page reclaim.
 *
 * Every anonymous or copy-on-write page is a private backing shared by
 * its mappings and kept on one global queue.  Reclaim takes the least
 * recently touched resident backing, revokes and unmaps its PTEs, and
 * either discards a clean file copy or writes the page to swap.  A
 * draining swap source has its pages read back in the same way.
 */

#include "kern/vm-reclaim.h"
#include "kern/vmspace.h"
#include "kern/swap.h"
#include "kern/kmem.h"
#include "kern/lock.h"
#include "kern/page.h"
#include "kern/sched.h"
#include "kern/vm-lock.h"
#include "kern/cache-memory.h"

extern size_t cache_memory_reclaim(size_t) __attribute__((weak));

#include <errno.h>
#include <hal/hal.h>
#include <string.h>

#define PAGE_SIZE ZEDBSD_PAGE_SIZE
#define VM_PAGE_TRACKED 0x0010U

static struct vm_private_page *page_queue;
static struct mutex reclaim_lock;
struct vm_reclaim_stats vm_reclaim_counters;
#define stats vm_reclaim_counters

static void private_page_advance_locked(struct vm_private_page *backing);
static int private_page_wait_sequence(struct vm_private_page *backing, uint64_t sequence);
static int private_page_wait_sequence_interruptible(struct vm_private_page *backing, uint64_t sequence);
static int vm_private_page_wait_idle_cancelable(struct vm_private_page *backing, int (*cancel)(void *), void *cancel_argument);
static uint32_t mapping_prot(const struct vm_page *page);
static void queue_remove(struct vm_private_page *backing);
static int backing_wired_or_avoided(struct vm_private_page *backing, struct vm_page *avoid);
static int backing_has_wired_mapping(struct vm_private_page *backing);
static int backing_has_busy_mapping(struct vm_private_page *backing);
static uint32_t backing_pte_flags(struct vm_private_page *backing);
static void vmspace_fault_wake(struct vmspace *vm);
static void pin_backing_mappings(struct vm_private_page *backing);
static void unpin_backing_mappings(struct vm_private_page *backing);
static int private_page_targets_source(struct vm_private_page *backing, unsigned source_id);
static void drain_mapping_pins_release(struct vm_private_page *backing);
static int unmap_backing_ptes(struct vm_private_page *backing, uint32_t *pte_flags);
static void rollback_backing_ptes(struct vm_private_page *backing);
static int swap_out_backing_owned(struct vm_private_page *backing);
static int discard_file_backing_owned(struct vm_private_page *backing);
static int reclaim_backing_owned(struct vm_private_page *backing, int file_candidate);

/*
 * Reclaims one object page; the default without VM objects has none.
 */
__attribute__((weak)) int
vm_object_reclaim_one(
	void)
{
	return ENOMEM;
}

/*
 * Counts the object pages; the default without VM objects has none.
 */
__attribute__((weak)) unsigned
vm_object_page_count(
	void)
{
	return 0;
}

/*
 * Initializes a private backing with one reference and no page.
 */
void
vm_private_page_init(
	struct vm_private_page *backing)
{
	/* Ignores a missing backing. */
	if (backing == NULL)
		return;

	/* Starts unmapped, unswapped, at the first generation. */
	refcount_init(&backing->refs, 1);
	spin_init(&backing->state_lock, LOCK_RANK_VM_OBJECT,
	    "VM private backing");
	waitq_init(&backing->state_waitq, "VM private backing");
	backing->generation = 1;
	backing->swap_slot = SWAP_SLOT_NONE;
}

/*
 * Takes a reference to a private backing.
 */
void
vm_private_page_ref(
	struct vm_private_page *backing)
{
	if (backing != NULL)
		refcount_get(&backing->refs);
}

/*
 * Drops a reference to a private backing, freeing it with the last.
 *
 * The last reference must find the backing unmapped, idle, and
 * untracked; its page and swap slot are released with it.
 */
void
vm_private_page_put(
	struct vm_private_page *backing)
{
	struct hal_pmem memory;
	uint32_t slot;
	unsigned long irq;

	/* Only the last reference frees. */
	if (backing == NULL || !refcount_put(&backing->refs))
		return;

	/* Takes the page and slot out of a backing nothing else uses. */
	irq = spin_lock_irqsave(&backing->state_lock);
	if (backing->mapping_count != 0 ||
	    backing->mappings != NULL ||
	    backing->active_operations != 0 ||
	    backing->pin_count != 0 ||
	    (backing->flags & (VM_PAGE_BUSY | VM_PAGE_TRACKED)) != 0)
		HAL_FATAL("destroying active VM private backing");
	memory = backing->pmem;
	memset(&backing->pmem, 0, sizeof(backing->pmem));
	slot = backing->swap_slot;
	backing->swap_slot = SWAP_SLOT_NONE;
	spin_unlock_irqrestore(&backing->state_lock, irq);

	/* Frees them and the metadata. */
	if (memory.size != 0)
		(void)hal_pmem_free(&memory);
	if (slot != SWAP_SLOT_NONE && swap_system_backend() != NULL)
		swap_free_slot(swap_system_backend(), slot);
	vm_private_page_free_metadata(backing);
}

/*
 * Takes exclusive I/O ownership of a backing, waiting for it to go idle.
 */
int
vm_private_page_io_acquire(
	struct vm_private_page *backing)
{
	uint64_t sequence;
	unsigned long irq;
	int error;

	/* Rejects a missing backing. */
	if (backing == NULL)
		return EINVAL;

	/* Retries until no owner, operation, or pin is present. */
	for (;;) {
		irq = spin_lock_irqsave(&backing->state_lock);
		if ((backing->flags & VM_PAGE_BUSY) == 0 &&
		    backing->active_operations == 0 &&
		    backing->pin_count == 0) {
			backing->flags |= VM_PAGE_BUSY;
			private_page_advance_locked(backing);
			spin_unlock_irqrestore(&backing->state_lock, irq);
			return 0;
		}
		sequence = waitq_sequence(&backing->state_waitq);
		spin_unlock_irqrestore(&backing->state_lock, irq);
		error = private_page_wait_sequence(backing, sequence);
		if (error != 0 && error != EAGAIN)
			return error;
	}
}

/*
 * Takes exclusive I/O ownership of a backing without waiting.
 *
 * Success also takes a lifetime reference that the owner drops after
 * releasing.
 */
int
vm_private_page_io_try_acquire(
	struct vm_private_page *backing)
{
	unsigned long irq;

	/* Rejects a missing backing. */
	if (backing == NULL)
		return EINVAL;

	/* Fails while an owner, operation, or pin is present. */
	irq = spin_lock_irqsave(&backing->state_lock);
	if ((backing->flags & VM_PAGE_BUSY) != 0 ||
	    backing->active_operations != 0 ||
	    backing->pin_count != 0) {
		spin_unlock_irqrestore(&backing->state_lock, irq);
		return EBUSY;
	}

	/* The returned I/O ownership includes an explicit lifetime hold. */
	refcount_get(&backing->refs);
	backing->flags |= VM_PAGE_BUSY;
	private_page_advance_locked(backing);
	spin_unlock_irqrestore(&backing->state_lock, irq);

	/* Reports the acquired ownership. */
	return 0;
}

/*
 * Releases I/O ownership of a backing and wakes its waiters.
 */
void
vm_private_page_io_release(
	struct vm_private_page *backing)
{
	unsigned long irq;

	/* Ignores a missing backing. */
	if (backing == NULL)
		return;

	/* Clears the ownership, which must be held. */
	irq = spin_lock_irqsave(&backing->state_lock);
	if ((backing->flags & VM_PAGE_BUSY) == 0)
		HAL_FATAL("VM private backing I/O ownership underflow");
	backing->flags &= ~VM_PAGE_BUSY;
	private_page_advance_locked(backing);
	waitq_wake_all(&backing->state_waitq);
	spin_unlock_irqrestore(&backing->state_lock, irq);
}

/*
 * Waits until a backing has no I/O owner, operation, or pin.
 */
int
vm_private_page_wait_idle(
	struct vm_private_page *backing)
{
	uint64_t sequence;
	unsigned long irq;
	int error;

	/* Rejects a missing backing. */
	if (backing == NULL)
		return EINVAL;

	/* Sleeps on the state queue until the backing is idle. */
	for (;;) {
		irq = spin_lock_irqsave(&backing->state_lock);
		if ((backing->flags & VM_PAGE_BUSY) == 0 &&
		    backing->active_operations == 0 &&
		    backing->pin_count == 0) {
			spin_unlock_irqrestore(&backing->state_lock, irq);
			return 0;
		}
		sequence = waitq_sequence(&backing->state_waitq);
		spin_unlock_irqrestore(&backing->state_lock, irq);
		error = private_page_wait_sequence(backing, sequence);
		if (error != 0 && error != EAGAIN)
			return error;
	}
}

/*
 * Begins a non-I/O operation on a backing unless it is owned or pinned.
 *
 * The operation holds a reference until vm_private_page_operation_end().
 */
int
vm_private_page_operation_try_begin(
	struct vm_private_page *backing)
{
	unsigned long irq;

	/* Rejects a missing backing. */
	if (backing == NULL)
		return EINVAL;

	/* Fails while an I/O owner or pin is present. */
	irq = spin_lock_irqsave(&backing->state_lock);
	if ((backing->flags & VM_PAGE_BUSY) != 0 || backing->pin_count != 0) {
		spin_unlock_irqrestore(&backing->state_lock, irq);
		return EBUSY;
	}
	refcount_get(&backing->refs);
	backing->active_operations++;
	if (backing->active_operations == 0)
		HAL_FATAL("VM private backing operation counter overflow");
	spin_unlock_irqrestore(&backing->state_lock, irq);

	/* Reports the begun operation. */
	return 0;
}

/*
 * Ends an operation begun with vm_private_page_operation_try_begin().
 */
void
vm_private_page_operation_end(
	struct vm_private_page *backing)
{
	unsigned long irq;

	/* Ignores a missing backing. */
	if (backing == NULL)
		return;

	/* Counts the operation down, waking waiters when it was the last. */
	irq = spin_lock_irqsave(&backing->state_lock);
	if (backing->active_operations == 0)
		HAL_FATAL("VM private backing operation counter underflow");
	backing->active_operations--;
	private_page_advance_locked(backing);
	if (backing->active_operations == 0)
		waitq_wake_all(&backing->state_waitq);
	spin_unlock_irqrestore(&backing->state_lock, irq);
	vm_private_page_put(backing);
}

/*
 * Marks a backing's page dirty.
 */
void
vm_private_page_mark_dirty(
	struct vm_private_page *backing)
{
	unsigned long irq;

	/* Ignores a missing backing. */
	if (backing == NULL)
		return;

	/* Sets the flag under the state lock. */
	irq = spin_lock_irqsave(&backing->state_lock);
	backing->flags |= VM_PAGE_DIRTY;
	private_page_advance_locked(backing);
	spin_unlock_irqrestore(&backing->state_lock, irq);
}

/*
 * Pins a resident backing's page in memory and reports it.
 *
 * The pin holds a reference until vm_private_page_unpin().
 */
int
vm_private_page_pin(
	struct vm_private_page *backing,
	struct hal_pmem *memory)
{
	unsigned long irq;

	/* Rejects a missing backing or result. */
	if (backing == NULL || memory == NULL)
		return EINVAL;

	/* Fails while an owner or operation is present, or the page is absent. */
	irq = spin_lock_irqsave(&backing->state_lock);
	if ((backing->flags & VM_PAGE_BUSY) != 0 ||
	    backing->active_operations != 0 ||
	    (backing->flags & VM_PAGE_RESIDENT) == 0) {
		spin_unlock_irqrestore(&backing->state_lock, irq);
		return EBUSY;
	}
	refcount_get(&backing->refs);
	backing->pin_count++;
	if (backing->pin_count == 0)
		HAL_FATAL("VM private backing pin counter overflow");
	*memory = backing->pmem;
	spin_unlock_irqrestore(&backing->state_lock, irq);

	/* Reports the pinned page. */
	return 0;
}

/*
 * Drops a pin taken with vm_private_page_pin().
 */
void
vm_private_page_unpin(
	struct vm_private_page *backing)
{
	unsigned long irq;

	/* Ignores a missing backing. */
	if (backing == NULL)
		return;

	/* Counts the pin down, waking waiters when it was the last. */
	irq = spin_lock_irqsave(&backing->state_lock);
	if (backing->pin_count == 0)
		HAL_FATAL("VM private backing pin counter underflow");
	backing->pin_count--;
	private_page_advance_locked(backing);
	if (backing->pin_count == 0)
		waitq_wake_all(&backing->state_waitq);
	spin_unlock_irqrestore(&backing->state_lock, irq);
	vm_private_page_put(backing);
}

/*
 * Initializes the reclaim queue and statistics.
 */
void
vm_reclaim_init(
	void)
{
	vm_metadata_init();
	page_queue = NULL;
	memset(&stats, 0, sizeof(stats));
	(void)mutex_init(&reclaim_lock, LOCK_RANK_VMSPACE, "VM reclaim");
}

/*
 * Puts a mapped page's backing on the reclaim queue.
 */
void
vm_page_track(
	struct vm_page *page)
{
	struct vm_private_page *backing;
	unsigned long irq;
	int resident;

	/* Ignores a page without a private backing. */
	if (page == NULL)
		return;
	backing = page->private_page;
	if (backing == NULL)
		return;

	/* A mapped, untracked backing joins the queue head. */
	vm_metadata_enter();
	mutex_lock(&reclaim_lock);
	irq = spin_lock_irqsave(&backing->state_lock);
	if (backing->mapping_count == 0 ||
	    (backing->flags & VM_PAGE_TRACKED) != 0)
		HAL_FATAL("invalid VM private backing track");
	backing->flags |= VM_PAGE_TRACKED;
	resident = (backing->flags & VM_PAGE_RESIDENT) != 0;
	spin_unlock_irqrestore(&backing->state_lock, irq);
	backing->queue_next = page_queue;
	page_queue = backing;
	if (resident)
		stats.resident++;
	mutex_unlock(&reclaim_lock);
	vm_metadata_leave();
}

/*
 * Removes a page's mapping from its backing, dequeuing the last one.
 */
void
vm_page_untrack(
	struct vm_page *page)
{
	struct vm_private_page *backing;
	struct vm_page **link;
	unsigned long irq;
	int last_mapping;
	int resident;
	int swapped;
	int tracked;

	/* Ignores a page without a private backing. */
	if (page == NULL)
		return;
	backing = page->private_page;
	if (backing == NULL)
		return;

	/* Unlinks the page from the reverse mapping list. */
	vm_metadata_enter();
	mutex_lock(&reclaim_lock);
	link = &backing->mappings;
	while (*link != NULL && *link != page)
		link = &(*link)->private_next;
	if (*link != page)
		HAL_FATAL("VM private page reverse mapping not linked");
	*link = page->private_next;
	page->private_page = NULL;
	page->private_next = NULL;

	/* The last mapping takes the backing off the queue. */
	irq = spin_lock_irqsave(&backing->state_lock);
	if (backing->mapping_count == 0)
		HAL_FATAL("VM private backing mapping counter underflow");
	backing->mapping_count--;
	last_mapping = backing->mapping_count == 0;
	resident = (backing->flags & VM_PAGE_RESIDENT) != 0;
	swapped = (backing->flags & VM_PAGE_SWAPPED) != 0;
	tracked = (backing->flags & VM_PAGE_TRACKED) != 0;
	if (last_mapping)
		backing->flags &= ~VM_PAGE_TRACKED;
	spin_unlock_irqrestore(&backing->state_lock, irq);
	if (last_mapping && tracked) {
		queue_remove(backing);
		if (resident && stats.resident)
			stats.resident--;
		if (swapped && stats.swapped)
			stats.swapped--;
	}
	mutex_unlock(&reclaim_lock);
	vm_metadata_leave();
	vm_private_page_put(backing);
}

/*
 * Moves a page from an owned backing to a fresh one, as a copy-on-write
 * fault does.
 */
void
vm_page_replace_private(
	struct vm_page *page,
	struct vm_private_page *fresh)
{
	struct vm_private_page *old;
	struct vm_page **link;
	unsigned long old_irq;
	unsigned long fresh_irq;
	int old_last;
	int old_resident;
	int old_swapped;
	int fresh_resident;

	/* Rejects a missing page or backing. */
	if (page == NULL || fresh == NULL || page->private_page == NULL)
		HAL_FATAL("invalid private page replacement");

	/* The old backing must be owned and mapped. */
	vm_metadata_enter();
	old = page->private_page;
	mutex_lock(&reclaim_lock);
	old_irq = spin_lock_irqsave(&old->state_lock);
	if ((old->flags & VM_PAGE_BUSY) == 0 || old->mapping_count == 0)
		HAL_FATAL("replacing an unowned VM private backing");
	spin_unlock_irqrestore(&old->state_lock, old_irq);

	/* Unlinks the page from the old reverse mapping list. */
	link = &old->mappings;
	while (*link != NULL && *link != page)
		link = &(*link)->private_next;
	if (*link != page)
		HAL_FATAL("private page mapping not linked");
	*link = page->private_next;
	old_irq = spin_lock_irqsave(&old->state_lock);
	old->mapping_count--;
	old_last = old->mapping_count == 0;
	old_resident = (old->flags & VM_PAGE_RESIDENT) != 0;
	old_swapped = (old->flags & VM_PAGE_SWAPPED) != 0;
	if (old_last)
		old->flags &= ~VM_PAGE_TRACKED;
	spin_unlock_irqrestore(&old->state_lock, old_irq);

	/* The fresh backing takes the page as its only mapping. */
	fresh_irq = spin_lock_irqsave(&fresh->state_lock);
	if (fresh->mapping_count != 0 ||
	    fresh->mappings != NULL ||
	    (fresh->flags & (VM_PAGE_BUSY | VM_PAGE_TRACKED)) != 0)
		HAL_FATAL("replacement VM private backing is not fresh");
	fresh->mapping_count = 1;
	fresh->flags |= VM_PAGE_TRACKED;
	fresh_resident = (fresh->flags & VM_PAGE_RESIDENT) != 0;
	spin_unlock_irqrestore(&fresh->state_lock, fresh_irq);
	page->private_page = fresh;
	page->private_next = fresh->mappings;
	fresh->mappings = page;
	fresh->queue_next = page_queue;
	page_queue = fresh;
	if (fresh_resident)
		stats.resident++;

	/* An old backing without mappings leaves the queue. */
	if (old_last) {
		queue_remove(old);
		if (old_resident && stats.resident)
			stats.resident--;
		if (old_swapped && stats.swapped)
			stats.swapped--;
	}
	mutex_unlock(&reclaim_lock);
	vm_metadata_leave();

	/* The fault's explicit I/O hold keeps old alive across this mapping drop. */
	vm_private_page_put(old);
}

/*
 * Shares a page's backing with a copy, marking both copy-on-write.
 */
int
vm_page_share_private(
	struct vm_page *source,
	struct vm_page *copy)
{
	struct vm_private_page *backing;
	unsigned long irq;
	int error;

	error = 0;

	/* Rejects a missing page or a source without a backing. */
	if (source == NULL || copy == NULL || source->private_page == NULL)
		return EINVAL;

	/* Holds the backing as an operation while the mapping is added. */
	backing = source->private_page;
	error = vm_private_page_operation_try_begin(backing);
	if (error != 0)
		return error;
	vm_metadata_enter();
	mutex_lock(&reclaim_lock);
	if (source->private_page != backing) {
		error = EAGAIN;
		goto out;
	}

	/* Adds the copy as another mapping. */
	irq = spin_lock_irqsave(&backing->state_lock);
	if ((backing->flags & VM_PAGE_BUSY) != 0)
		HAL_FATAL("VM private operation overlapped I/O owner");
	refcount_get(&backing->refs);
	backing->mapping_count++;
	if (backing->mapping_count == 0)
		HAL_FATAL("VM private backing mapping counter overflow");
	spin_unlock_irqrestore(&backing->state_lock, irq);
	copy->private_page = backing;
	copy->private_next = backing->mappings;
	backing->mappings = copy;
	source->flags |= VM_MAPPING_COW;
	copy->flags |= VM_MAPPING_COW;

	/* Undoes the mapping on a failure recorded above. */
	if (error != 0) {
		backing->mappings = copy->private_next;
		copy->private_page = NULL;
		copy->private_next = NULL;
		irq = spin_lock_irqsave(&backing->state_lock);
		if (backing->mapping_count == 0)
			HAL_FATAL("VM private backing mapping rollback underflow");
		backing->mapping_count--;
		spin_unlock_irqrestore(&backing->state_lock, irq);
		if (refcount_put(&backing->refs))
			HAL_FATAL("shared VM private backing lost source mapping");
	}
out:
	mutex_unlock(&reclaim_lock);
	vm_metadata_leave();
	if (error != 0)
		vm_private_page_operation_end(backing);

	/* Reports the share result. */
	return error;
}

/*
 * Reads a swapped-out page back into a fresh page for its I/O owner.
 *
 * The page comes back dirty, since the freed swap slot was its only
 * persistent copy.
 */
int
vm_private_page_in_owned(
	struct vm_private_page *backing,
	struct vm_page *accounting_page)
{
	const struct hal_pmem_request request = {
		HAL_PMEM_PADDR_ANY, PAGE_SIZE, PAGE_SIZE,
		HAL_PMEM_TYPE_RAM, 0
	};
	struct swap_backend *backend;
	uint32_t slot;
	unsigned long irq;
	int error;
	struct hal_pmem memory;

	backend = swap_system_backend();

	/* Rejects a missing operand or a backing that is not owned and swapped. */
	if (backing == NULL || accounting_page == NULL || backend == NULL)
		return EIO;
	irq = spin_lock_irqsave(&backing->state_lock);
	if ((backing->flags & (VM_PAGE_BUSY | VM_PAGE_SWAPPED)) !=
	    (VM_PAGE_BUSY | VM_PAGE_SWAPPED)) {
		spin_unlock_irqrestore(&backing->state_lock, irq);
		return EIO;
	}
	slot = backing->swap_slot;
	spin_unlock_irqrestore(&backing->state_lock, irq);

	/* Allocates a page, reclaiming once when memory is short, and reads it. */
	if (hal_pmem_alloc(&request, &backing->pmem) == HAL_OK)
		error = 0;
	else
		error = ENOMEM;
	if (error != 0 && vm_reclaim_one(accounting_page) == 0) {
		if (hal_pmem_alloc(&request, &backing->pmem) == HAL_OK)
			error = 0;
		else
			error = ENOMEM;
	}
	if (error == 0)
		error = swap_read_page(backend, slot,
		    (void *)backing->pmem.vaddr);
	if (error != 0) {
		if (backing->pmem.size != 0) {
			memory = backing->pmem;
			memset(&backing->pmem, 0, sizeof(backing->pmem));
			(void)hal_pmem_free(&memory);
		}
		return error;
	}

	/*
	 * No source state is held while the backing operation completes.  The
	 * exclusive private-page I/O owner keeps the token stable until this
	 * resident state is published.
	 */
	swap_free_slot(backend, slot);
	irq = spin_lock_irqsave(&backing->state_lock);
	if ((backing->flags & (VM_PAGE_BUSY | VM_PAGE_SWAPPED)) !=
	    (VM_PAGE_BUSY | VM_PAGE_SWAPPED) || backing->swap_slot != slot)
		HAL_FATAL("VM private page-in state changed under I/O owner");
	backing->swap_slot = SWAP_SLOT_NONE;
	backing->flags &= ~VM_PAGE_SWAPPED;

	/*
	 * The old slot was the only persistent copy.  Keep the resident page
	 * dirty until a later reclaim writes it to an active source.
	 */
	backing->flags |= VM_PAGE_RESIDENT | VM_PAGE_DIRTY;
	private_page_advance_locked(backing);
	spin_unlock_irqrestore(&backing->state_lock, irq);
	vm_page_note_in(accounting_page);

	/* Reports the paged-in backing. */
	return 0;
}

/*
 * Reads every page of a draining swap source back into memory.
 *
 * A cancel callback, when given, is polled between pages and while
 * waiting for a busy backing.
 */
int
vm_reclaim_drain_swap_source_cancelable(
	unsigned source_id,
	int (*cancel)(void *),
	void *cancel_argument)
{
	struct vm_private_page *selected;
	struct vm_private_page *wait_backing;
	struct vm_private_page *backing;
	struct vm_page *accounting_page;
	struct swap_backend *backend;
	struct swap_source_stats source_stats;
	int scan_error;
	int error;

	/* Rejects a source id out of range. */
	if (source_id >= SWAP_SOURCE_COUNT)
		return EINVAL;

	/* Pages in one backing per pass until the source is empty. */
	for (;;) {
		selected = NULL;
		wait_backing = NULL;
		accounting_page = NULL;
		scan_error = 0;

		/* Honors a cancellation between passes. */
		if (cancel != NULL) {
			error = cancel(cancel_argument);
			if (error != 0)
				return error;
		}

		/*
		 * Select and pin one target while reverse mappings are stable.  No
		 * VM metadata lock survives physical allocation or backing I/O.
		 */
		vm_metadata_enter();
		mutex_lock(&reclaim_lock);
		for (backing = page_queue; backing != NULL;
		     backing = backing->queue_next) {
			if (!private_page_targets_source(backing, source_id))
				continue;

			/*
			 * A wired page cannot legitimately have been reclaimed to
			 * swap.  Report the broken invariant instead of waiting
			 * forever.
			 */
			if (backing_has_wired_mapping(backing)) {
				scan_error = EIO;
				break;
			}
			if (backing_has_busy_mapping(backing) ||
			    vm_private_page_io_try_acquire(backing) != 0) {
				vm_private_page_ref(backing);
				wait_backing = backing;
				break;
			}

			/*
			 * A fault may have completed between the first observation
			 * and our exclusive acquisition.
			 */
			if (!private_page_targets_source(backing, source_id)) {
				vm_private_page_io_release(backing);
				vm_private_page_put(backing);
				continue;
			}
			if (backing->mappings == NULL)
				HAL_FATAL("tracked swapped backing has no mapping");
			pin_backing_mappings(backing);
			accounting_page = backing->mappings;
			selected = backing;
			break;
		}
		mutex_unlock(&reclaim_lock);
		vm_metadata_leave();
		if (scan_error != 0)
			return scan_error;

		/* Pages the selected backing in and goes around again. */
		if (selected != NULL) {
			error = vm_private_page_in_owned(selected, accounting_page);
			drain_mapping_pins_release(selected);
			vm_private_page_io_release(selected);
			vm_private_page_put(selected);
			if (error != 0)
				return error;
			if (cancel != NULL) {
				error = cancel(cancel_argument);
				if (error != 0)
					return error;
			}
			continue;
		}

		/* Waits for a busy backing to become idle. */
		if (wait_backing != NULL) {
			error = vm_private_page_wait_idle_cancelable(wait_backing,
			    cancel, cancel_argument);
			vm_private_page_put(wait_backing);
			if (error != 0 && error != EAGAIN)
				return error;
			sched_yield();
			continue;
		}

		/*
		 * A slot allocator which crossed begin_drain may not have published
		 * its backing token yet.  Source counters close that
		 * scan/publication window; yield until it finishes, then scan
		 * again.
		 */
		backend = swap_system_backend();
		if (backend == NULL)
			return ENXIO;
		error = swap_source_get_stats(backend, source_id, &source_stats);
		if (error != 0)
			return error;
		if (source_stats.state != SWAP_SOURCE_STATE_DRAINING)
			return EBUSY;
		if (source_stats.allocated_slots == 0 &&
		    source_stats.inflight == 0)
			return 0;
		sched_yield();
	}
}

/*
 * Reads every page of a draining swap source back into memory.
 */
int
vm_reclaim_drain_swap_source(
	unsigned source_id)
{
	int error;

	error = vm_reclaim_drain_swap_source_cancelable(source_id, NULL, NULL);

	/* Reports the drain result. */
	return error;
}

/*
 * Reclaims one resident private page, avoiding a given mapping.
 *
 * Unreferenced pages are preferred over recently accessed ones.
 * Reports EAGAIN when no candidate is available.
 */
int
vm_reclaim_private_one(
	struct vm_page *avoid)
{
	struct vm_private_page *selected;
	unsigned pass;
	int file_candidate;
	struct vm_private_page *backing;
	unsigned state_flags;
	unsigned long irq;
	uint32_t flags;
	int error;

	selected = NULL;
	file_candidate = 0;

	/* The first pass skips accessed pages; the second takes any. */
	vm_metadata_enter();
	mutex_lock(&reclaim_lock);
	for (pass = 0; pass < 2; pass++) {
		for (backing = page_queue; backing != NULL;
		     backing = backing->queue_next) {
			irq = spin_lock_irqsave(&backing->state_lock);
			state_flags = backing->flags;
			spin_unlock_irqrestore(&backing->state_lock, irq);
			if ((state_flags & VM_PAGE_RESIDENT) == 0 ||
			    (state_flags & VM_PAGE_BUSY) != 0 ||
			    backing_wired_or_avoided(backing, avoid))
				continue;
			flags = backing_pte_flags(backing);
			if (pass == 0 && (flags & HAL_PAGE_ACCESSED))
				continue;
			file_candidate = backing->mappings != NULL &&
			    backing->mappings->region->backing == VM_BACKING_FILE;
			if (vm_private_page_io_try_acquire(backing) != 0)
				continue;
			pin_backing_mappings(backing);
			selected = backing;
			goto out;
		}
	}
out:
	mutex_unlock(&reclaim_lock);
	vm_metadata_leave();

	/* Reclaims the selected backing outside the locks. */
	if (selected != NULL) {
		error = reclaim_backing_owned(selected, file_candidate);
		return error;
	}

	/* Reports no candidate. */
	return EAGAIN;
}

/*
 * Reclaims clean cache first, then private or writeback-backed pages.
 */
int
vm_reclaim_one(
	struct vm_page *avoid)
{
	int result;

	/* Returns disposable cache before initiating private-page swap or writeback. */
	if (cache_memory_reclaim != NULL && cache_memory_reclaim(PAGE_SIZE) != 0)
		return 0;

	/* A private page is the first choice. */
	result = vm_reclaim_private_one(avoid);
	if (result == 0)
		return 0;
	if (result != EAGAIN)
		return result;

	/*
	 * Object writeback can sleep in VFS I/O.  It must not inherit either the
	 * private-page queue lock or the global cross-VM metadata lock from the
	 * candidate scan above.
	 */
	result = vm_object_reclaim_one();
	if (result != 0)
		return ENOMEM;
	vm_metadata_enter();
	mutex_lock(&reclaim_lock);
	stats.reclaims++;
	mutex_unlock(&reclaim_lock);
	vm_metadata_leave();

	/* Reports the reclaimed object page. */
	return 0;
}

/*
 * Accounts a page that was paged in.
 */
void
vm_page_note_in(
	struct vm_page *page)
{
	/* Ignores a missing page. */
	if (page == NULL)
		return;

	/* Moves the count from swapped to resident. */
	vm_metadata_enter();
	mutex_lock(&reclaim_lock);
	if (stats.swapped)
		stats.swapped--;
	stats.resident++;
	stats.page_ins++;
	mutex_unlock(&reclaim_lock);
	vm_metadata_leave();
}

/*
 * Counts a page fault.
 */
void
vm_reclaim_note_fault(
	void)
{
	vm_metadata_enter();
	mutex_lock(&reclaim_lock);
	stats.faults++;
	mutex_unlock(&reclaim_lock);
	vm_metadata_leave();
}

/*
 * Copies the reclaim statistics, classifying the queued pages.
 */
void
vm_reclaim_get_stats(
	struct vm_reclaim_stats *output)
{
	struct vm_private_page *backing;
	struct vm_page *page;
	unsigned long irq;
	unsigned state_flags;
	uint32_t flags;

	/* Ignores a missing result. */
	if (output == NULL)
		return;

	/* Starts from the counters and derives the classified counts. */
	vm_metadata_enter();
	mutex_lock(&reclaim_lock);
	memcpy(output, &stats, sizeof(*output));
	output->anonymous_resident = 0;
	output->file_resident = 0;
	output->wired = 0;
	output->busy = 0;
	output->dirty = 0;
	output->clean = 0;
	for (backing = page_queue; backing != NULL; backing = backing->queue_next) {
		flags = backing_pte_flags(backing);
		irq = spin_lock_irqsave(&backing->state_lock);
		state_flags = backing->flags;
		spin_unlock_irqrestore(&backing->state_lock, irq);
		if (state_flags & VM_PAGE_BUSY)
			output->busy++;
		for (page = backing->mappings; page != NULL;
		     page = page->private_next) {
			if (page->wire_count != 0) {
				output->wired++;
				break;
			}
		}
		if (!(state_flags & VM_PAGE_RESIDENT))
			continue;
		page = backing->mappings;
		if (page != NULL && page->region->backing == VM_BACKING_ANON)
			output->anonymous_resident++;
		else
			output->file_resident++;
		if ((state_flags & VM_PAGE_DIRTY) || (flags & HAL_PAGE_DIRTY))
			output->dirty++;
		else
			output->clean++;
	}
	output->file_resident += vm_object_page_count();
	output->resident += vm_object_page_count();
	mutex_unlock(&reclaim_lock);
	vm_metadata_leave();
}

/* Advances a backing's generation, skipping zero. */
static void
private_page_advance_locked(
	struct vm_private_page *backing)
{
	backing->generation++;
	if (backing->generation == 0)
		backing->generation++;
}

/* Sleeps on a backing's state queue for the sequence observed. */
static int
private_page_wait_sequence(
	struct vm_private_page *backing,
	uint64_t sequence)
{
	unsigned long irq;
	int error;

	irq = spin_lock_irqsave(&backing->state_lock);
	error = waitq_sleep(&backing->state_waitq, &backing->state_lock,
	    sequence, 0, 0);
	spin_unlock_irqrestore(&backing->state_lock, irq);
	return error;
}

/* Sleeps interruptibly on a backing's state queue for the sequence observed. */
static int
private_page_wait_sequence_interruptible(
	struct vm_private_page *backing,
	uint64_t sequence)
{
	unsigned long irq;
	int error;

	irq = spin_lock_irqsave(&backing->state_lock);
	error = waitq_sleep(&backing->state_waitq, &backing->state_lock,
	    sequence, 0, WAITQ_INTERRUPTIBLE);
	spin_unlock_irqrestore(&backing->state_lock, irq);
	return error;
}

/* Waits for a backing to go idle, polling a cancel callback around each sleep. */
static int
vm_private_page_wait_idle_cancelable(
	struct vm_private_page *backing,
	int (*cancel)(void *),
	void *cancel_argument)
{
	uint64_t sequence;
	unsigned long irq;
	int error;

	/* Without a callback the plain wait suffices. */
	if (cancel == NULL) {
		error = vm_private_page_wait_idle(backing);
		return error;
	}
	if (backing == NULL)
		return EINVAL;

	/* Checks for cancellation before each sleep. */
	for (;;) {
		error = cancel(cancel_argument);
		if (error != 0)
			return error;
		irq = spin_lock_irqsave(&backing->state_lock);
		if ((backing->flags & VM_PAGE_BUSY) == 0 &&
		    backing->active_operations == 0 &&
		    backing->pin_count == 0) {
			spin_unlock_irqrestore(&backing->state_lock, irq);
			return 0;
		}
		sequence = waitq_sequence(&backing->state_waitq);
		spin_unlock_irqrestore(&backing->state_lock, irq);
		error = private_page_wait_sequence_interruptible(backing, sequence);
		if (error != 0 && error != EAGAIN)
			return error;
	}
}

/* Reports the protection a mapping should carry, read-only when copy-on-write. */
static uint32_t
mapping_prot(
	const struct vm_page *page)
{
	uint32_t prot;

	prot = page->region->prot;
	if (page->flags & VM_MAPPING_COW)
		prot &= ~HAL_SPACE_WRITE;
	return prot;
}

/* Unlinks a backing from the reclaim queue. */
static void
queue_remove(
	struct vm_private_page *backing)
{
	struct vm_private_page **link;

	link = &page_queue;
	while (*link != NULL && *link != backing)
		link = &(*link)->queue_next;
	if (*link == backing)
		*link = backing->queue_next;
}

/* Tests whether any mapping is wired, busy, or the one to avoid. */
static int
backing_wired_or_avoided(
	struct vm_private_page *backing,
	struct vm_page *avoid)
{
	struct vm_page *page;

	for (page = backing->mappings; page != NULL; page = page->private_next) {
		if (page == avoid ||
		    page->wire_count != 0 ||
		    (page->flags & VM_MAPPING_BUSY) != 0)
			return 1;
	}
	return 0;
}

/* Tests whether any mapping is wired. */
static int
backing_has_wired_mapping(
	struct vm_private_page *backing)
{
	struct vm_page *page;

	for (page = backing->mappings; page != NULL; page = page->private_next) {
		if (page->wire_count != 0)
			return 1;
	}
	return 0;
}

/* Tests whether any mapping is busy in a fault or reclaim. */
static int
backing_has_busy_mapping(
	struct vm_private_page *backing)
{
	struct vm_page *page;

	for (page = backing->mappings; page != NULL; page = page->private_next) {
		if ((page->flags & VM_MAPPING_BUSY) != 0)
			return 1;
	}
	return 0;
}

/* Combines the accessed and dirty PTE flags of every mapping. */
static uint32_t
backing_pte_flags(
	struct vm_private_page *backing)
{
	struct vm_page *page;
	uint32_t combined;
	uint32_t flags;

	combined = 0;
	for (page = backing->mappings; page != NULL; page = page->private_next) {
		if (!(page->flags & VM_MAPPING_MAPPED))
			continue;
		flags = 0;
		if (hal_page_query(page->vm->space, (void *)page->address,
		    &flags) == HAL_OK)
			combined |= flags;
	}
	return combined;
}

/* Wakes the faults waiting on an address space. */
static void
vmspace_fault_wake(
	struct vmspace *vm)
{
	unsigned long irq;

	irq = spin_lock_irqsave(&vm->lock.guard);
	waitq_wake_all(&vm->fault_waitq);
	spin_unlock_irqrestore(&vm->lock.guard, irq);
}

/* Marks every mapping busy and holds its region; called with the metadata and reclaim locks held. */
static void
pin_backing_mappings(
	struct vm_private_page *backing)
{
	struct vm_page *page;

	for (page = backing->mappings; page != NULL; page = page->private_next) {
		vmspace_ref(page->vm);
		mutex_lock(&page->vm->lock);
		if ((page->flags & VM_MAPPING_BUSY) != 0 ||
		    page->region->hold_count == (unsigned)-1)
			HAL_FATAL("VM reclaim mapping pin invariant failed");
		page->flags |= VM_MAPPING_BUSY;
		page->region->hold_count++;
		mutex_unlock(&page->vm->lock);
	}
}

/* Undoes pin_backing_mappings(), publishing unmapped PTEs; called with the metadata and reclaim locks held. */
static void
unpin_backing_mappings(
	struct vm_private_page *backing)
{
	struct vm_page *page;
	struct vmspace *vm;

	for (page = backing->mappings; page != NULL; page = page->private_next) {
		vm = page->vm;
		mutex_lock(&vm->lock);
		if ((page->flags & VM_MAPPING_RECLAIM_UNMAPPED) != 0) {
			page->flags &= ~(VM_MAPPING_RECLAIM_UNMAPPED |
			    VM_MAPPING_RECLAIM_PROTECTED | VM_MAPPING_MAPPED);
		}
		page->flags &= ~VM_MAPPING_RECLAIM_PROTECTED;
		page->flags &= ~VM_MAPPING_BUSY;
		if (page->region->hold_count == 0)
			HAL_FATAL("VM reclaim region hold underflow");
		page->region->hold_count--;
		vm->generation++;
		if (vm->generation == 0)
			vm->generation++;
		vmspace_fault_wake(vm);
		mutex_unlock(&vm->lock);

		/* Never run final vmspace destruction under either VM metadata lock. */
		vmspace_put_deferred(vm);
	}
}

/* Tests whether a backing's swap slot lies in a source. */
static int
private_page_targets_source(
	struct vm_private_page *backing,
	unsigned source_id)
{
	uint32_t slot;
	unsigned decoded_source;
	unsigned flags;
	unsigned long irq;

	/* Samples the swap state. */
	irq = spin_lock_irqsave(&backing->state_lock);
	flags = backing->flags;
	slot = backing->swap_slot;
	spin_unlock_irqrestore(&backing->state_lock, irq);

	/* The slot must decode to the source. */
	if ((flags & VM_PAGE_SWAPPED) == 0)
		return 0;
	if (slot == SWAP_SLOT_NONE)
		return 0;
	if (swap_slot_decode(slot, &decoded_source, NULL) != 0)
		return 0;
	if (decoded_source != source_id)
		return 0;
	return 1;
}

/* Unpins a drained backing's mappings under the locks. */
static void
drain_mapping_pins_release(
	struct vm_private_page *backing)
{
	vm_metadata_enter();
	mutex_lock(&reclaim_lock);
	unpin_backing_mappings(backing);
	mutex_unlock(&reclaim_lock);
	vm_metadata_leave();
}

/* Revokes and unmaps every PTE of an owned backing, collecting the dirty flags. */
static int
unmap_backing_ptes(
	struct vm_private_page *backing,
	uint32_t *pte_flags)
{
	struct vm_page *page;
	int error;
	uint32_t flags;
	uint32_t readonly;

	error = 0;

	/* Rejects a missing result. */
	if (pte_flags == NULL)
		return EINVAL;
	*pte_flags = 0;

	/*
	 * A pre-unmap query is not a stable dirty snapshot: userspace can write
	 * between the query and shootdown.  First make every mapping read-only
	 * and wait for that protection change, then collect flags and remove
	 * the PTEs.
	 */
	for (page = backing->mappings; page != NULL; page = page->private_next) {
		flags = 0;
		if ((page->flags & VM_MAPPING_MAPPED) == 0)
			continue;
		readonly = mapping_prot(page) & ~HAL_SPACE_WRITE;
		if (readonly == 0) {
			/*
			 * No non-writable representation exists for a write-only
			 * PTE.  Synchronous unmap is the revoke; conservatively
			 * write it back.
			 */
			if (hal_page_unmap(page->vm->space,
			    (void *)page->address, PAGE_SIZE) != HAL_OK) {
				error = EIO;
				break;
			}
			page->flags |= VM_MAPPING_RECLAIM_UNMAPPED;
			*pte_flags |= HAL_PAGE_DIRTY;
			continue;
		}
		if (hal_page_prot_query(page->vm->space,
		    (void *)page->address, PAGE_SIZE, readonly, &flags) != HAL_OK) {
			error = EIO;
			break;
		}
		page->flags |= VM_MAPPING_RECLAIM_PROTECTED;
		*pte_flags |= flags;
	}
	if (error != 0)
		goto rollback;

	/* Removes the protected PTEs. */
	for (page = backing->mappings; page != NULL; page = page->private_next) {
		if ((page->flags & VM_MAPPING_MAPPED) == 0 ||
		    (page->flags & VM_MAPPING_RECLAIM_UNMAPPED) != 0)
			continue;
		if (hal_page_unmap(page->vm->space, (void *)page->address,
		    PAGE_SIZE) != HAL_OK) {
			error = EIO;
			break;
		}
		page->flags |= VM_MAPPING_RECLAIM_UNMAPPED;
	}
	if (error == 0)
		return 0;

rollback:
	/* Roll back outside all VM locks.  A failed remap remains lazy. */
	for (page = backing->mappings; page != NULL; page = page->private_next) {
		if ((page->flags & VM_MAPPING_RECLAIM_UNMAPPED) != 0) {
			if (hal_page_map(page->vm->space, (void *)page->address,
			    backing->pmem.paddr, PAGE_SIZE,
			    mapping_prot(page)) == HAL_OK)
				page->flags &= ~(VM_MAPPING_RECLAIM_UNMAPPED |
				    VM_MAPPING_RECLAIM_PROTECTED);
			continue;
		}
		if ((page->flags & VM_MAPPING_RECLAIM_PROTECTED) == 0)
			continue;
		if (hal_page_prot(page->vm->space, (void *)page->address,
		    PAGE_SIZE, mapping_prot(page)) == HAL_OK) {
			page->flags &= ~VM_MAPPING_RECLAIM_PROTECTED;
			continue;
		}

		/* A stale read-only PTE would livelock a later write fault. */
		if (hal_page_unmap(page->vm->space, (void *)page->address,
		    PAGE_SIZE) != HAL_OK)
			HAL_FATAL("VM reclaim protection rollback failed");
		page->flags |= VM_MAPPING_RECLAIM_UNMAPPED;
	}

	/* Reports the failure. */
	return error;
}

/* Restores the PTEs of a backing whose reclaim was abandoned. */
static void
rollback_backing_ptes(
	struct vm_private_page *backing)
{
	struct vm_page *page;

	for (page = backing->mappings; page != NULL; page = page->private_next) {
		/* A failed remap remains lazy until the next fault. */
		if ((page->flags & VM_MAPPING_RECLAIM_UNMAPPED) != 0) {
			if (hal_page_map(page->vm->space, (void *)page->address,
			    backing->pmem.paddr, PAGE_SIZE,
			    mapping_prot(page)) == HAL_OK)
				page->flags &= ~(VM_MAPPING_RECLAIM_UNMAPPED |
				    VM_MAPPING_RECLAIM_PROTECTED);
			continue;
		}
		if ((page->flags & VM_MAPPING_RECLAIM_PROTECTED) == 0)
			continue;
		if (hal_page_prot(page->vm->space, (void *)page->address,
		    PAGE_SIZE, mapping_prot(page)) == HAL_OK) {
			page->flags &= ~VM_MAPPING_RECLAIM_PROTECTED;
			continue;
		}

		/* A stale read-only PTE is removed rather than left behind. */
		if (hal_page_unmap(page->vm->space, (void *)page->address,
		    PAGE_SIZE) != HAL_OK)
			HAL_FATAL("VM reclaim PTE rollback failed");
		page->flags |= VM_MAPPING_RECLAIM_UNMAPPED;
	}
}

/* Writes an owned, unmapped backing to swap and frees its page. */
static int
swap_out_backing_owned(
	struct vm_private_page *backing)
{
	struct swap_backend *backend;
	struct hal_pmem memory;
	uint32_t slot;
	unsigned long irq;
	int slot_allocated;
	int error;

	backend = swap_system_backend();
	slot_allocated = 0;

	/* Allocates a slot and writes the page. */
	if (PAGE_SIZE != SWAP_PAGE_SIZE || backend == NULL) {
		error = ENOSPC;
	} else {
		error = swap_alloc_slot(backend, &slot);
		if (error == 0)
			slot_allocated = 1;
	}
	if (error == 0)
		error = swap_write_page(backend, slot,
		    (const void *)backing->pmem.vaddr);

	/* A failure restores the PTEs and releases everything. */
	if (error != 0) {
		rollback_backing_ptes(backing);
		if (slot_allocated)
			swap_free_slot(backend, slot);
		vm_metadata_enter();
		mutex_lock(&reclaim_lock);
		unpin_backing_mappings(backing);
		if (error != ENOSPC)
			stats.io_errors++;
		mutex_unlock(&reclaim_lock);
		vm_metadata_leave();
		vm_private_page_io_release(backing);
		vm_private_page_put(backing);
		return error;
	}

	/* PTE shootdown and swap I/O are complete before the state is published. */
	memory = backing->pmem;
	(void)hal_pmem_free(&memory);
	vm_metadata_enter();
	mutex_lock(&reclaim_lock);
	irq = spin_lock_irqsave(&backing->state_lock);
	if ((backing->flags & (VM_PAGE_BUSY | VM_PAGE_RESIDENT)) !=
	    (VM_PAGE_BUSY | VM_PAGE_RESIDENT) ||
	    (backing->flags & VM_PAGE_SWAPPED) != 0)
		HAL_FATAL("VM swap-out state changed under I/O owner");
	memset(&backing->pmem, 0, sizeof(backing->pmem));
	backing->swap_slot = slot;
	backing->flags &= ~(VM_PAGE_RESIDENT | VM_PAGE_DIRTY);
	backing->flags |= VM_PAGE_SWAPPED;
	private_page_advance_locked(backing);
	spin_unlock_irqrestore(&backing->state_lock, irq);
	unpin_backing_mappings(backing);
	if (stats.resident)
		stats.resident--;
	stats.swapped++;
	stats.page_outs++;
	stats.reclaims++;
	mutex_unlock(&reclaim_lock);
	vm_metadata_leave();
	vm_private_page_io_release(backing);
	vm_private_page_put(backing);

	/* Reports the swapped-out backing. */
	return 0;
}

/* Discards a clean file-backed page, removing its mappings from their regions. */
static int
discard_file_backing_owned(
	struct vm_private_page *backing)
{
	struct vm_page *page;
	struct vm_page *next;
	struct hal_pmem memory;
	unsigned long irq;
	struct vm_page **link;
	struct vmspace *vm;

	/* Detaches every mapping from its region and frees it. */
	vm_metadata_enter();
	mutex_lock(&reclaim_lock);
	for (page = backing->mappings; page != NULL; page = next) {
		vm = page->vm;
		next = page->private_next;
		mutex_lock(&vm->lock);
		link = &page->region->pages;
		while (*link != NULL && *link != page)
			link = &(*link)->next;
		if (*link != page)
			HAL_FATAL("discarded VM page left its region");
		*link = page->next;
		if (page->region->hold_count == 0)
			HAL_FATAL("VM discard region hold underflow");
		page->region->hold_count--;
		vm->generation++;
		if (vm->generation == 0)
			vm->generation++;
		vmspace_fault_wake(vm);
		mutex_unlock(&vm->lock);
		page->private_page = NULL;
		page->private_next = NULL;
		if (refcount_put(&backing->refs))
			HAL_FATAL("VM discard lost I/O lifetime hold");
		vm_page_free_metadata(page);
		vmspace_put_deferred(vm);
	}

	/* Takes the backing off the queue and drops its page. */
	backing->mappings = NULL;
	queue_remove(backing);
	irq = spin_lock_irqsave(&backing->state_lock);
	if (backing->mapping_count == 0 ||
	    (backing->flags & (VM_PAGE_BUSY | VM_PAGE_TRACKED)) !=
	    (VM_PAGE_BUSY | VM_PAGE_TRACKED))
		HAL_FATAL("invalid VM file-discard backing state");
	backing->mapping_count = 0;
	backing->flags &= ~(VM_PAGE_TRACKED | VM_PAGE_RESIDENT |
	    VM_PAGE_DIRTY);
	memory = backing->pmem;
	memset(&backing->pmem, 0, sizeof(backing->pmem));
	private_page_advance_locked(backing);
	spin_unlock_irqrestore(&backing->state_lock, irq);
	if (stats.resident)
		stats.resident--;
	stats.reclaims++;
	mutex_unlock(&reclaim_lock);
	vm_metadata_leave();
	(void)hal_pmem_free(&memory);
	vm_private_page_io_release(backing);
	vm_private_page_put(backing);

	/* Reports the discarded backing. */
	return 0;
}

/* Reclaims an owned, pinned backing by discard or swap-out. */
static int
reclaim_backing_owned(
	struct vm_private_page *backing,
	int file_candidate)
{
	uint32_t pte_flags;
	unsigned state_flags;
	unsigned long irq;
	int error;

	/* Revokes the mappings, giving up on a shootdown failure. */
	error = unmap_backing_ptes(backing, &pte_flags);
	if (error != 0) {
		vm_metadata_enter();
		mutex_lock(&reclaim_lock);
		unpin_backing_mappings(backing);
		stats.io_errors++;
		mutex_unlock(&reclaim_lock);
		vm_metadata_leave();
		vm_private_page_io_release(backing);
		vm_private_page_put(backing);
		return error;
	}

	/* A clean file copy is discarded; anything else goes to swap. */
	irq = spin_lock_irqsave(&backing->state_lock);
	state_flags = backing->flags;
	spin_unlock_irqrestore(&backing->state_lock, irq);
	if (file_candidate &&
	    (state_flags & VM_PAGE_DIRTY) == 0 &&
	    (pte_flags & HAL_PAGE_DIRTY) == 0) {
		error = discard_file_backing_owned(backing);
		return error;
	}
	error = swap_out_backing_owned(backing);
	return error;
}
