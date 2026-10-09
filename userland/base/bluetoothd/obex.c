/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * bluetoothd's OBEX (ws197-p002, see obex.h).
 *
 * A packet is its opcode or response code, its length (two bytes,
 * big-endian, the whole packet) and its headers; Connect puts the version,
 * the flags and the largest packet before its headers, SetPath its flags
 * and constants.  A header's id says its encoding: Unicode text and byte
 * sequences carry a two-byte length that counts the id and the length
 * themselves, the others are one or four bytes.  IrOBEX is not public; the
 * values are those MAP and PBAP name and the peers' traces show
 * (plan/ws197/phase002/phase.md section 0).
 */

#include "userland/base/bluetoothd/obex.h"

#include <errno.h>
#include <string.h>

/* The bytes before Connect's headers (opcode, length, version, flags, largest packet), and before SetPath's. */
#define OBEX_CONNECT_PREFIX		7U
#define OBEX_SETPATH_PREFIX		5U
#define OBEX_PREFIX			3U

/* The bytes a Unicode or byte-sequence header adds to its value: the id and the length. */
#define OBEX_HEADER_OVERHEAD		3U

static void obex_begin(struct btd_obex_writer *writer, uint8_t *bytes, size_t size, uint8_t opcode);
static void obex_raw(struct btd_obex_writer *writer, const uint8_t *data, size_t length);
static size_t obex_finish(struct btd_obex_writer *writer);
static void obex_put16(uint8_t *bytes, size_t value);
static size_t obex_get16(const uint8_t *bytes);
static void obex_connection_id(struct btd_obex *ob, struct btd_obex_writer *writer);
static int obex_send(struct btd_obex *ob, size_t length, uint64_t now);
static void obex_packet(struct btd_obex *ob, const uint8_t *packet, size_t length, uint64_t now);
static void obex_client(struct btd_obex *ob, const uint8_t *packet, size_t length, uint64_t now);
static void obex_client_connect(struct btd_obex *ob, const uint8_t *packet, size_t length);
static void obex_client_get(struct btd_obex *ob, uint8_t code, const uint8_t *headers, size_t length, uint64_t now);
static void obex_client_put(struct btd_obex *ob, uint8_t code, const uint8_t *headers, size_t length, uint64_t now);
static int obex_put_next(struct btd_obex *ob, const uint8_t *headers, size_t length, uint64_t now);
static int obex_send_abort(struct btd_obex *ob, uint64_t now);
static void obex_end(struct btd_obex *ob, int error, uint8_t code, const uint8_t *headers, size_t length);
static void obex_server(struct btd_obex *ob, const uint8_t *packet, size_t length);
static void obex_server_connect(struct btd_obex *ob, const uint8_t *packet, size_t length);
static void obex_server_put(struct btd_obex *ob, const uint8_t *packet, size_t length);
static void obex_respond(struct btd_obex *ob, uint8_t code);
static void obex_break(struct btd_obex *ob, int error);

/*
 * Starts writing headers into a buffer (no packet prefix: for the headers
 * a caller hands to an operation).
 */
void
btd_obex_writer_init(
	struct btd_obex_writer *writer,
	uint8_t *bytes,
	size_t size)
{
	/* Empty, nothing lost yet. */
	writer->bytes = bytes;
	writer->size = size;
	writer->used = 0U;
	writer->overflow = 0;
}

/*
 * Writes a byte-sequence header (its id, its length with the three bytes
 * of the id and the length, its value); one that does not fit marks the
 * writer.
 */
void
btd_obex_put_bytes(
	struct btd_obex_writer *writer,
	uint8_t id,
	const uint8_t *data,
	size_t length)
{
	uint8_t head[OBEX_HEADER_OVERHEAD];

	/* A length the two bytes hold. */
	if (length > 0xffffU - OBEX_HEADER_OVERHEAD) {
		writer->overflow = 1;
		return;
	}

	/* The id and the length, then the value. */
	head[0] = id;
	obex_put16(head + 1, length + OBEX_HEADER_OVERHEAD);
	obex_raw(writer, head, sizeof(head));
	obex_raw(writer, data, length);
}

/*
 * Writes a Unicode header from UTF-8 text: UTF-16BE (a character beyond
 * the BMP as a surrogate pair) with its NUL; empty text is the empty
 * header of three bytes (SetPath's root, a listing of the present folder).
 * Text that is not UTF-8 marks the writer.
 */
void
btd_obex_put_text(
	struct btd_obex_writer *writer,
	uint8_t id,
	const char *text)
{
	uint8_t head[OBEX_HEADER_OVERHEAD];
	uint8_t unit[4];
	const unsigned char *at;
	size_t start;
	unsigned code;
	unsigned more;
	unsigned index;
	size_t length;

	/* The id and a length written once the text's size is known. */
	start = writer->used;
	head[0] = id;
	head[1] = 0U;
	head[2] = 0U;
	obex_raw(writer, head, sizeof(head));

	/* Each character, decoded from UTF-8 and written as one or two UTF-16 units. */
	at = (const unsigned char *)text;
	while (*at != 0U) {
		/* The lead byte and how many continuation bytes follow. */
		if (*at < 0x80U) {
			code = *at;
			more = 0U;
		} else if ((*at & 0xe0U) == 0xc0U) {
			code = *at & 0x1fU;
			more = 1U;
		} else if ((*at & 0xf0U) == 0xe0U) {
			code = *at & 0x0fU;
			more = 2U;
		} else if ((*at & 0xf8U) == 0xf0U) {
			code = *at & 0x07U;
			more = 3U;
		} else {
			writer->overflow = 1;
			return;
		}

		/* Past the lead byte. */
		at++;

		/* The continuation bytes. */
		for (index = 0U; index < more; index++) {
			if ((*at & 0xc0U) != 0x80U) {
				writer->overflow = 1;
				return;
			}

			/* Six more bits of the character. */
			code = (code << 6) | (*at & 0x3fU);
			at++;
		}

		/* One unit for the BMP, a surrogate pair beyond it. */
		if (code < 0x10000U) {
			unit[0] = (uint8_t)(code >> 8);
			unit[1] = (uint8_t)(code & 0xffU);
			obex_raw(writer, unit, 2U);
		} else {
			code -= 0x10000U;
			unit[0] = (uint8_t)(0xd8U | ((code >> 18) & 0x03U));
			unit[1] = (uint8_t)((code >> 10) & 0xffU);
			unit[2] = (uint8_t)(0xdcU | ((code >> 8) & 0x03U));
			unit[3] = (uint8_t)(code & 0xffU);
			obex_raw(writer, unit, 4U);
		}
	}

	/* A text that is not empty ends with its NUL. */
	if (writer->used != start + OBEX_HEADER_OVERHEAD) {
		unit[0] = 0U;
		unit[1] = 0U;
		obex_raw(writer, unit, 2U);
	}

	/* Succeeded: the length written back, when it all fitted. */
	if (writer->overflow)
		return;
	length = writer->used - start;
	obex_put16(writer->bytes + start + 1U, length);
}

/*
 * Writes a one-byte header.
 */
void
btd_obex_put_byte(
	struct btd_obex_writer *writer,
	uint8_t id,
	uint8_t value)
{
	uint8_t bytes[2];

	/* The id and the value. */
	bytes[0] = id;
	bytes[1] = value;
	obex_raw(writer, bytes, sizeof(bytes));
}

/*
 * Writes a four-byte header (big-endian).
 */
void
btd_obex_put_quad(
	struct btd_obex_writer *writer,
	uint8_t id,
	uint32_t value)
{
	uint8_t bytes[5];

	/* The id and the value. */
	bytes[0] = id;
	bytes[1] = (uint8_t)(value >> 24);
	bytes[2] = (uint8_t)(value >> 16);
	bytes[3] = (uint8_t)(value >> 8);
	bytes[4] = (uint8_t)value;
	obex_raw(writer, bytes, sizeof(bytes));
}

/*
 * Reads the header at an offset of a packet's headers and moves the offset
 * past it.  A Unicode value must be of an even length and end with its NUL
 * (the empty one excepted).  Returns 1 for a header, 0 at the end, -1 for
 * a header that runs past the headers or is malformed.
 */
int
btd_obex_header_next(
	const uint8_t *headers,
	size_t length,
	size_t *offset,
	struct btd_obex_header *header)
{
	size_t left;
	size_t size;
	uint8_t encoding;

	/* The end. */
	if (*offset >= length)
		return 0;
	left = length - *offset;
	memset(header, 0, sizeof(*header));
	header->id = headers[*offset];
	encoding = (uint8_t)(header->id & BTD_OBEX_ENCODING_MASK);

	/* One byte. */
	if (encoding == BTD_OBEX_ENCODING_BYTE) {
		if (left < 2U)
			return -1;
		header->value = headers[*offset + 1U];
		header->data = headers + *offset + 1U;
		header->length = 1U;
		*offset += 2U;
		return 1;
	}

	/* Four bytes, big-endian. */
	if (encoding == BTD_OBEX_ENCODING_QUAD) {
		if (left < 5U)
			return -1;
		header->data = headers + *offset + 1U;
		header->length = 4U;
		header->value = ((uint32_t)header->data[0] << 24) |
		    ((uint32_t)header->data[1] << 16) |
		    ((uint32_t)header->data[2] << 8) |
		    (uint32_t)header->data[3];
		*offset += 5U;
		return 1;
	}

	/* A length that counts the id and itself, within the headers. */
	if (left < OBEX_HEADER_OVERHEAD)
		return -1;
	size = obex_get16(headers + *offset + 1U);
	if (size < OBEX_HEADER_OVERHEAD || size > left)
		return -1;
	header->data = headers + *offset + OBEX_HEADER_OVERHEAD;
	header->length = size - OBEX_HEADER_OVERHEAD;

	/* Unicode: pairs of bytes ending with a NUL, or empty. */
	if (encoding == BTD_OBEX_ENCODING_UNICODE && header->length != 0U) {
		if ((header->length & 1U) != 0U)
			return -1;
		if (header->data[header->length - 2U] != 0U || header->data[header->length - 1U] != 0U)
			return -1;
	}

	/* Succeeded: a header read. */
	*offset += size;
	return 1;
}

/*
 * Finds the first header of an id in a packet's headers.  Returns 1 with
 * it, 0 when there is none, -1 for malformed headers before it.
 */
int
btd_obex_find(
	const uint8_t *headers,
	size_t length,
	uint8_t id,
	struct btd_obex_header *header)
{
	size_t offset;
	int read;

	/* Each header in turn. */
	offset = 0U;
	for (;;) {
		read = btd_obex_header_next(headers, length, &offset, header);
		if (read <= 0)
			return read;

		/* Succeeded: the one asked for. */
		if (header->id == id)
			return 1;
	}
}

/*
 * Empties a connection of a role over a DLC just opened.
 */
void
btd_obex_init(
	struct btd_obex *ob,
	const struct btd_obex_events *events,
	unsigned role)
{
	/* Nothing sent, nothing received, not connected; a server's first Connection ID is 1. */
	memset(ob, 0, sizeof(*ob));
	ob->events = *events;
	ob->role = role;
	ob->state = BTD_OBEX_IDLE;
	ob->next_connection_id = 1U;
}

/*
 * Connects as a client: Connect with OBEX 1.0, no flags, the largest
 * packet bluetoothd takes, and a Target when given (16 bytes for MAP and
 * PBAP).  done tells the answer.  Returns 0, EBUSY when not idle, or
 * EMSGSIZE.
 */
int
btd_obex_connect(
	struct btd_obex *ob,
	const uint8_t *target,
	size_t target_length,
	uint64_t now)
{
	struct btd_obex_writer writer;
	uint8_t prefix[4];
	size_t length;
	int error;

	/* A client not connected. */
	if (ob->role != BTD_OBEX_CLIENT || ob->state != BTD_OBEX_IDLE)
		return EBUSY;

	/* The version, the flags, the largest packet, then the Target. */
	obex_begin(&writer, ob->tx, sizeof(ob->tx), BTD_OBEX_CONNECT);
	prefix[0] = BTD_OBEX_VERSION;
	prefix[1] = 0x00U;
	obex_put16(prefix + 2, BTD_OBEX_PACKET_MAX);
	obex_raw(&writer, prefix, sizeof(prefix));
	if (target != NULL)
		btd_obex_put_bytes(&writer, BTD_OBEX_TARGET, target, target_length);
	length = obex_finish(&writer);
	if (length == 0U)
		return EMSGSIZE;

	/* Sent, the answer awaited. */
	ob->operation = BTD_OBEX_OP_CONNECT;
	ob->state = BTD_OBEX_CONNECTING;
	error = obex_send(ob, length, now);
	if (error != 0)
		return error;

	/* Succeeded: connecting. */
	return 0;
}

/*
 * Gets an object: one Get with the Final bit, the Connection ID first,
 * then the caller's headers (Name, Type, Application Parameters); each
 * Continue is followed by another Get until Success.  The body goes to the
 * body hook, at most body_limit bytes (more aborts the Get with
 * EMSGSIZE).  Returns 0, EBUSY when not connected or busy, or EMSGSIZE for
 * headers longer than the peer's packet.
 */
int
btd_obex_get(
	struct btd_obex *ob,
	const uint8_t *headers,
	size_t length,
	size_t body_limit,
	uint64_t now)
{
	struct btd_obex_writer writer;
	size_t packet;
	int error;

	/* A client connected and idle. */
	if (ob->role != BTD_OBEX_CLIENT || ob->state != BTD_OBEX_CONNECTED)
		return EBUSY;

	/* Get with the Final bit: the request's headers are all in this packet. */
	obex_begin(&writer, ob->tx, sizeof(ob->tx), (uint8_t)(BTD_OBEX_GET | BTD_OBEX_FINAL));
	obex_connection_id(ob, &writer);
	obex_raw(&writer, headers, length);
	packet = obex_finish(&writer);
	if (packet == 0U || packet > ob->peer_max)
		return EMSGSIZE;

	/* The body counted from nothing. */
	ob->operation = BTD_OBEX_OP_GET;
	ob->body_limit = body_limit;
	ob->body_count = 0U;
	ob->state = BTD_OBEX_BUSY;

	/* Sent. */
	error = obex_send(ob, packet, now);
	if (error != 0)
		return error;

	/* Succeeded: the answers come to body and done. */
	return 0;
}

/*
 * Puts an object: the Connection ID and the caller's headers in the first
 * packet, the body in Body headers cut to the peer's packet, and End of
 * Body in the last packet, which has the Final bit.  A NULL body is a Put
 * without a body (a delete); an empty body still sends an empty End of
 * Body.  The body stays the caller's until done.  Returns 0, EBUSY, or
 * EMSGSIZE.
 */
int
btd_obex_put(
	struct btd_obex *ob,
	const uint8_t *headers,
	size_t length,
	const uint8_t *body,
	size_t body_length,
	uint64_t now)
{
	int error;

	/* A client connected and idle. */
	if (ob->role != BTD_OBEX_CLIENT || ob->state != BTD_OBEX_CONNECTED)
		return EBUSY;

	/* The body to send, none of it sent yet. */
	ob->operation = BTD_OBEX_OP_PUT;
	ob->put_body = body;
	ob->put_length = body_length;
	ob->put_sent = 0U;
	ob->state = BTD_OBEX_BUSY;

	/* The first packet. */
	error = obex_put_next(ob, headers, length, now);
	if (error != 0) {
		ob->state = BTD_OBEX_CONNECTED;
		ob->operation = BTD_OBEX_OP_NONE;
		return error;
	}

	/* Succeeded: the answers come to done. */
	return 0;
}

/*
 * Sets the folder: SetPath with its flags and constants 0, the Connection
 * ID, then the caller's headers (a Name, or the empty Name for the root).
 * Returns 0, EBUSY, or EMSGSIZE.
 */
int
btd_obex_setpath(
	struct btd_obex *ob,
	uint8_t flags,
	const uint8_t *headers,
	size_t length,
	uint64_t now)
{
	struct btd_obex_writer writer;
	uint8_t prefix[2];
	size_t packet;
	int error;

	/* A client connected and idle. */
	if (ob->role != BTD_OBEX_CLIENT || ob->state != BTD_OBEX_CONNECTED)
		return EBUSY;

	/* The flags and the constants, the Connection ID, the caller's headers. */
	obex_begin(&writer, ob->tx, sizeof(ob->tx), BTD_OBEX_SETPATH);
	prefix[0] = flags;
	prefix[1] = 0x00U;
	obex_raw(&writer, prefix, sizeof(prefix));
	obex_connection_id(ob, &writer);
	obex_raw(&writer, headers, length);
	packet = obex_finish(&writer);
	if (packet == 0U || packet > ob->peer_max)
		return EMSGSIZE;

	/* Sent. */
	ob->operation = BTD_OBEX_OP_SETPATH;
	ob->state = BTD_OBEX_BUSY;
	error = obex_send(ob, packet, now);
	if (error != 0)
		return error;

	/* Succeeded: the answer comes to done. */
	return 0;
}

/*
 * Asks for the running Get or Put to be aborted: Abort goes at the client's
 * next turn (when the peer's answer comes), and done tells ECANCELED.
 * Returns 0, or EALREADY when no operation runs.
 */
int
btd_obex_abort(
	struct btd_obex *ob)
{
	/* An operation that can be aborted. */
	if (ob->state != BTD_OBEX_BUSY)
		return EALREADY;
	if (ob->operation != BTD_OBEX_OP_GET && ob->operation != BTD_OBEX_OP_PUT)
		return EALREADY;

	/* Succeeded: at the next turn. */
	ob->abort_wanted = 1;
	if (ob->abort_error == 0)
		ob->abort_error = ECANCELED;
	return 0;
}

/*
 * Disconnects a client: Disconnect with the Connection ID; done tells the
 * answer.  Returns 0, EBUSY when not connected and idle.
 */
int
btd_obex_disconnect(
	struct btd_obex *ob,
	uint64_t now)
{
	struct btd_obex_writer writer;
	size_t packet;
	int error;

	/* A client connected and idle. */
	if (ob->role != BTD_OBEX_CLIENT || ob->state != BTD_OBEX_CONNECTED)
		return EBUSY;

	/* Disconnect with the Connection ID. */
	obex_begin(&writer, ob->tx, sizeof(ob->tx), BTD_OBEX_DISCONNECT);
	obex_connection_id(ob, &writer);
	packet = obex_finish(&writer);
	if (packet == 0U)
		return EMSGSIZE;

	/* Sent. */
	ob->operation = BTD_OBEX_OP_DISCONNECT;
	ob->state = BTD_OBEX_DISCONNECTING;
	error = obex_send(ob, packet, now);
	if (error != 0)
		return error;

	/* Succeeded: disconnecting. */
	return 0;
}

/*
 * Takes bytes of the DLC: they are added to the packet being received, and
 * each whole packet is handled.  A packet whose length is below 3 or above
 * what bluetoothd takes breaks the connection (the owner closes the DLC).
 */
void
btd_obex_input(
	struct btd_obex *ob,
	const uint8_t *data,
	size_t length,
	uint64_t now)
{
	size_t take;
	size_t packet;

	/* Each byte, gathered into whole packets. */
	while (length != 0U && ob->state != BTD_OBEX_BROKEN) {
		/* As much as the packet buffer takes. */
		take = sizeof(ob->rx) - ob->rx_used;
		if (take > length)
			take = length;
		memcpy(ob->rx + ob->rx_used, data, take);
		ob->rx_used += take;
		data += take;
		length -= take;

		/* Each whole packet in the buffer. */
		while (ob->rx_used >= OBEX_PREFIX && ob->state != BTD_OBEX_BROKEN) {
			/* The packet's length, within bounds. */
			packet = obex_get16(ob->rx + 1);
			if (packet < OBEX_PREFIX || packet > sizeof(ob->rx)) {
				ob->malformed++;
				obex_break(ob, EPROTO);
				return;
			}

			/* Not all here yet. */
			if (ob->rx_used < packet)
				break;

			/* Handled (a packet that broke the connection leaves nothing to keep), then the bytes after it move up. */
			obex_packet(ob, ob->rx, packet, now);
			if (ob->state == BTD_OBEX_BROKEN)
				return;
			ob->rx_used -= packet;
			memmove(ob->rx, ob->rx + packet, ob->rx_used);
		}
	}
}

/*
 * Writes what of the packet being sent did not go yet (the owner calls it
 * when the DLC is writable again).
 */
void
btd_obex_pump(
	struct btd_obex *ob)
{
	size_t written;
	int error;

	/* What is left, as far as the DLC takes it. */
	while (ob->tx_sent < ob->tx_used) {
		written = 0U;
		error = ob->events.write(ob->events.context, ob->tx + ob->tx_sent, ob->tx_used - ob->tx_sent, &written);
		if (error != 0) {
			obex_break(ob, error);
			return;
		}

		/* Nothing taken: the DLC's credits ran out. */
		if (written == 0U)
			return;
		ob->tx_sent += written;
	}
}

/*
 * Breaks a client's connection whose answer did not come in time (done
 * tells ETIMEDOUT; the owner closes the DLC).
 */
void
btd_obex_tick(
	struct btd_obex *ob,
	uint64_t now)
{
	/* An answer awaited past its time. */
	if (ob->deadline == 0U || now < ob->deadline)
		return;

	/* Succeeded: broken. */
	ob->deadline = 0U;
	obex_break(ob, ETIMEDOUT);
}

/*
 * Tells when the answer awaited is due (0: none), for the daemon's poll.
 */
uint64_t
btd_obex_deadline(
	const struct btd_obex *ob)
{
	/* The one timer. */
	return ob->deadline;
}

/* Starts a packet: its opcode or response code, and two bytes of length written by obex_finish. */
static void
obex_begin(
	struct btd_obex_writer *writer,
	uint8_t *bytes,
	size_t size,
	uint8_t opcode)
{
	uint8_t head[OBEX_PREFIX];

	/* The opcode and a length to come. */
	btd_obex_writer_init(writer, bytes, size);
	head[0] = opcode;
	head[1] = 0U;
	head[2] = 0U;
	obex_raw(writer, head, sizeof(head));
}

/* Appends bytes to a writer, or marks it when they do not fit. */
static void
obex_raw(
	struct btd_obex_writer *writer,
	const uint8_t *data,
	size_t length)
{
	/* No room: nothing more is written. */
	if (writer->overflow || length > writer->size - writer->used) {
		writer->overflow = 1;
		return;
	}

	/* Succeeded: appended. */
	if (length != 0U)
		memcpy(writer->bytes + writer->used, data, length);
	writer->used += length;
}

/* Writes a packet's length into its prefix; returns the length, or 0 when something did not fit. */
static size_t
obex_finish(
	struct btd_obex_writer *writer)
{
	/* Something did not fit, or the length does not fit its two bytes. */
	if (writer->overflow || writer->used > 0xffffU)
		return 0U;

	/* Succeeded: the length written. */
	obex_put16(writer->bytes + 1, writer->used);
	return writer->used;
}

/* Writes a 16-bit value, most significant byte first. */
static void
obex_put16(
	uint8_t *bytes,
	size_t value)
{
	/* Big-endian. */
	bytes[0] = (uint8_t)((value >> 8) & 0xffU);
	bytes[1] = (uint8_t)(value & 0xffU);
}

/* Reads a 16-bit value, most significant byte first. */
static size_t
obex_get16(
	const uint8_t *bytes)
{
	/* Big-endian. */
	return ((size_t)bytes[0] << 8) | (size_t)bytes[1];
}

/* Writes the Connection ID header when the server gave one (it is a request's first header). */
static void
obex_connection_id(
	struct btd_obex *ob,
	struct btd_obex_writer *writer)
{
	/* None given. */
	if (!ob->have_connection_id)
		return;

	/* Succeeded: written. */
	btd_obex_put_quad(writer, BTD_OBEX_CONNECTION_ID, ob->connection_id);
}

/* Sends the packet written in tx (a client's request starts its timer); returns 0 or the write hook's error. */
static int
obex_send(
	struct btd_obex *ob,
	size_t length,
	uint64_t now)
{
	/* The whole packet to go. */
	ob->tx_used = length;
	ob->tx_sent = 0U;

	/* A client waits for the answer within its time. */
	if (ob->role == BTD_OBEX_CLIENT)
		ob->deadline = now + BTD_OBEX_TIMEOUT_MS;

	/* As much as the DLC takes now. */
	btd_obex_pump(ob);
	if (ob->state == BTD_OBEX_BROKEN)
		return EIO;

	/* Succeeded: sent, or going as the DLC allows. */
	return 0;
}

/* Gives a whole packet to the role's handler. */
static void
obex_packet(
	struct btd_obex *ob,
	const uint8_t *packet,
	size_t length,
	uint64_t now)
{
	/* A client takes answers, a server requests. */
	if (ob->role == BTD_OBEX_CLIENT) {
		obex_client(ob, packet, length, now);
	} else {
		obex_server(ob, packet, length);
	}
}

/* Takes an answer of the peer to the client's request in flight. */
static void
obex_client(
	struct btd_obex *ob,
	const uint8_t *packet,
	size_t length,
	uint64_t now)
{
	uint8_t code;
	const uint8_t *headers;
	size_t headers_length;
	int error;

	/* An answer nobody waits for. */
	if (ob->state != BTD_OBEX_CONNECTING &&
	    ob->state != BTD_OBEX_BUSY &&
	    ob->state != BTD_OBEX_DISCONNECTING) {
		ob->malformed++;
		return;
	}

	/* The answer came in time. */
	ob->deadline = 0U;
	code = packet[0];

	/* Connect's answer has its own prefix. */
	if (ob->state == BTD_OBEX_CONNECTING) {
		obex_client_connect(ob, packet, length);
		return;
	}

	/* The headers after the answer's prefix. */
	headers = packet + OBEX_PREFIX;
	headers_length = length - OBEX_PREFIX;

	/* Disconnect's answer: not connected any more, whatever it says. */
	if (ob->state == BTD_OBEX_DISCONNECTING) {
		ob->state = BTD_OBEX_IDLE;
		ob->have_connection_id = 0;
		error = 0;
		if (code != BTD_OBEX_SUCCESS)
			error = EIO;
		obex_end(ob, error, code, headers, headers_length);
		return;
	}

	/* Abort's answer ends the operation it aborted. */
	if (ob->aborting) {
		ob->state = BTD_OBEX_CONNECTED;
		obex_end(ob, ob->abort_error, code, headers, headers_length);
		return;
	}

	/* Each operation's answer. */
	if (ob->operation == BTD_OBEX_OP_GET) {
		obex_client_get(ob, code, headers, headers_length, now);
	} else if (ob->operation == BTD_OBEX_OP_PUT) {
		obex_client_put(ob, code, headers, headers_length, now);
	} else {
		/* SetPath's: done either way. */
		ob->state = BTD_OBEX_CONNECTED;
		error = 0;
		if (code != BTD_OBEX_SUCCESS)
			error = EIO;
		obex_end(ob, error, code, headers, headers_length);
	}
}

/*
 * Takes Connect's answer: on Success the peer's largest packet (at least
 * 255, else the connection is refused as broken), its Connection ID and
 * Who; anything else leaves the client not connected.
 */
static void
obex_client_connect(
	struct btd_obex *ob,
	const uint8_t *packet,
	size_t length)
{
	struct btd_obex_header header;
	const uint8_t *headers;
	size_t headers_length;
	size_t offset;
	uint8_t code;
	int read;

	/* The prefix of a Connect answer. */
	if (length < OBEX_CONNECT_PREFIX) {
		ob->malformed++;
		obex_break(ob, EPROTO);
		return;
	}

	/* The answer's code and its headers. */
	code = packet[0];
	headers = packet + OBEX_CONNECT_PREFIX;
	headers_length = length - OBEX_CONNECT_PREFIX;

	/* Refused. */
	if (code != BTD_OBEX_SUCCESS) {
		ob->state = BTD_OBEX_IDLE;
		obex_end(ob, ECONNREFUSED, code, headers, headers_length);
		return;
	}

	/* The peer's largest packet: OBEX's least at least, bluetoothd's own at most for what it sends. */
	ob->peer_max = obex_get16(packet + 5);
	if (ob->peer_max < BTD_OBEX_PACKET_MIN) {
		obex_break(ob, EPROTO);
		return;
	}

	/* At most bluetoothd's own for what it sends. */
	if (ob->peer_max > BTD_OBEX_PACKET_MAX)
		ob->peer_max = BTD_OBEX_PACKET_MAX;

	/* The Connection ID and Who. */
	offset = 0U;
	for (;;) {
		read = btd_obex_header_next(headers, headers_length, &offset, &header);
		if (read == 0)
			break;
		if (read < 0) {
			ob->malformed++;
			obex_break(ob, EPROTO);
			return;
		}

		/* The two headers kept. */
		if (header.id == BTD_OBEX_CONNECTION_ID) {
			ob->have_connection_id = 1;
			ob->connection_id = header.value;
		} else if (header.id == BTD_OBEX_WHO && header.length <= sizeof(ob->who)) {
			memcpy(ob->who, header.data, header.length);
			ob->who_length = header.length;
		}
	}

	/* Succeeded: connected. */
	ob->state = BTD_OBEX_CONNECTED;
	obex_end(ob, 0, code, headers, headers_length);
}

/*
 * Takes an answer to a Get: its Body and End of Body go to the body hook
 * within the limit; Continue asks for more (or aborts), Success ends it,
 * anything else fails it.
 */
static void
obex_client_get(
	struct btd_obex *ob,
	uint8_t code,
	const uint8_t *headers,
	size_t length,
	uint64_t now)
{
	struct btd_obex_header header;
	struct btd_obex_writer writer;
	size_t offset;
	size_t packet;
	int read;
	int stop;
	int error;

	/* The body's pieces. */
	offset = 0U;
	for (;;) {
		read = btd_obex_header_next(headers, length, &offset, &header);
		if (read == 0)
			break;
		if (read < 0) {
			ob->malformed++;
			obex_break(ob, EPROTO);
			return;
		}

		/* Only the body's headers. */
		if (header.id != BTD_OBEX_BODY && header.id != BTD_OBEX_END_OF_BODY)
			continue;

		/* Past the limit: aborted. */
		if (header.length > ob->body_limit - ob->body_count) {
			ob->abort_wanted = 1;
			ob->abort_error = EMSGSIZE;
			break;
		}

		/* Counted. */
		ob->body_count += header.length;

		/* To the hook, which may stop the Get. */
		if (ob->events.body == NULL || header.length == 0U)
			continue;
		stop = ob->events.body(ob->events.context, header.data, header.length);
		if (stop != 0) {
			ob->abort_wanted = 1;
			if (ob->abort_error == 0)
				ob->abort_error = ECANCELED;
			break;
		}
	}

	/* Success ends the Get (an abort asked for comes too late: the object is whole). */
	if (code == BTD_OBEX_SUCCESS && ob->abort_error != EMSGSIZE) {
		ob->state = BTD_OBEX_CONNECTED;
		obex_end(ob, 0, code, headers, length);
		return;
	}

	/* Anything but Continue fails it. */
	if (code != BTD_OBEX_CONTINUE && code != BTD_OBEX_SUCCESS) {
		ob->state = BTD_OBEX_CONNECTED;
		obex_end(ob, EIO, code, headers, length);
		return;
	}

	/* The client's turn: Abort when asked for. */
	if (ob->abort_wanted) {
		if (code == BTD_OBEX_SUCCESS) {
			ob->state = BTD_OBEX_CONNECTED;
			obex_end(ob, ob->abort_error, code, headers, length);
			return;
		}

		/* Not after a Continue: the Abort. */
		error = obex_send_abort(ob, now);
		if (error != 0)
			obex_break(ob, error);
		return;
	}

	/* Else the next Get, with nothing but its prefix. */
	obex_begin(&writer, ob->tx, sizeof(ob->tx), (uint8_t)(BTD_OBEX_GET | BTD_OBEX_FINAL));
	packet = obex_finish(&writer);
	error = obex_send(ob, packet, now);
	if (error != 0)
		obex_break(ob, error);
}

/* Takes an answer to a Put packet: Continue sends the next packet (or Abort), Success ends it, anything else fails it. */
static void
obex_client_put(
	struct btd_obex *ob,
	uint8_t code,
	const uint8_t *headers,
	size_t length,
	uint64_t now)
{
	int error;
	int finished;

	/* Whether the last packet went. */
	finished = 0;
	if (ob->put_body == NULL || ob->put_sent > ob->put_length)
		finished = 1;

	/* Success ends it. */
	if (code == BTD_OBEX_SUCCESS) {
		ob->state = BTD_OBEX_CONNECTED;
		error = 0;
		if (!finished)
			error = EPROTO;
		obex_end(ob, error, code, headers, length);
		return;
	}

	/* Anything but Continue, or Continue after the last packet, fails it. */
	if (code != BTD_OBEX_CONTINUE || finished) {
		ob->state = BTD_OBEX_CONNECTED;
		obex_end(ob, EIO, code, headers, length);
		return;
	}

	/* The client's turn: Abort when asked for. */
	if (ob->abort_wanted) {
		error = obex_send_abort(ob, now);
		if (error != 0)
			obex_break(ob, error);
		return;
	}

	/* Else the next packet. */
	error = obex_put_next(ob, NULL, 0U, now);
	if (error != 0)
		obex_break(ob, error);
}

/*
 * Writes and sends a Put's next packet: the Connection ID and the caller's
 * headers in the first, then as much body as the peer's packet takes; the
 * last part goes in End of Body with the Final bit.  put_sent beyond
 * put_length marks the last packet sent.
 */
static int
obex_put_next(
	struct btd_obex *ob,
	const uint8_t *headers,
	size_t length,
	uint64_t now)
{
	struct btd_obex_writer writer;
	size_t room;
	size_t left;
	size_t packet;
	int error;

	/* Put, its Final bit decided below. */
	obex_begin(&writer, ob->tx, sizeof(ob->tx), BTD_OBEX_PUT);
	if (ob->put_sent == 0U) {
		obex_connection_id(ob, &writer);
		obex_raw(&writer, headers, length);
	}

	/* The headers within the peer's packet. */
	if (writer.overflow || writer.used > ob->peer_max)
		return EMSGSIZE;

	/* A Put without a body (a delete): the request is whole. */
	if (ob->put_body == NULL) {
		ob->tx[0] = (uint8_t)(BTD_OBEX_PUT | BTD_OBEX_FINAL);
		ob->put_sent = 1U;
	} else {
		/* What is left of the body and what this packet takes of it. */
		left = ob->put_length - ob->put_sent;
		room = 0U;
		if (ob->peer_max > writer.used + OBEX_HEADER_OVERHEAD)
			room = ob->peer_max - writer.used - OBEX_HEADER_OVERHEAD;

		/* The first packet's headers leave no room for the body: the caller's headers are too long. */
		if (left > room && room == 0U)
			return EMSGSIZE;

		/* The rest fits: End of Body and the Final bit; else a Body as large as fits. */
		if (left <= room) {
			btd_obex_put_bytes(&writer, BTD_OBEX_END_OF_BODY, ob->put_body + ob->put_sent, left);
			ob->tx[0] = (uint8_t)(BTD_OBEX_PUT | BTD_OBEX_FINAL);
			ob->put_sent = ob->put_length + 1U;
		} else {
			btd_obex_put_bytes(&writer, BTD_OBEX_BODY, ob->put_body + ob->put_sent, room);
			ob->put_sent += room;
		}
	}

	/* The packet. */
	packet = obex_finish(&writer);
	if (packet == 0U)
		return EMSGSIZE;

	/* Sent. */
	error = obex_send(ob, packet, now);
	if (error != 0)
		return error;

	/* Succeeded: sent. */
	return 0;
}

/* Sends Abort with the Connection ID; its answer ends the aborted operation. */
static int
obex_send_abort(
	struct btd_obex *ob,
	uint64_t now)
{
	struct btd_obex_writer writer;
	size_t packet;
	int error;

	/* Abort. */
	obex_begin(&writer, ob->tx, sizeof(ob->tx), BTD_OBEX_ABORT);
	obex_connection_id(ob, &writer);
	packet = obex_finish(&writer);
	ob->abort_wanted = 0;
	ob->aborting = 1;

	/* Sent. */
	error = obex_send(ob, packet, now);
	if (error != 0)
		return error;

	/* Succeeded: the answer awaited. */
	return 0;
}

/* Ends the client's operation and tells the owner. */
static void
obex_end(
	struct btd_obex *ob,
	int error,
	uint8_t code,
	const uint8_t *headers,
	size_t length)
{
	unsigned operation;

	/* The operation over, nothing pending. */
	operation = ob->operation;
	ob->operation = BTD_OBEX_OP_NONE;
	ob->abort_wanted = 0;
	ob->aborting = 0;
	ob->abort_error = 0;
	ob->put_body = NULL;
	ob->deadline = 0U;

	/* Succeeded: the owner told. */
	if (ob->events.done != NULL)
		ob->events.done(ob->events.context, operation, error, code, headers, length);
}

/* Takes a request of the peer to the server: Connect, Disconnect, Put and Abort are served; anything else is not implemented. */
static void
obex_server(
	struct btd_obex *ob,
	const uint8_t *packet,
	size_t length)
{
	uint8_t opcode;

	/* Each request. */
	opcode = packet[0];
	switch (opcode) {
	case BTD_OBEX_CONNECT:
		obex_server_connect(ob, packet, length);
		break;
	case BTD_OBEX_DISCONNECT:
		ob->state = BTD_OBEX_IDLE;
		ob->server_put = 0;
		obex_respond(ob, BTD_OBEX_SUCCESS);
		break;
	case BTD_OBEX_PUT:
	case BTD_OBEX_PUT | BTD_OBEX_FINAL:
		obex_server_put(ob, packet, length);
		break;
	case BTD_OBEX_ABORT:
		ob->server_put = 0;
		obex_respond(ob, BTD_OBEX_SUCCESS);
		break;
	default:
		obex_respond(ob, BTD_OBEX_NOT_IMPLEMENTED);
		break;
	}
}

/*
 * Takes the peer's Connect: the owner decides on its Target; served, the
 * answer gives bluetoothd's largest packet, a new Connection ID and Who
 * (the Target).  A Connect with an authentication challenge is refused
 * (bluetoothd does not use OBEX's authentication).
 */
static void
obex_server_connect(
	struct btd_obex *ob,
	const uint8_t *packet,
	size_t length)
{
	struct btd_obex_header target;
	struct btd_obex_header challenge;
	struct btd_obex_writer writer;
	const uint8_t *headers;
	size_t headers_length;
	uint8_t prefix[4];
	size_t answer;
	int found;
	int served;

	/* The prefix of a Connect, and a peer's largest packet of OBEX's least at least. */
	if (length < OBEX_CONNECT_PREFIX) {
		obex_respond(ob, BTD_OBEX_BAD_REQUEST);
		return;
	}

	/* The peer's largest packet. */
	ob->peer_max = obex_get16(packet + 5);
	if (ob->peer_max < BTD_OBEX_PACKET_MIN) {
		obex_respond(ob, BTD_OBEX_BAD_REQUEST);
		return;
	}

	/* At most bluetoothd's own for what it sends. */
	if (ob->peer_max > BTD_OBEX_PACKET_MAX)
		ob->peer_max = BTD_OBEX_PACKET_MAX;
	headers = packet + OBEX_CONNECT_PREFIX;
	headers_length = length - OBEX_CONNECT_PREFIX;

	/* An authentication challenge: refused. */
	found = btd_obex_find(headers, headers_length, BTD_OBEX_CHALLENGE, &challenge);
	if (found < 0) {
		obex_respond(ob, BTD_OBEX_BAD_REQUEST);
		return;
	}

	/* A challenge in it. */
	if (found > 0) {
		obex_respond(ob, BTD_OBEX_UNAUTHORIZED);
		return;
	}

	/* The Target, if any, as the owner decides. */
	found = btd_obex_find(headers, headers_length, BTD_OBEX_TARGET, &target);
	if (found < 0) {
		obex_respond(ob, BTD_OBEX_BAD_REQUEST);
		return;
	}

	/* The owner's decision on the Target, or on none. */
	served = 0;
	if (ob->events.target != NULL && found > 0)
		served = ob->events.target(ob->events.context, target.data, target.length);
	if (ob->events.target != NULL && found == 0)
		served = ob->events.target(ob->events.context, NULL, 0U);
	if (served != 1) {
		obex_respond(ob, BTD_OBEX_SERVICE_UNAVAILABLE);
		return;
	}

	/* Success: the version, no flags, the largest packet, a Connection ID and Who. */
	obex_begin(&writer, ob->tx, sizeof(ob->tx), BTD_OBEX_SUCCESS);
	prefix[0] = BTD_OBEX_VERSION;
	prefix[1] = 0x00U;
	obex_put16(prefix + 2, BTD_OBEX_PACKET_MAX);
	obex_raw(&writer, prefix, sizeof(prefix));
	ob->connection_id = ob->next_connection_id;
	ob->next_connection_id++;
	ob->have_connection_id = 1;
	btd_obex_put_quad(&writer, BTD_OBEX_CONNECTION_ID, ob->connection_id);
	if (found > 0)
		btd_obex_put_bytes(&writer, BTD_OBEX_WHO, target.data, target.length);
	answer = obex_finish(&writer);
	if (answer == 0U) {
		obex_respond(ob, BTD_OBEX_INTERNAL_ERROR);
		return;
	}

	/* Succeeded: connected, the answer sent. */
	ob->state = BTD_OBEX_CONNECTED;
	ob->server_put = 0;
	(void)obex_send(ob, answer, 0U);
}

/*
 * Takes a packet of the peer's Put: the first one's headers other than the
 * body are kept, the body gathered within BTD_OBEX_PUT_MAX; Continue
 * answers all but the last, whose answer the owner's put hook gives.
 */
static void
obex_server_put(
	struct btd_obex *ob,
	const uint8_t *packet,
	size_t length)
{
	struct btd_obex_header header;
	const uint8_t *headers;
	size_t headers_length;
	size_t offset;
	size_t start;
	uint8_t code;
	int read;

	/* Only on a connection. */
	if (ob->state != BTD_OBEX_CONNECTED) {
		obex_respond(ob, BTD_OBEX_SERVICE_UNAVAILABLE);
		return;
	}

	/* The headers after the prefix. */
	headers = packet + OBEX_PREFIX;
	headers_length = length - OBEX_PREFIX;

	/* The first packet starts a new Put. */
	if (!ob->server_put) {
		ob->server_put = 1;
		ob->request_headers_length = 0U;
		ob->server_body_used = 0U;
	}

	/* Each header: the body gathered, the others kept from the first packet. */
	offset = 0U;
	for (;;) {
		start = offset;
		read = btd_obex_header_next(headers, headers_length, &offset, &header);
		if (read == 0)
			break;
		if (read < 0) {
			ob->server_put = 0;
			obex_respond(ob, BTD_OBEX_BAD_REQUEST);
			return;
		}

		/* The body, within its bound. */
		if (header.id == BTD_OBEX_BODY || header.id == BTD_OBEX_END_OF_BODY) {
			if (header.length > sizeof(ob->server_body) - ob->server_body_used) {
				ob->server_put = 0;
				obex_respond(ob, BTD_OBEX_NOT_ACCEPTABLE);
				return;
			}

			/* Gathered. */
			memcpy(ob->server_body + ob->server_body_used, header.data, header.length);
			ob->server_body_used += header.length;
			continue;
		}

		/* Another header, kept whole when it fits. */
		if (offset - start <= sizeof(ob->request_headers) - ob->request_headers_length) {
			memcpy(ob->request_headers + ob->request_headers_length, headers + start, offset - start);
			ob->request_headers_length += offset - start;
		}
	}

	/* More to come: Continue. */
	if ((packet[0] & BTD_OBEX_FINAL) == 0U) {
		obex_respond(ob, BTD_OBEX_CONTINUE);
		return;
	}

	/* Succeeded: the whole Put to the owner, and its answer. */
	ob->server_put = 0;
	code = BTD_OBEX_NOT_IMPLEMENTED;
	if (ob->events.put != NULL)
		code = ob->events.put(ob->events.context, ob->request_headers, ob->request_headers_length, ob->server_body, ob->server_body_used);
	obex_respond(ob, code);
}

/* Sends a server's answer with no headers. */
static void
obex_respond(
	struct btd_obex *ob,
	uint8_t code)
{
	struct btd_obex_writer writer;
	size_t packet;

	/* The code and the length. */
	obex_begin(&writer, ob->tx, sizeof(ob->tx), code);
	packet = obex_finish(&writer);

	/* Succeeded: sent (a broken DLC is the owner's to see). */
	(void)obex_send(ob, packet, 0U);
}

/* Breaks the connection: the client's operation fails with the error, and nothing more is taken (the owner closes the DLC). */
static void
obex_break(
	struct btd_obex *ob,
	int error)
{
	/* Broken already. */
	if (ob->state == BTD_OBEX_BROKEN)
		return;

	/* Nothing more goes or comes. */
	ob->state = BTD_OBEX_BROKEN;
	ob->tx_used = 0U;
	ob->tx_sent = 0U;
	ob->rx_used = 0U;

	/* Succeeded: an operation running is told. */
	if (ob->operation != BTD_OBEX_OP_NONE)
		obex_end(ob, error, 0U, NULL, 0U);
}
