/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The VCHIQ shared-memory layout, message framing and page lists (see
 * rpi4-vchiq-wire.h).
 */

#include "drivers/platform/rpi4/rpi4-vchiq-wire.h"
#include <kern/device-io.h>
#include <kern/kcrt.h>
#include <uapi/errno.h>

/* The slot zero occupies one whole slot, before the data slots. */
#define VCHIQ_ZERO_SLOTS		1U

/* The low bits of a stream position: the offset inside its slot. */
#define VCHIQ_SLOT_MASK			(DRV_RPI4_VCHIQ_SLOT_SIZE - 1U)

/* The low bits of a slot count: the index into a slot queue. */
#define VCHIQ_QUEUE_MASK		(DRV_RPI4_VCHIQ_MAX_SLOTS_PER_SIDE - 1U)

/* The bits of a page list address word that count the pages after the first. */
#define VCHIQ_PAGELIST_RUN_MASK		(DRV_RPI4_VCHIQ_PAGELIST_RUN_PAGES - 1U)

/* The largest page number a 32-bit long bulk address word holds (24 bits). */
#define VCHIQ_PAGELIST_PAGE_LIMIT	0x00ffffffULL

/* The layout is fixed by the protocol; a compiler that pads it would break the firmware. */
_Static_assert(sizeof(struct drv_rpi4_vchiq_event) == 12U, "VCHIQ event size");
_Static_assert(sizeof(struct drv_rpi4_vchiq_shared) == 372U, "VCHIQ shared state size");
_Static_assert(sizeof(struct drv_rpi4_vchiq_slot_zero) == 1288U, "VCHIQ slot zero size");
_Static_assert(sizeof(struct drv_rpi4_vchiq_slot_zero) <= DRV_RPI4_VCHIQ_SLOT_SIZE, "VCHIQ slot zero fits one slot");

static uint8_t *slot_data(struct drv_rpi4_vchiq_ring *ring, uint32_t index);
static bool release_remote_slot(struct drv_rpi4_vchiq_ring *ring);

/*
 * Makes a message id from its type and its source and destination ports.
 */
uint32_t
drv_rpi4_vchiq_msgid(
	uint32_t type,
	uint32_t source,
	uint32_t destination)
{
	uint32_t msgid;

	/* The type takes the top byte, the source the next twelve bits, the destination the low twelve. */
	msgid = type << 24;
	msgid |= (source & DRV_RPI4_VCHIQ_PORT_MASK) << 12;
	msgid |= destination & DRV_RPI4_VCHIQ_PORT_MASK;

	/* Succeeded: the id names the type and both ports. */
	return msgid;
}

/*
 * Reads the type of a message id.
 */
uint32_t
drv_rpi4_vchiq_msgid_type(
	uint32_t msgid)
{
	/* The type is the top byte. */
	return msgid >> 24;
}

/*
 * Reads the source port of a message id.
 */
uint32_t
drv_rpi4_vchiq_msgid_source(
	uint32_t msgid)
{
	/* The source port is the twelve bits above the destination. */
	return (msgid >> 12) & DRV_RPI4_VCHIQ_PORT_MASK;
}

/*
 * Reads the destination port of a message id.
 */
uint32_t
drv_rpi4_vchiq_msgid_destination(
	uint32_t msgid)
{
	/* The destination port is the low twelve bits. */
	return msgid & DRV_RPI4_VCHIQ_PORT_MASK;
}

/*
 * Reports the bytes a message of a payload size takes in a slot.
 *
 * A message is its eight-byte header and its payload, rounded up to the
 * header's size, so the next header stays aligned.
 */
uint32_t
drv_rpi4_vchiq_stride(
	uint32_t size)
{
	uint32_t stride;

	/* The header and the payload, rounded up to a multiple of eight. */
	stride = size + DRV_RPI4_VCHIQ_HEADER_BYTES;
	stride = (stride + DRV_RPI4_VCHIQ_HEADER_BYTES - 1U) & ~(DRV_RPI4_VCHIQ_HEADER_BYTES - 1U);

	/* Succeeded: the bytes the message occupies. */
	return stride;
}

/*
 * Reports the bytes the shared memory needs for the slots.
 *
 * Slot zero and the data slots; the caller places anything else (the
 * fragment area, page lists) after them.
 */
size_t
drv_rpi4_vchiq_slot_bytes(void)
{
	/* Slot zero and the data slots of both sides. */
	return (size_t)(VCHIQ_ZERO_SLOTS + DRV_RPI4_VCHIQ_DATA_SLOTS) * DRV_RPI4_VCHIQ_SLOT_SIZE;
}

/*
 * Lays out slot zero and the ARM's half of it in zeroed memory.
 *
 * base is the CPU address of slot-aligned memory of at least
 * drv_rpi4_vchiq_slot_bytes() bytes that the VPU reads at its bus alias.
 * The data slots are split in two: the first half and its first slot
 * (the synchronous slot) go to the master, the second half to the ARM.
 * The ARM's state is marked initialised last, so a VPU that looks early
 * sees either nothing or a complete state.
 */
int
drv_rpi4_vchiq_ring_init(
	struct drv_rpi4_vchiq_ring *ring,
	void *base,
	size_t bytes,
	uint32_t fragments_bus,
	uint32_t fragment_count)
{
	struct drv_rpi4_vchiq_slot_zero *zero;
	struct drv_rpi4_vchiq_shared *local;
	uint8_t *sync_slot;
	size_t needed;
	uint32_t half;
	uint32_t slot;

	/* Refuses memory that is too small or does not start on a slot. */
	needed = drv_rpi4_vchiq_slot_bytes();
	if (bytes < needed)
		return EINVAL;
	if (((uintptr_t)base & VCHIQ_SLOT_MASK) != 0)
		return EINVAL;

	/* Starts from a clear slot zero. */
	zero = base;
	kern_memset(zero, 0, sizeof(*zero));

	/* The configuration the firmware checks against its own. */
	zero->magic = DRV_RPI4_VCHIQ_MAGIC;
	zero->version = DRV_RPI4_VCHIQ_VERSION;
	zero->version_min = DRV_RPI4_VCHIQ_VERSION_MIN;
	zero->slot_zero_size = sizeof(*zero);
	zero->slot_size = DRV_RPI4_VCHIQ_SLOT_SIZE;
	zero->max_slots = DRV_RPI4_VCHIQ_MAX_SLOTS;
	zero->max_slots_per_side = DRV_RPI4_VCHIQ_MAX_SLOTS_PER_SIDE;

	/* Where the fragment area for unaligned reads is, and how many fragments it has. */
	zero->platform_data[0] = fragments_bus;
	zero->platform_data[1] = fragment_count;

	/* The master's slots: its synchronous slot, then the rest of the first half. */
	half = DRV_RPI4_VCHIQ_DATA_SLOTS / 2U;
	zero->master.slot_sync = VCHIQ_ZERO_SLOTS;
	zero->master.slot_first = VCHIQ_ZERO_SLOTS + 1U;
	zero->master.slot_last = VCHIQ_ZERO_SLOTS + half - 1U;

	/* The ARM's slots: its synchronous slot, then the rest of the second half. */
	zero->slave.slot_sync = VCHIQ_ZERO_SLOTS + half;
	zero->slave.slot_first = VCHIQ_ZERO_SLOTS + half + 1U;
	zero->slave.slot_last = VCHIQ_ZERO_SLOTS + DRV_RPI4_VCHIQ_DATA_SLOTS - 1U;

	/* The ARM is the slave: it writes its own half and reads the master's. */
	local = &zero->slave;
	ring->base = base;
	ring->zero = zero;
	ring->local = local;
	ring->remote = &zero->master;

	/* Every ARM slot starts in the ARM's queue, ready to write into. */
	ring->slot_queue_available = 0;
	for (slot = local->slot_first; slot <= local->slot_last; slot++) {
		local->slot_queue[ring->slot_queue_available] = slot;
		ring->slot_queue_available++;
	}

	/* The VPU recycles into the queue after the slots already there. */
	local->slot_queue_recycle = ring->slot_queue_available;

	/* Nothing written or read yet; no event armed or fired. */
	ring->tx_pos = 0;
	ring->tx_slots = 0;
	ring->tx_data = NULL;
	ring->rx_pos = 0;
	ring->rx_data = NULL;
	ring->rx_slot = 0;
	local->tx_pos = 0;

	/*
	 * The synchronous slot holds one padding message, and its release
	 * event is fired: the slot is free.  The ARM sends no synchronous
	 * messages, but the master reads this state.
	 */
	sync_slot = slot_data(ring, local->slot_sync);
	((uint32_t *)sync_slot)[0] = drv_rpi4_vchiq_msgid(DRV_RPI4_VCHIQ_MSG_PADDING, 0, 0);
	((uint32_t *)sync_slot)[1] = 0;
	local->sync_release.fired = 1U;

	/* The debug area's first word says how many words follow the count. */
	local->debug[0] = DRV_RPI4_VCHIQ_DEBUG_WORDS;

	/* Publishes the state before saying that it is there. */
	kern_io_write_barrier();
	local->initialised = 1U;
	kern_io_write_barrier();

	/* Succeeded: the memory is ready to hand to the firmware. */
	return 0;
}

/*
 * Takes back the ARM's slots the VPU has finished reading.
 *
 * The VPU writes each slot it is done with into the ARM's slot queue and
 * moves slot_queue_recycle; every queue entry before it may be written
 * again.
 */
void
drv_rpi4_vchiq_ring_recycle(
	struct drv_rpi4_vchiq_ring *ring)
{
	uint32_t recycled;

	/* Reads the VPU's count after anything it wrote before it. */
	recycled = ring->local->slot_queue_recycle;
	kern_io_read_barrier();

	/* Every slot up to the VPU's count is the ARM's to write again. */
	ring->slot_queue_available = recycled;
}

/*
 * Reserves room for one message and reports where its payload goes.
 *
 * A message never straddles two slots: when it does not fit in the rest of
 * the current slot, the rest is filled with a padding message and the
 * message starts the next slot.  EAGAIN means the next slot is still the
 * VPU's to read; nothing changed, and the caller recycles and tries again.
 */
int
drv_rpi4_vchiq_ring_reserve(
	struct drv_rpi4_vchiq_ring *ring,
	uint32_t size,
	uint8_t **payload)
{
	uint32_t stride;
	uint32_t position;
	uint32_t space;
	uint32_t queue_index;
	uint32_t slot;
	int32_t ahead;
	uint8_t *padding;

	/* Refuses a payload that cannot fit one slot. */
	if (size > DRV_RPI4_VCHIQ_MAX_PAYLOAD)
		return EMSGSIZE;
	stride = drv_rpi4_vchiq_stride(size);

	/* The message starts here, or at the next slot when it does not fit. */
	position = ring->tx_pos;
	space = DRV_RPI4_VCHIQ_SLOT_SIZE - (position & VCHIQ_SLOT_MASK);
	if ((position & VCHIQ_SLOT_MASK) != 0 && stride > space)
		position += space;

	/* A new slot must already be back from the VPU. */
	if ((position & VCHIQ_SLOT_MASK) == 0) {
		ahead = (int32_t)(ring->slot_queue_available - ring->tx_slots);
		if (ahead <= 0)
			return EAGAIN;
	}

	/* Pads out the rest of the slot the message skips. */
	if (position != ring->tx_pos) {
		padding = ring->tx_data + (ring->tx_pos & VCHIQ_SLOT_MASK);
		((uint32_t *)padding)[0] = drv_rpi4_vchiq_msgid(DRV_RPI4_VCHIQ_MSG_PADDING, 0, 0);
		((uint32_t *)padding)[1] = space - DRV_RPI4_VCHIQ_HEADER_BYTES;
	}

	/* Enters the next slot from the ARM's queue, counting it. */
	if ((position & VCHIQ_SLOT_MASK) == 0) {
		queue_index = ring->tx_slots & VCHIQ_QUEUE_MASK;
		slot = ring->local->slot_queue[queue_index];
		ring->tx_data = slot_data(ring, slot);
		ring->tx_slots++;
	}

	/* The header stays where the message starts; the payload follows it. */
	*payload = ring->tx_data + (position & VCHIQ_SLOT_MASK) + DRV_RPI4_VCHIQ_HEADER_BYTES;
	ring->tx_pos = position;

	/* Succeeded: the caller writes the payload, then commits. */
	return 0;
}

/*
 * Publishes the message the last reservation made room for.
 *
 * The header is written after the payload, and tx_pos after the header,
 * each behind a barrier, so the VPU never reads a half-written message.
 * The caller then signals the VPU's trigger.
 */
void
drv_rpi4_vchiq_ring_commit(
	struct drv_rpi4_vchiq_ring *ring,
	uint32_t msgid,
	uint32_t size)
{
	uint8_t *header;

	/* Writes the header of the reserved message. */
	header = ring->tx_data + (ring->tx_pos & VCHIQ_SLOT_MASK);
	((uint32_t *)header)[0] = msgid;
	((uint32_t *)header)[1] = size;

	/* Moves past the message and makes the new position the VPU's to read. */
	ring->tx_pos += drv_rpi4_vchiq_stride(size);
	kern_io_write_barrier();
	ring->local->tx_pos = ring->tx_pos;
	kern_io_write_barrier();
}

/*
 * Reports the VPU's next unread message.
 *
 * EAGAIN means the ARM has read everything the VPU has published; EIO means
 * the stream is malformed (a message past its slot's end).  The payload
 * stays valid until drv_rpi4_vchiq_ring_advance() moves past it.
 */
int
drv_rpi4_vchiq_ring_next(
	struct drv_rpi4_vchiq_ring *ring,
	uint32_t *msgid,
	uint32_t *size,
	const uint8_t **payload)
{
	uint32_t published;
	uint32_t queue_index;
	uint32_t offset;
	uint32_t length;
	uint32_t stride;
	const uint8_t *header;

	/* Reads the VPU's position before anything it published up to it. */
	published = ring->remote->tx_pos;
	kern_io_read_barrier();
	if (ring->rx_pos == published)
		return EAGAIN;

	/*
	 * Enters the VPU's next slot.  Its use count starts at one, the
	 * reader's own hold, which the end of the slot gives back.
	 */
	if (ring->rx_data == NULL) {
		queue_index = (ring->rx_pos / DRV_RPI4_VCHIQ_SLOT_SIZE) & VCHIQ_QUEUE_MASK;
		ring->rx_slot = ring->remote->slot_queue[queue_index];
		if (ring->rx_slot < ring->remote->slot_first || ring->rx_slot > ring->remote->slot_last)
			return EIO;
		ring->rx_data = slot_data(ring, ring->rx_slot);
		ring->zero->slots[ring->rx_slot].use_count = 1;
		ring->zero->slots[ring->rx_slot].release_count = 0;
	}

	/* Reads the header at the read position. */
	offset = ring->rx_pos & VCHIQ_SLOT_MASK;
	header = ring->rx_data + offset;
	length = ((const uint32_t *)header)[1];

	/* Refuses a message that would run past its slot. */
	if (length > DRV_RPI4_VCHIQ_MAX_PAYLOAD)
		return EIO;
	stride = drv_rpi4_vchiq_stride(length);
	if (offset + stride > DRV_RPI4_VCHIQ_SLOT_SIZE)
		return EIO;

	/* Succeeded: the message's id, size and payload. */
	*msgid = ((const uint32_t *)header)[0];
	*size = length;
	*payload = header + DRV_RPI4_VCHIQ_HEADER_BYTES;
	return 0;
}

/*
 * Moves past the message just read, handing its slot back at the slot's end.
 *
 * Reports true when a slot went back into the VPU's queue; the caller then
 * signals the VPU's recycle event.
 */
bool
drv_rpi4_vchiq_ring_advance(
	struct drv_rpi4_vchiq_ring *ring,
	uint32_t size)
{
	bool released;

	/* Moves past the message. */
	ring->rx_pos += drv_rpi4_vchiq_stride(size);

	/* The end of a slot gives back the reader's hold on it. */
	released = false;
	if ((ring->rx_pos & VCHIQ_SLOT_MASK) == 0) {
		released = release_remote_slot(ring);
		ring->rx_data = NULL;
	}

	/* Succeeded: reports whether the VPU has a slot back. */
	return released;
}

/*
 * Marks an event fired and reports whether its waiter must be woken.
 *
 * The waiter arms the event and then looks at fired before it sleeps, so a
 * signal between the two is seen either way.
 */
bool
drv_rpi4_vchiq_event_fire(
	struct drv_rpi4_vchiq_event *event)
{
	/* Publishes what the event announces before the event itself. */
	kern_io_write_barrier();
	event->fired = 1U;
	kern_io_barrier();

	/* Only an armed waiter sleeps and needs its doorbell. */
	if (event->armed == 0)
		return false;

	/* Succeeded: the waiter is asleep and must be woken. */
	return true;
}

/*
 * Builds the page list of one physically contiguous bulk buffer.
 *
 * The list is the byte length, the type, the offset of the data in its
 * first page and one word per run of up to 256 pages.  On the Pi 4
 * (the long bulk form) a word is the run's first page number shifted left
 * by eight with the number of pages after the first in the low byte, so
 * pages above 4 GiB are reachable.  used receives the list's size.
 */
int
drv_rpi4_vchiq_pagelist(
	void *pagelist,
	size_t capacity,
	uint64_t physical,
	uint32_t bytes,
	uint16_t type,
	size_t *used)
{
	uint32_t *words;
	uint64_t first_page;
	uint64_t pages;
	uint64_t page;
	uint64_t run;
	size_t needed;
	uint32_t count;
	uint32_t index;

	/* Refuses an empty transfer or an unknown type. */
	if (bytes == 0)
		return EINVAL;
	if (type != DRV_RPI4_VCHIQ_PAGELIST_WRITE && type != DRV_RPI4_VCHIQ_PAGELIST_READ)
		return EINVAL;

	/* Counts the pages the buffer touches and the words the list needs. */
	first_page = physical / DRV_RPI4_VCHIQ_PAGE_SIZE;
	pages = (physical % DRV_RPI4_VCHIQ_PAGE_SIZE + bytes + DRV_RPI4_VCHIQ_PAGE_SIZE - 1U) / DRV_RPI4_VCHIQ_PAGE_SIZE;
	count = (uint32_t)((pages + DRV_RPI4_VCHIQ_PAGELIST_RUN_PAGES - 1U) / DRV_RPI4_VCHIQ_PAGELIST_RUN_PAGES);
	needed = DRV_RPI4_VCHIQ_PAGELIST_HEADER_BYTES + (size_t)count * 4U;
	if (needed > capacity)
		return ENOSPC;

	/* Refuses pages a 32-bit address word cannot name. */
	if (first_page + pages - 1U > VCHIQ_PAGELIST_PAGE_LIMIT)
		return ERANGE;

	/* The header: the length, the type and the offset in the first page. */
	words = pagelist;
	words[0] = bytes;
	words[1] = (uint32_t)type | ((uint32_t)(physical % DRV_RPI4_VCHIQ_PAGE_SIZE) << 16);

	/* One word per run of up to 256 consecutive pages. */
	page = first_page;
	index = 0;
	while (pages > 0) {
		run = pages;
		if (run > DRV_RPI4_VCHIQ_PAGELIST_RUN_PAGES)
			run = DRV_RPI4_VCHIQ_PAGELIST_RUN_PAGES;
		words[2U + index] = (uint32_t)(page << 8) | (uint32_t)((run - 1U) & VCHIQ_PAGELIST_RUN_MASK);
		index++;
		page += run;
		pages -= run;
	}

	/* Succeeded: the list is complete. */
	*used = needed;
	return 0;
}

/* Reports the CPU address of a slot by its index. */
static uint8_t *
slot_data(
	struct drv_rpi4_vchiq_ring *ring,
	uint32_t index)
{
	/* Slots follow one another from slot zero. */
	return ring->base + (size_t)index * DRV_RPI4_VCHIQ_SLOT_SIZE;
}

/*
 * Gives back the reader's hold on the slot just finished, and returns the
 * slot to the VPU when nothing else holds it.
 */
static bool
release_remote_slot(
	struct drv_rpi4_vchiq_ring *ring)
{
	struct drv_rpi4_vchiq_slot_info *info;
	uint32_t recycle;

	/*
	 * The ARM copies every message out before it advances, so the
	 * reader's own hold is the only one, and the slot is free now.
	 */
	info = &ring->zero->slots[ring->rx_slot];
	info->release_count++;
	if (info->release_count != info->use_count)
		return false;

	/* Puts the slot at the end of the VPU's queue, then moves the queue's end. */
	recycle = ring->remote->slot_queue_recycle;
	ring->remote->slot_queue[recycle & VCHIQ_QUEUE_MASK] = ring->rx_slot;
	kern_io_write_barrier();
	ring->remote->slot_queue_recycle = recycle + 1U;

	/* Succeeded: the VPU has the slot back. */
	return true;
}
