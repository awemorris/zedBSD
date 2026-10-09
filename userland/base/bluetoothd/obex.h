/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * bluetoothd's OBEX (ws197-p002, plan/ws197/phase002/phase.md section 9):
 * the packets and headers of OBEX as GOEP 1.1 uses them over an RFCOMM
 * DLC, a client (Connect, Get, Put, SetPath, Abort, Disconnect; MAP's MAS
 * and PBAP's PSE) and a server (Connect, Put, Disconnect; MAP's MNS).  One
 * operation runs at a time on a connection: OBEX answers each request
 * before the next.
 *
 * Without system calls: the owner hands in the DLC's bytes and the clock,
 * and the connection writes through its write hook; the host tests build
 * it.
 */

#ifndef BLUETOOTHD_OBEX_H
#define BLUETOOTHD_OBEX_H

#include <stddef.h>
#include <stdint.h>

/* The packet bluetoothd takes at most, the least a peer may offer, and how long an answer may take. */
#define BTD_OBEX_PACKET_MAX		8192U
#define BTD_OBEX_PACKET_MIN		255U
#define BTD_OBEX_TIMEOUT_MS		10000U

/* The most a server's Put may carry in all, and the request headers it keeps from its first packet. */
#define BTD_OBEX_PUT_MAX		16384U
#define BTD_OBEX_HEADERS_MAX		1024U

/* The version of OBEX in Connect (1.0), and the Final bit of an opcode or a response code. */
#define BTD_OBEX_VERSION		0x10U
#define BTD_OBEX_FINAL			0x80U

/* The opcodes (the Final bit included where it always is). */
#define BTD_OBEX_CONNECT		0x80U
#define BTD_OBEX_DISCONNECT		0x81U
#define BTD_OBEX_PUT			0x02U
#define BTD_OBEX_GET			0x03U
#define BTD_OBEX_SETPATH		0x85U
#define BTD_OBEX_ABORT			0xffU

/* The response codes bluetoothd sends or reads (with the Final bit). */
#define BTD_OBEX_CONTINUE		0x90U
#define BTD_OBEX_SUCCESS		0xa0U
#define BTD_OBEX_BAD_REQUEST		0xc0U
#define BTD_OBEX_UNAUTHORIZED		0xc1U
#define BTD_OBEX_FORBIDDEN		0xc3U
#define BTD_OBEX_NOT_FOUND		0xc4U
#define BTD_OBEX_NOT_ACCEPTABLE		0xc6U
#define BTD_OBEX_PRECONDITION_FAILED	0xccU
#define BTD_OBEX_INTERNAL_ERROR		0xd0U
#define BTD_OBEX_NOT_IMPLEMENTED	0xd1U
#define BTD_OBEX_SERVICE_UNAVAILABLE	0xd3U

/* The headers bluetoothd uses; the two upper bits of an id give its encoding. */
#define BTD_OBEX_NAME			0x01U
#define BTD_OBEX_TYPE			0x42U
#define BTD_OBEX_LENGTH			0xc3U
#define BTD_OBEX_TARGET			0x46U
#define BTD_OBEX_BODY			0x48U
#define BTD_OBEX_END_OF_BODY		0x49U
#define BTD_OBEX_WHO			0x4aU
#define BTD_OBEX_APPLICATION		0x4cU
#define BTD_OBEX_CHALLENGE		0x4dU
#define BTD_OBEX_CONNECTION_ID		0xcbU

/* The encodings of a header id's two upper bits: Unicode text, a byte sequence, one byte, four bytes. */
#define BTD_OBEX_ENCODING_MASK		0xc0U
#define BTD_OBEX_ENCODING_UNICODE	0x00U
#define BTD_OBEX_ENCODING_BYTES		0x40U
#define BTD_OBEX_ENCODING_BYTE		0x80U
#define BTD_OBEX_ENCODING_QUAD		0xc0U

/* SetPath's flags: up one folder first, and do not create the folder. */
#define BTD_OBEX_SETPATH_UP		0x01U
#define BTD_OBEX_SETPATH_NO_CREATE	0x02U

/* The roles of a connection. */
#define BTD_OBEX_CLIENT			1U
#define BTD_OBEX_SERVER			2U

/*
 * The states: not connected, Connect sent (client), connected, an
 * operation's request out (client), Disconnect sent (client), and broken
 * (a timeout or a malformed packet: the owner closes the DLC).
 */
#define BTD_OBEX_IDLE			0U
#define BTD_OBEX_CONNECTING		1U
#define BTD_OBEX_CONNECTED		2U
#define BTD_OBEX_BUSY			3U
#define BTD_OBEX_DISCONNECTING		4U
#define BTD_OBEX_BROKEN			5U

/* The client's operations. */
#define BTD_OBEX_OP_NONE		0U
#define BTD_OBEX_OP_CONNECT		1U
#define BTD_OBEX_OP_GET			2U
#define BTD_OBEX_OP_PUT			3U
#define BTD_OBEX_OP_SETPATH		4U
#define BTD_OBEX_OP_ABORT		5U
#define BTD_OBEX_OP_DISCONNECT		6U

/* One header of a packet, read: its id, its value's bytes (Unicode as UTF-16BE, its NUL included), and a one or four byte value. */
struct btd_obex_header {
	uint8_t id;
	const uint8_t *data;
	size_t length;
	uint32_t value;
};

/* A packet being written into a buffer: its room, how much is used, and whether something did not fit. */
struct btd_obex_writer {
	uint8_t *bytes;
	size_t size;
	size_t used;
	int overflow;
};

/*
 * What a connection tells its owner, and how it writes.  write writes
 * bytes on the DLC and says how many went (fewer when credits ran out: the
 * owner calls btd_obex_pump when its DLC is writable again).  A client's
 * operation ends in done: 0 or an errno value, the response code (0 when
 * none came), and the final response's headers.  body gives a Get's body as
 * it comes and returns nonzero to stop (the Get is aborted).  A server asks
 * target whether a Connect's Target (NULL: none) is served (1), and put for
 * the response code of a whole Put (its first packet's headers, its body).
 */
struct btd_obex_events {
	void *context;
	int (*write)(void *context, const uint8_t *data, size_t length, size_t *written);
	void (*done)(void *context, unsigned operation, int error, uint8_t code, const uint8_t *headers, size_t length);
	int (*body)(void *context, const uint8_t *data, size_t length);
	int (*target)(void *context, const uint8_t *target, size_t length);
	uint8_t (*put)(void *context, const uint8_t *headers, size_t length, const uint8_t *body, size_t body_length);
};

/*
 * One OBEX connection over one DLC.  It lives in the profile's record from
 * the DLC's opening to its closing; btd_obex_init empties it.  The packet
 * being received and the one being sent are kept whole; a client's Put
 * keeps the caller's body (which stays valid until done) and how much of it
 * went; a server's Put keeps its first packet's headers and its body.
 * abort_wanted is an Abort asked for (or forced by abort_error) to be sent
 * at the client's next turn, aborting says it was sent; server_put says a
 * Put's packets are being gathered.
 */
struct btd_obex {
	struct btd_obex_events events;
	unsigned role;
	unsigned state;
	unsigned operation;
	int abort_wanted;
	int aborting;
	int abort_error;
	int server_put;
	uint64_t deadline;
	size_t peer_max;
	int have_connection_id;
	uint32_t connection_id;
	uint32_t next_connection_id;
	uint8_t who[16];
	size_t who_length;
	size_t body_limit;
	size_t body_count;
	const uint8_t *put_body;
	size_t put_length;
	size_t put_sent;
	size_t request_headers_length;
	uint8_t request_headers[BTD_OBEX_HEADERS_MAX];
	size_t rx_used;
	uint8_t rx[BTD_OBEX_PACKET_MAX];
	size_t tx_used;
	size_t tx_sent;
	uint8_t tx[BTD_OBEX_PACKET_MAX];
	size_t server_body_used;
	uint8_t server_body[BTD_OBEX_PUT_MAX];
	unsigned malformed;
};

void btd_obex_writer_init(struct btd_obex_writer *writer, uint8_t *bytes, size_t size);
void btd_obex_put_bytes(struct btd_obex_writer *writer, uint8_t id, const uint8_t *data, size_t length);
void btd_obex_put_text(struct btd_obex_writer *writer, uint8_t id, const char *text);
void btd_obex_put_byte(struct btd_obex_writer *writer, uint8_t id, uint8_t value);
void btd_obex_put_quad(struct btd_obex_writer *writer, uint8_t id, uint32_t value);
int btd_obex_header_next(const uint8_t *headers, size_t length, size_t *offset, struct btd_obex_header *header);
int btd_obex_find(const uint8_t *headers, size_t length, uint8_t id, struct btd_obex_header *header);

void btd_obex_init(struct btd_obex *ob, const struct btd_obex_events *events, unsigned role);
int btd_obex_connect(struct btd_obex *ob, const uint8_t *target, size_t target_length, uint64_t now);
int btd_obex_get(struct btd_obex *ob, const uint8_t *headers, size_t length, size_t body_limit, uint64_t now);
int btd_obex_put(struct btd_obex *ob, const uint8_t *headers, size_t length, const uint8_t *body, size_t body_length, uint64_t now);
int btd_obex_setpath(struct btd_obex *ob, uint8_t flags, const uint8_t *headers, size_t length, uint64_t now);
int btd_obex_abort(struct btd_obex *ob);
int btd_obex_disconnect(struct btd_obex *ob, uint64_t now);
void btd_obex_input(struct btd_obex *ob, const uint8_t *data, size_t length, uint64_t now);
void btd_obex_pump(struct btd_obex *ob);
void btd_obex_tick(struct btd_obex *ob, uint64_t now);
uint64_t btd_obex_deadline(const struct btd_obex *ob);

#endif
