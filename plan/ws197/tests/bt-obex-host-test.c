/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of bluetoothd's OBEX (ws197-p002, plan/ws197/phase002/
 * phase.md section 12.1), built with the host's compiler under ASan and
 * UBSan.  The peer's packets are written out here byte by byte (the
 * opcode, the length of the whole packet, the headers with lengths that
 * count their id and length): the header writers, the reading of headers
 * and its refusals, a client's Connect (the least packet of 255), Get with
 * Continue and its limit, Put cut to the peer's packet with End of Body,
 * the Put without a body and the one with MAP's filler, SetPath, Abort,
 * Disconnect, the timer, bytes coming one at a time and a DLC that takes
 * part of a packet; a server's Connect, its refusal of a challenge, a Put
 * in two packets, Disconnect; and a fuzz of both roles.
 *
 *   plan/ws197/tests/bt-phone-host-test.sh
 */

#include "userland/base/bluetoothd/obex.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The fuzz's rounds. */
#define TEST_FUZZ_ROUNDS	200000U

/* The packets kept of what the connection wrote, and the longest. */
#define TEST_SENT_MAX		32U
#define TEST_PACKET_MAX		8192U

/* The checks that failed, and those that ran. */
static unsigned failures;
static unsigned checks;

/* MAP's MAS Target (MAP 1.4.2 section 6.4.1, the UUID bb582b40-420c-11db-b0de-0800200c9a66). */
static const uint8_t test_target[16] = {
	0xbbU, 0x58U, 0x2bU, 0x40U, 0x42U, 0x0cU, 0x11U, 0xdbU,
	0xb0U, 0xdeU, 0x08U, 0x00U, 0x20U, 0x0cU, 0x9aU, 0x66U
};

/*
 * What the owner of the connection under test saw: the bytes written (cut
 * into the packets the connection wrote), how much the DLC takes at a
 * time (0: all), the body that came, the end of the operation, and a
 * server's Put.  One instance lives for each test.
 */
struct owner {
	unsigned sent_count;
	size_t sent_length[TEST_SENT_MAX];
	uint8_t sent[TEST_SENT_MAX][TEST_PACKET_MAX];
	size_t partial;
	int stalled;
	size_t pending;
	size_t body_length;
	uint8_t body[TEST_PACKET_MAX];
	int body_stop;
	unsigned done_count;
	unsigned done_operation;
	int done_error;
	uint8_t done_code;
	int target_answer;
	unsigned put_count;
	size_t put_headers_length;
	uint8_t put_headers[1024];
	size_t put_body_length;
	uint8_t put_body[64];
};

static void check(int condition, const char *what);
static int owner_write(void *context, const uint8_t *data, size_t length, size_t *written);
static void owner_done(void *context, unsigned operation, int error, uint8_t code, const uint8_t *headers, size_t length);
static int owner_body(void *context, const uint8_t *data, size_t length);
static int owner_target(void *context, const uint8_t *target, size_t length);
static uint8_t owner_put(void *context, const uint8_t *headers, size_t length, const uint8_t *body, size_t body_length);
static void owner_init(struct owner *owner, struct btd_obex *ob, unsigned role);
static size_t packet(uint8_t *out, uint8_t code, const uint8_t *prefix, size_t prefix_length, const uint8_t *headers, size_t length);
static void client_connected(struct owner *owner, struct btd_obex *ob, unsigned peer_max);
static void test_headers(void);
static void test_connect(void);
static void test_get(void);
static void test_put(void);
static void test_others(void);
static void test_server(void);
static void test_fuzz(void);

/*
 * Runs every part and reports the checks.
 */
int
main(void)
{
	/* Each part. */
	test_headers();
	test_connect();
	test_get();
	test_put();
	test_others();
	test_server();
	test_fuzz();

	/* The count of what failed. */
	printf("bt-obex-host-test: %u checks, %u failed\n", checks, failures);
	if (failures != 0U)
		return 1;

	/* Succeeded: every check held. */
	return 0;
}

/* Counts a check and reports the one that failed. */
static void
check(
	int condition,
	const char *what)
{
	/* Counted. */
	checks++;
	if (condition)
		return;

	/* Reported. */
	failures++;
	printf("FAIL: %s\n", what);
}

/*
 * Takes bytes the connection writes: when partial is set, at most that many
 * once, then nothing until the test clears stalled (a DLC whose credits ran
 * out); a packet is kept once all its bytes came (the length in its
 * prefix).
 */
static int
owner_write(
	void *context,
	const uint8_t *data,
	size_t length,
	size_t *written)
{
	struct owner *owner;
	size_t take;
	size_t whole;
	uint8_t *at;

	/* As much as the DLC takes: nothing while stalled. */
	owner = context;
	*written = 0U;
	if (owner->partial != 0U && owner->stalled)
		return 0;
	take = length;
	if (owner->partial != 0U && take > owner->partial)
		take = owner->partial;
	if (owner->partial != 0U)
		owner->stalled = 1;
	*written = take;

	/* Room in the test. */
	if (owner->sent_count >= TEST_SENT_MAX)
		return 0;

	/* Added to the packet being gathered. */
	at = owner->sent[owner->sent_count];
	memcpy(at + owner->pending, data, take);
	owner->pending += take;

	/* A packet whole: kept. */
	if (owner->pending >= 3U) {
		whole = ((size_t)at[1] << 8) | at[2];
		if (owner->pending >= whole) {
			owner->sent_length[owner->sent_count] = whole;
			owner->sent_count++;
			owner->pending = 0U;
		}
	}

	/* Succeeded: taken. */
	return 0;
}

/* Records the end of an operation. */
static void
owner_done(
	void *context,
	unsigned operation,
	int error,
	uint8_t code,
	const uint8_t *headers,
	size_t length)
{
	struct owner *owner;

	/* The count, the operation, the error and the code. */
	(void)headers;
	(void)length;
	owner = context;
	owner->done_count++;
	owner->done_operation = operation;
	owner->done_error = error;
	owner->done_code = code;
}

/* Gathers a Get's body, or asks to stop. */
static int
owner_body(
	void *context,
	const uint8_t *data,
	size_t length)
{
	struct owner *owner;

	/* Asked to stop. */
	owner = context;
	if (owner->body_stop)
		return 1;

	/* Gathered. */
	if (length <= sizeof(owner->body) - owner->body_length) {
		memcpy(owner->body + owner->body_length, data, length);
		owner->body_length += length;
	}

	/* Succeeded: go on. */
	return 0;
}

/* Answers a server's Connect as the test set it. */
static int
owner_target(
	void *context,
	const uint8_t *target,
	size_t length)
{
	struct owner *owner;
	int differs;

	/* The test's answer, and only for MAS's Target. */
	owner = context;
	if (target == NULL || length != sizeof(test_target))
		return 0;
	differs = memcmp(target, test_target, length);
	if (differs != 0)
		return 0;

	/* Succeeded: the test's answer. */
	return owner->target_answer;
}

/* Records a server's whole Put and answers Success. */
static uint8_t
owner_put(
	void *context,
	const uint8_t *headers,
	size_t length,
	const uint8_t *body,
	size_t body_length)
{
	struct owner *owner;

	/* The headers and the body. */
	owner = context;
	owner->put_count++;
	owner->put_headers_length = length;
	if (length <= sizeof(owner->put_headers))
		memcpy(owner->put_headers, headers, length);
	owner->put_body_length = body_length;
	if (body_length <= sizeof(owner->put_body))
		memcpy(owner->put_body, body, body_length);

	/* Succeeded: taken. */
	return BTD_OBEX_SUCCESS;
}

/* Empties the owner and gives a connection of a role its hooks. */
static void
owner_init(
	struct owner *owner,
	struct btd_obex *ob,
	unsigned role)
{
	struct btd_obex_events events;

	/* Nothing seen yet; a server's Target served. */
	memset(owner, 0, sizeof(*owner));
	owner->target_answer = 1;

	/* The hooks. */
	events.context = owner;
	events.write = owner_write;
	events.done = owner_done;
	events.body = owner_body;
	events.target = owner_target;
	events.put = owner_put;
	btd_obex_init(ob, &events, role);
}

/* Writes a packet as the peer would: the code, the length of the whole, a prefix and headers. */
static size_t
packet(
	uint8_t *out,
	uint8_t code,
	const uint8_t *prefix,
	size_t prefix_length,
	const uint8_t *headers,
	size_t length)
{
	size_t total;

	/* The code and the length. */
	total = 3U + prefix_length + length;
	out[0] = code;
	out[1] = (uint8_t)(total >> 8);
	out[2] = (uint8_t)(total & 0xffU);

	/* The prefix and the headers. */
	if (prefix_length != 0U)
		memcpy(out + 3, prefix, prefix_length);
	if (length != 0U)
		memcpy(out + 3 + prefix_length, headers, length);

	/* Succeeded: the packet's length. */
	return total;
}

/* Connects a client to a peer whose largest packet is given, with Connection ID 0x01020304. */
static void
client_connected(
	struct owner *owner,
	struct btd_obex *ob,
	unsigned peer_max)
{
	static const uint8_t id[] = { 0xcbU, 0x01U, 0x02U, 0x03U, 0x04U };
	uint8_t prefix[4];
	uint8_t answer[64];
	size_t length;
	int error;

	/* Connect, and the peer's Success. */
	owner_init(owner, ob, BTD_OBEX_CLIENT);
	error = btd_obex_connect(ob, test_target, sizeof(test_target), 1000U);
	check(error == 0, "connected: connect");
	prefix[0] = 0x10U;
	prefix[1] = 0x00U;
	prefix[2] = (uint8_t)(peer_max >> 8);
	prefix[3] = (uint8_t)(peer_max & 0xffU);
	length = packet(answer, 0xa0U, prefix, sizeof(prefix), id, sizeof(id));
	btd_obex_input(ob, answer, length, 1010U);
	check(ob->state == BTD_OBEX_CONNECTED, "connected: state");
	owner->sent_count = 0U;
	owner->done_count = 0U;
}

/*
 * The header writers and the reading: Name "telecom" (19 bytes: the id,
 * the length, 7 characters and the NUL as UTF-16BE), the empty Name (3), a
 * character beyond the BMP as a surrogate pair, text that is not UTF-8, the
 * one and four byte headers, and headers that are malformed.
 */
static void
test_headers(void)
{
	static const uint8_t telecom[] = {
		0x01U, 0x00U, 0x13U,
		0x00U, 't', 0x00U, 'e', 0x00U, 'l', 0x00U, 'e', 0x00U, 'c', 0x00U, 'o', 0x00U, 'm', 0x00U, 0x00U
	};
	static const uint8_t empty[] = { 0x01U, 0x00U, 0x03U };
	static const uint8_t smile[] = { 0x01U, 0x00U, 0x09U, 0xd8U, 0x3dU, 0xdeU, 0x00U, 0x00U, 0x00U };
	static const uint8_t odd[] = { 0x01U, 0x00U, 0x06U, 0x00U, 'a', 0x00U };
	static const uint8_t no_nul[] = { 0x01U, 0x00U, 0x05U, 0x00U, 'a' };
	static const uint8_t short_length[] = { 0x42U, 0x00U, 0x02U };
	static const uint8_t past[] = { 0x42U, 0x00U, 0x09U, 'a', 'b' };
	static const uint8_t quad[] = { 0xcbU, 0x01U, 0x02U, 0x03U, 0x04U, 0x93U, 0x7fU };
	struct btd_obex_writer writer;
	struct btd_obex_header header;
	uint8_t bytes[64];
	size_t offset;
	int read;

	/* Name "telecom". */
	btd_obex_writer_init(&writer, bytes, sizeof(bytes));
	btd_obex_put_text(&writer, BTD_OBEX_NAME, "telecom");
	check(!writer.overflow && writer.used == sizeof(telecom) && memcmp(bytes, telecom, sizeof(telecom)) == 0, "headers: Name telecom");

	/* The empty Name. */
	btd_obex_writer_init(&writer, bytes, sizeof(bytes));
	btd_obex_put_text(&writer, BTD_OBEX_NAME, "");
	check(writer.used == sizeof(empty) && memcmp(bytes, empty, sizeof(empty)) == 0, "headers: empty Name");

	/* U+1F600 as d83d de00. */
	btd_obex_writer_init(&writer, bytes, sizeof(bytes));
	btd_obex_put_text(&writer, BTD_OBEX_NAME, "\xf0\x9f\x98\x80");
	check(writer.used == sizeof(smile) && memcmp(bytes, smile, sizeof(smile)) == 0, "headers: surrogate pair");

	/* Not UTF-8. */
	btd_obex_writer_init(&writer, bytes, sizeof(bytes));
	btd_obex_put_text(&writer, BTD_OBEX_NAME, "\xff");
	check(writer.overflow, "headers: not UTF-8");

	/* Too long for the buffer. */
	btd_obex_writer_init(&writer, bytes, 10U);
	btd_obex_put_text(&writer, BTD_OBEX_NAME, "telecom");
	check(writer.overflow, "headers: overflow");

	/* Reading: Name, then the empty one, then the end. */
	memcpy(bytes, telecom, sizeof(telecom));
	memcpy(bytes + sizeof(telecom), empty, sizeof(empty));
	offset = 0U;
	read = btd_obex_header_next(bytes, sizeof(telecom) + sizeof(empty), &offset, &header);
	check(read == 1 && header.id == BTD_OBEX_NAME && header.length == 16U, "headers: read Name");
	read = btd_obex_header_next(bytes, sizeof(telecom) + sizeof(empty), &offset, &header);
	check(read == 1 && header.length == 0U, "headers: read empty Name");
	read = btd_obex_header_next(bytes, sizeof(telecom) + sizeof(empty), &offset, &header);
	check(read == 0, "headers: end");

	/* Four bytes and one byte. */
	offset = 0U;
	read = btd_obex_header_next(quad, sizeof(quad), &offset, &header);
	check(read == 1 && header.value == 0x01020304U, "headers: quad");
	read = btd_obex_header_next(quad, sizeof(quad), &offset, &header);
	check(read == 1 && header.value == 0x7fU, "headers: byte");

	/* Malformed: an odd Unicode length, no NUL, a length below 3, a length past the headers, a quad cut. */
	offset = 0U;
	read = btd_obex_header_next(odd, sizeof(odd), &offset, &header);
	check(read == -1, "headers: odd Unicode");
	offset = 0U;
	read = btd_obex_header_next(no_nul, sizeof(no_nul), &offset, &header);
	check(read == -1, "headers: Unicode without NUL");
	offset = 0U;
	read = btd_obex_header_next(short_length, sizeof(short_length), &offset, &header);
	check(read == -1, "headers: length below 3");
	offset = 0U;
	read = btd_obex_header_next(past, sizeof(past), &offset, &header);
	check(read == -1, "headers: length past the end");
	offset = 0U;
	read = btd_obex_header_next(quad, 3U, &offset, &header);
	check(read == -1, "headers: quad cut");
}

/*
 * Connect: the request's bytes (80, length 26, version 10, flags 00, 8192
 * as 20 00, Target 46 00 13 and the UUID), the peer's Success with 255
 * (the least) and its Connection ID, 254 refused as broken, and a refusal.
 */
static void
test_connect(void)
{
	static struct btd_obex ob;
	static struct owner owner;
	static const uint8_t id[] = { 0xcbU, 0x00U, 0x00U, 0x00U, 0x07U };
	uint8_t wanted[64];
	uint8_t prefix[4];
	uint8_t answer[64];
	size_t length;
	int error;

	/* The request. */
	owner_init(&owner, &ob, BTD_OBEX_CLIENT);
	error = btd_obex_connect(&ob, test_target, sizeof(test_target), 1000U);
	check(error == 0, "connect: sent");
	wanted[0] = 0x80U;
	wanted[1] = 0x00U;
	wanted[2] = 0x1aU;
	wanted[3] = 0x10U;
	wanted[4] = 0x00U;
	wanted[5] = 0x20U;
	wanted[6] = 0x00U;
	wanted[7] = 0x46U;
	wanted[8] = 0x00U;
	wanted[9] = 0x13U;
	memcpy(wanted + 10, test_target, sizeof(test_target));
	check(owner.sent_count == 1U && owner.sent_length[0] == 26U && memcmp(owner.sent[0], wanted, 26U) == 0, "connect: request bytes");

	/* Success with 255. */
	prefix[0] = 0x10U;
	prefix[1] = 0x00U;
	prefix[2] = 0x00U;
	prefix[3] = 0xffU;
	length = packet(answer, 0xa0U, prefix, sizeof(prefix), id, sizeof(id));
	btd_obex_input(&ob, answer, length, 1010U);
	check(owner.done_count == 1U && owner.done_error == 0 && ob.state == BTD_OBEX_CONNECTED, "connect: connected");
	check(ob.peer_max == 255U && ob.have_connection_id && ob.connection_id == 7U, "connect: the answer's values");

	/* 254: broken. */
	owner_init(&owner, &ob, BTD_OBEX_CLIENT);
	(void)btd_obex_connect(&ob, test_target, sizeof(test_target), 1000U);
	prefix[3] = 0xfeU;
	length = packet(answer, 0xa0U, prefix, sizeof(prefix), id, sizeof(id));
	btd_obex_input(&ob, answer, length, 1010U);
	check(owner.done_count == 1U && owner.done_error == EPROTO && ob.state == BTD_OBEX_BROKEN, "connect: 254 is too small");

	/* Refused (Service Unavailable): not connected. */
	owner_init(&owner, &ob, BTD_OBEX_CLIENT);
	(void)btd_obex_connect(&ob, test_target, sizeof(test_target), 1000U);
	length = packet(answer, 0xd3U, prefix, sizeof(prefix), NULL, 0U);
	btd_obex_input(&ob, answer, length, 1010U);
	check(owner.done_error == ECONNREFUSED && owner.done_code == 0xd3U && ob.state == BTD_OBEX_IDLE, "connect: refused");

	/* No answer: the timer breaks it. */
	owner_init(&owner, &ob, BTD_OBEX_CLIENT);
	(void)btd_obex_connect(&ob, test_target, sizeof(test_target), 1000U);
	check(btd_obex_deadline(&ob) == 1000U + BTD_OBEX_TIMEOUT_MS, "connect: deadline");
	btd_obex_tick(&ob, 1000U + BTD_OBEX_TIMEOUT_MS);
	check(owner.done_error == ETIMEDOUT && ob.state == BTD_OBEX_BROKEN, "connect: timeout");
}

/*
 * Get: the request (83, the Connection ID first, then the caller's Type),
 * Continue with a Body, the next Get of its prefix alone, Success with End
 * of Body; bytes coming one at a time; a body past the limit (Abort with
 * the Connection ID, EMSGSIZE); the owner stopping it (ECANCELED); an error
 * code.
 */
static void
test_get(void)
{
	static struct btd_obex ob;
	static struct owner owner;
	static const uint8_t type[] = { 0x42U, 0x00U, 0x08U, 'x', '-', 'a', 'b', 0x00U };
	static const uint8_t body_one[] = { 0x48U, 0x00U, 0x08U, 'a', 'b', 'c', 'd', 'e' };
	static const uint8_t body_two[] = { 0x49U, 0x00U, 0x06U, 'x', 'y', 'z' };
	static const uint8_t next[] = { 0x83U, 0x00U, 0x03U };
	static const uint8_t abort_packet[] = { 0xffU, 0x00U, 0x08U, 0xcbU, 0x01U, 0x02U, 0x03U, 0x04U };
	uint8_t wanted[64];
	uint8_t answer[64];
	size_t length;
	size_t index;
	int error;

	/* The request. */
	client_connected(&owner, &ob, 1024U);
	error = btd_obex_get(&ob, type, sizeof(type), 100U, 2000U);
	check(error == 0, "get: sent");
	wanted[0] = 0x83U;
	wanted[1] = 0x00U;
	wanted[2] = 0x10U;
	wanted[3] = 0xcbU;
	wanted[4] = 0x01U;
	wanted[5] = 0x02U;
	wanted[6] = 0x03U;
	wanted[7] = 0x04U;
	memcpy(wanted + 8, type, sizeof(type));
	check(owner.sent_count == 1U && owner.sent_length[0] == 16U && memcmp(owner.sent[0], wanted, 16U) == 0, "get: request bytes");

	/* Continue with a Body: the body taken, the next Get. */
	length = packet(answer, 0x90U, NULL, 0U, body_one, sizeof(body_one));
	btd_obex_input(&ob, answer, length, 2010U);
	check(owner.body_length == 5U && memcmp(owner.body, "abcde", 5U) == 0, "get: first body");
	check(owner.sent_count == 2U && owner.sent_length[1] == 3U && memcmp(owner.sent[1], next, 3U) == 0, "get: next request");

	/* Success with End of Body, a byte at a time. */
	length = packet(answer, 0xa0U, NULL, 0U, body_two, sizeof(body_two));
	for (index = 0U; index < length; index++)
		btd_obex_input(&ob, answer + index, 1U, 2020U);
	check(owner.done_count == 1U && owner.done_error == 0 && owner.done_operation == BTD_OBEX_OP_GET, "get: done");
	check(owner.body_length == 8U && memcmp(owner.body, "abcdexyz", 8U) == 0, "get: whole body");
	check(ob.state == BTD_OBEX_CONNECTED, "get: connected again");

	/* Past the limit (4 bytes): Abort, then its answer ends the Get with EMSGSIZE. */
	(void)btd_obex_get(&ob, type, sizeof(type), 4U, 3000U);
	owner.sent_count = 0U;
	length = packet(answer, 0x90U, NULL, 0U, body_one, sizeof(body_one));
	btd_obex_input(&ob, answer, length, 3010U);
	check(owner.sent_count == 1U && owner.sent_length[0] == sizeof(abort_packet) && memcmp(owner.sent[0], abort_packet, sizeof(abort_packet)) == 0, "get: abort bytes");
	length = packet(answer, 0xa0U, NULL, 0U, NULL, 0U);
	btd_obex_input(&ob, answer, length, 3020U);
	check(owner.done_count == 2U && owner.done_error == EMSGSIZE && ob.state == BTD_OBEX_CONNECTED, "get: limit");

	/* The owner stops it: Abort, ECANCELED. */
	owner.body_stop = 1;
	(void)btd_obex_get(&ob, type, sizeof(type), 100U, 4000U);
	length = packet(answer, 0x90U, NULL, 0U, body_one, sizeof(body_one));
	btd_obex_input(&ob, answer, length, 4010U);
	length = packet(answer, 0xa0U, NULL, 0U, NULL, 0U);
	btd_obex_input(&ob, answer, length, 4020U);
	check(owner.done_count == 3U && owner.done_error == ECANCELED, "get: stopped");
	owner.body_stop = 0;

	/* Not Found. */
	(void)btd_obex_get(&ob, type, sizeof(type), 100U, 5000U);
	length = packet(answer, 0xc4U, NULL, 0U, NULL, 0U);
	btd_obex_input(&ob, answer, length, 5010U);
	check(owner.done_count == 4U && owner.done_error == EIO && owner.done_code == 0xc4U, "get: not found");
}

/*
 * Put: 600 bytes to a peer of 255-byte packets (the first with the
 * Connection ID and Type, Body headers, the last with End of Body and the
 * Final bit, every packet within 255), each Continue answered by the next;
 * a Put without a body (82 with no body header); MAP's filler (End of Body
 * 30); a DLC that takes 7 bytes at a time.
 */
static void
test_put(void)
{
	static struct btd_obex ob;
	static struct owner owner;
	static const uint8_t type[] = { 0x42U, 0x00U, 0x08U, 'x', '-', 'a', 'b', 0x00U };
	static const uint8_t filler[] = { 0x30U };
	static uint8_t body[600];
	static uint8_t gathered[600];
	struct btd_obex_header header;
	uint8_t answer[16];
	size_t gathered_length;
	size_t length;
	size_t offset;
	unsigned index;
	int final_seen;
	int sizes_good;
	int read;
	int error;

	/* 600 bytes of a pattern. */
	for (index = 0U; index < sizeof(body); index++)
		body[index] = (uint8_t)(index * 7U);

	/* Put them, answering each packet with Continue until the final one, then Success. */
	client_connected(&owner, &ob, 255U);
	error = btd_obex_put(&ob, type, sizeof(type), body, sizeof(body), 1000U);
	check(error == 0, "put: sent");
	for (index = 0U; index < 10U && owner.done_count == 0U; index++) {
		length = packet(answer, 0x90U, NULL, 0U, NULL, 0U);
		if ((owner.sent[owner.sent_count - 1U][0] & 0x80U) != 0U)
			length = packet(answer, 0xa0U, NULL, 0U, NULL, 0U);
		btd_obex_input(&ob, answer, length, 1000U + index);
	}

	/* Ended with Success. */
	check(owner.done_count == 1U && owner.done_error == 0 && owner.done_operation == BTD_OBEX_OP_PUT, "put: done");

	/* The packets: each within 255, the first with the Connection ID, the body whole in order, the Final bit on the last only. */
	gathered_length = 0U;
	final_seen = 0;
	sizes_good = 1;
	for (index = 0U; index < owner.sent_count; index++) {
		if (owner.sent_length[index] > 255U)
			sizes_good = 0;
		if (index == 0U && owner.sent[0][3] != 0xcbU)
			sizes_good = 0;
		if ((owner.sent[index][0] & 0x80U) != 0U)
			final_seen = (int)index + 1;
		offset = 0U;
		for (;;) {
			read = btd_obex_header_next(owner.sent[index] + 3, owner.sent_length[index] - 3U, &offset, &header);
			if (read != 1)
				break;
			if ((header.id == 0x48U || header.id == 0x49U) && header.length <= sizeof(gathered) - gathered_length) {
				memcpy(gathered + gathered_length, header.data, header.length);
				gathered_length += header.length;
			}
		}
	}

	/* What the packets held. */
	check(sizes_good, "put: packets within 255 and the Connection ID first");
	check(final_seen == (int)owner.sent_count && owner.sent_count == 3U, "put: three packets, the Final bit on the last");
	check(gathered_length == sizeof(body) && memcmp(gathered, body, sizeof(body)) == 0, "put: the body whole");

	/* Without a body: 82, the Connection ID, Type, no body header. */
	owner.sent_count = 0U;
	(void)btd_obex_put(&ob, type, sizeof(type), NULL, 0U, 2000U);
	check(owner.sent_count == 1U && owner.sent[0][0] == 0x82U && owner.sent_length[0] == 3U + 5U + sizeof(type), "put: no body");
	length = packet(answer, 0xa0U, NULL, 0U, NULL, 0U);
	btd_obex_input(&ob, answer, length, 2010U);
	check(owner.done_count == 2U && owner.done_error == 0, "put: no body done");

	/* MAP's filler: End of Body 49 00 04 30, a DLC of 7 bytes at a time. */
	owner.sent_count = 0U;
	owner.partial = 7U;
	(void)btd_obex_put(&ob, type, sizeof(type), filler, sizeof(filler), 3000U);
	check(owner.sent_count == 0U && owner.pending == 7U, "put: part of the packet went");
	owner.stalled = 0;
	btd_obex_pump(&ob);
	check(owner.sent_count == 0U && owner.pending == 14U, "put: a second part by the pump");
	owner.stalled = 0;
	btd_obex_pump(&ob);
	check(owner.sent_count == 1U && owner.sent_length[0] == 3U + 5U + sizeof(type) + 4U, "put: the rest went by the pump");
	check(owner.sent[0][0] == 0x82U && owner.sent[0][owner.sent_length[0] - 4U] == 0x49U && owner.sent[0][owner.sent_length[0] - 1U] == 0x30U, "put: filler");
	owner.partial = 0U;
}

/* SetPath (85, flags, constants, the Connection ID, Name), Disconnect (81 with the Connection ID), and answers nobody waits for. */
static void
test_others(void)
{
	static struct btd_obex ob;
	static struct owner owner;
	static const uint8_t name[] = { 0x01U, 0x00U, 0x05U, 0x00U, 0x00U };
	static const uint8_t wanted_path[] = { 0x85U, 0x00U, 0x0dU, 0x02U, 0x00U, 0xcbU, 0x01U, 0x02U, 0x03U, 0x04U, 0x01U, 0x00U, 0x03U };
	static const uint8_t wanted_disconnect[] = { 0x81U, 0x00U, 0x08U, 0xcbU, 0x01U, 0x02U, 0x03U, 0x04U };
	struct btd_obex_writer writer;
	uint8_t headers[16];
	uint8_t answer[16];
	size_t length;
	int error;

	/* SetPath to the root (the empty Name), not creating. */
	client_connected(&owner, &ob, 1024U);
	btd_obex_writer_init(&writer, headers, sizeof(headers));
	btd_obex_put_text(&writer, BTD_OBEX_NAME, "");
	error = btd_obex_setpath(&ob, BTD_OBEX_SETPATH_NO_CREATE, headers, writer.used, 1000U);
	check(error == 0 && owner.sent_count == 1U && owner.sent_length[0] == sizeof(wanted_path), "others: setpath sent");
	check(memcmp(owner.sent[0], wanted_path, sizeof(wanted_path)) == 0, "others: setpath bytes");
	length = packet(answer, 0xa0U, NULL, 0U, NULL, 0U);
	btd_obex_input(&ob, answer, length, 1010U);
	check(owner.done_count == 1U && owner.done_operation == BTD_OBEX_OP_SETPATH && owner.done_error == 0, "others: setpath done");

	/* Busy: a second request while one runs. */
	(void)btd_obex_setpath(&ob, 0U, name, sizeof(name), 1100U);
	error = btd_obex_get(&ob, NULL, 0U, 10U, 1100U);
	check(error == EBUSY, "others: busy");
	btd_obex_input(&ob, answer, length, 1110U);

	/* Disconnect. */
	owner.sent_count = 0U;
	error = btd_obex_disconnect(&ob, 2000U);
	check(error == 0 && owner.sent_count == 1U && memcmp(owner.sent[0], wanted_disconnect, sizeof(wanted_disconnect)) == 0, "others: disconnect bytes");
	btd_obex_input(&ob, answer, length, 2010U);
	check(owner.done_operation == BTD_OBEX_OP_DISCONNECT && ob.state == BTD_OBEX_IDLE, "others: disconnected");

	/* An answer nobody waits for: counted, not taken. */
	btd_obex_input(&ob, answer, length, 2020U);
	check(ob.malformed == 1U, "others: stray answer");

	/* A packet whose length is below 3: broken. */
	answer[0] = 0xa0U;
	answer[1] = 0x00U;
	answer[2] = 0x02U;
	btd_obex_input(&ob, answer, 3U, 2030U);
	check(ob.state == BTD_OBEX_BROKEN, "others: length below 3");
}

/*
 * A server (MAP's MNS): Connect with MAS's Target (Success, 8192, the
 * Connection ID 1, Who), a Connect with a challenge (Unauthorized), a Put
 * in two packets (Continue, then the owner's answer with the first packet's
 * Type and the body gathered), Get not implemented, Disconnect.
 */
static void
test_server(void)
{
	static struct btd_obex ob;
	static struct owner owner;
	static const uint8_t type[] = { 0x42U, 0x00U, 0x08U, 'x', '-', 'a', 'b', 0x00U };
	uint8_t request[128];
	uint8_t headers[64];
	uint8_t prefix[4];
	size_t length;
	size_t used;

	/* Connect with the Target. */
	owner_init(&owner, &ob, BTD_OBEX_SERVER);
	prefix[0] = 0x10U;
	prefix[1] = 0x00U;
	prefix[2] = 0x04U;
	prefix[3] = 0x00U;
	headers[0] = 0x46U;
	headers[1] = 0x00U;
	headers[2] = 0x13U;
	memcpy(headers + 3, test_target, sizeof(test_target));
	length = packet(request, 0x80U, prefix, sizeof(prefix), headers, 19U);
	btd_obex_input(&ob, request, length, 1000U);
	check(owner.sent_count == 1U && owner.sent[0][0] == 0xa0U && owner.sent_length[0] == 7U + 5U + 19U, "server: connect answered");
	check(owner.sent[0][5] == 0x20U && owner.sent[0][6] == 0x00U, "server: 8192");
	check(owner.sent[0][7] == 0xcbU && owner.sent[0][11] == 0x01U, "server: Connection ID 1");
	check(owner.sent[0][12] == 0x4aU && memcmp(owner.sent[0] + 15, test_target, 16U) == 0, "server: Who");
	check(ob.state == BTD_OBEX_CONNECTED && ob.peer_max == 1024U, "server: connected");

	/* A Put in two packets: Type and Body "ab", then End of Body "cd". */
	used = 0U;
	memcpy(headers, type, sizeof(type));
	used += sizeof(type);
	headers[used] = 0x48U;
	headers[used + 1U] = 0x00U;
	headers[used + 2U] = 0x05U;
	headers[used + 3U] = 'a';
	headers[used + 4U] = 'b';
	used += 5U;
	length = packet(request, 0x02U, NULL, 0U, headers, used);
	btd_obex_input(&ob, request, length, 1010U);
	check(owner.sent_count == 2U && owner.sent[1][0] == 0x90U, "server: continue");
	headers[0] = 0x49U;
	headers[1] = 0x00U;
	headers[2] = 0x05U;
	headers[3] = 'c';
	headers[4] = 'd';
	length = packet(request, 0x82U, NULL, 0U, headers, 5U);
	btd_obex_input(&ob, request, length, 1020U);
	check(owner.put_count == 1U && owner.put_body_length == 4U && memcmp(owner.put_body, "abcd", 4U) == 0, "server: put body");
	check(owner.put_headers_length == sizeof(type) && memcmp(owner.put_headers, type, sizeof(type)) == 0, "server: put headers");
	check(owner.sent_count == 3U && owner.sent[2][0] == 0xa0U, "server: put answered");

	/* Get: not implemented. */
	length = packet(request, 0x83U, NULL, 0U, NULL, 0U);
	btd_obex_input(&ob, request, length, 1030U);
	check(owner.sent_count == 4U && owner.sent[3][0] == 0xd1U, "server: get not implemented");

	/* Disconnect. */
	length = packet(request, 0x81U, NULL, 0U, NULL, 0U);
	btd_obex_input(&ob, request, length, 1040U);
	check(owner.sent_count == 5U && owner.sent[4][0] == 0xa0U && ob.state == BTD_OBEX_IDLE, "server: disconnected");

	/* A Connect with a challenge: Unauthorized. */
	owner_init(&owner, &ob, BTD_OBEX_SERVER);
	headers[19] = 0x4dU;
	headers[20] = 0x00U;
	headers[21] = 0x05U;
	headers[22] = 0x00U;
	headers[23] = 0x00U;
	headers[0] = 0x46U;
	headers[1] = 0x00U;
	headers[2] = 0x13U;
	memcpy(headers + 3, test_target, sizeof(test_target));
	length = packet(request, 0x80U, prefix, sizeof(prefix), headers, 24U);
	btd_obex_input(&ob, request, length, 2000U);
	check(owner.sent_count == 1U && owner.sent[0][0] == 0xc1U && ob.state == BTD_OBEX_IDLE, "server: challenge refused");

	/* A Target not served: Service Unavailable. */
	owner.target_answer = 0;
	length = packet(request, 0x80U, prefix, sizeof(prefix), headers, 19U);
	btd_obex_input(&ob, request, length, 2010U);
	check(owner.sent_count == 2U && owner.sent[1][0] == 0xd3U, "server: target refused");
}

/* Random bytes into both roles (a fixed seed, connected first): nothing crashes under ASan and UBSan. */
static void
test_fuzz(void)
{
	static struct btd_obex ob;
	static struct owner owner;
	uint8_t bytes[300];
	unsigned round;
	unsigned seed;
	size_t length;
	size_t index;

	/* A fixed seed. */
	seed = 0x0bec0de1U;
	for (round = 0U; round < TEST_FUZZ_ROUNDS; round++) {
		/* A new connection every 100 rounds, of either role. */
		if (round % 100U == 0U) {
			if (((round / 100U) & 1U) != 0U) {
				owner_init(&owner, &ob, BTD_OBEX_SERVER);
			} else {
				client_connected(&owner, &ob, 512U);
				(void)btd_obex_get(&ob, NULL, 0U, 4096U, round);
			}
		}

		/* Random bytes, often with a length that matches. */
		seed = seed * 1103515245U + 12345U;
		length = (size_t)((seed >> 16) % sizeof(bytes)) + 1U;
		for (index = 0U; index < length; index++) {
			seed = seed * 1103515245U + 12345U;
			bytes[index] = (uint8_t)(seed >> 16);
		}

		/* Often a packet whose length says its size. */
		if ((seed & 0x300U) != 0U && length >= 3U) {
			bytes[1] = (uint8_t)(length >> 8);
			bytes[2] = (uint8_t)(length & 0xffU);
		}

		/* Taken, then the timer. */
		btd_obex_input(&ob, bytes, length, round);
		btd_obex_tick(&ob, round);
		owner.sent_count = 0U;
		owner.pending = 0U;
	}

	/* Nothing crashed. */
	check(1, "fuzz: ran");
}
