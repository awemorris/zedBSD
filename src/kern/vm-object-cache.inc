/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Bounded clean file-object retention, sharing the existing VM registry.
 */

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

	/* Excludes non-files and internal backing owners from optional caching. */
	if (file == NULL || file->f_inode == NULL ||
	    file->f_inode->i_type != INODE_REG || file->f_ops == NULL ||
	    file->f_ops->pread == NULL || file->f_format_claim != NULL ||
	    file->f_backing_claim != NULL)
		return;
	inode = file_vm_inode(file);
	if (inode == NULL ||
	    (inode->i_flags & (INODE_LOOPFILE | INODE_SWAPFILE)) != 0)
		return;

	/* Keeps a competing claim from publishing over the new cache identity. */
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

	/* Drops the operation reference while retaining the independent cache. */
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

	/* Refuses absent and transitioning identities without allocating or waiting. */
	if (inode == NULL || result == NULL)
		return EINVAL;
	*result = NULL;
	enabled = registry_lock();
	object = find_object_by_inode_locked(inode);
	if (object == NULL || (object->flags & VM_OBJECT_DETACHING) != 0) {
		registry_unlock(enabled);
		return EAGAIN;
	}

	/* Keeps final mapping teardown and cache eviction outside the transaction. */
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

/* Adds an operation or mapping reference while the registry lock is held. */
static int
object_reference_locked(
	struct vm_object *object,
	struct file *file,
	int cache_only)
{
	int writable;

	/* Existing mapping objects keep their original close and retention policy. */

	/* Preserves an existing write-capable handle for shared dirty pages. */
	writable = !cache_only &&
	    (file_status_flags_get(file) & O_ACCMODE) != O_RDONLY &&
	    file->f_ops != NULL && file->f_ops->pwrite != NULL;
	if (writable && object->write_file == NULL) {
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

	/* Shared physical credits now bound clean retention across all objects. */
	if ((object->flags & VM_OBJECT_CACHE_REFERENCE) == 0)
		return 0;
	retainable = object_can_destroy(object);
	return retainable;
}

/* Unlinks one idle cache under the registry lock, then closes it outside locks. */
static int
object_cache_evict_one(
	struct mount *mount)
{
	struct vm_object *object;
	bool enabled;

	/* Leaves every mandatory mapping, operation, pin and failure owner intact. */
	enabled = registry_lock();
	for (object = shared_objects; object != NULL; object = object->next) {
		if ((object->flags & VM_OBJECT_CACHE_REFERENCE) == 0 ||
		    (object->flags & (VM_OBJECT_DETACHING | VM_OBJECT_RESIZING |
		    VM_OBJECT_CONTENT | VM_OBJECT_RETAINED_WRITEBACK)) != 0 ||
		    object->active_operations != 0 || object->registry_waiters != 0 ||
		    refcount_load(&object->refs) != 1 || !object_can_destroy(object))
			continue;
		if (mount != NULL && object->file->f_path.p_mount != mount &&
		    (object->write_file == NULL ||
		    object->write_file->f_path.p_mount != mount))
			continue;
		if (!unlink_object_locked(object))
			HAL_FATAL("VM cache eviction lost registry entry");
		break;
	}
	registry_unlock(enabled);
	if (object == NULL)
		return 0;

	/* Releases the sole registry/cache reference after closing admission. */
	if (!refcount_put(&object->refs))
		HAL_FATAL("detached VM cache retained an unknown reference");
	destroy_object(object);

	/* Reports one complete eviction. */
	return 1;
}

/* Frees only unpublished preparation frames, which were never counted resident. */
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

/* Reads an absent bounded run and publishes only fully initialized cache pages. */
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
	eof = object->logical_size;
	size_generation = object->size_generation;
	content_generation = object->content_generation;
	spin_unlock_irqrestore(&object->lock, irq);
	if (offset >= eof) {
		*handled = 1;
		*stop = 1;
		return 0;
	}

	/* Bounds the run by requested bytes, EOF and the first resident page. */
	wanted = KERN_IO_BATCH_MAX - in_page;
	if (wanted > length)
		wanted = length;
	if ((uint64_t)wanted > (uint64_t)(eof - offset))
		wanted = (size_t)(eof - offset);
	transfer = (in_page + wanted + PAGE_SIZE - 1U) & ~(size_t)(PAGE_SIZE - 1U);
	if ((uint64_t)transfer > (uint64_t)(eof - base))
		transfer = (size_t)(eof - base);
	irq = spin_lock_irqsave(&object->lock);
	for (index = 1; (size_t)index * PAGE_SIZE < transfer; index++) {
		if (find_page(object, base + (off_t)index * PAGE_SIZE) != NULL) {
			transfer = (size_t)index * PAGE_SIZE;
			break;
		}
	}
	spin_unlock_irqrestore(&object->lock, irq);
	if (wanted > transfer - in_page)
		wanted = transfer - in_page;

	/* Borrows preparation storage without waiting inside the content read lease. */
	scratch = NULL;
	capacity = 0;
	if (io_pool_borrow != NULL &&
	    io_pool_release != NULL)
		scratch = io_pool_borrow(transfer, &capacity);
	prepared = 0;
	needed = (unsigned)((transfer + PAGE_SIZE - 1U) / PAGE_SIZE);
	publish = scratch != NULL && capacity >= transfer &&
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

	/* Revalidates the complete publication set after allocation and racing faults. */
	if (publish) {
		irq = spin_lock_irqsave(&object->lock);
		if (object->size_generation != size_generation ||
		    object->content_generation != content_generation)
			publish = 0;
		for (index = 0; index < prepared && publish; index++) {
			if (find_page(object, pages[index]->offset) != NULL)
				publish = 0;
		}
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

	/* Returns unused preparation before using the ordinary leased backend path. */
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
		error = file_io_begin(object->file, FILE_IO_PREAD, offset,
		    FILE_IO_VM_OBJECT, &io);
		if (error != 0)
			return error;
		count = file_io_transfer(&io, buffer, wanted);
		file_io_end(&io);
		*handled = 1;
		if (count < 0)
			return (int)-count;
		*result = count;
		*stop = (size_t)count < wanted;
		return 0;
	}

	/* Fills the run once; the explicit internal purpose prevents recursive caching. */
	memset(scratch, 0, transfer);
	error = file_io_begin(object->file, FILE_IO_PREAD, base,
	    FILE_IO_VM_OBJECT, &io);
	if (error != 0) {
		count = -(ssize_t)error;
	} else {
		count = file_io_transfer(&io, scratch, transfer);
		file_io_end(&io);
	}

	/* Publishes only complete pages and keeps failed fills retryable for waiters. */
	irq = spin_lock_irqsave(&object->lock);
	if (object->size_generation != size_generation ||
	    object->content_generation != content_generation)
		count = -EAGAIN;
	if (count > (ssize_t)transfer)
		count = -EIO;
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

	/* Preserves the confirmed short-read prefix rather than discarding it. */
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

	/* A short prefix ending before the requested offset cannot mean file EOF. */
	if (count > 0 && (size_t)count <= in_page)
		return EIO;

	/* Reports either the complete selected run or the backend's short result. */
	return 0;
}
