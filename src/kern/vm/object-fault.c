/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Faults on object pages, page pins and the mappings an object records.
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

static int vm_object_page_pin_copy(struct vm_object_page *page, size_t offset, void *buffer, size_t length, int write, int nowait);
static unsigned fault_around_prepare(struct vm_object *object, struct vm_object_page *page, off_t size, struct vm_object_page **pages);
static ssize_t fault_read(struct vm_object *object, struct file_io *io, struct vm_object_page *const *pages, unsigned count, off_t size, unsigned *filled);
static unsigned fault_around_publish_locked(struct vm_object *object, struct vm_object_page **pages, unsigned count);

/*
 * The pages a fault on a file whose data the page cache reads itself
 * reads at once: the faulted page and the missing pages after it, as a
 * sequential reader or an executable's loader goes on to use them.
 */
#define VM_OBJECT_FAULT_AROUND	16U

/*
 * Reports whether an object's pages are read from the disk by the page
 * cache itself (the inode's data_map) rather than through the file's own
 * read: a file object, opened on the inode it caches (not through a
 * stacking file system), whose file system maps its data, and which no
 * backing claim reserves.
 */
int
vm_object_page_io_candidate(
	const struct vm_object *object)
{
	const struct file *file;

	/* An anonymous object, or one without its file, reads nothing from a disk. */
	if ((object->flags & VM_OBJECT_ANONYMOUS) != 0 || object->file == NULL ||
	    object->inode == NULL)
		return 0;

	/* Only a file opened on the cached inode itself, unclaimed, of a file system that maps. */
	file = object->file;
	if (file->f_inode != object->inode || file->f_backing_claim != NULL ||
	    file->f_format_claim != NULL)
		return 0;
	if (object->inode->i_op == NULL || object->inode->i_op->data_map == NULL)
		return 0;

	/* Reports that the page cache reads it. */
	return 1;
}

/*
 * Reads part of a page through the file's own read, inside the caller's
 * file_io (a read of FILE_IO_VM_OBJECT, which holds the inode's I/O lock).
 */
ssize_t
vm_object_file_read_fallback(
	void *argument,
	off_t offset,
	void *buffer,
	size_t length)
{
	struct file_io *io;
	ssize_t count;

	/* Reads at the offset the page cache names. */
	io = argument;
	io->offset = offset;
	count = file_io_transfer(io, buffer, length);

	/* Reports what the file read. */
	return count;
}

/*
 * Faults a page of an object in, returning it with a hold.
 *
 * The fault waits out resize and content transactions, retries when
 * one starts underneath it, and reads the backend outside every lock.
 */
int
vm_object_fault(
	struct vm_object *object,
	off_t offset,
	struct vm_object_page **result)
{
	struct vm_object_page *around[VM_OBJECT_FAULT_AROUND];
	struct vm_object_page *page;
	struct file_io io;
	unsigned around_count;
	unsigned around_filled;
	unsigned around_kept;
	int page_io;
	off_t fault_size;
	uint64_t fault_generation;
	uint64_t fault_content_generation;
	size_t length;
	ssize_t count;
	unsigned long irq;
	unsigned read_retries;
	unsigned waits;
	int reclaimed;
	int owned;
	int error;
	uint64_t sequence;

	read_retries = 0;
	around_count = 0;
	around_filled = 1;

	/* Rejects a missing object or result, or an unaligned offset. */
	if (object == NULL ||
	    result == NULL ||
	    offset < 0 ||
	    (offset & (PAGE_SIZE - 1U)) != 0)
		return EINVAL;

retry:
	/* Waits for any transaction and samples the generations. */
	irq = spin_lock_irqsave(&object->lock);

	/* Waits out any transaction, then samples what the fault is measured against. */
	while ((object->flags & (VM_OBJECT_RESIZING | VM_OBJECT_CONTENT)) != 0) {
		sequence = waitq_sequence(&object->page_waitq);
		error = waitq_sleep(&object->page_waitq, &object->lock,
		    sequence, 0, 0);
		if (error != 0 && error != EAGAIN) {
			spin_unlock_irqrestore(&object->lock, irq);
			return error;
		}
	}

	fault_generation = object->size_generation;
	fault_content_generation = object->content_generation;
	fault_size = object->logical_size;

	/* A cached page is held once it is not busy and still current. */
	page = vm_object_find_page(object, offset);
	if (page != NULL) {
		/*
		 * Waits out a busy page, then looks it up again: nothing holds
		 * the page across the sleep, and the owner of the busy state
		 * (a write-back that invalidates, for one) may take it out of
		 * the index and free its descriptor meanwhile.
		 */
		if ((page->flags & VM_OBJECT_PAGE_BUSY) != 0) {
			sequence = waitq_sequence(&object->page_waitq);
			error = waitq_sleep(&object->page_waitq, &object->lock,
			    sequence, 0, 0);
			spin_unlock_irqrestore(&object->lock, irq);
			if (error != 0 && error != EAGAIN)
				return error;
			goto retry;
		}

		if ((object->flags & (VM_OBJECT_RESIZING |
		    VM_OBJECT_CONTENT)) != 0 ||
		    object->size_generation != fault_generation ||
		    object->content_generation != fault_content_generation) {
			spin_unlock_irqrestore(&object->lock, irq);
			goto retry;
		}

		/* A page whose read failed is retried inside the file. */
		if ((page->flags & VM_OBJECT_PAGE_ERROR) != 0) {
			if (offset >= fault_size) {
				if (page->error != 0)
					error = page->error;
				else
					error = ENXIO;
				spin_unlock_irqrestore(&object->lock, irq);
				return error;
			}

			page->flags &= ~VM_OBJECT_PAGE_ERROR;
			page->flags |= VM_OBJECT_PAGE_BUSY | VM_OBJECT_PAGE_FILL;
			page->error = 0;
			spin_unlock_irqrestore(&object->lock, irq);
			goto read_page;
		}

		/* Holds the hit for the caller and gives it a second chance on the clock. */
		page->hold_count++;
		vm_object_page_touch(page);
		*result = page;
		spin_unlock_irqrestore(&object->lock, irq);
		return 0;
	}

	spin_unlock_irqrestore(&object->lock, irq);

	if (offset >= fault_size)
		return ENXIO;

	/*
	 * Allocates a new busy page, reclaiming and then waiting while memory
	 * is short.  A caller inside the metadata lock cannot wait.
	 */
	page = vm_object_alloc_page(object, offset, 0);
	for (waits = 0; page == NULL; waits++) {
		reclaimed = vm_reclaim_one(NULL);
		if (reclaimed != 0) {
			owned = vm_metadata_owned();
			if (owned || waits >= VM_OBJECT_FAULT_WAITS)
				return ENOMEM;
			vm_reclaim_wait_free();
		}
		page = vm_object_alloc_page(object, offset, 0);
	}

	/* Publishes it unless the world changed or another fault won. */
	irq = spin_lock_irqsave(&object->lock);

	/*
	 * Starts over when a transaction, a size change or another fault
	 * overtook this one while the page was being read.
	 */
	if ((object->flags & (VM_OBJECT_RESIZING | VM_OBJECT_CONTENT)) != 0 ||
	    object->size_generation != fault_generation ||
	    object->content_generation != fault_content_generation ||
	    vm_object_find_page(object, offset) != NULL) {
		spin_unlock_irqrestore(&object->lock, irq);
		vm_object_release_page_storage(page);
		goto retry;
	}

	vm_object_page_index_insert(object, page);
	(void)atomic_fetch_add_relaxed(&object_pages, 1);

	spin_unlock_irqrestore(&object->lock, irq);

read_page:
	/* Reads the page's data, zero-filling past the end of file. */
	kern_memset((void *)hal_pmem_to_kernel(page->pmem.paddr), 0, PAGE_SIZE);
	length = (size_t)(fault_size - offset);
	if (length > PAGE_SIZE)
		length = PAGE_SIZE;

	/* Takes private frames for the missing pages after it, read in the same request. */
	around[0] = page;
	around_count = 1;
	page_io = vm_object_page_io_candidate(object);
	if (page_io)
		around_count = fault_around_prepare(object, page, fault_size, around);

read_io_retry:
	/* An anonymous object needs no backend read; its page starts zeroed. */
	if ((object->flags & VM_OBJECT_ANONYMOUS) != 0) {
		count = (ssize_t)length;
	} else {
		/* Reads the page through the backend under the file's I/O lease. */
		error = file_io_begin(object->file, FILE_IO_PREAD, offset,
		    FILE_IO_VM_OBJECT, &io);
		if (error != 0) {
			count = -error;
		} else {
			/*
			 * i_io_lock is now held.  A resize published after page
			 * BUSY but before the backend read is detected before
			 * stale I/O starts.
			 */
			irq = spin_lock_irqsave(&object->lock);
			error = 0;
			if ((object->flags & (VM_OBJECT_RESIZING |
			    VM_OBJECT_CONTENT)) != 0 ||
			    object->size_generation != fault_generation ||
			    object->content_generation != fault_content_generation ||
			    offset >= object->logical_size)
				error = EAGAIN;
			spin_unlock_irqrestore(&object->lock, irq);

			/* Transfers only into a page the file still describes. */
			if (error == 0)
				count = fault_read(object, &io, around, around_count, fault_size, &around_filled);
			else
				count = -error;
			file_io_end(&io);
		}
	}

	/* A read that ran out of memory retries after reclaiming a reserve. */
	if (count == -(ssize_t)ENOMEM &&
	    read_retries < VM_OBJECT_FAULT_RECLAIM_RETRIES &&
	    vm_reclaim_object_fault_reserve() != 0) {
		read_retries++;
		goto read_io_retry;
	}

	/* Publishes the result, marking the page in error on failure. */
	irq = spin_lock_irqsave(&object->lock);

	/* Fails the fault when a transaction or a size change overtook the read. */
	if ((object->flags & (VM_OBJECT_RESIZING | VM_OBJECT_CONTENT)) != 0 ||
	    object->size_generation != fault_generation ||
	    object->content_generation != fault_content_generation) {
		page->flags = VM_OBJECT_PAGE_ERROR;
		page->error = EAGAIN;
		waitq_wake_all(&object->page_waitq);
		spin_unlock_irqrestore(&object->lock, irq);
		vm_object_cache_discard_prepared(around + 1, around_count - 1U);
		return EAGAIN;
	}

	/* Fails the fault when the backend did not return the whole page. */
	if (count != (ssize_t)length) {
		if (count < 0)
			error = (int)-count;
		else
			error = EIO;
		page->flags = VM_OBJECT_PAGE_ERROR;
		page->error = error;
		waitq_wake_all(&object->page_waitq);
		spin_unlock_irqrestore(&object->lock, irq);
		vm_object_cache_discard_prepared(around + 1, around_count - 1U);
		return error;
	}

	page->flags = 0;
	page->error = 0;
	page->hold_count = 1;

	/* Publishes the pages read around it, against the same generations. */
	around_kept = 1;
	if (around_filled == around_count)
		around_kept = fault_around_publish_locked(object, around, around_count);

	waitq_wake_all(&object->page_waitq);

	*result = page;

	spin_unlock_irqrestore(&object->lock, irq);

	/* Gives back the frames of the pages around it that were not published. */
	vm_object_cache_discard_prepared(around + around_kept, around_count - around_kept);

	/* Reports the held page. */
	return 0;
}

/*
 * Allocates private frames for the missing pages that follow a faulted
 * page, up to VM_OBJECT_FAULT_AROUND pages in all, the end of file and the
 * first page already resident, without waiting for memory.  pages[0] is
 * the faulted page; reports how many pages the run holds.
 */
static unsigned
fault_around_prepare(
	struct vm_object *object,
	struct vm_object_page *page,
	off_t size,
	struct vm_object_page **pages)
{
	struct vm_object_page *resident;
	struct vm_object_page *frame;
	off_t next;
	unsigned count;
	unsigned long irq;

	/* Adds one page after another while it is missing and memory is to spare. */
	for (count = 1; count < VM_OBJECT_FAULT_AROUND; count++) {
		/* Stops at the end of file. */
		next = page->offset + (off_t)count * PAGE_SIZE;
		if (next >= size)
			break;

		/* Stops at a page that is already resident. */
		irq = spin_lock_irqsave(&object->lock);

		resident = vm_object_find_page(object, next);

		spin_unlock_irqrestore(&object->lock, irq);
		if (resident != NULL)
			break;

		/* Takes a frame when memory spares one, without waiting. */
		frame = vm_object_alloc_page(object, next, 1);
		if (frame == NULL)
			break;
		pages[count] = frame;
	}

	/* Reports the run. */
	return count;
}

/*
 * Reads a fault's run of pages, the faulted page first, inside its
 * file_io: from the disk by the page cache when the file allows it, else
 * the faulted page alone through the file's own read.  Reports the bytes
 * of the faulted page read or -errno.
 */
static ssize_t
fault_read(
	struct vm_object *object,
	struct file_io *io,
	struct vm_object_page *const *pages,
	unsigned count,
	off_t size,
	unsigned *filled)
{
	size_t length;
	size_t page_length;
	ssize_t count_read;
	int page_io;
	int error;

	/* The faulted page's bytes, up to the end of file. */
	*filled = 1;
	page_length = (size_t)(size - pages[0]->offset);
	if (page_length > PAGE_SIZE)
		page_length = PAGE_SIZE;

	/* Reads the run from the disk when the page cache reads this file. */
	page_io = vm_object_page_io_candidate(object);
	if (page_io) {
		length = (size_t)(size - pages[0]->offset);
		if (length > (size_t)count * PAGE_SIZE)
			length = (size_t)count * PAGE_SIZE;
		error = vm_object_io_read_pages(object->inode, pages[0]->offset, pages, count,
		    length, &io->context, vm_object_file_read_fallback, io);
		if (error == 0) {
			*filled = count;
			return (ssize_t)page_length;
		}
		if (error != EOPNOTSUPP)
			return -(ssize_t)error;
	}

	/* Reads the faulted page through the file. */
	count_read = file_io_transfer(io, (void *)hal_pmem_to_kernel(pages[0]->pmem.paddr), page_length);

	/* Reports what the file read. */
	return count_read;
}

/*
 * Publishes the pages a fault read around its page (all but pages[0]), in
 * order, up to the first offset another page took meanwhile or the end of
 * file, and reports how many pages of the run are published, the faulted
 * one included.  The caller holds the object's lock and has found the
 * file's generations unchanged since the read began, the same check the
 * faulted page is published under.
 */
static unsigned
fault_around_publish_locked(
	struct vm_object *object,
	struct vm_object_page **pages,
	unsigned count)
{
	struct vm_object_page *resident;
	struct vm_object_page *page;
	unsigned kept;

	/* Publishes each page while its offset is still free and inside the file. */
	for (kept = 1; kept < count; kept++) {
		/* Stops at the end of file, which may have moved down. */
		page = pages[kept];
		if (page->offset >= object->logical_size)
			break;

		/* Stops where another page took the offset meanwhile. */
		resident = vm_object_find_page(object, page->offset);
		if (resident != NULL)
			break;

		/* Publishes the page as resident and clean. */
		page->flags = 0;
		page->error = 0;
		vm_object_page_index_insert(object, page);
		(void)atomic_fetch_add_relaxed(&object_pages, 1);
	}

	/* Reports the published prefix. */
	return kept;
}

/*
 * Pins an object page for copying, holding its object as an operation.
 */
int
vm_object_page_pin(
	struct vm_object_page *page)
{
	struct vm_object *object;
	unsigned long irq;
	bool enabled;

	/* Rejects a missing page or one without an owner. */
	if (page == NULL)
		return EINVAL;

	object = page->owner;
	if (object == NULL)
		return EINVAL;

	/*
	 * The caller supplies a stable page pointer (normally while holding
	 * VM metadata).  Registry -> object is the same lifetime order as
	 * detach.
	 */
	enabled = vm_object_registry_lock();

	if ((object->flags & VM_OBJECT_DETACHING) != 0) {
		vm_object_registry_unlock(enabled);
		return EBUSY;
	}

	irq = spin_lock_irqsave(&object->lock);

	if (page->owner != object ||
	    page->hold_count == UINT_MAX ||
	    page->pin_count == UINT_MAX) {
		spin_unlock_irqrestore(&object->lock, irq);
		vm_object_registry_unlock(enabled);

		if (page->owner == object)
			return EOVERFLOW;

		return EINVAL;
	}

	/* Takes the hold, the pin, the operation, and a reference. */
	page->hold_count++;
	page->pin_count++;

	object->active_operations++;
	if (object->active_operations == 0)
		HAL_FATAL("VM object pin operation counter overflow");
	refcount_get(&object->refs);

	spin_unlock_irqrestore(&object->lock, irq);

	vm_object_registry_unlock(enabled);

	/* Reports the pinned page. */
	return 0;
}

/*
 * Copies out of a pinned page.
 */
int
vm_object_page_pin_read(
	struct vm_object_page *page,
	size_t offset,
	void *buffer,
	size_t length)
{
	int error;

	/* Reports why the copy failed. */
	error = vm_object_page_pin_copy(page, offset, buffer, length, 0, 0);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Copies into a pinned page, marking it dirty.
 */
int
vm_object_page_pin_write(
	struct vm_object_page *page,
	size_t offset,
	const void *buffer,
	size_t length)
{
	int error;

	/* Reports why the copy failed. */
	error = vm_object_page_pin_copy(page, offset, (void *)buffer, length, 1, 0);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Copies out of a pinned page unless a write-back owns it (EBUSY).  A fault
 * that holds a mapping reserved uses it: a sync that owns the page waits
 * for that reservation while revoking the page's mappings (BUG-082).
 */
int
vm_object_page_pin_read_nowait(
	struct vm_object_page *page,
	size_t offset,
	void *buffer,
	size_t length)
{
	int error;

	/* Reports why the copy failed. */
	error = vm_object_page_pin_copy(page, offset, buffer, length, 0, 1);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Waits until no write-back owns a pinned page, holding no VM lock.
 */
int
vm_object_page_pin_wait(
	struct vm_object_page *page)
{
	uint8_t nothing;
	int error;

	/* A zero-length copy waits and moves nothing. */
	error = vm_object_page_pin_copy(page, 0, &nothing, 0, 0, 0);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Releases a pin, freeing a resize orphan whose last pin it was.
 */
void
vm_object_page_unpin(
	struct vm_object_page *page)
{
	struct vm_object *object;
	struct vm_object_page *free_page;
	unsigned long irq;

	free_page = NULL;

	/* Ignores a missing page or one without an owner. */
	if (page == NULL)
		return;
	object = page->owner;
	if (object == NULL)
		return;

	/* Drops the pin and hold; an orphan with no holds left is freed. */
	irq = spin_lock_irqsave(&object->lock);

	if (page->hold_count == 0 || page->pin_count == 0)
		HAL_FATAL("VM object page pin underflow");
	page->pin_count--;
	page->hold_count--;

	/* Takes an orphan out of the object once its last hold is gone. */
	if ((page->flags & VM_OBJECT_PAGE_ORPHANED) != 0 &&
	    (object->flags & VM_OBJECT_RESIZING) == 0 &&
	    page->hold_count == 0) {
		if (page->pin_count != 0 ||
		    page->mapping_count != 0 ||
		    !vm_object_unlink_page_locked(&object->orphan_pages, page))
			HAL_FATAL("lost VM object resize orphan");
		page->flags &= ~VM_OBJECT_PAGE_ORPHANED;
		free_page = page;
	}

	waitq_wake_all(&object->page_waitq);

	spin_unlock_irqrestore(&object->lock, irq);

	/* Frees the unlinked orphan and ends the pin's operation. */
	if (free_page != NULL)
		vm_object_free_page(free_page);
	vm_object_operation_end(object);

	/*
	 * A concurrent final put may have unlinked the object after the
	 * operation wake.  The pin reference makes this the unique
	 * final-destroy point.
	 */
	if (refcount_put(&object->refs))
		vm_object_destroy(object);
}

/*
 * Releases the hold a fault returned without mapping the page.
 */
void
vm_object_fault_release(
	struct vm_object_page *page)
{
	unsigned long irq;

	/* Ignores a missing page or one without an owner. */
	if (page == NULL || page->owner == NULL)
		return;

	/* Drops the hold and wakes the waiters. */
	irq = spin_lock_irqsave(&page->owner->lock);

	if (page->hold_count == 0)
		HAL_FATAL("VM object fault hold underflow");
	page->hold_count--;
	waitq_wake_all(&page->owner->page_waitq);

	spin_unlock_irqrestore(&page->owner->lock, irq);
}

/*
 * Holds a page the object already caches, without reading or waiting.
 *
 * Fault-around maps the neighbours of a faulting page only when they are
 * already there: a page that is absent, being read, failed, past the end
 * of the file, or inside a resize or content transaction is left to its
 * own fault.  The hold is released as vm_object_fault()'s is.
 */
int
vm_object_fault_resident(
	struct vm_object *object,
	off_t offset,
	struct vm_object_page **result)
{
	struct vm_object_page *page;
	unsigned long irq;
	int error;

	/* Rejects a missing object or result, or an unaligned offset. */
	if (object == NULL ||
	    result == NULL ||
	    offset < 0 ||
	    (offset & (PAGE_SIZE - 1U)) != 0)
		return EINVAL;

	/* Looks the page up under the object lock. */
	error = 0;
	page = NULL;
	irq = spin_lock_irqsave(&object->lock);

	/* Leaves the page to its own fault while a transaction runs. */
	if ((object->flags & (VM_OBJECT_RESIZING | VM_OBJECT_CONTENT)) != 0)
		error = EAGAIN;

	/* Leaves a page past the end of the file to its own fault. */
	if (error == 0 && offset >= object->logical_size)
		error = ENXIO;

	/* Finds the cached page. */
	if (error == 0) {
		page = vm_object_find_page(object, offset);
		if (page == NULL)
			error = ENOENT;
	}

	/* Skips a page being read or one whose read failed. */
	if (error == 0 &&
	    (page->flags & (VM_OBJECT_PAGE_BUSY | VM_OBJECT_PAGE_ERROR)) != 0)
		error = EAGAIN;

	/* Holds the page for the caller's mapping; a hit is a use the clock remembers. */
	if (error == 0) {
		page->hold_count++;
		vm_object_page_touch(page);
		*result = page;
	}

	/* Lets the object's other users in. */
	spin_unlock_irqrestore(&object->lock, irq);

	/* Reports whether the page was held. */
	return error;
}

/*
 * Converts a fault hold into a reverse mapping.
 */
void
vm_object_mapping_add(
	struct vm_object_page *object_page,
	struct vm_page *mapping)
{
	unsigned long irq;

	/* Ignores a missing page or mapping. */
	if (object_page == NULL || mapping == NULL || object_page->owner == NULL)
		return;

	/* Links the mapping and releases the hold under the metadata lock. */
	vm_metadata_enter();
	irq = spin_lock_irqsave(&object_page->owner->lock);

	/* Turns the fault's hold into a published mapping. */
	if (object_page->hold_count == 0)
		HAL_FATAL("mapping VM object page without fault hold");

	/*
	 * Puts the mapping at the head of the list.  Each mapping's
	 * object_link points at the link that points at it, so that it can
	 * leave without a walk of the list.
	 */
	mapping->object_next = object_page->mappings;
	if (mapping->object_next != NULL)
		mapping->object_next->object_link = &mapping->object_next;
	mapping->object_link = &object_page->mappings;

	/* The mapping is published, and the fault's hold becomes it. */
	object_page->mappings = mapping;
	object_page->mapping_count++;
	object_page->hold_count--;

	/*
	 * A shared mapping that may be written can store into the page
	 * without a write through the file; from now on a sync looks at
	 * every mapped page of the object, not only the dirty ones.  A
	 * private mapping copies the page before it stores into it.
	 */
	if (mapping->region != NULL &&
	    (mapping->region->flags & VM_REGION_SHARED) != 0 &&
	    (mapping->region->max_prot & HAL_SPACE_WRITE) != 0)
		object_page->owner->store_mapped = 1;

	waitq_wake_all(&object_page->owner->page_waitq);

	spin_unlock_irqrestore(&object_page->owner->lock, irq);
	vm_metadata_leave();
}

/*
 * Unlinks a reverse mapping; the caller holds the owner's lock.
 */
void
vm_object_mapping_remove_locked(
	struct vm_object_page *object_page,
	struct vm_page *mapping)
{
	struct vm_page **link;

	/*
	 * The page knows the link that points at it, so it leaves without a
	 * walk of the list, which a page of a shared library makes as long
	 * as the number of processes.  A page that is not linked is left as
	 * it is.
	 */
	link = mapping->object_link;
	if (link == NULL || *link != mapping)
		return;

	/* Unlinks the mapping; the one after it now hangs from its link. */
	*link = mapping->object_next;
	if (mapping->object_next != NULL)
		mapping->object_next->object_link = link;
	mapping->object_next = NULL;
	mapping->object_link = NULL;

	/* One mapping fewer keeps the object page. */
	if (object_page->mapping_count == 0)
		HAL_FATAL("VM object mapping counter underflow");
	object_page->mapping_count--;
}

/*
 * Unlinks a reverse mapping under the owner's lock.
 */
void
vm_object_mapping_remove(
	struct vm_object_page *object_page,
	struct vm_page *mapping)
{
	unsigned long irq;

	/* Ignores a missing page or mapping. */
	if (object_page == NULL || mapping == NULL || object_page->owner == NULL)
		return;

	/* Unlinks under the metadata and owner locks. */
	vm_metadata_enter();
	irq = spin_lock_irqsave(&object_page->owner->lock);

	vm_object_mapping_remove_locked(object_page, mapping);

	spin_unlock_irqrestore(&object_page->owner->lock, irq);
	vm_metadata_leave();
}

/*
 * Marks an object page dirty with a fresh generation.
 */
void
vm_object_mark_dirty(
	struct vm_object_page *page)
{
	unsigned long irq;

	/* Ignores a missing page or one without an owner. */
	if (page == NULL || page->owner == NULL)
		return;

	/* Sets the flag under the metadata and owner locks. */
	vm_metadata_enter();
	irq = spin_lock_irqsave(&page->owner->lock);

	vm_object_page_dirty_mark(page);

	spin_unlock_irqrestore(&page->owner->lock, irq);
	vm_metadata_leave();
}

/*
 * Tests whether any page in a range has a wired mapping.
 */
int
vm_object_range_has_wired_mapping(
	struct vm_object *object,
	uint64_t start,
	uint64_t end)
{
	struct vm_object_page *page;
	struct vm_page *mapping;

	/* Looks for a wired mapping on any page of the range, from the first page that can reach it. */
	page = vm_object_page_at_or_after(object, start - start % PAGE_SIZE);
	for (;
	     page != NULL;
	     page = vm_object_page_after(object, page)) {
		/* Ends at the first page past the range. */
		if ((uint64_t)page->offset >= end)
			break;
		if (!vm_object_page_overlaps(page, start, end))
			continue;

		/* Reports the first mapping of this page that is wired. */
		for (mapping = page->mappings;
		     mapping != NULL;
		     mapping = mapping->object_next) {
			if (mapping->wire_count != 0)
				return 1;
		}
	}

	/* Reports a range nothing wires down. */
	return 0;
}

/* Copies into or out of a pinned page once it is not busy. */
static int
vm_object_page_pin_copy(
	struct vm_object_page *page,
	size_t offset,
	void *buffer,
	size_t length,
	int write,
	int nowait)
{
	struct vm_object *object;
	unsigned long irq;
	int error;
	uint64_t sequence;

	/* Rejects a call that names no page to copy through. */
	if (page == NULL)
		return EINVAL;

	/* Rejects a call that names no buffer to copy from or into. */
	if (buffer == NULL)
		return EINVAL;

	/* Rejects a page that no object owns, because it has no lock to take. */
	object = page->owner;
	if (object == NULL)
		return EINVAL;

	/* Rejects a start that lies past the end of the page. */
	if (offset > PAGE_SIZE)
		return EINVAL;

	/* Rejects a run that would reach past the end of the page. */
	if (length > PAGE_SIZE - offset)
		return EINVAL;

	/* Waits for the page to leave any writeback, still pinned. */
	irq = spin_lock_irqsave(&object->lock);

	for (;;) {
		if (page->owner != object || page->pin_count == 0) {
			spin_unlock_irqrestore(&object->lock, irq);
			return EINVAL;
		}

		if ((page->flags & (VM_OBJECT_PAGE_BUSY | VM_OBJECT_PAGE_WRITEBACK)) == 0)
			break;

		/* A caller that may not wait is told the page is taken. */
		if (nowait) {
			spin_unlock_irqrestore(&object->lock, irq);
			return EBUSY;
		}

		sequence = waitq_sequence(&object->page_waitq);
		error = waitq_sleep(&object->page_waitq, &object->lock, sequence, 0, 0);

		if (error != 0 && error != EAGAIN) {
			spin_unlock_irqrestore(&object->lock, irq);
			return error;
		}
	}

	/*
	 * Orphan writes stay dirty so abort can restore the old cache image.
	 * Commit discards the orphan without ever putting it on writeback.
	 */
	if (write) {
		kern_memcpy((uint8_t *)hal_pmem_to_kernel(page->pmem.paddr) + offset, buffer, length);
		vm_object_page_dirty_mark(page);
	} else {
		kern_memcpy(buffer, (const uint8_t *)hal_pmem_to_kernel(page->pmem.paddr) + offset, length);
	}

	spin_unlock_irqrestore(&object->lock, irq);

	return 0;
}
