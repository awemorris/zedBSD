/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/swap.h"
#include "kern/kmem.h"

#include <errno.h>
#include <hal/hal.h>
#include <string.h>

extern bool hal_irq_disable(void) __attribute__((weak));
extern void hal_irq_enable(void) __attribute__((weak));

static struct swap_backend *system_backend;

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

static bool lock(void)
{
	return hal_irq_disable != NULL ? hal_irq_disable() : false;
}

static void unlock(bool enabled)
{
	if (enabled && hal_irq_enable != NULL)
		hal_irq_enable();
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

	if (backend == NULL || ops == NULL || ops->read_page == NULL ||
	    ops->write_page == NULL || page_size != SWAP_PAGE_SIZE ||
	    slot_count == 0 || backend->enabled)
		return EINVAL;
	bytes = ((size_t)slot_count + 7U) / 8U;
	backend->bitmap = kern_calloc(1, bytes);
	if (backend->bitmap == NULL)
		return ENOMEM;
	backend->ops = ops;
	backend->data = data;
	backend->page_size = page_size;
	backend->slot_count = slot_count;
	backend->free_slots = slot_count;
	backend->enabled = 1;
	return 0;
}

int swap_alloc_slot(struct swap_backend *backend, uint32_t *slot)
{
	uint32_t index;
	bool enabled;

	if (backend == NULL || slot == NULL || !backend->enabled ||
	    backend->shutting_down)
		return ENXIO;
	enabled = lock();
	for (index = 0; index < backend->slot_count; index++) {
		uint8_t mask = (uint8_t)(1U << (index & 7U));
		if (!(backend->bitmap[index >> 3] & mask)) {
			backend->bitmap[index >> 3] |= mask;
			backend->free_slots--;
			*slot = index;
			unlock(enabled);
			return 0;
		}
	}
	unlock(enabled);
	return ENOSPC;
}

void swap_free_slot(struct swap_backend *backend, uint32_t slot)
{
	uint8_t mask;
	bool enabled;

	if (backend == NULL || !backend->enabled || slot >= backend->slot_count)
		return;
	enabled = lock();
	mask = (uint8_t)(1U << (slot & 7U));
	if (backend->bitmap[slot >> 3] & mask) {
		backend->bitmap[slot >> 3] &= (uint8_t)~mask;
		backend->free_slots++;
	}
	unlock(enabled);
}

static int swap_io(struct swap_backend *backend, uint32_t slot, void *page,
		   int write)
{
	int error;
	bool enabled;
	uint8_t mask;

	if (backend == NULL || !backend->enabled || backend->shutting_down ||
	    page == NULL || slot >= backend->slot_count)
		return ENXIO;
	mask = (uint8_t)(1U << (slot & 7U));
	if (!(backend->bitmap[slot >> 3] & mask))
		return EINVAL;
	enabled = lock();
	backend->inflight++;
	unlock(enabled);
	error = write ? backend->ops->write_page(backend->data, slot, page) :
		backend->ops->read_page(backend->data, slot, page);
	enabled = lock();
	backend->inflight--;
	unlock(enabled);
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
	if (backend == NULL || !backend->enabled)
		return ENXIO;
	return backend->ops->flush != NULL ?
		backend->ops->flush(backend->data) : 0;
}

int swap_shutdown(struct swap_backend *backend)
{
	if (backend == NULL || !backend->enabled)
		return 0;
	backend->shutting_down = 1;
	if (backend->inflight != 0)
		return EBUSY;
	(void)swap_flush(backend);
	if (backend->ops->destroy != NULL)
		backend->ops->destroy(backend->data);
	if (system_backend == backend)
		system_backend = NULL;
	kern_free(backend->bitmap);
	swap_init(backend);
	return 0;
}

void swap_set_system_backend(struct swap_backend *backend)
{
	system_backend = backend != NULL && backend->enabled ? backend : NULL;
}

struct swap_backend *swap_system_backend(void)
{
	return system_backend;
}
