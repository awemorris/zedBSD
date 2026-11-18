/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The private pages that back anonymous memory: their reference, I/O,
 * pin and operation state.
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

static int private_page_wait_sequence(struct vm_private_page *backing, uint64_t sequence);
static int private_page_wait_sequence_interruptible(struct vm_private_page *backing, uint64_t sequence);

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
	spin_init(&backing->state_lock, LOCK_RANK_VM_OBJECT, "VM private backing");
	waitq_init(&backing->state_waitq, "VM private backing");

	backing->generation = 1;
	backing->swap_slot = SWAP_SLOT_NONE;
	backing->queue_next = NULL;
	backing->queue_prev = NULL;
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
	struct kern_pmem memory;
	uint32_t slot;
	unsigned long irq;

	/* Only the last reference frees. */
	if (backing == NULL || !refcount_put(&backing->refs))
		return;

	/* Takes the page and slot out of a backing nothing else uses. */
	irq = spin_lock_irqsave(&backing->state_lock);

	/* Traps on a backing something still maps, holds, pins or owns. */
	if (backing->mapping_count != 0 ||
	    backing->mappings != NULL ||
	    backing->active_operations != 0 ||
	    backing->pin_count != 0 ||
	    (backing->flags & (VM_PAGE_BUSY | VM_PAGE_TRACKED)) != 0)
		HAL_FATAL("destroying active VM private backing");

	memory = backing->pmem;
	kern_memset(&backing->pmem, 0, sizeof(backing->pmem));

	slot = backing->swap_slot;
	backing->swap_slot = SWAP_SLOT_NONE;

	spin_unlock_irqrestore(&backing->state_lock, irq);

	/* Frees them and the metadata. */
	if (memory.size != 0)
		(void)hal_pmem_free(&memory.paddr, memory.size);

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

		/*
		 * Takes the page when it is idle: nobody owns it for I/O, no
		 * operation is running against it, and nothing has it pinned.
		 */
		if ((backing->flags & VM_PAGE_BUSY) == 0 &&
		    backing->active_operations == 0 &&
		    backing->pin_count == 0) {
			backing->flags |= VM_PAGE_BUSY;
			vm_reclaim_private_page_advance_locked(backing);
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

	/* Refuses a page that is not idle, rather than waiting for it. */
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
	vm_reclaim_private_page_advance_locked(backing);

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

	vm_reclaim_private_page_advance_locked(backing);

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

		/*
		 * Reports the page idle when nobody owns it for I/O, no
		 * operation is running against it, and nothing has it pinned.
		 */
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

	vm_reclaim_private_page_advance_locked(backing);
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
	vm_reclaim_private_page_advance_locked(backing);

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
	struct kern_pmem *memory)
{
	unsigned long irq;

	/* Rejects a missing backing or result. */
	if (backing == NULL || memory == NULL)
		return EINVAL;

	/* Fails while an owner or operation is present, or the page is absent. */

	/* Refuses a page that is busy, in use, or not in memory. */
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

	vm_reclaim_private_page_advance_locked(backing);
	if (backing->pin_count == 0)
		waitq_wake_all(&backing->state_waitq);

	spin_unlock_irqrestore(&backing->state_lock, irq);

	vm_private_page_put(backing);
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
	struct swap_backend *backend;
	uint32_t slot;
	unsigned long irq;
	int error;
	int tracked;
	struct kern_pmem memory;

	/* Rejects a missing operand or a backing that is not owned and swapped. */
	backend = swap_system_backend();
	if (backing == NULL || accounting_page == NULL || backend == NULL)
		return EIO;

	/* Takes the swap slot of an owned, swapped-out backing. */
	irq = spin_lock_irqsave(&backing->state_lock);

	/*
	 * Refuses a page that is not both owned for I/O by this caller and
	 * currently swapped out, because only such a page has a slot to read.
	 */
	if ((backing->flags & (VM_PAGE_BUSY | VM_PAGE_SWAPPED)) !=
	    (VM_PAGE_BUSY | VM_PAGE_SWAPPED)) {
		spin_unlock_irqrestore(&backing->state_lock, irq);
		return EIO;
	}

	slot = backing->swap_slot;

	spin_unlock_irqrestore(&backing->state_lock, irq);

	/* Allocates a page, reclaiming until one is free, and reads it. */
	error = vm_reclaim_frame(&backing->pmem, accounting_page);

	if (error == 0)
		error = swap_read_page(backend, slot,
		    (void *)hal_pmem_to_kernel(backing->pmem.paddr));

	/* Returns the page rather than leaving a failed read behind. */
	if (error != 0) {
		if (backing->pmem.size != 0) {
			memory = backing->pmem;
			kern_memset(&backing->pmem, 0, sizeof(backing->pmem));
			(void)hal_pmem_free(&memory.paddr, memory.size);
		}

		return error;
	}

	/*
	 * No source state is held while the backing operation completes.  The
	 * exclusive private-page I/O owner keeps the token stable until this
	 * resident state is published.
	 */
	swap_free_slot(backend, slot);

	/* Publishes the backing as resident and no longer swapped. */
	irq = spin_lock_irqsave(&backing->state_lock);

	if ((backing->flags & (VM_PAGE_BUSY | VM_PAGE_SWAPPED)) !=
	    (VM_PAGE_BUSY | VM_PAGE_SWAPPED) || backing->swap_slot != slot)
		HAL_FATAL("VM private page-in state changed under I/O owner");

	/* Forgets the slot, because the page now lives in memory again. */
	backing->swap_slot = SWAP_SLOT_NONE;
	backing->flags &= ~VM_PAGE_SWAPPED;

	/*
	 * The old slot was the only persistent copy.  Keep the resident page
	 * dirty until a later reclaim writes it to an active source.
	 */
	backing->flags |= VM_PAGE_RESIDENT | VM_PAGE_DIRTY;
	vm_reclaim_private_page_advance_locked(backing);

	/* Notes whether the reclaim lists hold the backing. */
	tracked = 0;
	if ((backing->flags & VM_PAGE_TRACKED) != 0)
		tracked = 1;

	spin_unlock_irqrestore(&backing->state_lock, irq);

	/* Moves a tracked backing from the swapped list back to the reclaim queue. */
	if (tracked) {
		vm_metadata_enter();
		mutex_lock(&reclaim_lock);

		vm_reclaim_queue_remove(backing);
		vm_reclaim_queue_insert(backing);

		mutex_unlock(&reclaim_lock);
		vm_metadata_leave();
	}

	/* Counts the page-in in the page's statistics. */
	vm_page_note_in(accounting_page);

	/* Reports the paged-in backing. */
	return 0;
}

/*
 * Allocates one page-sized physical run for the VM layer.
 */
int
vm_private_page_alloc(
	struct kern_pmem *memory)
{
	int error;

	/* Records the size so the matching free can split correctly. */
	error = hal_pmem_alloc(PAGE_SIZE, PAGE_SIZE, &memory->paddr);
	if (error != HAL_OK)
		return error;
	memory->size = PAGE_SIZE;
	return HAL_OK;
}

/*
 * Advances a backing's generation, skipping zero.
 */
void
vm_reclaim_private_page_advance_locked(
	struct vm_private_page *backing)
{
	backing->generation++;
	if (backing->generation == 0)
		backing->generation++;
}

/*
 * Waits for a backing to go idle, polling a cancel callback around each sleep.
 */
int
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

		/* Reports the page idle when nobody owns, runs against, or pins it. */
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

/* Sleeps on a backing's state queue for the sequence observed. */
static int
private_page_wait_sequence(
	struct vm_private_page *backing,
	uint64_t sequence)
{
	unsigned long irq;
	int error;

	/* Sleeps under the state lock, which waitq_sleep() drops and retakes. */
	irq = spin_lock_irqsave(&backing->state_lock);

	error = waitq_sleep(&backing->state_waitq, &backing->state_lock, sequence, 0, 0);

	spin_unlock_irqrestore(&backing->state_lock, irq);

	/* Reports why the sleep ended. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
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

	/* A signal ends this sleep as well as a wake-up does. */
	error = waitq_sleep(&backing->state_waitq, &backing->state_lock, sequence, 0, WAITQ_INTERRUPTIBLE);

	spin_unlock_irqrestore(&backing->state_lock, irq);

	/* Reports the signal or the failure that ended the sleep. */
	if (error != 0)
		return error;

	/* Succeeded: the backing reported progress. */
	return 0;
}
