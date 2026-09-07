/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Intrusive dirty membership: cache -> buffer -> dirty-index is the lock order.
 * Selection takes a reference under the index guard and drops that guard before
 * touching LRU metadata or a buffer. Eviction rechecks refs under the same guard.
 */

/* Publishes a first dirty transition while the caller holds the buffer lock. */
static void
dirty_link(
	struct buf *buffer)
{
	unsigned long irq;

	/* Inserts into the global age list and the owning device's list in O(1). */
	irq = spin_lock_irqsave(&dirty_index_lock);
	if (buffer->b_dirty_linked)
		HAL_FATAL("dirty buffer linked twice");
	buffer->b_dirty_previous = dirty_tail;
	buffer->b_dirty_next = NULL;
	if (dirty_tail != NULL)
		dirty_tail->b_dirty_next = buffer;
	else
		dirty_head = buffer;
	dirty_tail = buffer;
	buffer->b_device_dirty_previous = NULL;
	buffer->b_device_dirty_next = buffer->b_disk->d_dirty_buffers;
	if (buffer->b_device_dirty_next != NULL)
		buffer->b_device_dirty_next->b_device_dirty_previous = buffer;
	buffer->b_disk->d_dirty_buffers = buffer;
	buffer->b_dirty_linked = 1;
	spin_unlock_irqrestore(&dirty_index_lock, irq);
}

/* Removes a resolved dirty transition while the caller holds the buffer lock. */
static void
dirty_clear(
	struct buf *buffer)
{
	unsigned long irq;

	/* Serializes clean publication against dirty-candidate reference acquisition. */
	irq = spin_lock_irqsave(&dirty_index_lock);
	dirty_unlink_locked(buffer);
	spin_unlock_irqrestore(&dirty_index_lock, irq);
}

/* Detaches both memberships with the index guard already held. */
static void
dirty_unlink_locked(
	struct buf *buffer)
{
	/* Clean or never-published buffers have no membership to remove. */
	if (!buffer->b_dirty_linked)
		return;

	/* Repairs the global age list before clearing the descriptor links. */
	if (buffer->b_dirty_previous != NULL)
		buffer->b_dirty_previous->b_dirty_next = buffer->b_dirty_next;
	else
		dirty_head = buffer->b_dirty_next;
	if (buffer->b_dirty_next != NULL)
		buffer->b_dirty_next->b_dirty_previous = buffer->b_dirty_previous;
	else
		dirty_tail = buffer->b_dirty_previous;

	/* Repairs the device index without scanning its other dirty buffers. */
	if (buffer->b_device_dirty_previous != NULL)
		buffer->b_device_dirty_previous->b_device_dirty_next = buffer->b_device_dirty_next;
	else
		buffer->b_disk->d_dirty_buffers = buffer->b_device_dirty_next;
	if (buffer->b_device_dirty_next != NULL)
		buffer->b_device_dirty_next->b_device_dirty_previous = buffer->b_device_dirty_previous;
	buffer->b_dirty_previous = NULL;
	buffer->b_dirty_next = NULL;
	buffer->b_device_dirty_previous = NULL;
	buffer->b_device_dirty_next = NULL;
	buffer->b_dirty_linked = 0;
}

/* Pins one dirty member before exposing it outside the index guard. */
static struct buf *
dirty_reference(
	struct disk *disk,
	uint64_t start,
	uint64_t end,
	int reclaim)
{
	struct buf *buffer;
	unsigned long irq;

	/* Scans only dirty membership, never unrelated clean cache hash buckets. */
	irq = spin_lock_irqsave(&dirty_index_lock);
	buffer = disk != NULL ? disk->d_dirty_buffers : dirty_head;
	while (buffer != NULL) {
		if ((!reclaim || refcount_load(&buffer->b_refs) == 1) &&
		    (disk == NULL || (buffer->b_block < end &&
		    buffer->b_block + buffer->b_block_count > start))) {
			refcount_get(&buffer->b_refs);
			break;
		}
		buffer = disk != NULL ? buffer->b_device_dirty_next : buffer->b_dirty_next;
	}
	spin_unlock_irqrestore(&dirty_index_lock, irq);

	/* Takes the lower-ranked cache lock only after releasing the index guard. */
	if (buffer != NULL) {
		irq = spin_lock_irqsave(&cache_lock);
		lru_remove_locked(buffer);
		spin_unlock_irqrestore(&cache_lock, irq);
	}
	return buffer;
}
