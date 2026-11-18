/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The registry of shared objects and their lifetime: admission, lookup by
 * inode, references, generations and destruction.
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
 * The registry of shared objects, in most-recently-published order.
 *
 * A file-backed object lives here for as long as any mapping, cache
 * reference or write-back holds it, so a second mapping of the same inode
 * finds the object that already exists.
 */
struct vm_object *shared_objects VM_OBJECT_DATA;

/*
 * The registry's objects again, chained by the inode they cache.
 *
 * A file read or fault finds its inode's object here instead of walking
 * every published object with interrupts disabled.  The chains hold
 * exactly the objects of shared_objects, and change under the same lock.
 */
struct vm_object *object_hash[VM_OBJECT_HASH_BUCKETS] VM_OBJECT_DATA;

/*
 * How many objects the registry holds.
 */
unsigned object_count VM_OBJECT_DATA;

/*
 * The generation stamped on the object published next.
 *
 * It only ever increases, so a stale reference to a slot that has been
 * reused is recognized by the generation that came with it.
 */
uint64_t object_registry_generation VM_OBJECT_DATA;

/*
 * The use count stamped on an object each time it is admitted or referenced.
 *
 * It only ever increases under the registry lock, so the cache object with
 * the lowest stamp is the least recently used one.
 */
uint64_t object_use_generation VM_OBJECT_DATA;

/*
 * How many registry entries exist only because the page cache asked for them.
 *
 * These are bounded by VM_OBJECT_CACHE_OBJECTS: an optional admission is
 * refused once that many are already held.
 */
unsigned cache_objects VM_OBJECT_DATA;

/*
 * How many pages every object holds together.
 */
atomic_uint_t object_pages VM_OBJECT_DATA;

/*
 * The spin word that guards the registry above.
 *
 * It is taken raw rather than as a spinlock because the registry is walked
 * from contexts that must not block.
 */
static atomic_uint_t object_registry_lock VM_OBJECT_DATA;

/*
 * Optional collaborators.
 *
 * Each of these lives in a translation unit that some platforms leave out of
 * the kernel, so the reference is weak and every call site tests it first.
 */
extern bool hal_irq_disable(void) __attribute__((weak));
extern void hal_irq_enable(void) __attribute__((weak));

static int object_wait_registry_sequence(struct vm_object *object, uint64_t sequence);
static void object_wait_registry_waiters(struct vm_object *object);
static int object_reference_locked(struct vm_object *object, struct file *file, int cache_only);

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

	/* Rejects files without an authoritative content inode. */
	inode = file_vm_inode(file);
	if (inode == NULL)
		return EINVAL;

	/* Keeps claim preparation out until the shared object becomes visible. */
	error = backing_mutation_begin_inode(inode, &guard);
	if (error != 0)
		return error;

	/* Publishes the mapping before releasing the admission reservation. */
	error = vm_object_get_shared_internal(file, result, 0);
	backing_mutation_end(&guard);

	/* Reports why the object could not be admitted. */
	if (error != 0)
		return error;

	/* Succeeded: the caller now holds the shared object. */
	return 0;
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
	enabled = vm_object_registry_lock();
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

	vm_object_registry_unlock(enabled);

	/* Resolves sleeping FAT identities after dropping the registry spinlock. */
	for (index = 0; index < count && error == 0; index++) {
		/* Includes retained caches so stale writeback cannot cross formatting. */
		error = backing_claim_inode_matches(claim, inodes[index], &matched);
		if (error == 0 && matched)
			error = EBUSY;
	}

	/* Releases every inode even when an earlier comparison failed. */
	for (index = 0; index < count; index++)
		inode_release(inodes[index]);
	kern_free(inodes);

	/* Reports the alias that refused the admission. */
	if (error != 0)
		return error;

	/* Succeeded: no cached object aliases a retained claim. */
	return 0;
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

	/* Starts the object mapped once, empty, and at the first generation. */
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
	enabled = vm_object_registry_lock();
	if (object->mapping_count == 0)
		HAL_FATAL("referencing detached VM object");
	object->mapping_count++;
	refcount_get(&object->refs);
	vm_object_registry_unlock(enabled);
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
	struct file *write_file;
	int error;
	int removed;
	int anonymous;
	bool enabled;
	uint64_t sequence;
	unsigned long irq;
	int clean;
	int dirty;
	int busy;
	int destroyable;

	/* Starts with nothing unlinked and no writer to close. */
	removed = 0;
	write_file = NULL;

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
	enabled = vm_object_registry_lock();
	if (object->mapping_count == 0)
		HAL_FATAL("VM object mapping reference underflow");

	/* Waits for the operations in flight before releasing the last mapping. */
	if (object->mapping_count == 1 && object->active_operations != 0) {
		sequence = waitq_sequence(&object->registry_waitq);
		vm_object_registry_unlock(enabled);
		error = object_wait_registry_sequence(object, sequence);
		if (error != 0 && error != EAGAIN)
			HAL_FATAL("VM object operation drain wait failed");
		goto retry_mapping;
	}

	/* An ordinary drop leaves the object mapped and alive. */
	object->mapping_count--;
	if (object->mapping_count != 0) {
		vm_object_registry_unlock(enabled);
		if (refcount_put(&object->refs))
			HAL_FATAL("mapped VM object lost registry reference");
		return;
	}

	if ((object->flags & VM_OBJECT_DETACHING) != 0)
		HAL_FATAL("VM object entered final teardown twice");

	/* With the last mapping gone, no mapping can store into a page. */
	irq = spin_lock_irqsave(&object->lock);
	object->store_mapped = 0;
	spin_unlock_irqrestore(&object->lock, irq);

	/*
	 * Reclassify the last mapping reference as an explicit teardown hold.
	 * Registry lookup may now wait without either reviving this object
	 * during writeback or racing its destruction.
	 */
	object->flags |= VM_OBJECT_DETACHING;
	refcount_get(&object->refs);
	vm_object_registry_unlock(enabled);
	if (refcount_put(&object->refs))
		HAL_FATAL("VM object teardown lost registry reference");

	/*
	 * DETACHING excludes new mappings, so final writeback does not need
	 * the cross-vm metadata lock which a waiting mapper may already hold.
	 * An object with nothing dirty and nothing in flight has nothing to
	 * write back, and the walk over its pages is skipped: a short process
	 * that only read a large library would otherwise pay for every page.
	 */
	irq = spin_lock_irqsave(&object->lock);
	dirty = vm_object_has_dirty_pages_locked(object);
	busy = vm_object_has_busy_pages_locked(object);
	spin_unlock_irqrestore(&object->lock, irq);
	clean = 1;
	if (dirty || busy)
		clean = 0;
	error = 0;
	if (!clean)
		error = vm_object_sync_range_internal(object, 0, SIZE_MAX, MS_SYNC, 1, 0, 0);

	/*
	 * A file object that its last mapping is leaving stays as a cache
	 * object, so the next process that maps the same file finds its pages;
	 * the least recently used cache object makes room when the cache is
	 * full.  An object without a read handle cannot be kept.
	 */
	if (!anonymous && error == 0 &&
	    (object->flags & VM_OBJECT_CACHE_REFERENCE) == 0 &&
	    object->file != NULL &&
	    cache_objects >= VM_OBJECT_CACHE_OBJECTS)
		(void)vm_object_cache_evict_one(NULL);

	/* Unlinks a clean object, or retains one that still has dirty pages. */
	enabled = vm_object_registry_lock();
	if (object->mapping_count != 0 ||
	    (object->flags & VM_OBJECT_DETACHING) == 0)
		HAL_FATAL("VM object teardown state changed during writeback");

	/* Admits the leaving object to the cache when there is room for it. */
	destroyable = vm_object_can_destroy(object);
	if (!anonymous && error == 0 &&
	    (object->flags & VM_OBJECT_CACHE_REFERENCE) == 0 &&
	    object->file != NULL &&
	    cache_objects < VM_OBJECT_CACHE_OBJECTS &&
	    destroyable && !vm_object_inode_unnamed(object->inode)) {
		object->flags |= VM_OBJECT_CACHE_REFERENCE;
		cache_objects++;
		object->last_use = ++object_use_generation;
	}
	if (!anonymous && error == 0 && vm_object_cache_retainable(object)) {
		object->flags &= ~VM_OBJECT_DETACHING;
		write_file = object->write_file;
		object->write_file = NULL;
	} else if (error == 0 && vm_object_can_destroy(object)) {
		if (anonymous) {
			removed = 1;
		} else {
			removed = vm_object_unlink_locked(object);
			if (!removed)
				HAL_FATAL("detaching VM object left registry unexpectedly");
		}
	} else {
		if (anonymous)
			HAL_FATAL("anonymous VM object teardown could not complete");
		vm_object_retain(object, error);
		object->flags &= ~VM_OBJECT_DETACHING;
	}

	vm_object_registry_unlock(enabled);

	/* Releases the waiters, and drains them before the object disappears. */
	vm_object_wake_registry_waiters(object);
	if (removed)
		object_wait_registry_waiters(object);
	if (removed && !anonymous && refcount_put(&object->refs))
		HAL_FATAL("VM object registry reference was last unexpectedly");

	/* Closes the former mapping writer outside all registry and object locks. */
	if (write_file != NULL)
		(void)file_close(write_file);

	/* Drop the explicit teardown lifetime reference. */
	if (refcount_put(&object->refs))
		vm_object_destroy(object);
}

/*
 * Finds or creates the admitted shared object, taking a mapping reference.
 *
 * A writable file becomes the object's writeback file when it has none.
 * An object in final teardown is waited for and the lookup retried.
 */
int
vm_object_get_shared_internal(
	struct file *file,
	struct vm_object **result,
	int cache_only)
{
	struct vm_object *object;
	struct inode *inode;
	int error;
	int writable;
	bool enabled;
	uint64_t sequence;
	struct vm_object *existing;
	unsigned bucket;

	/* Rejects a missing file or result, or a file without an inode. */
	if (file == NULL || result == NULL)
		return EINVAL;

	inode = file_vm_inode(file);
	if (inode == NULL)
		return EINVAL;

	/* Notes whether this handle could serve as the object's writer. */
	writable = 0;
	if (!cache_only &&
	    (file_status_flags_get(file) & O_ACCMODE) != O_RDONLY &&
	    file->f_ops != NULL &&
	    file->f_ops->pwrite != NULL)
		writable = 1;

retry_lookup:
	/* Joins a published object, waiting out one that is detaching. */
	enabled = vm_object_registry_lock();
	for (object = object_hash[vm_object_hash_index(inode)]; object != NULL;
	     object = object->hash_next) {
		if (object->inode != inode)
			continue;
		if ((object->flags & VM_OBJECT_DETACHING) != 0) {
			sequence = waitq_sequence(&object->registry_waitq);

			/* Pins an object which may be unlinked before wakeup. */
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

		error = object_reference_locked(object, file, cache_only);
		if (error == 0)
			*result = object;

		vm_object_registry_unlock(enabled);

		return error;
	}

	vm_object_registry_unlock(enabled);

	/* Builds a new object holding the registry and the caller's references. */
	object = kern_calloc(1, sizeof(*object));
	if (object == NULL)
		return ENOMEM;

	/* Starts at the first generation, holding the registry and the caller. */
	refcount_init(&object->refs, 2);
	object->mapping_count = cache_only ? 0 : 1;
	spin_init(&object->lock, LOCK_RANK_VM_OBJECT, "VM object");
	waitq_init(&object->page_waitq, "VM object page");
	waitq_init(&object->registry_waitq, "VM object registry");
	object->generation = 1;
	object->inode = inode;

	/* An optional cache opens its own read handle on the same inode. */
	if (cache_only) {
		if (file->f_path.p_inode == NULL) {
			vm_object_destroy(object);
			return EOPNOTSUPP;
		}

		error = file_open_resolved(&file->f_path, O_RDONLY | O_NOFOLLOW, &object->file);
		if (error != 0) {
			vm_object_destroy(object);
			return error;
		}

		/* Restarts when the handle no longer names the inode it did. */
		if (file_vm_inode(object->file) != inode) {
			vm_object_destroy(object);
			return EAGAIN;
		}
	} else {
		object->file = file;
		file_ref(file);
	}

	/* Keeps the caller's writer when the object may have to flush pages. */
	if (writable) {
		object->write_file = file;
		file_ref(file);
	}

	/* Another CPU may have published the inode while allocation slept. */
	enabled = vm_object_registry_lock();
	for (existing = object_hash[vm_object_hash_index(inode)]; existing != NULL;
	     existing = existing->hash_next) {
		if (existing->inode != inode)
			continue;

		if ((existing->flags & VM_OBJECT_DETACHING) != 0) {
			sequence = waitq_sequence(&existing->registry_waitq);
			existing->registry_waiters++;
			if (existing->registry_waiters == 0)
				HAL_FATAL("VM object registry waiter overflow");

			/* Waits out the transition the existing object is in, then looks again. */
			refcount_get(&existing->refs);
			vm_object_registry_unlock(enabled);
			vm_object_destroy(object);
			error = vm_object_wait_registry_transition(existing, sequence);
			if (error != 0 && error != EAGAIN)
				return error;

			goto retry_lookup;
		}

		error = object_reference_locked(existing, file, cache_only);
		vm_object_registry_unlock(enabled);
		vm_object_destroy(object);

		if (error == 0)
			*result = existing;

		return error;
	}

	/* Refuses optional admission before publishing any registry ownership. */
	if (cache_only && cache_objects == VM_OBJECT_CACHE_OBJECTS) {
		vm_object_registry_unlock(enabled);
		vm_object_destroy(object);
		return ENOMEM;
	}

	/* Refuses an unrepresentable registry identity before publication. */
	if (object_registry_generation == UINT64_MAX) {
		vm_object_registry_unlock(enabled);
		vm_object_destroy(object);
		return EOVERFLOW;
	}

	object->registry_generation = ++object_registry_generation;
	object->last_use = ++object_use_generation;

	/* Publishes the new object with the inode's current end of file. */
	vm_object_initialize_eof_locked(object, inode);
	if (cache_only) {
		object->flags |= VM_OBJECT_CACHE_REFERENCE;
		object->active_operations++;
		cache_objects++;
	}

	/* Links the object into the registry list. */
	bucket = vm_object_hash_index(inode);
	object->previous = NULL;
	object->next = shared_objects;
	if (shared_objects != NULL)
		shared_objects->previous = object;
	shared_objects = object;

	/* Links it into its inode's hash chain. */
	object->hash_next = object_hash[bucket];
	object_hash[bucket] = object;
	object->flags |= VM_OBJECT_REGISTERED;
	object_count++;
	vm_object_registry_unlock(enabled);
	*result = object;

	/* Reports the new object. */
	return 0;
}

/*
 * Takes the registry lock with interrupts disabled, reporting whether they
 * were enabled.
 */
bool
vm_object_registry_lock(
	void)
{
	bool enabled;

	/* Disables interrupts where the HAL provides the routine. */
	if (hal_irq_disable != NULL)
		enabled = hal_irq_disable();
	else
		enabled = false;

	/* Spins until the registry is this owner's. */
	while (!atomic_try_acquire_zero(&object_registry_lock))
		hal_compiler_barrier();

	/* Reports the interrupt state the unlock has to restore. */
	return enabled;
}

/*
 * Releases the registry lock and restores the interrupt state.
 */
void
vm_object_registry_unlock(
	bool enabled)
{
	atomic_store_release(&object_registry_lock, 0);
	if (enabled && hal_irq_enable != NULL)
		hal_irq_enable();
}

/*
 * Finds the object of an inode; the caller holds the registry lock.
 */
struct vm_object *
vm_object_find_by_inode_locked(
	struct inode *inode)
{
	struct vm_object *object;

	/* Walks the inode's hash chain to its object. */
	for (object = object_hash[vm_object_hash_index(inode)]; object != NULL;
	     object = object->hash_next) {
		if (object->inode == inode)
			return object;
	}

	/* Reports an inode the registry holds no object for. */
	return NULL;
}

/*
 * Waits for a teardown transition, then drops the waiter's pin taken under
 * the registry lock.
 */
int
vm_object_wait_registry_transition(
	struct vm_object *object,
	uint64_t sequence)
{
	int error;
	int wake;
	bool enabled;

	error = object_wait_registry_sequence(object, sequence);
	enabled = vm_object_registry_lock();
	if (object->registry_waiters == 0)
		HAL_FATAL("VM object registry waiter counter underflow");

	/* Keeps the waiter's lifetime pin through its final wakeup. */
	object->registry_waiters--;
	wake = object->registry_waiters == 0;
	vm_object_registry_unlock(enabled);

	if (wake)
		vm_object_wake_registry_waiters(object);
	if (refcount_put(&object->refs))
		vm_object_destroy(object);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Wakes everyone waiting on an object's registry state.
 */
void
vm_object_wake_registry_waiters(
	struct vm_object *object)
{
	unsigned long irq;

	irq = spin_lock_irqsave(&object->lock);

	waitq_wake_all(&object->registry_waitq);

	spin_unlock_irqrestore(&object->lock, irq);
}

/*
 * Counts an operation on an object that is not detaching.
 */
int
vm_object_operation_begin(
	struct vm_object *object)
{
	bool enabled;

	/* Refuses to start work on an object that is tearing down. */
	enabled = vm_object_registry_lock();
	if ((object->flags & VM_OBJECT_DETACHING) != 0) {
		vm_object_registry_unlock(enabled);
		return EBUSY;
	}

	/* Counts this operation so that teardown waits for it. */
	object->active_operations++;
	if (object->active_operations == 0)
		HAL_FATAL("VM object operation counter overflow");
	vm_object_registry_unlock(enabled);

	/* Reports the started operation. */
	return 0;
}

/*
 * Counts an operation down, waking a final put waiting for the last.
 */
void
vm_object_operation_end(
	struct vm_object *object)
{
	int wake;
	bool enabled;

	/* Drops the operation, pinning the object when it was the last one. */
	enabled = vm_object_registry_lock();
	if (object->active_operations == 0)
		HAL_FATAL("VM object operation counter underflow");

	object->active_operations--;

	wake = object->active_operations == 0;
	if (wake)
		refcount_get(&object->refs);

	vm_object_registry_unlock(enabled);

	/* Releases whoever waited for the object to fall idle. */
	if (wake) {
		vm_object_wake_registry_waiters(object);
		if (refcount_put(&object->refs))
			vm_object_destroy(object);
	}
}

/*
 * Advances an object's generation, skipping zero; the caller holds the
 * object lock.
 */
uint64_t
vm_object_next_generation(
	struct vm_object *object)
{
	object->generation++;

	if (object->generation == 0)
		object->generation++;

	return object->generation;
}

/*
 * Advances an inode's resize generation, skipping zero.
 */
uint64_t
vm_object_next_inode_resize_generation_locked(
	struct inode *inode)
{
	inode->i_vm_resize_generation++;

	if (inode->i_vm_resize_generation == 0)
		inode->i_vm_resize_generation++;

	return inode->i_vm_resize_generation;
}

/*
 * Advances an inode's content generation, skipping zero.
 */
uint64_t
vm_object_next_inode_content_generation_locked(
	struct inode *inode)
{
	inode->i_vm_content_generation++;

	if (inode->i_vm_content_generation == 0)
		inode->i_vm_content_generation++;

	return inode->i_vm_content_generation;
}

/*
 * Tests whether an unmapped object is clean and idle enough to destroy.
 */
int
vm_object_can_destroy(
	struct vm_object *object)
{
	unsigned long irq;
	int result;

	/* A mapped object, or one holding a failure, is never destroyable. */
	if (object->mapping_count != 0 || object->writeback_error != 0)
		return 0;

	/* Requires every page to be clean and idle. */
	irq = spin_lock_irqsave(&object->lock);

	result = !vm_object_has_dirty_pages_locked(object) && !vm_object_has_busy_pages_locked(object);

	spin_unlock_irqrestore(&object->lock, irq);

	/* Reports whether the object may be destroyed. */
	return result;
}

/*
 * Removes a destroyable object from the registry; the caller holds the
 * registry lock.
 */
int
vm_object_unlink_locked(
	struct vm_object *object)
{
	struct vm_object **link;
	unsigned bucket;

	/* Refuses to unlink an object that still owns work. */
	if (!vm_object_can_destroy(object))
		HAL_FATAL("destroying unsynchronized VM object");

	/* Reports an object the registry does not hold. */
	if ((object->flags & VM_OBJECT_REGISTERED) == 0)
		return 0;

	/* Splices the object out of the registry list. */
	if (object->previous != NULL)
		object->previous->next = object->next;
	else
		shared_objects = object->next;
	if (object->next != NULL)
		object->next->previous = object->previous;
	object->next = NULL;
	object->previous = NULL;

	/* Finds the link in the inode's hash chain that holds the object. */
	bucket = vm_object_hash_index(object->inode);
	link = &object_hash[bucket];
	while (*link != NULL && *link != object)
		link = &(*link)->hash_next;
	if (*link == NULL)
		HAL_FATAL("VM object missing from its hash chain");

	/* Splices it out of the chain. */
	*link = object->hash_next;
	object->hash_next = NULL;
	object->flags &= ~VM_OBJECT_REGISTERED;

	/* Discharges the object from the registry counters. */
	if (object_count == 0)
		HAL_FATAL("VM object counter underflow");
	object_count--;
	if ((object->flags & VM_OBJECT_CACHE_REFERENCE) != 0) {
		if (cache_objects == 0)
			HAL_FATAL("VM cache reference counter underflow");
		cache_objects--;
		object->flags &= ~VM_OBJECT_CACHE_REFERENCE;
	}

	/* Reports the removal. */
	return 1;
}

/*
 * Names the registry hash bucket of an inode.
 *
 * Inodes are heap records at least 16 bytes apart, so the low bits carry
 * nothing; the rest is spread by Knuth's multiplicative constant.
 */
unsigned
vm_object_hash_index(
	const struct inode *inode)
{
	uint32_t key;
	uint32_t spread;

	/* Drops the alignment bits of the address. */
	key = (uint32_t)((uintptr_t)inode >> 4);

	/* Takes the top bits of the product as the bucket. */
	spread = key * 2654435761U;
	return (unsigned)(spread >> (32U - VM_OBJECT_HASH_BITS));
}

/*
 * Frees an object, its pages, its files, and its commitment.
 */
void
vm_object_destroy(
	struct vm_object *object)
{
	struct vm_object_page *page;

	/*
	 * Frees the live pages, taking each out of the page index (and so off
	 * the clock) and out of the dirty index first.
	 */
	page = object->page_index;
	while (page != NULL) {
		vm_object_page_index_remove(object, page);
		vm_object_page_dirty_unlink(page);
		vm_object_free_page(page);
		page = object->page_index;
	}

	/* Frees the pages a resize orphaned. */
	page = object->orphan_pages;
	while (page != NULL) {
		object->orphan_pages = page->next;
		vm_object_free_page(page);
		page = object->orphan_pages;
	}

	/* Closes the files the object held. */
	if (object->file != NULL)
		(void)file_close(object->file);
	if (object->write_file != NULL)
		(void)file_close(object->write_file);

	/* Gives the commitment back and frees the record. */
	if (object->commit_size != 0)
		vm_commit_release(object->commit_size);
	kern_free(object);
}

/*
 * Keeps an unmapped object in the registry until its writeback succeeds.
 */
void
vm_object_retain(
	struct vm_object *object,
	int error)
{
	unsigned long irq;

	/* Only an unmapped object is retained. */
	if (object->mapping_count != 0)
		HAL_FATAL("retaining referenced VM object");

	/* Names a reason when the caller had none. */
	if (error == 0) {
		if (vm_object_has_busy_pages(object))
			error = EBUSY;
		else
			error = EIO;
	}

	/* Records the failure and marks the object as retained. */
	irq = spin_lock_irqsave(&object->lock);

	vm_object_record_writeback_error_locked(object, error);

	spin_unlock_irqrestore(&object->lock, irq);

	object->flags |= VM_OBJECT_RETAINED_WRITEBACK;
}

/*
 * Tests the bounded clean retention policy under the registry lock.
 */
int
vm_object_cache_retainable(
	struct vm_object *object)
{
	int retainable;

	/*
	 * Shared physical credits now bound clean retention across all objects.
	 */
	if ((object->flags & VM_OBJECT_CACHE_REFERENCE) == 0)
		return 0;

	retainable = vm_object_can_destroy(object);

	return retainable;
}

/*
 * Tests whether a file has lost its last name.  Its storage goes back when
 * the last holder lets go, so an optional cache of it must not be one.
 */
int
vm_object_inode_unnamed(
	const struct inode *inode)
{
	/* An object without an inode names nothing to keep. */
	if (inode == NULL)
		return 0;

	/*
	 * Reports a removed inode.  The filesystems mark the last name's
	 * removal DEAD; the link count is not kept by all of them (FAT).
	 */
	return (inode->i_flags & INODE_DEAD) != 0;
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

	error = waitq_sleep(&object->registry_waitq, &object->lock, sequence, 0, 0);

	spin_unlock_irqrestore(&object->lock, irq);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Waits until no registry waiter still pins an unlinked object. */
static void
object_wait_registry_waiters(
	struct vm_object *object)
{
	uint64_t sequence;
	int error;
	bool enabled;

	/* Sleeps until the object has no registry waiter left. */
	for (;;) {
		/* Returns as soon as the waiter count reaches zero. */
		enabled = vm_object_registry_lock();
		if (object->registry_waiters == 0) {
			vm_object_registry_unlock(enabled);
			return;
		}

		/* Waits for the registry to report progress on this object. */
		sequence = waitq_sequence(&object->registry_waitq);
		vm_object_registry_unlock(enabled);
		error = object_wait_registry_sequence(object, sequence);
		if (error != 0 && error != EAGAIN)
			HAL_FATAL("VM object registry waiter drain failed");
	}
}

/* Adds an operation or mapping reference while the registry lock is held. */
static int
object_reference_locked(
	struct vm_object *object,
	struct file *file,
	int cache_only)
{
	/*
	 * Existing mapping objects keep their original close and retention
	 * policy.
	 */

	/*
	 * Adopts this handle as the one shared dirty pages are written back
	 * through.  That happens only when all of the following hold:
	 *
	 * - the caller is taking a mapping rather than a cache reference, so
	 *   the handle will outlive a transient read;
	 * - the handle was opened for writing;
	 * - the handle has file operations at all;
	 * - those operations can write at an offset, which is how write-back
	 *   reaches the backend;
	 * - the object has no write handle yet, because an existing one is
	 *   kept for the life of the object.
	 */
	if (!cache_only &&
	    (file_status_flags_get(file) & O_ACCMODE) != O_RDONLY &&
	    file->f_ops != NULL &&
	    file->f_ops->pwrite != NULL &&
	    object->write_file == NULL) {
		file_ref(file);
		object->write_file = file;
	}

	/* Separates transient readers from region ownership. */
	if (cache_only) {
		object->active_operations++;
		if (object->active_operations == 0)
			HAL_FATAL("VM cache operation counter overflow");
	} else {
		object->flags &= ~VM_OBJECT_RETAINED_WRITEBACK;
		object->mapping_count++;
		if (object->mapping_count == 0)
			HAL_FATAL("VM object mapping counter overflow");
	}

	refcount_get(&object->refs);

	/* Marks the object as the most recently used for the cache's eviction. */
	object->last_use = ++object_use_generation;

	/* Reports the reference published with its registry ownership. */
	return 0;
}
