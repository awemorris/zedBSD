/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The pure part of the Bluetooth HCI class (drivers/generic/bt-hci.h,
 * ws143-p002): the rings that queue the received packets, the reassembly
 * of a received byte stream into packets, and the checks of a packet a
 * program writes.  Nothing here locks, sleeps or allocates; the class
 * holds its lock around the rings, and the host test builds this file as
 * it is.
 *
 * A record in a ring is laid out as its body's length (two bytes, low
 * first), its type, its sequence number (four bytes, low first), then the
 * body.  A record may wrap around the ring's end; every byte is addressed
 * modulo the capacity.
 */

#include <drivers/generic/bt-hci.h>

#include <uapi/errno.h>

static void ring_put(struct bt_hci_ring *ring, size_t offset, const uint8_t *source, size_t length);
static void ring_get(const struct bt_hci_ring *ring, size_t offset, uint8_t *target, size_t length);
static size_t assembler_header(unsigned mode);
static int assembler_total(const struct bt_hci_assembler *assembler, size_t *total);
static size_t assembler_take(struct bt_hci_assembler *assembler, const uint8_t *data, size_t size, size_t wanted);

/*
 * Prepares an empty ring over capacity bytes, reserve of which only the
 * kernel's notices may use.
 */
void
bt_hci_ring_init(
	struct bt_hci_ring *ring,
	uint8_t *bytes,
	size_t capacity,
	size_t reserve)
{
	/* The storage and the share kept for the notices. */
	ring->bytes = bytes;
	ring->capacity = capacity;
	ring->reserve = reserve;

	/* Nothing queued yet. */
	ring->head = 0U;
	ring->used = 0U;
	ring->count = 0U;
}

/*
 * Reports how many bytes an ordinary record may still take, its header
 * included; the share reserved for the notices is not counted.
 */
size_t
bt_hci_ring_room(
	const struct bt_hci_ring *ring)
{
	size_t free_bytes;

	/* The bytes no record takes. */
	free_bytes = ring->capacity - ring->used;

	/* A ring whose free bytes are only the notices' share has no room for a packet. */
	if (free_bytes <= ring->reserve)
		return 0U;

	/* Succeeded: what is left beside the notices' share. */
	return free_bytes - ring->reserve;
}

/*
 * Appends one record: the type, the sequence number and length bytes of
 * body.  A notice (notice nonzero) may use the reserved share.  Returns 0,
 * EINVAL for a body longer than a record can say, or ENOSPC.
 */
int
bt_hci_ring_push(
	struct bt_hci_ring *ring,
	uint8_t type,
	uint32_t sequence,
	const uint8_t *body,
	size_t length,
	int notice)
{
	uint8_t header[BT_HCI_RECORD_HEADER];
	size_t available;
	size_t needed;
	size_t tail;

	/* A record's length field holds 16 bits. */
	if (length > 0xffffU)
		return EINVAL;

	/* The room: everything free for a notice, the free bytes beside the reserve for a packet. */
	needed = BT_HCI_RECORD_HEADER + length;
	available = ring->capacity - ring->used;
	if (!notice)
		available = bt_hci_ring_room(ring);
	if (needed > available)
		return ENOSPC;

	/* The header: the length, the type and the sequence number, each low byte first. */
	header[0] = (uint8_t)(length & 0xffU);
	header[1] = (uint8_t)(length >> 8);
	header[2] = type;
	header[3] = (uint8_t)(sequence & 0xffU);
	header[4] = (uint8_t)((sequence >> 8) & 0xffU);
	header[5] = (uint8_t)((sequence >> 16) & 0xffU);
	header[6] = (uint8_t)((sequence >> 24) & 0xffU);

	/* The header and the body after the last record. */
	tail = ring->head + ring->used;
	ring_put(ring, tail, header, BT_HCI_RECORD_HEADER);
	ring_put(ring, tail + BT_HCI_RECORD_HEADER, body, length);

	/* The record is now the newest. */
	ring->used += needed;
	ring->count++;

	/* Succeeded: the record is queued. */
	return 0;
}

/*
 * Reads the oldest record's type, sequence number and body length without
 * taking it.  Returns 0, or ENOENT when the ring is empty.
 */
int
bt_hci_ring_peek(
	const struct bt_hci_ring *ring,
	uint8_t *type,
	uint32_t *sequence,
	size_t *length)
{
	uint8_t header[BT_HCI_RECORD_HEADER];

	/* An empty ring has no oldest record. */
	if (ring->count == 0U)
		return ENOENT;

	/* The oldest record's header. */
	ring_get(ring, ring->head, header, BT_HCI_RECORD_HEADER);

	/* Its fields, each low byte first. */
	*length = (size_t)header[0] | ((size_t)header[1] << 8);
	*type = header[2];
	*sequence = (uint32_t)header[3];
	*sequence |= (uint32_t)header[4] << 8;
	*sequence |= (uint32_t)header[5] << 16;
	*sequence |= (uint32_t)header[6] << 24;

	/* Succeeded: the oldest record's header. */
	return 0;
}

/*
 * Copies the oldest record as a program reads it, the type's byte and then
 * the body, into target (which holds at least 1 + its body length), and
 * reports how many bytes that is.  The ring must not be empty.
 */
size_t
bt_hci_ring_copy(
	const struct bt_hci_ring *ring,
	uint8_t *target)
{
	uint32_t sequence;
	size_t length;
	uint8_t type;
	int error;

	/* The oldest record's type and length. */
	error = bt_hci_ring_peek(ring, &type, &sequence, &length);
	if (error != 0)
		return 0U;

	/* The type's byte, then the body. */
	target[0] = type;
	ring_get(ring, ring->head + BT_HCI_RECORD_HEADER, target + 1, length);

	/* Succeeded: the packet's length with its type's byte. */
	return 1U + length;
}

/* Takes the oldest record off the ring. */
void
bt_hci_ring_pop(
	struct bt_hci_ring *ring)
{
	uint32_t sequence;
	size_t length;
	size_t taken;
	uint8_t type;
	int error;

	/* The oldest record's length. */
	error = bt_hci_ring_peek(ring, &type, &sequence, &length);
	if (error != 0)
		return;

	/* Its bytes are free again and the next record is the oldest. */
	taken = BT_HCI_RECORD_HEADER + length;
	ring->head = (ring->head + taken) % ring->capacity;
	ring->used -= taken;
	ring->count--;

	/* An empty ring starts again at its beginning. */
	if (ring->count == 0U)
		ring->head = 0U;
}

/* Drops every record of a ring. */
void
bt_hci_ring_clear(
	struct bt_hci_ring *ring)
{
	/* Nothing queued. */
	ring->head = 0U;
	ring->used = 0U;
	ring->count = 0U;
}

/*
 * Prepares an assembler over buffer, which holds capacity bytes (at least
 * bt_hci_assembly_capacity() of the mode), for a stream of the mode.
 */
void
bt_hci_assembler_init(
	struct bt_hci_assembler *assembler,
	uint8_t *buffer,
	size_t capacity,
	unsigned mode,
	size_t acl_data_max)
{
	/* The storage, then an empty packet of the mode. */
	assembler->buffer = buffer;
	assembler->capacity = capacity;
	bt_hci_assembler_restart(assembler, mode, acl_data_max);
}

/*
 * Drops the packet being gathered and sets the mode and the ACL limit the
 * next bytes are read with (at a change of the bootloader's path, at a
 * reset, and after a malformed transfer).
 */
void
bt_hci_assembler_restart(
	struct bt_hci_assembler *assembler,
	unsigned mode,
	size_t acl_data_max)
{
	/* Nothing gathered, and the way the next packet is read. */
	assembler->have = 0U;
	assembler->mode = mode;
	assembler->acl_data_max = acl_data_max;
}

/*
 * Feeds the bytes of one received transfer: every packet they complete
 * goes to deliver, and a packet they leave unfinished stays gathered for
 * the next transfer.  *consumed says how many of the bytes were taken.
 *
 * Returns 0 when every byte was taken.  When deliver refuses a packet
 * (ENOSPC: no room), the packet stays gathered and whole, the feed stops
 * there, and the caller feeds the rest of the bytes later (with no bytes,
 * to deliver the gathered packet alone).  EBADMSG means an ACL header
 * said a packet is longer than the limit: the gathered packet is dropped
 * and *consumed covers the whole transfer, so the next transfer starts a
 * new packet.
 */
int
bt_hci_assembler_feed(
	struct bt_hci_assembler *assembler,
	const uint8_t *data,
	size_t size,
	bt_hci_packet_fn deliver,
	void *context,
	size_t *consumed)
{
	size_t header;
	size_t offset;
	size_t total;
	uint8_t type;
	int error;

	/* The header the mode's packets begin with, and the type they are delivered as. */
	header = assembler_header(assembler->mode);
	type = BT_PACKET_EVENT;
	if (assembler->mode == BT_HCI_ASSEMBLE_ACL)
		type = BT_PACKET_ACL;

	/* Gathers and delivers packets until the bytes run out or a packet is refused. */
	offset = 0U;
	for (;;) {
		/* A whole header says the packet's length; a too-long ACL header makes the transfer malformed. */
		total = 0U;
		if (assembler->have >= header) {
			error = assembler_total(assembler, &total);
			if (error != 0) {
				assembler->have = 0U;
				*consumed = size;
				return error;
			}
		}

		/* A whole packet goes to the class; a refused one stays for the next feed. */
		if (total != 0U && assembler->have == total) {
			error = deliver(context, type, assembler->buffer, total);
			if (error != 0) {
				*consumed = offset;
				return error;
			}

			/* The next packet starts empty. */
			assembler->have = 0U;
			continue;
		}

		/* The bytes are used up; a packet left unfinished waits for the next transfer. */
		if (offset == size)
			break;

		/* The header first, then the rest of the packet. */
		if (assembler->have < header)
			offset += assembler_take(assembler, data + offset, size - offset, header);
		else
			offset += assembler_take(assembler, data + offset, size - offset, total);
	}

	/* Succeeded: every byte was taken. */
	*consumed = offset;
	return 0;
}

/*
 * Checks one packet a program writes: a command whose length is its
 * header's, or an ACL packet whose length is its header's and whose data
 * is within the limit.  Returns 0 or EINVAL.
 */
int
bt_hci_check_write(
	const uint8_t *packet,
	size_t size,
	size_t acl_data_max)
{
	size_t data_length;

	/* The type's byte at least. */
	if (size < 1U)
		return EINVAL;

	/* A command: the opcode, the parameters' length and exactly that many. */
	if (packet[0] == BT_PACKET_COMMAND) {
		if (size < 1U + 3U)
			return EINVAL;
		if (size != 1U + 3U + (size_t)packet[3])
			return EINVAL;

		/* Succeeded: a whole command. */
		return 0;
	}

	/* Anything but a command or an ACL packet is not carried. */
	if (packet[0] != BT_PACKET_ACL)
		return EINVAL;

	/* An ACL packet: the handle, the data length and exactly that much, within the limit. */
	if (size < 1U + BT_HCI_ACL_HEADER)
		return EINVAL;
	data_length = (size_t)packet[3] | ((size_t)packet[4] << 8);
	if (data_length > acl_data_max)
		return EINVAL;
	if (size != 1U + BT_HCI_ACL_HEADER + data_length)
		return EINVAL;

	/* Succeeded: a whole ACL packet. */
	return 0;
}

/*
 * Reports whether a checked packet is the Intel bootloader's Secure Send
 * command, which the bootloader takes on the bulk OUT pipe.
 */
int
bt_hci_is_secure_send(
	const uint8_t *packet,
	size_t size)
{
	unsigned opcode;

	/* A command with its opcode. */
	if (size < 1U + 3U)
		return 0;
	if (packet[0] != BT_PACKET_COMMAND)
		return 0;

	/* The opcode, low byte first. */
	opcode = (unsigned)packet[1] | ((unsigned)packet[2] << 8);
	if (opcode != BT_HCI_OPCODE_INTEL_SECURE_SEND)
		return 0;

	/* Succeeded: it is Secure Send. */
	return 1;
}

/* Reports the bytes an assembler of the mode needs to hold its longest packet. */
size_t
bt_hci_assembly_capacity(
	unsigned mode)
{
	/* An ACL packet's header and the longest data. */
	if (mode == BT_HCI_ASSEMBLE_ACL)
		return BT_HCI_ACL_HEADER + BT_ACL_DATA_MAX;

	/* An event's header and the longest parameters. */
	return BT_HCI_EVENT_HEADER + 255U;
}

/* Writes length bytes into the ring from offset on, wrapping at its end. */
static void
ring_put(
	struct bt_hci_ring *ring,
	size_t offset,
	const uint8_t *source,
	size_t length)
{
	size_t index;

	/* Each byte at its place modulo the capacity. */
	for (index = 0U; index < length; index++)
		ring->bytes[(offset + index) % ring->capacity] = source[index];
}

/* Reads length bytes of the ring from offset on, wrapping at its end. */
static void
ring_get(
	const struct bt_hci_ring *ring,
	size_t offset,
	uint8_t *target,
	size_t length)
{
	size_t index;

	/* Each byte from its place modulo the capacity. */
	for (index = 0U; index < length; index++)
		target[index] = ring->bytes[(offset + index) % ring->capacity];
}

/* Reports the header's length of the mode's packets. */
static size_t
assembler_header(
	unsigned mode)
{
	/* An ACL packet's handle and data length. */
	if (mode == BT_HCI_ASSEMBLE_ACL)
		return BT_HCI_ACL_HEADER;

	/* An event's code and parameters' length. */
	return BT_HCI_EVENT_HEADER;
}

/*
 * Works out a gathered header's whole packet length.  Returns 0, or
 * EBADMSG for an ACL packet past the limit or a packet past the buffer.
 */
static int
assembler_total(
	const struct bt_hci_assembler *assembler,
	size_t *total)
{
	size_t body;

	/* The body's length the header says. */
	if (assembler->mode == BT_HCI_ASSEMBLE_ACL) {
		body = (size_t)assembler->buffer[2] | ((size_t)assembler->buffer[3] << 8);
		if (body > assembler->acl_data_max)
			return EBADMSG;
	} else {
		body = (size_t)assembler->buffer[1];
	}

	/* A packet the buffer cannot hold is refused rather than overrun. */
	*total = assembler_header(assembler->mode) + body;
	if (*total > assembler->capacity)
		return EBADMSG;

	/* Succeeded: the packet's length. */
	return 0;
}

/*
 * Gathers bytes of data until the packet being gathered holds wanted
 * bytes, or the data runs out; reports how many bytes were taken.
 */
static size_t
assembler_take(
	struct bt_hci_assembler *assembler,
	const uint8_t *data,
	size_t size,
	size_t wanted)
{
	size_t taken;
	size_t index;

	/* As many as are missing, or as many as there are. */
	taken = wanted - assembler->have;
	if (taken > size)
		taken = size;

	/* Each byte after the ones gathered. */
	for (index = 0U; index < taken; index++)
		assembler->buffer[assembler->have + index] = data[index];
	assembler->have += taken;

	/* Succeeded: the bytes taken. */
	return taken;
}
