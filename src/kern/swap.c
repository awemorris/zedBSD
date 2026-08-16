/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/swap.h"
#include "kern/kmem.h"
#include "kern/lock.h"

#include <errno.h>
#include <hal/hal.h>
#include <string.h>

static struct swap_backend *system_backend;
static struct spinlock swap_lock = {
	{ 0 }, LOCK_RANK_SWAP, "swap backend", 0, 0
};

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
		((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint32_t swap_header_checksum(const uint8_t *header)
{
	uint32_t hash = 2166136261U;
	unsigned i;
	for (i = 0; i < ZEDBSD_SWAP_HEADER_SIZE; i++) {
		uint8_t byte = i >= 28U && i < 32U ? 0 : header[i];
		hash = (hash ^ byte) * 16777619U;
	}
	return hash;
}

int swap_header_validate(const uint8_t *header, uint32_t file_bytes)
{
	static const uint8_t magic[8] = {
		'Z', 'E', 'D', 'S', 'W', 'A', 'P', '1'
	};
	unsigned i;
	uint32_t slots;
	if (header == NULL ||
	    (file_bytes != ZEDBSD_SWAP_FILE_MIN_BYTES &&
	     file_bytes != ZEDBSD_SWAP_FILE_MAX_BYTES) ||
	    file_bytes % SWAP_PAGE_SIZE != 0 ||
	    memcmp(header, magic, sizeof(magic)) != 0 || get32(header + 8) != 1 ||
	    get32(header + 12) != ZEDBSD_SWAP_HEADER_SIZE ||
	    get32(header + 16) != SWAP_PAGE_SIZE ||
	    get32(header + 20) != file_bytes ||
	    get32(header + 28) != swap_header_checksum(header))
		return EINVAL;
	slots = file_bytes / SWAP_PAGE_SIZE - 1U;
	if (get32(header + 24) != slots)
		return EINVAL;
	for (i = 32; i < ZEDBSD_SWAP_HEADER_SIZE; i++)
		if (header[i] != 0)
			return EINVAL;
	return 0;
}

void swap_init(struct swap_backend *backend)
{
	if (backend != NULL)
		memset(backend, 0, sizeof(*backend));
}

int swap_activate(struct swap_backend *backend,
		  const struct swap_backend_ops *ops, void *data,
		  uint32_t page_size, uint32_t slot_count)
{
	size_t bytes;
	uint8_t *bitmap;
	uint32_t *slot_inflight;
	uint8_t *slot_pending_free;
	unsigned long irq;

	if (backend == NULL || ops == NULL || ops->read_page == NULL ||
	    ops->write_page == NULL || page_size != SWAP_PAGE_SIZE ||
	    slot_count == 0)
		return EINVAL;
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
	if (backend->enabled || backend->shutting_down ||
	    backend->inflight != 0 || backend->bitmap != NULL) {
		spin_unlock_irqrestore(&swap_lock, irq);
		kern_free(slot_pending_free);
		kern_free(slot_inflight);
		kern_free(bitmap);
		return EBUSY;
	}
	backend->bitmap = bitmap;
	backend->slot_inflight = slot_inflight;
	backend->slot_pending_free = slot_pending_free;
	backend->ops = ops;
	backend->data = data;
	backend->page_size = page_size;
	backend->slot_count = slot_count;
	backend->free_slots = slot_count;
	backend->enabled = 1;
	spin_unlock_irqrestore(&swap_lock, irq);
	return 0;
}

int swap_alloc_slot(struct swap_backend *backend, uint32_t *slot)
{
	uint32_t index;
	unsigned long irq;

	if (backend == NULL || slot == NULL)
		return EINVAL;
	irq = spin_lock_irqsave(&swap_lock);
	if (!backend->enabled || backend->shutting_down) {
		spin_unlock_irqrestore(&swap_lock, irq);
		return ENXIO;
	}
	for (index = 0; index < backend->slot_count; index++) {
		uint8_t mask = (uint8_t)(1U << (index & 7U));
		if (!(backend->bitmap[index >> 3] & mask)) {
			backend->bitmap[index >> 3] |= mask;
			backend->slot_pending_free[index] = 0;
			backend->free_slots--;
			*slot = index;
			spin_unlock_irqrestore(&swap_lock, irq);
			return 0;
		}
	}
	spin_unlock_irqrestore(&swap_lock, irq);
	return ENOSPC;
}

void swap_free_slot(struct swap_backend *backend, uint32_t slot)
{
	uint8_t mask;
	unsigned long irq;

	if (backend == NULL)
		return;
	irq = spin_lock_irqsave(&swap_lock);
	if (!backend->enabled || slot >= backend->slot_count) {
		spin_unlock_irqrestore(&swap_lock, irq);
		return;
	}
	mask = (uint8_t)(1U << (slot & 7U));
	if (backend->bitmap[slot >> 3] & mask) {
		if (backend->slot_inflight[slot] != 0) {
			backend->slot_pending_free[slot] = 1;
		} else {
			backend->bitmap[slot >> 3] &= (uint8_t)~mask;
			backend->free_slots++;
		}
	}
	spin_unlock_irqrestore(&swap_lock, irq);
}

static int swap_io(struct swap_backend *backend, uint32_t slot, void *page,
		   int write)
{
	int error;
	unsigned long irq;
	uint8_t mask;
	const struct swap_backend_ops *ops;
	void *data;

	if (backend == NULL || page == NULL)
		return EINVAL;
	irq = spin_lock_irqsave(&swap_lock);
	if (!backend->enabled || backend->shutting_down ||
	    slot >= backend->slot_count) {
		spin_unlock_irqrestore(&swap_lock, irq);
		return ENXIO;
	}
	mask = (uint8_t)(1U << (slot & 7U));
	if (!(backend->bitmap[slot >> 3] & mask) ||
	    backend->slot_pending_free[slot]) {
		spin_unlock_irqrestore(&swap_lock, irq);
		return EINVAL;
	}
	backend->inflight++;
	backend->slot_inflight[slot]++;
	ops = backend->ops;
	data = backend->data;
	spin_unlock_irqrestore(&swap_lock, irq);
	error = write ? ops->write_page(data, slot, page) :
		ops->read_page(data, slot, page);
	irq = spin_lock_irqsave(&swap_lock);
	backend->slot_inflight[slot]--;
	if (backend->slot_inflight[slot] == 0 &&
	    backend->slot_pending_free[slot]) {
		backend->slot_pending_free[slot] = 0;
		backend->bitmap[slot >> 3] &= (uint8_t)~mask;
		backend->free_slots++;
	}
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
	const struct swap_backend_ops *ops;
	void *data;
	unsigned long irq;
	int error;

	if (backend == NULL)
		return EINVAL;
	irq = spin_lock_irqsave(&swap_lock);
	if (!backend->enabled || backend->shutting_down) {
		spin_unlock_irqrestore(&swap_lock, irq);
		return ENXIO;
	}
	ops = backend->ops;
	data = backend->data;
	backend->inflight++;
	spin_unlock_irqrestore(&swap_lock, irq);
	error = ops->flush != NULL ? ops->flush(data) : 0;
	irq = spin_lock_irqsave(&swap_lock);
	backend->inflight--;
	spin_unlock_irqrestore(&swap_lock, irq);
	return error;
}

int swap_shutdown(struct swap_backend *backend)
{
	const struct swap_backend_ops *ops;
	void *data;
	uint8_t *bitmap;
	uint32_t *slot_inflight;
	uint8_t *slot_pending_free;
	unsigned long irq;

	if (backend == NULL)
		return 0;
	irq = spin_lock_irqsave(&swap_lock);
	if (!backend->enabled) {
		spin_unlock_irqrestore(&swap_lock, irq);
		return 0;
	}
	backend->shutting_down = 1;
	if (backend->inflight != 0) {
		spin_unlock_irqrestore(&swap_lock, irq);
		return EBUSY;
	}
	/* Reserve the sole lifecycle callback while the spinlock is dropped. */
	backend->inflight = 1;
	ops = backend->ops;
	data = backend->data;
	spin_unlock_irqrestore(&swap_lock, irq);
	if (ops->flush != NULL)
		(void)ops->flush(data);
	irq = spin_lock_irqsave(&swap_lock);
	backend->inflight = 0;
	if (system_backend == backend)
		system_backend = NULL;
	bitmap = backend->bitmap;
	slot_inflight = backend->slot_inflight;
	slot_pending_free = backend->slot_pending_free;
	memset(backend, 0, sizeof(*backend));
	spin_unlock_irqrestore(&swap_lock, irq);
	if (ops->destroy != NULL)
		ops->destroy(data);
	kern_free(slot_pending_free);
	kern_free(slot_inflight);
	kern_free(bitmap);
	return 0;
}

void swap_set_system_backend(struct swap_backend *backend)
{
	unsigned long irq = spin_lock_irqsave(&swap_lock);
	system_backend = backend != NULL && backend->enabled ? backend : NULL;
	spin_unlock_irqrestore(&swap_lock, irq);
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
	if (!backend->enabled) {
		spin_unlock_irqrestore(&swap_lock, irq);
		return ENXIO;
	}
	*total = backend->slot_count;
	*free_slots = backend->free_slots;
	spin_unlock_irqrestore(&swap_lock, irq);
	return 0;
}
