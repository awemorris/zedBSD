/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Read-ahead of object pages: preparing, completing and abandoning a
 * prefetch.
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
extern int disk_cache_acquire(struct disk *, struct disk **) __attribute__((weak));
extern void disk_cache_release(struct disk *) __attribute__((weak));
extern void readahead_consumed(size_t, size_t) __attribute__((weak));

static int prefetch_read_through_file(struct vm_object_prefetch *fill, struct file_io *io);

/*
 * Retains an existing cache identity and allocates an unpublished bounded fill.
 */
int
vm_object_prefetch_prepare(
	struct inode *inode,
	off_t offset,
	size_t length,
	struct vm_object_prefetch *fill)
{
	struct vm_object *object;
	struct vm_object_page *page;
	unsigned index;
	unsigned needed;
	unsigned long irq;
	bool enabled;
	int page_io;
	int error;

	/* Rejects a request that names no file to read from. */
	if (inode == NULL)
		return EINVAL;

	/* Rejects a request that names nowhere to report the run. */
	if (fill == NULL)
		return EINVAL;

	/* Rejects a descriptor that already owns an object. */
	if (fill->object != NULL)
		return EINVAL;

	/* Rejects an offset before the start of the file. */
	if (offset < 0)
		return EINVAL;

	/* Rejects an offset that does not begin a page. */
	if ((offset & (PAGE_SIZE - 1U)) != 0)
		return EINVAL;

	/* Rejects an empty run. */
	if (length == 0)
		return EINVAL;

	/* Rejects a length that does not cover whole pages. */
	if ((length & (PAGE_SIZE - 1U)) != 0)
		return EINVAL;

	/* Rejects a run longer than one prefetch holds. */
	if (length > (size_t)VM_OBJECT_PREFETCH_PAGES * PAGE_SIZE)
		return EINVAL;

	kern_memset(fill, 0, sizeof(*fill));

	/*
	 * Leaves absent or transitioning objects to demand-driven cache
	 * construction.
	 */
	enabled = vm_object_registry_lock();
	object = vm_object_find_by_inode_locked(inode);
	if (object == NULL || (object->flags & VM_OBJECT_DETACHING) != 0) {
		vm_object_registry_unlock(enabled);
		return EAGAIN;
	}

	/* Counts this prefetch as an operation and pins the object. */
	object->active_operations++;
	if (object->active_operations == 0)
		HAL_FATAL("prefetch operation counter overflow");
	object->prefetch_operations++;
	if (object->prefetch_operations == 0)
		HAL_FATAL("private prefetch counter overflow");

	refcount_get(&object->refs);
	vm_object_registry_unlock(enabled);

	fill->object = object;
	fill->offset = offset;

	/*
	 * Pins disk admission for both the pending fill and its later
	 * publication.
	 */
	if (inode->i_mount != NULL &&
	    inode->i_mount->m_disk != NULL &&
	    disk_cache_acquire != NULL &&
	    disk_cache_release != NULL) {
		error = disk_cache_acquire(inode->i_mount->m_disk, &fill->disk);
		if (error != 0) {
			vm_object_prefetch_abort(fill);
			return error;
		}
	}

	/*
	 * Captures only a missing prefix of the currently valid file
	 * generation.
	 */
	irq = spin_lock_irqsave(&object->lock);

	/*
	 * Leaves the fill to demand paging unless the object is one a
	 * prefetch may fill.  It is not when any of the following holds:
	 *
	 * - the object carries no cache reference, so nothing keeps the
	 *   pages this fill would publish;
	 * - a content transaction, a resize or an anonymous mapping owns
	 *   the object, so its pages are not the file's to read;
	 * - the offset is at or past the end of file;
	 * - the first page is already resident, so there is no missing
	 *   prefix to read.
	 */
	if ((object->flags & VM_OBJECT_CACHE_REFERENCE) == 0 ||
	    (object->flags & (VM_OBJECT_CONTENT | VM_OBJECT_RESIZING | VM_OBJECT_ANONYMOUS)) != 0 ||
	    offset >= object->logical_size ||
	    vm_object_find_page(object, offset) != NULL) {
		spin_unlock_irqrestore(&object->lock, irq);
		vm_object_prefetch_abort(fill);
		return EAGAIN;
	}

	/*
	 * A run longer than a worker's buffer is read straight into its pages,
	 * so only a file the page cache reads itself takes one.
	 */
	page_io = vm_object_page_io_candidate(object);
	if (length > KERN_IO_BATCH_MAX && !page_io) {
		spin_unlock_irqrestore(&object->lock, irq);
		vm_object_prefetch_abort(fill);
		return EINVAL;
	}

	/* Clamps the run to the end of file and to the first resident page. */
	if ((uint64_t)length > (uint64_t)(object->logical_size - offset))
		length = (size_t)(object->logical_size - offset);

	/* Stops the run at the first page that is already resident. */
	for (index = 1; (size_t)index * PAGE_SIZE < length; index++) {
		if (vm_object_find_page(object, offset + (off_t)index * PAGE_SIZE) != NULL) {
			length = (size_t)index * PAGE_SIZE;
			break;
		}
	}

	/* Records the run against the generations it was measured in. */
	fill->length = length;
	fill->size_generation = object->size_generation;
	fill->content_generation = object->content_generation;

	spin_unlock_irqrestore(&object->lock, irq);

	/*
	 * Allocates private optional frames without publishing BUSY pages to
	 * readers.
	 */
	needed = (unsigned)((length + PAGE_SIZE - 1U) / PAGE_SIZE);
	if (needed > VM_OBJECT_PREFETCH_PAGES) {
		vm_object_prefetch_abort(fill);
		return EINVAL;
	}

	/* Allocates a private frame for every page of the run. */
	while (fill->count < needed) {
		page = vm_object_alloc_page(object,
		    offset + (off_t)fill->count * PAGE_SIZE, 1);
		if (page == NULL) {
			vm_object_prefetch_abort(fill);
			return EAGAIN;
		}

		fill->pages[fill->count++] = page;
	}

	/* Reports the prepared run. */
	return 0;
}

/*
 * Reads a prepared run straight from the disk into its private pages, for
 * a file whose data the page cache reads itself, under the file's I/O
 * lock as a read through the file would take it.  On success the pages
 * hold the run and vm_object_prefetch_complete() takes no bytes.
 * EOPNOTSUPP, with the pages untouched, leaves the run to a read of the
 * file into a buffer.
 */
int
vm_object_prefetch_read(
	struct vm_object_prefetch *fill)
{
	struct vm_object *object;
	struct file_io io;
	int page_io;
	int error;

	/* Rejects a token that holds no run. */
	if (fill == NULL || fill->object == NULL || fill->count == 0 || fill->length == 0)
		return EINVAL;

	/* Leaves a file whose data the page cache does not read to the old way. */
	object = fill->object;
	page_io = vm_object_page_io_candidate(object);
	if (!page_io)
		return EOPNOTSUPP;

	/* Reads the run inside the file's I/O lease. */
	error = file_io_begin(object->file, FILE_IO_PREAD, fill->offset, FILE_IO_VM_OBJECT, &io);
	if (error != 0)
		return error;
	error = vm_object_io_read_pages(object->inode, fill->offset, fill->pages, fill->count,
	    fill->length, &io.context, vm_object_file_read_fallback, &io);

	/*
	 * A file system that turns the mapping down (its mount reads through
	 * the buffer cache) has the run read through the file, a page at a
	 * time, into the same pages: the run can be longer than any buffer.
	 */
	if (error == EOPNOTSUPP)
		error = prefetch_read_through_file(fill, &io);
	file_io_end(&io);

	/* Reports why the run could not be read. */
	if (error != 0)
		return error;

	/* Succeeded: the pages hold the run. */
	fill->filled = 1;
	return 0;
}

/* Reads a prefetch's run through the file's own read, page by page, inside its file_io. */
static int
prefetch_read_through_file(
	struct vm_object_prefetch *fill,
	struct file_io *io)
{
	unsigned index;
	size_t wanted;
	ssize_t got;
	void *data;

	/* Reads each page's bytes, the last one up to the end of the run, and zeroes the rest. */
	for (index = 0; index < fill->count; index++) {
		wanted = fill->length - (size_t)index * PAGE_SIZE;
		if (wanted > PAGE_SIZE)
			wanted = PAGE_SIZE;
		data = hal_pmem_to_kernel(fill->pages[index]->pmem.paddr);
		kern_memset(data, 0, PAGE_SIZE);
		got = vm_object_file_read_fallback(io, fill->offset + (off_t)index * PAGE_SIZE, data, wanted);
		if (got < 0)
			return (int)-got;
		if ((size_t)got != wanted)
			return EIO;
	}

	/* Succeeded: every page holds its part of the run. */
	return 0;
}

/*
 * Publishes completed private bytes only into the captured unchanged missing range.
 */
int
vm_object_prefetch_complete(
	struct vm_object_prefetch *fill,
	const void *bytes,
	ssize_t received,
	size_t *published)
{
	struct vm_object *object;
	struct vm_object_page *page;
	unsigned index;
	unsigned long irq;
	size_t valid;
	int error;

	/* Requires a live ownership token and an output counter. */
	if (fill == NULL || fill->object == NULL || published == NULL)
		return EINVAL;

	*published = 0;

	/* Discards a short or failed read rather than publishing it. */
	if (received < 0 || (size_t)received != fill->length || (bytes == NULL && !fill->filled)) {
		error = received < 0 && received >= -INT_MAX ? (int)-received : EIO;
		vm_object_prefetch_abort(fill);
		return error;
	}

	object = fill->object;

	/* Initializes every private page before taking the publication lock, unless the read filled it. */
	for (index = 0; index < fill->count; index++) {
		page = fill->pages[index];
		valid = fill->length - (size_t)index * PAGE_SIZE;
		if (valid > PAGE_SIZE)
			valid = PAGE_SIZE;
		if (!fill->filled) {
			kern_memset(hal_pmem_to_kernel(page->pmem.paddr), 0, PAGE_SIZE);
			kern_memcpy(hal_pmem_to_kernel(page->pmem.paddr), (const uint8_t *)bytes +
			    (size_t)index * PAGE_SIZE, valid);
		}
		page->flags = 0;
		page->content_generation = fill->content_generation;
	}

	/*
	 * Rejects stale data and every demand/mapped/write publication that won
	 * the race.
	 */
	error = 0;
	irq = spin_lock_irqsave(&object->lock);

	if (object->size_generation != fill->size_generation) {
		/* The file size has changed since the run was measured. */
		error = EAGAIN;
	} else if (object->content_generation != fill->content_generation) {
		/* The contents have changed since the run was measured. */
		error = EAGAIN;
	} else if ((object->flags &
	    (VM_OBJECT_CONTENT | VM_OBJECT_RESIZING | VM_OBJECT_DETACHING)) != 0) {
		/*
		 * A write, a resize or a detach now owns the object, so these
		 * pages are no longer the file's to publish.
		 */
		error = EAGAIN;
	} else if (fill->offset > object->logical_size ||
	    (uint64_t)fill->length > (uint64_t)(object->logical_size - fill->offset)) {
		/* The run no longer lies inside the file. */
		error = EAGAIN;
	}

	/* Yields to a demand fault that already published one of these pages. */
	for (index = 0; index < fill->count && error == 0; index++) {
		if (vm_object_find_page(object, fill->pages[index]->offset) != NULL)
			error = EAGAIN;
	}

	/* Publishes every page of the run as resident and prefetched. */
	if (error == 0) {
		for (index = 0; index < fill->count; index++) {
			page = fill->pages[index];
			page->prefetch_valid = (unsigned)(fill->length - (size_t)index * PAGE_SIZE);
			if (page->prefetch_valid > PAGE_SIZE)
				page->prefetch_valid = PAGE_SIZE;

			/* Publishes the page as resident, indexed and untouched. */
			page->prefetch_size_generation = fill->size_generation;
			page->prefetch_frontier = 0;
			page->prefetch_used = 0;
			vm_object_page_index_insert(object, page);

			(void)atomic_fetch_add_relaxed(&object_pages, 1);
		}

		*published = fill->length;
		fill->count = 0;

		waitq_wake_all(&object->page_waitq);
	}

	spin_unlock_irqrestore(&object->lock, irq);

	/* Releases whatever the publication did not take. */
	vm_object_prefetch_abort(fill);

	/* Reports why the publication failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Retires private frames, disk admission and the retained cache operation.
 */
void
vm_object_prefetch_abort(
	struct vm_object_prefetch *fill)
{
	struct vm_object *object;
	bool enabled;

	/*
	 * Makes cancellation of an unused or previously consumed token
	 * idempotent.
	 */
	if (fill == NULL || fill->object == NULL)
		return;

	/* Keeps the object, because the descriptor is cleared below. */
	object = fill->object;

	/* Returns every frame the fill prepared but never published. */
	vm_object_cache_discard_prepared(fill->pages, fill->count);

	/* Releases the disk admission the fill pinned. */
	if (fill->disk != NULL)
		disk_cache_release(fill->disk);

	/* Empties the descriptor so a second cancellation does nothing. */
	kern_memset(fill, 0, sizeof(*fill));

	/* Stops qualifying this owner for clean reclaim before dropping its pin. */
	enabled = vm_object_registry_lock();
	if (object->prefetch_operations == 0)
		HAL_FATAL("private prefetch counter underflow");
	object->prefetch_operations--;
	vm_object_registry_unlock(enabled);

	/* Retires the operation this prefetch counted against the object. */
	vm_object_operation_end(object);

	/* Destroys the object when this was the last reference to it. */
	if (refcount_put(&object->refs))
		vm_object_destroy(object);
}

/*
 * Credits disjoint forward demand spans while the object lock serializes
 * readers.
 */
size_t
vm_object_prefetch_consume(
	struct vm_object_page *page,
	size_t start,
	size_t length)
{
	size_t end;
	size_t used;

	/*
	 * Stops attribution after mappings or writes can replace prefetched
	 * contents.
	 */
	if (page->prefetch_valid == 0)
		return 0;

	/* Retires the prefetch once the page is mapped, resized under, or written. */
	if (page->mapping_count != 0 ||
	    page->prefetch_size_generation != page->owner->size_generation ||
	    (page->flags & VM_OBJECT_PAGE_DIRTY) != 0) {
		vm_object_prefetch_retire(page);
		return 0;
	}

	/* Clamps the run to the bytes read but not yet consumed. */
	end = start + length;
	if (end > page->prefetch_valid)
		end = page->prefetch_valid;
	if (start < page->prefetch_frontier)
		start = page->prefetch_frontier;
	if (end <= start)
		return 0;

	/*
	 * Never credits repeated bytes; skipped earlier gaps remain
	 * conservatively unused.
	 */
	used = end - start;
	page->prefetch_frontier = (unsigned)end;
	page->prefetch_used += (unsigned)used;
	if (readahead_consumed != NULL)
		readahead_consumed(used, 0);

	return used;
}

/*
 * Retires speculative attribution before content replacement or storage
 * release.
 */
void
vm_object_prefetch_retire(
	struct vm_object_page *page)
{
	size_t unused;

	/*
	 * Private failed fills never published attribution and therefore
	 * contribute nothing.
	 */
	if (page->prefetch_valid == 0)
		return;

	unused = page->prefetch_valid - page->prefetch_used;

	page->prefetch_valid = 0;
	page->prefetch_frontier = 0;
	page->prefetch_used = 0;

	if (readahead_consumed != NULL)
		readahead_consumed(0, unused);
}
