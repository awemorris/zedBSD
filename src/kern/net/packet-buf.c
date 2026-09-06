/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Packet buffers.
 *
 * Packets come from a fixed pool of slots with inline storage, guarded by
 * a spin lock that also disables interrupts because drivers free packets
 * from interrupt context.  A packet has headroom before its data and
 * tailroom after it; the layer offsets index into the storage so that
 * copies keep them.  The HAL and device hooks are weak so that host tests
 * can use the pool without them.
 */

#include "kern/net/packet-buf.h"
#include "kern/net/net-device.h"
#include "kern/test-fault.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>

#define NET_BSS __attribute__((section(".net_bss")))

extern bool hal_irq_disable(void) __attribute__((weak));
extern void hal_irq_enable(void) __attribute__((weak));
extern void net_device_ref(struct net_device *) __attribute__((weak));
extern void net_device_release(struct net_device *) __attribute__((weak));

struct packet_slot {
	struct packet_buf packet;
	uint8_t storage[PACKET_BUF_STORAGE_SIZE];
	uint8_t used;
};

static struct packet_slot packet_pool[PACKET_BUF_POOL_COUNT] NET_BSS;
static unsigned packet_used;
static atomic_uint_t packet_guard;

static bool packet_lock(void);
static void packet_unlock(bool enabled);

/*
 * Empties the packet pool.
 */
void
packet_buf_pool_init(
	void)
{
	bool enabled;

	enabled = packet_lock();
	memset(packet_pool, 0, sizeof(packet_pool));
	packet_used = 0;
	packet_unlock(enabled);
}

/*
 * Allocates an empty packet with the requested headroom.
 *
 * The test fault point lets a test make the pool look exhausted.
 */
struct packet_buf *
packet_buf_alloc(
	size_t headroom)
{
	struct packet_buf *packet;
	struct packet_slot *slot;
	struct kern_test_fault_result fault;
	bool enabled;
	unsigned index;

	packet = NULL;

	/* Rejects headroom beyond the slot storage. */
	if (headroom > PACKET_BUF_STORAGE_SIZE)
		return NULL;

	/* Fails on demand under test. */
	if (KERN_TEST_FAULT(KERN_TEST_FAULT_NET_PACKET_ALLOC, UINT32_MAX,
	    UINT32_MAX, &fault))
		return NULL;

	/* Takes the first free slot and resets its packet. */
	enabled = packet_lock();
	for (index = 0; index < PACKET_BUF_POOL_COUNT; index++) {
		slot = &packet_pool[index];
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

	/* Reports the packet, or none when the pool is exhausted. */
	return packet;
}

/*
 * Takes a reference on a packet.
 */
void
packet_buf_ref(
	struct packet_buf *packet)
{
	bool enabled;

	/* Ignores a missing packet. */
	if (packet == NULL)
		return;

	/* Counts the reference under the pool lock. */
	enabled = packet_lock();
	refcount_get(&packet->refcount);
	packet_unlock(enabled);
}

/*
 * Drops a reference on a packet and returns the last one to the pool.
 *
 * The device reference and the control block are released outside the
 * pool lock.
 */
void
packet_buf_free(
	struct packet_buf *packet)
{
	struct net_device *device;
	struct packet_slot *slot;
	void *control;
	void (*control_release)(void *);
	bool enabled;
	unsigned index;

	device = NULL;
	control = NULL;
	control_release = NULL;

	/* Ignores a missing packet. */
	if (packet == NULL)
		return;

	/* Only the last reference frees the slot. */
	enabled = packet_lock();
	if (!refcount_put(&packet->refcount)) {
		packet_unlock(enabled);
		return;
	}

	/* Detaches the device and control block, then clears the slot. */
	for (index = 0; index < PACKET_BUF_POOL_COUNT; index++) {
		slot = &packet_pool[index];
		if (&slot->packet != packet)
			continue;
		device = slot->packet.device;
		control = slot->packet.control;
		control_release = slot->packet.control_release;
		slot->packet.device = NULL;
		slot->packet.control = NULL;
		slot->packet.control_release = NULL;
		memset(&slot->packet, 0, sizeof(slot->packet));
		slot->used = 0;
		if (packet_used != 0)
			packet_used--;
		break;
	}
	packet_unlock(enabled);

	/* Releases what the packet held. */
	if (device != NULL && net_device_release != NULL)
		net_device_release(device);
	if (control != NULL && control_release != NULL)
		control_release(control);
}

/*
 * Reports the bytes available before the packet data.
 */
size_t
packet_buf_headroom(
	const struct packet_buf *packet)
{
	/* A missing or inconsistent packet has no headroom. */
	if (packet == NULL || packet->data < packet->storage)
		return 0;

	/* Reports the distance from the storage to the data. */
	return (size_t)(packet->data - packet->storage);
}

/*
 * Reports the bytes available after the packet data.
 */
size_t
packet_buf_tailroom(
	const struct packet_buf *packet)
{
	size_t headroom;

	/* A missing or inconsistent packet has no tailroom. */
	if (packet == NULL)
		return 0;
	headroom = packet_buf_headroom(packet);
	if (headroom > packet->capacity ||
	    packet->length > packet->capacity - headroom)
		return 0;

	/* Reports the capacity left after the headroom and the data. */
	return packet->capacity - headroom - packet->length;
}

/*
 * Extends the packet data into the headroom.
 */
void *
packet_buf_push(
	struct packet_buf *packet,
	size_t length)
{
	/* Rejects a missing packet or more than the headroom. */
	if (packet == NULL || length > packet_buf_headroom(packet))
		return NULL;

	/* Moves the data start back. */
	packet->data -= length;
	packet->length += length;

	/* Reports the new data start. */
	return packet->data;
}

/*
 * Removes bytes from the front of the packet data.
 */
void *
packet_buf_pull(
	struct packet_buf *packet,
	size_t length)
{
	void *data;

	/* Rejects a missing packet or more than the data. */
	if (packet == NULL || length > packet->length)
		return NULL;

	/* Moves the data start forward. */
	packet->data += length;
	packet->length -= length;
	data = packet->data;

	/* Reports the new data start. */
	return data;
}

/*
 * Extends the packet data into the tailroom.
 */
void *
packet_buf_append(
	struct packet_buf *packet,
	size_t length)
{
	void *tail;

	/* Rejects a missing packet or more than the tailroom. */
	if (packet == NULL || length > packet_buf_tailroom(packet))
		return NULL;

	/* Extends the data. */
	tail = packet->data + packet->length;
	packet->length += length;

	/* Reports the start of the appended bytes. */
	return tail;
}

/*
 * Shortens the packet data to a length.
 */
int
packet_buf_trim(
	struct packet_buf *packet,
	size_t length)
{
	/* Rejects a missing packet or a length beyond the data. */
	if (packet == NULL || length > packet->length)
		return EINVAL;

	packet->length = length;

	/* Reports the trimmed packet. */
	return 0;
}

/*
 * Copies a packet with its headroom, offsets, device, and source address.
 */
struct packet_buf *
packet_buf_copy(
	const struct packet_buf *source)
{
	struct packet_buf *copy;
	size_t headroom;
	void *data;

	/* There is nothing to copy without a source. */
	if (source == NULL)
		return NULL;

	/* Allocates a packet with the same headroom and copies the data. */
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

	/* Copies the layer offsets, protocol, flags, device, and source. */
	copy->l2_offset = source->l2_offset;
	copy->l3_offset = source->l3_offset;
	copy->l4_offset = source->l4_offset;
	copy->l3_length = source->l3_length;
	copy->protocol = source->protocol;
	copy->flags = source->flags;
	copy->device = source->device;
	if (copy->device != NULL && net_device_ref != NULL)
		net_device_ref(copy->device);
	copy->source_length = source->source_length;
	memcpy(copy->source_address, source->source_address,
	    sizeof(copy->source_address));

	/* Reports the copy. */
	return copy;
}

/*
 * Copies a region of a packet's storage into a new packet without headroom.
 */
struct packet_buf *
packet_buf_copy_region(
	const struct packet_buf *source,
	size_t offset,
	size_t length)
{
	struct packet_buf *copy;
	void *data;

	/* Rejects a missing source or a region outside its storage. */
	if (source == NULL ||
	    offset > source->capacity ||
	    length > source->capacity - offset)
		return NULL;

	/* Allocates a packet and copies the region as its data. */
	copy = packet_buf_alloc(0);
	if (copy == NULL)
		return NULL;
	data = packet_buf_append(copy, length);
	if (data == NULL) {
		packet_buf_free(copy);
		return NULL;
	}
	memcpy(data, source->storage + offset, length);

	/* Copies the protocol, flags, device, and source. */
	copy->protocol = source->protocol;
	copy->flags = source->flags;
	copy->device = source->device;
	if (copy->device != NULL && net_device_ref != NULL)
		net_device_ref(copy->device);
	copy->source_length = source->source_length;
	memcpy(copy->source_address, source->source_address,
	    sizeof(copy->source_address));

	/* Reports the copy. */
	return copy;
}

/*
 * Reports the number of packets in use.
 */
unsigned
packet_buf_in_use(
	void)
{
	bool enabled;
	unsigned result;

	/* Samples the count under the pool lock. */
	enabled = packet_lock();
	result = packet_used;
	packet_unlock(enabled);

	/* Reports the sampled count. */
	return result;
}

/* Disables interrupts, when the HAL is present, and takes the pool lock. */
static bool
packet_lock(
	void)
{
	bool enabled;

	/* Without the HAL there are no interrupts to disable. */
	if (hal_irq_disable != NULL)
		enabled = hal_irq_disable();
	else
		enabled = false;

	/* Spins for the lock. */
	while (!atomic_try_acquire_zero(&packet_guard))
		__asm__ volatile("" ::: "memory");

	/* Reports whether interrupts were enabled. */
	return enabled;
}

/* Releases the pool lock and restores the interrupt state. */
static void
packet_unlock(
	bool enabled)
{
	atomic_store_release(&packet_guard, 0);

	/* Re-enables interrupts only when they were enabled before. */
	if (enabled && hal_irq_enable != NULL)
		hal_irq_enable();
}
