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
 * program writes; and for SCO (ws197-p007a2) the queue that drops its
 * oldest, the links' checks and alternate, the reassembly of the
 * isochronous IN bytes, and the queue of bytes for the OUT pipe.  The
 * reassembly also reads a UART's H4 stream, where every packet starts with
 * its type's byte (ws215-p003).  Nothing here locks, sleeps or allocates; the class
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
static int assembler_measure(const struct bt_hci_assembler *assembler, size_t *header, size_t *total, uint8_t *type);
static int assembler_total(const struct bt_hci_assembler *assembler, size_t *total);
static int assembler_h4_header(const struct bt_hci_assembler *assembler, size_t *header);
static int assembler_h4_total(const struct bt_hci_assembler *assembler, size_t *total);
static int event_code_known(unsigned code);
static size_t assembler_take(struct bt_hci_assembler *assembler, const uint8_t *data, size_t size, size_t wanted);
static int sco_header_fits(const struct bt_hci_sco_assembler *assembler);

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
 * said a packet is longer than the limit (or, in the H4 mode, a header
 * that no controller sends came): the gathered packet is dropped and
 * *consumed covers the whole transfer, so the next transfer starts a new
 * packet.
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
	const uint8_t *packet;
	size_t length;
	size_t header;
	size_t offset;
	size_t total;
	uint8_t type;
	int error;

	/* Gathers and delivers packets until the bytes run out or a packet is refused. */
	offset = 0U;
	for (;;) {
		/* The header gathered so far says the packet's length; a header no controller sends makes the transfer malformed. */
		error = assembler_measure(assembler, &header, &total, &type);
		if (error != 0) {
			assembler->have = 0U;
			*consumed = size;
			return error;
		}

		/* A whole packet goes to the class, without the H4 type's byte; a refused one stays for the next feed. */
		if (total != 0U && assembler->have == total) {
			packet = assembler->buffer;
			length = total;
			if (assembler->mode == BT_HCI_ASSEMBLE_H4) {
				packet = assembler->buffer + 1;
				length = total - 1U;
			}

			/* The class takes it, or refuses it for room. */
			error = deliver(context, type, packet, length);
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
 * Puts a whole packet the transport found by itself (in the H4 form, the
 * type's byte first) into an empty assembler, as though it had been
 * gathered: the next feed, with no bytes, delivers it, and keeps it when
 * the class refuses it.  Returns 0, EBUSY when a packet is being gathered,
 * or EINVAL for a packet the assembler cannot hold or whose header does
 * not say its length.
 */
int
bt_hci_assembler_load(
	struct bt_hci_assembler *assembler,
	const uint8_t *packet,
	size_t length)
{
	size_t header;
	size_t total;
	size_t index;
	uint8_t type;
	int error;

	/* Only an assembler with nothing gathered takes a packet whole. */
	if (assembler->have != 0U)
		return EBUSY;
	if (length == 0U || length > assembler->capacity)
		return EINVAL;

	/* The packet's bytes, as the feed would have gathered them. */
	for (index = 0U; index < length; index++)
		assembler->buffer[index] = packet[index];
	assembler->have = length;

	/* A packet whose header says another length is not taken. */
	error = assembler_measure(assembler, &header, &total, &type);
	if (error != 0 || total != length) {
		assembler->have = 0U;
		return EINVAL;
	}

	/* Succeeded: the next feed delivers the packet. */
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

	/* Anything but a command or an ACL packet is not carried here (SCO: bt_hci_check_sco). */
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

	/* The type's byte and the longest packet, an ACL packet's. */
	if (mode == BT_HCI_ASSEMBLE_H4)
		return 1U + BT_HCI_ACL_HEADER + BT_ACL_DATA_MAX;

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
 * Works out how long the gathered packet's header is, its whole length
 * once the header is complete (0 before), and the type it is delivered as.
 * Returns 0, or EBADMSG for a header the mode does not take.
 */
static int
assembler_measure(
	const struct bt_hci_assembler *assembler,
	size_t *header,
	size_t *total,
	uint8_t *type)
{
	int error;

	/* Nothing is known of the length until the header is complete. */
	*total = 0U;

	/* An H4 stream: the type's byte says which header follows. */
	if (assembler->mode == BT_HCI_ASSEMBLE_H4) {
		*type = 0U;
		*header = 1U;
		if (assembler->have == 0U)
			return 0;

		/* The type's header, refusing a type no controller sends. */
		*type = assembler->buffer[0];
		error = assembler_h4_header(assembler, header);
		if (error != 0)
			return error;

		/* A whole header: the packet's length, refusing fields out of range. */
		if (assembler->have >= *header) {
			error = assembler_h4_total(assembler, total);
			if (error != 0)
				return error;
		}

		/* Succeeded: the header's length, and the packet's once known. */
		return 0;
	}

	/* An event's or an ACL packet's stream: one header, one type. */
	*header = assembler_header(assembler->mode);
	*type = BT_PACKET_EVENT;
	if (assembler->mode == BT_HCI_ASSEMBLE_ACL)
		*type = BT_PACKET_ACL;

	/* A whole header: the packet's length. */
	if (assembler->have >= *header) {
		error = assembler_total(assembler, total);
		if (error != 0)
			return error;
	}

	/* Succeeded: the header's length, and the packet's once known. */
	return 0;
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
 * Works out the header's length of an H4 packet from its type's byte:
 * the type's byte and an event's, an ACL packet's or an SCO packet's
 * header.  Returns 0, or EBADMSG for a type a controller does not send.
 */
static int
assembler_h4_header(
	const struct bt_hci_assembler *assembler,
	size_t *header)
{
	uint8_t type;

	/* Each type a controller sends, with its header. */
	type = assembler->buffer[0];
	if (type == BT_PACKET_EVENT) {
		*header = 1U + BT_HCI_EVENT_HEADER;
	} else if (type == BT_PACKET_ACL) {
		*header = 1U + BT_HCI_ACL_HEADER;
	} else if (type == BT_PACKET_SCO) {
		*header = 1U + BT_HCI_SCO_PACKET_HEADER;
	} else {
		/* A command, an ISO packet or a byte of no type: the line is out of step. */
		return EBADMSG;
	}

	/* Succeeded: the header's length with the type's byte. */
	return 0;
}

/*
 * Works out a whole H4 header's packet length, checking what the header
 * says against what a controller sends.  Returns 0, or EBADMSG for an
 * unknown event code, a handle out of range, an ACL packet past the limit
 * or a packet past the buffer.
 */
static int
assembler_h4_total(
	const struct bt_hci_assembler *assembler,
	size_t *total)
{
	const uint8_t *bytes;
	unsigned handle;
	size_t body;
	int known;

	/* The gathered header, the type's byte first. */
	bytes = assembler->buffer;

	/* Each type's header. */
	if (bytes[0] == BT_PACKET_EVENT) {
		/* An event: a code the specification defines or the vendor's, and its parameters' length. */
		known = event_code_known(bytes[1]);
		if (!known)
			return EBADMSG;
		body = (size_t)bytes[2];
		*total = 1U + BT_HCI_EVENT_HEADER + body;
	} else if (bytes[0] == BT_PACKET_ACL) {
		/* An ACL packet: a connection handle and a data length within the limit. */
		handle = ((unsigned)bytes[1] | ((unsigned)bytes[2] << 8)) & BT_HCI_ACL_HANDLE_MASK;
		if (handle > BT_HCI_ACL_HANDLE_MAX)
			return EBADMSG;
		body = (size_t)bytes[3] | ((size_t)bytes[4] << 8);
		if (body > assembler->acl_data_max)
			return EBADMSG;
		*total = 1U + BT_HCI_ACL_HEADER + body;
	} else {
		/* An SCO packet: a connection handle and its data length. */
		handle = ((unsigned)bytes[1] | ((unsigned)bytes[2] << 8)) & BT_HCI_SCO_HANDLE_MASK;
		if (handle > BT_HCI_SCO_HANDLE_MAX)
			return EBADMSG;
		body = (size_t)bytes[3];
		*total = 1U + BT_HCI_SCO_PACKET_HEADER + body;
	}

	/* A packet the buffer cannot hold is refused rather than overrun. */
	if (*total > assembler->capacity)
		return EBADMSG;

	/* Succeeded: the packet's length with the type's byte. */
	return 0;
}

/* Tells whether an event code is one the specification defines, or the vendor's. */
static int
event_code_known(
	unsigned code)
{
	/* The vendor's events. */
	if (code == BT_HCI_EVENT_CODE_VENDOR)
		return 1;

	/* Below the first defined code (0x00 is none). */
	if (code < BT_HCI_EVENT_CODE_FIRST)
		return 0;

	/* Above the last one the specification defines. */
	if (code > BT_HCI_EVENT_CODE_LAST)
		return 0;

	/* Succeeded: a defined event. */
	return 1;
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

/*
 * Checks an SCO packet a program writes (ws197-p007a2): the type's byte,
 * the handle and the data length, and exactly that much data.  Returns 0
 * or EINVAL.
 */
int
bt_hci_check_sco(
	const uint8_t *packet,
	size_t size)
{
	/* The type and the header. */
	if (size < 1U + BT_SCO_HEADER || packet[0] != BT_PACKET_SCO)
		return EINVAL;

	/* Exactly the data the header says. */
	if (size != 1U + BT_SCO_HEADER + (size_t)packet[3])
		return EINVAL;

	/* Succeeded: a whole SCO packet. */
	return 0;
}

/*
 * Queues a record as bt_hci_ring_push does, taking the oldest records out
 * until it fits (the SCO packets' ring: the newest sound matters, ws197-
 * p007a2); *dropped counts them.  Returns 0, or EINVAL for a record the
 * ring cannot hold even empty.
 */
int
bt_hci_ring_push_drop(
	struct bt_hci_ring *ring,
	uint8_t type,
	uint32_t sequence,
	const uint8_t *body,
	size_t length,
	unsigned *dropped)
{
	int error;

	/* Until it fits, or nothing is left to drop. */
	*dropped = 0U;
	for (;;) {
		error = bt_hci_ring_push(ring, type, sequence, body, length, 0);
		if (error != ENOSPC)
			break;
		if (ring->count == 0U)
			return EINVAL;

		/* The oldest goes. */
		bt_hci_ring_pop(ring);
		(*dropped)++;
	}

	/* Queued, or refused for its length. */
	return error;
}

/*
 * Checks the synchronous links a program sets (struct bt_sco, ws197-p007a2
 * design S2): reserved zero, at most BT_SCO_LINKS_MAX, each handle in range
 * and once, each length 1 to BT_SCO_DATA_MAX, CVSD links all of one sample
 * size, or one transparent link.  Returns 0 or EINVAL.
 */
int
bt_hci_sco_check(
	const struct bt_sco *links)
{
	unsigned index;
	unsigned other;
	unsigned transparent;
	unsigned sixteen;
	unsigned first_sixteen;

	/* Reserved words and the count. */
	if (links->reserved[0] != 0U || links->reserved[1] != 0U)
		return EINVAL;
	if (links->count > BT_SCO_LINKS_MAX)
		return EINVAL;

	/* Each link. */
	transparent = 0U;
	first_sixteen = 0U;
	for (index = 0U; index < links->count; index++) {
		/* Its handle, its length. */
		if (links->handle[index] > BT_HCI_SCO_HANDLE_MAX)
			return EINVAL;
		if (links->packet_length[index] == 0U || links->packet_length[index] > BT_SCO_DATA_MAX)
			return EINVAL;
		for (other = 0U; other < index; other++) {
			if (links->handle[other] == links->handle[index])
				return EINVAL;
		}

		/* Its coding: transparent alone, CVSD of one sample size. */
		if ((links->voice_setting[index] & BT_HCI_VOICE_AIR_MASK) == BT_HCI_VOICE_AIR_TRANSPARENT) {
			transparent++;
			continue;
		}

		/* A CVSD link's sample size, the same as the first's. */
		sixteen = (links->voice_setting[index] & BT_HCI_VOICE_SIXTEEN_BITS) != 0U;
		if (index == 0U)
			first_sixteen = sixteen;
		if (sixteen != first_sixteen)
			return EINVAL;
	}

	/* One transparent link, alone. */
	if (transparent != 0U && links->count != 1U)
		return EINVAL;

	/* Succeeded: the links can be streamed. */
	return 0;
}

/*
 * Chooses the isochronous interface's alternate setting for checked links
 * (design S3, as Linux's btusb does): none for no link, the transparent
 * alternate (6, or 1 when the interface has no 6) for a transparent link,
 * 2, 4 or 5 for one to three 16-bit CVSD links, and 1 to 3 for 8-bit ones.
 */
unsigned
bt_hci_sco_alternate(
	const struct bt_sco *links,
	int transparent_alternate)
{
	static const unsigned sixteen_bits[BT_SCO_LINKS_MAX] = { 2U, 4U, 5U };

	/* No link: the alternate without bandwidth. */
	if (links->count == 0U)
		return 0U;

	/* A transparent link. */
	if ((links->voice_setting[0] & BT_HCI_VOICE_AIR_MASK) == BT_HCI_VOICE_AIR_TRANSPARENT) {
		if (transparent_alternate)
			return 6U;
		return 1U;
	}

	/* 16-bit CVSD links. */
	if ((links->voice_setting[0] & BT_HCI_VOICE_SIXTEEN_BITS) != 0U)
		return sixteen_bits[links->count - 1U];

	/* Succeeded: 8-bit CVSD links, one alternate a link. */
	return links->count;
}

/* Starts an SCO assembler for the links streamed, with nothing gathered. */
void
bt_hci_sco_assembler_init(
	struct bt_hci_sco_assembler *assembler,
	const struct bt_sco *links)
{
	unsigned index;

	/* The links' handles and lengths. */
	assembler->count = links->count;
	for (index = 0U; index < BT_SCO_LINKS_MAX; index++) {
		assembler->handle[index] = 0U;
		assembler->length[index] = 0U;
		if (index < links->count) {
			assembler->handle[index] = links->handle[index];
			assembler->length[index] = links->packet_length[index];
		}
	}

	/* Nothing gathered, nothing lost. */
	assembler->have = 0U;
	assembler->total = 0U;
	assembler->searching = 0U;
	assembler->resyncs = 0U;
}

/* Drops the packet being gathered at a gap in the bytes (a missed or failed isochronous packet). */
void
bt_hci_sco_gap(
	struct bt_hci_sco_assembler *assembler)
{
	/* A packet cut short is lost. */
	if (assembler->have != 0U && !assembler->searching)
		assembler->resyncs++;

	/* The next header starts afresh. */
	assembler->have = 0U;
	assembler->total = 0U;
}

/*
 * Feeds isochronous IN bytes: each SCO packet put together goes to deliver
 * (header first).  Bytes that start no fitting header are passed by, one
 * at a time, until one does.
 */
void
bt_hci_sco_feed(
	struct bt_hci_sco_assembler *assembler,
	const uint8_t *data,
	size_t size,
	bt_hci_sco_fn deliver,
	void *context)
{
	size_t index;
	int fits;

	/* Byte by byte (17 to 63 a millisecond). */
	for (index = 0U; index < size; index++) {
		assembler->buffer[assembler->have] = data[index];
		assembler->have++;

		/* A header complete: a streamed handle with its length, or the next byte tried as its start. */
		if (assembler->total == 0U && assembler->have == BT_SCO_HEADER) {
			fits = sco_header_fits(assembler);
			if (!fits) {
				if (!assembler->searching)
					assembler->resyncs++;
				assembler->searching = 1U;
				assembler->buffer[0] = assembler->buffer[1];
				assembler->buffer[1] = assembler->buffer[2];
				assembler->have = 2U;
				continue;
			}

			/* In step again. */
			assembler->searching = 0U;
			assembler->total = BT_SCO_HEADER + (size_t)assembler->buffer[2];
		}

		/* A whole packet. */
		if (assembler->total != 0U && assembler->have == assembler->total) {
			deliver(context, assembler->buffer, assembler->total);
			assembler->have = 0U;
			assembler->total = 0U;
		}
	}
}

/* Tells whether a gathered header names a streamed link with that link's packet length. */
static int
sco_header_fits(
	const struct bt_hci_sco_assembler *assembler)
{
	unsigned handle;
	unsigned index;

	/* The handle without the Packet Status Flag. */
	handle = ((unsigned)assembler->buffer[0] | ((unsigned)assembler->buffer[1] << 8)) & BT_HCI_SCO_HANDLE_MASK;

	/* One of the links, with its length. */
	for (index = 0U; index < assembler->count; index++) {
		if (assembler->handle[index] == handle && assembler->length[index] == assembler->buffer[2])
			return 1;
	}

	/* Not a header of ours. */
	return 0;
}

/* Prepares an empty queue of bytes over capacity bytes. */
void
bt_hci_fifo_init(
	struct bt_hci_fifo *fifo,
	uint8_t *bytes,
	size_t capacity)
{
	/* Empty. */
	fifo->bytes = bytes;
	fifo->capacity = capacity;
	fifo->head = 0U;
	fifo->used = 0U;
}

/* Tells how many bytes the queue takes now. */
size_t
bt_hci_fifo_room(
	const struct bt_hci_fifo *fifo)
{
	/* The free bytes. */
	return fifo->capacity - fifo->used;
}

/* Queues bytes whole; 0, or EAGAIN when they do not fit. */
int
bt_hci_fifo_put(
	struct bt_hci_fifo *fifo,
	const uint8_t *data,
	size_t size)
{
	size_t tail;
	size_t index;

	/* Whole or not at all. */
	if (size > fifo->capacity - fifo->used)
		return EAGAIN;

	/* After the last byte, around the end. */
	tail = fifo->head + fifo->used;
	for (index = 0U; index < size; index++)
		fifo->bytes[(tail + index) % fifo->capacity] = data[index];
	fifo->used += size;

	/* Succeeded: queued. */
	return 0;
}

/* Takes up to size bytes, oldest first; returns how many. */
size_t
bt_hci_fifo_get(
	struct bt_hci_fifo *fifo,
	uint8_t *target,
	size_t size)
{
	size_t index;

	/* At most what is there. */
	if (size > fifo->used)
		size = fifo->used;
	for (index = 0U; index < size; index++)
		target[index] = fifo->bytes[(fifo->head + index) % fifo->capacity];

	/* They leave the queue. */
	fifo->head = (fifo->head + size) % fifo->capacity;
	fifo->used -= size;

	/* Succeeded: the count. */
	return size;
}

/* Empties the queue. */
void
bt_hci_fifo_clear(
	struct bt_hci_fifo *fifo)
{
	/* Nothing queued. */
	fifo->head = 0U;
	fifo->used = 0U;
}
