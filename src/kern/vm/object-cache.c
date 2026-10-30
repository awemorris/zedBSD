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
static int object_discard_mount_check_locked(struct mount *mount);
static int object_discard_file_check(struct vm_object *, struct mount *, struct file *);

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
	struct vm_object_page **link;
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
	link = &object->pages;
	while (*link != NULL && freed < target) {
		page = *link;
		if ((page->flags & ~VM_OBJECT_PAGE_ERROR) != 0 ||
		    page->mapping_count != 0 || page->hold_count != 0 ||
		    page->pin_count != 0) {
			link = &page->next;
			continue;
		}

		*link = page->next;
		vm_object_page_index_remove(object, page);
		page->next = retired;
		retired = page;
		freed += page->pmem.size;
	}
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
		for (list = 0; list < 2; list++) {
			page = list == 0 ? object->pages : object->orphan_pages;
			for (; page != NULL; page = page->next) {
				if (page->flags & VM_OBJECT_PAGE_DIRTY)
					discarded += PAGE_SIZE;
				vm_object_clear_page_dirty_locked(page);
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
	struct vm_object_page **link;
	struct vm_object_page *retired;
	size_t freed;
	unsigned long irq;
	bool enabled;

	/* Bounds one pressure pass and excludes every concurrent metadata user. */
	if (target > KERN_IO_BATCH_MAX)
		target = KERN_IO_BATCH_MAX;
	retired = NULL;
	freed = 0;
	enabled = vm_object_registry_lock();
	for (object = shared_objects; object != NULL && freed < target;
	     object = object->next) {
		if (vm_object_mount_reserved(object))
			continue;
		/* Skips an object anything still owns. */
		if ((object->flags & VM_OBJECT_CACHE_REFERENCE) == 0 ||
		    (object->flags & (VM_OBJECT_DETACHING | VM_OBJECT_RESIZING |
		    VM_OBJECT_CONTENT | VM_OBJECT_RETAINED_WRITEBACK)) != 0 ||
		    object->mapping_count != 0 || object->active_operations != 0 ||
		    object->registry_waiters != 0 || refcount_load(&object->refs) != 1)
			continue;

		/* Detaches every clean, unreferenced page of the object. */
		irq = spin_lock_irqsave(&object->lock);
		link = &object->pages;
		while (*link != NULL && freed < target) {
			page = *link;
			if ((page->flags & ~VM_OBJECT_PAGE_ERROR) != 0 ||
			    page->mapping_count != 0 || page->hold_count != 0 ||
			    page->pin_count != 0) {
				link = &page->next;
				continue;
			}

			*link = page->next;
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

		/* Looks for a reclaimable page of this object. */
		irq = spin_lock_irqsave(&object->lock);
		for (page = object->pages; page != NULL; page = page->next) {
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
				pages[index]->next = object->pages;
				object->pages = pages[index];
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
		for (list = 0; list < 2 && error == 0; list++) {
			page = list == 0 ? object->pages : object->orphan_pages;
			for (; page != NULL; page = page->next) {
				if ((page->flags & (VM_OBJECT_PAGE_BUSY | VM_OBJECT_PAGE_WRITEBACK)) != 0 ||
				    page->hold_count != 0 || page->pin_count != 0 ||
				    page->mapping_count != 0 || page->mappings != NULL) {
					error = EBUSY;
					break;
				}
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
