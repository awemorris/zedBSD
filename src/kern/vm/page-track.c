/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Tracking of the mappings that share a private page, and replacing or
 * sharing that page.
 */

#include "kern/vm-object.h"
#include "kern/cred.h"
#include "kern/file.h"
#include "kern/kmem.h"
#include "kern/page.h"
#include "kern/vm-reclaim.h"
#include "kern/vm-lock.h"
#include "kern/vmspace.h"
#include "kern/vm-commit.h"
#include "kern/io-pool.h"
#include "kern/disk.h"
#include "kern/cache-memory.h"
#include "kern/writeback.h"
#include <uapi/errno.h>
#include <uapi/mman.h>
#include "kern/lock.h"
#include "kern/swap.h"
#include <hal/hal.h>
#include <kern/pmem.h>
#include <limits.h>
#include "kern/sched.h"
#include "kern/clock.h"
#include "kern/thread.h"
#include "kern/process.h"
#include "kern/signal.h"
#include "kern/klog.h"
#include <uapi/signal.h>
#include <kern/kcrt.h>
#include "reclaim-internal.h"

#define stats				vm_reclaim_counters

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

	/*
	 * Moves one page from the swapped count to the resident count and
	 * records the page-in.  The swapped count is only decremented when
	 * it is non-zero, because a page read for the first time was never
	 * counted as swapped.
	 */
	if (stats.swapped)
		stats.swapped--;
	stats.resident++;
	stats.page_ins++;

	mutex_unlock(&reclaim_lock);
	vm_metadata_leave();
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
	int swapped;

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

	/*
	 * Traps on a backing that cannot be tracked: one that no mapping
	 * refers to, or one the reclaim queue already holds.
	 */
	if (backing->mapping_count == 0 ||
	    (backing->flags & VM_PAGE_TRACKED) != 0)
		HAL_FATAL("invalid VM private backing track");

	/* Marks the backing as one the reclaim queue now holds. */
	backing->flags |= VM_PAGE_TRACKED;

	/* Notes whether the page is in memory, for the count kept below. */
	resident = (backing->flags & VM_PAGE_RESIDENT) != 0;

	/* Notes whether the page is on swap, which decides the list it joins. */
	swapped = 0;
	if ((backing->flags & VM_PAGE_SWAPPED) != 0)
		swapped = 1;

	spin_unlock_irqrestore(&backing->state_lock, irq);

	/* Puts a resident backing on the reclaim queue, a swapped one on the swapped list. */
	if (resident)
		vm_reclaim_queue_insert(backing);
	else if (swapped)
		vm_reclaim_swapped_insert(backing);

	/* Counts a resident page towards what reclaim can free. */
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

	/*
	 * Walks the reverse mapping list to the link that refers to this
	 * page, keeping the address of the link so it can be unlinked.
	 */
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

	/* Notes what the backing holds, for the counters below. */
	resident = (backing->flags & VM_PAGE_RESIDENT) != 0;
	swapped = (backing->flags & VM_PAGE_SWAPPED) != 0;
	tracked = (backing->flags & VM_PAGE_TRACKED) != 0;

	/* A backing that no mapping refers to leaves the reclaim queue. */
	if (last_mapping)
		backing->flags &= ~VM_PAGE_TRACKED;

	spin_unlock_irqrestore(&backing->state_lock, irq);

	/* Discharges the untracked backing from the reclaim counters. */
	if (last_mapping && tracked) {
		vm_reclaim_queue_remove(backing);

		/* Takes a page that was in memory off the resident count. */
		if (resident && stats.resident)
			stats.resident--;

		/* Takes a page that was on swap off the swapped count. */
		if (swapped && stats.swapped)
			stats.swapped--;
	}

	mutex_unlock(&reclaim_lock);
	vm_metadata_leave();

	/* Drops the reference this mapping held on the backing. */
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

	/*
	 * Traps unless the caller holds the old backing and at least one
	 * mapping still refers to it.  Holding it is owning it for I/O, or,
	 * for a copy-on-write break of a page another process has pinned,
	 * holding a pin of its own: a pin keeps owners, reclaim and swap away
	 * just as ownership does, and the reverse mapping list is changed
	 * below under reclaim_lock either way.
	 */
	if (((old->flags & VM_PAGE_BUSY) == 0 && old->pin_count == 0) ||
	    old->mapping_count == 0)
		HAL_FATAL("replacing an unowned VM private backing");

	spin_unlock_irqrestore(&old->state_lock, old_irq);

	/* Unlinks the page from the old reverse mapping list. */
	link = &old->mappings;
	while (*link != NULL && *link != page)
		link = &(*link)->private_next;
	if (*link != page)
		HAL_FATAL("private page mapping not linked");
	*link = page->private_next;

	/*
	 * Drops the mapping the old backing just lost, and remembers the
	 * state it was in, because the counters below are adjusted outside
	 * the state lock.
	 */
	old_irq = spin_lock_irqsave(&old->state_lock);

	/* Drops the mapping and samples the state the counters below need. */
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

	/* Gives the fresh backing this page as its one and only mapping. */
	fresh->mapping_count = 1;
	fresh->flags |= VM_PAGE_TRACKED;
	fresh_resident = (fresh->flags & VM_PAGE_RESIDENT) != 0;

	spin_unlock_irqrestore(&fresh->state_lock, fresh_irq);

	/* Points the page at the fresh backing. */
	page->private_page = fresh;
	page->private_next = fresh->mappings;

	/* Links the page into the fresh backing's reverse mapping list. */
	fresh->mappings = page;

	/* Puts a resident fresh backing at the head of the reclaim queue. */
	if (fresh_resident)
		vm_reclaim_queue_insert(fresh);

	/* Counts a resident page towards what reclaim can free. */
	if (fresh_resident)
		stats.resident++;

	/* An old backing without mappings leaves the queue. */
	if (old_last) {
		vm_reclaim_queue_remove(old);
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

	/*
	 * Links the copy into the same backing as the source and marks both
	 * sides copy-on-write, so the first write to either one faults.
	 */
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

	/* Reports why the share failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}
