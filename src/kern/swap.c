/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
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

void swap_init(struct swap_backend *backend)
{
	if (backend != NULL)
		memset(backend, 0, sizeof(*backend));
}

int
swap_slot_encode(unsigned source_id, uint32_t local_slot, uint32_t *slot)
{
	if (source_id >= SWAP_SOURCE_COUNT ||
	    local_slot > SWAP_SLOT_LOCAL_MASK || slot == NULL)
		return EINVAL;
	*slot = ((uint32_t)source_id << SWAP_SLOT_SOURCE_SHIFT) | local_slot;
	return 0;
}

int
swap_slot_decode(uint32_t slot, unsigned *source_id, uint32_t *local_slot)
{
	if ((slot & ~SWAP_SLOT_VALID_MASK) != 0)
		return EINVAL;
	if (source_id != NULL)
		*source_id = (slot & SWAP_SLOT_SOURCE_MASK) >>
		    SWAP_SLOT_SOURCE_SHIFT;
	if (local_slot != NULL)
		*local_slot = slot & SWAP_SLOT_LOCAL_MASK;
	return 0;
}

static int
swap_manager_enable_transition(struct swap_backend *backend, int *enabled_here)
{
	unsigned long irq;
	unsigned source_id;

	if (enabled_here != NULL)
		*enabled_here = 0;
	if (backend == NULL)
		return EINVAL;
	irq = spin_lock_irqsave(&swap_lock);
	if (backend->shutting_down) {
		spin_unlock_irqrestore(&swap_lock, irq);
		return EBUSY;
	}
	if (backend->enabled) {
		spin_unlock_irqrestore(&swap_lock, irq);
		return 0;
	}
	if (backend->slot_count != 0 || backend->free_slots != 0 ||
	    backend->source_count != 0 || backend->inflight != 0) {
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
	backend->enabled = 1;
	if (enabled_here != NULL)
		*enabled_here = 1;
	spin_unlock_irqrestore(&swap_lock, irq);
	return 0;
}

int
swap_manager_enable(struct swap_backend *backend)
{
	return swap_manager_enable_transition(backend, NULL);
}

/* Undo a private empty-manager enable without tearing down work which another
 * control path may have published in the meantime. */
static void
swap_manager_disable_empty(struct swap_backend *backend)
{
	unsigned long irq;
	unsigned source_id;

	irq = spin_lock_irqsave(&swap_lock);
	if (!backend->enabled || backend->shutting_down ||
	    system_backend == backend || backend->slot_count != 0 ||
	    backend->free_slots != 0 || backend->source_count != 0 ||
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

int
swap_source_prepare(struct swap_backend *backend, unsigned source_id,
		    const struct swap_backend_ops *ops, void *data,
		    uint32_t page_size, uint32_t slot_count)
{
	size_t bytes;
	uint8_t *bitmap;
	uint32_t *slot_inflight;
	uint8_t *slot_pending_free;
	struct swap_backend_source *source;
	unsigned long irq;

	if (backend == NULL || ops == NULL || ops->read_page == NULL ||
	    ops->write_page == NULL || source_id >= SWAP_SOURCE_COUNT ||
	    page_size != SWAP_PAGE_SIZE || slot_count == 0)
		return EINVAL;
	if (slot_count > SWAP_SOURCE_MAX_SLOTS)
		return EOVERFLOW;
#if SIZE_MAX <= UINT32_MAX
	if ((size_t)slot_count > SIZE_MAX - 7U ||
	    (size_t)slot_count > SIZE_MAX / sizeof(*slot_inflight))
		return EOVERFLOW;
#endif
	bytes = ((size_t)slot_count + 7U) / 8U;
	bitmap = kern_calloc(1, bytes);
	slot_inflight = kern_calloc(slot_count, sizeof(*slot_inflight));
	slot_pending_free = kern_calloc(slot_count,
	    sizeof(*slot_pending_free));
	if (bitmap == NULL || slot_inflight == NULL ||
	    slot_pending_free == NULL) {
		kern_free(slot_pending_free);
		kern_free(slot_inflight);
		kern_free(bitmap);
		return ENOMEM;
	}
	irq = spin_lock_irqsave(&swap_lock);
	source = &backend->source[source_id];
	if (!backend->enabled || backend->shutting_down ||
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
	return 0;
}

int
swap_source_publish(struct swap_backend *backend, unsigned source_id)
{
	struct swap_backend_source *source;
	unsigned long irq;

	if (backend == NULL || source_id >= SWAP_SOURCE_COUNT)
		return EINVAL;
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
	backend->slot_count += source->slot_count;
	backend->free_slots += source->slot_count;
	backend->source_count++;
	source->state = SWAP_SOURCE_STATE_ACTIVE;
	spin_unlock_irqrestore(&swap_lock, irq);
	return 0;
}

int
swap_source_cancel_prepare(struct swap_backend *backend, unsigned source_id)
{
	struct swap_backend_source *source;
	uint8_t *bitmap;
	uint32_t *slot_inflight;
	uint8_t *slot_pending_free;
	unsigned long irq;

	if (backend == NULL || source_id >= SWAP_SOURCE_COUNT)
		return EINVAL;
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
	bitmap = source->bitmap;
	slot_inflight = source->slot_inflight;
	slot_pending_free = source->slot_pending_free;
	/* Keep a tombstone until all manager-owned metadata is released. */
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
	irq = spin_lock_irqsave(&swap_lock);
	memset(source, 0, sizeof(*source));
	spin_unlock_irqrestore(&swap_lock, irq);
	return 0;
}

int swap_source_add(struct swap_backend *backend, unsigned source_id,
		    const struct swap_backend_ops *ops, void *data,
		    uint32_t page_size, uint32_t slot_count)
{
	int enabled_here;
	int error;

	error = swap_manager_enable_transition(backend, &enabled_here);
	if (error != 0)
		return error;
	error = swap_source_prepare(backend, source_id, ops, data, page_size,
	    slot_count);
	if (error != 0) {
		if (enabled_here)
			swap_manager_disable_empty(backend);
		return error;
	}
	error = swap_source_publish(backend, source_id);
	if (error != 0) {
		if (swap_source_cancel_prepare(backend, source_id) != 0)
			HAL_FATAL("swap source publish rollback failed");
		if (enabled_here)
			swap_manager_disable_empty(backend);
	}
	return error;
}

int swap_activate(struct swap_backend *backend,
		  const struct swap_backend_ops *ops, void *data,
		  uint32_t page_size, uint32_t slot_count)
{
	return swap_source_add(backend, 0, ops, data, page_size, slot_count);
}

int swap_alloc_slot(struct swap_backend *backend, uint32_t *slot)
{
	unsigned source_id;
	unsigned long irq;

	if (backend == NULL || slot == NULL)
		return EINVAL;
	irq = spin_lock_irqsave(&swap_lock);
	if (!backend->enabled || backend->shutting_down) {
		spin_unlock_irqrestore(&swap_lock, irq);
		return ENXIO;
	}
	for (source_id = 0; source_id < SWAP_SOURCE_COUNT; source_id++) {
		struct swap_backend_source *source =
		    &backend->source[source_id];
		uint32_t index;

		if (source->state != SWAP_SOURCE_STATE_ACTIVE ||
		    source->free_slots == 0)
			continue;
		for (index = 0; index < source->slot_count; index++) {
			uint8_t mask = (uint8_t)(1U << (index & 7U));

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
	return ENOSPC;
}

void swap_free_slot(struct swap_backend *backend, uint32_t slot)
{
	struct swap_backend_source *source;
	unsigned source_id;
	uint32_t local_slot;
	uint8_t mask;
	unsigned long irq;

	if (backend == NULL ||
	    swap_slot_decode(slot, &source_id, &local_slot) != 0)
		return;
	irq = spin_lock_irqsave(&swap_lock);
	source = &backend->source[source_id];
	if (!backend->enabled ||
	    (source->state != SWAP_SOURCE_STATE_ACTIVE &&
	     source->state != SWAP_SOURCE_STATE_DRAINING) ||
	    local_slot >= source->slot_count) {
		spin_unlock_irqrestore(&swap_lock, irq);
		return;
	}
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

static int swap_io(struct swap_backend *backend, uint32_t slot, void *page,
		   int write)
{
	struct swap_backend_source *source;
	unsigned source_id;
	uint32_t local_slot;
	int error;
	unsigned long irq;
	uint8_t mask;
	const struct swap_backend_ops *ops;
	void *data;

	if (backend == NULL || page == NULL)
		return EINVAL;
	if (swap_slot_decode(slot, &source_id, &local_slot) != 0)
		return EINVAL;
	irq = spin_lock_irqsave(&swap_lock);
	source = &backend->source[source_id];
	if (!backend->enabled || backend->shutting_down ||
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
	backend->inflight++;
	source->inflight++;
	source->slot_inflight[local_slot]++;
	ops = source->ops;
	data = source->data;
	spin_unlock_irqrestore(&swap_lock, irq);
	error = write ? ops->write_page(data, local_slot, page) :
		ops->read_page(data, local_slot, page);
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
	return error;
}

int swap_read_page(struct swap_backend *backend, uint32_t slot, void *page)
{
	return swap_io(backend, slot, page, 0);
}

int swap_write_page(struct swap_backend *backend, uint32_t slot,
		    const void *page)
{
	return swap_io(backend, slot, (void *)page, 1);
}

int swap_flush(struct swap_backend *backend)
{
	const struct swap_backend_ops *ops[SWAP_SOURCE_COUNT];
	void *data[SWAP_SOURCE_COUNT];
	uint8_t reserved[SWAP_SOURCE_COUNT];
	unsigned long irq;
	int first_error = 0;
	unsigned source_id;

	if (backend == NULL)
		return EINVAL;
	memset(reserved, 0, sizeof(reserved));
	irq = spin_lock_irqsave(&swap_lock);
	if (!backend->enabled || backend->shutting_down) {
		spin_unlock_irqrestore(&swap_lock, irq);
		return ENXIO;
	}
	for (source_id = 0; source_id < SWAP_SOURCE_COUNT; source_id++) {
		struct swap_backend_source *source =
		    &backend->source[source_id];

		if (source->state == SWAP_SOURCE_STATE_REMOVING) {
			spin_unlock_irqrestore(&swap_lock, irq);
			return EBUSY;
		}
	}
	for (source_id = 0; source_id < SWAP_SOURCE_COUNT; source_id++) {
		struct swap_backend_source *source =
		    &backend->source[source_id];

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
	for (source_id = 0; source_id < SWAP_SOURCE_COUNT; source_id++) {
		int error;

		if (!reserved[source_id])
			continue;
		error = ops[source_id]->flush != NULL ?
		    ops[source_id]->flush(data[source_id]) : 0;
		if (first_error == 0 && error != 0)
			first_error = error;
	}
	irq = spin_lock_irqsave(&swap_lock);
	for (source_id = 0; source_id < SWAP_SOURCE_COUNT; source_id++) {
		if (reserved[source_id]) {
			backend->source[source_id].inflight--;
			backend->inflight--;
		}
	}
	spin_unlock_irqrestore(&swap_lock, irq);
	return first_error;
}

int swap_source_begin_drain(struct swap_backend *backend, unsigned source_id)
{
	struct swap_backend_source *source;
	unsigned long irq;

	if (backend == NULL || source_id >= SWAP_SOURCE_COUNT)
		return EINVAL;
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
	return 0;
}

int swap_source_abort_drain(struct swap_backend *backend, unsigned source_id)
{
	struct swap_backend_source *source;
	unsigned long irq;

	if (backend == NULL || source_id >= SWAP_SOURCE_COUNT)
		return EINVAL;
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
	return 0;
}

int swap_source_get_stats(struct swap_backend *backend, unsigned source_id,
			  struct swap_source_stats *stats)
{
	struct swap_backend_source *source;
	unsigned long irq;

	if (backend == NULL || source_id >= SWAP_SOURCE_COUNT || stats == NULL)
		return EINVAL;
	irq = spin_lock_irqsave(&swap_lock);
	source = &backend->source[source_id];
	stats->source_id = source_id;
	stats->state = source->state;
	stats->total_slots = source->slot_count;
	stats->free_slots = source->free_slots;
	stats->allocated_slots = source->slot_count - source->free_slots;
	stats->inflight = source->inflight;
	spin_unlock_irqrestore(&swap_lock, irq);
	return 0;
}

int swap_source_remove(struct swap_backend *backend, unsigned source_id)
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

	if (backend == NULL || source_id >= SWAP_SOURCE_COUNT)
		return EINVAL;
	irq = spin_lock_irqsave(&swap_lock);
	source = &backend->source[source_id];
	if (!backend->enabled || source->state == SWAP_SOURCE_STATE_INACTIVE) {
		spin_unlock_irqrestore(&swap_lock, irq);
		return ENXIO;
	}
	if (backend->shutting_down ||
	    source->state != SWAP_SOURCE_STATE_DRAINING ||
	    source->inflight != 0 || source->free_slots != source->slot_count) {
		spin_unlock_irqrestore(&swap_lock, irq);
		return EBUSY;
	}
	/* Exclude I/O, shutdown, and ID reuse while flush is in progress. */
	source->state = SWAP_SOURCE_STATE_REMOVING;
	source->inflight = 1;
	backend->inflight++;
	ops = source->ops;
	data = source->data;
	spin_unlock_irqrestore(&swap_lock, irq);
	flush_error = ops->flush != NULL ? ops->flush(data) : 0;
	irq = spin_lock_irqsave(&swap_lock);
	backend->inflight--;
	source->inflight--;
	if (flush_error != 0) {
		source->state = SWAP_SOURCE_STATE_DRAINING;
		spin_unlock_irqrestore(&swap_lock, irq);
		return flush_error;
	}
	bitmap = source->bitmap;
	slot_inflight = source->slot_inflight;
	slot_pending_free = source->slot_pending_free;
	slot_count = source->slot_count;
	/*
	 * Keep a REMOVING tombstone until callbacks and frees finish.  This
	 * prevents a concurrent add from reusing the numeric ID while the old
	 * lifecycle is still observable, without freeing memory under a lock.
	 */
	source->ops = NULL;
	source->data = NULL;
	source->bitmap = NULL;
	source->slot_inflight = NULL;
	source->slot_pending_free = NULL;
	backend->slot_count -= slot_count;
	backend->free_slots -= slot_count;
	backend->source_count--;
	spin_unlock_irqrestore(&swap_lock, irq);
	if (ops->destroy != NULL)
		ops->destroy(data);
	kern_free(slot_pending_free);
	kern_free(slot_inflight);
	kern_free(bitmap);
	irq = spin_lock_irqsave(&swap_lock);
	memset(source, 0, sizeof(*source));
	spin_unlock_irqrestore(&swap_lock, irq);
	return 0;
}

int swap_shutdown(struct swap_backend *backend)
{
	struct swap_backend_source detached[SWAP_SOURCE_COUNT];
	unsigned long irq;
	int flush_error = 0;
	unsigned source_id;

	if (backend == NULL)
		return 0;
	memset(detached, 0, sizeof(detached));
	irq = spin_lock_irqsave(&swap_lock);
	if (!backend->enabled) {
		spin_unlock_irqrestore(&swap_lock, irq);
		return 0;
	}
	if (backend->shutting_down || backend->inflight != 0) {
		spin_unlock_irqrestore(&swap_lock, irq);
		return EBUSY;
	}
	for (source_id = 0; source_id < SWAP_SOURCE_COUNT; source_id++) {
		struct swap_backend_source *source =
		    &backend->source[source_id];

		if (source->state == SWAP_SOURCE_STATE_PREPARED ||
		    source->state == SWAP_SOURCE_STATE_REMOVING ||
		    source->inflight != 0 ||
		    (source->state != SWAP_SOURCE_STATE_INACTIVE &&
		     source->free_slots != source->slot_count)) {
			spin_unlock_irqrestore(&swap_lock, irq);
			return EBUSY;
		}
	}
	backend->shutting_down = 1;
	if (system_backend == backend)
		system_backend = NULL;
	for (source_id = 0; source_id < SWAP_SOURCE_COUNT; source_id++) {
		struct swap_backend_source *source =
		    &backend->source[source_id];

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
	/* Preserve numeric flush-before-destroy ordering from the boot backend. */
	for (source_id = 0; source_id < SWAP_SOURCE_COUNT; source_id++) {
		struct swap_backend_source *source = &detached[source_id];
		int error;

		if (source->state == SWAP_SOURCE_STATE_INACTIVE)
			continue;
		error = source->ops->flush != NULL ?
		    source->ops->flush(source->data) : 0;
		if (flush_error == 0 && error != 0)
			flush_error = error;
	}
	for (source_id = 0; source_id < SWAP_SOURCE_COUNT; source_id++) {
		struct swap_backend_source *source = &detached[source_id];

		if (source->state == SWAP_SOURCE_STATE_INACTIVE)
			continue;
		if (source->ops->destroy != NULL)
			source->ops->destroy(source->data);
		kern_free(source->slot_pending_free);
		kern_free(source->slot_inflight);
		kern_free(source->bitmap);
	}
	irq = spin_lock_irqsave(&swap_lock);
	memset(backend, 0, sizeof(*backend));
	spin_unlock_irqrestore(&swap_lock, irq);
	return flush_error;
}

int swap_set_system_backend(struct swap_backend *backend)
{
	unsigned long irq;
	int error = 0;

	if (backend == NULL)
		return EINVAL;
	irq = spin_lock_irqsave(&swap_lock);
	/*
	 * Publication is part of the backend lifecycle transaction.  Check the
	 * live state while holding the same lock used by swap_shutdown(); a
	 * pre-lock check can otherwise publish a backend after shutdown has
	 * detached and destroyed its data.
	 */
	if (!backend->enabled || backend->shutting_down)
		error = ENXIO;
	else if (system_backend != NULL && system_backend != backend)
		error = EBUSY;
	else
		system_backend = backend;
	spin_unlock_irqrestore(&swap_lock, irq);
	return error;
}

struct swap_backend *swap_system_backend(void)
{
	struct swap_backend *backend;
	unsigned long irq = spin_lock_irqsave(&swap_lock);
	backend = system_backend;
	spin_unlock_irqrestore(&swap_lock, irq);
	return backend;
}

int swap_get_stats(struct swap_backend *backend, uint32_t *total,
		   uint32_t *free_slots)
{
	unsigned long irq;

	if (backend == NULL || total == NULL || free_slots == NULL)
		return EINVAL;
	irq = spin_lock_irqsave(&swap_lock);
	if (!backend->enabled || backend->shutting_down) {
		spin_unlock_irqrestore(&swap_lock, irq);
		return ENXIO;
	}
	*total = backend->slot_count;
	*free_slots = backend->free_slots;
	spin_unlock_irqrestore(&swap_lock, irq);
	return 0;
}
