/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Private speculative pages never delay a demand fault waiting for publication. */

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

	/* Rejects malformed or already owned requests before reserving resources. */
	if (inode == NULL || fill == NULL || fill->object != NULL || offset < 0 ||
	    (offset & (PAGE_SIZE - 1U)) != 0 || length == 0 ||
	    (length & (PAGE_SIZE - 1U)) != 0 ||
	    length > KERN_IO_BATCH_MAX)
		return EINVAL;
	memset(fill, 0, sizeof(*fill));

	/* Leaves absent or transitioning objects to demand-driven cache construction. */
	enabled = registry_lock();
	object = find_object_by_inode_locked(inode);
	if (object == NULL || (object->flags & VM_OBJECT_DETACHING) != 0) {
		registry_unlock(enabled);
		return EAGAIN;
	}
	object->active_operations++;
	if (object->active_operations == 0)
		HAL_FATAL("prefetch operation counter overflow");
	refcount_get(&object->refs);
	registry_unlock(enabled);
	fill->object = object;
	fill->offset = offset;

	/* Pins disk admission for both the pending fill and its later publication. */
	if (inode->i_mount != NULL && inode->i_mount->m_disk != NULL &&
	    disk_cache_acquire != NULL && disk_cache_release != NULL) {
		error = disk_cache_acquire(inode->i_mount->m_disk, &fill->disk);
		if (error != 0) {
			vm_object_prefetch_abort(fill);
			return error;
		}
	}

	/* Captures only a missing prefix of the currently valid file generation. */
	irq = spin_lock_irqsave(&object->lock);
	if ((object->flags & VM_OBJECT_CACHE_REFERENCE) == 0 ||
	    (object->flags & (VM_OBJECT_CONTENT | VM_OBJECT_RESIZING | VM_OBJECT_ANONYMOUS)) != 0 ||
	    offset >= object->logical_size || find_page(object, offset) != NULL) {
		spin_unlock_irqrestore(&object->lock, irq);
		vm_object_prefetch_abort(fill);
		return EAGAIN;
	}
	if ((uint64_t)length > (uint64_t)(object->logical_size - offset))
		length = (size_t)(object->logical_size - offset);
	for (index = 1; (size_t)index * PAGE_SIZE < length; index++) {
		if (find_page(object, offset + (off_t)index * PAGE_SIZE) != NULL) {
			length = (size_t)index * PAGE_SIZE;
			break;
		}
	}
	fill->length = length;
	fill->size_generation = object->size_generation;
	fill->content_generation = object->content_generation;
	spin_unlock_irqrestore(&object->lock, irq);

	/* Allocates private optional frames without publishing BUSY pages to readers. */
	needed = (unsigned)((length + PAGE_SIZE - 1U) / PAGE_SIZE);
	if (needed > VM_OBJECT_PREFETCH_PAGES) {
		vm_object_prefetch_abort(fill);
		return EINVAL;
	}
	while (fill->count < needed) {
		page = alloc_object_page(object,
		    offset + (off_t)fill->count * PAGE_SIZE, 1);
		if (page == NULL) {
			vm_object_prefetch_abort(fill);
			return EAGAIN;
		}
		fill->pages[fill->count++] = page;
	}
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

	/* Rejects stale data and every demand/mapped/write publication that won the race. */
	error = 0;
	irq = spin_lock_irqsave(&object->lock);
	if (object->size_generation != fill->size_generation ||
	    object->content_generation != fill->content_generation ||
	    (object->flags & (VM_OBJECT_CONTENT | VM_OBJECT_RESIZING | VM_OBJECT_DETACHING)) != 0 ||
	    fill->offset > object->logical_size ||
	    (uint64_t)fill->length > (uint64_t)(object->logical_size - fill->offset))
		error = EAGAIN;
	for (index = 0; index < fill->count && error == 0; index++) {
		if (find_page(object, fill->pages[index]->offset) != NULL)
			error = EAGAIN;
	}
	if (error == 0) {
		for (index = 0; index < fill->count; index++) {
			page = fill->pages[index];
			page->prefetch_valid = (unsigned)(fill->length - (size_t)index * PAGE_SIZE);
			if (page->prefetch_valid > PAGE_SIZE)
				page->prefetch_valid = PAGE_SIZE;
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
	vm_object_prefetch_abort(fill);
	return error;
}

/*
 * Retires private frames, disk admission and the retained cache operation.
 */
void
vm_object_prefetch_abort(
	struct vm_object_prefetch *fill)
{
	struct vm_object *object;

	/* Makes cancellation of an unused or previously consumed token idempotent. */
	if (fill == NULL || fill->object == NULL)
		return;
	object = fill->object;
	object_cache_discard_prepared(fill->pages, fill->count);
	if (fill->disk != NULL)
		disk_cache_release(fill->disk);
	memset(fill, 0, sizeof(*fill));
	object_operation_end(object);
	if (refcount_put(&object->refs))
		destroy_object(object);
}

/* Credits disjoint forward demand spans while the object lock serializes readers. */
static size_t
object_prefetch_consume(
	struct vm_object_page *page,
	size_t start,
	size_t length)
{
	size_t end;
	size_t used;

	/* Stops attribution after mappings or writes can replace prefetched contents. */
	if (page->prefetch_valid == 0)
		return 0;
	if (page->mapping_count != 0 ||
	    page->prefetch_size_generation != page->owner->size_generation ||
	    (page->flags & VM_OBJECT_PAGE_DIRTY) != 0) {
		object_prefetch_retire(page);
		return 0;
	}
	end = start + length;
	if (end > page->prefetch_valid)
		end = page->prefetch_valid;
	if (start < page->prefetch_frontier)
		start = page->prefetch_frontier;
	if (end <= start)
		return 0;

	/* Never credits repeated bytes; skipped earlier gaps remain conservatively unused. */
	used = end - start;
	page->prefetch_frontier = (unsigned)end;
	page->prefetch_used += (unsigned)used;
	if (readahead_consumed != NULL)
		readahead_consumed(used, 0);
	return used;
}

/* Retires speculative attribution before content replacement or storage release. */
static void
object_prefetch_retire(
	struct vm_object_page *page)
{
	size_t unused;

	/* Private failed fills never published attribution and therefore contribute nothing. */
	if (page->prefetch_valid == 0)
		return;
	unused = page->prefetch_valid - page->prefetch_used;
	page->prefetch_valid = 0;
	page->prefetch_frontier = 0;
	page->prefetch_used = 0;
	if (readahead_consumed != NULL)
		readahead_consumed(0, unused);
}
