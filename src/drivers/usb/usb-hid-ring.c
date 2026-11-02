/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The ring of USB HID reports (see usb-hid-ring.h).  Pure (no lock, no
 * allocation): the driver serializes every call, and the host test compiles
 * the file as it is.
 */

#include <drivers/usb/usb-hid-ring.h>

#include <uapi/errno.h>

/* The bytes of one record in the ring's byte stream. */
#define RING_RECORD_BYTES	((uint64_t)sizeof(struct drv_usb_hid_ring_record))

/* A reader of /dev/usbhid-ring decodes 64-byte records with no padding. */
_Static_assert(sizeof(struct drv_usb_hid_ring_record) == 64U, "USB HID ring record size");

/*
 * Starts an empty ring over caller-provided records.
 */
int
drv_usb_hid_ring_init(
	struct drv_usb_hid_ring *ring,
	struct drv_usb_hid_ring_record *records,
	uint64_t capacity)
{
	/* Refuses no records, and a capacity the index mask cannot wrap. */
	if (ring == NULL || records == NULL || capacity == 0U)
		return EINVAL;
	if ((capacity & (capacity - 1U)) != 0U)
		return EINVAL;

	/* An empty ring: the first record gets sequence 0. */
	ring->records = records;
	ring->capacity = capacity;
	ring->next = 0;

	/* Succeeded: the ring takes records. */
	return 0;
}

/*
 * Adds a record, overwriting the oldest when the ring is full.
 */
void
drv_usb_hid_ring_put(
	struct drv_usb_hid_ring *ring,
	const struct drv_usb_hid_ring_record *record)
{
	struct drv_usb_hid_ring_record *slot;

	/* The slot of the next sequence, which held the record capacity older. */
	slot = &ring->records[ring->next & (ring->capacity - 1U)];
	*slot = *record;
	slot->sequence = (uint32_t)ring->next;

	/* The record is held; the next one follows it. */
	ring->next++;
}

/*
 * Copies the ring's bytes from an offset on.
 */
size_t
drv_usb_hid_ring_read(
	const struct drv_usb_hid_ring *ring,
	uint64_t offset,
	uint8_t *out,
	size_t size,
	uint64_t *next_offset)
{
	const struct drv_usb_hid_ring_record *record;
	const uint8_t *bytes;
	uint64_t oldest;
	uint64_t sequence;
	uint64_t within;
	size_t done;

	/* The oldest record still held: everything when the ring has not wrapped. */
	oldest = 0;
	if (ring->next > ring->capacity)
		oldest = ring->next - ring->capacity;

	/* An offset in an overwritten record moves to the oldest record's start. */
	sequence = offset / RING_RECORD_BYTES;
	within = offset % RING_RECORD_BYTES;
	if (sequence < oldest) {
		sequence = oldest;
		within = 0;
	}

	/* Copies whole and partial records until the request or the held records end. */
	done = 0;
	while (done < size && sequence < ring->next) {
		record = &ring->records[sequence & (ring->capacity - 1U)];
		bytes = (const uint8_t *)record;
		out[done] = bytes[within];
		done++;
		within++;

		/* The next record starts after the last byte of this one. */
		if (within == RING_RECORD_BYTES) {
			within = 0;
			sequence++;
		}
	}

	/* Succeeded: the bytes copied, and the offset after them. */
	*next_offset = sequence * RING_RECORD_BYTES + within;
	return done;
}
