/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The object page cache: its counts, pins and drains, the discard of a
 * mount's objects and the reclaim of clean object pages.
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

static int object_cache_idle_locked(struct vm_object *object);
static size_t object_detach_idle_pages_locked(struct vm_object *object, size_t freed, size_t target, struct vm_object_page **retired);
static int object_page_idle_clean(const struct vm_object_page *page);
static int object_reclaimable_locked(struct vm_object *object);
static int object_discard_mount_check_locked(struct mount *mount);
static int object_discard_file_check(struct vm_object *, struct mount *, struct file *);
static int cache_read_pages(struct vm_object *object, off_t offset, void *buffer, size_t length, off_t eof, uint64_t size_generation, uint64_t content_generation, ssize_t *result, int *handled);

/*
 * The private frames of one absent run that the page cache reads from the
 * disk itself, kept off the stack.
 */
struct cache_read_run {
	struct vm_object_page *pages[VM_OBJECT_READ_WINDOW_PAGES];
};

/*
 * Counts the published objects.
 */
unsigned
vm_object_count(
	void)
{
	bool enabled;
	unsigned count;

	enabled = vm_object_registry_lock();
	count = object_count;
	vm_object_registry_unlock(enabled);

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

	enabled = vm_object_registry_lock();
	count = atomic_load_acquire(&object_pages);
	vm_object_registry_unlock(enabled);

	/* Reports the sampled count. */
	return count;
}

/*
 * Prepares optional read ownership before the caller takes I/O locks.
 */
void
vm_object_cache_prepare(
	struct file *file)
{
	struct backing_mutation_guard guard;
	struct vm_object *object;
	struct inode *inode;
	int error;
	int unnamed;

	/*
	 * Excludes non-files and internal backing owners from optional caching.
	 */
	if (file == NULL || file->f_inode == NULL ||
	    file->f_inode->i_type != INODE_REG || file->f_ops == NULL ||
	    file->f_ops->pread == NULL || file->f_format_claim != NULL ||
	    file->f_backing_claim != NULL)
		return;

	inode = file_vm_inode(file);
	if (inode == NULL ||
	    (inode->i_flags & (INODE_LOOPFILE | INODE_SWAPFILE)) != 0)
		return;

	/* A removed file is not cached: the cache would keep its storage (BUG-075). */
	unnamed = vm_object_inode_unnamed(inode);
	if (unnamed)
		return;

	/*
	 * Keeps a competing claim from publishing over the new cache identity.
	 */
	error = backing_mutation_begin_inode(inode, &guard);
	if (error != 0)
		return;

	/* Retries optional admission once after reclaiming one idle object. */
	error = vm_object_get_shared_internal(file, &object, 1);
	if (error == ENOMEM && vm_object_cache_evict_one(NULL))
		error = vm_object_get_shared_internal(file, &object, 1);
	backing_mutation_end(&guard);
	if (error != 0)
		return;

	/*
	 * Drops the operation reference while retaining the independent cache.
	 */
	vm_object_operation_end(object);
	if (refcount_put(&object->refs))
		vm_object_destroy(object);
}

/*
 * Pins a published cache across every chunk of a shared content read lease.
 */
int
vm_object_cache_pin(
	struct inode *inode,
	struct vm_object **result)
{
	struct vm_object *object;
	bool enabled;

	/*
	 * Refuses absent and transitioning identities without allocating or
	 * waiting.
	 */
	if (inode == NULL || result == NULL)
		return EINVAL;
	*result = NULL;
	enabled = vm_object_registry_lock();
	object = vm_object_find_by_inode_locked(inode);
	if (object == NULL || (object->flags & VM_OBJECT_DETACHING) != 0) {
		vm_object_registry_unlock(enabled);
		return EAGAIN;
	}

	/*
	 * Keeps final mapping teardown and cache eviction outside the
	 * transaction.
	 */
	object->active_operations++;
	if (object->active_operations == 0)
		HAL_FATAL("VM cache pin operation counter overflow");
	refcount_get(&object->refs);
	*result = object;
	vm_object_registry_unlock(enabled);
	return 0;
}

/*
 * Releases a cache pin after the caller has released its content and file locks.
 */
void
vm_object_cache_unpin(
	struct vm_object *object)
{
	/* Balances both the operation and its lifetime reference. */
	if (object == NULL)
		return;
	vm_object_operation_end(object);
	if (refcount_put(&object->refs))
		vm_object_destroy(object);
}

/*
 * Makes read progress without releasing the caller's cache identity pin.
 * The caller must own that pin and retain no page pointer across this call.
 */
size_t
vm_object_cache_reclaim_pinned(
	struct vm_object *object,
	size_t target)
{
	struct vm_object_page *page;
	struct vm_object_page *retired;
	size_t freed;
	unsigned long irq;
	bool enabled;

	if (object == NULL || target == 0)
		return 0;
	if (target > KERN_IO_BATCH_MAX)
		target = KERN_IO_BATCH_MAX;
	retired = NULL;
	freed = 0;

	/*
	 * The caller is the only resident-page user. Prefetch operations own
	 * private frames and publish under the object lock, so they do not
	 * prevent retirement of unrelated clean pages. Faults, other readers,
	 * waiters, mappings and transactions keep the conservative exclusion.
	 */
	enabled = vm_object_registry_lock();
	if ((object->flags & VM_OBJECT_CACHE_REFERENCE) == 0 ||
	    (object->flags & (VM_OBJECT_DETACHING | VM_OBJECT_RESIZING |
	    VM_OBJECT_CONTENT | VM_OBJECT_RETAINED_WRITEBACK)) != 0 ||
	    object->mapping_count != 0 || object->registry_waiters != 0 ||
	    object->active_operations <= object->prefetch_operations ||
	    object->active_operations - object->prefetch_operations != 1 ||
	    refcount_load(&object->refs) < 2 ||
	    refcount_load(&object->refs) - 2 != object->prefetch_operations) {

		vm_object_registry_unlock(enabled);
		return 0;
	}

	/* Detaches only immediately disposable pages; the object stays pinned. */
	irq = spin_lock_irqsave(&object->lock);

	freed = object_detach_idle_pages_locked(object, freed, target, &retired);

	spin_unlock_irqrestore(&object->lock, irq);

	vm_object_registry_unlock(enabled);

	/* Returns physical storage and shared credits outside metadata locks. */
	while (retired != NULL) {
		page = retired;
		retired = page->next;
		vm_object_free_page(page);
	}
	return freed;
}

/*
 * Discards eligible clean cache references before mount or claim teardown.
 */
unsigned
vm_object_cache_drain(
	struct mount *mount)
{
	unsigned count;

	/* Bounds this pass even when other readers keep publishing objects. */
	count = 0;
	while (count < VM_OBJECT_CACHE_OBJECTS) {
		if (!vm_object_cache_evict_one(mount))
			break;
		count++;
	}

	/* Reports the number of detached cache objects. */
	return count;
}

/*
 * Global reclaim must not acquire owners after unmount has closed admission.
 */
int
vm_object_mount_reserved(
	const struct vm_object *object)
{
	if (object->inode != NULL && object->inode->i_mount != NULL &&
	    object->inode->i_mount->m_state == MOUNT_STATE_DYING)
		return 1;
	if (object->file != NULL && object->file->f_path.p_mount != NULL &&
	    object->file->f_path.p_mount->m_state == MOUNT_STATE_DYING)
		return 1;
	if (object->write_file != NULL && object->write_file->f_path.p_mount != NULL &&
	    object->write_file->f_path.p_mount->m_state == MOUNT_STATE_DYING)
		return 1;
	return 0;
}

/*
 * Counts only paths proven to disappear when the eligible objects are
 * destroyed.
 */
int
vm_object_discard_mount_refs(
	struct mount *mount,
	struct inode *inode,
	unsigned *refs)
{
	struct vm_object *object;
	unsigned count;
	unsigned owned;
	bool enabled;
	int error;

	if (refs == NULL)
		return EINVAL;
	*refs = 0;
	if (mount == NULL || mount->m_disk == NULL)
		return EINVAL;
	if (disk_media_status(mount->m_disk) == 0)
		return EINVAL;
	if (inode != NULL && inode->i_mount != mount)
		return EINVAL;

	/* Caller keeps new mount users out; no inode-cache lock may be held here. */
	count = 0;
	enabled = vm_object_registry_lock();
	error = object_discard_mount_check_locked(mount);
	if (error != 0) {
		vm_object_registry_unlock(enabled);
		return error;
	}
	for (object = shared_objects; object != NULL; object = object->next) {
		if (object->inode == NULL || object->inode->i_mount != mount)
			continue;
		if (inode != NULL && object->inode != inode)
			continue;

		owned = 0;
		if (object->file != NULL)
			owned++;
		if (object->write_file != NULL && object->write_file != object->file)
			owned++;
		if (count > UINT_MAX - owned) {
			vm_object_registry_unlock(enabled);
			return EOVERFLOW;
		}
		count += owned;
	}
	vm_object_registry_unlock(enabled);
	*refs = count;
	return 0;
}

/*
 * The caller has quiesced the mount, workers and all non-VM dependencies.
 */
int
vm_object_discard_mount(struct mount *mount, uint64_t *dirty_bytes)
{
	struct vm_object *object;
	struct vm_object *next;
	struct vm_object *detached;
	struct vm_object_page *page;
	uint64_t discarded;
	unsigned long irq;
	unsigned list;
	bool enabled;
	int error;

	if (dirty_bytes == NULL)
		return EINVAL;
	*dirty_bytes = 0;
	if (mount == NULL || mount->m_disk == NULL || disk_media_status(mount->m_disk) == 0)
		return EINVAL;
	discarded = 0;
	detached = NULL;
	enabled = vm_object_registry_lock();
	error = object_discard_mount_check_locked(mount);
	if (error != 0) {
		vm_object_registry_unlock(enabled);
		return error;
	}
	/* No fallible admission remains; remove all selected identities before freeing. */
	for (object = shared_objects; object != NULL; object = next) {
		next = object->next;
		if ((object->inode == NULL || object->inode->i_mount != mount) &&
		    (object->file == NULL || object->file->f_path.p_mount != mount) &&
		    (object->write_file == NULL || object->write_file->f_path.p_mount != mount))
			continue;
		irq = spin_lock_irqsave(&object->lock);

		/* Forgets the dirty data of the resident pages and of the orphans. */
		for (list = 0; list < 2; list++) {
			if (list == 0)
				page = vm_object_page_at_or_after(object, 0);
			else
				page = object->orphan_pages;
			while (page != NULL) {
				if (page->flags & VM_OBJECT_PAGE_DIRTY)
					discarded += PAGE_SIZE;
				vm_object_clear_page_dirty_locked(page);
				if (list == 0)
					page = vm_object_page_after(object, page);
				else
					page = page->next;
			}
		}
		object->writeback_error = 0;
		object->flags &= ~VM_OBJECT_RETAINED_WRITEBACK;
		spin_unlock_irqrestore(&object->lock, irq);
		if (!vm_object_unlink_locked(object))
			HAL_FATAL("revoked VM discard lost registry entry");
		object->next = detached;
		detached = object;
	}
	vm_object_registry_unlock(enabled);

	/* Physical frame and file finalizers may sleep or enter other owners. */
	while (detached != NULL) {
		object = detached;
		detached = object->next;
		object->next = NULL;
		if (!refcount_put(&object->refs))
			HAL_FATAL("revoked VM discard retained external reference");
		vm_object_destroy(object);
	}
	*dirty_bytes = discarded;
	return 0;
}

/*
 * Retires idle clean cache pages without waiting or performing filesystem I/O.
 */
size_t
vm_object_reclaim_clean(
	size_t target)
{
	struct vm_object *object;
	struct vm_object_page *page;
	struct vm_object_page *retired;
	size_t freed;
	unsigned long irq;
	bool enabled;
	int reclaimable;

	/* Bounds one pressure pass and excludes every concurrent metadata user. */
	if (target > KERN_IO_BATCH_MAX)
		target = KERN_IO_BATCH_MAX;
	retired = NULL;
	freed = 0;
	enabled = vm_object_registry_lock();
	for (object = shared_objects; object != NULL && freed < target;
	     object = object->next) {
		/* Skips an object anything still owns. */
		reclaimable = object_reclaimable_locked(object);
		if (!reclaimable)
			continue;

		/* Detaches every clean, unreferenced page of the object. */
		irq = spin_lock_irqsave(&object->lock);

		freed = object_detach_idle_pages_locked(object, freed, target, &retired);

		spin_unlock_irqrestore(&object->lock, irq);
	}

	vm_object_registry_unlock(enabled);

	/* Frees detached storage without closing any inode or mount reference. */
	while (retired != NULL) {
		page = retired;
		retired = page->next;
		vm_object_free_page(page);
	}

	/* Reports how many bytes the pass freed. */
	return freed;
}

/*
 * Retires idle clean file pages in the order of the clock, without waiting
 * or performing file system I/O.
 *
 * The hand offers pages nobody used since it last passed them; a page is
 * taken only when its object is one the registry alone holds (as for
 * vm_object_reclaim_clean) and the page is clean and idle.  One pass looks
 * at no more than two turns of the clock, so that every referenced bit can
 * be cleared and its page offered once.
 */
size_t
vm_object_clock_reclaim(
	size_t target)
{
	struct vm_object *object;
	struct vm_object_page *candidate;
	struct vm_object_page *page;
	struct vm_object_page *retired;
	off_t offset;
	size_t budget;
	size_t freed;
	unsigned long irq;
	bool enabled;
	int reclaimable;
	int idle;
	int error;

	/* Bounds one pressure pass and excludes every concurrent metadata user. */
	if (target > KERN_IO_BATCH_MAX)
		target = KERN_IO_BATCH_MAX;
	retired = NULL;
	freed = 0;
	enabled = vm_object_registry_lock();

	/* Two turns of the clock as it is now bound the pages this pass looks at. */
	budget = vm_object_clock_count() * 2U;

	/* Takes the pages the hand offers until the target is met or the budget is spent. */
	while (freed < target) {
		error = vm_object_clock_offer_locked(&budget, &candidate, &object, &offset);
		if (error != 0)
			break;

		/* Passes over a page whose object is leaving the registry or that anything still owns. */
		if (object == NULL)
			continue;
		reclaimable = object_reclaimable_locked(object);
		if (!reclaimable)
			continue;

		/*
		 * The page is trusted only if the object still has it at the
		 * offset the hand saw, and it is clean and idle there.
		 */
		irq = spin_lock_irqsave(&object->lock);

		page = vm_object_find_page(object, offset);
		idle = 0;
		if (page == candidate)
			idle = object_page_idle_clean(page);
		if (idle) {
			vm_object_page_index_remove(object, page);
			page->next = retired;
			retired = page;
			freed += page->pmem.size;
		}

		spin_unlock_irqrestore(&object->lock, irq);
	}

	vm_object_registry_unlock(enabled);

	/* Frees detached storage without closing any inode or mount reference. */
	while (retired != NULL) {
		page = retired;
		retired = page->next;
		vm_object_free_page(page);
	}

	/* Reports how many bytes the pass freed. */
	return freed;
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

	enabled = vm_object_registry_lock();
	for (object = shared_objects; object != NULL; object = object->next) {
		if (vm_object_mount_reserved(object))
			continue;
		/* Skips an object that is being detached or resized. */
		if ((object->flags & (VM_OBJECT_DETACHING |
		    VM_OBJECT_RESIZING)) != 0)
			continue;

		/* Looks for a reclaimable page of this object, the lowest offset first. */
		irq = spin_lock_irqsave(&object->lock);
		for (page = vm_object_page_at_or_after(object, 0);
		     page != NULL;
		     page = vm_object_page_after(object, page)) {
			/* Skips a page that is held or under I/O. */
			wired = 0;
			if ((page->flags & (VM_OBJECT_PAGE_BUSY |
			    VM_OBJECT_PAGE_WRITEBACK)) != 0 ||
			    page->hold_count != 0)
				continue;

			/* Skips a page that any mapping wires down. */
			for (mapping = page->mappings; mapping != NULL;
			     mapping = mapping->object_next) {
				if (mapping->wire_count != 0) {
					wired = 1;
					break;
				}
			}

			if (wired)
				continue;

			/* Takes the page, pinning its object for the writeback. */
			candidate_offset = page->offset;
			found = 1;
			object->active_operations++;
			if (object->active_operations == 0)
				HAL_FATAL("VM object operation counter overflow");
			refcount_get(&object->refs);
			break;
		}

		spin_unlock_irqrestore(&object->lock, irq);

		/* Stops at the first object that offered a page. */
		if (found)
			break;
	}

	vm_object_registry_unlock(enabled);

	vm_metadata_leave();

	/* Reports an empty object store. */
	if (!found)
		return ENOMEM;

	/* Writes the page back and invalidates it. */
	error = vm_object_sync_range_internal(object, candidate_offset,
	    PAGE_SIZE, MS_SYNC | MS_INVALIDATE, 0, 0, 0);

	/* Unregisters an object the writeback left empty and unreferenced. */
	removed = 0;
	if (error == 0) {
		enabled = vm_object_registry_lock();
		if (object->mapping_count == 0 &&
		    object->active_operations == 1 &&
		    (object->flags & VM_OBJECT_DETACHING) == 0 &&
		    vm_object_can_destroy(object))
			removed = vm_object_unlink_locked(object);
		vm_object_registry_unlock(enabled);
	}

	if (removed && refcount_put(&object->refs))
		HAL_FATAL("VM object registry reference was last unexpectedly");

	/* Ends the operation and drops the reference this pass took. */
	vm_object_operation_end(object);
	if (refcount_put(&object->refs))
		vm_object_destroy(object);

	/* Reclaim reports whether a page was freed; writeback retains error. */
	if (error != 0)
		return ENOMEM;
	return 0;
}

/*
 * Discards the idle cache-only object of one inode (BUG-075).  A file's
 * page cache opens a read handle of its own on the inode; an unlinked file
 * whose data only the cache still holds keeps its storage until that
 * handle closes.  A filesystem calls this when a removal leaves the inode
 * without names, so the storage goes back as the last other holder lets
 * go.  An object that is mapped, dirty or in use is left for its owners.
 * Returns 1 when an object was discarded.
 */
int
vm_object_cache_discard_inode(
	struct inode *inode)
{
	struct vm_object *object;
	bool enabled;
	int idle;
	int unlinked;
	int last;

	/* A missing inode has no object. */
	if (inode == NULL)
		return 0;

	/* The inode's object, if the cache alone holds it. */
	enabled = vm_object_registry_lock();
	for (object = object_hash[vm_object_hash_index(inode)]; object != NULL;
	     object = object->hash_next) {
		if (object->inode == inode)
			break;
	}

	/* Handles the object that is absent or still owned. */
	idle = 0;
	if (object != NULL)
		idle = object_cache_idle_locked(object);
	if (!idle) {
		vm_object_registry_unlock(enabled);
		return 0;
	}

	/* Takes it out of the registry, which closes admission. */
	unlinked = vm_object_unlink_locked(object);
	if (!unlinked)
		HAL_FATAL("VM cache discard lost registry entry");
	vm_object_registry_unlock(enabled);

	/* Releases the sole registry reference and closes the handle outside locks. */
	last = refcount_put(&object->refs);
	if (!last)
		HAL_FATAL("discarded VM cache retained an unknown reference");
	vm_object_destroy(object);

	/* Reports the discarded object. */
	return 1;
}

/*
 * Unlinks one idle cache under the registry lock, then closes it outside locks.
 */
int
vm_object_cache_evict_one(
	struct mount *mount)
{
	struct vm_object *object;
	struct vm_object *oldest;
	bool enabled;
	int unlinked;
	int idle;

	/*
	 * Leaves every mandatory mapping, operation, pin and failure owner
	 * intact.
	 */
	enabled = vm_object_registry_lock();
	oldest = NULL;

	for (object = shared_objects; object != NULL; object = object->next) {
		/* Skips an object anything still owns. */
		idle = object_cache_idle_locked(object);
		if (!idle)
			continue;

		/* Skips an object outside the mount the caller named. */
		if (mount != NULL && object->file->f_path.p_mount != mount &&
		    (object->write_file == NULL ||
		    object->write_file->f_path.p_mount != mount))
			continue;

		/* Remembers the least recently used of the evictable objects. */
		if (oldest == NULL || object->last_use < oldest->last_use)
			oldest = object;
	}

	/* Takes the least recently used object out of the registry. */
	object = oldest;
	unlinked = 1;
	if (object != NULL)
		unlinked = vm_object_unlink_locked(object);
	if (!unlinked)
		HAL_FATAL("VM cache eviction lost registry entry");
	vm_object_registry_unlock(enabled);

	/* Reports that the cache held nothing evictable. */
	if (object == NULL)
		return 0;

	/*
	 * Releases the sole registry/cache reference after closing admission.
	 */
	if (!refcount_put(&object->refs))
		HAL_FATAL("detached VM cache retained an unknown reference");

	vm_object_destroy(object);

	/* Reports one complete eviction. */
	return 1;
}

/*
 * Frees only unpublished preparation frames, which were never counted resident.
 */
void
vm_object_cache_discard_prepared(
	struct vm_object_page **pages,
	unsigned count)
{
	unsigned index;

	/* Releases every successfully prepared frame and its metadata. */
	for (index = 0; index < count; index++) {
		vm_object_release_page_storage(pages[index]);
	}
}

/*
 * Reads an absent bounded run and publishes only fully initialized cache pages.
 */
int
vm_object_cache_read_missing(
	struct vm_object *object,
	off_t offset,
	void *buffer,
	size_t length,
	ssize_t *result,
	int *handled,
	int *stop)
{
	struct vm_object_page *pages[VM_OBJECT_CACHE_PAGES];
	struct vm_object_page *page;
	struct file_io io;
	void *scratch;
	off_t base;
	off_t eof;
	uint64_t size_generation;
	uint64_t content_generation;
	size_t capacity;
	size_t in_page;
	size_t wanted;
	size_t transfer;
	size_t valid;
	size_t copied;
	ssize_t count;
	unsigned prepared;
	unsigned needed;
	unsigned index;
	unsigned long irq;
	int error;
	int publish;
	int through;
	int page_io;

	/* Leaves already resident pages to the existing coherent fault path. */
	*handled = 0;
	*stop = 0;
	*result = 0;
	base = offset & ~(off_t)(PAGE_SIZE - 1U);
	in_page = (size_t)(offset - base);
	irq = spin_lock_irqsave(&object->lock);

	if ((object->flags & VM_OBJECT_CACHE_REFERENCE) == 0 ||
	    vm_object_find_page(object, base) != NULL) {
		spin_unlock_irqrestore(&object->lock, irq);
		return 0;
	}

	/* Samples the file geometry this run has to stay consistent with. */
	eof = object->logical_size;
	size_generation = object->size_generation;
	content_generation = object->content_generation;

	spin_unlock_irqrestore(&object->lock, irq);

	/* A read that starts at or past the end of file stops the caller. */
	if (offset >= eof) {
		*handled = 1;
		*stop = 1;
		return 0;
	}

	/* A file whose data the page cache reads itself is read into its pages. */
	page_io = vm_object_page_io_candidate(object);
	if (page_io) {
		error = cache_read_pages(object, offset, buffer, length, eof,
		    size_generation, content_generation, result, handled);
		if (error != EOPNOTSUPP)
			return error;
	}

	/*
	 * Bounds the run by requested bytes, EOF and the first resident page.
	 * A long read goes straight to the caller's buffer, up to
	 * VM_OBJECT_READ_THROUGH_MAX bytes a run, without making the pages
	 * resident: they would be copied twice more and then be no more use
	 * than the device cache's copy of the blocks.
	 */
	through = length >= VM_OBJECT_READ_THROUGH_MIN;
	wanted = (through ? VM_OBJECT_READ_THROUGH_MAX : KERN_IO_BATCH_MAX) - in_page;

	if (wanted > length)
		wanted = length;

	if ((uint64_t)wanted > (uint64_t)(eof - offset))
		wanted = (size_t)(eof - offset);

	transfer = (in_page + wanted + PAGE_SIZE - 1U) & ~(size_t)(PAGE_SIZE - 1U);
	if ((uint64_t)transfer > (uint64_t)(eof - base))
		transfer = (size_t)(eof - base);

	irq = spin_lock_irqsave(&object->lock);

	/* Stops the read at the first page that is already resident. */
	for (index = 1; (size_t)index * PAGE_SIZE < transfer; index++) {
		if (vm_object_find_page(object, base + (off_t)index * PAGE_SIZE) != NULL) {
			transfer = (size_t)index * PAGE_SIZE;
			break;
		}
	}

	spin_unlock_irqrestore(&object->lock, irq);

	if (wanted > transfer - in_page)
		wanted = transfer - in_page;

	/*
	 * Borrows preparation storage without waiting inside the content read
	 * lease.
	 */
	scratch = NULL;
	capacity = 0;

	/*
	 * Borrows only when both halves of the pool are present, because a
	 * buffer that cannot be given back must not be taken.
	 */
	if (!through &&
	    io_pool_borrow != NULL &&
	    io_pool_release != NULL)
		scratch = io_pool_borrow(transfer, &capacity);

	prepared = 0;
	needed = (unsigned)((transfer + PAGE_SIZE - 1U) / PAGE_SIZE);
	publish = scratch != NULL &&
		capacity >= transfer &&
		needed <= VM_OBJECT_CACHE_PAGES;

	/* Allocates all private frames before any busy page is made visible. */
	while (publish && prepared < needed) {
		page = vm_object_alloc_page(object, base + (off_t)prepared * PAGE_SIZE, 1);
		if (page == NULL) {
			publish = 0;
			break;
		}

		pages[prepared++] = page;
	}

	/*
	 * Revalidates the complete publication set after allocation and racing
	 * faults.
	 */
	if (publish) {
		/* Refuses a run the file changed under, or a fault already filled. */
		irq = spin_lock_irqsave(&object->lock);

		if (object->size_generation != size_generation ||
		    object->content_generation != content_generation)
			publish = 0;

		for (index = 0; index < prepared && publish; index++) {
			if (vm_object_find_page(object, pages[index]->offset) != NULL)
				publish = 0;
		}

		/* Publishes the whole run as resident pages of the object. */
		if (publish) {
			for (index = 0; index < prepared; index++) {
				vm_object_page_index_insert(object, pages[index]);
				(void)atomic_fetch_add_relaxed(&object_pages, 1);
			}
		}

		spin_unlock_irqrestore(&object->lock, irq);
	}

	/*
	 * Returns unused preparation before using the ordinary leased backend
	 * path.
	 */
	if (!publish) {
		vm_object_cache_discard_prepared(pages, prepared);

		if (scratch != NULL)
			io_pool_release(scratch);

		/* A racing publication may now contain dirty shared data. */
		irq = spin_lock_irqsave(&object->lock);
		for (index = 0; (size_t)index * PAGE_SIZE < transfer; index++) {
			if (vm_object_find_page(object, base + (off_t)index * PAGE_SIZE) != NULL) {
				if (index == 0) {
					spin_unlock_irqrestore(&object->lock, irq);
					return 0;
				}

				if (wanted > (size_t)index * PAGE_SIZE - in_page)
					wanted = (size_t)index * PAGE_SIZE - in_page;
				break;
			}
		}

		spin_unlock_irqrestore(&object->lock, irq);

		/* Serves the read through the ordinary leased backend path. */
		error = file_io_begin(object->file, FILE_IO_PREAD, offset, FILE_IO_VM_OBJECT, &io);
		if (error != 0)
			return error;

		count = file_io_transfer(&io, buffer, wanted);

		file_io_end(&io);

		/* Reports what the backend returned. */
		*handled = 1;

		if (count < 0)
			return (int)-count;

		*result = count;
		*stop = (size_t)count < wanted;

		return 0;
	}

	/*
	 * Fills the run once; the explicit internal purpose prevents recursive
	 * caching.
	 */
	kern_memset(scratch, 0, transfer);
	error = file_io_begin(object->file, FILE_IO_PREAD, base, FILE_IO_VM_OBJECT, &io);
	if (error != 0) {
		count = -(ssize_t)error;
	} else {
		count = file_io_transfer(&io, scratch, transfer);
		file_io_end(&io);
	}

	/*
	 * Publishes only complete pages and keeps failed fills retryable for
	 * waiters.
	 */
	irq = spin_lock_irqsave(&object->lock);

	if (object->size_generation != size_generation ||
	    object->content_generation != content_generation)
		count = -EAGAIN;

	if (count > (ssize_t)transfer)
		count = -EIO;

	/* Fills each page from the staging, or marks it failed. */
	for (index = 0; index < prepared; index++) {
		page = pages[index];
		valid = transfer - (size_t)index * PAGE_SIZE;
		if (valid > PAGE_SIZE)
			valid = PAGE_SIZE;
		if (count >= (ssize_t)((size_t)index * PAGE_SIZE + valid)) {
			kern_memset(hal_pmem_to_kernel(page->pmem.paddr), 0, PAGE_SIZE);
			kern_memcpy_ram(hal_pmem_to_kernel(page->pmem.paddr), (uint8_t *)scratch +
			    (size_t)index * PAGE_SIZE, valid);
			page->flags = 0;
			page->error = 0;
		} else {
			page->flags = VM_OBJECT_PAGE_ERROR;
			page->error = count < 0 ? (int)-count : EIO;
		}
	}

	waitq_wake_all(&object->page_waitq);

	spin_unlock_irqrestore(&object->lock, irq);

	/*
	 * Preserves the confirmed short-read prefix rather than discarding it.
	 */
	copied = 0;
	if (count > (ssize_t)in_page) {
		copied = (size_t)count - in_page;
		if (copied > wanted)
			copied = wanted;
		kern_memcpy_ram(buffer, (uint8_t *)scratch + in_page, copied);
	}

	io_pool_release(scratch);

	*handled = 1;
	*result = (ssize_t)copied;
	*stop = count < (ssize_t)transfer;
	if (count < 0)
		return (int)-count;

	/*
	 * A short prefix ending before the requested offset cannot mean file
	 * EOF.
	 */
	if (count > 0 && (size_t)count <= in_page)
		return EIO;

	/*
	 * Reports either the complete selected run or the backend's short
	 * result.
	 */
	return 0;
}

/*
 * Reads an absent run of a file whose data the page cache reads itself
 * (vm_object_page_io_candidate), for a read shorter than the read-through
 * of the old way: the pages the read reaches, or a whole window of them for
 * a sequential reader, stopping at the end of file and at the first
 * resident page, into private frames, straight from the disk; copies the
 * caller's bytes out of them and then publishes them as resident pages.
 *
 * EOPNOTSUPP, with nothing done, sends the caller to the old way: no frame
 * could be had, or the file's data turned out not to be mapped.  When the
 * file changed while the run was read, nothing is reported handled and the
 * caller faults the page in instead.
 */
static int
cache_read_pages(
	struct vm_object *object,
	off_t offset,
	void *buffer,
	size_t length,
	off_t eof,
	uint64_t size_generation,
	uint64_t content_generation,
	ssize_t *result,
	int *handled)
{
	struct cache_read_run *run;
	struct vm_object_page *resident;
	struct vm_object_page *page;
	struct file_io io;
	off_t base;
	size_t in_page;
	size_t window;
	size_t reach;
	size_t wanted;
	size_t transfer;
	size_t copied;
	size_t chunk;
	unsigned needed;
	unsigned prepared;
	unsigned unpublished;
	unsigned index;
	unsigned long irq;
	int sequential;
	int error;

	/*
	 * A long read goes through the file, as the read-through of the old
	 * way does (the buffer cache and its read-ahead keep the device busy
	 * ahead of it); the page cache's own read-ahead does not yet run that
	 * far ahead of a long reader.
	 */
	if (length >= VM_OBJECT_READ_THROUGH_MIN)
		return EOPNOTSUPP;

	/* The bytes the read asks for, up to a window and the end of file. */
	base = offset & ~(off_t)(PAGE_SIZE - 1U);
	in_page = (size_t)(offset - base);
	window = (size_t)VM_OBJECT_READ_WINDOW_PAGES * PAGE_SIZE;
	wanted = window - in_page;
	if (wanted > length)
		wanted = length;
	if ((uint64_t)wanted > (uint64_t)(eof - offset))
		wanted = (size_t)(eof - offset);

	/*
	 * A read that starts the file or goes on from a resident page (a
	 * sequential reader) reads the whole window, so that the reads after
	 * it find their pages; any other reads only the pages it asks for.
	 */
	sequential = 0;
	if (base == 0)
		sequential = 1;
	if (base != 0) {
		irq = spin_lock_irqsave(&object->lock);

		resident = vm_object_find_page(object, base - (off_t)PAGE_SIZE);

		spin_unlock_irqrestore(&object->lock, irq);
		if (resident != NULL)
			sequential = 1;
	}

	/* The pages to read, up to the end of file and the first resident page. */
	reach = in_page + wanted;
	if (sequential)
		reach = window;
	if ((uint64_t)reach > (uint64_t)(eof - base))
		reach = (size_t)(eof - base);
	needed = (unsigned)((reach + PAGE_SIZE - 1U) / PAGE_SIZE);
	irq = spin_lock_irqsave(&object->lock);

	for (index = 1; index < needed; index++) {
		resident = vm_object_find_page(object, base + (off_t)index * PAGE_SIZE);
		if (resident != NULL) {
			needed = index;
			break;
		}
	}

	spin_unlock_irqrestore(&object->lock, irq);

	/* Takes private frames, as many as memory spares, without waiting. */
	run = kern_malloc(sizeof(*run));
	if (run == NULL)
		return EOPNOTSUPP;
	for (prepared = 0; prepared < needed; prepared++) {
		page = vm_object_alloc_page(object, base + (off_t)prepared * PAGE_SIZE, 1);
		if (page == NULL)
			break;
		run->pages[prepared] = page;
	}
	if (prepared == 0) {
		kern_free(run);
		return EOPNOTSUPP;
	}

	/* Reads the frames' part of the file. */
	transfer = (size_t)prepared * PAGE_SIZE;
	if ((uint64_t)transfer > (uint64_t)(eof - base))
		transfer = (size_t)(eof - base);
	if (wanted > transfer - in_page)
		wanted = transfer - in_page;
	error = file_io_begin(object->file, FILE_IO_PREAD, base, FILE_IO_VM_OBJECT, &io);
	if (error == 0) {
		error = vm_object_io_read_pages(object->inode, base, run->pages, prepared,
		    transfer, &io.context, vm_object_file_read_fallback, &io);
		file_io_end(&io);
	}
	if (error != 0) {
		vm_object_cache_discard_prepared(run->pages, prepared);
		kern_free(run);
		if (error == EOPNOTSUPP)
			return EOPNOTSUPP;
		*handled = 1;
		return error;
	}

	/* Copies the caller's bytes out while the frames are still private. */
	for (copied = 0; copied < wanted; copied += chunk) {
		index = (unsigned)((in_page + copied) / PAGE_SIZE);
		chunk = PAGE_SIZE - (in_page + copied) % PAGE_SIZE;
		if (chunk > wanted - copied)
			chunk = wanted - copied;
		kern_memcpy_ram((uint8_t *)buffer + copied,
		    (const uint8_t *)hal_pmem_to_kernel(run->pages[index]->pmem.paddr) +
		    (in_page + copied) % PAGE_SIZE, chunk);
	}

	/*
	 * Publishes the frames when the file is as it was sampled and no page
	 * took their place; a file that changed meanwhile leaves the bytes to
	 * a fault.
	 */
	irq = spin_lock_irqsave(&object->lock);
	if (object->size_generation != size_generation ||
	    object->content_generation != content_generation ||
	    (object->flags & (VM_OBJECT_RESIZING | VM_OBJECT_CONTENT)) != 0) {
		spin_unlock_irqrestore(&object->lock, irq);
		vm_object_cache_discard_prepared(run->pages, prepared);
		kern_free(run);
		return 0;
	}
	/*
	 * Publishes each frame as a resident, clean page where no other page
	 * (a fault's, a prefetch's) took its offset meanwhile, and keeps the
	 * others, in order, at the front of the run to be given back.
	 */
	unpublished = 0;
	for (index = 0; index < prepared; index++) {
		page = run->pages[index];
		resident = vm_object_find_page(object, page->offset);
		if (resident != NULL) {
			run->pages[unpublished++] = page;
			continue;
		}

		page->flags = 0;
		page->error = 0;
		vm_object_page_index_insert(object, page);
		(void)atomic_fetch_add_relaxed(&object_pages, 1);
	}
	waitq_wake_all(&object->page_waitq);

	spin_unlock_irqrestore(&object->lock, irq);

	/* Gives back the frames that another page beat to their offsets. */
	vm_object_cache_discard_prepared(run->pages, unpublished);
	kern_free(run);

	/* Reports the bytes copied out. */
	*handled = 1;
	*result = (ssize_t)wanted;
	return 0;
}

/* A retained file may still be shared with a descriptor outside the VM cache. */
static int
object_discard_file_check(
	struct vm_object *object,
	struct mount *mount,
	struct file *file)
{
	unsigned owned;

	if (file == NULL)
		return 0;

	/* Only ordinary paths on this exact mount have proven disposal ownership. */
	if (object->inode == NULL)
		return EBUSY;
	if (object->inode->i_mount != mount)
		return EBUSY;
	if (file->f_path.p_mount != mount)
		return EBUSY;
	if (file->f_path.p_inode != object->inode)
		return EBUSY;
	if (file->f_inode != object->inode)
		return EBUSY;
	if (file->f_vm_inode != NULL && file->f_vm_inode != object->inode)
		return EBUSY;

	/* Two slots can hold one description, but it still owns only one path. */
	owned = 0;
	if (object->file == file)
		owned++;
	if (object->write_file == file)
		owned++;
	if (refcount_load(&file->f_refs) != owned)
		return EBUSY;

	return 0;
}

/* Checks ownership without clearing dirty/error state or detaching any object. */
static int
object_discard_mount_check_locked(struct mount *mount)
{
	struct vm_object *object;
	struct vm_object_page *page;
	unsigned long irq;
	unsigned list;
	int error;

	error = 0;
	for (object = shared_objects; object != NULL; object = object->next) {
		if ((object->inode == NULL || object->inode->i_mount != mount) &&
		    (object->file == NULL || object->file->f_path.p_mount != mount) &&
		    (object->write_file == NULL || object->write_file->f_path.p_mount != mount))
			continue;
		irq = spin_lock_irqsave(&object->lock);
		if ((object->flags & (VM_OBJECT_CACHE_REFERENCE | VM_OBJECT_RETAINED_WRITEBACK)) == 0 ||
		    (object->flags & (VM_OBJECT_DETACHING | VM_OBJECT_RESIZING |
		    VM_OBJECT_CONTENT | VM_OBJECT_ANONYMOUS)) != 0 ||
		    refcount_load(&object->refs) != 1 || object->mapping_count != 0 ||
		    object->active_operations != 0 || object->registry_waiters != 0)
			error = EBUSY;
		if (error == 0)
			error = object_discard_file_check(object, mount, object->file);
		if (error == 0)
			error = object_discard_file_check(object, mount, object->write_file);
		/* Refuses a mount whose resident pages or orphans anything still uses. */
		for (list = 0; list < 2 && error == 0; list++) {
			if (list == 0)
				page = vm_object_page_at_or_after(object, 0);
			else
				page = object->orphan_pages;
			while (page != NULL) {
				if ((page->flags & (VM_OBJECT_PAGE_BUSY | VM_OBJECT_PAGE_WRITEBACK)) != 0 ||
				    page->hold_count != 0 || page->pin_count != 0 ||
				    page->mapping_count != 0 || page->mappings != NULL) {
					error = EBUSY;
					break;
				}

				if (list == 0)
					page = vm_object_page_after(object, page);
				else
					page = page->next;
			}
		}
		spin_unlock_irqrestore(&object->lock, irq);
		if (error != 0)
			break;
	}
	return error;
}

/*
 * Tests, under the registry lock, whether a cache-only object is idle: only
 * the registry holds it, and it has no mapping, operation, waiter, dirty or
 * busy page, failure or transition, on a mount that is not being removed.
 */
static int
object_cache_idle_locked(
	struct vm_object *object)
{
	unsigned refs;
	int reserved;
	int clean;

	/* An object on a mount that unmount has closed is unmount's. */
	reserved = vm_object_mount_reserved(object);
	if (reserved)
		return 0;

	/* Only the cache reference may remain, with nothing in progress. */
	refs = refcount_load(&object->refs);
	if ((object->flags & VM_OBJECT_CACHE_REFERENCE) == 0 ||
	    (object->flags & (VM_OBJECT_DETACHING | VM_OBJECT_RESIZING |
	    VM_OBJECT_CONTENT | VM_OBJECT_RETAINED_WRITEBACK)) != 0 ||
	    object->active_operations != 0 || object->registry_waiters != 0 ||
	    refs != 1)
		return 0;

	/* Reports whether every page is clean and idle. */
	clean = vm_object_can_destroy(object);
	return clean;
}

/*
 * Takes the idle clean pages of an object out of its index, the lowest
 * offset first, until freed reaches target; the caller holds the object
 * lock and frees the pages it is handed on retired after dropping it.
 * Reports the bytes freed so far, including what freed already held.
 */
static size_t
object_detach_idle_pages_locked(
	struct vm_object *object,
	size_t freed,
	size_t target,
	struct vm_object_page **retired)
{
	struct vm_object_page *page;
	struct vm_object_page *next;
	int idle;

	/* Walks the index in offset order, finding each next page before this one may leave. */
	page = vm_object_page_at_or_after(object, 0);
	while (page != NULL && freed < target) {
		next = vm_object_page_after(object, page);

		/* Leaves a page that is dirty, busy, mapped, held or pinned. */
		idle = object_page_idle_clean(page);
		if (!idle) {
			page = next;
			continue;
		}

		/* Moves the page to the caller's batch of pages to free. */
		vm_object_page_index_remove(object, page);
		page->next = *retired;
		*retired = page;
		freed += page->pmem.size;
		page = next;
	}

	/* Reports the running total. */
	return freed;
}

/* Tests whether a resident page may be dropped at once: clean, idle and referenced by nothing. */
static int
object_page_idle_clean(
	const struct vm_object_page *page)
{
	/* Any state other than a remembered error means someone owns or needs the page. */
	if ((page->flags & ~VM_OBJECT_PAGE_ERROR) != 0)
		return 0;

	/* So does a mapping, a hold or a pin. */
	if (page->mapping_count != 0 || page->hold_count != 0)
		return 0;
	if (page->pin_count != 0)
		return 0;

	/* Reports a page nothing refers to. */
	return 1;
}

/*
 * Tests, under the registry lock, whether the clean pages of an object may
 * be dropped without asking anyone: only the registry holds the object, on
 * a mount unmount has not closed, and no transaction, mapping, operation or
 * waiter is on it.
 */
static int
object_reclaimable_locked(
	struct vm_object *object)
{
	int reserved;

	/* An object on a mount that unmount has closed is unmount's. */
	reserved = vm_object_mount_reserved(object);
	if (reserved)
		return 0;

	/* Only a cache object can be emptied behind its users' backs. */
	if ((object->flags & VM_OBJECT_CACHE_REFERENCE) == 0)
		return 0;

	/* A detach, a resize, a content change or a retained write-back owns it. */
	if ((object->flags & (VM_OBJECT_DETACHING | VM_OBJECT_RESIZING |
	    VM_OBJECT_CONTENT | VM_OBJECT_RETAINED_WRITEBACK)) != 0)
		return 0;

	/* So does a mapping, an operation in progress or a lookup waiting on it. */
	if (object->mapping_count != 0 || object->active_operations != 0)
		return 0;
	if (object->registry_waiters != 0)
		return 0;

	/* So does any reference besides the registry's. */
	if (refcount_load(&object->refs) != 1)
		return 0;

	/* Reports an object the registry alone holds. */
	return 1;
}
