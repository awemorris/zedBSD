/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Finds the next resident offset under the object's index lock. */
static struct vm_object_page *sync_page_at_or_after(struct vm_object *object, uint64_t offset);

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

	/* Borrows optional scratch without waiting on a nested drain's owner. */
	capacity = 0;
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

	/* Walks offsets once; only this captured sync generation owns the pages. */
	for (;;) {
		irq = spin_lock_irqsave(&object->lock);
		page = sync_page_at_or_after(object, position);
		if (page == NULL) {
			spin_unlock_irqrestore(&object->lock, irq);
			break;
		}
		position = (uint64_t)page->offset + PAGE_SIZE;
		if (page->write_generation != generation ||
		    page->write_dirty_generation == 0 || page->offset >= logical_size) {
			spin_unlock_irqrestore(&object->lock, irq);
			continue;
		}
		start = page->offset;
		dirty_generation = page->write_dirty_generation;
		spin_unlock_irqrestore(&object->lock, irq);

		/* Keeps a scalar drain available when all optional scratch is occupied. */
		if (scratch == NULL || capacity < PAGE_SIZE) {
			error = write_page_data(object, page, logical_size, inode_io_owned);
			if (error != 0)
				break;
			continue;
		}

		/* Coalesces adjacent captured pages; BUSY protects bytes outside the lock. */
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

			/* Requires exact adjacency and immutable ownership before the next copy. */
			irq = spin_lock_irqsave(&object->lock);
			page = sync_page_at_or_after(object, position);
			if (page == NULL || (uint64_t)page->offset != position ||
			    page->write_generation != generation ||
			    page->write_dirty_generation == 0 || page->offset >= logical_size) {
				spin_unlock_irqrestore(&object->lock, irq);
				break;
			}
			if (page->write_dirty_generation > dirty_generation)
				dirty_generation = page->write_dirty_generation;
			spin_unlock_irqrestore(&object->lock, irq);
		}

		/* Submits one bounded content drain for the captured contiguous bytes. */
		if (object->write_file == NULL) {
			error = EACCES;
			break;
		}
		error = file_io_begin(object->write_file, FILE_IO_PWRITE, start,
		    io_flags, &io);
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
	return error;
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

	/* Pins only the next inode, allowing each completed object to retire normally. */
	for (;;) {
		inode = NULL;
		next = UINT64_MAX;
		enabled = registry_lock();
		for (object = shared_objects; object != NULL; object = object->next) {
			if (object->registry_generation <= cursor ||
			    object->registry_generation > cutoff ||
			    object->registry_generation > next || object->inode == NULL)
				continue;
			if (mount != NULL && object->inode->i_mount != mount)
				continue;
			next = object->registry_generation;
			inode = object->inode;
		}
		if (inode != NULL)
			inode_ref(inode);
		registry_unlock(enabled);
		if (inode == NULL)
			break;

		/* Retains the first failure while independent owners still get a drain attempt. */
		cursor = next;
		error = vm_object_sync_inode_buffer(inode, scratch, capacity);
		inode_release(inode);
		if (error != 0 && first_error == 0)
			first_error = error;
	}

	/* Reports whether every captured owner reached its durability boundary. */
	return first_error;
}
