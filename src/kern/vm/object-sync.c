/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Synchronizing dirty object pages to their inode, write-back preparation
 * and the delayed content path.
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
extern void *io_pool_borrow(size_t, size_t *) __attribute__((weak));
extern void io_pool_release(void *) __attribute__((weak));
extern int vmspace_object_page_revoke(struct vm_object_page *, uint32_t *) __attribute__((weak));
extern void io_error_record(struct io_error_state *, int) __attribute__((weak));

static int vm_object_sync_range_buffer(struct vm_object *object, off_t offset, size_t size, int flags, int detaching, int resize_owner, off_t resize_target, void *scratch, size_t capacity);
static struct vm_object_page *sync_page_at_or_after(struct vm_object *object, uint64_t offset);
static int write_dirty_pages(struct vm_object *object, uint64_t generation, off_t logical_size, int inode_io_owned, void *reserved_scratch, size_t reserved_capacity);

/*
 * Synchronizes a range of an object, as msync() does.
 */
int
vm_object_sync_range(
	struct vm_object *object,
	off_t offset,
	size_t size,
	int flags)
{
	int error;

	/* Rejects a missing object. */
	if (object == NULL)
		return EINVAL;

	/* Runs the sync as an operation once no transaction is active. */
	error = vm_object_wait_resize(object);
	if (error != 0)
		return error;
	error = vm_object_operation_begin(object);
	if (error != 0)
		return error;
	error = vm_object_sync_range_internal(object, offset, size, flags, 0, 0,
	    0);
	vm_object_operation_end(object);

	/* Reports why the sync failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Writes back an inode's object, unlinking a retained one that becomes
 * clean.
 */
int
vm_object_sync_inode(
	struct inode *inode)
{
	int error;

	/* Uses optional shared scratch for an ordinary caller. */

	/* Reports the failure. */
	error = vm_object_sync_inode_buffer(inode, NULL, 0);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Drains one inode using an optional caller-owned transfer reserve.
 */
int
vm_object_sync_inode_buffer(
	struct inode *inode,
	void *scratch,
	size_t capacity)
{
	struct vm_object *object;
	struct file *write_file;
	unsigned long irq;
	bool enabled;
	int error;
	int removed;
	uint64_t sequence;

	removed = 0;
	write_file = NULL;

retry_lookup:
	/* Holds the object as an operation, waiting out teardown and resize. */
	object = NULL;
	enabled = vm_object_registry_lock();
	for (object = object_hash[vm_object_hash_index(inode)]; object != NULL;
	     object = object->hash_next) {
		if (object->inode != inode)
			continue;
		if ((object->flags & VM_OBJECT_DETACHING) != 0) {
			sequence = waitq_sequence(&object->registry_waitq);

			object->registry_waiters++;
			if (object->registry_waiters == 0)
				HAL_FATAL("VM object registry waiter overflow");

			refcount_get(&object->refs);
			vm_object_registry_unlock(enabled);

			error = vm_object_wait_registry_transition(object, sequence);
			if (error != 0 && error != EAGAIN)
				return error;

			goto retry_lookup;
		}

		/* Waits out a resize and looks the object up again. */
		if ((object->flags & VM_OBJECT_RESIZING) != 0) {
			refcount_get(&object->refs);
			vm_object_registry_unlock(enabled);

			error = vm_object_wait_resize(object);

			if (refcount_put(&object->refs))
				vm_object_destroy(object);

			if (error != 0)
				return error;
			goto retry_lookup;
		}

		/* Counts this sync as an operation and pins the object. */
		object->active_operations++;
		if (object->active_operations == 0)
			HAL_FATAL("VM object operation counter overflow");

		refcount_get(&object->refs);
		break;
	}

	vm_object_registry_unlock(enabled);

	/* An uncached inode has nothing to write back. */
	if (object == NULL)
		return 0;

	/* Writes everything back. */
	error = vm_object_sync_range_buffer(object, 0, SIZE_MAX, MS_SYNC, 0, 0,
	    0, scratch, capacity);

	/* Retires a resolved failure independently of optional clean retention. */
	enabled = vm_object_registry_lock();
	if (error == 0) {
		irq = spin_lock_irqsave(&object->lock);
		if (object->writeback_error == 0 &&
		    !vm_object_has_dirty_pages_locked(object))
			object->flags &= ~VM_OBJECT_RETAINED_WRITEBACK;
		spin_unlock_irqrestore(&object->lock, irq);
	}

	/* Keeps clean cache ownership, or retires the old registry-only object. */
	if (object->mapping_count == 0 &&
	    object->active_operations == 1 &&
	    (object->flags & VM_OBJECT_DETACHING) == 0) {
		if (error == 0 && vm_object_cache_retainable(object)) {
			write_file = object->write_file;
			object->write_file = NULL;
		} else if (error == 0 && vm_object_can_destroy(object)) {
			removed = vm_object_unlink_locked(object);
		} else {
			vm_object_retain(object, error);
		}
	}

	vm_object_registry_unlock(enabled);

	/* Ends the operation and drops the reference this sync took. */
	if (removed && refcount_put(&object->refs))
		HAL_FATAL("VM object registry reference was last unexpectedly");
	vm_object_operation_end(object);
	if (refcount_put(&object->refs))
		vm_object_destroy(object);

	/* Releases the recovered writer without delaying the caller's last close. */
	if (write_file != NULL)
		(void)file_close(write_file);

	/* Reports why the writeback failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Establishes an independent writer before taking any file/content lease.
 */
int
vm_object_writeback_prepare(
	struct file *file,
	struct vm_object **result)
{
	struct vm_object *object;
	struct file *writer;
	struct backing_mutation_guard guard;
	struct inode *inode;
	bool enabled;
	int error;

	if (result == NULL)
		return EINVAL;

	*result = NULL;

	/* Refuses a handle that cannot own write-back of a regular file. */
	if (file == NULL ||
	    file->f_inode == NULL ||
	    file->f_path.p_inode == NULL ||
	    file->f_inode->i_type != INODE_REG ||
	    file->f_backing_claim != NULL ||
	    file->f_format_claim != NULL ||
	    (file_status_flags_get(file) & O_ACCMODE) == O_RDONLY)
		return EOPNOTSUPP;

	inode = file_vm_inode(file);
	if (inode != file->f_inode ||
	    (inode->i_flags & (INODE_LOOPFILE | INODE_SWAPFILE)) != 0)
		return EOPNOTSUPP;

	error = backing_mutation_begin_inode(inode, &guard);
	if (error != 0)
		return error;

	error = vm_object_get_shared_internal(file, &object, 1);
	backing_mutation_end(&guard);
	if (error != 0)
		return error;

	writer = NULL;

	enabled = vm_object_registry_lock();
	if (object->write_file != NULL) {
		vm_object_registry_unlock(enabled);
		*result = object;
		return 0;
	}

	vm_object_registry_unlock(enabled);

	/*
	 * The caller already owns write access; the clone never reaches
	 * userland.
	 */
	error = file_open_resolved(&file->f_path, O_RDWR | O_NOFOLLOW, &writer);
	if (error == 0 && file_vm_inode(writer) != inode)
		error = EAGAIN;
	if (error == 0) {
		enabled = vm_object_registry_lock();
		if (object->write_file == NULL) {
			object->write_file = writer;
			writer = NULL;
		}

		vm_object_registry_unlock(enabled);
	}

	if (writer != NULL)
		(void)file_close(writer);

	if (error != 0) {
		vm_object_writeback_release(object);
		return error;
	}

	*result = object;

	return 0;
}

/*
 * Releases the object a write-back preparation returned.
 *
 * Ends the operation the preparation began and drops its reference; a null
 * object, from a file without one, is ignored.
 */
void
vm_object_writeback_release(
	struct vm_object *object)
{
	if (object == NULL)
		return;

	vm_object_operation_end(object);

	if (refcount_put(&object->refs))
		vm_object_destroy(object);
}

/*
 * Takes the existing content transaction without flushing its old dirty image.
 * New pages remain private until all old bytes and mapping revocations succeed.
 * The caller drops inode I/O locks while this preparation performs backend reads.
 */
int
vm_object_content_prepare_delayed(
	struct vm_object_content *content,
	struct vm_object *object,
	struct writeback_ticket *ticket)
{
	struct vm_object_page *pages[16];
	unsigned fresh[16];
	uint64_t base;
	uint64_t end;
	uint64_t current;
	unsigned count;
	unsigned index;
	unsigned long irq;
	uint32_t observed;
	size_t wanted;
	ssize_t received;
	int error;

	/* Refuses a transaction that is not an unprepared, writable, non-empty one for this object. */
	if (content == NULL ||
	    !content->active ||
	    content->prepared ||
	    object == NULL ||
	    object->inode != content->inode ||
	    ticket == NULL ||
	    ticket->budget == NULL ||
	    object->write_file == NULL ||
	    content->length == 0)
		return EINVAL;

	if (content->offset < 0 ||
	    content->offset > object->logical_size ||
	    content->length > (uint64_t)(object->logical_size - content->offset))
		return EAGAIN;

	base = (uint64_t)content->offset & ~(uint64_t)(PAGE_SIZE - 1U);
	end = (uint64_t)content->offset + content->length;

	count = (unsigned)((end - base + PAGE_SIZE - 1U) / PAGE_SIZE);
	if (count > 16 || ticket->reserved < (uint64_t)count * PAGE_SIZE)
		return EAGAIN;

	kern_memset(pages, 0, sizeof(pages));
	kern_memset(fresh, 0, sizeof(fresh));
	error = 0;

	/*
	 * Existing readers/pins remain authoritative; no partial taking on
	 * refusal.
	 */
	irq = spin_lock_irqsave(&object->lock);

	if ((object->flags & VM_OBJECT_CONTENT) == 0 ||
	    object->content_generation != content->generation)
		HAL_FATAL("delayed write lost content gate");

	for (index = 0; index < count; index++) {
		pages[index] = vm_object_find_page(object, (off_t)(base + index * PAGE_SIZE));

		/*
		 * Refuses the whole transaction when a page this write would
		 * take is not free to take.  That is the case when the page
		 * exists and any of the following holds:
		 *
		 * - it is busy, so another transaction owns it;
		 * - it is under write-back, so its contents are being written
		 *   out and must not change underneath;
		 * - it carries an error, so its contents are not trustworthy;
		 * - it is orphaned, so it no longer belongs to this object;
		 * - its hold count and its pin count disagree, which means a
		 *   holder other than a pin is still looking at it.
		 */
		if (pages[index] != NULL &&
		    ((pages[index]->flags &
		      (VM_OBJECT_PAGE_BUSY |
		       VM_OBJECT_PAGE_WRITEBACK |
		       VM_OBJECT_PAGE_ERROR |
		       VM_OBJECT_PAGE_ORPHANED)) != 0 ||
		     pages[index]->hold_count != pages[index]->pin_count)) {
			spin_unlock_irqrestore(&object->lock, irq);
			return EAGAIN;
		}
	}

	/* Takes every page that already exists, under this transaction's generation. */
	for (index = 0; index < count; index++) {
		if (pages[index] != NULL) {
			pages[index]->flags |= VM_OBJECT_PAGE_BUSY | VM_OBJECT_PAGE_WRITEBACK;
			pages[index]->content_generation = content->generation;
		}
	}

	spin_unlock_irqrestore(&object->lock, irq);

	for (index = 0; index < count; index++) {
		/* Allocates a page for a hole this transaction covers. */
		current = base + (uint64_t)index * PAGE_SIZE;
		if (pages[index] == NULL) {
			pages[index] = vm_object_alloc_page(object, (off_t)current, 1);
			if (pages[index] == NULL) {
				error = EAGAIN;
				goto failed;
			}

			fresh[index] = 1;

			kern_memset(hal_pmem_to_kernel(pages[index]->pmem.paddr), 0, PAGE_SIZE);

			wanted = (size_t)(object->logical_size - (off_t)current);
			if (wanted > PAGE_SIZE)
				wanted = PAGE_SIZE;

			/*
			 * Preserve old bytes even for a full overwrite: commit
			 * may still abort.
			 */
			received = file_pread_internal(object->file,
						       hal_pmem_to_kernel(pages[index]->pmem.paddr),
						       wanted,
						       (off_t)current,
						       FILE_IO_VM_OBJECT);
			if (received != (ssize_t)wanted) {
				error = received < 0 ? (int)-received : EIO;
				goto failed;
			}

			pages[index]->content_generation = content->generation;

			continue;
		}

		observed = 0;

		/* Revokes every mapping of a taken page before it may change. */
		if (pages[index]->mapping_count != 0) {
			if (vmspace_object_page_revoke == NULL)
				error = EOPNOTSUPP;
			else
				error = vmspace_object_page_revoke(pages[index], &observed);
		}

		irq = spin_lock_irqsave(&object->lock);

		if ((observed & HAL_SPACE_PAGE_DIRTY) != 0)
			vm_object_page_dirty_mark(pages[index]);

		spin_unlock_irqrestore(&object->lock, irq);

		if (error != 0)
			goto failed;
	}

	/* Recheck all private identities before publishing any of them. */
	irq = spin_lock_irqsave(&object->lock);

	/* Refuses the transaction when a fault published one of these pages first. */
	for (index = 0; index < count; index++) {
		if (fresh[index]) {
			if (vm_object_find_page(object, pages[index]->offset) != NULL) {
				spin_unlock_irqrestore(&object->lock, irq);
				error = EAGAIN;
				goto failed;
			}
		}
	}

	/* Publishes the pages this transaction allocated. */
	for (index = 0; index < count; index++) {
		if (fresh[index]) {
			pages[index]->next = object->pages;
			object->pages = pages[index];
			vm_object_page_index_insert(object, pages[index]);
			(void)atomic_fetch_add_relaxed(&object_pages, 1);
		}
	}

	content->prepared = 1;
	content->writeback_ticket = ticket;

	spin_unlock_irqrestore(&object->lock, irq);

	return 0;

failed:
	irq = spin_lock_irqsave(&object->lock);

	vm_object_content_release_pages_locked(object, content->generation);

	spin_unlock_irqrestore(&object->lock, irq);

	for (index = 0; index < count; index++) {
		if (fresh[index])
			vm_object_release_page_storage(pages[index]);
	}

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Drains mount objects published before this call without allocating a snapshot.
 * New publications have larger registry identities and cannot extend this pass.
 */
int
vm_object_sync_mount_buffer(
	struct mount *mount,
	void *scratch,
	size_t capacity)
{
	struct vm_object *object;
	struct inode *inode;
	uint64_t cutoff;
	uint64_t cursor;
	uint64_t next;
	bool enabled;
	int error;
	int first_error;

	/* Captures a finite publication frontier before any sleeping I/O. */
	enabled = vm_object_registry_lock();
	cutoff = object_registry_generation;
	vm_object_registry_unlock(enabled);
	cursor = 0;
	first_error = 0;

	/*
	 * Pins only the next inode, allowing each completed object to retire
	 * normally.
	 */
	for (;;) {
		inode = NULL;
		next = UINT64_MAX;
		enabled = vm_object_registry_lock();
		for (object = shared_objects; object != NULL; object = object->next) {
			/* Skips what this pass has done, or published too late. */
			if (object->registry_generation <= cursor ||
			    object->registry_generation > cutoff ||
			    object->registry_generation > next || object->inode == NULL)
				continue;

			/* Skips an object outside the mount the caller named. */
			if (mount != NULL && object->inode->i_mount != mount)
				continue;
			if (mount == NULL && vm_object_mount_reserved(object))
				continue;
			next = object->registry_generation;
			inode = object->inode;
		}

		/* Pins the chosen inode, and stops when none is left. */
		if (inode != NULL)
			inode_ref(inode);
		vm_object_registry_unlock(enabled);
		if (inode == NULL)
			break;

		/*
		 * Retains the first failure while independent owners still get
		 * a drain attempt.
		 */
		cursor = next;
		error = vm_object_sync_inode_buffer(inode, scratch, capacity);
		inode_release(inode);
		if (error != 0 && first_error == 0)
			first_error = error;
	}

	/*
	 * Reports whether every captured owner reached its durability boundary.
	 */
	return first_error;
}

/*
 * Remembers the first writeback error; the caller holds the object lock.
 */
void
vm_object_record_writeback_error_locked(
	struct vm_object *object,
	int error)
{
	if (error == 0)
		return;

	if (object->writeback_error == 0)
		object->writeback_error = error;

	/* Keeps failed dirty ownership observable after another descriptor retries it. */
	if (object->inode != NULL && io_error_record != NULL) {
		/* Records the failure against the file itself. */
		io_error_record(&object->inode->i_write_error, error);

		/* And against the file system it belongs to. */
		if (object->inode->i_mount != NULL)
			io_error_record(&object->inode->i_mount->m_write_error, error);
	}
}

/*
 * Writes a page's data to the backend through the writeback file.
 */
int
vm_object_write_page_data(
	struct vm_object *object,
	struct vm_object_page *page,
	off_t logical_size,
	int inode_io_owned)
{
	struct file_io io;
	size_t length;
	ssize_t count;
	unsigned io_flags;
	int error;

	/* Only a writable object can write back. */
	if (object->write_file == NULL)
		return EACCES;
	if (page->offset < 0)
		return EIO;

	/* Truncate already discarded full pages beyond the new EOF. */
	if (page->offset >= logical_size)
		return 0;

	length = (size_t)(logical_size - page->offset);
	if (length > PAGE_SIZE)
		length = PAGE_SIZE;

	/* Writes as a content change, inside the caller's inode I/O when owned. */
	io_flags = FILE_IO_VM_OBJECT | FILE_IO_CONTENT_CHANGE | FILE_IO_DRAIN;
	if (inode_io_owned)
		io_flags |= FILE_IO_INODE_IO_OWNED;

	error = file_io_begin(object->write_file, FILE_IO_PWRITE, page->offset, io_flags, &io);
	if (error != 0)
		return error;

	io.context.content_generation = page->write_dirty_generation;
	count = file_io_transfer(&io, (void *)hal_pmem_to_kernel(page->pmem.paddr), length);
	file_io_end(&io);

	if (count == (ssize_t)length)
		return 0;

	if (count < 0)
		return (int)-count;

	return EIO;
}

/*
 * Revokes, writes back, and optionally invalidates the pages of a range.
 */
int
vm_object_sync_range_internal(
	struct vm_object *object,
	off_t offset,
	size_t size,
	int flags,
	int detaching,
	int resize_owner,
	off_t resize_target)
{
	int error;

	/* Preserves the ordinary entry point without reserving a second payload. */
	error = vm_object_sync_range_buffer(object, offset, size, flags, detaching,
					    resize_owner, resize_target, NULL, 0);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Indexes resident dirty data, excluding resize-orphaned storage.
 */
void
vm_object_page_dirty_link(
	struct vm_object_page *page)
{
	/*
	 * Leaves clean, already indexed and orphaned descriptors out of
	 * publication.
	 */
	if (page->dirty_linked ||
	    (page->flags & VM_OBJECT_PAGE_DIRTY) == 0 ||
	    (page->flags & VM_OBJECT_PAGE_ORPHANED) != 0)
		return;

	/*
	 * Adds membership without scanning or sorting other resident pages.
	 *
	 * The page goes to the head of the owner's dirty list: its previous
	 * link is cleared, its next link takes what the head held, the old
	 * head points back at it when there was one, and the owner's head
	 * becomes this page.  The membership flag then records that the page
	 * is on the list, so a second mark does nothing.
	 */
	page->dirty_previous = NULL;
	page->dirty_next = page->owner->dirty_pages;
	if (page->dirty_next != NULL)
		page->dirty_next->dirty_previous = page;
	page->owner->dirty_pages = page;
	page->dirty_linked = 1;
}

/*
 * Removes writeback membership before a clean transition, orphan or retirement.
 */
void
vm_object_page_dirty_unlink(
	struct vm_object_page *page)
{
	/*
	 * Accepts never-dirty and already detached pages without touching their
	 * owner.
	 */
	if (!page->dirty_linked)
		return;

	/*
	 * Repairs the owner list before the descriptor may leave its resident
	 * index.
	 */
	if (page->dirty_previous != NULL)
		page->dirty_previous->dirty_next = page->dirty_next;
	else
		page->owner->dirty_pages = page->dirty_next;

	/* Closes the list from the other side as well. */
	if (page->dirty_next != NULL)
		page->dirty_next->dirty_previous = page->dirty_previous;

	/* Leaves the page off the list and records that it is off it. */
	page->dirty_previous = NULL;
	page->dirty_next = NULL;
	page->dirty_linked = 0;
}

/*
 * Advances dirty content and publishes eligible membership as one owner
 * transition.
 */
void
vm_object_page_dirty_mark(
	struct vm_object_page *page)
{
	/* A page that has been written is no longer a prefetch of the file. */
	vm_object_prefetch_retire(page);

	/* Records the new contents as not yet written back. */
	page->flags |= VM_OBJECT_PAGE_DIRTY;

	/* Stamps the change, so a write-back knows what it cleaned. */
	page->dirty_generation = vm_object_next_generation(page->owner);

	/* Publishes the page to the owner's dirty list. */
	vm_object_page_dirty_link(page);
}

/* Drains the captured range while using the supplied worker reserve when present. */
static int
vm_object_sync_range_buffer(
	struct vm_object *object,
	off_t offset,
	size_t size,
	int flags,
	int detaching,
	int resize_owner,
	off_t resize_target,
	void *scratch,
	size_t capacity)
{
	struct vm_object_page *page;
	struct vm_object_page **link;
	struct vm_object_page *retired;
	off_t write_limit;
	uint64_t start;
	uint64_t end;
	uint64_t position;
	uint64_t scan;
	uint64_t sync_generation;
	uint64_t sequence;
	unsigned long irq;
	int first_error;
	int selected_write;
	int retry_writeback;
	int held_inode_io;
	int transaction_active;
	int error;
	struct vm_object_page *candidate;
	int has_mappings;
	int wait_for_page;
	int dirty;
	uint32_t observed;

	retired = NULL;
	first_error = 0;
	selected_write = 0;
	held_inode_io = 0;

	/* Rejects a call that names no object to sync. */
	if (object == NULL)
		return EINVAL;

	/* Rejects an offset before the start of the object. */
	if (offset < 0)
		return EINVAL;

	/* Rejects an empty range. */
	if (size == 0)
		return EINVAL;

	/* Rejects a flag this call does not define. */
	if ((flags & ~(MS_ASYNC | MS_INVALIDATE | MS_SYNC)) != 0)
		return EINVAL;

	/* Rejects a call that says neither whether to wait nor not to. */
	if ((flags & (MS_ASYNC | MS_SYNC)) == 0)
		return EINVAL;

	/* Rejects a call that asks both to wait and not to. */
	if ((flags & (MS_ASYNC | MS_SYNC)) == (MS_ASYNC | MS_SYNC))
		return EINVAL;

	/* offset zero plus SIZE_MAX is the internal full-object sentinel. */
	start = (uint64_t)offset;
	if (start == 0 && size == SIZE_MAX) {
		end = UINT64_MAX;
	} else {
		end = start + size;
		if (end < start)
			end = UINT64_MAX;
	}

	(void)resize_target;

	/* An ordinary sync holds the inode's I/O lock outside any transaction. */
	if (!resize_owner && object->inode != NULL) {
		for (;;) {
			mutex_lock(&object->inode->i_io_lock);

			irq = spin_lock_irqsave(&object->lock);
			transaction_active = (object->flags & (VM_OBJECT_RESIZING | VM_OBJECT_CONTENT)) != 0;
			spin_unlock_irqrestore(&object->lock, irq);

			if (!transaction_active)
				break;

			mutex_unlock(&object->inode->i_io_lock);

			/* Waits out the transaction and takes the lock again. */
			error = vm_object_wait_resize(object);
			if (error != 0)
				return error;
		}

		held_inode_io = 1;
	}

	/*
	 * Wired mappings make MS_INVALIDATE non-atomic.  This is only a
	 * metadata query; all later PTE changes are performed by the
	 * reverse-map transaction after both metadata locks have been dropped.
	 */
	if (!detaching && !resize_owner && (flags & MS_INVALIDATE) != 0) {
		vm_metadata_enter();
		irq = spin_lock_irqsave(&object->lock);
		if (vm_object_range_has_wired_mapping(object, start, end)) {
			spin_unlock_irqrestore(&object->lock, irq);
			vm_metadata_leave();
			if (held_inode_io)
				mutex_unlock(&object->inode->i_io_lock);
			return EBUSY;
		}

		spin_unlock_irqrestore(&object->lock, irq);
		vm_metadata_leave();
	}

	/* Samples the end of file and starts a sync generation. */
	irq = spin_lock_irqsave(&object->lock);

	write_limit = object->logical_size;
	if (resize_owner && (object->flags & VM_OBJECT_RESIZING) == 0)
		HAL_FATAL("VM resize writeback without transaction ownership");

	/* A detaching object must have no mapping left anywhere. */
	if (detaching) {
		/* Traps on the first page a mapping still refers to. */
		for (page = object->pages; page != NULL; page = page->next) {
			if (page->mappings != NULL || page->mapping_count != 0)
				HAL_FATAL("detaching VM object still has reverse mappings");
		}
	}

	/* Stamps this pass, so a page is taken at most once by it. */
	sync_generation = vm_object_next_generation(object);

	spin_unlock_irqrestore(&object->lock, irq);

	/*
	 * Revoke every resident page in the range before deciding whether it
	 * is dirty.  BUSY excludes pin copies and new fault publication;
	 * draining the non-pin holds closes the pre-existing fault/publish
	 * window.
	 *
	 * The pass walks the range in offset order through the page index and
	 * resumes after the page it took last.  Searching the whole
	 * page list from its head for every page made a sync quadratic in the
	 * resident pages: a 3 GiB file kept the shutdown's sync running for
	 * hours.
	 */
	position = start - start % PAGE_SIZE;
	for (;;) {
		candidate = NULL;
		has_mappings = 0;
		wait_for_page = 0;
		dirty = 0;
		observed = 0;

		/* Looks for the next page of the range this pass has not taken. */
		irq = spin_lock_irqsave(&object->lock);
		scan = position;
		for (;;) {
			/* Finds the first resident page at or after the search point. */
			page = sync_page_at_or_after(object, scan);
			if (page == NULL)
				break;

			/* A page past the range, or one without an offset, ends the search. */
			if (page->offset < 0 || (uint64_t)page->offset >= end) {
				page = NULL;
				break;
			}

			/* Moves the search point past this page whatever is decided about it. */
			scan = (uint64_t)page->offset + PAGE_SIZE;

			/* Skips a page outside the range this call names. */
			if (!vm_object_page_overlaps(page, start, end))
				continue;

			/* Skips a page this pass has already taken. */
			if (page->write_generation == sync_generation)
				continue;

			/*
			 * Leaves a page that a read is still filling to that read
			 * when this pass holds the inode's I/O lock.  A fill is
			 * BUSY without WRITEBACK, and it takes the same lock to
			 * read the backend, so waiting for it here would wait
			 * forever (BUG-082).  Until the fill publishes the page
			 * it is clean and unmapped: there is nothing in it for
			 * this pass to revoke or write.
			 */
			if (held_inode_io &&
			    (page->flags & (VM_OBJECT_PAGE_BUSY | VM_OBJECT_PAGE_WRITEBACK)) == VM_OBJECT_PAGE_BUSY)
				continue;

			/* Waits out a page another transaction owns or is writing. */
			if ((page->flags & (VM_OBJECT_PAGE_BUSY | VM_OBJECT_PAGE_WRITEBACK)) != 0) {
				wait_for_page = 1;
				break;
			}

			/* Waits out a page a reader other than a pin still holds. */
			if (page->hold_count != page->pin_count) {
				wait_for_page = 1;
				break;
			}

			/*
			 * An invalidating sync also waits out a pinned page,
			 * because it may not drop one out from under a pin.
			 */
			if (!resize_owner && (flags & MS_INVALIDATE) != 0 && page->pin_count != 0) {
				wait_for_page = 1;
				break;
			}

			candidate = page;
			break;
		}

		/* Sleeps until a page that is still busy reports progress. */
		if (wait_for_page) {
			sequence = waitq_sequence(&object->page_waitq);
			error = waitq_sleep(&object->page_waitq, &object->lock, sequence, 0, 0);
			if (error != 0 && error != EAGAIN) {
				first_error = error;
				spin_unlock_irqrestore(&object->lock, irq);
				break;
			}

			spin_unlock_irqrestore(&object->lock, irq);
			continue;
		}

		/* Ends the pass once the range holds no untaken page. */
		if (candidate == NULL) {
			spin_unlock_irqrestore(&object->lock, irq);
			break;
		}

		/* Takes the page out of service for this sync generation. */
		candidate->flags |= VM_OBJECT_PAGE_BUSY | VM_OBJECT_PAGE_WRITEBACK;
		candidate->write_generation = sync_generation;
		has_mappings = candidate->mapping_count != 0;

		/* The next search starts after the page just taken. */
		position = (uint64_t)candidate->offset + PAGE_SIZE;

		spin_unlock_irqrestore(&object->lock, irq);

		/* Revokes the mappings and writes the page back when dirty. */
		if (has_mappings) {
			if (vmspace_object_page_revoke == NULL)
				first_error = EOPNOTSUPP;
			else
				first_error = vmspace_object_page_revoke(candidate,
				    &observed);
		}

		/* Folds the revoked hardware state into the page's dirty record. */
		irq = spin_lock_irqsave(&object->lock);

		if ((observed & HAL_SPACE_PAGE_DIRTY) != 0) {
			vm_object_page_dirty_mark(candidate);
		}

		dirty = (candidate->flags & VM_OBJECT_PAGE_DIRTY) != 0;
		candidate->write_dirty_generation = candidate->dirty_generation;

		spin_unlock_irqrestore(&object->lock, irq);

		/* Notes that a file-backed dirty page has to be written. */
		if (first_error == 0 && dirty &&
		    (object->flags & VM_OBJECT_ANONYMOUS) == 0) {
			selected_write = 1;
		}

		/* Preserves the first failure before a later revoke can overwrite it. */
		if (first_error != 0)
			break;
	}

	/* Writes immutable adjacent dirty pages as bounded backend transactions. */
	if (first_error == 0 && selected_write) {
		first_error = write_dirty_pages(object,
						sync_generation,
						write_limit,
						!resize_owner,
						scratch,
						capacity);
	}

	/* Notes an earlier failure the object is still carrying. */
	irq = spin_lock_irqsave(&object->lock);

	retry_writeback = object->writeback_error != 0;

	spin_unlock_irqrestore(&object->lock, irq);

	/*
	 * Resize writeback is immediately followed by its serialized
	 * mutation; ordinary sync/final-put waits for the backend's
	 * durability barrier.
	 */
	if (!resize_owner && (selected_write || retry_writeback) && first_error == 0) {
		if (object->write_file == NULL) {
			/*
			 * Nothing was opened for writing, so the data this
			 * pass wrote out cannot be made durable.
			 */
			error = EACCES;
		} else if (object->write_file->f_ops != NULL && object->write_file->f_ops->fsync != NULL) {
			/* The open file offers a barrier of its own. */
			error = object->write_file->f_ops->fsync(object->write_file);
		} else if (object->inode != NULL &&
		    object->inode->i_op != NULL &&
		    object->inode->i_op->sync != NULL) {
			/* The file system offers one instead. */
			error = object->inode->i_op->sync(object->inode);
		} else {
			/* Neither offers one, so the pass is already durable. */
			error = 0;
		}

		if (error != 0)
			first_error = error;
	}

	/* Releases the pages, orphaning or retiring the ones a caller asked for. */
	irq = spin_lock_irqsave(&object->lock);

	if (first_error != 0)
		vm_object_record_writeback_error_locked(object, first_error);

	/*
	 * Walks the page list once, releasing every page this pass took and
	 * unlinking the ones that leave the list as orphans or retirements.
	 * The walk keeps the address of the link, so a page can be taken out
	 * without a second search.
	 */
	link = &object->pages;
	for (;;) {
		/* Visits only the pages this sync generation took. */
		page = *link;
		if (page == NULL)
			break;
		if (page->write_generation != sync_generation) {
			link = &page->next;
			continue;
		}

		/* Clears the dirty record of a page nothing raced. */
		if (first_error == 0 &&
		    page->write_dirty_generation != 0 &&
		    page->dirty_generation == page->write_dirty_generation)
			vm_object_clear_page_dirty_locked(page);

		/* Puts the page back into service. */
		page->write_generation = 0;
		page->write_dirty_generation = 0;
		page->flags &= ~(VM_OBJECT_PAGE_BUSY | VM_OBJECT_PAGE_WRITEBACK);

		/* A resize owner orphans a pinned page instead of releasing it. */
		if (first_error == 0 && resize_owner && page->pin_count != 0) {
			/*
			 * Traps unless the page is one an orphan may be made
			 * of: no mapping refers to it and the only holders
			 * left are its pins.
			 */
			if (page->mappings != NULL ||
			    page->mapping_count != 0 ||
			    page->hold_count != page->pin_count)
				HAL_FATAL("invalid VM resize orphan candidate");

			/* Takes the page out of the live list. */
			*link = page->next;

			/* Moves it to the orphan list, out of the indexes. */
			vm_object_page_index_remove(object, page);
			vm_object_page_dirty_unlink(page);
			page->flags |= VM_OBJECT_PAGE_ORPHANED;
			page->next = object->orphan_pages;
			object->orphan_pages = page;

			continue;
		}

		/* MS_INVALIDATE retires a page nothing references any more. */
		if (first_error == 0 && (flags & MS_INVALIDATE) != 0) {
			/*
			 * Traps unless nothing refers to the page any more:
			 * no mapping, no holder and no pin.
			 */
			if (page->mappings != NULL ||
			    page->mapping_count != 0 ||
			    page->hold_count != 0 ||
			    page->pin_count != 0)
				HAL_FATAL("invalid VM invalidate retire candidate");

			/* Takes the page out of the live list. */
			*link = page->next;

			/* Moves it to the list this call frees on the way out. */
			vm_object_page_index_remove(object, page);
			page->next = retired;
			retired = page;

			continue;
		}

		link = &page->next;
	}

	waitq_wake_all(&object->page_waitq);

	/*
	 * A clean pass over an object with nothing dirty left clears the
	 * remembered write-back error, because there is no longer any data
	 * the error could still apply to.
	 */
	if (first_error == 0 && !vm_object_has_dirty_pages_locked(object))
		object->writeback_error = 0;

	spin_unlock_irqrestore(&object->lock, irq);

	/* Frees the retired pages and releases the inode. */
	while (retired != NULL) {
		page = retired;
		retired = page->next;
		vm_object_free_page(page);
	}

	if (held_inode_io)
		mutex_unlock(&object->inode->i_io_lock);

	return first_error;
}

/* Finds the first resident page at or after one offset. */
static struct vm_object_page *
sync_page_at_or_after(
	struct vm_object *object,
	uint64_t offset)
{
	struct vm_object_page *node;
	struct vm_object_page *result;

	/* Descends the AVL index without rescanning earlier resident pages. */
	result = NULL;
	node = object->page_index;
	while (node != NULL) {
		if ((uint64_t)node->offset >= offset) {
			result = node;
			node = node->index_left;
		} else {
			node = node->index_right;
		}
	}

	/* Returns a descriptor whose caller still holds the index lock. */
	return result;
}

/* Copies already revoked BUSY pages into bounded immutable transfer runs. */
static int
write_dirty_pages(
	struct vm_object *object,
	uint64_t generation,
	off_t logical_size,
	int inode_io_owned,
	void *reserved_scratch,
	size_t reserved_capacity)
{
	struct vm_object_page *page;
	struct file_io io;
	void *scratch;
	size_t capacity;
	size_t bytes;
	size_t amount;
	uint64_t position;
	uint64_t dirty_generation;
	off_t start;
	unsigned long irq;
	unsigned io_flags;
	ssize_t count;
	int error;

	/*
	 * Borrows optional scratch without waiting on a nested drain's owner.
	 */
	capacity = 0;

	/* Uses the caller's staging, or borrows one from the pool. */
	scratch = NULL;
	if (reserved_scratch != NULL && reserved_capacity >= PAGE_SIZE) {
		scratch = reserved_scratch;
		capacity = reserved_capacity;
	} else if (io_pool_borrow != NULL && io_pool_release != NULL) {
		scratch = io_pool_borrow(KERN_IO_BATCH_MAX, &capacity);
	}

	if (capacity > KERN_IO_BATCH_MAX)
		capacity = KERN_IO_BATCH_MAX;

	position = 0;
	error = 0;
	io_flags = FILE_IO_VM_OBJECT | FILE_IO_CONTENT_CHANGE | FILE_IO_DRAIN;

	if (inode_io_owned)
		io_flags |= FILE_IO_INODE_IO_OWNED;

	/*
	 * Walks offsets once; only this captured sync generation owns the
	 * pages.
	 */
	for (;;) {
		irq = spin_lock_irqsave(&object->lock);

		page = sync_page_at_or_after(object, position);
		if (page == NULL) {
			spin_unlock_irqrestore(&object->lock, irq);
			break;
		}

		/* Skips a page this pass did not take or that now lies past the end of file. */
		position = (uint64_t)page->offset + PAGE_SIZE;
		if (page->write_generation != generation ||
		    page->write_dirty_generation == 0 || page->offset >= logical_size) {
			spin_unlock_irqrestore(&object->lock, irq);
			continue;
		}

		start = page->offset;
		dirty_generation = page->write_dirty_generation;
		spin_unlock_irqrestore(&object->lock, irq);

		/*
		 * Keeps a scalar drain available when all optional scratch is
		 * occupied.
		 */
		if (scratch == NULL || capacity < PAGE_SIZE) {
			error = vm_object_write_page_data(object, page, logical_size, inode_io_owned);
			if (error != 0)
				break;
			continue;
		}

		/*
		 * Coalesces adjacent captured pages; BUSY protects bytes
		 * outside the lock.
		 */
		bytes = 0;
		for (;;) {
			amount = (size_t)(logical_size - page->offset);
			if (amount > PAGE_SIZE)
				amount = PAGE_SIZE;

			kern_memcpy((char *)scratch + bytes, hal_pmem_to_kernel(page->pmem.paddr), amount);

			bytes += amount;

			position = (uint64_t)page->offset + PAGE_SIZE;
			if (amount != PAGE_SIZE || capacity - bytes < PAGE_SIZE)
				break;

			/*
			 * Requires exact adjacency and immutable ownership
			 * before the next copy.
			 */
			irq = spin_lock_irqsave(&object->lock);

			/* Stops the batch at the first page that does not continue it. */
			page = sync_page_at_or_after(object, position);
			if (page == NULL ||
			    (uint64_t)page->offset != position ||
			    page->write_generation != generation ||
			    page->write_dirty_generation == 0 ||
			    page->offset >= logical_size) {
				spin_unlock_irqrestore(&object->lock, irq);
				break;
			}

			if (page->write_dirty_generation > dirty_generation)
				dirty_generation = page->write_dirty_generation;

			spin_unlock_irqrestore(&object->lock, irq);
		}

		/*
		 * Submits one bounded content drain for the captured contiguous
		 * bytes.
		 */
		if (object->write_file == NULL) {
			error = EACCES;
			break;
		}

		error = file_io_begin(object->write_file, FILE_IO_PWRITE, start, io_flags, &io);
		if (error != 0)
			break;

		io.context.content_generation = dirty_generation;
		count = file_io_transfer(&io, scratch, bytes);

		file_io_end(&io);

		if (count != (ssize_t)bytes) {
			error = count < 0 ? (int)-count : EIO;
			break;
		}
	}

	/* Returns scratch before the caller performs its durability barrier. */
	if (scratch != NULL && scratch != reserved_scratch)
		io_pool_release(scratch);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}
