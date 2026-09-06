/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * VM objects.
 *
 * A VM object caches the pages of a file for shared mappings, or holds
 * anonymous shared memory.  File objects live in a registry keyed by
 * inode and stay coherent with ordinary reads and writes through two
 * inode-level transactions: a resize, which invalidates the pages a new
 * end of file discards, and a content write, which revokes and flushes
 * the pages it overlaps before the backend changes.  An object with
 * dirty pages that cannot be written back is retained until it can.
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

#include <errno.h>
#include <string.h>
#include <sys/mman.h>

extern bool hal_irq_disable(void) __attribute__((weak));
extern void hal_irq_enable(void) __attribute__((weak));
extern int vmspace_object_page_revoke(struct vm_object_page *, uint32_t *)
    __attribute__((weak));
extern void vm_object_read_checkpoint(struct inode *, size_t, size_t)
    __attribute__((weak));

#define PAGE_SIZE ZEDBSD_PAGE_SIZE
#define VM_OBJECT_DATA __attribute__((section(".vfs_bss")))
#define VM_OBJECT_FAULT_RECLAIM_RETRIES 4U
#define VM_OBJECT_FAULT_RESERVE_PAGES 4U
#define VM_OBJECT_BACKING_SNAPSHOT_MAX 192U

static struct vm_object *shared_objects VM_OBJECT_DATA;
static unsigned object_count VM_OBJECT_DATA;
static atomic_uint_t object_pages VM_OBJECT_DATA;
static atomic_uint_t object_registry_lock VM_OBJECT_DATA;

static int vm_object_get_shared_internal(struct file *file, struct vm_object **result);
static bool registry_lock(void);
static void registry_unlock(bool enabled);
static struct vm_object * find_object_by_inode_locked(struct inode *inode);
static int object_wait_registry_sequence(struct vm_object *object, uint64_t sequence);
static int object_wait_registry_transition(struct vm_object *object, uint64_t sequence);
static void object_wake_registry_waiters(struct vm_object *object);
static void object_wait_registry_waiters(struct vm_object *object);
static int object_operation_begin(struct vm_object *object);
static void object_operation_end(struct vm_object *object);
static int alloc_vm_page(struct hal_pmem *memory);
static unsigned reclaim_object_fault_reserve(void);
static struct vm_object_page * find_page(struct vm_object *object, off_t offset);
static int unlink_page_locked(struct vm_object_page **head, struct vm_object_page *page);
static uint64_t next_generation(struct vm_object *object);
static uint64_t next_inode_resize_generation_locked(struct inode *inode);
static uint64_t next_inode_content_generation_locked(struct inode *inode);
static void object_initialize_eof_locked(struct vm_object *object, struct inode *inode);
static int page_overlaps(const struct vm_object_page *page, uint64_t start, uint64_t end);
static int page_is_dirty_locked(struct vm_object_page *page);
static int object_has_dirty_pages_locked(struct vm_object *object);
static int object_has_busy_pages_locked(const struct vm_object *object);
static int object_has_busy_pages(struct vm_object *object);
static void object_record_writeback_error_locked(struct vm_object *object, int error);
static int write_page_data(struct vm_object *object, struct vm_object_page *page, off_t logical_size, int inode_io_owned);
static void clear_page_dirty_locked(struct vm_object_page *page);
static void free_object_page(struct vm_object_page *page);
static int object_can_destroy(struct vm_object *object);
static int unlink_object_locked(struct vm_object *object);
static void destroy_object(struct vm_object *object);
static void retain_object(struct vm_object *object, int error);
static int object_wait_resize(struct vm_object *object);
static void content_release_pages_locked(struct vm_object *object, uint64_t generation);
static void vm_object_content_finish(struct vm_object_content *content, const void *buffer, size_t committed, int commit);
static void vm_object_resize_finish(struct vm_object_resize *resize, off_t logical_size, int commit);
static int vm_object_page_pin_copy(struct vm_object_page *page, size_t offset, void *buffer, size_t length, int write);
static int range_has_wired_mapping(struct vm_object *object, uint64_t start, uint64_t end);
static int vm_object_sync_range_internal(struct vm_object *object, off_t offset, size_t size, int flags, int detaching, int resize_owner, off_t resize_target);

/*
 * Admits shared mappings before a competing backing claim can be published.
 */
int
vm_object_get_shared(
	struct file *file,
	struct vm_object **result)
{
	struct backing_mutation_guard guard;
	struct inode *inode;
	int error;

	/* Validates the final content inode before reserving admission. */
	if (file == NULL || result == NULL)
		return EINVAL;
	inode = file_vm_inode(file);

	/* Rejects files without an authoritative content inode. */
	if (inode == NULL)
		return EINVAL;

	/* Keeps claim preparation out until the shared object becomes visible. */
	error = backing_mutation_begin_inode(inode, &guard);
	if (error != 0)
		return error;

	/* Publishes the mapping before releasing the admission reservation. */
	error = vm_object_get_shared_internal(file, result);
	backing_mutation_end(&guard);
	return error;
}

/*
 * Rejects cached shared objects that alias a retained backing-file claim.
 *
 * Admission is already closed by the claim before this registry inspection.
 */
int
vm_object_backing_busy(
	const struct backing_claim *claim)
{
	struct inode **inodes;
	struct vm_object *object;
	unsigned count;
	unsigned index;
	int error;
	int matched;
	bool enabled;

	/* Rejects incomplete queries before taking registry references. */
	if (claim == NULL)
		return EINVAL;

	/* Bounds temporary memory independently of concurrent registry growth. */
	inodes = kern_calloc(VM_OBJECT_BACKING_SNAPSHOT_MAX, sizeof(*inodes));
	if (inodes == NULL)
		return ENOMEM;

	/* Pins each content inode while its shared object remains registered. */
	count = 0;
	error = 0;
	enabled = registry_lock();
	for (object = shared_objects; object != NULL; object = object->next) {
		/* Refuses an oversized snapshot instead of overlooking an alias. */
		if (count == VM_OBJECT_BACKING_SNAPSHOT_MAX) {
			error = EBUSY;
			break;
		}
		inodes[count] = object->inode;
		inode_ref(inodes[count]);
		count++;
	}
	registry_unlock(enabled);

	/* Resolves sleeping FAT identities after dropping the registry spinlock. */
	for (index = 0; index < count && error == 0; index++) {
		error = backing_claim_inode_matches(claim, inodes[index], &matched);

		/* Includes retained caches so stale writeback cannot cross formatting. */
		if (error == 0 && matched)
			error = EBUSY;
	}

	/* Releases every inode even when an earlier comparison failed. */
	for (index = 0; index < count; index++)
		inode_release(inodes[index]);
	kern_free(inodes);

	/* Reports either a complete absence of aliases or the refusal reason. */
	return error;
}

/*
 * Creates an anonymous shared object of a page-aligned size, charged
 * against the commit limit.
 */
int
vm_object_create_anonymous(
	size_t size,
	struct vm_object **result)
{
	struct vm_object *object;
	size_t offset_max;
	int error;

	/* The size must be a positive page multiple that an offset can hold. */
	if (sizeof(off_t) == sizeof(int64_t))
		offset_max = (size_t)INT64_MAX;
	else
		offset_max = (size_t)INT32_MAX;
	if (result == NULL ||
	    size == 0 ||
	    (size & (PAGE_SIZE - 1U)) != 0 ||
	    size > offset_max)
		return EINVAL;

	/* Charges the commitment and builds the object. */
	error = vm_commit_reserve(size);
	if (error != 0)
		return error;
	object = kern_calloc(1, sizeof(*object));
	if (object == NULL) {
		vm_commit_release(size);
		return ENOMEM;
	}
	refcount_init(&object->refs, 1);
	object->mapping_count = 1;
	spin_init(&object->lock, LOCK_RANK_VM_OBJECT, "anonymous VM object");
	waitq_init(&object->page_waitq, "anonymous VM object page");
	waitq_init(&object->registry_waitq, "anonymous VM object lifetime");
	object->generation = 1;
	object->size_generation = 1;
	object->logical_size = (off_t)size;
	object->flags = VM_OBJECT_ANONYMOUS;
	object->commit_size = size;
	*result = object;

	/* Reports the new object. */
	return 0;
}

/*
 * Takes another mapping reference to a mapped object.
 */
void
vm_object_ref(
	struct vm_object *object)
{
	bool enabled;

	/* Ignores a missing object. */
	if (object == NULL)
		return;

	/* Only a mapped object can be shared further. */
	enabled = registry_lock();
	if (object->mapping_count == 0)
		HAL_FATAL("referencing detached VM object");
	object->mapping_count++;
	refcount_get(&object->refs);
	registry_unlock(enabled);
}

/*
 * Drops a mapping reference, tearing the object down with the last.
 *
 * The final drop writes every dirty page back and then either unlinks
 * and destroys the object or retains it for a later writeback.
 */
void
vm_object_put(
	struct vm_object *object)
{
	int error;
	int removed;
	int anonymous;
	bool enabled;
	uint64_t sequence;

	removed = 0;

	/* Ignores a missing object. */
	if (object == NULL)
		return;
	anonymous = (object->flags & VM_OBJECT_ANONYMOUS) != 0;

retry_mapping:
	/*
	 * Keep the final mapping reference published while older metadata
	 * users drain.  A racing mapper may safely join this still-live
	 * object; in that case the retried put becomes an ordinary non-final
	 * reference drop.
	 */
	enabled = registry_lock();
	if (object->mapping_count == 0)
		HAL_FATAL("VM object mapping reference underflow");
	if (object->mapping_count == 1 && object->active_operations != 0) {
		sequence = waitq_sequence(&object->registry_waitq);
		registry_unlock(enabled);
		error = object_wait_registry_sequence(object, sequence);
		if (error != 0 && error != EAGAIN)
			HAL_FATAL("VM object operation drain wait failed");
		goto retry_mapping;
	}
	object->mapping_count--;
	if (object->mapping_count != 0) {
		registry_unlock(enabled);
		if (refcount_put(&object->refs))
			HAL_FATAL("mapped VM object lost registry reference");
		return;
	}
	if ((object->flags & VM_OBJECT_DETACHING) != 0)
		HAL_FATAL("VM object entered final teardown twice");

	/*
	 * Reclassify the last mapping reference as an explicit teardown hold.
	 * Registry lookup may now wait without either reviving this object
	 * during writeback or racing its destruction.
	 */
	object->flags |= VM_OBJECT_DETACHING;
	refcount_get(&object->refs);
	registry_unlock(enabled);
	if (refcount_put(&object->refs))
		HAL_FATAL("VM object teardown lost registry reference");

	/*
	 * DETACHING excludes new mappings, so final writeback does not need
	 * the cross-vm metadata lock which a waiting mapper may already hold.
	 */
	error = vm_object_sync_range_internal(object, 0, SIZE_MAX, MS_SYNC, 1, 0,
	    0);

	/* Unlinks a clean object, or retains one that still has dirty pages. */
	enabled = registry_lock();
	if (object->mapping_count != 0 ||
	    (object->flags & VM_OBJECT_DETACHING) == 0)
		HAL_FATAL("VM object teardown state changed during writeback");
	if (error == 0 && object_can_destroy(object)) {
		if (anonymous) {
			removed = 1;
		} else {
			removed = unlink_object_locked(object);
			if (!removed)
				HAL_FATAL("detaching VM object left registry unexpectedly");
		}
	} else {
		if (anonymous)
			HAL_FATAL("anonymous VM object teardown could not complete");
		retain_object(object, error);
		object->flags &= ~VM_OBJECT_DETACHING;
	}
	registry_unlock(enabled);
	object_wake_registry_waiters(object);
	if (removed)
		object_wait_registry_waiters(object);
	if (removed && !anonymous && refcount_put(&object->refs))
		HAL_FATAL("VM object registry reference was last unexpectedly");

	/* Drop the explicit teardown lifetime reference. */
	if (refcount_put(&object->refs))
		destroy_object(object);
}

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
		enabled = registry_lock();
		object = find_object_by_inode_locked(inode);
		if (object == NULL ||
		    (object->flags & VM_OBJECT_DETACHING) == 0) {
			registry_unlock(enabled);
			return 0;
		}
		sequence = waitq_sequence(&object->registry_waitq);
		object->registry_waiters++;
		if (object->registry_waiters == 0)
			HAL_FATAL("VM object registry waiter overflow");
		refcount_get(&object->refs);
		registry_unlock(enabled);
		error = object_wait_registry_transition(object, sequence);
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
	irq = spin_lock_irqsave(&inode->i_vm_lock);
	if (inode->i_vm_resize_active || inode->i_vm_content_active) {
		spin_unlock_irqrestore(&inode->i_vm_lock, irq);
		return EBUSY;
	}
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
	enabled = registry_lock();
	object = find_object_by_inode_locked(inode);
	if (object == NULL)
		result = 0;
	else if ((object->flags & VM_OBJECT_DETACHING) != 0)
		result = -EAGAIN;
	else
		result = 1;
	registry_unlock(enabled);

	/* Reports the classification. */
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
	memset(content, 0, sizeof(*content));
	if (length == 0)
		return 0;
	inode = file_vm_inode(file);
	if (inode == NULL || inode->i_type != INODE_REG)
		return EINVAL;

	/* Only one content transaction runs, and never under a read lease. */
	enabled = registry_lock();
	inode_irq = spin_lock_irqsave(&inode->i_vm_lock);
	if (inode->i_vm_content_active || inode->i_vm_content_readers != 0) {
		spin_unlock_irqrestore(&inode->i_vm_lock, inode_irq);
		registry_unlock(enabled);
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
			registry_unlock(enabled);
			return EINVAL;
		}
	} else if (inode->i_vm_resize_active) {
		spin_unlock_irqrestore(&inode->i_vm_lock, inode_irq);
		registry_unlock(enabled);
		return EBUSY;
	}
	object = find_object_by_inode_locked(inode);
	if (object != NULL && (object->flags & VM_OBJECT_DETACHING) != 0) {
		spin_unlock_irqrestore(&inode->i_vm_lock, inode_irq);
		registry_unlock(enabled);
		return EAGAIN;
	}

	/* Publishes the transaction on the inode and the object. */
	generation = next_inode_content_generation_locked(inode);
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
		if (object->write_file == NULL) {
			file_ref(file);
			object->write_file = file;
		}
	}
	spin_unlock_irqrestore(&inode->i_vm_lock, inode_irq);
	registry_unlock(enabled);
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
	enabled = registry_lock();
	object = find_object_by_inode_locked(content->inode);
	if (object != NULL) {
		irq = spin_lock_irqsave(&object->lock);
		if ((object->flags & VM_OBJECT_CONTENT) == 0 ||
		    object->content_generation != content->generation)
			HAL_FATAL("VM object lost content transaction");
		spin_unlock_irqrestore(&object->lock, irq);
		refcount_get(&object->refs);
	}
	registry_unlock(enabled);
	if (object == NULL) {
		content->prepared = 1;
		return 0;
	}

	/* Takes each overlapping page in turn once it is idle. */
	for (;;) {
		page = NULL;
		irq = spin_lock_irqsave(&object->lock);
		for (;;) {
			wait = 0;
			for (scan = object->pages; scan != NULL; scan = scan->next) {
				if (!page_overlaps(scan, start, end) ||
				    scan->content_generation == content->generation)
					continue;
				if ((scan->flags & (VM_OBJECT_PAGE_BUSY |
				    VM_OBJECT_PAGE_WRITEBACK)) != 0 ||
				    scan->hold_count != scan->pin_count) {
					wait = 1;
					break;
				}
				page = scan;
				break;
			}
			if (page != NULL || !wait)
				break;
			sequence = waitq_sequence(&object->page_waitq);
			error = waitq_sleep(&object->page_waitq,
			    &object->lock, sequence, 0, 0);
			if (error != 0 && error != EAGAIN) {
				first_error = error;
				break;
			}
		}
		if (page == NULL || first_error != 0) {
			spin_unlock_irqrestore(&object->lock, irq);
			break;
		}
		page->flags |= VM_OBJECT_PAGE_BUSY | VM_OBJECT_PAGE_WRITEBACK;
		page->content_generation = content->generation;
		has_mappings = page->mapping_count != 0;
		spin_unlock_irqrestore(&object->lock, irq);

		/* Revokes the mappings and writes the page back when dirty. */
		observed = 0;
		error = 0;
		if (has_mappings) {
			if (vmspace_object_page_revoke == NULL)
				error = EOPNOTSUPP;
			else
				error = vmspace_object_page_revoke(page,
				    &observed);
		}
		irq = spin_lock_irqsave(&object->lock);
		if ((observed & HAL_PAGE_DIRTY) != 0) {
			page->flags |= VM_OBJECT_PAGE_DIRTY;
			page->dirty_generation = next_generation(object);
		}
		dirty = (page->flags & VM_OBJECT_PAGE_DIRTY) != 0;
		page->write_dirty_generation = page->dirty_generation;
		spin_unlock_irqrestore(&object->lock, irq);
		if (error == 0 && dirty)
			error = write_page_data(object, page,
			    object->logical_size, 0);
		irq = spin_lock_irqsave(&object->lock);
		if (error == 0 && dirty &&
		    page->dirty_generation == page->write_dirty_generation) {
			clear_page_dirty_locked(page);
		} else if (error != 0) {
			object_record_writeback_error_locked(object, error);
			first_error = error;
		}
		spin_unlock_irqrestore(&object->lock, irq);
		if (first_error != 0)
			break;
	}

	/* A failure releases the pages taken so far. */
	if (first_error != 0) {
		irq = spin_lock_irqsave(&object->lock);
		content_release_pages_locked(object, content->generation);
		spin_unlock_irqrestore(&object->lock, irq);
	} else {
		content->prepared = 1;
	}
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
	struct vm_object *object;
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
	enabled = registry_lock();
	object = find_object_by_inode_locked(inode);
	if (object == NULL) {
		registry_unlock(enabled);
		return ENOENT;
	}
	if ((object->flags & VM_OBJECT_DETACHING) != 0) {
		sequence = waitq_sequence(&object->registry_waitq);
		object->registry_waiters++;
		if (object->registry_waiters == 0)
			HAL_FATAL("VM object registry waiter overflow");
		refcount_get(&object->refs);
		registry_unlock(enabled);
		error = object_wait_registry_transition(object, sequence);
		if (error == 0 || error == EAGAIN)
			goto retry_lookup;
		return error;
	}
	object->active_operations++;
	if (object->active_operations == 0)
		HAL_FATAL("VM object read operation counter overflow");
	refcount_get(&object->refs);
	registry_unlock(enabled);

	/* Copies page by page up to the end of file. */
	while (done < length) {
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
		error = vm_object_fault(object, page_offset, &page);
		if (error != 0)
			break;
		irq = spin_lock_irqsave(&object->lock);
		if (page->hold_count == 0)
			HAL_FATAL("VM object coherent read lost fault hold");
		memcpy(bytes + done, (const uint8_t *)page->pmem.vaddr + in_page,
		    chunk);
		page->hold_count--;
		waitq_wake_all(&object->page_waitq);
		spin_unlock_irqrestore(&object->lock, irq);
		done += chunk;
		if (vm_object_read_checkpoint != NULL)
			vm_object_read_checkpoint(inode, done, length);
	}
	object_operation_end(object);
	if (refcount_put(&object->refs))
		HAL_FATAL("VM object read lost registry reference");

	/* Data, a clean end, or the end of file is a success. */
	if (done != 0 || error == 0 || error == ENXIO) {
		*result = (ssize_t)done;
		return 0;
	}
	return error;
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
	memset(resize, 0, sizeof(*resize));

	/* Only one transaction runs, and never under a read lease. */
	enabled = registry_lock();
	inode_irq = spin_lock_irqsave(&inode->i_vm_lock);
	if (inode->i_vm_resize_active ||
	    inode->i_vm_content_active ||
	    inode->i_vm_content_readers != 0) {
		spin_unlock_irqrestore(&inode->i_vm_lock, inode_irq);
		registry_unlock(enabled);
		return EBUSY;
	}
	object = find_object_by_inode_locked(inode);
	if (object != NULL && (object->flags & VM_OBJECT_DETACHING) != 0) {
		spin_unlock_irqrestore(&inode->i_vm_lock, inode_irq);
		registry_unlock(enabled);
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
	resize->inode = inode;
	resize->target_size = target_size;
	if (resize->old_size == target_size) {
		spin_unlock_irqrestore(&inode->i_vm_lock, inode_irq);
		registry_unlock(enabled);
		return 0;
	}

	/* Publishes the transaction on the inode and the object. */
	generation = next_inode_resize_generation_locked(inode);
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
	registry_unlock(enabled);

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
	enabled = registry_lock();
	object = find_object_by_inode_locked(resize->inode);
	if (object != NULL) {
		if ((object->flags & VM_OBJECT_RESIZING) == 0 ||
		    object->resize_generation != resize->generation)
			HAL_FATAL("VM object lost resize transaction");
		refcount_get(&object->refs);
	}
	registry_unlock(enabled);
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

	/* Reports the preparation result. */
	return error;
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
	struct vm_object_page *page;
	struct file_io io;
	off_t fault_size;
	uint64_t fault_generation;
	uint64_t fault_content_generation;
	size_t length;
	ssize_t count;
	unsigned long irq;
	unsigned read_retries;
	int error;
	uint64_t sequence;

	read_retries = 0;

	/* Rejects a missing object or result, or an unaligned offset. */
	if (object == NULL ||
	    result == NULL ||
	    offset < 0 ||
	    (offset & (PAGE_SIZE - 1U)) != 0)
		return EINVAL;

retry:
	/* Waits for any transaction and samples the generations. */
	irq = spin_lock_irqsave(&object->lock);
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
	page = find_page(object, offset);
	if (page != NULL) {
		while ((page->flags & VM_OBJECT_PAGE_BUSY) != 0) {
			sequence = waitq_sequence(&object->page_waitq);
			error = waitq_sleep(&object->page_waitq, &object->lock,
			    sequence, 0, 0);
			if (error != 0 && error != EAGAIN) {
				spin_unlock_irqrestore(&object->lock, irq);
				return error;
			}
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
			page->flags |= VM_OBJECT_PAGE_BUSY;
			page->error = 0;
			spin_unlock_irqrestore(&object->lock, irq);
			goto read_page;
		}
		page->hold_count++;
		*result = page;
		spin_unlock_irqrestore(&object->lock, irq);
		return 0;
	}
	spin_unlock_irqrestore(&object->lock, irq);
	if (offset >= fault_size)
		return ENXIO;

	/* Allocates a new busy page, reclaiming once when memory is short. */
	page = kern_calloc(1, sizeof(*page));
	if (page == NULL)
		return ENOMEM;
	page->offset = offset;
	page->flags = VM_OBJECT_PAGE_BUSY;
	page->owner = object;
	if (alloc_vm_page(&page->pmem) != HAL_OK) {
		if (vm_reclaim_one(NULL) != 0 ||
		    alloc_vm_page(&page->pmem) != HAL_OK) {
			kern_free(page);
			return ENOMEM;
		}
	}

	/* Publishes it unless the world changed or another fault won. */
	irq = spin_lock_irqsave(&object->lock);
	if ((object->flags & (VM_OBJECT_RESIZING | VM_OBJECT_CONTENT)) != 0 ||
	    object->size_generation != fault_generation ||
	    object->content_generation != fault_content_generation ||
	    find_page(object, offset) != NULL) {
		spin_unlock_irqrestore(&object->lock, irq);
		(void)hal_pmem_free(&page->pmem);
		kern_free(page);
		goto retry;
	}
	page->next = object->pages;
	object->pages = page;
	(void)atomic_fetch_add_relaxed(&object_pages, 1);
	spin_unlock_irqrestore(&object->lock, irq);

read_page:
	/* Reads the page's data, zero-filling past the end of file. */
	memset((void *)page->pmem.vaddr, 0, PAGE_SIZE);
	length = (size_t)(fault_size - offset);
	if (length > PAGE_SIZE)
		length = PAGE_SIZE;

read_io_retry:
	if ((object->flags & VM_OBJECT_ANONYMOUS) != 0) {
		count = (ssize_t)length;
	} else {
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
			if (error == 0)
				count = file_io_transfer(&io,
				    (void *)page->pmem.vaddr, length);
			else
				count = -error;
			file_io_end(&io);
		}
	}

	/* A read that ran out of memory retries after reclaiming a reserve. */
	if (count == -(ssize_t)ENOMEM &&
	    read_retries < VM_OBJECT_FAULT_RECLAIM_RETRIES &&
	    reclaim_object_fault_reserve() != 0) {
		read_retries++;
		goto read_io_retry;
	}

	/* Publishes the result, marking the page in error on failure. */
	irq = spin_lock_irqsave(&object->lock);
	if ((object->flags & (VM_OBJECT_RESIZING | VM_OBJECT_CONTENT)) != 0 ||
	    object->size_generation != fault_generation ||
	    object->content_generation != fault_content_generation) {
		page->flags = VM_OBJECT_PAGE_ERROR;
		page->error = EAGAIN;
		waitq_wake_all(&object->page_waitq);
		spin_unlock_irqrestore(&object->lock, irq);
		return EAGAIN;
	}
	if (count != (ssize_t)length) {
		if (count < 0)
			error = (int)-count;
		else
			error = EIO;
		page->flags = VM_OBJECT_PAGE_ERROR;
		page->error = error;
		waitq_wake_all(&object->page_waitq);
		spin_unlock_irqrestore(&object->lock, irq);
		return error;
	}
	page->flags = 0;
	page->error = 0;
	page->hold_count = 1;
	waitq_wake_all(&object->page_waitq);
	*result = page;
	spin_unlock_irqrestore(&object->lock, irq);

	/* Reports the held page. */
	return 0;
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
	enabled = registry_lock();
	if ((object->flags & VM_OBJECT_DETACHING) != 0) {
		registry_unlock(enabled);
		return EBUSY;
	}
	irq = spin_lock_irqsave(&object->lock);
	if (page->owner != object ||
	    page->hold_count == UINT_MAX ||
	    page->pin_count == UINT_MAX) {
		spin_unlock_irqrestore(&object->lock, irq);
		registry_unlock(enabled);
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
	registry_unlock(enabled);

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

	error = vm_object_page_pin_copy(page, offset, buffer, length, 0);

	/* Reports the copy result. */
	return error;
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

	error = vm_object_page_pin_copy(page, offset, (void *)buffer, length, 1);

	/* Reports the copy result. */
	return error;
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
	if ((page->flags & VM_OBJECT_PAGE_ORPHANED) != 0 &&
	    (object->flags & VM_OBJECT_RESIZING) == 0 &&
	    page->hold_count == 0) {
		if (page->pin_count != 0 ||
		    page->mapping_count != 0 ||
		    !unlink_page_locked(&object->orphan_pages, page))
			HAL_FATAL("lost VM object resize orphan");
		page->flags &= ~VM_OBJECT_PAGE_ORPHANED;
		free_page = page;
	}
	waitq_wake_all(&object->page_waitq);
	spin_unlock_irqrestore(&object->lock, irq);
	if (free_page != NULL)
		free_object_page(free_page);
	object_operation_end(object);

	/*
	 * A concurrent final put may have unlinked the object after the
	 * operation wake.  The pin reference makes this the unique
	 * final-destroy point.
	 */
	if (refcount_put(&object->refs))
		destroy_object(object);
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
	if (object_page->hold_count == 0)
		HAL_FATAL("mapping VM object page without fault hold");
	mapping->object_next = object_page->mappings;
	object_page->mappings = mapping;
	object_page->mapping_count++;
	object_page->hold_count--;
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

	for (link = &object_page->mappings; *link != NULL;
	     link = &(*link)->object_next) {
		if (*link == mapping) {
			*link = mapping->object_next;
			mapping->object_next = NULL;
			if (object_page->mapping_count == 0)
				HAL_FATAL("VM object mapping counter underflow");
			object_page->mapping_count--;
			return;
		}
	}
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
	page->flags |= VM_OBJECT_PAGE_DIRTY;
	page->dirty_generation = next_generation(page->owner);
	spin_unlock_irqrestore(&page->owner->lock, irq);
	vm_metadata_leave();
}

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
	error = object_wait_resize(object);
	if (error != 0)
		return error;
	error = object_operation_begin(object);
	if (error != 0)
		return error;
	error = vm_object_sync_range_internal(object, offset, size, flags, 0, 0,
	    0);
	object_operation_end(object);

	/* Reports the sync result. */
	return error;
}

/*
 * Writes back an inode's object, unlinking a retained one that becomes
 * clean.
 */
int
vm_object_sync_inode(
	struct inode *inode)
{
	struct vm_object *object;
	bool enabled;
	int error;
	int removed;
	uint64_t sequence;

	removed = 0;

retry_lookup:
	/* Holds the object as an operation, waiting out teardown and resize. */
	object = NULL;
	enabled = registry_lock();
	for (object = shared_objects; object != NULL; object = object->next) {
		if (object->inode != inode)
			continue;
		if ((object->flags & VM_OBJECT_DETACHING) != 0) {
			sequence = waitq_sequence(&object->registry_waitq);
			object->registry_waiters++;
			if (object->registry_waiters == 0)
				HAL_FATAL("VM object registry waiter overflow");
			refcount_get(&object->refs);
			registry_unlock(enabled);
			error = object_wait_registry_transition(object, sequence);
			if (error != 0 && error != EAGAIN)
				return error;
			goto retry_lookup;
		}
		if ((object->flags & VM_OBJECT_RESIZING) != 0) {
			refcount_get(&object->refs);
			registry_unlock(enabled);
			error = object_wait_resize(object);
			if (refcount_put(&object->refs))
				destroy_object(object);
			if (error != 0)
				return error;
			goto retry_lookup;
		}
		object->active_operations++;
		if (object->active_operations == 0)
			HAL_FATAL("VM object operation counter overflow");
		refcount_get(&object->refs);
		break;
	}
	registry_unlock(enabled);
	if (object == NULL)
		return 0;

	/* Writes everything back. */
	error = vm_object_sync_range_internal(object, 0, SIZE_MAX, MS_SYNC, 0, 0,
	    0);

	/* An unmapped object that is now clean leaves the registry. */
	enabled = registry_lock();
	if (object->mapping_count == 0 &&
	    object->active_operations == 1 &&
	    (object->flags & VM_OBJECT_DETACHING) == 0) {
		if (error == 0 && object_can_destroy(object))
			removed = unlink_object_locked(object);
		else
			retain_object(object, error);
	}
	registry_unlock(enabled);
	if (removed && refcount_put(&object->refs))
		HAL_FATAL("VM object registry reference was last unexpectedly");
	object_operation_end(object);
	if (refcount_put(&object->refs))
		destroy_object(object);

	/* Reports the writeback result. */
	return error;
}

/*
 * Applies a truncation to an inode's object after the inode size changed.
 */
void
vm_object_truncate_inode(
	struct inode *inode,
	off_t size)
{
	struct vm_object *object;
	bool enabled;
	int error;
	uint64_t sequence;
	unsigned long irq;

	/* Ignores a missing inode or a negative size. */
	if (inode == NULL || size < 0)
		return;

retry_lookup:
	/* Holds the object as an operation, waiting out a teardown. */
	enabled = registry_lock();
	object = find_object_by_inode_locked(inode);
	if (object != NULL && (object->flags & VM_OBJECT_DETACHING) != 0) {
		sequence = waitq_sequence(&object->registry_waitq);
		object->registry_waiters++;
		if (object->registry_waiters == 0)
			HAL_FATAL("VM object registry waiter overflow");
		refcount_get(&object->refs);
		registry_unlock(enabled);
		error = object_wait_registry_transition(object, sequence);
		if (error == 0 || error == EAGAIN)
			goto retry_lookup;
		return;
	}
	if (object != NULL) {
		object->active_operations++;
		if (object->active_operations == 0)
			HAL_FATAL("VM object operation counter overflow");
		refcount_get(&object->refs);
	}
	registry_unlock(enabled);
	if (object == NULL)
		return;

	/*
	 * Compatibility callers historically update inode->i_size first.  Do
	 * not publish RESIZING here: an already-running page fill must
	 * complete, after which the normal revoke transaction removes its
	 * PTE/cache identity.
	 */
	error = vm_object_sync_range_internal(object,
	    size & ~(off_t)(PAGE_SIZE - 1U), SIZE_MAX,
	    MS_SYNC | MS_INVALIDATE, 0, 0, 0);
	if (error == 0) {
		irq = spin_lock_irqsave(&object->lock);
		object->logical_size = size;
		object->size_generation = next_generation(object);
		spin_unlock_irqrestore(&object->lock, irq);
	}
	object_operation_end(object);
	if (refcount_put(&object->refs))
		HAL_FATAL("published VM object lost registry reference");
}

/*
 * Frees one idle, unwired object page, writing it back first.
 */
int
vm_object_reclaim_one(
	void)
{
	struct vm_object *object;
	off_t candidate_offset;
	int found;
	bool enabled;
	struct vm_object_page *page;
	unsigned long irq;
	struct vm_page *mapping;
	int wired;
	int error;
	int removed;

	candidate_offset = 0;
	found = 0;

	/* Selects the first idle, unwired page of a quiet object. */
	vm_metadata_enter();
	enabled = registry_lock();
	for (object = shared_objects; object != NULL; object = object->next) {
		if ((object->flags & (VM_OBJECT_DETACHING |
		    VM_OBJECT_RESIZING)) != 0)
			continue;
		irq = spin_lock_irqsave(&object->lock);
		for (page = object->pages; page != NULL; page = page->next) {
			wired = 0;
			if ((page->flags & (VM_OBJECT_PAGE_BUSY |
			    VM_OBJECT_PAGE_WRITEBACK)) != 0 ||
			    page->hold_count != 0)
				continue;
			for (mapping = page->mappings; mapping != NULL;
			     mapping = mapping->object_next) {
				if (mapping->wire_count != 0) {
					wired = 1;
					break;
				}
			}
			if (wired)
				continue;
			candidate_offset = page->offset;
			found = 1;
			object->active_operations++;
			if (object->active_operations == 0)
				HAL_FATAL("VM object operation counter overflow");
			refcount_get(&object->refs);
			break;
		}
		spin_unlock_irqrestore(&object->lock, irq);
		if (found)
			break;
	}
	registry_unlock(enabled);
	vm_metadata_leave();
	if (!found)
		return ENOMEM;

	/* Writes the page back and invalidates it. */
	error = vm_object_sync_range_internal(object, candidate_offset,
	    PAGE_SIZE, MS_SYNC | MS_INVALIDATE, 0, 0, 0);
	removed = 0;
	if (error == 0) {
		enabled = registry_lock();
		if (object->mapping_count == 0 &&
		    object->active_operations == 1 &&
		    (object->flags & VM_OBJECT_DETACHING) == 0 &&
		    object_can_destroy(object))
			removed = unlink_object_locked(object);
		registry_unlock(enabled);
	}
	if (removed && refcount_put(&object->refs))
		HAL_FATAL("VM object registry reference was last unexpectedly");
	object_operation_end(object);
	if (refcount_put(&object->refs))
		destroy_object(object);

	/* Reclaim reports whether a page was freed; writeback retains error. */
	if (error != 0)
		return ENOMEM;
	return 0;
}

/*
 * Counts the published objects.
 */
unsigned
vm_object_count(
	void)
{
	bool enabled;
	unsigned count;

	enabled = registry_lock();
	count = object_count;
	registry_unlock(enabled);

	/* Reports the sampled count. */
	return count;
}

/*
 * Counts the cached object pages.
 */
unsigned
vm_object_page_count(
	void)
{
	bool enabled;
	unsigned count;

	enabled = registry_lock();
	count = atomic_load_acquire(&object_pages);
	registry_unlock(enabled);

	/* Reports the sampled count. */
	return count;
}

/*
 * Counts the objects retained for a later writeback.
 */
unsigned
vm_object_retained_count(
	void)
{
	struct vm_object *object;
	unsigned count;
	bool enabled;

	count = 0;
	enabled = registry_lock();
	for (object = shared_objects; object != NULL; object = object->next) {
		if (object->flags & VM_OBJECT_RETAINED_WRITEBACK)
			count++;
	}
	registry_unlock(enabled);

	/* Reports the sampled count. */
	return count;
}

/*
 * Finds or creates the admitted shared object, taking a mapping reference.
 *
 * A writable file becomes the object's writeback file when it has none.
 * An object in final teardown is waited for and the lookup retried.
 */
static int
vm_object_get_shared_internal(
	struct file *file,
	struct vm_object **result)
{
	struct vm_object *object;
	struct inode *inode;
	int error;
	int writable;
	bool enabled;
	uint64_t sequence;
	struct vm_object *existing;

	/* Rejects a missing file or result, or a file without an inode. */
	if (file == NULL || result == NULL)
		return EINVAL;
	inode = file_vm_inode(file);
	if (inode == NULL)
		return EINVAL;
	writable = 0;
	if ((file_status_flags_get(file) & O_ACCMODE) != O_RDONLY &&
	    file->f_ops != NULL &&
	    file->f_ops->pwrite != NULL)
		writable = 1;

retry_lookup:
	/* Joins a published object, waiting out one that is detaching. */
	enabled = registry_lock();
	for (object = shared_objects; object != NULL; object = object->next) {
		if (object->inode != inode)
			continue;
		if ((object->flags & VM_OBJECT_DETACHING) != 0) {
			sequence = waitq_sequence(&object->registry_waitq);

			/* Pins an object which may be unlinked before wakeup. */
			object->registry_waiters++;
			if (object->registry_waiters == 0)
				HAL_FATAL("VM object registry waiter overflow");
			refcount_get(&object->refs);
			registry_unlock(enabled);
			error = object_wait_registry_transition(object, sequence);
			if (error != 0 && error != EAGAIN)
				return error;
			goto retry_lookup;
		}
		if (object->mapping_count == 0) {
			if (!(object->flags & VM_OBJECT_RETAINED_WRITEBACK))
				HAL_FATAL("reviving unretained VM object");
			object->flags &= ~VM_OBJECT_RETAINED_WRITEBACK;
		}
		if (writable && object->write_file == NULL) {
			file_ref(file);
			object->write_file = file;
		}
		object->mapping_count++;
		refcount_get(&object->refs);
		*result = object;
		registry_unlock(enabled);
		return 0;
	}
	registry_unlock(enabled);

	/* Builds a new object holding the registry and the caller's references. */
	object = kern_calloc(1, sizeof(*object));
	if (object == NULL)
		return ENOMEM;
	refcount_init(&object->refs, 2);
	object->mapping_count = 1;
	spin_init(&object->lock, LOCK_RANK_VM_OBJECT, "VM object");
	waitq_init(&object->page_waitq, "VM object page");
	waitq_init(&object->registry_waitq, "VM object registry");
	object->generation = 1;
	object->file = file;
	object->inode = inode;
	file_ref(file);
	if (writable) {
		object->write_file = file;
		file_ref(file);
	}

	/* Another CPU may have published the inode while allocation slept. */
	enabled = registry_lock();
	for (existing = shared_objects; existing != NULL;
	     existing = existing->next) {
		if (existing->inode != inode)
			continue;
		if ((existing->flags & VM_OBJECT_DETACHING) != 0) {
			sequence = waitq_sequence(&existing->registry_waitq);
			existing->registry_waiters++;
			if (existing->registry_waiters == 0)
				HAL_FATAL("VM object registry waiter overflow");
			refcount_get(&existing->refs);
			registry_unlock(enabled);
			destroy_object(object);
			error = object_wait_registry_transition(existing, sequence);
			if (error != 0 && error != EAGAIN)
				return error;
			goto retry_lookup;
		}
		if (existing->mapping_count == 0) {
			if (!(existing->flags & VM_OBJECT_RETAINED_WRITEBACK))
				HAL_FATAL("reviving unretained VM object");
			existing->flags &= ~VM_OBJECT_RETAINED_WRITEBACK;
		}
		existing->mapping_count++;
		refcount_get(&existing->refs);
		if (writable && existing->write_file == NULL) {
			file_ref(file);
			existing->write_file = file;
		}
		registry_unlock(enabled);
		destroy_object(object);
		*result = existing;
		return 0;
	}

	/* Publishes the new object with the inode's current end of file. */
	object_initialize_eof_locked(object, inode);
	object->next = shared_objects;
	shared_objects = object;
	object_count++;
	registry_unlock(enabled);
	*result = object;

	/* Reports the new object. */
	return 0;
}

/* Takes the registry lock with interrupts disabled, reporting whether they were enabled. */
static bool
registry_lock(
	void)
{
	bool enabled;

	if (hal_irq_disable != NULL)
		enabled = hal_irq_disable();
	else
		enabled = false;
	while (!atomic_try_acquire_zero(&object_registry_lock))
		hal_compiler_barrier();
	return enabled;
}

/* Releases the registry lock and restores the interrupt state. */
static void
registry_unlock(
	bool enabled)
{
	atomic_store_release(&object_registry_lock, 0);
	if (enabled && hal_irq_enable != NULL)
		hal_irq_enable();
}

/* Finds the object of an inode; the caller holds the registry lock. */
static struct vm_object *
find_object_by_inode_locked(
	struct inode *inode)
{
	struct vm_object *object;

	for (object = shared_objects; object != NULL; object = object->next) {
		if (object->inode == inode)
			return object;
	}
	return NULL;
}

/* Sleeps on an object's registry queue for the sequence observed. */
static int
object_wait_registry_sequence(
	struct vm_object *object,
	uint64_t sequence)
{
	unsigned long irq;
	int error;

	irq = spin_lock_irqsave(&object->lock);
	error = waitq_sleep(&object->registry_waitq, &object->lock, sequence,
	    0, 0);
	spin_unlock_irqrestore(&object->lock, irq);
	return error;
}

/* Waits for a teardown transition, then drops the waiter's pin taken under the registry lock. */
static int
object_wait_registry_transition(
	struct vm_object *object,
	uint64_t sequence)
{
	int error;
	int wake;
	bool enabled;

	error = object_wait_registry_sequence(object, sequence);
	enabled = registry_lock();
	if (object->registry_waiters == 0)
		HAL_FATAL("VM object registry waiter counter underflow");

	/* Registry/teardown ownership makes this a guaranteed non-final drop. */
	if (refcount_put(&object->refs))
		HAL_FATAL("VM object waiter lost teardown lifetime reference");
	object->registry_waiters--;
	wake = object->registry_waiters == 0;
	registry_unlock(enabled);
	if (wake)
		object_wake_registry_waiters(object);
	return error;
}

/* Wakes everyone waiting on an object's registry state. */
static void
object_wake_registry_waiters(
	struct vm_object *object)
{
	unsigned long irq;

	irq = spin_lock_irqsave(&object->lock);
	waitq_wake_all(&object->registry_waitq);
	spin_unlock_irqrestore(&object->lock, irq);
}

/* Waits until no registry waiter still pins an unlinked object. */
static void
object_wait_registry_waiters(
	struct vm_object *object)
{
	uint64_t sequence;
	int error;
	bool enabled;

	for (;;) {
		enabled = registry_lock();
		if (object->registry_waiters == 0) {
			registry_unlock(enabled);
			return;
		}
		sequence = waitq_sequence(&object->registry_waitq);
		registry_unlock(enabled);
		error = object_wait_registry_sequence(object, sequence);
		if (error != 0 && error != EAGAIN)
			HAL_FATAL("VM object registry waiter drain failed");
	}
}

/* Counts an operation on an object that is not detaching. */
static int
object_operation_begin(
	struct vm_object *object)
{
	bool enabled;

	enabled = registry_lock();
	if ((object->flags & VM_OBJECT_DETACHING) != 0) {
		registry_unlock(enabled);
		return EBUSY;
	}
	object->active_operations++;
	if (object->active_operations == 0)
		HAL_FATAL("VM object operation counter overflow");
	registry_unlock(enabled);
	return 0;
}

/* Counts an operation down, waking a final put waiting for the last. */
static void
object_operation_end(
	struct vm_object *object)
{
	int wake;
	bool enabled;

	enabled = registry_lock();
	if (object->active_operations == 0)
		HAL_FATAL("VM object operation counter underflow");
	object->active_operations--;
	wake = object->active_operations == 0;
	registry_unlock(enabled);
	if (wake)
		object_wake_registry_waiters(object);
}

/* Allocates one page of physical memory. */
static int
alloc_vm_page(
	struct hal_pmem *memory)
{
	const struct hal_pmem_request request = {
		HAL_PMEM_PADDR_ANY, PAGE_SIZE, PAGE_SIZE,
		HAL_PMEM_TYPE_RAM, 0
	};
	int error;

	error = hal_pmem_alloc(&request, memory);
	return error;
}

/* Reclaims a few private pages so a fault's backend read has scratch memory. */
static unsigned
reclaim_object_fault_reserve(
	void)
{
	unsigned reclaimed;

	/*
	 * A filesystem page read may need page-backed scratch storage after
	 * the object frame itself has consumed the page obtained by normal
	 * reclaim.  Reclaim a small bounded reserve only from the lockless
	 * fault-I/O boundary; the current object page is BUSY and therefore
	 * cannot be selected.
	 */
	for (reclaimed = 0; reclaimed < VM_OBJECT_FAULT_RESERVE_PAGES;
	     reclaimed++) {
		if (vm_reclaim_private_one(NULL) != 0)
			break;
	}
	return reclaimed;
}

/* Finds the cached page at an offset. */
static struct vm_object_page *
find_page(
	struct vm_object *object,
	off_t offset)
{
	struct vm_object_page *page;

	for (page = object->pages; page != NULL; page = page->next) {
		if (page->offset == offset)
			return page;
	}
	return NULL;
}

/* Unlinks a page from a list; the caller holds the object lock. */
static int
unlink_page_locked(
	struct vm_object_page **head,
	struct vm_object_page *page)
{
	struct vm_object_page **link;

	for (link = head; *link != NULL; link = &(*link)->next) {
		if (*link != page)
			continue;
		*link = page->next;
		page->next = NULL;
		return 1;
	}
	return 0;
}

/* Advances an object's generation, skipping zero; the caller holds the object lock. */
static uint64_t
next_generation(
	struct vm_object *object)
{
	object->generation++;
	if (object->generation == 0)
		object->generation++;
	return object->generation;
}

/* Advances an inode's resize generation, skipping zero. */
static uint64_t
next_inode_resize_generation_locked(
	struct inode *inode)
{
	inode->i_vm_resize_generation++;
	if (inode->i_vm_resize_generation == 0)
		inode->i_vm_resize_generation++;
	return inode->i_vm_resize_generation;
}

/* Advances an inode's content generation, skipping zero. */
static uint64_t
next_inode_content_generation_locked(
	struct inode *inode)
{
	inode->i_vm_content_generation++;
	if (inode->i_vm_content_generation == 0)
		inode->i_vm_content_generation++;
	return inode->i_vm_content_generation;
}

/* Gives an unpublished object the inode's end of file and any transaction in progress. */
static void
object_initialize_eof_locked(
	struct vm_object *object,
	struct inode *inode)
{
	unsigned long irq;

	irq = spin_lock_irqsave(&inode->i_vm_lock);

	/* A resize in progress pins registry teardown until it finishes. */
	if (inode->i_vm_resize_active) {
		object->logical_size = inode->i_vm_resize_old_size;
		object->size_generation = inode->i_vm_resize_generation;
		object->resize_generation = inode->i_vm_resize_generation;
		object->flags |= VM_OBJECT_RESIZING;
		object->active_operations = 1;
	} else {
		object->logical_size = inode->i_size;
		if (inode->i_vm_resize_generation != 0)
			object->size_generation = inode->i_vm_resize_generation;
		else
			object->size_generation = 1U;
	}

	/* So does a content write in progress. */
	if (inode->i_vm_content_active) {
		object->flags |= VM_OBJECT_CONTENT;
		object->content_generation = inode->i_vm_content_generation;
		object->active_operations++;
		if (object->active_operations == 0)
			HAL_FATAL("VM object operation counter overflow");
	}
	spin_unlock_irqrestore(&inode->i_vm_lock, irq);
}

/* Tests whether a page overlaps a byte range. */
static int
page_overlaps(
	const struct vm_object_page *page,
	uint64_t start,
	uint64_t end)
{
	uint64_t page_start;
	uint64_t page_end;

	if (page->offset < 0)
		return 0;
	page_start = (uint64_t)page->offset;
	page_end = page_start + PAGE_SIZE;
	if (page_end <= start)
		return 0;
	if (page_start >= end)
		return 0;
	return 1;
}

/* Tests whether a page is dirty; the caller holds the object lock. */
static int
page_is_dirty_locked(
	struct vm_object_page *page)
{
	if ((page->flags & VM_OBJECT_PAGE_DIRTY) == 0)
		return 0;
	return 1;
}

/* Tests whether any page is dirty; the caller holds the object lock. */
static int
object_has_dirty_pages_locked(
	struct vm_object *object)
{
	struct vm_object_page *page;

	for (page = object->pages; page != NULL; page = page->next) {
		if (page_is_dirty_locked(page))
			return 1;
	}
	return 0;
}

/* Tests whether any page or orphan is busy or held; the caller holds the object lock. */
static int
object_has_busy_pages_locked(
	const struct vm_object *object)
{
	const struct vm_object_page *page;

	for (page = object->pages; page != NULL; page = page->next) {
		if (page->flags & (VM_OBJECT_PAGE_BUSY | VM_OBJECT_PAGE_WRITEBACK))
			return 1;
		if (page->hold_count != 0)
			return 1;
	}
	for (page = object->orphan_pages; page != NULL; page = page->next) {
		if ((page->flags & (VM_OBJECT_PAGE_BUSY |
		    VM_OBJECT_PAGE_WRITEBACK)) != 0 || page->hold_count != 0)
			return 1;
	}
	return 0;
}

/* Tests whether any page or orphan is busy or held. */
static int
object_has_busy_pages(
	struct vm_object *object)
{
	unsigned long irq;
	int busy;

	irq = spin_lock_irqsave(&object->lock);
	busy = object_has_busy_pages_locked(object);
	spin_unlock_irqrestore(&object->lock, irq);
	return busy;
}

/* Remembers the first writeback error; the caller holds the object lock. */
static void
object_record_writeback_error_locked(
	struct vm_object *object,
	int error)
{
	if (object->writeback_error == 0 && error != 0)
		object->writeback_error = error;
}

/* Writes a page's data to the backend through the writeback file. */
static int
write_page_data(
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
	io_flags = FILE_IO_VM_OBJECT | FILE_IO_CONTENT_CHANGE;
	if (inode_io_owned)
		io_flags |= FILE_IO_INODE_IO_OWNED;
	error = file_io_begin(object->write_file, FILE_IO_PWRITE, page->offset,
	    io_flags, &io);
	if (error != 0)
		return error;
	count = file_io_transfer(&io, (void *)page->pmem.vaddr, length);
	file_io_end(&io);
	if (count == (ssize_t)length)
		return 0;
	if (count < 0)
		return (int)-count;
	return EIO;
}

/* Clears a page's dirty state; the caller holds the object lock. */
static void
clear_page_dirty_locked(
	struct vm_object_page *page)
{
	page->flags &= ~VM_OBJECT_PAGE_DIRTY;
	page->dirty_generation = 0;
}

/* Frees an unmapped, unheld page and its memory. */
static void
free_object_page(
	struct vm_object_page *page)
{
	/* Region teardown must remove every mapping before the final object ref. */
	if (page->mapping_count != 0 || page->hold_count != 0)
		HAL_FATAL("destroying mapped VM object page");
	(void)hal_pmem_free(&page->pmem);
	kern_free(page);
	if (atomic_load_acquire(&object_pages) == 0)
		HAL_FATAL("VM object page counter underflow");
	(void)atomic_fetch_add_relaxed(&object_pages, (unsigned)-1);
}

/* Tests whether an unmapped object is clean and idle enough to destroy. */
static int
object_can_destroy(
	struct vm_object *object)
{
	unsigned long irq;
	int result;

	if (object->mapping_count != 0 || object->writeback_error != 0)
		return 0;
	irq = spin_lock_irqsave(&object->lock);
	result = !object_has_dirty_pages_locked(object) &&
	    !object_has_busy_pages_locked(object);
	spin_unlock_irqrestore(&object->lock, irq);
	return result;
}

/* Removes a destroyable object from the registry; the caller holds the registry lock. */
static int
unlink_object_locked(
	struct vm_object *object)
{
	struct vm_object **link;

	if (!object_can_destroy(object))
		HAL_FATAL("destroying unsynchronized VM object");
	for (link = &shared_objects; *link != NULL; link = &(*link)->next) {
		if (*link == object) {
			*link = object->next;
			object->next = NULL;
			if (object_count == 0)
				HAL_FATAL("VM object counter underflow");
			object_count--;
			return 1;
		}
	}
	return 0;
}

/* Frees an object, its pages, its files, and its commitment. */
static void
destroy_object(
	struct vm_object *object)
{
	struct vm_object_page *page;

	page = object->pages;
	while (page != NULL) {
		object->pages = page->next;
		free_object_page(page);
		page = object->pages;
	}
	page = object->orphan_pages;
	while (page != NULL) {
		object->orphan_pages = page->next;
		free_object_page(page);
		page = object->orphan_pages;
	}
	if (object->file != NULL)
		(void)file_close(object->file);
	if (object->write_file != NULL)
		(void)file_close(object->write_file);
	if (object->commit_size != 0)
		vm_commit_release(object->commit_size);
	kern_free(object);
}

/* Keeps an unmapped object in the registry until its writeback succeeds. */
static void
retain_object(
	struct vm_object *object,
	int error)
{
	unsigned long irq;

	if (object->mapping_count != 0)
		HAL_FATAL("retaining referenced VM object");
	if (error == 0) {
		if (object_has_busy_pages(object))
			error = EBUSY;
		else
			error = EIO;
	}
	irq = spin_lock_irqsave(&object->lock);
	object_record_writeback_error_locked(object, error);
	spin_unlock_irqrestore(&object->lock, irq);
	object->flags |= VM_OBJECT_RETAINED_WRITEBACK;
}

/* Waits until no resize or content transaction is active on an object. */
static int
object_wait_resize(
	struct vm_object *object)
{
	unsigned long irq;
	int error;
	uint64_t sequence;

	irq = spin_lock_irqsave(&object->lock);
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
	return 0;
}

/* Releases the pages a content transaction took; the caller holds the object lock. */
static void
content_release_pages_locked(
	struct vm_object *object,
	uint64_t generation)
{
	struct vm_object_page *page;

	for (page = object->pages; page != NULL; page = page->next) {
		if (page->content_generation != generation)
			continue;
		page->content_generation = 0;
		page->write_generation = 0;
		page->write_dirty_generation = 0;
		page->flags &= ~(VM_OBJECT_PAGE_BUSY |
		    VM_OBJECT_PAGE_WRITEBACK);
	}
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
	if (commit && (!content->prepared || committed > content->length ||
	    (committed != 0 && buffer == NULL)))
		HAL_FATAL("invalid VM content commit");
	inode = content->inode;
	enabled = registry_lock();
	inode_irq = spin_lock_irqsave(&inode->i_vm_lock);
	if (!inode->i_vm_content_active ||
	    inode->i_vm_content_generation != content->generation)
		HAL_FATAL("inode lost VM content transaction");

	/* Updates the taken pages from the committed bytes and releases them. */
	object = find_object_by_inode_locked(inode);
	if (object != NULL) {
		write_start = (uint64_t)content->offset;
		write_end = write_start + committed;
		object_irq = spin_lock_irqsave(&object->lock);
		if ((object->flags & VM_OBJECT_CONTENT) == 0 ||
		    object->content_generation != content->generation)
			HAL_FATAL("VM object content generation mismatch");
		for (page = object->pages; page != NULL; page = page->next) {
			if (page->content_generation != content->generation)
				continue;
			if (commit && committed != 0) {
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
				if (copy_start < copy_end)
					memcpy((uint8_t *)page->pmem.vaddr +
					    (copy_start - page_start),
					    (const uint8_t *)buffer +
					    (copy_start - write_start),
					    (size_t)(copy_end - copy_start));
			}
		}
		content_release_pages_locked(object, content->generation);
		object->flags &= ~VM_OBJECT_CONTENT;
		object->content_generation = next_generation(object);
		waitq_wake_all(&object->page_waitq);
		spin_unlock_irqrestore(&object->lock, object_irq);
		if (object->active_operations == 0)
			HAL_FATAL("VM object content operation counter underflow");
		object->active_operations--;
		wake_registry = object->active_operations == 0;
	}

	/* Closes the transaction on the inode. */
	inode->i_vm_content_active = 0;
	inode->i_vm_content_start = 0;
	inode->i_vm_content_end = 0;
	(void)next_inode_content_generation_locked(inode);
	waitq_wake_all(&inode->i_vm_waitq);
	spin_unlock_irqrestore(&inode->i_vm_lock, inode_irq);
	registry_unlock(enabled);
	if (wake_registry)
		object_wake_registry_waiters(object);
	memset(content, 0, sizeof(*content));
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

	free_pages = NULL;
	wake_registry = 0;

	/* Ignores an inactive transaction; a commit must be prepared. */
	if (resize == NULL || !resize->active || resize->inode == NULL)
		return;
	if (commit && (!resize->prepared || logical_size < 0))
		HAL_FATAL("committing unprepared VM object resize");
	inode = resize->inode;
	enabled = registry_lock();
	inode_irq = spin_lock_irqsave(&inode->i_vm_lock);
	if (!inode->i_vm_resize_active ||
	    inode->i_vm_resize_generation != resize->generation)
		HAL_FATAL("inode lost VM object resize transaction");
	stable_generation = next_inode_resize_generation_locked(inode);
	object = find_object_by_inode_locked(inode);
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
	}

	/* Closes the transaction on the inode and frees the settled orphans. */
	inode->i_vm_resize_active = 0;
	inode->i_vm_resize_old_size = 0;
	inode->i_vm_resize_target_size = 0;
	waitq_wake_all(&inode->i_vm_waitq);
	spin_unlock_irqrestore(&inode->i_vm_lock, inode_irq);
	registry_unlock(enabled);
	if (wake_registry)
		object_wake_registry_waiters(object);
	while (free_pages != NULL) {
		page = free_pages;
		free_pages = page->next;
		free_object_page(page);
	}
	memset(resize, 0, sizeof(*resize));
}

/* Copies into or out of a pinned page once it is not busy. */
static int
vm_object_page_pin_copy(
	struct vm_object_page *page,
	size_t offset,
	void *buffer,
	size_t length,
	int write)
{
	struct vm_object *object;
	unsigned long irq;
	int error;
	uint64_t sequence;

	/* Rejects a missing page, owner, or buffer, or a range past the page. */
	if (page == NULL || buffer == NULL)
		return EINVAL;
	object = page->owner;
	if (object == NULL || offset > PAGE_SIZE || length > PAGE_SIZE - offset)
		return EINVAL;

	/* Waits for the page to leave any writeback, still pinned. */
	irq = spin_lock_irqsave(&object->lock);
	for (;;) {
		if (page->owner != object || page->pin_count == 0) {
			spin_unlock_irqrestore(&object->lock, irq);
			return EINVAL;
		}
		if ((page->flags & (VM_OBJECT_PAGE_BUSY |
		    VM_OBJECT_PAGE_WRITEBACK)) == 0)
			break;
		sequence = waitq_sequence(&object->page_waitq);
		error = waitq_sleep(&object->page_waitq, &object->lock,
		    sequence, 0, 0);
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
		memcpy((uint8_t *)page->pmem.vaddr + offset, buffer, length);
		page->flags |= VM_OBJECT_PAGE_DIRTY;
		page->dirty_generation = next_generation(object);
	} else {
		memcpy(buffer, (const uint8_t *)page->pmem.vaddr + offset, length);
	}
	spin_unlock_irqrestore(&object->lock, irq);
	return 0;
}

/* Tests whether any page in a range has a wired mapping. */
static int
range_has_wired_mapping(
	struct vm_object *object,
	uint64_t start,
	uint64_t end)
{
	struct vm_object_page *page;
	struct vm_page *mapping;

	for (page = object->pages; page != NULL; page = page->next) {
		if (!page_overlaps(page, start, end))
			continue;
		for (mapping = page->mappings; mapping != NULL;
		     mapping = mapping->object_next) {
			if (mapping->wire_count != 0)
				return 1;
		}
	}
	return 0;
}

/* Revokes, writes back, and optionally invalidates the pages of a range. */
static int
vm_object_sync_range_internal(
	struct vm_object *object,
	off_t offset,
	size_t size,
	int flags,
	int detaching,
	int resize_owner,
	off_t resize_target)
{
	struct vm_object_page *page;
	struct vm_object_page **link;
	struct vm_object_page *retired;
	off_t write_limit;
	uint64_t start;
	uint64_t end;
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

	/* Rejects a missing object, an empty range, or inconsistent flags. */
	if (object == NULL ||
	    offset < 0 ||
	    size == 0 ||
	    (flags & ~(MS_ASYNC | MS_INVALIDATE | MS_SYNC)) != 0 ||
	    (flags & (MS_ASYNC | MS_SYNC)) == 0 ||
	    (flags & (MS_ASYNC | MS_SYNC)) == (MS_ASYNC | MS_SYNC))
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
			transaction_active = (object->flags &
			    (VM_OBJECT_RESIZING | VM_OBJECT_CONTENT)) != 0;
			spin_unlock_irqrestore(&object->lock, irq);
			if (!transaction_active)
				break;
			mutex_unlock(&object->inode->i_io_lock);
			error = object_wait_resize(object);
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
		if (range_has_wired_mapping(object, start, end)) {
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
	if (detaching) {
		for (page = object->pages; page != NULL; page = page->next) {
			if (page->mappings != NULL || page->mapping_count != 0)
				HAL_FATAL("detaching VM object still has reverse mappings");
		}
	}
	sync_generation = next_generation(object);
	spin_unlock_irqrestore(&object->lock, irq);

	/*
	 * Revoke every resident page in the range before deciding whether it
	 * is dirty.  BUSY excludes pin copies and new fault publication;
	 * draining the non-pin holds closes the pre-existing fault/publish
	 * window.
	 */
	for (;;) {
		candidate = NULL;
		has_mappings = 0;
		wait_for_page = 0;
		dirty = 0;
		observed = 0;
		irq = spin_lock_irqsave(&object->lock);
		for (page = object->pages; page != NULL; page = page->next) {
			if (!page_overlaps(page, start, end) ||
			    page->write_generation == sync_generation)
				continue;
			if ((page->flags & (VM_OBJECT_PAGE_BUSY |
			    VM_OBJECT_PAGE_WRITEBACK)) != 0 ||
			    page->hold_count != page->pin_count ||
			    (!resize_owner && (flags & MS_INVALIDATE) != 0 &&
			    page->pin_count != 0)) {
				wait_for_page = 1;
				break;
			}
			candidate = page;
			break;
		}
		if (wait_for_page) {
			sequence = waitq_sequence(&object->page_waitq);
			error = waitq_sleep(&object->page_waitq, &object->lock,
			    sequence, 0, 0);
			if (error != 0 && error != EAGAIN) {
				first_error = error;
				spin_unlock_irqrestore(&object->lock, irq);
				break;
			}
			spin_unlock_irqrestore(&object->lock, irq);
			continue;
		}
		if (candidate == NULL) {
			spin_unlock_irqrestore(&object->lock, irq);
			break;
		}
		candidate->flags |= VM_OBJECT_PAGE_BUSY |
		    VM_OBJECT_PAGE_WRITEBACK;
		candidate->write_generation = sync_generation;
		has_mappings = candidate->mapping_count != 0;
		spin_unlock_irqrestore(&object->lock, irq);

		/* Revokes the mappings and writes the page back when dirty. */
		if (has_mappings) {
			if (vmspace_object_page_revoke == NULL)
				first_error = EOPNOTSUPP;
			else
				first_error = vmspace_object_page_revoke(candidate,
				    &observed);
		}
		irq = spin_lock_irqsave(&object->lock);
		if ((observed & HAL_PAGE_DIRTY) != 0) {
			candidate->flags |= VM_OBJECT_PAGE_DIRTY;
			candidate->dirty_generation = next_generation(object);
		}
		dirty = (candidate->flags & VM_OBJECT_PAGE_DIRTY) != 0;
		candidate->write_dirty_generation = candidate->dirty_generation;
		spin_unlock_irqrestore(&object->lock, irq);
		if (first_error == 0 && dirty &&
		    (object->flags & VM_OBJECT_ANONYMOUS) == 0) {
			first_error = write_page_data(object, candidate, write_limit,
			    !resize_owner);
			selected_write = 1;
		}
	}
	irq = spin_lock_irqsave(&object->lock);
	retry_writeback = object->writeback_error != 0;
	spin_unlock_irqrestore(&object->lock, irq);

	/*
	 * Resize writeback is immediately followed by its serialized
	 * mutation; ordinary sync/final-put waits for the backend's
	 * durability barrier.
	 */
	if (!resize_owner && (selected_write || retry_writeback) &&
	    first_error == 0) {
		if (object->write_file == NULL) {
			error = EACCES;
		} else if (object->write_file->f_ops != NULL &&
		    object->write_file->f_ops->fsync != NULL) {
			error = object->write_file->f_ops->fsync(object->write_file);
		} else if (object->inode != NULL &&
		    object->inode->i_op != NULL &&
		    object->inode->i_op->sync != NULL) {
			error = object->inode->i_op->sync(object->inode);
		} else {
			error = 0;
		}
		if (error != 0)
			first_error = error;
	}

	/* Releases the pages, orphaning or retiring the ones a caller asked for. */
	irq = spin_lock_irqsave(&object->lock);
	if (first_error != 0)
		object_record_writeback_error_locked(object, first_error);
	link = &object->pages;
	for (;;) {
		page = *link;
		if (page == NULL)
			break;
		if (page->write_generation != sync_generation) {
			link = &page->next;
			continue;
		}
		if (first_error == 0 &&
		    page->write_dirty_generation != 0 &&
		    page->dirty_generation == page->write_dirty_generation)
			clear_page_dirty_locked(page);
		page->write_generation = 0;
		page->write_dirty_generation = 0;
		page->flags &= ~(VM_OBJECT_PAGE_BUSY | VM_OBJECT_PAGE_WRITEBACK);
		if (first_error == 0 && resize_owner && page->pin_count != 0) {
			if (page->mappings != NULL ||
			    page->mapping_count != 0 ||
			    page->hold_count != page->pin_count)
				HAL_FATAL("invalid VM resize orphan candidate");
			*link = page->next;
			page->flags |= VM_OBJECT_PAGE_ORPHANED;
			page->next = object->orphan_pages;
			object->orphan_pages = page;
			continue;
		}
		if (first_error == 0 && (flags & MS_INVALIDATE) != 0) {
			if (page->mappings != NULL ||
			    page->mapping_count != 0 ||
			    page->hold_count != 0 ||
			    page->pin_count != 0)
				HAL_FATAL("invalid VM invalidate retire candidate");
			*link = page->next;
			page->next = retired;
			retired = page;
			continue;
		}
		link = &page->next;
	}
	waitq_wake_all(&object->page_waitq);
	if (first_error == 0 && !object_has_dirty_pages_locked(object))
		object->writeback_error = 0;
	spin_unlock_irqrestore(&object->lock, irq);

	/* Frees the retired pages and releases the inode. */
	while (retired != NULL) {
		page = retired;
		retired = page->next;
		free_object_page(page);
	}
	if (held_inode_io)
		mutex_unlock(&object->inode->i_io_lock);
	return first_error;
}
