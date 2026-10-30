/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Changes to an object's content and size: the content and resize
 * transactions, inode I/O waits and coherent reads.
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
#include "object-internal.h"

/*
 * Optional collaborators.
 *
 * Each of these lives in a translation unit that some platforms leave out of
 * the kernel, so the reference is weak and every call site tests it first.
 */
extern void writeback_ticket_commit(struct writeback_ticket *, size_t) __attribute__((weak));
extern int disk_cache_acquire(struct disk *, struct disk **) __attribute__((weak));
extern void disk_cache_release(struct disk *) __attribute__((weak));
extern int vmspace_object_page_revoke(struct vm_object_page *, uint32_t *) __attribute__((weak));

static void vm_object_content_finish(struct vm_object_content *content, const void *buffer, size_t committed, int commit);
static void vm_object_resize_finish(struct vm_object_resize *resize, off_t logical_size, int commit);

/*
 * Waits until an inode has no resize, content, or read transaction and
 * no object in final teardown.
 */
int
vm_object_inode_io_wait(
	struct inode *inode)
{
	struct vm_object *object;
	unsigned long irq;
	bool enabled;
	int error;
	uint64_t sequence;

	/* Rejects a missing inode. */
	if (inode == NULL)
		return EINVAL;

	/* Retries until both the inode and its object are quiet. */
	for (;;) {
		irq = spin_lock_irqsave(&inode->i_vm_lock);
		if (inode->i_vm_resize_active ||
		    inode->i_vm_content_active ||
		    inode->i_vm_content_readers != 0) {
			sequence = waitq_sequence(&inode->i_vm_waitq);
			error = waitq_sleep(&inode->i_vm_waitq, &inode->i_vm_lock,
			    sequence, 0, 0);
			spin_unlock_irqrestore(&inode->i_vm_lock, irq);
			if (error != 0 && error != EAGAIN)
				return error;
			continue;
		}

		spin_unlock_irqrestore(&inode->i_vm_lock, irq);

		/*
		 * A resize cannot join an object while its final writeback owns
		 * the DETACHING transition.  Wait here, outside
		 * inode->i_io_lock.
		 */
		enabled = vm_object_registry_lock();

		object = vm_object_find_by_inode_locked(inode);
		if (object == NULL ||
		    (object->flags & VM_OBJECT_DETACHING) == 0) {
			vm_object_registry_unlock(enabled);

			/* Succeeded: no teardown stands in the way. */
			return 0;
		}

		sequence = waitq_sequence(&object->registry_waitq);

		object->registry_waiters++;
		if (object->registry_waiters == 0)
			HAL_FATAL("VM object registry waiter overflow");

		refcount_get(&object->refs);
		vm_object_registry_unlock(enabled);

		error = vm_object_wait_registry_transition(object, sequence);
		if (error != 0 && error != EAGAIN)
			return error;
	}
}

/*
 * Tests whether an inode has a resize, content, or read transaction.
 */
int
vm_object_inode_resize_active(
	struct inode *inode)
{
	unsigned long irq;
	int active;

	/* Ignores a missing inode. */
	if (inode == NULL)
		return 0;

	/* Samples the three gates together. */
	irq = spin_lock_irqsave(&inode->i_vm_lock);

	active = 0;
	if (inode->i_vm_resize_active != 0 ||
	    inode->i_vm_content_active != 0 ||
	    inode->i_vm_content_readers != 0)
		active = 1;

	spin_unlock_irqrestore(&inode->i_vm_lock, irq);

	/* Reports the sampled state. */
	return active;
}

/*
 * Takes a read lease on an inode's content, excluding resizes and
 * content writes.
 */
int
vm_object_content_read_begin(
	struct inode *inode)
{
	unsigned long irq;

	/* Rejects anything but a regular file. */
	if (inode == NULL || inode->i_type != INODE_REG)
		return EINVAL;

	/* Counts the reader unless a transaction is in progress. */

	/* Refuses a reader while a resize or a write owns the inode. */
	irq = spin_lock_irqsave(&inode->i_vm_lock);
	if (inode->i_vm_resize_active || inode->i_vm_content_active) {
		spin_unlock_irqrestore(&inode->i_vm_lock, irq);
		return EBUSY;
	}

	/* Refuses a reader the counter cannot hold. */
	if (inode->i_vm_content_readers == UINT_MAX) {
		spin_unlock_irqrestore(&inode->i_vm_lock, irq);
		return EOVERFLOW;
	}

	inode->i_vm_content_readers++;

	spin_unlock_irqrestore(&inode->i_vm_lock, irq);

	/* Reports the taken lease. */
	return 0;
}

/*
 * Releases a read lease, waking transactions waiting for the last one.
 */
void
vm_object_content_read_end(
	struct inode *inode)
{
	unsigned long irq;

	/* Ignores a missing inode. */
	if (inode == NULL)
		return;

	/* Counts the reader down. */
	irq = spin_lock_irqsave(&inode->i_vm_lock);

	if (inode->i_vm_content_readers == 0)
		HAL_FATAL("VM content read lease underflow");

	inode->i_vm_content_readers--;
	if (inode->i_vm_content_readers == 0)
		waitq_wake_all(&inode->i_vm_waitq);

	spin_unlock_irqrestore(&inode->i_vm_lock, irq);
}

/*
 * Reports whether an inode has a published object: 1, 0, or -EAGAIN
 * while one is in final teardown.
 */
int
vm_object_cache_published(
	struct inode *inode)
{
	struct vm_object *object;
	bool enabled;
	int result;

	/* Rejects a missing inode. */
	if (inode == NULL)
		return -EINVAL;

	/* Classifies the registry entry. */
	enabled = vm_object_registry_lock();

	/* Classifies the registry entry the inode has, if it has one. */
	object = vm_object_find_by_inode_locked(inode);
	if (object == NULL)
		result = 0;
	else if ((object->flags & VM_OBJECT_DETACHING) != 0)
		result = -EAGAIN;
	else
		result = 1;

	vm_object_registry_unlock(enabled);

	/* Reports a teardown in progress as a refusal. */
	if (result < 0)
		return result;

	/* Succeeded: reports whether an object is published. */
	return result;
}

/*
 * Begins a content write transaction on a file's inode.
 *
 * The transaction may nest inside a prepared resize by the same owner.
 * A published object is marked so that faults wait for the commit.
 */
int
vm_object_content_begin(
	struct file *file,
	off_t offset,
	size_t length,
	struct vm_object_resize *resize_owner,
	struct vm_object_content *content)
{
	struct inode *inode;
	struct vm_object *object;
	unsigned long inode_irq;
	unsigned long object_irq;
	uint64_t generation;
	bool enabled;

	/* Rejects a missing file or content, or a range that wraps. */
	if (file == NULL ||
	    content == NULL ||
	    offset < 0 ||
	    (uint64_t)offset + length < (uint64_t)offset)
		return EINVAL;

	kern_memset(content, 0, sizeof(*content));

	/* An empty range needs no transaction. */
	if (length == 0)
		return 0;

	/* Requires a regular file that carries VM state. */
	inode = file_vm_inode(file);
	if (inode == NULL || inode->i_type != INODE_REG)
		return EINVAL;

	/* Only one content transaction runs, and never under a read lease. */
	enabled = vm_object_registry_lock();
	inode_irq = spin_lock_irqsave(&inode->i_vm_lock);

	if (inode->i_vm_content_active || inode->i_vm_content_readers != 0) {
		spin_unlock_irqrestore(&inode->i_vm_lock, inode_irq);
		vm_object_registry_unlock(enabled);
		return EBUSY;
	}

	/* A resize owner must own the inode's current prepared resize. */
	if (resize_owner != NULL) {
		if (!resize_owner->active ||
		    !resize_owner->prepared ||
		    resize_owner->inode != inode ||
		    !inode->i_vm_resize_active ||
		    inode->i_vm_resize_generation != resize_owner->generation) {
			spin_unlock_irqrestore(&inode->i_vm_lock, inode_irq);
			vm_object_registry_unlock(enabled);
			return EINVAL;
		}
	} else if (inode->i_vm_resize_active) {
		spin_unlock_irqrestore(&inode->i_vm_lock, inode_irq);
		vm_object_registry_unlock(enabled);
		return EBUSY;
	}

	/* Refuses an object that is already tearing down. */
	object = vm_object_find_by_inode_locked(inode);
	if (object != NULL && (object->flags & VM_OBJECT_DETACHING) != 0) {
		spin_unlock_irqrestore(&inode->i_vm_lock, inode_irq);
		vm_object_registry_unlock(enabled);
		return EAGAIN;
	}

	/* Publishes the transaction on the inode and the object. */
	generation = vm_object_next_inode_content_generation_locked(inode);
	inode->i_vm_content_active = 1;
	inode->i_vm_content_start = offset;
	inode->i_vm_content_end = offset + (off_t)length;
	if (object != NULL) {
		object_irq = spin_lock_irqsave(&object->lock);
		if ((object->flags & VM_OBJECT_CONTENT) != 0)
			HAL_FATAL("VM object content gate diverged from inode");
		object->flags |= VM_OBJECT_CONTENT;
		object->content_generation = generation;
		waitq_wake_all(&object->page_waitq);
		spin_unlock_irqrestore(&object->lock, object_irq);
		object->active_operations++;
		if (object->active_operations == 0)
			HAL_FATAL("VM object operation counter overflow");

		/* A mapped object keeps the writer that may flush its pages. */
		if (object->mapping_count != 0 && object->write_file == NULL) {
			file_ref(file);
			object->write_file = file;
		}
	}

	spin_unlock_irqrestore(&inode->i_vm_lock, inode_irq);

	vm_object_registry_unlock(enabled);

	/* Describes the transaction to its caller. */
	content->inode = inode;
	content->resize_owner = resize_owner;
	content->offset = offset;
	content->length = length;
	content->generation = generation;
	content->active = 1;

	/* Reports the begun transaction. */
	return 0;
}

/*
 * Prepares a content transaction by revoking and flushing the cached
 * pages it overlaps.
 *
 * The pages stay busy under the transaction until it finishes, so no
 * fault or pin can see the old data once the backend changes.
 */
int
vm_object_content_prepare(
	struct vm_object_content *content)
{
	struct vm_object *object;
	uint64_t start;
	uint64_t end;
	bool enabled;
	int first_error;
	unsigned long irq;
	struct vm_object_page *page;
	int dirty;
	int has_mappings;
	struct vm_object_page *scan;
	int wait;
	uint64_t sequence;
	int error;
	uint32_t observed;

	first_error = 0;

	/* Rejects an inactive transaction. */
	if (content == NULL || !content->active || content->inode == NULL)
		return EINVAL;
	start = (uint64_t)content->offset;
	end = start + content->length;

	/* Holds the object across the page work; none means nothing cached. */
	enabled = vm_object_registry_lock();
	object = vm_object_find_by_inode_locked(content->inode);
	if (object != NULL) {
		irq = spin_lock_irqsave(&object->lock);
		if ((object->flags & VM_OBJECT_CONTENT) == 0 ||
		    object->content_generation != content->generation)
			HAL_FATAL("VM object lost content transaction");
		spin_unlock_irqrestore(&object->lock, irq);
		refcount_get(&object->refs);
	}

	vm_object_registry_unlock(enabled);

	/* An uncached inode has no page to prepare. */
	if (object == NULL) {
		content->prepared = 1;
		return 0;
	}

	/* Takes each overlapping page in turn once it is idle. */
	for (;;) {
		/* Waits for one overlapping page to become takeable. */
		page = NULL;
		irq = spin_lock_irqsave(&object->lock);
		for (;;) {
			/* Looks for an untaken overlapping page. */
			wait = 0;
			for (scan = object->pages; scan != NULL; scan = scan->next) {
				/* Skips a page outside the range or already taken. */
				if (!vm_object_page_overlaps(scan, start, end) ||
				    scan->content_generation == content->generation)
					continue;

				/* Waits out a page that is busy or held. */
				if ((scan->flags & (VM_OBJECT_PAGE_BUSY |
				    VM_OBJECT_PAGE_WRITEBACK)) != 0 ||
				    scan->hold_count != scan->pin_count) {
					wait = 1;
					break;
				}

				page = scan;
				break;
			}

			/* Stops once a page is found or none has to be waited for. */
			if (page != NULL || !wait)
				break;

			/* Sleeps until the object reports page progress. */
			sequence = waitq_sequence(&object->page_waitq);
			error = waitq_sleep(&object->page_waitq,
			    &object->lock, sequence, 0, 0);
			if (error != 0 && error != EAGAIN) {
				first_error = error;
				break;
			}
		}

		/* Ends the pass when no page is left or the wait failed. */
		if (page == NULL || first_error != 0) {
			spin_unlock_irqrestore(&object->lock, irq);
			break;
		}

		/* Takes the page out of service for this transaction. */
		page->flags |= VM_OBJECT_PAGE_BUSY | VM_OBJECT_PAGE_WRITEBACK;
		vm_object_prefetch_retire(page);
		page->content_generation = content->generation;
		has_mappings = page->mapping_count != 0;
		spin_unlock_irqrestore(&object->lock, irq);

		/* Revokes the mappings, collecting the hardware dirty bit. */
		observed = 0;
		error = 0;
		if (has_mappings) {
			if (vmspace_object_page_revoke == NULL)
				error = EOPNOTSUPP;
			else
				error = vmspace_object_page_revoke(page,
				    &observed);
		}

		/* Folds the revoked state into the page's dirty record. */
		irq = spin_lock_irqsave(&object->lock);
		if ((observed & HAL_SPACE_PAGE_DIRTY) != 0) {
			vm_object_page_dirty_mark(page);
		}

		dirty = (page->flags & VM_OBJECT_PAGE_DIRTY) != 0;
		page->write_dirty_generation = page->dirty_generation;
		spin_unlock_irqrestore(&object->lock, irq);

		/* Writes a dirty page back before the caller overwrites it. */
		if (error == 0 && dirty)
			error = vm_object_write_page_data(object, page,
			    object->logical_size, 0);

		/* Clears the dirty record only for a write nothing raced. */
		irq = spin_lock_irqsave(&object->lock);
		if (error == 0 && dirty &&
		    page->dirty_generation == page->write_dirty_generation) {
			vm_object_clear_page_dirty_locked(page);
		} else if (error != 0) {
			vm_object_record_writeback_error_locked(object, error);
			first_error = error;
		}

		spin_unlock_irqrestore(&object->lock, irq);
		if (first_error != 0)
			break;
	}

	/* A failure releases the pages taken so far. */
	if (first_error != 0) {
		irq = spin_lock_irqsave(&object->lock);
		vm_object_content_release_pages_locked(object, content->generation);
		spin_unlock_irqrestore(&object->lock, irq);
	} else {
		content->prepared = 1;
	}

	/* Drops the reference this transaction held on the object. */
	if (refcount_put(&object->refs))
		HAL_FATAL("content transaction lost registry reference");

	/* Reports the preparation result. */
	return first_error;
}

/*
 * Commits a content transaction, copying the written bytes into the
 * cached pages.
 */
void
vm_object_content_commit(
	struct vm_object_content *content,
	const void *buffer,
	size_t committed)
{
	vm_object_content_finish(content, buffer, committed, 1);
}

/*
 * Abandons a content transaction, leaving the cached pages as they were.
 */
void
vm_object_content_abort(
	struct vm_object_content *content)
{
	vm_object_content_finish(content, NULL, 0, 0);
}

/*
 * Reads from an inode through its published object, faulting pages in.
 *
 * Reports ENOENT without a published object; a short read at the end
 * of file is not an error.
 */
int
vm_object_read_coherent(
	struct inode *inode,
	off_t offset,
	void *buffer,
	size_t length,
	ssize_t *result)
{
	int error;

	/* Preserves the existing coherent-read contract for callers without feedback. */

	/* Reports the failure. */
	error = vm_object_read_coherent_useful(inode, offset, buffer, length, result, NULL);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Reads coherent bytes while reporting newly confirmed speculative consumption.
 */
int
vm_object_read_coherent_useful(
	struct inode *inode,
	off_t offset,
	void *buffer,
	size_t length,
	ssize_t *result,
	size_t *useful)
{
	struct vm_object *object;
	struct disk *cache_disk;
	uint8_t *bytes;
	size_t done;
	bool enabled;
	int error;
	uint64_t sequence;
	struct vm_object_page *page;
	off_t current;
	off_t logical_size;
	off_t page_offset;
	size_t in_page;
	size_t chunk;
	unsigned long irq;
	ssize_t populated;
	int handled;
	int stop;
	size_t consumed;

	if (useful != NULL)
		*useful = 0;
	bytes = buffer;
	done = 0;
	error = 0;

	/* Rejects a missing inode, result, or buffer, or a negative offset. */
	if (inode == NULL ||
	    result == NULL ||
	    offset < 0 ||
	    (buffer == NULL && length != 0))
		return EINVAL;

retry_lookup:
	/* Holds the object as an operation, waiting out a teardown. */
	enabled = vm_object_registry_lock();
	object = vm_object_find_by_inode_locked(inode);
	if (object == NULL) {
		vm_object_registry_unlock(enabled);
		return ENOENT;
	}

	/* Waits out a teardown and looks the object up again. */
	if ((object->flags & VM_OBJECT_DETACHING) != 0) {
		sequence = waitq_sequence(&object->registry_waitq);
		object->registry_waiters++;
		if (object->registry_waiters == 0)
			HAL_FATAL("VM object registry waiter overflow");
		refcount_get(&object->refs);
		vm_object_registry_unlock(enabled);
		error = vm_object_wait_registry_transition(object, sequence);
		if (error == 0 || error == EAGAIN)
			goto retry_lookup;
		return error;
	}

	/* Counts this read as an operation and pins the object. */
	object->active_operations++;
	if (object->active_operations == 0)
		HAL_FATAL("VM object read operation counter overflow");
	refcount_get(&object->refs);
	vm_object_registry_unlock(enabled);

	/* Covers cache hits with the same detach barrier as device-backed reads. */
	cache_disk = NULL;
	if (inode->i_mount != NULL && inode->i_mount->m_disk != NULL &&
	    disk_cache_acquire != NULL && disk_cache_release != NULL)
		error = disk_cache_acquire(inode->i_mount->m_disk, &cache_disk);

	/* Reports device retirement as an error rather than the fault EOF sentinel. */
	if (error != 0) {
		vm_object_operation_end(object);
		if (refcount_put(&object->refs))
			vm_object_destroy(object);
		return error;
	}

	/* Copies resident pages or bounded miss runs up to the end of file. */
	while (error == 0 && done < length) {
		/* Places the next byte inside its page and clamps the copy. */
		current = offset + (off_t)done;
		page_offset = current & ~(off_t)(PAGE_SIZE - 1U);
		in_page = (size_t)(current - page_offset);
		chunk = PAGE_SIZE - in_page;
		irq = spin_lock_irqsave(&object->lock);
		logical_size = object->logical_size;
		spin_unlock_irqrestore(&object->lock, irq);
		if (current >= logical_size)
			break;
		if (chunk > length - done)
			chunk = length - done;
		if ((off_t)chunk > logical_size - current)
			chunk = (size_t)(logical_size - current);

		/* Serves an absent run through one bounded internal backend request. */
		error = vm_object_cache_read_missing(object, current, bytes + done,
		    length - done, &populated, &handled, &stop);
		if (error != 0)
			break;
		if (handled) {
			done += (size_t)populated;
			if (stop || populated == 0)
				break;
			continue;
		}

		/* Faults the page in and copies the overlapping bytes out of it. */
		error = vm_object_fault(object, page_offset, &page);
		if (error != 0)
			break;
		irq = spin_lock_irqsave(&object->lock);
		if (page->hold_count == 0)
			HAL_FATAL("VM object coherent read lost fault hold");
		kern_memcpy_ram(bytes + done, (const uint8_t *)hal_pmem_to_kernel(page->pmem.paddr) + in_page,
		    chunk);

		/* Credits the prefetch that made this page resident. */
		consumed = vm_object_prefetch_consume(page, in_page, chunk);
		if (useful != NULL)
			*useful += consumed;

		/* Releases the fault hold and reports the progress. */
		page->hold_count--;
		waitq_wake_all(&object->page_waitq);
		spin_unlock_irqrestore(&object->lock, irq);
		done += chunk;
	}

	/* Releases the device admission and the object this read held. */
	if (cache_disk != NULL)
		disk_cache_release(cache_disk);
	vm_object_operation_end(object);
	if (refcount_put(&object->refs))
		vm_object_destroy(object);

	/* Data, a clean end, or the end of file is a success. */
	if (done != 0 || error == 0 || error == ENXIO) {
		*result = (ssize_t)done;
		return 0;
	}

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Begins a resize transaction on an inode.
 *
 * A published object records the transaction so faults wait for it; a
 * resize to the current size completes immediately.
 */
int
vm_object_resize_begin(
	struct inode *inode,
	off_t target_size,
	struct vm_object_resize *resize)
{
	struct vm_object *object;
	unsigned long inode_irq;
	unsigned long object_irq;
	uint64_t generation;
	bool enabled;

	/* Rejects a missing inode or result, or a negative size. */
	if (inode == NULL || resize == NULL || target_size < 0)
		return EINVAL;
	kern_memset(resize, 0, sizeof(*resize));

	/* Only one transaction runs, and never under a read lease. */
	enabled = vm_object_registry_lock();

	/* Refuses a resize while another transaction or a reader owns the inode. */
	inode_irq = spin_lock_irqsave(&inode->i_vm_lock);
	if (inode->i_vm_resize_active ||
	    inode->i_vm_content_active ||
	    inode->i_vm_content_readers != 0) {
		spin_unlock_irqrestore(&inode->i_vm_lock, inode_irq);
		vm_object_registry_unlock(enabled);
		return EBUSY;
	}

	/* Refuses an object that is already tearing down. */
	object = vm_object_find_by_inode_locked(inode);
	if (object != NULL && (object->flags & VM_OBJECT_DETACHING) != 0) {
		spin_unlock_irqrestore(&inode->i_vm_lock, inode_irq);
		vm_object_registry_unlock(enabled);
		return EAGAIN;
	}

	/*
	 * Once published, the object's EOF is authoritative for mappings.
	 * Generic writes/truncates keep it equal to inode->i_size.
	 */
	resize->old_size = inode->i_size;
	if (object != NULL) {
		object_irq = spin_lock_irqsave(&object->lock);
		if ((object->flags & VM_OBJECT_RESIZING) != 0)
			HAL_FATAL("VM object resize state diverged from inode");
		resize->old_size = object->logical_size;
		spin_unlock_irqrestore(&object->lock, object_irq);
	}

	/* A resize to the current size has nothing to publish. */
	resize->inode = inode;
	resize->target_size = target_size;
	if (resize->old_size == target_size) {
		spin_unlock_irqrestore(&inode->i_vm_lock, inode_irq);
		vm_object_registry_unlock(enabled);
		return 0;
	}

	/* Publishes the transaction on the inode and the object. */
	generation = vm_object_next_inode_resize_generation_locked(inode);
	inode->i_vm_resize_active = 1;
	inode->i_vm_resize_old_size = resize->old_size;
	inode->i_vm_resize_target_size = target_size;
	resize->generation = generation;
	resize->active = 1;
	if (object != NULL) {
		object_irq = spin_lock_irqsave(&object->lock);
		object->flags |= VM_OBJECT_RESIZING;
		object->resize_generation = generation;
		object->size_generation = generation;
		waitq_wake_all(&object->page_waitq);
		spin_unlock_irqrestore(&object->lock, object_irq);
		object->active_operations++;
		if (object->active_operations == 0)
			HAL_FATAL("VM object operation counter overflow");
	}

	spin_unlock_irqrestore(&inode->i_vm_lock, inode_irq);

	vm_object_registry_unlock(enabled);

	/* Reports the begun transaction. */
	return 0;
}

/*
 * Prepares a resize transaction by writing back and invalidating the
 * pages the new end of file affects.
 */
int
vm_object_resize_prepare(
	struct vm_object_resize *resize)
{
	struct vm_object *object;
	uint64_t start;
	uint64_t length;
	bool enabled;
	int error;

	error = 0;

	/* Rejects an inactive transaction. */
	if (resize == NULL || !resize->active || resize->inode == NULL)
		return EINVAL;

	/* Holds the object across the page work; none means nothing cached. */
	enabled = vm_object_registry_lock();
	object = vm_object_find_by_inode_locked(resize->inode);
	if (object != NULL) {
		if ((object->flags & VM_OBJECT_RESIZING) == 0 ||
		    object->resize_generation != resize->generation)
			HAL_FATAL("VM object lost resize transaction");
		refcount_get(&object->refs);
	}

	vm_object_registry_unlock(enabled);
	if (object == NULL) {
		resize->prepared = 1;
		return 0;
	}

	/*
	 * Shrink invalidates every possibly discarded page.  Grow only needs
	 * the old partial EOF page; it may contain mmap stores beyond the old
	 * EOF, which must never become visible after extension.
	 */
	if (resize->target_size < resize->old_size) {
		start = (uint64_t)resize->target_size &
		    ~(uint64_t)(PAGE_SIZE - 1U);
		length = (uint64_t)resize->old_size - start;
	} else if (resize->old_size != 0 &&
	    ((uint64_t)resize->old_size & (PAGE_SIZE - 1U)) != 0) {
		start = (uint64_t)resize->old_size &
		    ~(uint64_t)(PAGE_SIZE - 1U);
		length = (uint64_t)resize->old_size - start;
	} else {
		start = 0;
		length = 0;
	}

	if (length != 0)
		error = vm_object_sync_range_internal(object, (off_t)start,
		    (size_t)length, MS_SYNC | MS_INVALIDATE, 0, 1,
		    resize->target_size);
	if (refcount_put(&object->refs))
		HAL_FATAL("resize transaction lost VM object registry reference");
	if (error == 0)
		resize->prepared = 1;

	/* Reports why the preparation failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Commits a resize transaction with the final size.
 */
void
vm_object_resize_commit(
	struct vm_object_resize *resize,
	off_t logical_size)
{
	vm_object_resize_finish(resize, logical_size, 1);
}

/*
 * Abandons a resize transaction, restoring the old size.
 */
void
vm_object_resize_abort(
	struct vm_object_resize *resize)
{
	off_t old_size;

	if (resize != NULL)
		old_size = resize->old_size;
	else
		old_size = 0;

	vm_object_resize_finish(resize, old_size, 0);
}

/*
 * Gives an unpublished object the inode's end of file and any transaction in
 * progress.
 */
void
vm_object_initialize_eof_locked(
	struct vm_object *object,
	struct inode *inode)
{
	unsigned long irq;

	irq = spin_lock_irqsave(&inode->i_vm_lock);

	/* A resize in progress pins registry teardown until it finishes. */
	if (inode->i_vm_resize_active) {
		/*
		 * Adopts the size the resize started from, and joins the
		 * resize as one more operation holding the object open.
		 */
		object->logical_size = inode->i_vm_resize_old_size;
		object->size_generation = inode->i_vm_resize_generation;
		object->resize_generation = inode->i_vm_resize_generation;
		object->flags |= VM_OBJECT_RESIZING;
		object->active_operations = 1;
	} else {
		/* Adopts the size the file has right now. */
		object->logical_size = inode->i_size;

		/*
		 * Continues the inode's size generation where a past resize
		 * left it, and starts at one when there has never been one.
		 */
		if (inode->i_vm_resize_generation != 0)
			object->size_generation = inode->i_vm_resize_generation;
		else
			object->size_generation = 1U;
	}

	/* So does a content write in progress. */
	if (inode->i_vm_content_active) {
		/*
		 * Adopts the write's content generation and joins it as one
		 * more operation holding the object open.
		 */
		object->flags |= VM_OBJECT_CONTENT;
		object->content_generation = inode->i_vm_content_generation;
		object->active_operations++;

		/* Traps when the operation counter wraps. */
		if (object->active_operations == 0)
			HAL_FATAL("VM object operation counter overflow");
	}

	spin_unlock_irqrestore(&inode->i_vm_lock, irq);
}

/*
 * Waits until no resize or content transaction is active on an object.
 */
int
vm_object_wait_resize(
	struct vm_object *object)
{
	unsigned long irq;
	int error;
	uint64_t sequence;

	/* Sleeps while a resize or content transaction owns the object. */
	irq = spin_lock_irqsave(&object->lock);

	/* Sleeps until no transaction owns the object. */
	while ((object->flags & (VM_OBJECT_RESIZING | VM_OBJECT_CONTENT)) != 0) {
		sequence = waitq_sequence(&object->page_waitq);
		error = waitq_sleep(&object->page_waitq, &object->lock,
		    sequence, 0, 0);
		if (error != 0 && error != EAGAIN) {
			spin_unlock_irqrestore(&object->lock, irq);
			return error;
		}
	}

	spin_unlock_irqrestore(&object->lock, irq);

	/* Reports an object no transaction owns any more. */
	return 0;
}

/*
 * Releases the pages a content transaction took; the caller holds the object
 * lock.
 */
void
vm_object_content_release_pages_locked(
	struct vm_object *object,
	uint64_t generation)
{
	struct vm_object_page *page;

	/* Puts every page this transaction took back into service. */
	for (page = object->pages; page != NULL; page = page->next) {
		if (page->content_generation != generation)
			continue;

		/* Returns the page to service, owned by no transaction. */
		page->content_generation = 0;
		page->write_generation = 0;
		page->write_dirty_generation = 0;
		page->flags &= ~(VM_OBJECT_PAGE_BUSY |
		    VM_OBJECT_PAGE_WRITEBACK);
	}

	/* Wakes whoever waited for those pages. */
	waitq_wake_all(&object->page_waitq);
}

/* Ends a content transaction, copying committed bytes into the taken pages. */
static void
vm_object_content_finish(
	struct vm_object_content *content,
	const void *buffer,
	size_t committed,
	int commit)
{
	struct inode *inode;
	struct vm_object *object;
	unsigned long inode_irq;
	unsigned long object_irq;
	int wake_registry;
	bool enabled;
	struct vm_object_page *page;
	uint64_t write_start;
	uint64_t write_end;
	uint64_t page_start;
	uint64_t page_end;
	uint64_t copy_start;
	uint64_t copy_end;

	wake_registry = 0;

	/* Ignores an inactive transaction; a commit must be prepared and bounded. */
	if (content == NULL || !content->active || content->inode == NULL)
		return;
	if (commit) {
		/*
		 * Traps on a commit that cannot be honoured: one the
		 * transaction never prepared, one claiming more bytes than
		 * the transaction covers, or one with bytes but no buffer.
		 */
		if (!content->prepared ||
		    committed > content->length ||
		    (committed != 0 && buffer == NULL))
			HAL_FATAL("invalid VM content commit");
	}

	/* Enters the inode under the registry, refusing a lost transaction. */
	inode = content->inode;
	enabled = vm_object_registry_lock();
	inode_irq = spin_lock_irqsave(&inode->i_vm_lock);

	if (!inode->i_vm_content_active ||
	    inode->i_vm_content_generation != content->generation)
		HAL_FATAL("inode lost VM content transaction");

	/* Updates the taken pages from the committed bytes and releases them. */
	object = vm_object_find_by_inode_locked(inode);
	if (object != NULL) {
		write_start = (uint64_t)content->offset;
		write_end = write_start + committed;

		object_irq = spin_lock_irqsave(&object->lock);

		/*
		 * Traps when the object no longer carries this transaction,
		 * because the pages below would then belong to another one.
		 */
		if ((object->flags & VM_OBJECT_CONTENT) == 0 ||
		    object->content_generation != content->generation)
			HAL_FATAL("VM object content generation mismatch");

		/* Copies the committed bytes into every page this transaction took. */
		for (page = object->pages; page != NULL; page = page->next) {
			/* Skips a page that belongs to another transaction. */
			if (page->content_generation != content->generation)
				continue;

			/* Leaves a page untouched when nothing was committed. */
			if (!commit || committed == 0)
				continue;

			/* Intersects this page with the committed byte range. */
			page_start = (uint64_t)page->offset;
			page_end = page_start + PAGE_SIZE;

			if (page_start > write_start)
				copy_start = page_start;
			else
				copy_start = write_start;

			if (page_end < write_end)
				copy_end = page_end;
			else
				copy_end = write_end;

			/* Skips a page the committed range does not reach. */
			if (copy_start >= copy_end)
				continue;

			/* Copies the overlapping bytes into the page. */
			kern_memcpy_ram((uint8_t *)hal_pmem_to_kernel(page->pmem.paddr) +
			       (copy_start - page_start),
			       (const uint8_t *)buffer +
			       (copy_start - write_start),
			       (size_t)(copy_end - copy_start));

			/* Leaves an immediate write without delayed credit. */
			if (content->writeback_ticket == NULL)
				continue;

			/*
			 * Charges the page against the transaction's credit the
			 * first time it is dirtied, and refuses a page that
			 * changed owner.
			 */
			if (page->writeback_budget == NULL) {
				if (writeback_ticket_commit == NULL)
					HAL_FATAL("missing delayed write credit owner");

				writeback_ticket_commit(content->writeback_ticket, PAGE_SIZE);
				page->writeback_budget = content->writeback_ticket->budget;
			} else if (page->writeback_budget != content->writeback_ticket->budget) {
				HAL_FATAL("delayed page changed credit owner");
			}

			/* Publishes the page as dirty for the syncer. */
			vm_object_page_dirty_mark(page);
		}

		/* Releases the taken pages and closes the transaction on the object. */
		vm_object_content_release_pages_locked(object, content->generation);
		object->flags &= ~VM_OBJECT_CONTENT;
		object->content_generation = vm_object_next_generation(object);
		waitq_wake_all(&object->page_waitq);
		spin_unlock_irqrestore(&object->lock, object_irq);

		/* Drops the operation, pinning the object for a registry wakeup. */
		if (object->active_operations == 0)
			HAL_FATAL("VM object content operation counter underflow");

		object->active_operations--;

		wake_registry = object->active_operations == 0;
		if (wake_registry)
			refcount_get(&object->refs);
	}

	/* Closes the transaction on the inode. */
	inode->i_vm_content_active = 0;
	inode->i_vm_content_start = 0;
	inode->i_vm_content_end = 0;

	(void)vm_object_next_inode_content_generation_locked(inode);
	waitq_wake_all(&inode->i_vm_waitq);

	spin_unlock_irqrestore(&inode->i_vm_lock, inode_irq);

	vm_object_registry_unlock(enabled);

	/*
	 * Releases the object outside every lock, waking whoever waited on
	 * the registry for this transaction to end.
	 */
	if (wake_registry) {
		vm_object_wake_registry_waiters(object);
		if (refcount_put(&object->refs))
			vm_object_destroy(object);
	}

	/* Empties the descriptor so a second finish does nothing. */
	kern_memset(content, 0, sizeof(*content));
}

/* Ends a resize transaction, publishing the size and settling the orphans. */
static void
vm_object_resize_finish(
	struct vm_object_resize *resize,
	off_t logical_size,
	int commit)
{
	struct inode *inode;
	struct vm_object *object;
	struct vm_object_page *free_pages;
	unsigned long inode_irq;
	unsigned long object_irq;
	uint64_t stable_generation;
	int wake_registry;
	bool enabled;
	struct vm_object_page *page;
	struct vm_object_page **link;

	/* Starts with no page to free and nobody to wake. */
	free_pages = NULL;
	wake_registry = 0;

	/* Ignores an inactive transaction; a commit must be prepared. */
	if (resize == NULL || !resize->active || resize->inode == NULL)
		return;

	/*
	 * Traps on a commit the transaction never prepared, or one that
	 * names a size no file can have.
	 */
	if (commit && (!resize->prepared || logical_size < 0))
		HAL_FATAL("committing unprepared VM object resize");

	inode = resize->inode;

	enabled = vm_object_registry_lock();
	inode_irq = spin_lock_irqsave(&inode->i_vm_lock);

	/* Traps when the inode no longer carries this resize. */
	if (!inode->i_vm_resize_active ||
	    inode->i_vm_resize_generation != resize->generation)
		HAL_FATAL("inode lost VM object resize transaction");

	/* Takes the generation the settled size is published under. */
	stable_generation = vm_object_next_inode_resize_generation_locked(inode);

	/* Finds the object this resize has to settle, if one exists. */
	object = vm_object_find_by_inode_locked(inode);

	if (object != NULL) {
		object_irq = spin_lock_irqsave(&object->lock);

		if ((object->flags & VM_OBJECT_RESIZING) == 0 ||
		    object->resize_generation != resize->generation)
			HAL_FATAL("VM object resize generation mismatch");

		if (commit) {
			/*
			 * Orphans still pinned remain owner-linked until their
			 * last unpin.  Orphans whose pins drained during prepare
			 * can be reclaimed as soon as the transaction is
			 * published.
			 */
			object->logical_size = logical_size;
			link = &object->orphan_pages;
			while (*link != NULL) {
				page = *link;
				if (page->hold_count != 0) {
					link = &page->next;
					continue;
				}

				if (page->pin_count != 0 || page->mapping_count != 0)
					HAL_FATAL("invalid VM resize orphan state");
				*link = page->next;
				page->flags &= ~VM_OBJECT_PAGE_ORPHANED;
				page->next = free_pages;
				free_pages = page;
			}
		} else {
			/*
			 * No replacement page can be faulted while RESIZING is
			 * set, so an abort may atomically return every orphan to
			 * the cache.
			 */
			object->logical_size = resize->old_size;
			page = object->orphan_pages;
			while (page != NULL) {
				object->orphan_pages = page->next;
				page->flags &= ~VM_OBJECT_PAGE_ORPHANED;
				page->next = object->pages;
				object->pages = page;
				vm_object_page_index_insert(object, page);
				vm_object_page_dirty_link(page);
				page = object->orphan_pages;
			}
		}

		object->size_generation = stable_generation;
		object->resize_generation = 0;
		object->flags &= ~VM_OBJECT_RESIZING;

		waitq_wake_all(&object->page_waitq);
		spin_unlock_irqrestore(&object->lock, object_irq);
		if (object->active_operations == 0)
			HAL_FATAL("VM object resize operation counter underflow");

		object->active_operations--;
		wake_registry = object->active_operations == 0;

		if (wake_registry)
			refcount_get(&object->refs);
	}

	/* Closes the transaction on the inode and frees the settled orphans. */
	inode->i_vm_resize_active = 0;
	inode->i_vm_resize_old_size = 0;
	inode->i_vm_resize_target_size = 0;

	waitq_wake_all(&inode->i_vm_waitq);

	spin_unlock_irqrestore(&inode->i_vm_lock, inode_irq);

	vm_object_registry_unlock(enabled);

	/*
	 * Releases the object outside every lock, waking whoever waited on
	 * the registry for this resize to end.
	 */
	if (wake_registry) {
		vm_object_wake_registry_waiters(object);
		if (refcount_put(&object->refs))
			vm_object_destroy(object);
	}

	/* Frees the orphans the resize left behind, outside every lock. */
	while (free_pages != NULL) {
		page = free_pages;
		free_pages = page->next;
		vm_object_free_page(page);
	}

	/* Empties the descriptor so a second finish does nothing. */
	kern_memset(resize, 0, sizeof(*resize));
}
