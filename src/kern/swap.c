/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The swap backend.
 *
 * A backend manages up to SWAP_SOURCE_COUNT sources, each a bitmap of
 * page slots served by a driver's read and write hooks.  A slot number
 * encodes its source, so pages can be read back after sources come and
 * go.  Sources move through prepared, active, draining, and removing
 * states; in-flight I/O is counted so that a slot freed under I/O is
 * released only when the I/O completes.  Swap header encoding and
 * validation are implemented separately in swap-format.c.
 */

#include "kern/swap.h"
#include "kern/kmem.h"
#include "kern/lock.h"

#include <errno.h>
#include <hal/hal.h>
#include <stddef.h>
#include <string.h>

static struct swap_backend *system_backend;
static struct spinlock swap_lock = {
	{ 0 }, LOCK_RANK_SWAP, "swap backend", 0, 0
};

static int swap_manager_enable_transition(struct swap_backend *backend, int *enabled_here);
static void swap_manager_disable_empty(struct swap_backend *backend);
static int swap_io(struct swap_backend *backend, uint32_t slot, void *page, int write);

/*
 * Initializes an empty, disabled backend.
 */
void
swap_init(
	struct swap_backend *backend)
{
	/* Ignores a missing backend. */
	if (backend != NULL)
		memset(backend, 0, sizeof(*backend));
}

/*
 * Encodes a source and local slot as a global slot number.
 */
int
swap_slot_encode(
	unsigned source_id,
	uint32_t local_slot,
	uint32_t *slot)
{
	/* Rejects a source or slot out of range, or a missing result. */
	if (source_id >= SWAP_SOURCE_COUNT ||
	    local_slot > SWAP_SLOT_LOCAL_MASK ||
	    slot == NULL)
		return EINVAL;

	*slot = ((uint32_t)source_id << SWAP_SLOT_SOURCE_SHIFT) | local_slot;

	/* Reports the encoded slot. */
	return 0;
}

/*
 * Decodes a global slot number into its source and local slot.
 */
int
swap_slot_decode(
	uint32_t slot,
	unsigned *source_id,
	uint32_t *local_slot)
{
	/* Rejects a slot with bits outside the encoding. */
	if ((slot & ~SWAP_SLOT_VALID_MASK) != 0)
		return EINVAL;

	/* Reports the parts that were asked for. */
	if (source_id != NULL)
		*source_id = (slot & SWAP_SLOT_SOURCE_MASK) >>
		    SWAP_SLOT_SOURCE_SHIFT;
	if (local_slot != NULL)
		*local_slot = slot & SWAP_SLOT_LOCAL_MASK;
	return 0;
}

/*
 * Enables an empty backend.
 */
int
swap_manager_enable(
	struct swap_backend *backend)
{
	int error;

	error = swap_manager_enable_transition(backend, NULL);

	/* Reports the enable result. */
	return error;
}

/*
 * Prepares a source with its driver hooks and slot bitmap.
 *
 * The source is not visible to allocation until it is published.
 */
int
swap_source_prepare(
	struct swap_backend *backend,
	unsigned source_id,
	const struct swap_backend_ops *ops,
	void *data,
	uint32_t page_size,
	uint32_t slot_count)
{
	struct swap_backend_source *source;
	size_t bytes;
	uint8_t *bitmap;
	uint32_t *slot_inflight;
	uint8_t *slot_pending_free;
	unsigned long irq;

	/* Rejects a missing operand, incomplete hooks, or a bad geometry. */
	if (backend == NULL ||
	    ops == NULL ||
	    ops->read_page == NULL ||
	    ops->write_page == NULL ||
	    source_id >= SWAP_SOURCE_COUNT ||
	    page_size != SWAP_PAGE_SIZE ||
	    slot_count == 0)
		return EINVAL;
	if (slot_count > SWAP_SOURCE_MAX_SLOTS)
		return EOVERFLOW;
#if SIZE_MAX <= UINT32_MAX
	if ((size_t)slot_count > SIZE_MAX - 7U ||
	    (size_t)slot_count > SIZE_MAX / sizeof(*slot_inflight))
		return EOVERFLOW;
#endif

	/* Allocates the bitmap and the per-slot I/O bookkeeping. */
	bytes = ((size_t)slot_count + 7U) / 8U;
	bitmap = kern_calloc(1, bytes);
	slot_inflight = kern_calloc(slot_count, sizeof(*slot_inflight));
	slot_pending_free = kern_calloc(slot_count,
	    sizeof(*slot_pending_free));
	if (bitmap == NULL ||
	    slot_inflight == NULL ||
	    slot_pending_free == NULL) {
		kern_free(slot_pending_free);
		kern_free(slot_inflight);
		kern_free(bitmap);
		return ENOMEM;
	}

	/* Installs them in an inactive source of an enabled backend. */
	irq = spin_lock_irqsave(&swap_lock);
	source = &backend->source[source_id];
	if (!backend->enabled ||
	    backend->shutting_down ||
	    source->state != SWAP_SOURCE_STATE_INACTIVE) {
		spin_unlock_irqrestore(&swap_lock, irq);
		kern_free(slot_pending_free);
		kern_free(slot_inflight);
		kern_free(bitmap);
		return EBUSY;
	}
	source->bitmap = bitmap;
	source->slot_inflight = slot_inflight;
	source->slot_pending_free = slot_pending_free;
	source->ops = ops;
	source->data = data;
	source->page_size = page_size;
	source->slot_count = slot_count;
	source->free_slots = slot_count;
	source->state = SWAP_SOURCE_STATE_PREPARED;
	spin_unlock_irqrestore(&swap_lock, irq);

	/* Reports the prepared source. */
	return 0;
}

/*
 * Makes a prepared source available for allocation.
 */
int
swap_source_publish(
	struct swap_backend *backend,
	unsigned source_id)
{
	struct swap_backend_source *source;
	unsigned long irq;

	/* Rejects a missing backend or a source out of range. */
	if (backend == NULL || source_id >= SWAP_SOURCE_COUNT)
		return EINVAL;

	/* Only a prepared source of an enabled backend can be published. */
	irq = spin_lock_irqsave(&swap_lock);
	source = &backend->source[source_id];
	if (!backend->enabled || backend->shutting_down) {
		spin_unlock_irqrestore(&swap_lock, irq);
		return ENXIO;
	}
	if (source->state != SWAP_SOURCE_STATE_PREPARED) {
		spin_unlock_irqrestore(&swap_lock, irq);
		return EBUSY;
	}
	if (backend->slot_count > UINT32_MAX - source->slot_count ||
	    backend->free_slots > UINT32_MAX - source->slot_count) {
		spin_unlock_irqrestore(&swap_lock, irq);
		return EOVERFLOW;
	}

	/* Adds its slots to the backend totals. */
	backend->slot_count += source->slot_count;
	backend->free_slots += source->slot_count;
	backend->source_count++;
	source->state = SWAP_SOURCE_STATE_ACTIVE;
	spin_unlock_irqrestore(&swap_lock, irq);

	/* Reports the published source. */
	return 0;
}

/*
 * Discards a prepared source that will not be published.
 */
int
swap_source_cancel_prepare(
	struct swap_backend *backend,
	unsigned source_id)
{
	struct swap_backend_source *source;
	uint8_t *bitmap;
	uint32_t *slot_inflight;
	uint8_t *slot_pending_free;
	unsigned long irq;

	/* Rejects a missing backend or a source out of range. */
	if (backend == NULL || source_id >= SWAP_SOURCE_COUNT)
		return EINVAL;

	/* Only a prepared source of an enabled backend can be cancelled. */
	irq = spin_lock_irqsave(&swap_lock);
	source = &backend->source[source_id];
	if (!backend->enabled || backend->shutting_down) {
		spin_unlock_irqrestore(&swap_lock, irq);
		return ENXIO;
	}
	if (source->state != SWAP_SOURCE_STATE_PREPARED) {
		spin_unlock_irqrestore(&swap_lock, irq);
		return EBUSY;
	}

	/* Keeps a tombstone until all manager-owned metadata is released. */
	bitmap = source->bitmap;
	slot_inflight = source->slot_inflight;
	slot_pending_free = source->slot_pending_free;
	source->state = SWAP_SOURCE_STATE_REMOVING;
	source->ops = NULL;
	source->data = NULL;
	source->bitmap = NULL;
	source->slot_inflight = NULL;
	source->slot_pending_free = NULL;
	spin_unlock_irqrestore(&swap_lock, irq);
	kern_free(slot_pending_free);
	kern_free(slot_inflight);
	kern_free(bitmap);

	/* Clears the tombstone. */
	irq = spin_lock_irqsave(&swap_lock);
	memset(source, 0, sizeof(*source));
	spin_unlock_irqrestore(&swap_lock, irq);

	/* Reports the cancelled source. */
	return 0;
}

/*
 * Prepares and publishes a source, enabling the backend if needed.
 *
 * A backend enabled here is disabled again when the source cannot be
 * added.
 */
int
swap_source_add(
	struct swap_backend *backend,
	unsigned source_id,
	const struct swap_backend_ops *ops,
	void *data,
	uint32_t page_size,
	uint32_t slot_count)
{
	int enabled_here;
	int error;

	/* Enables the backend for its first source. */
	error = swap_manager_enable_transition(backend, &enabled_here);
	if (error != 0)
		return error;

	/* Prepares the source, undoing the enable on failure. */
	error = swap_source_prepare(backend, source_id, ops, data, page_size,
	    slot_count);
	if (error != 0) {
		if (enabled_here)
			swap_manager_disable_empty(backend);
		return error;
	}

	/* Publishes it, cancelling the preparation on failure. */
	error = swap_source_publish(backend, source_id);
	if (error != 0) {
		if (swap_source_cancel_prepare(backend, source_id) != 0)
			HAL_FATAL("swap source publish rollback failed");
		if (enabled_here)
			swap_manager_disable_empty(backend);
	}

	/* Reports the add result. */
	return error;
}

/*
 * Adds source zero, the boot swap source.
 */
int
swap_activate(
	struct swap_backend *backend,
	const struct swap_backend_ops *ops,
	void *data,
	uint32_t page_size,
	uint32_t slot_count)
{
	int error;

	error = swap_source_add(backend, 0, ops, data, page_size, slot_count);

	/* Reports the add result. */
	return error;
}

/*
 * Allocates a free slot from the first active source that has one.
 */
int
swap_alloc_slot(
	struct swap_backend *backend,
	uint32_t *slot)
{
	struct swap_backend_source *source;
	unsigned source_id;
	unsigned long irq;
	uint32_t index;
	uint8_t mask;

	/* Rejects a missing backend or result. */
	if (backend == NULL || slot == NULL)
		return EINVAL;

	/* Only an enabled backend allocates. */
	irq = spin_lock_irqsave(&swap_lock);
	if (!backend->enabled || backend->shutting_down) {
		spin_unlock_irqrestore(&swap_lock, irq);
		return ENXIO;
	}

	/* Scans the active sources for a clear bitmap bit. */
	for (source_id = 0; source_id < SWAP_SOURCE_COUNT; source_id++) {
		source = &backend->source[source_id];
		if (source->state != SWAP_SOURCE_STATE_ACTIVE ||
		    source->free_slots == 0)
			continue;
		for (index = 0; index < source->slot_count; index++) {
			mask = (uint8_t)(1U << (index & 7U));
			if (!(source->bitmap[index >> 3] & mask)) {
				source->bitmap[index >> 3] |= mask;
				source->slot_pending_free[index] = 0;
				source->free_slots--;
				backend->free_slots--;
				*slot = ((uint32_t)source_id <<
				    SWAP_SLOT_SOURCE_SHIFT) | index;
				spin_unlock_irqrestore(&swap_lock, irq);
				return 0;
			}
		}
	}
	spin_unlock_irqrestore(&swap_lock, irq);

	/* Reports exhausted swap. */
	return ENOSPC;
}

/*
 * Frees a slot, deferring the release while I/O on it is in flight.
 */
void
swap_free_slot(
	struct swap_backend *backend,
	uint32_t slot)
{
	struct swap_backend_source *source;
	unsigned source_id;
	uint32_t local_slot;
	uint8_t mask;
	unsigned long irq;

	/* Ignores a missing backend or a malformed slot. */
	if (backend == NULL ||
	    swap_slot_decode(slot, &source_id, &local_slot) != 0)
		return;

	/* Only a slot of an active or draining source can be freed. */
	irq = spin_lock_irqsave(&swap_lock);
	source = &backend->source[source_id];
	if (!backend->enabled ||
	    (source->state != SWAP_SOURCE_STATE_ACTIVE &&
	     source->state != SWAP_SOURCE_STATE_DRAINING) ||
	    local_slot >= source->slot_count) {
		spin_unlock_irqrestore(&swap_lock, irq);
		return;
	}

	/* Releases the slot now, or once its I/O completes. */
	mask = (uint8_t)(1U << (local_slot & 7U));
	if (source->bitmap[local_slot >> 3] & mask) {
		if (source->slot_inflight[local_slot] != 0) {
			source->slot_pending_free[local_slot] = 1;
		} else {
			source->bitmap[local_slot >> 3] &= (uint8_t)~mask;
			source->free_slots++;
			backend->free_slots++;
		}
	}
	spin_unlock_irqrestore(&swap_lock, irq);
}

/*
 * Reads a page from a slot.
 */
int
swap_read_page(
	struct swap_backend *backend,
	uint32_t slot,
	void *page)
{
	int error;

	error = swap_io(backend, slot, page, 0);

	/* Reports the read result. */
	return error;
}

/*
 * Writes a page to a slot.
 */
int
swap_write_page(
	struct swap_backend *backend,
	uint32_t slot,
	const void *page)
{
	int error;

	error = swap_io(backend, slot, (void *)page, 1);

	/* Reports the write result. */
	return error;
}

/*
 * Flushes every active and draining source.
 *
 * The sources are held in flight while their flush hooks run outside
 * the lock; the first error is reported after every source was flushed.
 */
int
swap_flush(
	struct swap_backend *backend)
{
	const struct swap_backend_ops *ops[SWAP_SOURCE_COUNT];
	struct swap_backend_source *source;
	void *data[SWAP_SOURCE_COUNT];
	uint8_t reserved[SWAP_SOURCE_COUNT];
	unsigned long irq;
	unsigned source_id;
	int first_error;
	int error;

	first_error = 0;

	/* Rejects a missing backend. */
	if (backend == NULL)
		return EINVAL;
	memset(reserved, 0, sizeof(reserved));

	/* Only an enabled backend with no removal in progress flushes. */
	irq = spin_lock_irqsave(&swap_lock);
	if (!backend->enabled || backend->shutting_down) {
		spin_unlock_irqrestore(&swap_lock, irq);
		return ENXIO;
	}
	for (source_id = 0; source_id < SWAP_SOURCE_COUNT; source_id++) {
		source = &backend->source[source_id];
		if (source->state == SWAP_SOURCE_STATE_REMOVING) {
			spin_unlock_irqrestore(&swap_lock, irq);
			return EBUSY;
		}
	}

	/* Holds every live source in flight and remembers its hooks. */
	for (source_id = 0; source_id < SWAP_SOURCE_COUNT; source_id++) {
		source = &backend->source[source_id];
		if (source->state != SWAP_SOURCE_STATE_ACTIVE &&
		    source->state != SWAP_SOURCE_STATE_DRAINING)
			continue;
		reserved[source_id] = 1;
		ops[source_id] = source->ops;
		data[source_id] = source->data;
		source->inflight++;
		backend->inflight++;
	}
	spin_unlock_irqrestore(&swap_lock, irq);

	/* Flushes outside the lock, keeping the first error. */
	for (source_id = 0; source_id < SWAP_SOURCE_COUNT; source_id++) {
		if (!reserved[source_id])
			continue;
		if (ops[source_id]->flush != NULL)
			error = ops[source_id]->flush(data[source_id]);
		else
			error = 0;
		if (first_error == 0 && error != 0)
			first_error = error;
	}

	/* Releases the in-flight holds. */
	irq = spin_lock_irqsave(&swap_lock);
	for (source_id = 0; source_id < SWAP_SOURCE_COUNT; source_id++) {
		if (reserved[source_id]) {
			backend->source[source_id].inflight--;
			backend->inflight--;
		}
	}
	spin_unlock_irqrestore(&swap_lock, irq);

	/* Reports the first flush failure. */
	return first_error;
}

/*
 * Stops allocation from a source so that it can be emptied and removed.
 */
int
swap_source_begin_drain(
	struct swap_backend *backend,
	unsigned source_id)
{
	struct swap_backend_source *source;
	unsigned long irq;

	/* Rejects a missing backend or a source out of range. */
	if (backend == NULL || source_id >= SWAP_SOURCE_COUNT)
		return EINVAL;

	/* Only an active or already draining source of a live backend drains. */
	irq = spin_lock_irqsave(&swap_lock);
	source = &backend->source[source_id];
	if (!backend->enabled || source->state == SWAP_SOURCE_STATE_INACTIVE) {
		spin_unlock_irqrestore(&swap_lock, irq);
		return ENXIO;
	}
	if (backend->shutting_down ||
	    (source->state != SWAP_SOURCE_STATE_ACTIVE &&
	     source->state != SWAP_SOURCE_STATE_DRAINING)) {
		spin_unlock_irqrestore(&swap_lock, irq);
		return EBUSY;
	}
	source->state = SWAP_SOURCE_STATE_DRAINING;
	spin_unlock_irqrestore(&swap_lock, irq);

	/* Reports the draining source. */
	return 0;
}

/*
 * Returns a draining source to allocation.
 */
int
swap_source_abort_drain(
	struct swap_backend *backend,
	unsigned source_id)
{
	struct swap_backend_source *source;
	unsigned long irq;

	/* Rejects a missing backend or a source out of range. */
	if (backend == NULL || source_id >= SWAP_SOURCE_COUNT)
		return EINVAL;

	/* Only a draining source of a live backend can be reactivated. */
	irq = spin_lock_irqsave(&swap_lock);
	source = &backend->source[source_id];
	if (!backend->enabled || source->state == SWAP_SOURCE_STATE_INACTIVE) {
		spin_unlock_irqrestore(&swap_lock, irq);
		return ENXIO;
	}
	if (backend->shutting_down ||
	    source->state != SWAP_SOURCE_STATE_DRAINING) {
		spin_unlock_irqrestore(&swap_lock, irq);
		return EBUSY;
	}
	source->state = SWAP_SOURCE_STATE_ACTIVE;
	spin_unlock_irqrestore(&swap_lock, irq);

	/* Reports the reactivated source. */
	return 0;
}

/*
 * Reports the state and slot counts of a source.
 */
int
swap_source_get_stats(
	struct swap_backend *backend,
	unsigned source_id,
	struct swap_source_stats *stats)
{
	struct swap_backend_source *source;
	unsigned long irq;

	/* Rejects a missing operand or a source out of range. */
	if (backend == NULL || source_id >= SWAP_SOURCE_COUNT || stats == NULL)
		return EINVAL;

	/* Samples the source under the lock. */
	irq = spin_lock_irqsave(&swap_lock);
	source = &backend->source[source_id];
	stats->source_id = source_id;
	stats->state = source->state;
	stats->total_slots = source->slot_count;
	stats->free_slots = source->free_slots;
	stats->allocated_slots = source->slot_count - source->free_slots;
	stats->inflight = source->inflight;
	spin_unlock_irqrestore(&swap_lock, irq);

	/* Reports the sampled statistics. */
	return 0;
}

/*
 * Removes a drained source whose slots are all free.
 *
 * The source is flushed and destroyed outside the lock while a tombstone
 * keeps its identifier from being reused.
 */
int
swap_source_remove(
	struct swap_backend *backend,
	unsigned source_id)
{
	struct swap_backend_source *source;
	const struct swap_backend_ops *ops;
	void *data;
	uint8_t *bitmap;
	uint32_t *slot_inflight;
	uint8_t *slot_pending_free;
	uint32_t slot_count;
	unsigned long irq;
	int flush_error;

	/* Rejects a missing backend or a source out of range. */
	if (backend == NULL || source_id >= SWAP_SOURCE_COUNT)
		return EINVAL;

	/* Only an idle, empty, draining source of a live backend is removed. */
	irq = spin_lock_irqsave(&swap_lock);
	source = &backend->source[source_id];
	if (!backend->enabled || source->state == SWAP_SOURCE_STATE_INACTIVE) {
		spin_unlock_irqrestore(&swap_lock, irq);
		return ENXIO;
	}
	if (backend->shutting_down ||
	    source->state != SWAP_SOURCE_STATE_DRAINING ||
	    source->inflight != 0 ||
	    source->free_slots != source->slot_count) {
		spin_unlock_irqrestore(&swap_lock, irq);
		return EBUSY;
	}

	/* Excludes I/O, shutdown, and ID reuse while flush is in progress. */
	source->state = SWAP_SOURCE_STATE_REMOVING;
	source->inflight = 1;
	backend->inflight++;
	ops = source->ops;
	data = source->data;
	spin_unlock_irqrestore(&swap_lock, irq);
	if (ops->flush != NULL)
		flush_error = ops->flush(data);
	else
		flush_error = 0;

	/* A failed flush puts the source back to draining. */
	irq = spin_lock_irqsave(&swap_lock);
	backend->inflight--;
	source->inflight--;
	if (flush_error != 0) {
		source->state = SWAP_SOURCE_STATE_DRAINING;
		spin_unlock_irqrestore(&swap_lock, irq);
		return flush_error;
	}

	/*
	 * Keeps a REMOVING tombstone until callbacks and frees finish.  This
	 * prevents a concurrent add from reusing the numeric ID while the
	 * old lifecycle is still observable, without freeing memory under a
	 * lock.
	 */
	bitmap = source->bitmap;
	slot_inflight = source->slot_inflight;
	slot_pending_free = source->slot_pending_free;
	slot_count = source->slot_count;
	source->ops = NULL;
	source->data = NULL;
	source->bitmap = NULL;
	source->slot_inflight = NULL;
	source->slot_pending_free = NULL;
	backend->slot_count -= slot_count;
	backend->free_slots -= slot_count;
	backend->source_count--;
	spin_unlock_irqrestore(&swap_lock, irq);

	/* Destroys the driver state and the metadata. */
	if (ops->destroy != NULL)
		ops->destroy(data);
	kern_free(slot_pending_free);
	kern_free(slot_inflight);
	kern_free(bitmap);

	/* Clears the tombstone. */
	irq = spin_lock_irqsave(&swap_lock);
	memset(source, 0, sizeof(*source));
	spin_unlock_irqrestore(&swap_lock, irq);

	/* Reports the removed source. */
	return 0;
}

/*
 * Shuts an idle backend down, flushing and destroying every source.
 *
 * The backend must have no I/O in flight and every slot free.  The
 * sources are detached under the lock and flushed before they are
 * destroyed, in numeric order.
 */
int
swap_shutdown(
	struct swap_backend *backend)
{
	struct swap_backend_source detached[SWAP_SOURCE_COUNT];
	struct swap_backend_source *source;
	struct swap_backend_source *detached_source;
	unsigned long irq;
	unsigned source_id;
	int flush_error;
	int error;

	flush_error = 0;

	/* A missing or disabled backend needs no shutdown. */
	if (backend == NULL)
		return 0;
	memset(detached, 0, sizeof(detached));
	irq = spin_lock_irqsave(&swap_lock);
	if (!backend->enabled) {
		spin_unlock_irqrestore(&swap_lock, irq);
		return 0;
	}

	/* Every source must be idle and empty, with no transition under way. */
	if (backend->shutting_down || backend->inflight != 0) {
		spin_unlock_irqrestore(&swap_lock, irq);
		return EBUSY;
	}
	for (source_id = 0; source_id < SWAP_SOURCE_COUNT; source_id++) {
		source = &backend->source[source_id];
		if (source->state == SWAP_SOURCE_STATE_PREPARED ||
		    source->state == SWAP_SOURCE_STATE_REMOVING ||
		    source->inflight != 0 ||
		    (source->state != SWAP_SOURCE_STATE_INACTIVE &&
		     source->free_slots != source->slot_count)) {
			spin_unlock_irqrestore(&swap_lock, irq);
			return EBUSY;
		}
	}

	/* Detaches the sources, leaving tombstones. */
	backend->shutting_down = 1;
	if (system_backend == backend)
		system_backend = NULL;
	for (source_id = 0; source_id < SWAP_SOURCE_COUNT; source_id++) {
		source = &backend->source[source_id];
		if (source->state == SWAP_SOURCE_STATE_INACTIVE)
			continue;
		detached[source_id] = *source;
		memset(source, 0, sizeof(*source));
		source->state = SWAP_SOURCE_STATE_REMOVING;
	}
	backend->slot_count = 0;
	backend->free_slots = 0;
	backend->source_count = 0;
	spin_unlock_irqrestore(&swap_lock, irq);

	/* Preserves numeric flush-before-destroy ordering from the boot backend. */
	for (source_id = 0; source_id < SWAP_SOURCE_COUNT; source_id++) {
		detached_source = &detached[source_id];
		if (detached_source->state == SWAP_SOURCE_STATE_INACTIVE)
			continue;
		if (detached_source->ops->flush != NULL)
			error = detached_source->ops->flush(detached_source->data);
		else
			error = 0;
		if (flush_error == 0 && error != 0)
			flush_error = error;
	}
	for (source_id = 0; source_id < SWAP_SOURCE_COUNT; source_id++) {
		detached_source = &detached[source_id];
		if (detached_source->state == SWAP_SOURCE_STATE_INACTIVE)
			continue;
		if (detached_source->ops->destroy != NULL)
			detached_source->ops->destroy(detached_source->data);
		kern_free(detached_source->slot_pending_free);
		kern_free(detached_source->slot_inflight);
		kern_free(detached_source->bitmap);
	}

	/* Clears the backend. */
	irq = spin_lock_irqsave(&swap_lock);
	memset(backend, 0, sizeof(*backend));
	spin_unlock_irqrestore(&swap_lock, irq);

	/* Reports the first flush failure. */
	return flush_error;
}

/*
 * Publishes a backend as the system swap backend.
 */
int
swap_set_system_backend(
	struct swap_backend *backend)
{
	unsigned long irq;
	int error;

	error = 0;

	/* Rejects a missing backend. */
	if (backend == NULL)
		return EINVAL;

	irq = spin_lock_irqsave(&swap_lock);

	/*
	 * Publication is part of the backend lifecycle transaction.  Check
	 * the live state while holding the same lock used by swap_shutdown();
	 * a pre-lock check can otherwise publish a backend after shutdown has
	 * detached and destroyed its data.
	 */
	if (!backend->enabled || backend->shutting_down)
		error = ENXIO;
	else if (system_backend != NULL && system_backend != backend)
		error = EBUSY;
	else
		system_backend = backend;
	spin_unlock_irqrestore(&swap_lock, irq);

	/* Reports the publication result. */
	return error;
}

/*
 * Reads the system swap backend, or none.
 */
struct swap_backend *
swap_system_backend(
	void)
{
	struct swap_backend *backend;
	unsigned long irq;

	/* Samples the pointer under the lock. */
	irq = spin_lock_irqsave(&swap_lock);
	backend = system_backend;
	spin_unlock_irqrestore(&swap_lock, irq);

	/* Reports the backend, or none. */
	return backend;
}

/*
 * Reports the total and free slot counts of a backend.
 */
int
swap_get_stats(
	struct swap_backend *backend,
	uint32_t *total,
	uint32_t *free_slots)
{
	unsigned long irq;

	/* Rejects a missing operand. */
	if (backend == NULL || total == NULL || free_slots == NULL)
		return EINVAL;

	/* Only an enabled backend has statistics. */
	irq = spin_lock_irqsave(&swap_lock);
	if (!backend->enabled || backend->shutting_down) {
		spin_unlock_irqrestore(&swap_lock, irq);
		return ENXIO;
	}
	*total = backend->slot_count;
	*free_slots = backend->free_slots;
	spin_unlock_irqrestore(&swap_lock, irq);

	/* Reports the sampled counts. */
	return 0;
}

/* Enables an empty backend, reporting whether this call did it. */
static int
swap_manager_enable_transition(
	struct swap_backend *backend,
	int *enabled_here)
{
	unsigned long irq;
	unsigned source_id;

	/* Rejects a missing backend. */
	if (enabled_here != NULL)
		*enabled_here = 0;
	if (backend == NULL)
		return EINVAL;

	/* An enabled backend stays enabled; a stopping one refuses. */
	irq = spin_lock_irqsave(&swap_lock);
	if (backend->shutting_down) {
		spin_unlock_irqrestore(&swap_lock, irq);
		return EBUSY;
	}
	if (backend->enabled) {
		spin_unlock_irqrestore(&swap_lock, irq);
		return 0;
	}

	/* A disabled backend must be completely empty. */
	if (backend->slot_count != 0 ||
	    backend->free_slots != 0 ||
	    backend->source_count != 0 ||
	    backend->inflight != 0) {
		spin_unlock_irqrestore(&swap_lock, irq);
		return EBUSY;
	}
	for (source_id = 0; source_id < SWAP_SOURCE_COUNT; source_id++) {
		if (backend->source[source_id].state !=
		    SWAP_SOURCE_STATE_INACTIVE) {
			spin_unlock_irqrestore(&swap_lock, irq);
			return EBUSY;
		}
	}

	/* Enables it. */
	backend->enabled = 1;
	if (enabled_here != NULL)
		*enabled_here = 1;
	spin_unlock_irqrestore(&swap_lock, irq);

	/* Reports the enabled backend. */
	return 0;
}

/* Undoes a private enable of a backend that stayed empty. */
static void
swap_manager_disable_empty(
	struct swap_backend *backend)
{
	unsigned long irq;
	unsigned source_id;

	/*
	 * Leaves alone anything another control path may have published in
	 * the meantime: the system backend, any source, or in-flight I/O.
	 */
	irq = spin_lock_irqsave(&swap_lock);
	if (!backend->enabled ||
	    backend->shutting_down ||
	    system_backend == backend ||
	    backend->slot_count != 0 ||
	    backend->free_slots != 0 ||
	    backend->source_count != 0 ||
	    backend->inflight != 0) {
		spin_unlock_irqrestore(&swap_lock, irq);
		return;
	}
	for (source_id = 0; source_id < SWAP_SOURCE_COUNT; source_id++) {
		if (backend->source[source_id].state !=
		    SWAP_SOURCE_STATE_INACTIVE) {
			spin_unlock_irqrestore(&swap_lock, irq);
			return;
		}
	}
	backend->enabled = 0;
	spin_unlock_irqrestore(&swap_lock, irq);
}

/* Reads or writes one slot through its source, counting the I/O in flight. */
static int
swap_io(
	struct swap_backend *backend,
	uint32_t slot,
	void *page,
	int write)
{
	struct swap_backend_source *source;
	const struct swap_backend_ops *ops;
	void *data;
	unsigned source_id;
	uint32_t local_slot;
	unsigned long irq;
	uint8_t mask;
	int error;

	/* Rejects a missing operand or a malformed slot. */
	if (backend == NULL || page == NULL)
		return EINVAL;
	if (swap_slot_decode(slot, &source_id, &local_slot) != 0)
		return EINVAL;

	/* The slot must be allocated in a live source and not being freed. */
	irq = spin_lock_irqsave(&swap_lock);
	source = &backend->source[source_id];
	if (!backend->enabled ||
	    backend->shutting_down ||
	    (source->state != SWAP_SOURCE_STATE_ACTIVE &&
	     source->state != SWAP_SOURCE_STATE_DRAINING) ||
	    local_slot >= source->slot_count) {
		spin_unlock_irqrestore(&swap_lock, irq);
		return ENXIO;
	}
	mask = (uint8_t)(1U << (local_slot & 7U));
	if (!(source->bitmap[local_slot >> 3] & mask) ||
	    source->slot_pending_free[local_slot]) {
		spin_unlock_irqrestore(&swap_lock, irq);
		return EINVAL;
	}

	/* Holds the slot and source in flight while the driver works. */
	backend->inflight++;
	source->inflight++;
	source->slot_inflight[local_slot]++;
	ops = source->ops;
	data = source->data;
	spin_unlock_irqrestore(&swap_lock, irq);
	if (write)
		error = ops->write_page(data, local_slot, page);
	else
		error = ops->read_page(data, local_slot, page);

	/* Releases the holds, completing a free deferred behind the I/O. */
	irq = spin_lock_irqsave(&swap_lock);
	source->slot_inflight[local_slot]--;
	if (source->slot_inflight[local_slot] == 0 &&
	    source->slot_pending_free[local_slot]) {
		source->slot_pending_free[local_slot] = 0;
		source->bitmap[local_slot >> 3] &= (uint8_t)~mask;
		source->free_slots++;
		backend->free_slots++;
	}
	source->inflight--;
	backend->inflight--;
	spin_unlock_irqrestore(&swap_lock, irq);

	/* Reports the driver's result. */
	return error;
}
