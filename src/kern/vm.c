/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The virtual memory core.
 *
 * One unit holds the object store and its AVL page index, the page cache
 * and write-back of object pages, the commitment accounting that bounds
 * anonymous memory, the metadata lock that orders VM work, and the reclaim
 * policy that frees private pages to swap.  These parts were separate
 * translation units and share enough state that they are kept together
 * until a better boundary is found.
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
#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include "kern/lock.h"
#include "kern/swap.h"
#include <hal/hal.h>
#include <limits.h>
#include "kern/sched.h"

#define PAGE_SIZE			ZEDBSD_PAGE_SIZE
#define VM_OBJECT_DATA			__attribute__((section(".vfs_bss")))
#define VM_OBJECT_FAULT_RECLAIM_RETRIES	4U
#define VM_OBJECT_FAULT_RESERVE_PAGES	4U
#define VM_OBJECT_BACKING_SNAPSHOT_MAX	192U
#define VM_OBJECT_CACHE_OBJECTS		32U
#define VM_OBJECT_CACHE_PAGES		16U /* Bounded transfer vector, not retention. */
#define VM_PAGE_TRACKED			0x0010U
#define stats				vm_reclaim_counters

/*
 * One page of storage carved into page descriptors.
 *
 * Descriptors are handed out from a slab's free list rather than allocated
 * one at a time, and a slab that still has a free descriptor stays on the
 * global list so the next allocation finds it without a search.
 */
struct vm_page_slab {
	struct hal_pmem memory;
	struct vm_page_slab *next;
	struct vm_page_slab *previous;
	struct vm_object_page *free_pages;
	unsigned used;
};

/*
 * The registry of shared objects, in most-recently-published order.
 *
 * A file-backed object lives here for as long as any mapping, cache
 * reference or write-back holds it, so a second mapping of the same inode
 * finds the object that already exists.
 */
static struct vm_object *shared_objects VM_OBJECT_DATA;

/*
 * How many objects the registry holds.
 */
static unsigned object_count VM_OBJECT_DATA;

/*
 * The generation stamped on the object published next.
 *
 * It only ever increases, so a stale reference to a slot that has been
 * reused is recognized by the generation that came with it.
 */
static uint64_t object_registry_generation VM_OBJECT_DATA;

/*
 * How many registry entries exist only because the page cache asked for them.
 *
 * These are bounded by VM_OBJECT_CACHE_OBJECTS: an optional admission is
 * refused once that many are already held.
 */
static unsigned cache_objects VM_OBJECT_DATA;

/*
 * How many pages every object holds together.
 */
static atomic_uint_t object_pages VM_OBJECT_DATA;

/*
 * The spin word that guards the registry above.
 *
 * It is taken raw rather than as a spinlock because the registry is walked
 * from contexts that must not block.
 */
static atomic_uint_t object_registry_lock VM_OBJECT_DATA;

/*
 * The slabs that back the object page descriptors.
 */
static struct vm_page_slab *page_slabs VM_OBJECT_DATA;

/*
 * The spin word that guards the slab list above.
 */
static atomic_uint_t page_slab_lock VM_OBJECT_DATA;

/*
 * The commitment accounting that bounds anonymous memory.
 *
 * It holds the physical and swap capacity, the limit those two add up to,
 * and how much of that limit is already committed.
 */
static struct vm_commit_stats commit_stats;

/*
 * The spinlock that guards the commitment accounting above.
 */
static struct spinlock commit_lock = {
	{ 0 }, LOCK_RANK_VM_OBJECT, "VM commit accounting", 0, 0
};

/*
 * The mutex that orders every piece of VM metadata work.
 */
static struct mutex metadata_lock;

/*
 * How deep the owning thread has entered the metadata lock.
 *
 * The lock is re-entrant for the thread that already holds it, and the
 * mutex is released only when the outermost entry leaves.
 */
static unsigned metadata_depth;

/*
 * The list of private pages that reclaim may write to swap.
 */
static struct vm_private_page *page_queue;

/*
 * The mutex that guards the private page queue above.
 */
static struct mutex reclaim_lock;

/*
 * The reclaim statistics, published for the sysctl that reports them.
 */
struct vm_reclaim_stats vm_reclaim_counters;

/*
 * Whether the commitment limit has been published.
 *
 * Until it is, nothing can commit and a swap resize only seeds the
 * accounting rather than changing a live limit.
 */
static int commit_initialized;

/*
 * Whether a swap capacity has been seeded before initialization.
 *
 * The prepared manager and the published backend must agree on the
 * capacity, so initialization compares against what a resize left here.
 */
static int commit_swap_seeded;

/*
 * Whether the metadata lock has been initialized.
 */
static int metadata_initialized;

/*
 * Optional collaborators.
 *
 * Each of these lives in a translation unit that some platforms leave out of
 * the kernel, so the reference is weak and every call site tests it first.
 */
extern void writeback_ticket_commit(struct writeback_ticket *, size_t) __attribute__((weak));
extern void writeback_budget_clean(struct writeback_budget *, size_t) __attribute__((weak));
extern int disk_cache_acquire(struct disk *, struct disk **) __attribute__((weak));
extern void disk_cache_release(struct disk *) __attribute__((weak));
extern void *io_pool_borrow(size_t, size_t *) __attribute__((weak));
extern void io_pool_release(void *) __attribute__((weak));
extern bool hal_irq_disable(void) __attribute__((weak));
extern void hal_irq_enable(void) __attribute__((weak));
extern int vmspace_object_page_revoke(struct vm_object_page *, uint32_t *) __attribute__((weak));
extern void vm_object_read_checkpoint(struct inode *, size_t, size_t) __attribute__((weak));
extern void io_error_record(struct io_error_state *, int) __attribute__((weak));
extern void readahead_consumed(size_t, size_t) __attribute__((weak));
extern size_t cache_memory_reclaim(size_t) __attribute__((weak));
extern int cache_memory_reserve(enum cache_memory_kind, size_t, int) __attribute__((weak));
extern void cache_memory_commit(enum cache_memory_kind, size_t) __attribute__((weak));
extern void cache_memory_cancel(enum cache_memory_kind, size_t) __attribute__((weak));
extern void cache_memory_release(enum cache_memory_kind, size_t) __attribute__((weak));

/*
 * Forward declarations.
 */
static int vm_object_get_shared_internal(struct file *file, struct vm_object **result, int cache_only);
static bool registry_lock(void);
static void registry_unlock(bool enabled);
static struct vm_object *find_object_by_inode_locked(struct inode *inode);
static int object_wait_registry_sequence(struct vm_object *object, uint64_t sequence);
static int object_wait_registry_transition(struct vm_object *object, uint64_t sequence);
static void object_wake_registry_waiters(struct vm_object *object);
static void object_wait_registry_waiters(struct vm_object *object);
static int object_operation_begin(struct vm_object *object);
static void object_operation_end(struct vm_object *object);
static int alloc_vm_page(struct hal_pmem *memory);
static struct vm_object_page *find_page(struct vm_object *object, off_t offset);
static int unlink_page_locked(struct vm_object_page **head, struct vm_object_page *page);
static uint64_t next_generation(struct vm_object *object);
static uint64_t next_inode_resize_generation_locked(struct inode *inode);
static uint64_t next_inode_content_generation_locked(struct inode *inode);
static void object_initialize_eof_locked(struct vm_object *object, struct inode *inode);
static int page_overlaps(const struct vm_object_page *page, uint64_t start, uint64_t end);
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
static int vm_object_sync_range_buffer(struct vm_object *object, off_t offset, size_t size, int flags, int detaching, int resize_owner, off_t resize_target, void *scratch, size_t capacity);
static void object_page_dirty_link(struct vm_object_page *page);
static void object_page_dirty_unlink(struct vm_object_page *page);
static void object_page_dirty_mark(struct vm_object_page *page);
static int object_memory_reserve(enum cache_memory_kind kind, int optional);
static bool object_slab_lock(void);
static void object_slab_unlock(bool enabled);
static struct vm_page_slab *object_slab_alloc(int optional);
static struct vm_object_page *object_descriptor_alloc(int optional);
static void object_descriptor_free(struct vm_object_page *page);
static struct vm_object_page *alloc_object_page(struct vm_object *object, off_t offset, int optional);
static void release_object_page_storage(struct vm_object_page *page);
static unsigned page_index_height(const struct vm_object_page *page);
static void page_index_update(struct vm_object_page *page);
static struct vm_object_page *page_index_rotate_left(struct vm_object_page *page);
static struct vm_object_page *page_index_rotate_right(struct vm_object_page *page);
static struct vm_object_page *page_index_balance(struct vm_object_page *page);
static struct vm_object_page *page_index_insert(struct vm_object_page *root, struct vm_object_page *page);
static struct vm_object_page *page_index_extract_min(struct vm_object_page *root, struct vm_object_page **minimum);
static struct vm_object_page *page_index_remove(struct vm_object_page *root, struct vm_object_page *page);
static void object_page_index_insert(struct vm_object *object, struct vm_object_page *page);
static void object_page_index_remove(struct vm_object *object, struct vm_object_page *page);
static int object_reference_locked(struct vm_object *object, struct file *file, int cache_only);
static int object_cache_retainable(struct vm_object *object);
static int object_cache_evict_one(struct mount *mount);
static void object_cache_discard_prepared(struct vm_object_page **pages, unsigned count);
static int object_cache_read_missing(struct vm_object *object, off_t offset, void *buffer, size_t length, ssize_t *result, int *handled, int *stop);
static struct vm_object_page *sync_page_at_or_after(struct vm_object *object, uint64_t offset);
static int write_dirty_pages(struct vm_object *object, uint64_t generation, off_t logical_size, int inode_io_owned, void *reserved_scratch, size_t reserved_capacity);
static size_t object_prefetch_consume(struct vm_object_page *page, size_t start, size_t length);
static void object_prefetch_retire(struct vm_object_page *page);
static uint32_t mapping_prot(const struct vm_page *page);
static void queue_remove(struct vm_private_page *backing);
static int backing_wired_or_avoided(struct vm_private_page *backing, struct vm_page *avoid);
static int backing_has_wired_mapping(struct vm_private_page *backing);
static int backing_has_busy_mapping(struct vm_private_page *backing);
static uint32_t backing_pte_flags(struct vm_private_page *backing);
static void vmspace_fault_wake(struct vmspace *vm);
static void pin_backing_mappings(struct vm_private_page *backing);
static void unpin_backing_mappings(struct vm_private_page *backing);
static int unmap_backing_ptes(struct vm_private_page *backing, uint32_t *pte_flags);
static void rollback_backing_ptes(struct vm_private_page *backing);
static int swap_out_backing_owned(struct vm_private_page *backing);
static int discard_file_backing_owned(struct vm_private_page *backing);
static int private_page_targets_source(struct vm_private_page *backing, unsigned source_id);
static void private_page_advance_locked(struct vm_private_page *backing);
static int private_page_wait_sequence(struct vm_private_page *backing, uint64_t sequence);
static int private_page_wait_sequence_interruptible(struct vm_private_page *backing, uint64_t sequence);
static int vm_private_page_wait_idle_cancelable(struct vm_private_page *backing, int (*cancel)(void *), void *cancel_argument);
static unsigned reclaim_object_fault_reserve(void);
static void drain_mapping_pins_release(struct vm_private_page *backing);
static int reclaim_backing_owned(struct vm_private_page *backing, int file_candidate);

/*
 * Object
 */

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
	struct file *write_file;
	int error;
	int removed;
	int anonymous;
	bool enabled;
	uint64_t sequence;

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
	enabled = registry_lock();
	if (object->mapping_count == 0)
		HAL_FATAL("VM object mapping reference underflow");

	/* Waits for the operations in flight before releasing the last mapping. */
	if (object->mapping_count == 1 && object->active_operations != 0) {
		sequence = waitq_sequence(&object->registry_waitq);
		registry_unlock(enabled);
		error = object_wait_registry_sequence(object, sequence);
		if (error != 0 && error != EAGAIN)
			HAL_FATAL("VM object operation drain wait failed");
		goto retry_mapping;
	}

	/* An ordinary drop leaves the object mapped and alive. */
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
	error = vm_object_sync_range_internal(object, 0, SIZE_MAX, MS_SYNC, 1, 0, 0);

	/* Unlinks a clean object, or retains one that still has dirty pages. */
	enabled = registry_lock();
	if (object->mapping_count != 0 ||
	    (object->flags & VM_OBJECT_DETACHING) == 0)
		HAL_FATAL("VM object teardown state changed during writeback");
	if (!anonymous && error == 0 && object_cache_retainable(object)) {
		object->flags &= ~VM_OBJECT_DETACHING;
		write_file = object->write_file;
		object->write_file = NULL;
	} else if (error == 0 && object_can_destroy(object)) {
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

	/* Releases the waiters, and drains them before the object disappears. */
	object_wake_registry_waiters(object);
	if (removed)
		object_wait_registry_waiters(object);
	if (removed && !anonymous && refcount_put(&object->refs))
		HAL_FATAL("VM object registry reference was last unexpectedly");

	/* Closes the former mapping writer outside all registry and object locks. */
	if (write_file != NULL)
		(void)file_close(write_file);

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

			/* Succeeded: no teardown stands in the way. */
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

	/* Refuses a reader while a resize or a write owns the inode. */
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
	enabled = registry_lock();

	/* Classifies the registry entry the inode has, if it has one. */
	object = find_object_by_inode_locked(inode);
	if (object == NULL)
		result = 0;
	else if ((object->flags & VM_OBJECT_DETACHING) != 0)
		result = -EAGAIN;
	else
		result = 1;

	registry_unlock(enabled);

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

	memset(content, 0, sizeof(*content));

	/* An empty range needs no transaction. */
	if (length == 0)
		return 0;

	/* Requires a regular file that carries VM state. */
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
	/* Refuses an object that is already tearing down. */
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

		/* A mapped object keeps the writer that may flush its pages. */
		if (object->mapping_count != 0 && object->write_file == NULL) {
			file_ref(file);
			object->write_file = file;
		}
	}

	spin_unlock_irqrestore(&inode->i_vm_lock, inode_irq);

	registry_unlock(enabled);

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
				if (!page_overlaps(scan, start, end) ||
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
		object_prefetch_retire(page);
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
		if ((observed & HAL_PAGE_DIRTY) != 0) {
			object_page_dirty_mark(page);
		}
		dirty = (page->flags & VM_OBJECT_PAGE_DIRTY) != 0;
		page->write_dirty_generation = page->dirty_generation;
		spin_unlock_irqrestore(&object->lock, irq);

		/* Writes a dirty page back before the caller overwrites it. */
		if (error == 0 && dirty)
			error = write_page_data(object, page,
			    object->logical_size, 0);

		/* Clears the dirty record only for a write nothing raced. */
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
	error = vm_object_read_coherent_useful(inode, offset, buffer, length, result, NULL);

	/* Reports the failure. */
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
	enabled = registry_lock();
	object = find_object_by_inode_locked(inode);
	if (object == NULL) {
		registry_unlock(enabled);
		return ENOENT;
	}

	/* Waits out a teardown and looks the object up again. */
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

	/* Counts this read as an operation and pins the object. */
	object->active_operations++;
	if (object->active_operations == 0)
		HAL_FATAL("VM object read operation counter overflow");
	refcount_get(&object->refs);
	registry_unlock(enabled);

	/* Covers cache hits with the same detach barrier as device-backed reads. */
	cache_disk = NULL;
	if (inode->i_mount != NULL && inode->i_mount->m_disk != NULL &&
	    disk_cache_acquire != NULL && disk_cache_release != NULL)
		error = disk_cache_acquire(inode->i_mount->m_disk, &cache_disk);

	/* Reports device retirement as an error rather than the fault EOF sentinel. */
	if (error != 0) {
		object_operation_end(object);
		if (refcount_put(&object->refs))
			destroy_object(object);
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
		error = object_cache_read_missing(object, current, bytes + done,
		    length - done, &populated, &handled, &stop);
		if (error != 0)
			break;
		if (handled) {
			done += (size_t)populated;
			if (populated > 0 && vm_object_read_checkpoint != NULL)
				vm_object_read_checkpoint(inode, done, length);
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
		memcpy(bytes + done, (const uint8_t *)page->pmem.vaddr + in_page,
		    chunk);

		/* Credits the prefetch that made this page resident. */
		consumed = object_prefetch_consume(page, in_page, chunk);
		if (useful != NULL)
			*useful += consumed;

		/* Releases the fault hold and reports the progress. */
		page->hold_count--;
		waitq_wake_all(&object->page_waitq);
		spin_unlock_irqrestore(&object->lock, irq);
		done += chunk;
		if (vm_object_read_checkpoint != NULL)
			vm_object_read_checkpoint(inode, done, length);
	}

	/* Releases the device admission and the object this read held. */
	if (cache_disk != NULL)
		disk_cache_release(cache_disk);
	object_operation_end(object);
	if (refcount_put(&object->refs))
		destroy_object(object);

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
	memset(resize, 0, sizeof(*resize));

	/* Only one transaction runs, and never under a read lease. */
	enabled = registry_lock();
	inode_irq = spin_lock_irqsave(&inode->i_vm_lock);

	/* Refuses a resize while another transaction or a reader owns the inode. */
	if (inode->i_vm_resize_active ||
	    inode->i_vm_content_active ||
	    inode->i_vm_content_readers != 0) {
		spin_unlock_irqrestore(&inode->i_vm_lock, inode_irq);
		registry_unlock(enabled);
		return EBUSY;
	}
	/* Refuses an object that is already tearing down. */
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

	/* A resize to the current size has nothing to publish. */
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
	page = alloc_object_page(object, offset, 0);
	if (page == NULL) {
		if (vm_reclaim_one(NULL) != 0)
			return ENOMEM;

		page = alloc_object_page(object, offset, 0);
		if (page == NULL)
			return ENOMEM;
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
	    find_page(object, offset) != NULL) {
		spin_unlock_irqrestore(&object->lock, irq);
		release_object_page_storage(page);
		goto retry;
	}
	page->next = object->pages;
	object->pages = page;
	object_page_index_insert(object, page);
	(void)atomic_fetch_add_relaxed(&object_pages, 1);

	spin_unlock_irqrestore(&object->lock, irq);

read_page:
	/* Reads the page's data, zero-filling past the end of file. */
	memset((void *)page->pmem.vaddr, 0, PAGE_SIZE);
	length = (size_t)(fault_size - offset);
	if (length > PAGE_SIZE)
		length = PAGE_SIZE;

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

	/* Fails the fault when a transaction or a size change overtook the read. */
	if ((object->flags & (VM_OBJECT_RESIZING | VM_OBJECT_CONTENT)) != 0 ||
	    object->size_generation != fault_generation ||
	    object->content_generation != fault_content_generation) {
		page->flags = VM_OBJECT_PAGE_ERROR;
		page->error = EAGAIN;
		waitq_wake_all(&object->page_waitq);
		spin_unlock_irqrestore(&object->lock, irq);
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

	/* Reports why the copy failed. */
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

	error = vm_object_page_pin_copy(page, offset, (void *)buffer, length, 1);

	/* Reports why the copy failed. */
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
		    !unlink_page_locked(&object->orphan_pages, page))
			HAL_FATAL("lost VM object resize orphan");
		page->flags &= ~VM_OBJECT_PAGE_ORPHANED;
		free_page = page;
	}
	waitq_wake_all(&object->page_waitq);

	spin_unlock_irqrestore(&object->lock, irq);

	/* Frees the unlinked orphan and ends the pin's operation. */
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

	/* Turns the fault's hold into a published mapping. */
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

	object_page_dirty_mark(page);

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
	error = vm_object_sync_inode_buffer(inode, NULL, 0);

	/* Reports the failure. */
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

		/* Waits out a resize and looks the object up again. */
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

		/* Counts this sync as an operation and pins the object. */
		object->active_operations++;
		if (object->active_operations == 0)
			HAL_FATAL("VM object operation counter overflow");

		refcount_get(&object->refs);
		break;
	}
	registry_unlock(enabled);

	/* An uncached inode has nothing to write back. */
	if (object == NULL)
		return 0;

	/* Writes everything back. */
	error = vm_object_sync_range_buffer(object, 0, SIZE_MAX, MS_SYNC, 0, 0,
	    0, scratch, capacity);

	/* Retires a resolved failure independently of optional clean retention. */
	enabled = registry_lock();
	if (error == 0) {
		irq = spin_lock_irqsave(&object->lock);
		if (object->writeback_error == 0 &&
		    !object_has_dirty_pages_locked(object))
			object->flags &= ~VM_OBJECT_RETAINED_WRITEBACK;
		spin_unlock_irqrestore(&object->lock, irq);
	}

	/* Keeps clean cache ownership, or retires the old registry-only object. */
	if (object->mapping_count == 0 &&
	    object->active_operations == 1 &&
	    (object->flags & VM_OBJECT_DETACHING) == 0) {
		if (error == 0 && object_cache_retainable(object)) {
			write_file = object->write_file;
			object->write_file = NULL;
		} else if (error == 0 && object_can_destroy(object)) {
			removed = unlink_object_locked(object);
		} else {
			retain_object(object, error);
		}
	}
	registry_unlock(enabled);

	/* Ends the operation and drops the reference this sync took. */
	if (removed && refcount_put(&object->refs))
		HAL_FATAL("VM object registry reference was last unexpectedly");
	object_operation_end(object);
	if (refcount_put(&object->refs))
		destroy_object(object);

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

	/* Counts this truncate as an operation and pins the object. */
	if (object != NULL) {
		object->active_operations++;
		if (object->active_operations == 0)
			HAL_FATAL("VM object operation counter overflow");
		refcount_get(&object->refs);
	}
	registry_unlock(enabled);

	/* An uncached inode has no object to truncate. */
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

	/* Publishes the new end of file once the pages are gone. */
	if (error == 0) {
		irq = spin_lock_irqsave(&object->lock);
		object->logical_size = size;
		object->size_generation = next_generation(object);
		spin_unlock_irqrestore(&object->lock, irq);
	}

	/* Ends the operation and drops the reference this truncate took. */
	object_operation_end(object);
	if (refcount_put(&object->refs))
		destroy_object(object);
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

	/* Counts the registered objects a failed writeback retained. */
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

	/*
	 * Keeps a competing claim from publishing over the new cache identity.
	 */
	error = backing_mutation_begin_inode(inode, &guard);
	if (error != 0)
		return;

	/* Retries optional admission once after reclaiming one idle object. */
	error = vm_object_get_shared_internal(file, &object, 1);
	if (error == ENOMEM && object_cache_evict_one(NULL))
		error = vm_object_get_shared_internal(file, &object, 1);
	backing_mutation_end(&guard);
	if (error != 0)
		return;

	/*
	 * Drops the operation reference while retaining the independent cache.
	 */
	object_operation_end(object);
	if (refcount_put(&object->refs))
		destroy_object(object);
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
	enabled = registry_lock();
	object = find_object_by_inode_locked(inode);
	if (object == NULL || (object->flags & VM_OBJECT_DETACHING) != 0) {
		registry_unlock(enabled);
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
	registry_unlock(enabled);
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
	object_operation_end(object);
	if (refcount_put(&object->refs))
		destroy_object(object);
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
		if (!object_cache_evict_one(mount))
			break;
		count++;
	}

	/* Reports the number of detached cache objects. */
	return count;
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

	enabled = registry_lock();
	if (object->write_file != NULL) {
		registry_unlock(enabled);
		*result = object;
		return 0;
	}
	registry_unlock(enabled);

	/*
	 * The caller already owns write access; the clone never reaches
	 * userland.
	 */
	error = file_open_resolved(&file->f_path, O_RDWR | O_NOFOLLOW, &writer);
	if (error == 0 && file_vm_inode(writer) != inode)
		error = EAGAIN;
	if (error == 0) {
		enabled = registry_lock();
		if (object->write_file == NULL) {
			object->write_file = writer;
			writer = NULL;
		}
		registry_unlock(enabled);
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

void
vm_object_writeback_release(
	struct vm_object *object)
{
	if (object == NULL)
		return;

	object_operation_end(object);

	if (refcount_put(&object->refs))
		destroy_object(object);
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

	memset(pages, 0, sizeof(pages));
	memset(fresh, 0, sizeof(fresh));
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
		pages[index] = find_page(object, (off_t)(base + index * PAGE_SIZE));

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
		current = base + (uint64_t)index * PAGE_SIZE;

		/* Allocates a page for a hole this transaction covers. */
		if (pages[index] == NULL) {
			pages[index] = alloc_object_page(object, (off_t)current, 1);
			if (pages[index] == NULL) {
				error = EAGAIN;
				goto failed;
			}

			fresh[index] = 1;

			memset(pages[index]->pmem.vaddr, 0, PAGE_SIZE);

			wanted = (size_t)(object->logical_size - (off_t)current);
			if (wanted > PAGE_SIZE)
				wanted = PAGE_SIZE;

			/*
			 * Preserve old bytes even for a full overwrite: commit
			 * may still abort.
			 */
			received = file_pread_internal(object->file,
						       pages[index]->pmem.vaddr,
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

		if ((observed & HAL_PAGE_DIRTY) != 0)
			object_page_dirty_mark(pages[index]);

		spin_unlock_irqrestore(&object->lock, irq);

		if (error != 0)
			goto failed;
	}

	/* Recheck all private identities before publishing any of them. */
	irq = spin_lock_irqsave(&object->lock);

	/* Refuses the transaction when a fault published one of these pages first. */
	for (index = 0; index < count; index++) {
		if (fresh[index]) {
			if (find_page(object, pages[index]->offset) != NULL) {
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
			object_page_index_insert(object, pages[index]);
			(void)atomic_fetch_add_relaxed(&object_pages, 1);
		}
	}

	content->prepared = 1;
	content->writeback_ticket = ticket;

	spin_unlock_irqrestore(&object->lock, irq);

	return 0;

failed:
	irq = spin_lock_irqsave(&object->lock);

	content_release_pages_locked(object, content->generation);

	spin_unlock_irqrestore(&object->lock, irq);

	for (index = 0; index < count; index++) {
		if (fresh[index])
			release_object_page_storage(pages[index]);
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
	enabled = registry_lock();
	cutoff = object_registry_generation;
	registry_unlock(enabled);
	cursor = 0;
	first_error = 0;

	/*
	 * Pins only the next inode, allowing each completed object to retire
	 * normally.
	 */
	for (;;) {
		inode = NULL;
		next = UINT64_MAX;
		enabled = registry_lock();
		for (object = shared_objects; object != NULL; object = object->next) {
			/* Skips what this pass has done, or published too late. */
			if (object->registry_generation <= cursor ||
			    object->registry_generation > cutoff ||
			    object->registry_generation > next || object->inode == NULL)
				continue;

			/* Skips an object outside the mount the caller named. */
			if (mount != NULL && object->inode->i_mount != mount)
				continue;
			next = object->registry_generation;
			inode = object->inode;
		}

		/* Pins the chosen inode, and stops when none is left. */
		if (inode != NULL)
			inode_ref(inode);
		registry_unlock(enabled);
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

	/* Rejects a run longer than one batch of disk work. */
	if (length > KERN_IO_BATCH_MAX)
		return EINVAL;

	memset(fill, 0, sizeof(*fill));

	/*
	 * Leaves absent or transitioning objects to demand-driven cache
	 * construction.
	 */
	enabled = registry_lock();
	object = find_object_by_inode_locked(inode);
	if (object == NULL || (object->flags & VM_OBJECT_DETACHING) != 0) {
		registry_unlock(enabled);
		return EAGAIN;
	}

	/* Counts this prefetch as an operation and pins the object. */
	object->active_operations++;
	if (object->active_operations == 0)
		HAL_FATAL("prefetch operation counter overflow");

	refcount_get(&object->refs);
	registry_unlock(enabled);

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
	    find_page(object, offset) != NULL) {
		spin_unlock_irqrestore(&object->lock, irq);
		vm_object_prefetch_abort(fill);
		return EAGAIN;
	}

	/* Clamps the run to the end of file and to the first resident page. */
	if ((uint64_t)length > (uint64_t)(object->logical_size - offset))
		length = (size_t)(object->logical_size - offset);

	/* Stops the run at the first page that is already resident. */
	for (index = 1; (size_t)index * PAGE_SIZE < length; index++) {
		if (find_page(object, offset + (off_t)index * PAGE_SIZE) != NULL) {
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
		page = alloc_object_page(object,
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
	if (received < 0 || (size_t)received != fill->length || bytes == NULL) {
		error = received < 0 && received >= -INT_MAX ? (int)-received : EIO;
		vm_object_prefetch_abort(fill);
		return error;
	}

	object = fill->object;

	/* Initializes every private page before taking the publication lock. */
	for (index = 0; index < fill->count; index++) {
		page = fill->pages[index];
		valid = fill->length - (size_t)index * PAGE_SIZE;
		if (valid > PAGE_SIZE)
			valid = PAGE_SIZE;
		memset(page->pmem.vaddr, 0, PAGE_SIZE);
		memcpy(page->pmem.vaddr, (const uint8_t *)bytes +
		    (size_t)index * PAGE_SIZE, valid);
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
		if (find_page(object, fill->pages[index]->offset) != NULL)
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
			page->next = object->pages;
			object->pages = page;
			object_page_index_insert(object, page);

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

	/*
	 * Makes cancellation of an unused or previously consumed token
	 * idempotent.
	 */
	if (fill == NULL || fill->object == NULL)
		return;

	/* Keeps the object, because the descriptor is cleared below. */
	object = fill->object;

	/* Returns every frame the fill prepared but never published. */
	object_cache_discard_prepared(fill->pages, fill->count);

	/* Releases the disk admission the fill pinned. */
	if (fill->disk != NULL)
		disk_cache_release(fill->disk);

	/* Empties the descriptor so a second cancellation does nothing. */
	memset(fill, 0, sizeof(*fill));

	/* Retires the operation this prefetch counted against the object. */
	object_operation_end(object);

	/* Destroys the object when this was the last reference to it. */
	if (refcount_put(&object->refs))
		destroy_object(object);
}

/*
 * Commit
 */

/*
 * Computes the commit limit from the free physical memory and the swap
 * capacity.
 *
 * A swap size seeded earlier by vm_commit_resize_swap() must match the
 * published backend, otherwise the initialization is retried later.
 */
int
vm_commit_init(
	void)
{
	struct hal_memory_stats memory;
	struct swap_backend *swap;
	uint32_t swap_pages;
	uint32_t swap_free;
	uint64_t physical_pages;
	unsigned long irq;

	swap_pages = 0;
	swap_free = 0;

	irq = spin_lock_irqsave(&commit_lock);

	/* Rejects a second initialization. */
	if (commit_initialized) {
		spin_unlock_irqrestore(&commit_lock, irq);
		return EBUSY;
	}

	/* Counts the physical pages left after the system reserve. */
	hal_memory_get_stats(&memory);
	physical_pages = memory.physical_free / VM_COMMIT_PAGE_SIZE;
	if (physical_pages > VM_COMMIT_SYSTEM_RESERVE_PAGES)
		physical_pages -= VM_COMMIT_SYSTEM_RESERVE_PAGES;
	else
		physical_pages = 0;

	/* Reads the swap capacity when a backend is published. */
	swap = swap_system_backend();
	if (swap != NULL)
		(void)swap_get_stats(swap, &swap_pages, &swap_free);

	/*
	 * The prepared manager and published backend must describe the same
	 * capacity before user commitment can begin.
	 */
	if (!commit_swap_seeded) {
		memset(&commit_stats, 0, sizeof(commit_stats));
		commit_stats.swap_pages = swap_pages;
	} else if (commit_stats.swap_pages != swap_pages) {
		spin_unlock_irqrestore(&commit_lock, irq);
		return EAGAIN;
	}

	/* Publishes the limit; a zero limit means nothing can ever commit. */
	commit_stats.physical_pages = physical_pages;
	commit_stats.limit_pages = commit_stats.physical_pages + commit_stats.swap_pages;
	if (commit_stats.limit_pages == 0) {
		spin_unlock_irqrestore(&commit_lock, irq);
		return ENOMEM;
	}
	commit_initialized = 1;

	spin_unlock_irqrestore(&commit_lock, irq);

	/* Reports the initialized accounting. */
	return 0;
}

/*
 * Replaces the swap capacity in the commit limit.
 *
 * The caller states the capacity it expects to replace, so a concurrent
 * change is detected.  Before initialization the new capacity only seeds
 * the accounting.
 */
int
vm_commit_resize_swap(
	uint64_t expected_pages,
	uint64_t replacement_pages)
{
	uint64_t limit;
	unsigned long irq;

	irq = spin_lock_irqsave(&commit_lock);

	/* Rejects a resize based on a stale capacity. */
	if (commit_stats.swap_pages != expected_pages) {
		spin_unlock_irqrestore(&commit_lock, irq);
		return EAGAIN;
	}

	/* Rejects a limit that would overflow. */
	if (replacement_pages > UINT64_MAX - commit_stats.physical_pages) {
		spin_unlock_irqrestore(&commit_lock, irq);
		return EOVERFLOW;
	}

	/* Rejects a limit below the pages already committed. */
	limit = commit_stats.physical_pages + replacement_pages;
	if (commit_initialized && commit_stats.used_pages > limit) {
		spin_unlock_irqrestore(&commit_lock, irq);
		return ENOMEM;
	}

	/* Publishes the new capacity, seeding the accounting when early. */
	commit_stats.swap_pages = replacement_pages;
	commit_stats.limit_pages = limit;
	if (!commit_initialized)
		commit_swap_seeded = 1;

	spin_unlock_irqrestore(&commit_lock, irq);

	/* Reports the accepted resize. */
	return 0;
}

/*
 * Commits pages against the limit.
 */
int
vm_commit_reserve(
	size_t bytes)
{
	uint64_t pages;
	unsigned long irq;

	/* Rejects an empty or unaligned reservation. */
	if (bytes == 0 || (bytes & (VM_COMMIT_PAGE_SIZE - 1U)) != 0)
		return EINVAL;
	pages = bytes / VM_COMMIT_PAGE_SIZE;

	irq = spin_lock_irqsave(&commit_lock);

	/* A reservation before initialization is a programming error. */
	if (!commit_initialized) {
		spin_unlock_irqrestore(&commit_lock, irq);
		HAL_FATAL("VM commit before initialization");
	}

	/* Refuses to exceed the limit. */
	if (pages > commit_stats.limit_pages - commit_stats.used_pages) {
		spin_unlock_irqrestore(&commit_lock, irq);
		return ENOMEM;
	}
	commit_stats.used_pages += pages;

	spin_unlock_irqrestore(&commit_lock, irq);

	/* Reports the committed pages. */
	return 0;
}

/*
 * Returns committed pages to the limit.
 */
void
vm_commit_release(
	size_t bytes)
{
	uint64_t pages;
	unsigned long irq;

	/* An empty or unaligned release is a programming error. */
	if (bytes == 0 || (bytes & (VM_COMMIT_PAGE_SIZE - 1U)) != 0)
		HAL_FATAL("invalid VM commit release");
	pages = bytes / VM_COMMIT_PAGE_SIZE;

	irq = spin_lock_irqsave(&commit_lock);

	/* A release before initialization is a programming error. */
	if (!commit_initialized) {
		spin_unlock_irqrestore(&commit_lock, irq);
		HAL_FATAL("VM commit release before initialization");
	}

	/* Releasing more than was committed is a programming error. */
	if (pages > commit_stats.used_pages) {
		spin_unlock_irqrestore(&commit_lock, irq);
		HAL_FATAL("VM commit accounting underflow");
	}
	commit_stats.used_pages -= pages;

	spin_unlock_irqrestore(&commit_lock, irq);
}

/*
 * Copies the current commit statistics.
 */
void
vm_commit_get_stats(
	struct vm_commit_stats *output)
{
	unsigned long irq;

	/* Ignores a missing record. */
	if (output == NULL)
		return;

	/* Copies the statistics under the lock. */
	irq = spin_lock_irqsave(&commit_lock);

	memcpy(output, &commit_stats, sizeof(*output));

	spin_unlock_irqrestore(&commit_lock, irq);
}

/*
 * Tests whether swap can be shut down without stranding committed pages.
 */
int
vm_commit_can_shutdown_swap(
	void)
{
	unsigned long irq;
	int safe;

	/* Swap is safe to remove while nothing depends on its capacity. */
	irq = spin_lock_irqsave(&commit_lock);

	safe = !commit_initialized ||
		commit_stats.swap_pages == 0 ||
		commit_stats.used_pages == 0;

	spin_unlock_irqrestore(&commit_lock, irq);

	/* Reports the verdict. */
	return safe;
}

/*
 * Page
 */

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
 * Metadata
 */

/*
 * Initializes the metadata lock once.
 */
void
vm_metadata_init(
	void)
{
	/* Ignores a repeated initialization. */
	if (metadata_initialized)
		return;

	/* Creates the mutex; failure here leaves the VM unusable. */
	if (mutex_init(&metadata_lock, LOCK_RANK_VMSPACE, "VM metadata") != 0)
		HAL_FATAL("VM metadata lock initialization failed");

	metadata_initialized = 1;
}

/*
 * Enters the metadata lock, re-entering it when already owned.
 */
void
vm_metadata_enter(
	void)
{
	vm_metadata_init();

	/* Deepens an entry that the current thread already owns. */
	if (mutex_owned(&metadata_lock)) {
		if (metadata_depth == UINT_MAX)
			HAL_FATAL("VM metadata lock recursion overflow");
		metadata_depth++;
		return;
	}

	/* Takes the first entry. */
	mutex_lock(&metadata_lock);

	metadata_depth = 1;
}

/*
 * Leaves one level of the metadata lock.
 */
void
vm_metadata_leave(
	void)
{
	/* Traps on a release by a thread that does not own the lock. */
	if (!metadata_initialized ||
	    !mutex_owned(&metadata_lock) ||
	    metadata_depth == 0)
		HAL_FATAL("VM metadata lock ownership mismatch");

	/* Releases the mutex when the outermost entry leaves. */
	if (--metadata_depth == 0)
		mutex_unlock(&metadata_lock);
}

/*
 * Tests whether the current thread owns the metadata lock.
 */
int
vm_metadata_owned(
	void)
{
	int owned;

	/* An uninitialized lock is owned by nobody. */
	if (!metadata_initialized)
		return 0;

	/* Asks the mutex. */
	owned = mutex_owned(&metadata_lock);

	/* Reports the ownership. */
	return owned;
}

/*
 * Private
 */

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
	struct hal_pmem memory;
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
	memset(&backing->pmem, 0, sizeof(backing->pmem));

	slot = backing->swap_slot;
	backing->swap_slot = SWAP_SLOT_NONE;

	spin_unlock_irqrestore(&backing->state_lock, irq);

	/* Frees them and the metadata. */
	if (memory.size != 0)
		(void)hal_pmem_free(&memory);

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
			private_page_advance_locked(backing);
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
	irq = spin_lock_irqsave(&backing->state_lock);

	/* Refuses a page that is not idle, rather than waiting for it. */
	if ((backing->flags & VM_PAGE_BUSY) != 0 ||
	    backing->active_operations != 0 ||
	    backing->pin_count != 0) {
		spin_unlock_irqrestore(&backing->state_lock, irq);
		return EBUSY;
	}

	/* The returned I/O ownership includes an explicit lifetime hold. */
	refcount_get(&backing->refs);
	backing->flags |= VM_PAGE_BUSY;
	private_page_advance_locked(backing);

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

	private_page_advance_locked(backing);

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

	private_page_advance_locked(backing);
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
	private_page_advance_locked(backing);

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
	struct hal_pmem *memory)
{
	unsigned long irq;

	/* Rejects a missing backing or result. */
	if (backing == NULL || memory == NULL)
		return EINVAL;

	/* Fails while an owner or operation is present, or the page is absent. */
	irq = spin_lock_irqsave(&backing->state_lock);

	/* Refuses a page that is busy, in use, or not in memory. */
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

	private_page_advance_locked(backing);
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
	const struct hal_pmem_request request = {
		HAL_PMEM_PADDR_ANY, PAGE_SIZE, PAGE_SIZE,
		HAL_PMEM_TYPE_RAM, 0
	};

	struct swap_backend *backend;
	uint32_t slot;
	unsigned long irq;
	int error;
	struct hal_pmem memory;

	backend = swap_system_backend();

	/* Rejects a missing operand or a backing that is not owned and swapped. */
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

	/* Allocates a page, reclaiming once when memory is short, and reads it. */
	if (hal_pmem_alloc(&request, &backing->pmem) == HAL_OK)
		error = 0;
	else
		error = ENOMEM;

	/* Reclaims once and tries again before giving up on memory. */
	if (error != 0 && vm_reclaim_one(accounting_page) == 0) {
		if (hal_pmem_alloc(&request, &backing->pmem) == HAL_OK)
			error = 0;
		else
			error = ENOMEM;
	}
	if (error == 0)
		error = swap_read_page(backend, slot,
		    (void *)backing->pmem.vaddr);

	/* Returns the page rather than leaving a failed read behind. */
	if (error != 0) {
		if (backing->pmem.size != 0) {
			memory = backing->pmem;
			memset(&backing->pmem, 0, sizeof(backing->pmem));
			(void)hal_pmem_free(&memory);
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
	private_page_advance_locked(backing);

	spin_unlock_irqrestore(&backing->state_lock, irq);

	vm_page_note_in(accounting_page);

	/* Reports the paged-in backing. */
	return 0;
}

/*
 * Page
 */

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

	spin_unlock_irqrestore(&backing->state_lock, irq);

	/* Puts the backing at the head of the reclaim queue. */
	backing->queue_next = page_queue;
	page_queue = backing;

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

	resident = (backing->flags & VM_PAGE_RESIDENT) != 0;
	swapped = (backing->flags & VM_PAGE_SWAPPED) != 0;
	tracked = (backing->flags & VM_PAGE_TRACKED) != 0;

	/* A backing that no mapping refers to leaves the reclaim queue. */
	if (last_mapping)
		backing->flags &= ~VM_PAGE_TRACKED;

	spin_unlock_irqrestore(&backing->state_lock, irq);

	/* Discharges the untracked backing from the reclaim counters. */
	if (last_mapping && tracked) {
		queue_remove(backing);

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
	 * Traps unless the caller owns the old backing for I/O and at least
	 * one mapping still refers to it.
	 */
	if ((old->flags & VM_PAGE_BUSY) == 0 || old->mapping_count == 0)
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
	fresh->queue_next = page_queue;

	/* Puts the fresh backing at the head of the reclaim queue. */
	page_queue = fresh;

	/* Counts a resident page towards what reclaim can free. */
	if (fresh_resident)
		stats.resident++;

	/* An old backing without mappings leaves the queue. */
	if (old_last) {
		queue_remove(old);
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

/*
 * Reclaim
 */

/*
 * Initializes the reclaim queue and statistics.
 */
void
vm_reclaim_init(
	void)
{
	vm_metadata_init();

	page_queue = NULL;

	memset(&stats, 0, sizeof(stats));

	(void)mutex_init(&reclaim_lock, LOCK_RANK_VMSPACE, "VM reclaim");
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
	enabled = registry_lock();
	for (object = shared_objects; object != NULL && freed < target;
	     object = object->next) {
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
			object_page_index_remove(object, page);
			page->next = retired;
			retired = page;
			freed += page->pmem.size;
		}
		spin_unlock_irqrestore(&object->lock, irq);
	}
	registry_unlock(enabled);

	/* Frees detached storage without closing any inode or mount reference. */
	while (retired != NULL) {
		page = retired;
		retired = page->next;
		free_object_page(page);
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

	enabled = registry_lock();
	for (object = shared_objects; object != NULL; object = object->next) {
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
	registry_unlock(enabled);

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

	/* Ends the operation and drops the reference this pass took. */
	object_operation_end(object);
	if (refcount_put(&object->refs))
		destroy_object(object);

	/* Reclaim reports whether a page was freed; writeback retains error. */
	if (error != 0)
		return ENOMEM;
	return 0;
}

/*
 * Reclaims clean cache first, then private or writeback-backed pages.
 */
int
vm_reclaim_one(
	struct vm_page *avoid)
{
	int result;

	/* Returns disposable cache before initiating private-page swap or writeback. */
	if (cache_memory_reclaim != NULL && cache_memory_reclaim(PAGE_SIZE) != 0)
		return 0;

	/* A private page is the first choice. */
	result = vm_reclaim_private_one(avoid);
	if (result == 0)
		return 0;
	if (result != EAGAIN)
		return result;

	/*
	 * Object writeback can sleep in VFS I/O.  It must not inherit either the
	 * private-page queue lock or the global cross-VM metadata lock from the
	 * candidate scan above.
	 */
	result = vm_object_reclaim_one();
	if (result != 0)
		return ENOMEM;
	vm_metadata_enter();
	mutex_lock(&reclaim_lock);

	stats.reclaims++;

	mutex_unlock(&reclaim_lock);
	vm_metadata_leave();

	/* Reports the reclaimed object page. */
	return 0;
}

/*
 * Counts a page fault.
 */
void
vm_reclaim_note_fault(
	void)
{
	vm_metadata_enter();
	mutex_lock(&reclaim_lock);

	stats.faults++;

	mutex_unlock(&reclaim_lock);
	vm_metadata_leave();
}

/*
 * Copies the reclaim statistics, classifying the queued pages.
 */
void
vm_reclaim_get_stats(
	struct vm_reclaim_stats *output)
{
	struct vm_private_page *backing;
	struct vm_page *page;
	unsigned long irq;
	unsigned state_flags;
	uint32_t flags;

	/* Ignores a missing result. */
	if (output == NULL)
		return;

	/* Takes the counters as the starting point under the reclaim lock. */
	vm_metadata_enter();
	mutex_lock(&reclaim_lock);

	memcpy(output, &stats, sizeof(*output));

	/* Clears the fields this pass classifies for itself. */
	output->anonymous_resident = 0;
	output->file_resident = 0;
	output->wired = 0;
	output->busy = 0;
	output->dirty = 0;
	output->clean = 0;

	/* Classifies every private backing on the reclaim queue. */
	for (backing = page_queue; backing != NULL; backing = backing->queue_next) {
		/* Samples the hardware and software state of this backing. */
		flags = backing_pte_flags(backing);
		irq = spin_lock_irqsave(&backing->state_lock);
		state_flags = backing->flags;
		spin_unlock_irqrestore(&backing->state_lock, irq);

		/* Counts a backing whose page is under I/O. */
		if (state_flags & VM_PAGE_BUSY)
			output->busy++;

		/* Counts a backing that any mapping wires down. */
		for (page = backing->mappings; page != NULL;
		     page = page->private_next) {
			if (page->wire_count != 0) {
				output->wired++;
				break;
			}
		}

		/* Leaves a backing without a resident page unclassified. */
		if (!(state_flags & VM_PAGE_RESIDENT))
			continue;

		/* Attributes the resident page to anonymous or file memory. */
		page = backing->mappings;
		if (page != NULL && page->region->backing == VM_BACKING_ANON)
			output->anonymous_resident++;
		else
			output->file_resident++;

		/* Separates pages that need writing from pages that do not. */
		if ((state_flags & VM_PAGE_DIRTY) || (flags & HAL_PAGE_DIRTY))
			output->dirty++;
		else
			output->clean++;
	}

	/* Adds the object store, whose pages are not on this queue. */
	output->file_resident += vm_object_page_count();
	output->resident += vm_object_page_count();

	mutex_unlock(&reclaim_lock);
	vm_metadata_leave();
}

/*
 * Reads every page of a draining swap source back into memory.
 *
 * A cancel callback, when given, is polled between pages and while
 * waiting for a busy backing.
 */
int
vm_reclaim_drain_swap_source_cancelable(
	unsigned source_id,
	int (*cancel)(void *),
	void *cancel_argument)
{
	struct vm_private_page *selected;
	struct vm_private_page *wait_backing;
	struct vm_private_page *backing;
	struct vm_page *accounting_page;
	struct swap_backend *backend;
	struct swap_source_stats source_stats;
	int scan_error;
	int error;

	/* Rejects a source id out of range. */
	if (source_id >= SWAP_SOURCE_COUNT)
		return EINVAL;

	/* Pages in one backing per pass until the source is empty. */
	for (;;) {
		selected = NULL;
		wait_backing = NULL;
		accounting_page = NULL;
		scan_error = 0;

		/* Honors a cancellation between passes. */
		if (cancel != NULL) {
			error = cancel(cancel_argument);
			if (error != 0)
				return error;
		}

		/*
		 * Select and pin one target while reverse mappings are stable.  No
		 * VM metadata lock survives physical allocation or backing I/O.
		 */
		vm_metadata_enter();
		mutex_lock(&reclaim_lock);
		for (backing = page_queue; backing != NULL;
		     backing = backing->queue_next) {
			if (!private_page_targets_source(backing, source_id))
				continue;

			/*
			 * A wired page cannot legitimately have been reclaimed to
			 * swap.  Report the broken invariant instead of waiting
			 * forever.
			 */
			if (backing_has_wired_mapping(backing)) {
				scan_error = EIO;
				break;
			}
			if (backing_has_busy_mapping(backing) ||
			    vm_private_page_io_try_acquire(backing) != 0) {
				vm_private_page_ref(backing);
				wait_backing = backing;
				break;
			}

			/*
			 * A fault may have completed between the first observation
			 * and our exclusive acquisition.
			 */
			if (!private_page_targets_source(backing, source_id)) {
				vm_private_page_io_release(backing);
				vm_private_page_put(backing);
				continue;
			}
			if (backing->mappings == NULL)
				HAL_FATAL("tracked swapped backing has no mapping");
			pin_backing_mappings(backing);
			accounting_page = backing->mappings;
			selected = backing;
			break;
		}
		mutex_unlock(&reclaim_lock);
		vm_metadata_leave();
		if (scan_error != 0)
			return scan_error;

		/* Pages the selected backing in and goes around again. */
		if (selected != NULL) {
			error = vm_private_page_in_owned(selected, accounting_page);
			drain_mapping_pins_release(selected);
			vm_private_page_io_release(selected);
			vm_private_page_put(selected);
			if (error != 0)
				return error;
			if (cancel != NULL) {
				error = cancel(cancel_argument);
				if (error != 0)
					return error;
			}
			continue;
		}

		/* Waits for a busy backing to become idle. */
		if (wait_backing != NULL) {
			error = vm_private_page_wait_idle_cancelable(wait_backing,
			    cancel, cancel_argument);
			vm_private_page_put(wait_backing);
			if (error != 0 && error != EAGAIN)
				return error;
			sched_yield();
			continue;
		}

		/*
		 * A slot allocator which crossed begin_drain may not have published
		 * its backing token yet.  Source counters close that
		 * scan/publication window; yield until it finishes, then scan
		 * again.
		 */
		backend = swap_system_backend();
		if (backend == NULL)
			return ENXIO;
		error = swap_source_get_stats(backend, source_id, &source_stats);
		if (error != 0)
			return error;
		if (source_stats.state != SWAP_SOURCE_STATE_DRAINING)
			return EBUSY;
		if (source_stats.allocated_slots == 0 &&
		    source_stats.inflight == 0)
			return 0;
		sched_yield();
	}
}

/*
 * Reads every page of a draining swap source back into memory.
 */
int
vm_reclaim_drain_swap_source(
	unsigned source_id)
{
	int error;

	error = vm_reclaim_drain_swap_source_cancelable(source_id, NULL, NULL);

	/* Reports why the drain failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Reclaims one resident private page, avoiding a given mapping.
 *
 * Unreferenced pages are preferred over recently accessed ones.
 * Reports EAGAIN when no candidate is available.
 */
int
vm_reclaim_private_one(
	struct vm_page *avoid)
{
	struct vm_private_page *selected;
	unsigned pass;
	int file_candidate;
	struct vm_private_page *backing;
	unsigned state_flags;
	unsigned long irq;
	uint32_t flags;
	int error;

	selected = NULL;
	file_candidate = 0;

	/* The first pass skips accessed pages; the second takes any. */
	vm_metadata_enter();
	mutex_lock(&reclaim_lock);

	for (pass = 0; pass < 2; pass++) {
		for (backing = page_queue; backing != NULL;
		     backing = backing->queue_next) {
			/* Skips a backing that is absent, busy, wired or avoided. */
			irq = spin_lock_irqsave(&backing->state_lock);
			state_flags = backing->flags;
			spin_unlock_irqrestore(&backing->state_lock, irq);
			if ((state_flags & VM_PAGE_RESIDENT) == 0 ||
			    (state_flags & VM_PAGE_BUSY) != 0 ||
			    backing_wired_or_avoided(backing, avoid))
				continue;

			/* Leaves a recently accessed page to the second pass. */
			flags = backing_pte_flags(backing);
			if (pass == 0 && (flags & HAL_PAGE_ACCESSED))
				continue;

			/* Takes the backing and pins its mappings for the reclaim. */
			file_candidate = backing->mappings != NULL &&
			    backing->mappings->region->backing == VM_BACKING_FILE;
			if (vm_private_page_io_try_acquire(backing) != 0)
				continue;
			pin_backing_mappings(backing);
			selected = backing;
			goto out;
		}
	}
out:

	mutex_unlock(&reclaim_lock);
	vm_metadata_leave();

	/* Reclaims the selected backing outside the locks. */
	if (selected != NULL) {
		error = reclaim_backing_owned(selected, file_candidate);
		return error;
	}

	/* Reports no candidate. */
	return EAGAIN;
}

/*
 * Helpers
 */

/*
 * Finds or creates the admitted shared object, taking a mapping reference.
 *
 * A writable file becomes the object's writeback file when it has none.
 * An object in final teardown is waited for and the lookup retried.
 */
static int
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

		error = object_reference_locked(object, file, cache_only);
		if (error == 0)
			*result = object;

		registry_unlock(enabled);

		return error;
	}
	registry_unlock(enabled);

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
			destroy_object(object);
			return EOPNOTSUPP;
		}

		error = file_open_resolved(&file->f_path, O_RDONLY | O_NOFOLLOW, &object->file);
		if (error != 0) {
			destroy_object(object);
			return error;
		}

		/* Restarts when the handle no longer names the inode it did. */
		if (file_vm_inode(object->file) != inode) {
			destroy_object(object);
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

			/* Waits out the transition the existing object is in, then looks again. */
			refcount_get(&existing->refs);
			registry_unlock(enabled);
			destroy_object(object);
			error = object_wait_registry_transition(existing, sequence);
			if (error != 0 && error != EAGAIN)
				return error;

			goto retry_lookup;
		}

		error = object_reference_locked(existing, file, cache_only);
		registry_unlock(enabled);
		destroy_object(object);

		if (error == 0)
			*result = existing;

		return error;
	}

	/* Refuses optional admission before publishing any registry ownership. */
	if (cache_only && cache_objects == VM_OBJECT_CACHE_OBJECTS) {
		registry_unlock(enabled);
		destroy_object(object);
		return ENOMEM;
	}

	/* Refuses an unrepresentable registry identity before publication. */
	if (object_registry_generation == UINT64_MAX) {
		registry_unlock(enabled);
		destroy_object(object);
		return EOVERFLOW;
	}
	object->registry_generation = ++object_registry_generation;

	/* Publishes the new object with the inode's current end of file. */
	object_initialize_eof_locked(object, inode);
	if (cache_only) {
		object->flags |= VM_OBJECT_CACHE_REFERENCE;
		object->active_operations++;
		cache_objects++;
	}

	/* Links the object into the registry. */
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

	error = waitq_sleep(&object->registry_waitq, &object->lock, sequence, 0, 0);

	spin_unlock_irqrestore(&object->lock, irq);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
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

	/* Keeps the waiter's lifetime pin through its final wakeup. */
	object->registry_waiters--;
	wake = object->registry_waiters == 0;
	registry_unlock(enabled);

	if (wake)
		object_wake_registry_waiters(object);
	if (refcount_put(&object->refs))
		destroy_object(object);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
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

	/* Sleeps until the object has no registry waiter left. */
	for (;;) {
		/* Returns as soon as the waiter count reaches zero. */
		enabled = registry_lock();
		if (object->registry_waiters == 0) {
			registry_unlock(enabled);
			return;
		}

		/* Waits for the registry to report progress on this object. */
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

	/* Refuses to start work on an object that is tearing down. */
	enabled = registry_lock();
	if ((object->flags & VM_OBJECT_DETACHING) != 0) {
		registry_unlock(enabled);
		return EBUSY;
	}

	/* Counts this operation so that teardown waits for it. */
	object->active_operations++;
	if (object->active_operations == 0)
		HAL_FATAL("VM object operation counter overflow");
	registry_unlock(enabled);

	/* Reports the started operation. */
	return 0;
}

/* Counts an operation down, waking a final put waiting for the last. */
static void
object_operation_end(
	struct vm_object *object)
{
	int wake;
	bool enabled;

	/* Drops the operation, pinning the object when it was the last one. */
	enabled = registry_lock();
	if (object->active_operations == 0)
		HAL_FATAL("VM object operation counter underflow");

	object->active_operations--;

	wake = object->active_operations == 0;
	if (wake)
		refcount_get(&object->refs);

	registry_unlock(enabled);

	/* Releases whoever waited for the object to fall idle. */
	if (wake) {
		object_wake_registry_waiters(object);
		if (refcount_put(&object->refs))
			destroy_object(object);
	}
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

	/* Asks the HAL for one page of ordinary memory. */
	memset(memory, 0, sizeof(*memory));
	error = hal_pmem_alloc(&request, memory);

	/* Returns a short allocation rather than leaving it behind. */
	if (error != HAL_OK && memory->size != 0) {
		if (hal_pmem_free(memory) != HAL_OK)
			HAL_FATAL("VM page allocation rollback failed");
	}

	/* Reports why the allocation failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Finds the cached page at an offset. */
static struct vm_object_page *
find_page(
	struct vm_object *object,
	off_t offset)
{
	struct vm_object_page *page;

	/* Traverses the balanced index without scanning unrelated cache pages. */
	page = object->page_index;
	while (page != NULL) {
		if (page->offset == offset)
			return page;
		if (offset < page->offset)
			page = page->index_left;
		else
			page = page->index_right;
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

/* Tests whether a page overlaps a byte range. */
static int
page_overlaps(
	const struct vm_object_page *page,
	uint64_t start,
	uint64_t end)
{
	uint64_t page_start;
	uint64_t page_end;

	/* A page without an offset covers nothing. */
	if (page->offset < 0)
		return 0;

	/* Spans the page and tests it against both ends of the range. */
	page_start = (uint64_t)page->offset;
	page_end = page_start + PAGE_SIZE;
	if (page_end <= start)
		return 0;
	if (page_start >= end)
		return 0;

	/* Reports an overlap. */
	return 1;
}

/* Tests indexed dirty ownership; the caller holds the object lock. */
static int
object_has_dirty_pages_locked(
	struct vm_object *object)
{
	return object->dirty_pages != NULL;
}

/* Tests whether any page or orphan is busy or held; the caller holds the object lock. */
static int
object_has_busy_pages_locked(
	const struct vm_object *object)
{
	const struct vm_object_page *page;

	/* Looks for a live page that is under I/O or held. */
	for (page = object->pages; page != NULL; page = page->next) {
		/* A page another transaction owns or is writing out is busy. */
		if (page->flags & (VM_OBJECT_PAGE_BUSY | VM_OBJECT_PAGE_WRITEBACK))
			return 1;

		/* So is a page some reader is still holding. */
		if (page->hold_count != 0)
			return 1;
	}

	/* Looks for an orphan that is still finishing its writeback. */
	for (page = object->orphan_pages; page != NULL; page = page->next) {
		/*
		 * An orphan is busy on the same terms: owned by another
		 * transaction, still being written out, or still held.
		 */
		if ((page->flags & (VM_OBJECT_PAGE_BUSY | VM_OBJECT_PAGE_WRITEBACK)) != 0 ||
		    page->hold_count != 0)
			return 1;
	}

	/* Reports an object whose pages are all idle. */
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
	io_flags = FILE_IO_VM_OBJECT | FILE_IO_CONTENT_CHANGE | FILE_IO_DRAIN;
	if (inode_io_owned)
		io_flags |= FILE_IO_INODE_IO_OWNED;

	error = file_io_begin(object->write_file, FILE_IO_PWRITE, page->offset, io_flags, &io);
	if (error != 0)
		return error;

	io.context.content_generation = page->write_dirty_generation;
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
	/* Takes the page out of the object's dirty index. */
	object_page_dirty_unlink(page);

	/* Gives the delayed write credit back to the budget that granted it. */
	if (page->writeback_budget != NULL) {
		if (writeback_budget_clean == NULL)
			HAL_FATAL("missing writeback credit owner");
		writeback_budget_clean(page->writeback_budget, PAGE_SIZE);
		page->writeback_budget = NULL;
	}

	/* Records the page as clean. */
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

	if (page->dirty_linked)
		HAL_FATAL("destroying indexed dirty VM object page");

	/* A committed orphan/anonymous discard also retires optional ownership. */
	if (page->writeback_budget != NULL)
		clear_page_dirty_locked(page);

	release_object_page_storage(page);

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

	/* A mapped object, or one holding a failure, is never destroyable. */
	if (object->mapping_count != 0 || object->writeback_error != 0)
		return 0;

	/* Requires every page to be clean and idle. */
	irq = spin_lock_irqsave(&object->lock);

	result = !object_has_dirty_pages_locked(object) && !object_has_busy_pages_locked(object);

	spin_unlock_irqrestore(&object->lock, irq);

	/* Reports whether the object may be destroyed. */
	return result;
}

/* Removes a destroyable object from the registry; the caller holds the registry lock. */
static int
unlink_object_locked(
	struct vm_object *object)
{
	struct vm_object **link;

	/* Refuses to unlink an object that still owns work. */
	if (!object_can_destroy(object))
		HAL_FATAL("destroying unsynchronized VM object");

	/* Finds the link that holds the object and splices it out. */
	for (link = &shared_objects; *link != NULL; link = &(*link)->next) {
		if (*link == object) {
			*link = object->next;
			object->next = NULL;

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
			return 1;
		}
	}

	/* Reports an object the registry did not hold. */
	return 0;
}

/* Frees an object, its pages, its files, and its commitment. */
static void
destroy_object(
	struct vm_object *object)
{
	struct vm_object_page *page;

	/* Frees the live pages, taking each out of the dirty index first. */
	page = object->pages;
	while (page != NULL) {
		object->pages = page->next;
		object_page_dirty_unlink(page);
		free_object_page(page);
		page = object->pages;
	}

	/* Frees the pages a resize orphaned. */
	page = object->orphan_pages;
	while (page != NULL) {
		object->orphan_pages = page->next;
		free_object_page(page);
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

/* Keeps an unmapped object in the registry until its writeback succeeds. */
static void
retain_object(
	struct vm_object *object,
	int error)
{
	unsigned long irq;

	/* Only an unmapped object is retained. */
	if (object->mapping_count != 0)
		HAL_FATAL("retaining referenced VM object");

	/* Names a reason when the caller had none. */
	if (error == 0) {
		if (object_has_busy_pages(object))
			error = EBUSY;
		else
			error = EIO;
	}

	/* Records the failure and marks the object as retained. */
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

/* Releases the pages a content transaction took; the caller holds the object lock. */
static void
content_release_pages_locked(
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
			memcpy((uint8_t *)page->pmem.vaddr +
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
			object_page_dirty_mark(page);
		}

		/* Releases the taken pages and closes the transaction on the object. */
		content_release_pages_locked(object, content->generation);
		object->flags &= ~VM_OBJECT_CONTENT;
		object->content_generation = next_generation(object);
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

	(void)next_inode_content_generation_locked(inode);
	waitq_wake_all(&inode->i_vm_waitq);

	spin_unlock_irqrestore(&inode->i_vm_lock, inode_irq);

	registry_unlock(enabled);

	/*
	 * Releases the object outside every lock, waking whoever waited on
	 * the registry for this transaction to end.
	 */
	if (wake_registry) {
		object_wake_registry_waiters(object);
		if (refcount_put(&object->refs))
			destroy_object(object);
	}

	/* Empties the descriptor so a second finish does nothing. */
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

	enabled = registry_lock();
	inode_irq = spin_lock_irqsave(&inode->i_vm_lock);

	/* Traps when the inode no longer carries this resize. */
	if (!inode->i_vm_resize_active ||
	    inode->i_vm_resize_generation != resize->generation)
		HAL_FATAL("inode lost VM object resize transaction");

	/* Takes the generation the settled size is published under. */
	stable_generation = next_inode_resize_generation_locked(inode);

	/* Finds the object this resize has to settle, if one exists. */
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
				object_page_index_insert(object, page);
				object_page_dirty_link(page);
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

	registry_unlock(enabled);

	/*
	 * Releases the object outside every lock, waking whoever waited on
	 * the registry for this resize to end.
	 */
	if (wake_registry) {
		object_wake_registry_waiters(object);
		if (refcount_put(&object->refs))
			destroy_object(object);
	}

	/* Frees the orphans the resize left behind, outside every lock. */
	while (free_pages != NULL) {
		page = free_pages;
		free_pages = page->next;
		free_object_page(page);
	}

	/* Empties the descriptor so a second finish does nothing. */
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

	/* Rejects a call that names no page to copy through. */
	if (page == NULL)
		return EINVAL;

	/* Rejects a call that names no buffer to copy from or into. */
	if (buffer == NULL)
		return EINVAL;

	object = page->owner;

	/* Rejects a page that no object owns, because it has no lock to take. */
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
		memcpy((uint8_t *)page->pmem.vaddr + offset, buffer, length);
		object_page_dirty_mark(page);
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

	/* Looks for a wired mapping on any page of the range. */
	for (page = object->pages; page != NULL; page = page->next) {
		if (!page_overlaps(page, start, end))
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
	int error;

	/* Preserves the ordinary entry point without reserving a second payload. */
	error = vm_object_sync_range_buffer(object, offset, size, flags, detaching,
	    resize_owner, resize_target, NULL, 0);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
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

	/* A detaching object must have no mapping left anywhere. */
	if (detaching) {
		/* Traps on the first page a mapping still refers to. */
		for (page = object->pages; page != NULL; page = page->next) {
			if (page->mappings != NULL || page->mapping_count != 0)
				HAL_FATAL("detaching VM object still has reverse mappings");
		}
	}

	/* Stamps this pass, so a page is taken at most once by it. */
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

		/* Looks for the next page of the range this pass has not taken. */
		irq = spin_lock_irqsave(&object->lock);
		for (page = object->pages; page != NULL; page = page->next) {
			/* Skips a page outside the range this call names. */
			if (!page_overlaps(page, start, end))
				continue;

			/* Skips a page this pass has already taken. */
			if (page->write_generation == sync_generation)
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

		if ((observed & HAL_PAGE_DIRTY) != 0) {
			object_page_dirty_mark(candidate);
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
		object_record_writeback_error_locked(object, first_error);

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
			clear_page_dirty_locked(page);

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
			object_page_index_remove(object, page);
			object_page_dirty_unlink(page);
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
			object_page_index_remove(object, page);
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

/* Indexes resident dirty data, excluding resize-orphaned storage. */
static void
object_page_dirty_link(
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
static void
object_page_dirty_unlink(
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
static void
object_page_dirty_mark(
	struct vm_object_page *page)
{
	/* A page that has been written is no longer a prefetch of the file. */
	object_prefetch_retire(page);

	/* Records the new contents as not yet written back. */
	page->flags |= VM_OBJECT_PAGE_DIRTY;

	/* Stamps the change, so a write-back knows what it cleaned. */
	page->dirty_generation = next_generation(page->owner);

	/* Publishes the page to the owner's dirty list. */
	object_page_dirty_link(page);
}

/*
 * Tries one bounded clean-pressure pass before refusing optional cache growth.
 */
static int
object_memory_reserve(
	enum cache_memory_kind kind,
	int optional)
{
	int error;

	/*
	 * Keeps standalone VM fixtures usable without installing a budget
	 * owner.
	 */
	if (cache_memory_reserve == NULL)
		return 0;

	/* Asks the budget owner for one page. */
	error = cache_memory_reserve(kind, PAGE_SIZE, optional);
	if (error == 0 || !optional || cache_memory_reclaim == NULL)
		return error;

	/*
	 * Reclaims only independent idle ownership; the current read stays
	 * active.
	 */
	if (cache_memory_reclaim(PAGE_SIZE) == 0)
		return error;

	error = cache_memory_reserve(kind, PAGE_SIZE, optional);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Serializes descriptor ownership without sleeping or taking another lock. */
static bool
object_slab_lock(
	void)
{
	unsigned expected;
	bool enabled;

	/*
	 * Preserves interrupt state while acquiring the short metadata guard.
	 */
	enabled = hal_irq_disable != NULL ? hal_irq_disable() : false;
	expected = 0;
	while (!atomic_compare_exchange(&page_slab_lock, &expected, 1)) {
		expected = 0;
		hal_compiler_barrier();
	}

	return enabled;
}

/* Releases metadata ownership before restoring interrupts. */
static void
object_slab_unlock(
	bool enabled)
{
	atomic_store_release(&page_slab_lock, 0);

	if (enabled && hal_irq_enable != NULL)
		hal_irq_enable();
}

/* Constructs a private metadata slab outside all VM and slab locks. */
static struct vm_page_slab *
object_slab_alloc(
	int optional)
{
	struct hal_pmem memory;
	struct vm_page_slab *slab;
	struct vm_object_page *page;
	size_t header;
	size_t offset;

	/*
	 * Reserves the entire physical page, including unused descriptor
	 * capacity.
	 */
	if (object_memory_reserve(CACHE_MEMORY_FILE_META, optional) != 0)
		return NULL;

	memset(&memory, 0, sizeof(memory));

	if (alloc_vm_page(&memory) != HAL_OK) {
		if (cache_memory_cancel != NULL)
			cache_memory_cancel(CACHE_MEMORY_FILE_META, PAGE_SIZE);
		return NULL;
	}

	if (memory.size != PAGE_SIZE || memory.vaddr == NULL)
		HAL_FATAL("unexpected VM descriptor slab allocation");

	/*
	 * Formats aligned descriptors without consuming the fixed kernel heap.
	 */
	slab = memory.vaddr;
	memset(slab, 0, PAGE_SIZE);
	slab->memory = memory;

	/*
	 * Carves the page after the slab header into descriptors and puts
	 * every one of them on the slab's free list.  The header size is
	 * rounded up so the first descriptor is correctly aligned, and the
	 * walk stops before the last descriptor that would not fit whole.
	 */
	header = (sizeof(*slab) + __alignof__(struct vm_object_page) - 1U) & ~(size_t)(__alignof__(struct vm_object_page) - 1U);
	for (offset = header;
	     offset + sizeof(*page) <= PAGE_SIZE;
	     offset += sizeof(*page)) {
		page = (struct vm_object_page *)((char *)slab + offset);
		page->metadata_slab = slab;
		page->next = slab->free_pages;
		slab->free_pages = page;
	}

	if (slab->free_pages == NULL)
		HAL_FATAL("VM page descriptor exceeds metadata slab");

	if (cache_memory_commit != NULL)
		cache_memory_commit(CACHE_MEMORY_FILE_META, PAGE_SIZE);

	return slab;
}

/* Borrows a descriptor, adding one private slab only when no slot is free. */
static struct vm_object_page *
object_descriptor_alloc(
	int optional)
{
	struct vm_page_slab *slab;
	struct vm_page_slab *prepared;
	struct vm_object_page *page;
	bool enabled;

	/*
	 * Finds existing capacity without entering an allocator under the
	 * guard.
	 */
	enabled = object_slab_lock();
	slab = page_slabs;
	if (slab == NULL) {
		object_slab_unlock(enabled);

		prepared = object_slab_alloc(optional);
		if (prepared == NULL)
			return NULL;

		enabled = object_slab_lock();

		prepared->next = page_slabs;

		if (page_slabs != NULL)
			page_slabs->previous = prepared;

		page_slabs = prepared;
		slab = prepared;
	}

	/*
	 * Holds the slab through the descriptor's complete private/resident
	 * lifetime.
	 */
	page = slab->free_pages;
	slab->free_pages = page->next;
	slab->used++;

	/* Takes a slab that has run out off the head of the list. */
	if (slab->free_pages == NULL) {
		page_slabs = slab->next;
		if (page_slabs != NULL)
			page_slabs->previous = NULL;
		slab->next = NULL;
	}

	object_slab_unlock(enabled);

	memset(page, 0, sizeof(*page));
	page->metadata_slab = slab;

	return page;
}

/*
 * Returns one descriptor and retires its slab only after the last owner leaves.
 */
static void
object_descriptor_free(
	struct vm_object_page *page)
{
	struct vm_page_slab *slab;
	struct hal_pmem memory;
	bool enabled;

	/*
	 * Removes empty slabs from allocation visibility before physical
	 * retirement.
	 */
	slab = page->metadata_slab;
	memset(&memory, 0, sizeof(memory));
	enabled = object_slab_lock();
	if (slab == NULL || slab->used == 0)
		HAL_FATAL("VM page descriptor ownership underflow");

	/* A slab that had no free descriptor becomes allocatable again. */
	if (slab->free_pages == NULL) {
		slab->previous = NULL;
		slab->next = page_slabs;
		if (page_slabs != NULL)
			page_slabs->previous = slab;
		page_slabs = slab;
	}

	/* Returns the descriptor to its slab. */
	page->next = slab->free_pages;
	slab->free_pages = page;
	slab->used--;

	/* An emptied slab leaves allocation and gives its memory back. */
	if (slab->used == 0) {
		if (slab->previous != NULL)
			slab->previous->next = slab->next;
		else
			page_slabs = slab->next;
		if (slab->next != NULL)
			slab->next->previous = slab->previous;
		memory = slab->memory;
	}
	object_slab_unlock(enabled);

	/* Releases physical metadata and its credits only outside the guard. */
	if (memory.size != 0) {
		if (hal_pmem_free(&memory) != HAL_OK)
			HAL_FATAL("VM descriptor slab retirement failed");
		if (cache_memory_release != NULL)
			cache_memory_release(CACHE_MEMORY_FILE_META, PAGE_SIZE);
	}
}

/* Allocates a fully charged private frame before any page index publication. */
static struct vm_object_page *
alloc_object_page(
	struct vm_object *object,
	off_t offset,
	int optional)
{
	struct vm_object_page *page;

	/*
	 * Reserves optional cache growth or records mandatory mapping
	 * commitment.
	 */
	if (object_memory_reserve(CACHE_MEMORY_FILE_DATA, optional) != 0)
		return NULL;

	/* Returns the reservation when no descriptor is free. */
	page = object_descriptor_alloc(optional);
	if (page == NULL) {
		if (cache_memory_cancel != NULL)
			cache_memory_cancel(CACHE_MEMORY_FILE_DATA, PAGE_SIZE);
		return NULL;
	}

	/* Returns the descriptor and the reservation when no frame is free. */
	if (alloc_vm_page(&page->pmem) != HAL_OK) {
		object_descriptor_free(page);
		if (cache_memory_cancel != NULL)
			cache_memory_cancel(CACHE_MEMORY_FILE_DATA, PAGE_SIZE);
		return NULL;
	}

	if (page->pmem.size != PAGE_SIZE || page->pmem.vaddr == NULL)
		HAL_FATAL("unexpected VM object frame allocation");

	/*
	 * Publishes resident ownership while the initialized descriptor stays
	 * private.
	 */
	page->owner = object;
	page->offset = offset;
	page->flags = VM_OBJECT_PAGE_BUSY;

	if (cache_memory_commit != NULL)
		cache_memory_commit(CACHE_MEMORY_FILE_DATA, PAGE_SIZE);

	return page;
}

/*
 * Frees private or detached frame storage without changing publication counts.
 */
static void
release_object_page_storage(
	struct vm_object_page *page)
{
	/*
	 * Ends optional attribution before the descriptor or its storage can be
	 * reused.
	 */
	object_prefetch_retire(page);

	/* Returns data credits only after the physical frame has retired. */
	if (hal_pmem_free(&page->pmem) != HAL_OK)
		HAL_FATAL("VM object frame retirement failed");

	if (cache_memory_release != NULL)
		cache_memory_release(CACHE_MEMORY_FILE_DATA, PAGE_SIZE);

	object_descriptor_free(page);
}

/* Reports the empty subtree height without dereferencing a missing child. */
static unsigned
page_index_height(
	const struct vm_object_page *page)
{
	if (page == NULL)
		return 0;

	return page->index_height;
}

/* Refreshes one node after its children change. */
static void
page_index_update(
	struct vm_object_page *page)
{
	unsigned left;
	unsigned right;

	/* Derives height from already balanced child subtrees. */
	left = page_index_height(page->index_left);
	right = page_index_height(page->index_right);
	page->index_height = 1U + (left > right ? left : right);
}

/* Rotates a right-heavy subtree without changing page identity. */
static struct vm_object_page *
page_index_rotate_left(
	struct vm_object_page *page)
{
	struct vm_object_page *root;

	/* Transfers the middle subtree before refreshing both heights. */
	root = page->index_right;
	page->index_right = root->index_left;
	root->index_left = page;
	page_index_update(page);
	page_index_update(root);
	return root;
}

/* Rotates a left-heavy subtree without changing page identity. */
static struct vm_object_page *
page_index_rotate_right(
	struct vm_object_page *page)
{
	struct vm_object_page *root;

	/* Transfers the middle subtree before refreshing both heights. */
	root = page->index_left;
	page->index_left = root->index_right;
	root->index_right = page;
	page_index_update(page);
	page_index_update(root);
	return root;
}

/* Restores the AVL height bound after one insertion or removal. */
static struct vm_object_page *
page_index_balance(
	struct vm_object_page *page)
{
	unsigned left;
	unsigned right;

	/* Measures the changed children before choosing a rotation. */
	page_index_update(page);
	left = page_index_height(page->index_left);
	right = page_index_height(page->index_right);

	/* Corrects a left-heavy subtree, including its inner-heavy case. */
	if (left > right + 1U) {
		/*
		 * The left subtree is at least two levels taller than the
		 * right one, so an insertion or a removal on the left has
		 * left this node out of balance.
		 */

		/*
		 * When the taller side of that left child is its own right
		 * subtree, the imbalance is the inner-heavy case: one right
		 * rotation here would only move it.  Rotating the child left
		 * first turns it into the outer-heavy case.
		 */
		if (page_index_height(page->index_left->index_right) >
		    page_index_height(page->index_left->index_left))
			page->index_left = page_index_rotate_left(page->index_left);

		/* Rotates right, which lifts the left child into this place. */
		page = page_index_rotate_right(page);
	} else if (right > left + 1U) {
		/*
		 * The mirror case: the right subtree is at least two levels
		 * taller than the left one.
		 */

		/*
		 * When the taller side of that right child is its own left
		 * subtree, this is the inner-heavy case.  Rotating the child
		 * right first turns it into the outer-heavy case.
		 */
		if (page_index_height(page->index_right->index_left) >
		    page_index_height(page->index_right->index_right))
			page->index_right = page_index_rotate_right(page->index_right);

		/* Rotates left, which lifts the right child into this place. */
		page = page_index_rotate_left(page);
	}

	/* Returns the balanced subtree root to its parent. */
	return page;
}

/* Inserts one unique page while preserving ordered child keys. */
static struct vm_object_page *
page_index_insert(
	struct vm_object_page *root,
	struct vm_object_page *page)
{
	/* Publishes a new leaf or rejects an inconsistent duplicate owner. */
	if (root == NULL)
		return page;
	if (root->offset == page->offset)
		HAL_FATAL("duplicate VM object page index key");

	/* Descends through one subtree and repairs heights on return. */
	if (page->offset < root->offset)
		root->index_left = page_index_insert(root->index_left, page);
	else
		root->index_right = page_index_insert(root->index_right, page);

	root = page_index_balance(root);

	return root;
}

/* Extracts the successor node itself, preserving all descriptor identities. */
static struct vm_object_page *
page_index_extract_min(
	struct vm_object_page *root,
	struct vm_object_page **minimum)
{
	/* Removes the smallest node and returns its surviving right subtree. */
	if (root->index_left == NULL) {
		*minimum = root;
		return root->index_right;
	}

	/* Repairs ancestors after unlinking the successor. */
	root->index_left = page_index_extract_min(root->index_left, minimum);
	root = page_index_balance(root);

	return root;
}

/* Removes the exact descriptor without copying another page's contents. */
static struct vm_object_page *
page_index_remove(
	struct vm_object_page *root,
	struct vm_object_page *page)
{
	struct vm_object_page *successor;
	struct vm_object_page *right;

	/* Detects list/index divergence at the mutation which caused it. */
	if (root == NULL)
		HAL_FATAL("missing VM object page index key");

	/* Finds the exact key, preserving a balanced path after removal. */
	if (page->offset < root->offset) {
		root->index_left = page_index_remove(root->index_left, page);
	} else if (page->offset > root->offset) {
		root->index_right = page_index_remove(root->index_right, page);
	} else {
		/* A node with at most one child is replaced by that child. */
		if (root != page)
			HAL_FATAL("VM object page index identity mismatch");
		if (root->index_left == NULL)
			return root->index_right;
		if (root->index_right == NULL)
			return root->index_left;

		/* Otherwise the in-order successor takes the node's place. */
		right = page_index_extract_min(root->index_right, &successor);
		successor->index_left = root->index_left;
		successor->index_right = right;
		root = successor;
	}

	root = page_index_balance(root);

	return root;
}

/* Publishes a descriptor under the already held object lock. */
static void
object_page_index_insert(
	struct vm_object *object,
	struct vm_object_page *page)
{
	/* Starts with an independent leaf before linking it into the object. */
	page->index_left = NULL;
	page->index_right = NULL;
	page->index_height = 1;
	object->page_index = page_index_insert(object->page_index, page);
}

/* Detaches a descriptor before it becomes an orphan or is retired. */
static void
object_page_index_remove(
	struct vm_object *object,
	struct vm_object_page *page)
{
	/*
	 * Removes the key and clears links before another owner can reuse them.
	 */
	object->page_index = page_index_remove(object->page_index, page);
	page->index_left = NULL;
	page->index_right = NULL;
	page->index_height = 0;
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

	/* Reports the reference published with its registry ownership. */
	return 0;
}

/* Tests the bounded clean retention policy under the registry lock. */
static int
object_cache_retainable(
	struct vm_object *object)
{
	int retainable;

	/*
	 * Shared physical credits now bound clean retention across all objects.
	 */
	if ((object->flags & VM_OBJECT_CACHE_REFERENCE) == 0)
		return 0;

	retainable = object_can_destroy(object);

	return retainable;
}

/*
 * Unlinks one idle cache under the registry lock, then closes it outside locks.
 */
static int
object_cache_evict_one(
	struct mount *mount)
{
	struct vm_object *object;
	bool enabled;

	/*
	 * Leaves every mandatory mapping, operation, pin and failure owner
	 * intact.
	 */
	enabled = registry_lock();

	for (object = shared_objects; object != NULL; object = object->next) {
		/* Skips an object anything still owns. */
		if ((object->flags & VM_OBJECT_CACHE_REFERENCE) == 0 ||
		    (object->flags & (VM_OBJECT_DETACHING | VM_OBJECT_RESIZING |
		    VM_OBJECT_CONTENT | VM_OBJECT_RETAINED_WRITEBACK)) != 0 ||
		    object->active_operations != 0 || object->registry_waiters != 0 ||
		    refcount_load(&object->refs) != 1 || !object_can_destroy(object))
			continue;

		/* Skips an object outside the mount the caller named. */
		if (mount != NULL && object->file->f_path.p_mount != mount &&
		    (object->write_file == NULL ||
		    object->write_file->f_path.p_mount != mount))
			continue;

		/* Takes the object out of the registry. */
		if (!unlink_object_locked(object))
			HAL_FATAL("VM cache eviction lost registry entry");
		break;
	}

	registry_unlock(enabled);

	/* Reports that the cache held nothing evictable. */
	if (object == NULL)
		return 0;

	/*
	 * Releases the sole registry/cache reference after closing admission.
	 */
	if (!refcount_put(&object->refs))
		HAL_FATAL("detached VM cache retained an unknown reference");

	destroy_object(object);

	/* Reports one complete eviction. */
	return 1;
}

/*
 * Frees only unpublished preparation frames, which were never counted resident.
 */
static void
object_cache_discard_prepared(
	struct vm_object_page **pages,
	unsigned count)
{
	unsigned index;

	/* Releases every successfully prepared frame and its metadata. */
	for (index = 0; index < count; index++) {
		release_object_page_storage(pages[index]);
	}
}

/*
 * Reads an absent bounded run and publishes only fully initialized cache pages.
 */
static int
object_cache_read_missing(
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

	/* Leaves already resident pages to the existing coherent fault path. */
	*handled = 0;
	*stop = 0;
	*result = 0;
	base = offset & ~(off_t)(PAGE_SIZE - 1U);
	in_page = (size_t)(offset - base);
	irq = spin_lock_irqsave(&object->lock);

	if ((object->flags & VM_OBJECT_CACHE_REFERENCE) == 0 ||
	    find_page(object, base) != NULL) {
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
	 */
	wanted = KERN_IO_BATCH_MAX - in_page;

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
		if (find_page(object, base + (off_t)index * PAGE_SIZE) != NULL) {
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
	if (io_pool_borrow != NULL &&
	    io_pool_release != NULL)
		scratch = io_pool_borrow(transfer, &capacity);

	prepared = 0;
	needed = (unsigned)((transfer + PAGE_SIZE - 1U) / PAGE_SIZE);
	publish = scratch != NULL &&
		capacity >= transfer &&
		needed <= VM_OBJECT_CACHE_PAGES;

	/* Allocates all private frames before any busy page is made visible. */
	while (publish && prepared < needed) {
		page = alloc_object_page(object, base + (off_t)prepared * PAGE_SIZE, 1);
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
			if (find_page(object, pages[index]->offset) != NULL)
				publish = 0;
		}

		/* Publishes the whole run as resident pages of the object. */
		if (publish) {
			for (index = 0; index < prepared; index++) {
				pages[index]->next = object->pages;
				object->pages = pages[index];
				object_page_index_insert(object, pages[index]);
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
		object_cache_discard_prepared(pages, prepared);

		if (scratch != NULL)
			io_pool_release(scratch);

		/* A racing publication may now contain dirty shared data. */
		irq = spin_lock_irqsave(&object->lock);
		for (index = 0; (size_t)index * PAGE_SIZE < transfer; index++) {
			if (find_page(object, base + (off_t)index * PAGE_SIZE) != NULL) {
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
	memset(scratch, 0, transfer);
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
			memset(page->pmem.vaddr, 0, PAGE_SIZE);
			memcpy(page->pmem.vaddr, (uint8_t *)scratch +
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
		memcpy(buffer, (uint8_t *)scratch + in_page, copied);
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
	scratch = NULL;

	/* Uses the caller's staging, or borrows one from the pool. */
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
			error = write_page_data(object, page, logical_size, inode_io_owned);
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

			memcpy((char *)scratch + bytes, page->pmem.vaddr, amount);

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

/*
 * Credits disjoint forward demand spans while the object lock serializes
 * readers.
 */
static size_t
object_prefetch_consume(
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
		object_prefetch_retire(page);
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
static void
object_prefetch_retire(
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

/* Reports the protection a mapping should carry, read-only when copy-on-write. */
static uint32_t
mapping_prot(
	const struct vm_page *page)
{
	uint32_t prot;

	prot = page->region->prot;

	if (page->flags & VM_MAPPING_COW)
		prot &= ~HAL_SPACE_WRITE;

	return prot;
}

/* Unlinks a backing from the reclaim queue. */
static void
queue_remove(
	struct vm_private_page *backing)
{
	struct vm_private_page **link;

	link = &page_queue;

	while (*link != NULL && *link != backing)
		link = &(*link)->queue_next;

	if (*link == backing)
		*link = backing->queue_next;
}

/* Tests whether any mapping is wired, busy, or the one to avoid. */
static int
backing_wired_or_avoided(
	struct vm_private_page *backing,
	struct vm_page *avoid)
{
	struct vm_page *page;

	/* Looks for a mapping that reclaim must not take. */
	for (page = backing->mappings; page != NULL; page = page->private_next) {
		if (page == avoid ||
		    page->wire_count != 0 ||
		    (page->flags & VM_MAPPING_BUSY) != 0)
			return 1;
	}

	/* Reports a backing every mapping allows. */
	return 0;
}

/* Tests whether any mapping is wired. */
static int
backing_has_wired_mapping(
	struct vm_private_page *backing)
{
	struct vm_page *page;

	for (page = backing->mappings; page != NULL; page = page->private_next) {
		if (page->wire_count != 0)
			return 1;
	}
	return 0;
}

/* Tests whether any mapping is busy in a fault or reclaim. */
static int
backing_has_busy_mapping(
	struct vm_private_page *backing)
{
	struct vm_page *page;

	for (page = backing->mappings; page != NULL; page = page->private_next) {
		if ((page->flags & VM_MAPPING_BUSY) != 0)
			return 1;
	}
	return 0;
}

/* Combines the accessed and dirty PTE flags of every mapping. */
static uint32_t
backing_pte_flags(
	struct vm_private_page *backing)
{
	struct vm_page *page;
	uint32_t combined;
	uint32_t flags;

	/* Ors the hardware flags of every live mapping of this backing. */
	combined = 0;
	for (page = backing->mappings; page != NULL; page = page->private_next) {
		if (!(page->flags & VM_MAPPING_MAPPED))
			continue;
		flags = 0;
		if (hal_page_query(page->vm->space, (void *)page->address,
		    &flags) == HAL_OK)
			combined |= flags;
	}

	/* Reports the combined flags. */
	return combined;
}

/* Wakes the faults waiting on an address space. */
static void
vmspace_fault_wake(
	struct vmspace *vm)
{
	unsigned long irq;

	irq = spin_lock_irqsave(&vm->lock.guard);

	waitq_wake_all(&vm->fault_waitq);

	spin_unlock_irqrestore(&vm->lock.guard, irq);
}

/* Marks every mapping busy and holds its region; called with the metadata and reclaim locks held. */
static void
pin_backing_mappings(
	struct vm_private_page *backing)
{
	struct vm_page *page;

	/* Pins every mapping and the address space that holds it. */
	for (page = backing->mappings; page != NULL; page = page->private_next) {
		vmspace_ref(page->vm);
		mutex_lock(&page->vm->lock);

		/*
		 * Traps on a mapping that cannot be pinned: one a pass
		 * already owns, or one whose region hold count would wrap.
		 */
		if ((page->flags & VM_MAPPING_BUSY) != 0 ||
		    page->region->hold_count == (unsigned)-1)
			HAL_FATAL("VM reclaim mapping pin invariant failed");

		/*
		 * BUSY marks the mapping as owned by this reclaim pass, so a
		 * fault on it waits instead of racing.  The region hold keeps
		 * the address space from retiring it underneath.
		 */
		page->flags |= VM_MAPPING_BUSY;
		page->region->hold_count++;

		mutex_unlock(&page->vm->lock);
	}
}

/* Undoes pin_backing_mappings(), publishing unmapped PTEs; called with the metadata and reclaim locks held. */
static void
unpin_backing_mappings(
	struct vm_private_page *backing)
{
	struct vm_page *page;
	struct vmspace *vm;

	/* Releases the pin this reclaim pass took on every mapping. */
	for (page = backing->mappings; page != NULL; page = page->private_next) {
		/* Retires the reclaim state the pass left on the mapping. */
		vm = page->vm;
		mutex_lock(&vm->lock);

		/*
		 * A mapping this pass unmapped is published as no longer
		 * mapped, so the next fault reads it in again.
		 */
		if ((page->flags & VM_MAPPING_RECLAIM_UNMAPPED) != 0) {
			page->flags &= ~(VM_MAPPING_RECLAIM_UNMAPPED |
			    VM_MAPPING_RECLAIM_PROTECTED | VM_MAPPING_MAPPED);
		}

		/*
		 * Drops the write protection the pass installed, and then the
		 * ownership itself, which lets a waiting fault proceed.
		 */
		page->flags &= ~VM_MAPPING_RECLAIM_PROTECTED;
		page->flags &= ~VM_MAPPING_BUSY;

		/* Releases the region and wakes the faults that waited. */
		if (page->region->hold_count == 0)
			HAL_FATAL("VM reclaim region hold underflow");

		/* Releases the hold that kept the region from retiring. */
		page->region->hold_count--;

		/*
		 * Advances the address space's generation so a fault that
		 * sampled it before this change knows to look again.  Zero is
		 * skipped, because it is the never-sampled value.
		 */
		vm->generation++;
		if (vm->generation == 0)
			vm->generation++;

		vmspace_fault_wake(vm);

		mutex_unlock(&vm->lock);

		/* Never run final vmspace destruction under either VM metadata lock. */
		vmspace_put_deferred(vm);
	}
}

/* Revokes and unmaps every PTE of an owned backing, collecting the dirty flags. */
static int
unmap_backing_ptes(
	struct vm_private_page *backing,
	uint32_t *pte_flags)
{
	struct vm_page *page;
	int error;
	uint32_t flags;
	uint32_t readonly;

	error = 0;

	/* Rejects a missing result. */
	if (pte_flags == NULL)
		return EINVAL;
	*pte_flags = 0;

	/*
	 * A pre-unmap query is not a stable dirty snapshot: userspace can write
	 * between the query and shootdown.  First make every mapping read-only
	 * and wait for that protection change, then collect flags and remove
	 * the PTEs.
	 */
	for (page = backing->mappings; page != NULL; page = page->private_next) {
		flags = 0;

		if ((page->flags & VM_MAPPING_MAPPED) == 0)
			continue;

		readonly = mapping_prot(page) & ~HAL_SPACE_WRITE;
		if (readonly == 0) {
			/*
			 * No non-writable representation exists for a write-only
			 * PTE.  Synchronous unmap is the revoke; conservatively
			 * write it back.
			 */
			if (hal_page_unmap(page->vm->space,
			    (void *)page->address, PAGE_SIZE) != HAL_OK) {
				error = EIO;
				break;
			}
			page->flags |= VM_MAPPING_RECLAIM_UNMAPPED;
			*pte_flags |= HAL_PAGE_DIRTY;
			continue;
		}

		if (hal_page_prot_query(page->vm->space,
		    (void *)page->address, PAGE_SIZE, readonly, &flags) != HAL_OK) {
			error = EIO;
			break;
		}

		page->flags |= VM_MAPPING_RECLAIM_PROTECTED;

		*pte_flags |= flags;
	}
	if (error != 0)
		goto rollback;

	/* Removes the protected PTEs. */
	for (page = backing->mappings; page != NULL; page = page->private_next) {
		if ((page->flags & VM_MAPPING_MAPPED) == 0 ||
		    (page->flags & VM_MAPPING_RECLAIM_UNMAPPED) != 0)
			continue;

		if (hal_page_unmap(page->vm->space, (void *)page->address, PAGE_SIZE) != HAL_OK) {
			error = EIO;
			break;
		}

		page->flags |= VM_MAPPING_RECLAIM_UNMAPPED;
	}
	if (error == 0)
		return 0;

rollback:
	/* Roll back outside all VM locks.  A failed remap remains lazy. */
	for (page = backing->mappings; page != NULL; page = page->private_next) {
		if ((page->flags & VM_MAPPING_RECLAIM_UNMAPPED) != 0) {
			if (hal_page_map(page->vm->space, (void *)page->address,
			    backing->pmem.paddr, PAGE_SIZE,
			    mapping_prot(page)) == HAL_OK)
				page->flags &= ~(VM_MAPPING_RECLAIM_UNMAPPED |
				    VM_MAPPING_RECLAIM_PROTECTED);
			continue;
		}

		if ((page->flags & VM_MAPPING_RECLAIM_PROTECTED) == 0)
			continue;

		if (hal_page_prot(page->vm->space, (void *)page->address,
		    PAGE_SIZE, mapping_prot(page)) == HAL_OK) {
			page->flags &= ~VM_MAPPING_RECLAIM_PROTECTED;
			continue;
		}

		/* A stale read-only PTE would livelock a later write fault. */
		if (hal_page_unmap(page->vm->space, (void *)page->address, PAGE_SIZE) != HAL_OK)
			HAL_FATAL("VM reclaim protection rollback failed");

		page->flags |= VM_MAPPING_RECLAIM_UNMAPPED;
	}

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Restores the PTEs of a backing whose reclaim was abandoned. */
static void
rollback_backing_ptes(
	struct vm_private_page *backing)
{
	struct vm_page *page;

	for (page = backing->mappings; page != NULL; page = page->private_next) {
		/* A failed remap remains lazy until the next fault. */
		if ((page->flags & VM_MAPPING_RECLAIM_UNMAPPED) != 0) {
			if (hal_page_map(page->vm->space, (void *)page->address,
			    backing->pmem.paddr, PAGE_SIZE,
			    mapping_prot(page)) == HAL_OK)
				page->flags &= ~(VM_MAPPING_RECLAIM_UNMAPPED |
				    VM_MAPPING_RECLAIM_PROTECTED);
			continue;
		}

		if ((page->flags & VM_MAPPING_RECLAIM_PROTECTED) == 0)
			continue;

		/* Restores the protection reclaim took away from this mapping. */
		if (hal_page_prot(page->vm->space,
				  (void *)page->address,
				  PAGE_SIZE,
				  mapping_prot(page)) == HAL_OK) {
			page->flags &= ~VM_MAPPING_RECLAIM_PROTECTED;
			continue;
		}

		/* A stale read-only PTE is removed rather than left behind. */
		if (hal_page_unmap(page->vm->space,
				   (void *)page->address,
				   PAGE_SIZE) != HAL_OK)
			HAL_FATAL("VM reclaim PTE rollback failed");

		page->flags |= VM_MAPPING_RECLAIM_UNMAPPED;
	}
}

/* Writes an owned, unmapped backing to swap and frees its page. */
static int
swap_out_backing_owned(
	struct vm_private_page *backing)
{
	struct swap_backend *backend;
	struct hal_pmem memory;
	uint32_t slot;
	unsigned long irq;
	int slot_allocated;
	int error;

	backend = swap_system_backend();
	slot_allocated = 0;

	/* Allocates a slot and writes the page. */
	if (PAGE_SIZE != SWAP_PAGE_SIZE || backend == NULL) {
		error = ENOSPC;
	} else {
		error = swap_alloc_slot(backend, &slot);
		if (error == 0)
			slot_allocated = 1;
	}

	if (error == 0) {
		error = swap_write_page(backend,
					slot,
					(const void *)backing->pmem.vaddr);
	}

	/* A failure restores the PTEs and releases everything. */
	if (error != 0) {
		rollback_backing_ptes(backing);

		if (slot_allocated)
			swap_free_slot(backend, slot);

		vm_metadata_enter();
		mutex_lock(&reclaim_lock);
		unpin_backing_mappings(backing);

		if (error != ENOSPC)
			stats.io_errors++;

		mutex_unlock(&reclaim_lock);
		vm_metadata_leave();

		vm_private_page_io_release(backing);
		vm_private_page_put(backing);

		return error;
	}

	/* PTE shootdown and swap I/O are complete before the state is published. */
	memory = backing->pmem;
	(void)hal_pmem_free(&memory);

	/* Publishes the backing as swapped out rather than resident. */
	vm_metadata_enter();
	mutex_lock(&reclaim_lock);
	irq = spin_lock_irqsave(&backing->state_lock);

	if ((backing->flags & (VM_PAGE_BUSY | VM_PAGE_RESIDENT)) != (VM_PAGE_BUSY | VM_PAGE_RESIDENT) ||
	    (backing->flags & VM_PAGE_SWAPPED) != 0)
		HAL_FATAL("VM swap-out state changed under I/O owner");

	memset(&backing->pmem, 0, sizeof(backing->pmem));
	backing->swap_slot = slot;
	backing->flags &= ~(VM_PAGE_RESIDENT | VM_PAGE_DIRTY);
	backing->flags |= VM_PAGE_SWAPPED;

	private_page_advance_locked(backing);

	spin_unlock_irqrestore(&backing->state_lock, irq);

	/* Releases the mappings and moves the page between the counters. */
	unpin_backing_mappings(backing);

	if (stats.resident)
		stats.resident--;

	stats.swapped++;
	stats.page_outs++;
	stats.reclaims++;

	mutex_unlock(&reclaim_lock);
	vm_metadata_leave();

	/* Releases the ownership this reclaim pass held. */
	vm_private_page_io_release(backing);
	vm_private_page_put(backing);

	/* Reports the swapped-out backing. */
	return 0;
}

/* Discards a clean file-backed page, removing its mappings from their regions. */
static int
discard_file_backing_owned(
	struct vm_private_page *backing)
{
	struct vm_page *page;
	struct vm_page *next;
	struct hal_pmem memory;
	unsigned long irq;
	struct vm_page **link;
	struct vmspace *vm;

	/* Detaches every mapping from its region and frees it. */
	vm_metadata_enter();
	mutex_lock(&reclaim_lock);

	for (page = backing->mappings; page != NULL; page = next) {
		/* Splices the page out of its region. */
		vm = page->vm;
		next = page->private_next;
		mutex_lock(&vm->lock);

		link = &page->region->pages;
		while (*link != NULL && *link != page)
			link = &(*link)->next;
		if (*link != page)
			HAL_FATAL("discarded VM page left its region");
		*link = page->next;

		/* Releases the region and wakes the faults that waited. */
		if (page->region->hold_count == 0)
			HAL_FATAL("VM discard region hold underflow");

		page->region->hold_count--;

		vm->generation++;
		if (vm->generation == 0)
			vm->generation++;

		vmspace_fault_wake(vm);

		mutex_unlock(&vm->lock);

		/* Frees the mapping and its share of the backing. */
		page->private_page = NULL;
		page->private_next = NULL;

		if (refcount_put(&backing->refs))
			HAL_FATAL("VM discard lost I/O lifetime hold");

		vm_page_free_metadata(page);
		vmspace_put_deferred(vm);
	}

	/* Takes the backing off the queue and drops its page. */
	backing->mappings = NULL;
	queue_remove(backing);
	irq = spin_lock_irqsave(&backing->state_lock);

	if (backing->mapping_count == 0 ||
	    (backing->flags & (VM_PAGE_BUSY | VM_PAGE_TRACKED)) !=
	    (VM_PAGE_BUSY | VM_PAGE_TRACKED))
		HAL_FATAL("invalid VM file-discard backing state");

	backing->mapping_count = 0;
	backing->flags &= ~(VM_PAGE_TRACKED | VM_PAGE_RESIDENT | VM_PAGE_DIRTY);

	memory = backing->pmem;

	memset(&backing->pmem, 0, sizeof(backing->pmem));

	private_page_advance_locked(backing);

	spin_unlock_irqrestore(&backing->state_lock, irq);

	/* Discharges the page from the reclaim counters. */
	if (stats.resident)
		stats.resident--;

	stats.reclaims++;

	mutex_unlock(&reclaim_lock);
	vm_metadata_leave();

	/* Frees the memory and releases the ownership this pass held. */
	(void)hal_pmem_free(&memory);
	vm_private_page_io_release(backing);
	vm_private_page_put(backing);

	/* Reports the discarded backing. */
	return 0;
}

/* Tests whether a backing's swap slot lies in a source. */
static int
private_page_targets_source(
	struct vm_private_page *backing,
	unsigned source_id)
{
	uint32_t slot;
	unsigned decoded_source;
	unsigned flags;
	unsigned long irq;

	/* Samples the swap state. */
	irq = spin_lock_irqsave(&backing->state_lock);

	flags = backing->flags;
	slot = backing->swap_slot;

	spin_unlock_irqrestore(&backing->state_lock, irq);

	/* Reports no match for a page that is not on swap at all. */
	if ((flags & VM_PAGE_SWAPPED) == 0)
		return 0;

	/* Reports no match for a page that holds no slot. */
	if (slot == SWAP_SLOT_NONE)
		return 0;

	/* Reports no match for a slot that does not decode. */
	if (swap_slot_decode(slot, &decoded_source, NULL) != 0)
		return 0;

	/* Reports no match for a slot on some other source. */
	if (decoded_source != source_id)
		return 0;

	/* Succeeded: the page is on the source the caller named. */
	return 1;
}

/* Advances a backing's generation, skipping zero. */
static void
private_page_advance_locked(
	struct vm_private_page *backing)
{
	backing->generation++;
	if (backing->generation == 0)
		backing->generation++;
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

/* Waits for a backing to go idle, polling a cancel callback around each sleep. */
static int
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

		irq = spin_lock_irqsave(&backing->state_lock);

		/* Reports the page idle when nobody owns, runs against, or pins it. */
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
	for (reclaimed = 0; reclaimed < VM_OBJECT_FAULT_RESERVE_PAGES; reclaimed++) {
		if (vm_reclaim_private_one(NULL) != 0)
			break;
	}

	return reclaimed;
}

/* Unpins a drained backing's mappings under the locks. */
static void
drain_mapping_pins_release(
	struct vm_private_page *backing)
{
	vm_metadata_enter();
	mutex_lock(&reclaim_lock);

	unpin_backing_mappings(backing);

	mutex_unlock(&reclaim_lock);
	vm_metadata_leave();
}

/* Reclaims an owned, pinned backing by discard or swap-out. */
static int
reclaim_backing_owned(
	struct vm_private_page *backing,
	int file_candidate)
{
	uint32_t pte_flags;
	unsigned state_flags;
	unsigned long irq;
	int error;

	/* Revokes the mappings, giving up on a shootdown failure. */
	error = unmap_backing_ptes(backing, &pte_flags);
	if (error != 0) {
		vm_metadata_enter();
		mutex_lock(&reclaim_lock);

		unpin_backing_mappings(backing);
		stats.io_errors++;

		mutex_unlock(&reclaim_lock);
		vm_metadata_leave();

		/*
		 * Gives up the I/O ownership and the reference this pass
		 * held, leaving the backing exactly as it was found.
		 */
		vm_private_page_io_release(backing);
		vm_private_page_put(backing);

		/* Reports the shootdown failure. */
		return error;
	}

	/* Samples the state that decides between discard and swap-out. */
	irq = spin_lock_irqsave(&backing->state_lock);

	state_flags = backing->flags;

	spin_unlock_irqrestore(&backing->state_lock, irq);

	/*
	 * A file page neither the software nor the hardware recorded as
	 * written still matches the file, so it is discarded rather than
	 * written to swap.
	 */
	if (file_candidate &&
	    (state_flags & VM_PAGE_DIRTY) == 0 &&
	    (pte_flags & HAL_PAGE_DIRTY) == 0) {
		error = discard_file_backing_owned(backing);

		/* Reports why the page could not be discarded. */
		if (error != 0)
			return error;

		/* Succeeded: the page was dropped without any write. */
		return 0;
	}

	/* Anything else keeps its only copy on swap. */
	error = swap_out_backing_owned(backing);

	/* Reports why the page could not be written out. */
	if (error != 0)
		return error;

	/* Succeeded: the page now lives on swap. */
	return 0;
}
