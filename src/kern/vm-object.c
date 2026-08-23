/*
 * VM object
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#include "kern/vm-object.h"
#include "kern/cred.h"
#include "kern/file.h"
#include "kern/kmem.h"
#include "kern/page.h"
#include "kern/vm-reclaim.h"
#include "kern/vm-lock.h"
#include "kern/vmspace.h"

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

static struct vm_object *shared_objects VM_OBJECT_DATA;
static unsigned object_count VM_OBJECT_DATA;
static atomic_uint_t object_pages VM_OBJECT_DATA;
static atomic_uint_t object_registry_lock VM_OBJECT_DATA;

static bool
registry_lock(void)
{
	bool enabled = hal_irq_disable != NULL ? hal_irq_disable() : false;

	while (!atomic_try_acquire_zero(&object_registry_lock))
		hal_compiler_barrier();
	return enabled;
}

static void
registry_unlock(bool enabled)
{
	atomic_store_release(&object_registry_lock, 0);
	if (enabled && hal_irq_enable != NULL)
		hal_irq_enable();
}

static void destroy_object(struct vm_object *);
static int vm_object_sync_range_internal(struct vm_object *, off_t, size_t,
	int, int, int, off_t);
static void object_wake_registry_waiters(struct vm_object *);

/* Caller holds the object registry lock. */
static struct vm_object *
find_object_by_inode_locked(struct inode *inode)
{
	struct vm_object *object;

	for (object = shared_objects; object != NULL; object = object->next)
		if (object->inode == inode)
			return object;
	return NULL;
}

static int
object_wait_registry_sequence(struct vm_object *object, uint64_t sequence)
{
	unsigned long irq;
	int error;

	irq = spin_lock_irqsave(&object->lock);
	error = waitq_sleep(&object->registry_waitq, &object->lock, sequence,
	    0, 0);
	spin_unlock_irqrestore(&object->lock, irq);
	return error;
}

/* Caller holds a lifetime reference acquired while the registry was locked. */
static int
object_wait_registry_transition(struct vm_object *object, uint64_t sequence)
{
	int error = object_wait_registry_sequence(object, sequence);
	int wake;
	bool enabled = registry_lock();

	if (object->registry_waiters == 0)
		HAL_FATAL("VM object registry waiter counter underflow");
	/* Registry/teardown ownership makes this a guaranteed non-final drop. */
	if (refcount_put(&object->refs))
		HAL_FATAL("VM object waiter lost teardown lifetime reference");
	wake = --object->registry_waiters == 0;
	registry_unlock(enabled);
	if (wake)
		object_wake_registry_waiters(object);
	return error;
}

static void
object_wake_registry_waiters(struct vm_object *object)
{
	unsigned long irq = spin_lock_irqsave(&object->lock);

	waitq_wake_all(&object->registry_waitq);
	spin_unlock_irqrestore(&object->lock, irq);
}

static void
object_wait_registry_waiters(struct vm_object *object)
{
	for (;;) {
		uint64_t sequence;
		int error;
		bool enabled = registry_lock();

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

static int
object_operation_begin(struct vm_object *object)
{
	bool enabled = registry_lock();

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

static void
object_operation_end(struct vm_object *object)
{
	int wake;
	bool enabled = registry_lock();

	if (object->active_operations == 0)
		HAL_FATAL("VM object operation counter underflow");
	wake = --object->active_operations == 0;
	registry_unlock(enabled);
	if (wake)
		object_wake_registry_waiters(object);
}

static int
alloc_vm_page(struct hal_pmem *memory)
{
	const struct hal_pmem_request request = {
		HAL_PMEM_PADDR_ANY, PAGE_SIZE, PAGE_SIZE,
		HAL_PMEM_TYPE_RAM, 0
	};
	return hal_pmem_alloc(&request, memory);
}

static struct vm_object_page *
find_page(struct vm_object *object, off_t offset)
{
	struct vm_object_page *page;
	for (page = object->pages; page != NULL; page = page->next)
		if (page->offset == offset)
			return page;
	return NULL;
}

/* Caller holds object->lock. */
static int
unlink_page_locked(struct vm_object_page **head,
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

/* Called with object->lock held.  Generation zero is reserved for "none". */
static uint64_t
next_generation(struct vm_object *object)
{
	object->generation++;
	if (object->generation == 0)
		object->generation++;
	return object->generation;
}

static uint64_t
next_inode_resize_generation_locked(struct inode *inode)
{
	if (++inode->i_vm_resize_generation == 0)
		inode->i_vm_resize_generation++;
	return inode->i_vm_resize_generation;
}

static uint64_t
next_inode_content_generation_locked(struct inode *inode)
{
	if (++inode->i_vm_content_generation == 0)
		inode->i_vm_content_generation++;
	return inode->i_vm_content_generation;
}

/* Caller holds the object registry lock; the object is not yet published. */
static void
object_initialize_eof_locked(struct vm_object *object, struct inode *inode)
{
	unsigned long irq = spin_lock_irqsave(&inode->i_vm_lock);

	if (inode->i_vm_resize_active) {
		object->logical_size = inode->i_vm_resize_old_size;
		object->size_generation = inode->i_vm_resize_generation;
		object->resize_generation = inode->i_vm_resize_generation;
		object->flags |= VM_OBJECT_RESIZING;
		/* Pins registry teardown until the inode transaction finishes. */
		object->active_operations = 1;
	} else {
		object->logical_size = inode->i_size;
		object->size_generation = inode->i_vm_resize_generation != 0 ?
		    inode->i_vm_resize_generation : 1U;
	}
	if (inode->i_vm_content_active) {
		object->flags |= VM_OBJECT_CONTENT;
		object->content_generation = inode->i_vm_content_generation;
		object->active_operations++;
		if (object->active_operations == 0)
			HAL_FATAL("VM object operation counter overflow");
	}
	spin_unlock_irqrestore(&inode->i_vm_lock, irq);
}

static int
page_overlaps(const struct vm_object_page *page, uint64_t start, uint64_t end)
{
	uint64_t page_start, page_end;
	if (page->offset < 0)
		return 0;
	page_start = (uint64_t)page->offset;
	page_end = page_start + PAGE_SIZE;
	return page_end > start && page_start < end;
}

int
vm_object_get_shared(struct file *file, struct vm_object **result)
{
	struct vm_object *object;
	struct inode *inode;
	int error, writable;
	bool enabled;
	if (file == NULL || result == NULL)
		return EINVAL;
	inode = file_vm_inode(file);
	if (inode == NULL)
		return EINVAL;
	writable = (file_status_flags_get(file) & O_ACCMODE) != O_RDONLY &&
	    file->f_ops != NULL && file->f_ops->pwrite != NULL;

retry_lookup:
	enabled = registry_lock();
	for (object = shared_objects; object != NULL; object = object->next) {
		if (object->inode == inode) {
			if ((object->flags & VM_OBJECT_DETACHING) != 0) {
				uint64_t sequence =
				    waitq_sequence(&object->registry_waitq);
				/* Pins an object which may be unlinked before wakeup. */
				object->registry_waiters++;
				if (object->registry_waiters == 0)
					HAL_FATAL("VM object registry waiter overflow");
				refcount_get(&object->refs);
				registry_unlock(enabled);
				error = object_wait_registry_transition(object,
				    sequence);
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
	}
	registry_unlock(enabled);
	object = kern_calloc(1, sizeof(*object));
	if (object == NULL)
		return ENOMEM;
	refcount_init(&object->refs, 2); /* registry plus returned region */
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
	enabled = registry_lock();
	/* Another CPU may have published the inode while allocation slept. */
	{
		struct vm_object *existing;
		for (existing = shared_objects; existing != NULL;
		     existing = existing->next) {
			if (existing->inode != inode)
				continue;
			if ((existing->flags & VM_OBJECT_DETACHING) != 0) {
				uint64_t sequence =
				    waitq_sequence(&existing->registry_waitq);
				existing->registry_waiters++;
				if (existing->registry_waiters == 0)
					HAL_FATAL("VM object registry waiter overflow");
				refcount_get(&existing->refs);
				registry_unlock(enabled);
				destroy_object(object);
				error = object_wait_registry_transition(existing,
				    sequence);
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
	}
	object_initialize_eof_locked(object, inode);
	object->next = shared_objects;
	shared_objects = object;
	object_count++;
	registry_unlock(enabled);
	*result = object;
	return 0;
}

void
vm_object_ref(struct vm_object *object)
{
	bool enabled;
	if (object == NULL)
		return;
	enabled = registry_lock();
	if (object->mapping_count == 0)
		HAL_FATAL("referencing detached VM object");
	object->mapping_count++;
	refcount_get(&object->refs);
	registry_unlock(enabled);
}

static int
page_is_dirty_locked(struct vm_object_page *page)
{
	return (page->flags & VM_OBJECT_PAGE_DIRTY) != 0;
}

static int
object_has_dirty_pages_locked(struct vm_object *object)
{
	struct vm_object_page *page;
	for (page = object->pages; page != NULL; page = page->next)
		if (page_is_dirty_locked(page))
			return 1;
	return 0;
}

static int
object_has_busy_pages_locked(const struct vm_object *object)
{
	const struct vm_object_page *page;
	for (page = object->pages; page != NULL; page = page->next)
		if (page->flags & (VM_OBJECT_PAGE_BUSY | VM_OBJECT_PAGE_WRITEBACK))
			return 1;
		else if (page->hold_count != 0)
			return 1;
	for (page = object->orphan_pages; page != NULL; page = page->next)
		if ((page->flags & (VM_OBJECT_PAGE_BUSY |
		    VM_OBJECT_PAGE_WRITEBACK)) != 0 || page->hold_count != 0)
			return 1;
	return 0;
}

static int
object_has_busy_pages(struct vm_object *object)
{
	unsigned long irq = spin_lock_irqsave(&object->lock);
	int busy = object_has_busy_pages_locked(object);
	spin_unlock_irqrestore(&object->lock, irq);
	return busy;
}

static void
object_record_writeback_error_locked(struct vm_object *object, int error)
{
	if (object->writeback_error == 0 && error != 0)
		object->writeback_error = error;
}

static int
write_page_data(struct vm_object *object, struct vm_object_page *page,
	off_t logical_size, int inode_io_owned)
{
	struct file_io io;
	size_t length;
	ssize_t count;
	int error;
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
	error = file_io_begin(object->write_file, FILE_IO_PWRITE, page->offset,
	    FILE_IO_VM_OBJECT | FILE_IO_CONTENT_CHANGE |
	    (inode_io_owned ? FILE_IO_INODE_IO_OWNED : 0),
	    &io);
	if (error != 0)
		return error;
	count = file_io_transfer(&io, (void *)page->pmem.vaddr, length);
	file_io_end(&io);
	return count == (ssize_t)length ? 0 : count < 0 ? (int)-count : EIO;
}

static void
clear_page_dirty_locked(struct vm_object_page *page)
{
	page->flags &= ~VM_OBJECT_PAGE_DIRTY;
	page->dirty_generation = 0;
}

static void
free_object_page(struct vm_object_page *page)
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

static int
object_can_destroy(struct vm_object *object)
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

static int
unlink_object_locked(struct vm_object *object)
{
	struct vm_object **link;
	if (!object_can_destroy(object))
		HAL_FATAL("destroying unsynchronized VM object");
	for (link = &shared_objects; *link != NULL; link = &(*link)->next)
		if (*link == object) {
			*link = object->next;
			object->next = NULL;
			if (object_count == 0)
				HAL_FATAL("VM object counter underflow");
			object_count--;
			return 1;
		}
	return 0;
}

static void
destroy_object(struct vm_object *object)
{
	struct vm_object_page *page;

	while ((page = object->pages) != NULL) {
		object->pages = page->next;
		free_object_page(page);
	}
	while ((page = object->orphan_pages) != NULL) {
		object->orphan_pages = page->next;
		free_object_page(page);
	}
	(void)file_close(object->file);
	if (object->write_file != NULL)
		(void)file_close(object->write_file);
	kern_free(object);
}

static void
retain_object(struct vm_object *object, int error)
{
	unsigned long irq;
	if (object->mapping_count != 0)
		HAL_FATAL("retaining referenced VM object");
	if (error == 0)
		error = object_has_busy_pages(object) ? EBUSY : EIO;
	irq = spin_lock_irqsave(&object->lock);
	object_record_writeback_error_locked(object, error);
	spin_unlock_irqrestore(&object->lock, irq);
	object->flags |= VM_OBJECT_RETAINED_WRITEBACK;
}

void
vm_object_put(struct vm_object *object)
{
	int error;
	int removed = 0;
	bool enabled;

	if (object == NULL)
		return;
retry_mapping:
	enabled = registry_lock();
	if (object->mapping_count == 0)
		HAL_FATAL("VM object mapping reference underflow");
	/*
	 * Keep the final mapping reference published while older metadata users
	 * drain.  A racing mapper may safely join this still-live object; in that
	 * case the retried put becomes an ordinary non-final reference drop.
	 */
	if (object->mapping_count == 1 && object->active_operations != 0) {
		uint64_t sequence = waitq_sequence(&object->registry_waitq);
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
	 * Registry lookup may now wait without either reviving this object during
	 * writeback or racing its destruction.
	 */
	object->flags |= VM_OBJECT_DETACHING;
	refcount_get(&object->refs);
	registry_unlock(enabled);
	if (refcount_put(&object->refs))
		HAL_FATAL("VM object teardown lost registry reference");
	/* DETACHING excludes new mappings, so final writeback does not need the
	 * cross-vm metadata lock which a waiting mapper may already hold. */
	error = vm_object_sync_range_internal(object, 0, SIZE_MAX, MS_SYNC, 1, 0,
	    0);
	enabled = registry_lock();
	if (object->mapping_count != 0 ||
	    (object->flags & VM_OBJECT_DETACHING) == 0)
		HAL_FATAL("VM object teardown state changed during writeback");
	if (error == 0 && object_can_destroy(object)) {
		removed = unlink_object_locked(object);
		if (!removed)
			HAL_FATAL("detaching VM object left registry unexpectedly");
	} else {
		retain_object(object, error);
		object->flags &= ~VM_OBJECT_DETACHING;
	}
	registry_unlock(enabled);
	object_wake_registry_waiters(object);
	if (removed)
		object_wait_registry_waiters(object);
	if (removed && refcount_put(&object->refs))
		HAL_FATAL("VM object registry reference was last unexpectedly");
	/* Drop the explicit teardown lifetime reference. */
	if (refcount_put(&object->refs))
		destroy_object(object);
}

static int
object_wait_resize(struct vm_object *object)
{
	unsigned long irq;
	int error;

	irq = spin_lock_irqsave(&object->lock);
	while ((object->flags & (VM_OBJECT_RESIZING | VM_OBJECT_CONTENT)) != 0) {
		uint64_t sequence = waitq_sequence(&object->page_waitq);

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

int
vm_object_inode_io_wait(struct inode *inode)
{
	if (inode == NULL)
		return EINVAL;
	for (;;) {
		struct vm_object *object;
		unsigned long irq;
		bool enabled;
		int error;

		irq = spin_lock_irqsave(&inode->i_vm_lock);
		if (inode->i_vm_resize_active || inode->i_vm_content_active ||
		    inode->i_vm_content_readers != 0) {
			uint64_t sequence = waitq_sequence(&inode->i_vm_waitq);

			error = waitq_sleep(&inode->i_vm_waitq, &inode->i_vm_lock,
			    sequence, 0, 0);
			spin_unlock_irqrestore(&inode->i_vm_lock, irq);
			if (error != 0 && error != EAGAIN)
				return error;
			continue;
		}
		spin_unlock_irqrestore(&inode->i_vm_lock, irq);

		/* A resize cannot join an object while its final writeback owns the
		 * DETACHING transition.  Wait here, outside inode->i_io_lock. */
		enabled = registry_lock();
		object = find_object_by_inode_locked(inode);
		if (object == NULL ||
		    (object->flags & VM_OBJECT_DETACHING) == 0) {
			registry_unlock(enabled);
			return 0;
		}
		{
			uint64_t sequence = waitq_sequence(&object->registry_waitq);

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
}

int
vm_object_inode_resize_active(struct inode *inode)
{
	unsigned long irq;
	int active;

	if (inode == NULL)
		return 0;
	irq = spin_lock_irqsave(&inode->i_vm_lock);
	active = inode->i_vm_resize_active != 0 ||
	    inode->i_vm_content_active != 0 ||
	    inode->i_vm_content_readers != 0;
	spin_unlock_irqrestore(&inode->i_vm_lock, irq);
	return active;
}

int
vm_object_content_read_begin(struct inode *inode)
{
	unsigned long irq;

	if (inode == NULL || inode->i_type != INODE_REG)
		return EINVAL;
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
	return 0;
}

void
vm_object_content_read_end(struct inode *inode)
{
	unsigned long irq;

	if (inode == NULL)
		return;
	irq = spin_lock_irqsave(&inode->i_vm_lock);
	if (inode->i_vm_content_readers == 0)
		HAL_FATAL("VM content read lease underflow");
	inode->i_vm_content_readers--;
	if (inode->i_vm_content_readers == 0)
		waitq_wake_all(&inode->i_vm_waitq);
	spin_unlock_irqrestore(&inode->i_vm_lock, irq);
}

int
vm_object_cache_published(struct inode *inode)
{
	struct vm_object *object;
	bool enabled;
	int result;

	if (inode == NULL)
		return -EINVAL;
	enabled = registry_lock();
	object = find_object_by_inode_locked(inode);
	result = object == NULL ? 0 :
	    (object->flags & VM_OBJECT_DETACHING) != 0 ? -EAGAIN : 1;
	registry_unlock(enabled);
	return result;
}

int
vm_object_content_begin(struct file *file, off_t offset, size_t length,
	struct vm_object_resize *resize_owner, struct vm_object_content *content)
{
	struct inode *inode;
	struct vm_object *object;
	unsigned long inode_irq, object_irq;
	uint64_t generation;
	bool enabled;

	if (file == NULL || content == NULL || offset < 0 ||
	    (uint64_t)offset + length < (uint64_t)offset)
		return EINVAL;
	memset(content, 0, sizeof(*content));
	if (length == 0)
		return 0;
	inode = file_vm_inode(file);
	if (inode == NULL || inode->i_type != INODE_REG)
		return EINVAL;
	enabled = registry_lock();
	inode_irq = spin_lock_irqsave(&inode->i_vm_lock);
	if (inode->i_vm_content_active || inode->i_vm_content_readers != 0) {
		spin_unlock_irqrestore(&inode->i_vm_lock, inode_irq);
		registry_unlock(enabled);
		return EBUSY;
	}
	if (resize_owner != NULL) {
		if (!resize_owner->active || !resize_owner->prepared ||
		    resize_owner->inode != inode || !inode->i_vm_resize_active ||
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
	return 0;
}

static void
content_release_pages_locked(struct vm_object *object, uint64_t generation)
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

int
vm_object_content_prepare(struct vm_object_content *content)
{
	struct vm_object *object;
	uint64_t start, end;
	bool enabled;
	int first_error = 0;

	if (content == NULL || !content->active || content->inode == NULL)
		return EINVAL;
	start = (uint64_t)content->offset;
	end = start + content->length;
	enabled = registry_lock();
	object = find_object_by_inode_locked(content->inode);
	if (object != NULL) {
		unsigned long irq = spin_lock_irqsave(&object->lock);

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
	for (;;) {
		struct vm_object_page *page = NULL;
		unsigned long irq = spin_lock_irqsave(&object->lock);
		int dirty, has_mappings;

		for (;;) {
			struct vm_object_page *scan;
			int wait = 0;

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
			{
				uint64_t sequence = waitq_sequence(&object->page_waitq);
				int error = waitq_sleep(&object->page_waitq,
				    &object->lock, sequence, 0, 0);

				if (error != 0 && error != EAGAIN) {
					first_error = error;
					break;
				}
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

		{
			uint32_t observed = 0;
			int error = 0;

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
			    page->dirty_generation == page->write_dirty_generation)
				clear_page_dirty_locked(page);
			else if (error != 0) {
				object_record_writeback_error_locked(object, error);
				first_error = error;
			}
			spin_unlock_irqrestore(&object->lock, irq);
		}
		if (first_error != 0)
			break;
	}
	if (first_error != 0) {
		unsigned long irq = spin_lock_irqsave(&object->lock);

		content_release_pages_locked(object, content->generation);
		spin_unlock_irqrestore(&object->lock, irq);
	} else {
		content->prepared = 1;
	}
	if (refcount_put(&object->refs))
		HAL_FATAL("content transaction lost registry reference");
	return first_error;
}

static void
vm_object_content_finish(struct vm_object_content *content,
	const void *buffer, size_t committed, int commit)
{
	struct inode *inode;
	struct vm_object *object;
	unsigned long inode_irq, object_irq;
	int wake_registry = 0;
	bool enabled;

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
	object = find_object_by_inode_locked(inode);
	if (object != NULL) {
		struct vm_object_page *page;
		uint64_t write_start = (uint64_t)content->offset;
		uint64_t write_end = write_start + committed;

		object_irq = spin_lock_irqsave(&object->lock);
		if ((object->flags & VM_OBJECT_CONTENT) == 0 ||
		    object->content_generation != content->generation)
			HAL_FATAL("VM object content generation mismatch");
		for (page = object->pages; page != NULL; page = page->next) {
			uint64_t page_start, page_end, copy_start, copy_end;

			if (page->content_generation != content->generation)
				continue;
			if (commit && committed != 0) {
				page_start = (uint64_t)page->offset;
				page_end = page_start + PAGE_SIZE;
				copy_start = page_start > write_start ?
				    page_start : write_start;
				copy_end = page_end < write_end ? page_end : write_end;
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
		wake_registry = --object->active_operations == 0;
	}
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

void
vm_object_content_commit(struct vm_object_content *content,
	const void *buffer, size_t committed)
{
	vm_object_content_finish(content, buffer, committed, 1);
}

void
vm_object_content_abort(struct vm_object_content *content)
{
	vm_object_content_finish(content, NULL, 0, 0);
}

int
vm_object_read_coherent(struct inode *inode, off_t offset, void *buffer,
	size_t length, ssize_t *result)
{
	struct vm_object *object;
	uint8_t *bytes = buffer;
	size_t done = 0;
	bool enabled;
	int error = 0;

	if (inode == NULL || result == NULL || offset < 0 ||
	    (buffer == NULL && length != 0))
		return EINVAL;
	retry_lookup:
	enabled = registry_lock();
	object = find_object_by_inode_locked(inode);
	if (object == NULL) {
		registry_unlock(enabled);
		return ENOENT;
	}
	if ((object->flags & VM_OBJECT_DETACHING) != 0) {
		uint64_t sequence = waitq_sequence(&object->registry_waitq);

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
	while (done < length) {
		struct vm_object_page *page;
		off_t current = offset + (off_t)done;
		off_t logical_size;
		off_t page_offset = current & ~(off_t)(PAGE_SIZE - 1U);
		size_t in_page = (size_t)(current - page_offset);
		size_t chunk = PAGE_SIZE - in_page;
		unsigned long irq = spin_lock_irqsave(&object->lock);

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
	if (done != 0 || error == 0 || error == ENXIO) {
		*result = (ssize_t)done;
		return 0;
	}
	return error;
}

int
vm_object_resize_begin(struct inode *inode, off_t target_size,
	struct vm_object_resize *resize)
{
	struct vm_object *object;
	unsigned long inode_irq, object_irq;
	uint64_t generation;
	bool enabled;

	if (inode == NULL || resize == NULL || target_size < 0)
		return EINVAL;
	memset(resize, 0, sizeof(*resize));
	enabled = registry_lock();
	inode_irq = spin_lock_irqsave(&inode->i_vm_lock);
	if (inode->i_vm_resize_active || inode->i_vm_content_active ||
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
	resize->old_size = inode->i_size;
	if (object != NULL) {
		object_irq = spin_lock_irqsave(&object->lock);
		if ((object->flags & VM_OBJECT_RESIZING) != 0)
			HAL_FATAL("VM object resize state diverged from inode");
		/* Once published, the object's EOF is authoritative for mappings.
		 * Generic writes/truncates keep it equal to inode->i_size. */
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
	return 0;
}

int
vm_object_resize_prepare(struct vm_object_resize *resize)
{
	struct vm_object *object;
	uint64_t start, length;
	bool enabled;
	int error = 0;

	if (resize == NULL || !resize->active || resize->inode == NULL)
		return EINVAL;
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

	/* Shrink invalidates every possibly discarded page.  Grow only needs the
	 * old partial EOF page; it may contain mmap stores beyond the old EOF,
	 * which must never become visible after extension. */
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
		start = length = 0;
	}
	if (length != 0)
		error = vm_object_sync_range_internal(object, (off_t)start,
		    (size_t)length, MS_SYNC | MS_INVALIDATE, 0, 1,
		    resize->target_size);
	if (refcount_put(&object->refs))
		HAL_FATAL("resize transaction lost VM object registry reference");
	if (error == 0)
		resize->prepared = 1;
	return error;
}

static void
vm_object_resize_finish(struct vm_object_resize *resize, off_t logical_size,
	int commit)
{
	struct inode *inode;
	struct vm_object *object;
	struct vm_object_page *free_pages = NULL;
	unsigned long inode_irq, object_irq;
	uint64_t stable_generation;
	int wake_registry = 0;
	bool enabled;

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
		struct vm_object_page *page;
		struct vm_object_page **link;

		object_irq = spin_lock_irqsave(&object->lock);
		if ((object->flags & VM_OBJECT_RESIZING) == 0 ||
		    object->resize_generation != resize->generation)
			HAL_FATAL("VM object resize generation mismatch");
		if (commit) {
			object->logical_size = logical_size;
			/* Orphans still pinned remain owner-linked until their last
			 * unpin.  Orphans whose pins drained during prepare can be
			 * reclaimed as soon as the transaction is published. */
			for (link = &object->orphan_pages; *link != NULL; ) {
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
			object->logical_size = resize->old_size;
			/* No replacement page can be faulted while RESIZING is set, so
			 * an abort may atomically return every orphan to the cache. */
			while ((page = object->orphan_pages) != NULL) {
				object->orphan_pages = page->next;
				page->flags &= ~VM_OBJECT_PAGE_ORPHANED;
				page->next = object->pages;
				object->pages = page;
			}
		}
		object->size_generation = stable_generation;
		object->resize_generation = 0;
		object->flags &= ~VM_OBJECT_RESIZING;
		waitq_wake_all(&object->page_waitq);
		spin_unlock_irqrestore(&object->lock, object_irq);
		if (object->active_operations == 0)
			HAL_FATAL("VM object resize operation counter underflow");
		wake_registry = --object->active_operations == 0;
	}
	inode->i_vm_resize_active = 0;
	inode->i_vm_resize_old_size = 0;
	inode->i_vm_resize_target_size = 0;
	waitq_wake_all(&inode->i_vm_waitq);
	spin_unlock_irqrestore(&inode->i_vm_lock, inode_irq);
	registry_unlock(enabled);
	if (wake_registry)
		object_wake_registry_waiters(object);
	while (free_pages != NULL) {
		struct vm_object_page *page = free_pages;

		free_pages = page->next;
		free_object_page(page);
	}
	memset(resize, 0, sizeof(*resize));
}

void
vm_object_resize_commit(struct vm_object_resize *resize, off_t logical_size)
{
	vm_object_resize_finish(resize, logical_size, 1);
}

void
vm_object_resize_abort(struct vm_object_resize *resize)
{
	vm_object_resize_finish(resize,
	    resize != NULL ? resize->old_size : 0, 0);
}

int
vm_object_fault(struct vm_object *object, off_t offset,
		struct vm_object_page **result)
{
	struct vm_object_page *page;
	struct file_io io;
	off_t fault_size;
	uint64_t fault_generation, fault_content_generation;
	size_t length;
	ssize_t count;
	unsigned long irq;
	int error;
	if (object == NULL || result == NULL || offset < 0 ||
	    (offset & (PAGE_SIZE - 1U)) != 0)
		return EINVAL;
	retry:
	irq = spin_lock_irqsave(&object->lock);
	while ((object->flags & (VM_OBJECT_RESIZING | VM_OBJECT_CONTENT)) != 0) {
		uint64_t sequence = waitq_sequence(&object->page_waitq);

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
	page = find_page(object, offset);
	if (page != NULL) {
		while ((page->flags & VM_OBJECT_PAGE_BUSY) != 0) {
			uint64_t sequence = waitq_sequence(&object->page_waitq);
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
		if ((page->flags & VM_OBJECT_PAGE_ERROR) != 0) {
			if (offset >= fault_size) {
				error = page->error != 0 ? page->error : ENXIO;
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
	page = kern_calloc(1, sizeof(*page));
	if (page == NULL)
		return ENOMEM;
	page->offset = offset;
	page->flags = VM_OBJECT_PAGE_BUSY;
	page->owner = object;
	if (alloc_vm_page(&page->pmem) != HAL_OK &&
	    (vm_reclaim_one(NULL) != 0 ||
	     alloc_vm_page(&page->pmem) != HAL_OK)) {
		kern_free(page);
		return ENOMEM;
	}
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
	memset((void *)page->pmem.vaddr, 0, PAGE_SIZE);
	length = (size_t)(fault_size - offset);
	if (length > PAGE_SIZE)
		length = PAGE_SIZE;
	error = file_io_begin(object->file, FILE_IO_PREAD, offset,
	    FILE_IO_VM_OBJECT, &io);
	if (error != 0) {
		count = -error;
	} else {
		/* i_io_lock is now held.  A resize published after page BUSY but
		 * before the backend read is detected before any stale I/O starts. */
		irq = spin_lock_irqsave(&object->lock);
		error = (object->flags & (VM_OBJECT_RESIZING |
		    VM_OBJECT_CONTENT)) != 0 ||
		    object->size_generation != fault_generation ||
		    object->content_generation != fault_content_generation ||
		    offset >= object->logical_size ? EAGAIN : 0;
		spin_unlock_irqrestore(&object->lock, irq);
		count = error == 0 ? file_io_transfer(&io,
		    (void *)page->pmem.vaddr, length) : -error;
		file_io_end(&io);
	}
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
		error = count < 0 ? (int)-count : EIO;
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
	return 0;
}

int
vm_object_page_pin(struct vm_object_page *page)
{
	struct vm_object *object;
	unsigned long irq;
	bool enabled;

	if (page == NULL || (object = page->owner) == NULL)
		return EINVAL;
	/* The caller supplies a stable page pointer (normally while holding VM
	 * metadata).  Registry -> object is the same lifetime order as detach. */
	enabled = registry_lock();
	if ((object->flags & VM_OBJECT_DETACHING) != 0) {
		registry_unlock(enabled);
		return EBUSY;
	}
	irq = spin_lock_irqsave(&object->lock);
	if (page->owner != object || page->hold_count == UINT_MAX ||
	    page->pin_count == UINT_MAX) {
		spin_unlock_irqrestore(&object->lock, irq);
		registry_unlock(enabled);
		return page->owner == object ? EOVERFLOW : EINVAL;
	}
	page->hold_count++;
	page->pin_count++;
	object->active_operations++;
	if (object->active_operations == 0)
		HAL_FATAL("VM object pin operation counter overflow");
	refcount_get(&object->refs);
	spin_unlock_irqrestore(&object->lock, irq);
	registry_unlock(enabled);
	return 0;
}

static int
vm_object_page_pin_copy(struct vm_object_page *page, size_t offset,
	void *buffer, size_t length, int write)
{
	struct vm_object *object;
	unsigned long irq;
	int error;

	if (page == NULL || buffer == NULL ||
	    (object = page->owner) == NULL || offset > PAGE_SIZE ||
	    length > PAGE_SIZE - offset)
		return EINVAL;
	irq = spin_lock_irqsave(&object->lock);
	for (;;) {
		if (page->owner != object || page->pin_count == 0) {
			spin_unlock_irqrestore(&object->lock, irq);
			return EINVAL;
		}
		if ((page->flags & (VM_OBJECT_PAGE_BUSY |
		    VM_OBJECT_PAGE_WRITEBACK)) == 0)
			break;
		{
			uint64_t sequence = waitq_sequence(&object->page_waitq);

			error = waitq_sleep(&object->page_waitq, &object->lock,
			    sequence, 0, 0);
			if (error != 0 && error != EAGAIN) {
				spin_unlock_irqrestore(&object->lock, irq);
				return error;
			}
		}
	}
	if (write) {
		memcpy((uint8_t *)page->pmem.vaddr + offset, buffer, length);
		/* Orphan writes stay dirty so abort can restore the old cache image.
		 * Commit discards the orphan without ever putting it on writeback. */
		page->flags |= VM_OBJECT_PAGE_DIRTY;
		page->dirty_generation = next_generation(object);
	} else {
		memcpy(buffer, (const uint8_t *)page->pmem.vaddr + offset, length);
	}
	spin_unlock_irqrestore(&object->lock, irq);
	return 0;
}

int
vm_object_page_pin_read(struct vm_object_page *page, size_t offset,
	void *buffer, size_t length)
{
	return vm_object_page_pin_copy(page, offset, buffer, length, 0);
}

int
vm_object_page_pin_write(struct vm_object_page *page, size_t offset,
	const void *buffer, size_t length)
{
	return vm_object_page_pin_copy(page, offset, (void *)buffer, length, 1);
}

void
vm_object_page_unpin(struct vm_object_page *page)
{
	struct vm_object *object;
	struct vm_object_page *free_page = NULL;
	unsigned long irq;

	if (page == NULL || (object = page->owner) == NULL)
		return;
	irq = spin_lock_irqsave(&object->lock);
	if (page->hold_count == 0 || page->pin_count == 0)
		HAL_FATAL("VM object page pin underflow");
	page->pin_count--;
	page->hold_count--;
	if ((page->flags & VM_OBJECT_PAGE_ORPHANED) != 0 &&
	    (object->flags & VM_OBJECT_RESIZING) == 0 &&
	    page->hold_count == 0) {
		if (page->pin_count != 0 || page->mapping_count != 0 ||
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
	/* A concurrent final put may have unlinked the object after the operation
	 * wake.  The pin reference makes this the unique final-destroy point. */
	if (refcount_put(&object->refs))
		destroy_object(object);
}

void
vm_object_fault_release(struct vm_object_page *page)
{
	unsigned long irq;
	if (page == NULL || page->owner == NULL)
		return;
	irq = spin_lock_irqsave(&page->owner->lock);
	if (page->hold_count == 0)
		HAL_FATAL("VM object fault hold underflow");
	page->hold_count--;
	waitq_wake_all(&page->owner->page_waitq);
	spin_unlock_irqrestore(&page->owner->lock, irq);
}

void
vm_object_mapping_add(struct vm_object_page *object_page,
		      struct vm_page *mapping)
{
	unsigned long irq;
	if (object_page == NULL || mapping == NULL || object_page->owner == NULL)
		return;
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

/* Called with object_page->owner->lock held. */
void
vm_object_mapping_remove_locked(struct vm_object_page *object_page,
		      struct vm_page *mapping)
{
	struct vm_page **link;
	for (link = &object_page->mappings; *link != NULL;
	     link = &(*link)->object_next)
		if (*link == mapping) {
			*link = mapping->object_next;
			mapping->object_next = NULL;
			if (object_page->mapping_count == 0)
				HAL_FATAL("VM object mapping counter underflow");
			object_page->mapping_count--;
			return;
		}
}

void
vm_object_mapping_remove(struct vm_object_page *object_page,
			 struct vm_page *mapping)
{
	unsigned long irq;
	if (object_page == NULL || mapping == NULL || object_page->owner == NULL)
		return;
	vm_metadata_enter();
	irq = spin_lock_irqsave(&object_page->owner->lock);
	vm_object_mapping_remove_locked(object_page, mapping);
	spin_unlock_irqrestore(&object_page->owner->lock, irq);
	vm_metadata_leave();
}

void
vm_object_mark_dirty(struct vm_object_page *page)
{
	unsigned long irq;
	if (page == NULL || page->owner == NULL)
		return;
	vm_metadata_enter();
	irq = spin_lock_irqsave(&page->owner->lock);
	page->flags |= VM_OBJECT_PAGE_DIRTY;
	page->dirty_generation = next_generation(page->owner);
	spin_unlock_irqrestore(&page->owner->lock, irq);
	vm_metadata_leave();
}

static int
range_has_wired_mapping(struct vm_object *object, uint64_t start, uint64_t end)
{
	struct vm_object_page *page;
	for (page = object->pages; page != NULL; page = page->next) {
		struct vm_page *mapping;
		if (!page_overlaps(page, start, end))
			continue;
		for (mapping = page->mappings; mapping != NULL;
		     mapping = mapping->object_next)
			if (mapping->wire_count != 0)
				return 1;
	}
	return 0;
}

static int
vm_object_sync_range_internal(struct vm_object *object, off_t offset,
	size_t size, int flags, int detaching, int resize_owner,
	off_t resize_target)
{
	struct vm_object_page *page, **link, *retired = NULL;
	off_t write_limit;
	uint64_t start, end;
	uint64_t sync_generation;
	unsigned long irq;
	int first_error = 0;
	int selected_write = 0;
	int retry_writeback;
	int held_inode_io = 0;

	if (object == NULL || offset < 0 || size == 0 ||
	    (flags & ~(MS_ASYNC | MS_INVALIDATE | MS_SYNC)) != 0 ||
	    (flags & (MS_ASYNC | MS_SYNC)) == 0 ||
	    (flags & (MS_ASYNC | MS_SYNC)) == (MS_ASYNC | MS_SYNC))
		return EINVAL;
	start = (uint64_t)offset;
	/* offset zero plus SIZE_MAX is the internal full-object sentinel. */
	if (start == 0 && size == SIZE_MAX)
		end = UINT64_MAX;
	else {
		end = start + size;
		if (end < start)
			end = UINT64_MAX;
	}
	(void)resize_target;
	if (!resize_owner && object->inode != NULL) {
		for (;;) {
			int transaction_active;

			mutex_lock(&object->inode->i_io_lock);
			irq = spin_lock_irqsave(&object->lock);
			transaction_active = (object->flags &
			    (VM_OBJECT_RESIZING | VM_OBJECT_CONTENT)) != 0;
			spin_unlock_irqrestore(&object->lock, irq);
			if (!transaction_active)
				break;
			mutex_unlock(&object->inode->i_io_lock);
			{
				int error = object_wait_resize(object);

				if (error != 0)
					return error;
			}
		}
		held_inode_io = 1;
	}
	/* Wired mappings make MS_INVALIDATE non-atomic.  This is only a metadata
	 * query; all later PTE changes are performed by the reverse-map transaction
	 * after both metadata locks have been dropped. */
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
	irq = spin_lock_irqsave(&object->lock);
	write_limit = object->logical_size;
	if (resize_owner && (object->flags & VM_OBJECT_RESIZING) == 0)
		HAL_FATAL("VM resize writeback without transaction ownership");
	if (detaching)
		for (page = object->pages; page != NULL; page = page->next)
			if (page->mappings != NULL || page->mapping_count != 0)
				HAL_FATAL("detaching VM object still has reverse mappings");
	sync_generation = next_generation(object);
	spin_unlock_irqrestore(&object->lock, irq);

	/* Revoke every resident page in the range before deciding whether it is
	 * dirty.  BUSY excludes pin copies and new fault publication; draining the
	 * non-pin holds closes the pre-existing fault/publish window. */
	for (;;) {
		struct vm_object_page *candidate = NULL;
		int has_mappings = 0;
		int wait_for_page = 0, dirty = 0;
		uint32_t observed = 0;

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
			uint64_t sequence = waitq_sequence(&object->page_waitq);
			int error = waitq_sleep(&object->page_waitq, &object->lock,
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
		if (first_error == 0 && dirty) {
			first_error = write_page_data(object, candidate, write_limit,
			    !resize_owner);
			selected_write = 1;
		}
	}
	irq = spin_lock_irqsave(&object->lock);
	retry_writeback = object->writeback_error != 0;
	spin_unlock_irqrestore(&object->lock, irq);

	/* Resize writeback is immediately followed by its serialized mutation;
	 * ordinary sync/final-put waits for the backend's durability barrier. */
	if (!resize_owner && (selected_write || retry_writeback) &&
	    first_error == 0) {
		int error;

		if (object->write_file == NULL)
			error = EACCES;
		else if (object->write_file->f_ops != NULL &&
		    object->write_file->f_ops->fsync != NULL)
			error = object->write_file->f_ops->fsync(object->write_file);
		else
			error = object->inode != NULL && object->inode->i_op != NULL &&
			    object->inode->i_op->sync != NULL ?
			    object->inode->i_op->sync(object->inode) : 0;

		if (error != 0)
			first_error = error;
	}

	irq = spin_lock_irqsave(&object->lock);
	if (first_error != 0)
		object_record_writeback_error_locked(object, first_error);
	for (link = &object->pages; (page = *link) != NULL; ) {
		if (page->write_generation != sync_generation) {
			link = &page->next;
			continue;
		}
		if (first_error == 0 && page->write_dirty_generation != 0 &&
		    page->dirty_generation == page->write_dirty_generation)
			clear_page_dirty_locked(page);
		page->write_generation = 0;
		page->write_dirty_generation = 0;
		page->flags &= ~(VM_OBJECT_PAGE_BUSY | VM_OBJECT_PAGE_WRITEBACK);
		if (first_error == 0 && resize_owner && page->pin_count != 0) {
			if (page->mappings != NULL || page->mapping_count != 0 ||
			    page->hold_count != page->pin_count)
				HAL_FATAL("invalid VM resize orphan candidate");
			*link = page->next;
			page->flags |= VM_OBJECT_PAGE_ORPHANED;
			page->next = object->orphan_pages;
			object->orphan_pages = page;
			continue;
		}
		if (first_error == 0 && (flags & MS_INVALIDATE) != 0) {
			if (page->mappings != NULL || page->mapping_count != 0 ||
			    page->hold_count != 0 || page->pin_count != 0)
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
	while (retired != NULL) {
		page = retired;
		retired = page->next;
		free_object_page(page);
	}
	if (held_inode_io)
		mutex_unlock(&object->inode->i_io_lock);
	return first_error;
}

int
vm_object_sync_range(struct vm_object *object, off_t offset, size_t size,
	int flags)
{
	int error;

	if (object == NULL)
		return EINVAL;
	error = object_wait_resize(object);
	if (error != 0)
		return error;
	error = object_operation_begin(object);
	if (error != 0)
		return error;
	error = vm_object_sync_range_internal(object, offset, size, flags, 0, 0,
	    0);
	object_operation_end(object);
	return error;
}

int
vm_object_sync_inode(struct inode *inode)
{
	struct vm_object *object;
	bool enabled;
	int error, removed = 0;

retry_lookup:
	object = NULL;
	enabled = registry_lock();
	for (object = shared_objects; object != NULL; object = object->next)
		if (object->inode == inode) {
			if ((object->flags & VM_OBJECT_DETACHING) != 0) {
				uint64_t sequence =
				    waitq_sequence(&object->registry_waitq);
				object->registry_waiters++;
				if (object->registry_waiters == 0)
					HAL_FATAL("VM object registry waiter overflow");
				refcount_get(&object->refs);
				registry_unlock(enabled);
				error = object_wait_registry_transition(object,
				    sequence);
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
	error = vm_object_sync_range_internal(object, 0, SIZE_MAX, MS_SYNC, 0, 0,
	    0);
	enabled = registry_lock();
	if (object->mapping_count == 0 && object->active_operations == 1 &&
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
	return error;
}

void
vm_object_truncate_inode(struct inode *inode, off_t size)
{
	struct vm_object *object;
	bool enabled;
	int error;

	if (inode == NULL || size < 0)
		return;
retry_lookup:
	enabled = registry_lock();
	object = find_object_by_inode_locked(inode);
	if (object != NULL && (object->flags & VM_OBJECT_DETACHING) != 0) {
		uint64_t sequence = waitq_sequence(&object->registry_waitq);

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
	/* Compatibility callers historically update inode->i_size first.  Do not
	 * publish RESIZING here: an already-running page fill must complete, after
	 * which the normal revoke transaction removes its PTE/cache identity. */
	error = vm_object_sync_range_internal(object,
	    size & ~(off_t)(PAGE_SIZE - 1U), SIZE_MAX,
	    MS_SYNC | MS_INVALIDATE, 0, 0, 0);
	if (error == 0) {
		unsigned long irq = spin_lock_irqsave(&object->lock);

		object->logical_size = size;
		object->size_generation = next_generation(object);
		spin_unlock_irqrestore(&object->lock, irq);
	}
	object_operation_end(object);
	if (refcount_put(&object->refs))
		HAL_FATAL("published VM object lost registry reference");
}

int
vm_object_reclaim_one(void)
{
	struct vm_object *object;
	off_t candidate_offset = 0;
	int found = 0;
	bool enabled;

	vm_metadata_enter();
	enabled = registry_lock();
	for (object = shared_objects; object != NULL; object = object->next) {
		struct vm_object_page *page;
		if ((object->flags & (VM_OBJECT_DETACHING |
		    VM_OBJECT_RESIZING)) != 0)
			continue;
		unsigned long irq = spin_lock_irqsave(&object->lock);
		for (page = object->pages; page != NULL; page = page->next) {
			struct vm_page *mapping;
			int wired = 0;
			if ((page->flags & (VM_OBJECT_PAGE_BUSY |
			    VM_OBJECT_PAGE_WRITEBACK)) != 0 ||
			    page->hold_count != 0)
				continue;
			for (mapping = page->mappings; mapping != NULL;
			     mapping = mapping->object_next)
				if (mapping->wire_count != 0) {
					wired = 1;
					break;
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
	{
		int error = vm_object_sync_range_internal(object, candidate_offset,
		    PAGE_SIZE, MS_SYNC | MS_INVALIDATE, 0, 0, 0);
		int removed = 0;
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
		return error == 0 ? 0 : ENOMEM;
	}
}

unsigned vm_object_count(void)
{
	bool enabled = registry_lock();
	unsigned count = object_count;
	registry_unlock(enabled);
	return count;
}
unsigned vm_object_page_count(void)
{
	bool enabled = registry_lock();
	unsigned count = atomic_load_acquire(&object_pages);
	registry_unlock(enabled);
	return count;
}

unsigned
vm_object_retained_count(void)
{
	struct vm_object *object;
	unsigned count = 0;
	bool enabled = registry_lock();
	for (object = shared_objects; object != NULL; object = object->next)
		if (object->flags & VM_OBJECT_RETAINED_WRITEBACK)
			count++;
	registry_unlock(enabled);
	return count;
}
