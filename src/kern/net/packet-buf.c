/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/net/packet-buf.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>

#define NET_BSS __attribute__((section(".net_bss")))

extern bool hal_irq_disable(void) __attribute__((weak));
extern void hal_irq_enable(void) __attribute__((weak));

struct packet_slot {
	struct packet_buf packet;
	uint8_t storage[PACKET_BUF_STORAGE_SIZE];
	uint8_t used;
};

static struct packet_slot packet_pool[PACKET_BUF_POOL_COUNT] NET_BSS;
static unsigned packet_used;
static atomic_uint_t packet_guard;

static bool
packet_lock(void)
{
	bool enabled = hal_irq_disable != NULL ? hal_irq_disable() : false;
	while (!atomic_try_acquire_zero(&packet_guard))
		__asm__ volatile("" ::: "memory");
	return enabled;
}

static void
packet_unlock(bool enabled)
{
	atomic_store_release(&packet_guard, 0);
	if (enabled && hal_irq_enable != NULL)
		hal_irq_enable();
}

void
packet_buf_pool_init(void)
{
	bool enabled = packet_lock();

	memset(packet_pool, 0, sizeof(packet_pool));
	packet_used = 0;
	packet_unlock(enabled);
}

struct packet_buf *
packet_buf_alloc(size_t headroom)
{
	struct packet_buf *packet = NULL;
	bool enabled;
	unsigned index;

	if (headroom > PACKET_BUF_STORAGE_SIZE)
		return NULL;
	enabled = packet_lock();
	for (index = 0; index < PACKET_BUF_POOL_COUNT; index++) {
		struct packet_slot *slot = &packet_pool[index];

		if (slot->used)
			continue;
		slot->used = 1;
		packet_used++;
		memset(&slot->packet, 0, sizeof(slot->packet));
		slot->packet.storage = slot->storage;
		slot->packet.data = slot->storage + headroom;
		slot->packet.capacity = sizeof(slot->storage);
		slot->packet.l2_offset = PACKET_OFFSET_NONE;
		slot->packet.l3_offset = PACKET_OFFSET_NONE;
		slot->packet.l4_offset = PACKET_OFFSET_NONE;
		refcount_init(&slot->packet.refcount, 1);
		packet = &slot->packet;
		break;
	}
	packet_unlock(enabled);
	return packet;
}

void
packet_buf_ref(struct packet_buf *packet)
{
	bool enabled;

	if (packet == NULL)
		return;
	enabled = packet_lock();
	refcount_get(&packet->refcount);
	packet_unlock(enabled);
}

void
packet_buf_free(struct packet_buf *packet)
{
	bool enabled;
	unsigned index;

	if (packet == NULL)
		return;
	enabled = packet_lock();
	if (!refcount_put(&packet->refcount)) {
		packet_unlock(enabled);
		return;
	}
	for (index = 0; index < PACKET_BUF_POOL_COUNT; index++) {
		struct packet_slot *slot = &packet_pool[index];

		if (&slot->packet != packet)
			continue;
		memset(&slot->packet, 0, sizeof(slot->packet));
		slot->used = 0;
		if (packet_used != 0)
			packet_used--;
		break;
	}
	packet_unlock(enabled);
}

size_t
packet_buf_headroom(const struct packet_buf *packet)
{
	if (packet == NULL || packet->data < packet->storage)
		return 0;
	return (size_t)(packet->data - packet->storage);
}

size_t
packet_buf_tailroom(const struct packet_buf *packet)
{
	size_t headroom;

	if (packet == NULL)
		return 0;
	headroom = packet_buf_headroom(packet);
	if (headroom > packet->capacity ||
	    packet->length > packet->capacity - headroom)
		return 0;
	return packet->capacity - headroom - packet->length;
}

void *
packet_buf_push(struct packet_buf *packet, size_t length)
{
	if (packet == NULL || length > packet_buf_headroom(packet))
		return NULL;
	packet->data -= length;
	packet->length += length;
	return packet->data;
}

void *
packet_buf_pull(struct packet_buf *packet, size_t length)
{
	void *data;

	if (packet == NULL || length > packet->length)
		return NULL;
	packet->data += length;
	packet->length -= length;
	data = packet->data;
	return data;
}

void *
packet_buf_append(struct packet_buf *packet, size_t length)
{
	void *tail;

	if (packet == NULL || length > packet_buf_tailroom(packet))
		return NULL;
	tail = packet->data + packet->length;
	packet->length += length;
	return tail;
}

int
packet_buf_trim(struct packet_buf *packet, size_t length)
{
	if (packet == NULL || length > packet->length)
		return EINVAL;
	packet->length = length;
	return 0;
}

struct packet_buf *
packet_buf_copy(const struct packet_buf *source)
{
	struct packet_buf *copy;
	size_t headroom;
	void *data;

	if (source == NULL)
		return NULL;
	headroom = packet_buf_headroom(source);
	copy = packet_buf_alloc(headroom);
	if (copy == NULL)
		return NULL;
	data = packet_buf_append(copy, source->length);
	if (data == NULL) {
		packet_buf_free(copy);
		return NULL;
	}
	memcpy(data, source->data, source->length);
	copy->l2_offset = source->l2_offset;
	copy->l3_offset = source->l3_offset;
	copy->l4_offset = source->l4_offset;
	copy->l3_length = source->l3_length;
	copy->protocol = source->protocol;
	copy->flags = source->flags;
	copy->device = source->device;
	copy->source_length = source->source_length;
	memcpy(copy->source_address, source->source_address,
	    sizeof(copy->source_address));
	return copy;
}

struct packet_buf *
packet_buf_copy_region(const struct packet_buf *source, size_t offset,
		       size_t length)
{
	struct packet_buf *copy;
	void *data;

	if (source == NULL || offset > source->capacity ||
	    length > source->capacity - offset)
		return NULL;
	copy = packet_buf_alloc(0);
	if (copy == NULL)
		return NULL;
	data = packet_buf_append(copy, length);
	if (data == NULL) {
		packet_buf_free(copy);
		return NULL;
	}
	memcpy(data, source->storage + offset, length);
	copy->protocol = source->protocol;
	copy->flags = source->flags;
	copy->device = source->device;
	copy->source_length = source->source_length;
	memcpy(copy->source_address, source->source_address,
	    sizeof(copy->source_address));
	return copy;
}

unsigned
packet_buf_in_use(void)
{
	bool enabled = packet_lock();
	unsigned result = packet_used;
	packet_unlock(enabled);
	return result;
}
