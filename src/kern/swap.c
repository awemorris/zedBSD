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

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
		((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t get16(const uint8_t *p)
{
	return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
}

static uint64_t get64(const uint8_t *p)
{
	return (uint64_t)get32(p) | ((uint64_t)get32(p + 4U) << 32);
}

uint32_t swap_header_checksum(const uint8_t *header)
{
	uint32_t hash = 2166136261U;
	unsigned checksum_offset;
	unsigned i;

	if (header == NULL)
		return 0;
	checksum_offset = memcmp(header, "ZEDSWAP2", 8U) == 0 ? 60U : 28U;
	for (i = 0; i < ZEDBSD_SWAP_HEADER_SIZE; i++) {
		uint8_t byte = i >= checksum_offset &&
		    i < checksum_offset + 4U ? 0 : header[i];
		hash = (hash ^ byte) * 16777619U;
	}
	return hash;
}

int
swap_header_parse(const uint8_t *header, uint64_t backing_bytes,
		  struct swap_header_info *result)
{
	static const uint8_t magic_v1[8] = {
		'Z', 'E', 'D', 'S', 'W', 'A', 'P', '1'
	};
	static const uint8_t magic_v2[8] = {
		'Z', 'E', 'D', 'S', 'W', 'A', 'P', '2'
	};
	struct swap_header_info parsed;
	unsigned i;

	if (header == NULL || backing_bytes < SWAP_PAGE_SIZE * 2ULL ||
	    backing_bytes % SWAP_PAGE_SIZE != 0)
		return EINVAL;
	memset(&parsed, 0, sizeof(parsed));
	if (memcmp(header, magic_v1, sizeof(magic_v1)) == 0) {
		uint64_t slots;

		if ((backing_bytes != ZEDBSD_SWAP_FILE_MIN_BYTES &&
		     backing_bytes != ZEDBSD_SWAP_FILE_MAX_BYTES) ||
		    get32(header + 8U) != 1U ||
		    get32(header + 12U) != ZEDBSD_SWAP_HEADER_SIZE ||
		    get32(header + 16U) != SWAP_PAGE_SIZE ||
		    get32(header + 20U) != backing_bytes ||
		    get32(header + 28U) != swap_header_checksum(header))
			return EINVAL;
		slots = backing_bytes / SWAP_PAGE_SIZE - 1U;
		if (get32(header + 24U) != slots)
			return EINVAL;
		for (i = 32U; i < ZEDBSD_SWAP_HEADER_SIZE; i++)
			if (header[i] != 0U)
				return EINVAL;
		parsed.version = 1U;
		parsed.backing_bytes = backing_bytes;
		parsed.slot_count = slots;
	} else if (memcmp(header, magic_v2, sizeof(magic_v2)) == 0) {
		uint64_t slots = backing_bytes / SWAP_PAGE_SIZE - 1U;
		int terminated = 0;

		if (get16(header + 8U) != 2U ||
		    get16(header + 10U) != ZEDBSD_SWAP_HEADER_SIZE ||
		    get32(header + 12U) != SWAP_PAGE_SIZE ||
		    get64(header + 16U) != backing_bytes ||
		    get64(header + 24U) != slots ||
		    get32(header + 60U) != swap_header_checksum(header))
			return EINVAL;
		for (i = 0; i < ZEDBSD_SWAP_V2_UUID_SIZE; i++)
			parsed.uuid[i] = header[32U + i];
		for (i = 0; i < ZEDBSD_SWAP_V2_LABEL_SIZE; i++) {
			uint8_t byte = header[40U + i];

			if (terminated && byte != 0U)
				return EINVAL;
			if (!terminated && byte == 0U)
				terminated = 1;
			else if (!terminated && (byte < 0x20U || byte > 0x7eU))
				return EINVAL;
			parsed.label[i] = (char)byte;
		}
		if (!terminated)
			return EINVAL;
		parsed.version = 2U;
		parsed.backing_bytes = backing_bytes;
		parsed.slot_count = slots;
	} else {
		return EINVAL;
	}
	if (result != NULL)
		*result = parsed;
	return 0;
}

int
swap_header_validate(const uint8_t *header, uint64_t backing_bytes)
{
	return swap_header_parse(header, backing_bytes, NULL);
}

int
swap_header_uuid_format(const struct swap_header_info *header, char *output,
			size_t capacity)
{
	static const char digits[] = "0123456789ABCDEF";
	unsigned i;
	int present = 0;

	if (header == NULL || output == NULL || capacity < 17U)
		return EINVAL;
	for (i = 0; i < ZEDBSD_SWAP_V2_UUID_SIZE; i++)
		present |= header->uuid[i] != 0U;
	if (!present) {
		output[0] = '\0';
		return ENOENT;
	}
	for (i = 0; i < ZEDBSD_SWAP_V2_UUID_SIZE; i++) {
		output[i * 2U] = digits[header->uuid[i] >> 4];
		output[i * 2U + 1U] = digits[header->uuid[i] & 15U];
	}
	output[16] = '\0';
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
	int flush_error = 0;

	if (backend == NULL)
		return 0;
	irq = spin_lock_irqsave(&swap_lock);
	if (!backend->enabled) {
		spin_unlock_irqrestore(&swap_lock, irq);
		return 0;
	}
	/*
	 * A used slot is owned by a VM page and may be its only valid copy.
	 * Refuse teardown until every owner has returned its slot.  Keep the
	 * backend live on EBUSY so those pages can still be read and released.
	 */
	if (backend->shutting_down || backend->inflight != 0 ||
	    backend->free_slots != backend->slot_count) {
		spin_unlock_irqrestore(&swap_lock, irq);
		return EBUSY;
	}
	backend->shutting_down = 1;
	/* Reserve the sole lifecycle callback while the spinlock is dropped. */
	backend->inflight = 1;
	ops = backend->ops;
	data = backend->data;
	spin_unlock_irqrestore(&swap_lock, irq);
	if (ops->flush != NULL)
		flush_error = ops->flush(data);
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
	if (!backend->enabled || backend->shutting_down ||
	    backend->bitmap == NULL || backend->ops == NULL)
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
	if (!backend->enabled) {
		spin_unlock_irqrestore(&swap_lock, irq);
		return ENXIO;
	}
	*total = backend->slot_count;
	*free_slots = backend->free_slots;
	spin_unlock_irqrestore(&swap_lock, irq);
	return 0;
}
