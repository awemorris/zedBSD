/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The test of ssh-transport.c.  Two transports, a client and a server, are
 * joined in memory (one's pending output is the other's input), with the
 * host key operations made from a test key (ssh-key.h).  A peer that
 * breaks the protocol is played by the test: its identification lines and
 * unkeyed packets are built here with wire.h from RFC 4253 sections 4.2
 * and 6 (uint32 packet_length, byte padding_length, payload, padding), the
 * KEXINIT from its section 7.1, the generic messages from its section 11,
 * the strict markers from draft-ietf-sshm-strict-kex-02 section 3.1 and
 * the hybrid messages from draft-ietf-sshm-mlkem-hybrid-kex-10 section
 * 2.2.  Keyed packets of a misbehaving peer are framed with the peer
 * transport's own packet layer (ssh-packet.h), so that the numbers the
 * transport refuses to send can be sent.  The export layout checked is the
 * one ssh-transport.c documents.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "wire.h"
#include "ssh-key.h"
#include "ssh-transport.h"

/* The most messages one side of a test receives. */
#define INBOX_MAX		512U

/* The bytes kept of the start of each received message. */
#define INBOX_HEAD		64U

/* The most packets read from a side's unkeyed output. */
#define PLAIN_MAX		8U

/* The largest unkeyed payload kept. */
#define PLAIN_SIZE		4096U

/* The bytes moved from one side to the other in one step. */
#define MOVE_CHUNK		65536U

/* The time the tests start the transports at. */
#define TEST_NOW		1000U

/* The number of random inputs at each step of the exchange. */
#define TEST_RANDOM_RUNS	1000U

/* The number of random messages sent over a running connection. */
#define TEST_RANDOM_RUNNING	20000U

/* The messages one side received, in order: the first bytes, the size and a hash of each. */
struct inbox {
	size_t count;
	size_t size[INBOX_MAX];
	uint32_t hash[INBOX_MAX];
	uint8_t head[INBOX_MAX][INBOX_HEAD];
};

/* What the host key operations of one side do and saw. */
struct test_context {
	const struct ssh_key *key;
	int refuse;
	int fail;
	int flip;
	int fail_host_key;
	int fail_sign;
	unsigned host_key_calls;
	unsigned sign_calls;
	unsigned check_calls;
	uint8_t blob[SSH_KEY_BLOB_SIZE];
	size_t blob_size;
};

/* The payloads read from a side's unkeyed output. */
struct plain {
	size_t count;
	size_t size[PLAIN_MAX];
	uint8_t data[PLAIN_MAX][PLAIN_SIZE];
};

/* The client and the server of each test, and what they received (large: static). */
static struct ssh_transport client;
static struct ssh_transport server;
static struct inbox client_inbox;
static struct inbox server_inbox;
static struct test_context client_context;
static struct test_context server_context;
static struct ssh_key host_key;
static struct plain plain_out;

/* Wire buffers for the bytes a test builds. */
static struct wire message_wire;
static struct wire frame_wire;
static struct wire export_wire;

/* 1 when exchange_with_reply adds a byte to the server's NEWKEYS; 1 when it sends the reply twice. */
static int newkeys_extra;
static int reply_twice;

/* A payload buffer for the receiving side. */
static uint8_t big_payload[SSH_PAYLOAD_SEND_MAX];

/* The state of the pseudo random generator that makes the random inputs. */
static uint32_t random_state = 0x6d2b79f5U;

static uint32_t next_random(void);
static uint32_t fnv(const uint8_t *data, size_t size);
static int test_host_key(void *context, struct ssh_buf *out);
static int test_sign(void *context, const uint8_t *h, struct ssh_buf *out);
static int test_check_host_key(void *context, const uint8_t *blob, size_t size);
static void start_pair(void);
static void start_one(struct ssh_transport *transport, enum ssh_role role);
static void stop_pair(void);
static int step(struct ssh_transport *transport, struct inbox *inbox);
static int transfer(struct ssh_transport *from, struct ssh_transport *to, struct inbox *inbox, int *progress);
static int pump(void);
static int connect_pair(void);
static void move_all(struct ssh_transport *from, struct ssh_transport *to);
static void frame_plain(struct wire *out, const uint8_t *payload, size_t size);
static void kexinit_wire(struct wire *out, int server_side, const char *kex, const char *cipher, int follows);
static void feed(struct ssh_transport *transport, const struct wire *wire);
static void feed_text(struct ssh_transport *transport, const char *text);
static size_t read_plain(struct ssh_transport *transport, int skip_line);
static uint32_t plain_disconnect_code(void);
static int raw_send(struct ssh_transport *transport, const uint8_t *payload, size_t size);
static size_t raw_read(struct ssh_transport *from, struct ssh_transport *reader, uint8_t *out, size_t capacity);
static void make_message(uint8_t *out, size_t size, unsigned number, unsigned index);
static void test_connect(void);
static void test_early_send(void);
static void test_send_errors(void);
static void test_peer_rekey_held(void);
static void test_held_full(void);
static void test_output_full(void);
static void test_held_running(void);
static void test_output_reserve(void);
static void test_mac_error(void);
static void test_rekey_time(void);
static void test_rekey_counters(void);
static void test_rekey_peer_messages(void);
static void test_strict(void);
static void test_strict_server(void);
static void test_wrap(void);
static void test_guess(void);
static void test_version(void);
static void test_exchange_errors(void);
static void test_kexinit_mismatch(void);
static void test_unimplemented(void);
static void test_kex_out_of_step(void);
static void test_disconnect(void);
static void test_export(void);
static void test_import_errors(void);
static void test_random(void);
static void test_random_running(void);

int
main(void)
{
	int error;

	/* Makes the server's host key. */
	error = ssh_key_generate(&host_key, "host");
	CHECK_EQ(error, 0);

	/* Runs each group of checks. */
	test_connect();
	test_early_send();
	test_send_errors();
	test_peer_rekey_held();
	test_held_full();
	test_output_full();
	test_held_running();
	test_output_reserve();
	test_mac_error();
	test_rekey_time();
	test_rekey_counters();
	test_rekey_peer_messages();
	test_strict();
	test_strict_server();
	test_wrap();
	test_guess();
	test_version();
	test_exchange_errors();
	test_kexinit_mismatch();
	test_unimplemented();
	test_kex_out_of_step();
	test_disconnect();
	test_export();
	test_import_errors();
	test_random();
	test_random_running();

	/* Prints the verdict. */
	ssh_key_clear(&host_key);
	return check_finish("transport");
}

/* Returns the next number of a xorshift generator. */
static uint32_t
next_random(void)
{
	/* One xorshift step. */
	random_state ^= random_state << 13;
	random_state ^= random_state >> 17;
	random_state ^= random_state << 5;
	return random_state;
}

/* Returns the FNV-1a hash of size bytes, to compare whole messages cheaply. */
static uint32_t
fnv(
	const uint8_t *data,
	size_t size)
{
	uint32_t hash;
	size_t index;

	/* Mixes each byte in. */
	hash = 0x811c9dc5U;
	for (index = 0; index < size; index++) {
		hash ^= data[index];
		hash *= 0x01000193U;
	}
	return hash;
}

/* Appends the server's public key blob (the host_key operation). */
static int
test_host_key(
	void *context,
	struct ssh_buf *out)
{
	struct test_context *test;

	test = context;
	test->host_key_calls++;
	if (test->fail_host_key != 0)
		return EIO;
	ssh_key_put_public_blob(test->key->public_key, out);
	return 0;
}

/* Signs H with the server's key, flipping one bit when asked (the sign operation). */
static int
test_sign(
	void *context,
	const uint8_t *h,
	struct ssh_buf *out)
{
	struct test_context *test;
	int error;

	test = context;
	test->sign_calls++;
	if (test->fail_sign != 0)
		return EIO;
	error = ssh_key_sign(test->key, h, SSH_KEX_HASH_SIZE, out);
	if (error != 0)
		return error;
	if (test->flip != 0 && out->size > 0)
		out->data[out->size - 1U] ^= 0x01U;
	return 0;
}

/* Records the server's key blob and accepts it, or refuses it when asked (the check_host_key operation). */
static int
test_check_host_key(
	void *context,
	const uint8_t *blob,
	size_t size)
{
	struct test_context *test;

	test = context;
	test->check_calls++;
	test->blob_size = 0;
	if (size <= sizeof(test->blob)) {
		memcpy(test->blob, blob, size);
		test->blob_size = size;
	}
	if (test->refuse != 0)
		return EACCES;
	if (test->fail != 0)
		return EIO;
	return 0;
}

/* Starts a fresh client and server with empty inboxes and default operations. */
static void
start_pair(void)
{
	/* Starts both sides. */
	start_one(&client, SSH_ROLE_CLIENT);
	start_one(&server, SSH_ROLE_SERVER);
}

/* Starts one side with the test's operations and an empty inbox. */
static void
start_one(
	struct ssh_transport *transport,
	enum ssh_role role)
{
	struct ssh_transport_ops ops;
	struct test_context *context;
	struct inbox *inbox;
	int error;

	/* Chooses the side's context and inbox. */
	context = &client_context;
	inbox = &client_inbox;
	if (role == SSH_ROLE_SERVER) {
		context = &server_context;
		inbox = &server_inbox;
	}
	memset(context, 0, sizeof(*context));
	context->key = &host_key;
	memset(inbox, 0, sizeof(*inbox));

	/* Gives the server its key operations and the client its check. */
	memset(&ops, 0, sizeof(ops));
	ops.context = context;
	if (role == SSH_ROLE_SERVER) {
		ops.host_key = test_host_key;
		ops.sign = test_sign;
	} else {
		ops.check_host_key = test_check_host_key;
	}
	error = ssh_transport_init(transport, role, &ops, TEST_NOW);
	CHECK_EQ(error, 0);
}

/* Frees both sides. */
static void
stop_pair(void)
{
	/* Wipes and frees both. */
	ssh_transport_free(&client);
	ssh_transport_free(&server);
}

/* Takes messages from one side until it needs more input, recording them; returns the last answer. */
static int
step(
	struct ssh_transport *transport,
	struct inbox *inbox)
{
	struct ssh_buf payload;
	size_t index;
	size_t head;
	int error;

	/* Reads until the side stops delivering. */
	ssh_buf_init(&payload, SSH_PACKET_LIMIT, 1);
	for (;;) {
		error = ssh_transport_next(transport, &payload);
		if (error != 0)
			break;
		CHECK(payload.size > 0);
		if (inbox == NULL || inbox->count >= INBOX_MAX)
			continue;
		index = inbox->count;
		head = payload.size;
		if (head > INBOX_HEAD)
			head = INBOX_HEAD;
		memset(inbox->head[index], 0, INBOX_HEAD);
		memcpy(inbox->head[index], payload.data, head);
		inbox->size[index] = payload.size;
		inbox->hash[index] = fnv(payload.data, payload.size);
		inbox->count++;
	}
	ssh_buf_free(&payload);
	return error;
}

/* Moves one chunk of from's output into to's input and lets to read it; returns to's error other than EAGAIN. */
static int
transfer(
	struct ssh_transport *from,
	struct ssh_transport *to,
	struct inbox *inbox,
	int *progress)
{
	const uint8_t *pending;
	size_t size;
	size_t room;
	int error;

	/* Moves what fits. */
	pending = ssh_transport_pending(from, &size);
	if (size > 0 && to->state != SSH_TRANSPORT_CLOSED) {
		room = SSH_PACKET_INPUT_MAX - to->packet.input.size;
		if (size > room)
			size = room;
		if (size > MOVE_CHUNK)
			size = MOVE_CHUNK;
		if (size > 0) {
			error = ssh_transport_input(to, pending, size);
			if (error != 0)
				return error;
			ssh_transport_written(from, size);
			*progress = 1;
		}
	}

	/* Lets the receiver act on it. */
	if (to->state == SSH_TRANSPORT_CLOSED)
		return 0;
	error = step(to, inbox);
	if (error != EAGAIN)
		return error;
	return 0;
}

/* Moves bytes both ways until nothing moves; returns 0 or the first error a side answered. */
static int
pump(void)
{
	unsigned round;
	int progress;
	int error;

	/* Goes back and forth until both sides are quiet. */
	for (round = 0; round < 100000U; round++) {
		progress = 0;
		error = transfer(&client, &server, &server_inbox, &progress);
		if (error != 0)
			return error;
		error = transfer(&server, &client, &client_inbox, &progress);
		if (error != 0)
			return error;
		if (progress == 0)
			return 0;
	}
	return ETIMEDOUT;
}

/* Starts a pair and runs the initial exchange; returns pump's answer. */
static int
connect_pair(void)
{
	int error;

	/* Starts and pumps. */
	start_pair();
	error = pump();
	CHECK_EQ(error, 0);
	CHECK_EQ(client.state, SSH_TRANSPORT_RUNNING);
	CHECK_EQ(server.state, SSH_TRANSPORT_RUNNING);
	return error;
}

/* Moves all of from's output into to's input without letting to read it. */
static void
move_all(
	struct ssh_transport *from,
	struct ssh_transport *to)
{
	const uint8_t *pending;
	size_t size;
	int error;

	/* Feeds the whole output. */
	pending = ssh_transport_pending(from, &size);
	if (size == 0)
		return;
	error = ssh_transport_input(to, pending, size);
	CHECK_EQ(error, 0);
	ssh_transport_written(from, size);
}

/*
 * Appends one unkeyed packet (RFC 4253 6): packet_length, padding_length,
 * payload, padding of 4 to 11 bytes so the whole is a multiple of 8.
 */
static void
frame_plain(
	struct wire *out,
	const uint8_t *payload,
	size_t size)
{
	static const uint8_t padding[16] = { 0 };
	size_t pad;

	/* Pads to a multiple of 8 with at least 4 bytes. */
	pad = 8U - ((4U + 1U + size) % 8U);
	if (pad < 4U)
		pad += 8U;
	wire_u32(out, (uint32_t)(1U + size + pad));
	wire_u8(out, (unsigned)pad);
	wire_bytes(out, payload, size);
	wire_bytes(out, padding, pad);
}

/*
 * Builds a KEXINIT payload (RFC 4253 7.1): number 20, a cookie, ten
 * name-lists, first_kex_packet_follows and the reserved uint32.
 */
static void
kexinit_wire(
	struct wire *out,
	int server_side,
	const char *kex,
	const char *cipher,
	int follows)
{
	static const uint8_t cookie[16] = {
		0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
		0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x01
	};

	/* The fields in order. */
	(void)server_side;
	wire_reset(out);
	wire_u8(out, 20);
	wire_bytes(out, cookie, sizeof(cookie));
	wire_text(out, kex);
	wire_text(out, "ssh-ed25519");
	wire_text(out, cipher);
	wire_text(out, cipher);
	wire_text(out, "");
	wire_text(out, "");
	wire_text(out, "none");
	wire_text(out, "none");
	wire_text(out, "");
	wire_text(out, "");
	wire_bool(out, (unsigned)follows);
	wire_u32(out, 0);
}

/* Feeds the bytes of a wire buffer to a side. */
static void
feed(
	struct ssh_transport *transport,
	const struct wire *wire)
{
	int error;

	/* Appends them. */
	CHECK(wire->overflow == 0);
	error = ssh_transport_input(transport, wire->data, wire->size);
	CHECK_EQ(error, 0);
}

/* Feeds a text to a side. */
static void
feed_text(
	struct ssh_transport *transport,
	const char *text)
{
	int error;

	/* Appends it without its NUL. */
	error = ssh_transport_input(transport, (const uint8_t *)text, strlen(text));
	CHECK_EQ(error, 0);
}

/*
 * Reads a side's unkeyed output into plain_out (skipping its
 * identification line first when skip_line is 1), and drops the output.
 * Returns the number of payloads read.
 */
static size_t
read_plain(
	struct ssh_transport *transport,
	int skip_line)
{
	struct ssh_packet reader;
	struct ssh_buf payload;
	const uint8_t *pending;
	char line[256];
	size_t length;
	size_t size;
	int error;

	/* Copies the output into a packet layer of its own. */
	memset(&plain_out, 0, sizeof(plain_out));
	ssh_packet_init(&reader);
	pending = ssh_transport_pending(transport, &size);
	if (size > 0) {
		error = ssh_packet_feed(&reader, pending, size);
		CHECK_EQ(error, 0);
		ssh_transport_written(transport, size);
	}

	/* Skips the identification line. */
	if (skip_line != 0) {
		error = ssh_packet_take_line(&reader, line, sizeof(line), &length);
		CHECK_EQ(error, 0);
		CHECK(strcmp(line, SSH_VERSION_STRING) == 0);
	}

	/* Takes the packets. */
	ssh_buf_init(&payload, SSH_PACKET_LIMIT, 0);
	while (plain_out.count < PLAIN_MAX) {
		ssh_buf_reset(&payload);
		error = ssh_packet_next(&reader, &payload);
		if (error != 0)
			break;
		size = payload.size;
		if (size > PLAIN_SIZE)
			size = PLAIN_SIZE;
		memcpy(plain_out.data[plain_out.count], payload.data, size);
		plain_out.size[plain_out.count] = payload.size;
		plain_out.count++;
	}
	ssh_buf_free(&payload);
	ssh_packet_free(&reader);
	return plain_out.count;
}

/* Returns the reason code of the last payload read when it is a DISCONNECT, or 0xffffffff. */
static uint32_t
plain_disconnect_code(void)
{
	const uint8_t *last;

	/* Looks at the last payload. */
	if (plain_out.count == 0)
		return 0xffffffffU;
	last = plain_out.data[plain_out.count - 1U];
	if (plain_out.size[plain_out.count - 1U] < 5U || last[0] != SSH_MSG_DISCONNECT)
		return 0xffffffffU;
	return ssh_load_be32(last + 1);
}

/* Frames a payload with a side's packet layer directly, bypassing the transport's checks. */
static int
raw_send(
	struct ssh_transport *transport,
	const uint8_t *payload,
	size_t size)
{
	int error;

	/* Frames it with the current key. */
	error = ssh_packet_send(&transport->packet, payload, size);
	return error;
}

/*
 * Reads one keyed packet of from's output with reader's packet layer
 * (which holds the matching receiving key) and drops from's output.
 * Returns the payload size, or 0 when nothing came.
 */
static size_t
raw_read(
	struct ssh_transport *from,
	struct ssh_transport *reader,
	uint8_t *out,
	size_t capacity)
{
	struct ssh_buf payload;
	const uint8_t *pending;
	size_t size;
	int error;

	/* Moves the output into the reader's packet layer. */
	pending = ssh_transport_pending(from, &size);
	if (size == 0)
		return 0;
	error = ssh_packet_feed(&reader->packet, pending, size);
	CHECK_EQ(error, 0);
	ssh_transport_written(from, size);

	/* Takes one packet. */
	ssh_buf_init(&payload, SSH_PACKET_LIMIT, 0);
	error = ssh_packet_next(&reader->packet, &payload);
	CHECK_EQ(error, 0);
	size = payload.size;
	if (size > capacity)
		size = capacity;
	if (size > 0)
		memcpy(out, payload.data, size);
	size = payload.size;
	ssh_buf_free(&payload);
	return size;
}

/* Fills a message: its number, then bytes that depend on index and position. */
static void
make_message(
	uint8_t *out,
	size_t size,
	unsigned number,
	unsigned index)
{
	size_t position;

	/* The number first, then the pattern. */
	out[0] = (uint8_t)number;
	for (position = 1; position < size; position++)
		out[position] = (uint8_t)(index * 7U + position * 13U);
}

/*
 * The initial exchange: both sides RUNNING with the same session
 * identifier, the host key seen by the client, the strict reset of the
 * sequence numbers, and upper-layer messages both ways.
 */
static void
test_connect(void)
{
	static const unsigned client_numbers[4] = { 5, 50, 94, 200 };
	static const unsigned server_numbers[4] = { 6, 60, 94, 255 };
	uint8_t message[100];
	unsigned index;
	int error;

	/* Runs the exchange. */
	connect_pair();
	CHECK_EQ(client.kex_step, 0);
	CHECK_EQ(server.kex_step, 0);
	CHECK_EQ(client.initial_done, 1);
	CHECK_EQ(server.initial_done, 1);
	CHECK_EQ(client.have_session_id, 1);
	CHECK_EQ(server.have_session_id, 1);
	CHECK_BYTES(client.session_id, server.session_id, SSH_KEX_HASH_SIZE);
	CHECK_EQ(client.choice.strict, 1);
	CHECK_EQ(server.choice.strict, 1);
	CHECK(strcmp(client.version_peer, SSH_VERSION_STRING) == 0);
	CHECK(strcmp(server.version_peer, SSH_VERSION_STRING) == 0);
	CHECK_EQ(client.kex_time, TEST_NOW);

	/* The client checked the server's blob: string "ssh-ed25519", string of the 32-byte key (RFC 8709 4). */
	wire_reset(&message_wire);
	wire_text(&message_wire, "ssh-ed25519");
	wire_string(&message_wire, host_key.public_key, SSH_ED25519_PUBLIC);
	CHECK_EQ(client_context.check_calls, 1);
	CHECK_EQ(client_context.blob_size, message_wire.size);
	CHECK_BYTES(client_context.blob, message_wire.data, message_wire.size);
	CHECK_EQ(server_context.sign_calls, 1);
	CHECK_EQ(server_context.host_key_calls, 1);

	/* Strict key exchange set every sequence number to 0 at NEWKEYS, and NEWKEYS was the last packet each way. */
	CHECK_EQ(client.packet.send.seq, 0);
	CHECK_EQ(client.packet.receive.seq, 0);
	CHECK_EQ(server.packet.send.seq, 0);
	CHECK_EQ(server.packet.receive.seq, 0);
	CHECK_EQ(client.packet.send.keyed, 1);
	CHECK_EQ(client.packet.receive.keyed, 1);
	CHECK_BYTES(client.packet.send.cipher.main_key, server.packet.receive.cipher.main_key, SSH_CHACHA20_KEY);
	CHECK_BYTES(server.packet.send.cipher.header_key, client.packet.receive.cipher.header_key, SSH_CHACHA20_KEY);
	CHECK(memcmp(client.packet.send.cipher.main_key, client.packet.receive.cipher.main_key, SSH_CHACHA20_KEY) != 0);

	/* The secrets of the exchange are gone. */
	for (index = 0; index < SSH_KEX_HASH_SIZE; index++) {
		CHECK_EQ(client.k[index], 0);
		CHECK_EQ(server.h[index], 0);
	}
	for (index = 0; index < SSH_HYBRID_INIT_SIZE; index++) {
		if (client.c_init[index] != 0)
			break;
	}
	CHECK_EQ(index, SSH_HYBRID_INIT_SIZE);

	/* Sends upper-layer messages both ways. */
	for (index = 0; index < 4U; index++) {
		make_message(message, sizeof(message) - index, client_numbers[index], index);
		error = ssh_transport_send(&client, message, sizeof(message) - index);
		CHECK_EQ(error, 0);
		make_message(message, 10U + index, server_numbers[index], index + 10U);
		error = ssh_transport_send(&server, message, 10U + index);
		CHECK_EQ(error, 0);
	}
	error = pump();
	CHECK_EQ(error, 0);

	/* Each arrived whole and in order. */
	CHECK_EQ(server_inbox.count, 4);
	CHECK_EQ(client_inbox.count, 4);
	for (index = 0; index < 4U; index++) {
		make_message(message, sizeof(message) - index, client_numbers[index], index);
		CHECK_EQ(server_inbox.size[index], sizeof(message) - index);
		CHECK_EQ(server_inbox.hash[index], fnv(message, sizeof(message) - index));
		make_message(message, 10U + index, server_numbers[index], index + 10U);
		CHECK_EQ(client_inbox.size[index], 10U + index);
		CHECK_EQ(client_inbox.hash[index], fnv(message, 10U + index));
	}
	CHECK_EQ(client.packet.send.seq, 4);
	CHECK_EQ(server.packet.receive.seq, 4);
	stop_pair();
}

/* Messages sent before the initial exchange ends are held and come out after this side's NEWKEYS. */
static void
test_early_send(void)
{
	static const uint8_t request[] = { 5, 0, 0, 0, 12, 's', 's', 'h', '-', 'u', 's', 'e', 'r', 'a', 'u', 't', 'h' };
	uint8_t accept[3];
	size_t size;
	int error;

	/* Sends before anything was received: only the identification line is in the output. */
	start_pair();
	error = ssh_transport_send(&client, request, sizeof(request));
	CHECK_EQ(error, 0);
	(void)ssh_transport_pending(&client, &size);
	CHECK_EQ(size, strlen(SSH_VERSION_STRING) + 2U);
	CHECK_EQ(client.held.size, 4U + sizeof(request));
	accept[0] = 6;
	accept[1] = 1;
	accept[2] = 2;
	error = ssh_transport_send(&server, accept, sizeof(accept));
	CHECK_EQ(error, 0);

	/* Runs the exchange: the held messages arrive after it. */
	error = pump();
	CHECK_EQ(error, 0);
	CHECK_EQ(server_inbox.count, 1);
	CHECK_EQ(server_inbox.size[0], sizeof(request));
	CHECK_BYTES(server_inbox.head[0], request, sizeof(request));
	CHECK_EQ(client_inbox.count, 1);
	CHECK_BYTES(client_inbox.head[0], accept, sizeof(accept));
	CHECK_EQ(client.held.size, 0);
	CHECK_EQ(server.held.size, 0);
	stop_pair();
}

/* The send arguments the transport refuses, and the boundaries it takes. */
static void
test_send_errors(void)
{
	static const unsigned refused[] = { 0, 1, 2, 3, 4, 7, 8, 19, 20, 21, 29, 30, 31, 49 };
	static const unsigned taken[] = { 5, 6, 50, 79, 80, 127, 128, 191, 192, 255 };
	uint8_t message[4];
	uint8_t *large;
	unsigned index;
	int error;

	connect_pair();

	/* Refuses a missing or empty payload. */
	error = ssh_transport_send(&client, NULL, 1);
	CHECK_EQ(error, EINVAL);
	message[0] = 50;
	error = ssh_transport_send(&client, message, 0);
	CHECK_EQ(error, EINVAL);

	/* Refuses the transport's own numbers and takes 5, 6 and 50 and above. */
	for (index = 0; index < sizeof(refused) / sizeof(refused[0]); index++) {
		message[0] = (uint8_t)refused[index];
		error = ssh_transport_send(&client, message, sizeof(message));
		CHECK_EQ(error, EINVAL);
	}
	for (index = 0; index < sizeof(taken) / sizeof(taken[0]); index++) {
		message[0] = (uint8_t)taken[index];
		error = ssh_transport_send(&client, message, sizeof(message));
		CHECK_EQ(error, 0);
	}

	/* Takes the largest payload a packet carries and refuses one byte more. */
	large = malloc(SSH_PACKET_LIMIT);
	CHECK(large != NULL);
	if (large != NULL) {
		make_message(large, SSH_PACKET_LIMIT - 256U, 94, 1);
		error = ssh_transport_send(&client, large, SSH_PACKET_LIMIT - 256U);
		CHECK_EQ(error, 0);
		error = ssh_transport_send(&client, large, SSH_PACKET_LIMIT - 255U);
		CHECK_EQ(error, EINVAL);
	}
	error = pump();
	CHECK_EQ(error, 0);
	/* The reserved 128 and 191 are taken for sending but answered with UNIMPLEMENTED by the receiver (RFC 4250 4.1.2). */
	CHECK_EQ(server_inbox.count, sizeof(taken) / sizeof(taken[0]) - 2U + 1U);
	CHECK_EQ(server_inbox.head[6][0], 192);
	if (large != NULL) {
		CHECK_EQ(server_inbox.size[server_inbox.count - 1U], SSH_PACKET_LIMIT - 256U);
		CHECK_EQ(server_inbox.hash[server_inbox.count - 1U], fnv(large, SSH_PACKET_LIMIT - 256U));
	}
	free(large);

	/* Refuses everything after a disconnect, as EPIPE. */
	ssh_transport_disconnect(&client, SSH_DISCONNECT_BY_APPLICATION, "done");
	CHECK_EQ(client.state, SSH_TRANSPORT_CLOSED);
	message[0] = 50;
	error = ssh_transport_send(&client, message, sizeof(message));
	CHECK_EQ(error, EPIPE);
	error = ssh_transport_input(&client, message, sizeof(message));
	CHECK_EQ(error, EPIPE);
	error = step(&client, NULL);
	CHECK_EQ(error, EPIPE);
	stop_pair();
}

/*
 * A re-exchange the server starts: the client, between its KEXINIT and its
 * NEWKEYS, holds what it sends; it goes out after its NEWKEYS in order.
 */
static void
test_peer_rekey_held(void)
{
	uint8_t old_send[SSH_CHACHA20_KEY];
	uint8_t session[SSH_KEX_HASH_SIZE];
	uint8_t message[200];
	size_t before;
	size_t after;
	unsigned index;
	int error;

	connect_pair();
	memcpy(session, client.session_id, sizeof(session));
	memcpy(old_send, client.packet.send.cipher.main_key, sizeof(old_send));

	/* The server starts at the hour. */
	error = ssh_transport_tick(&server, TEST_NOW + SSH_TRANSPORT_REKEY_SECONDS);
	CHECK_EQ(error, 0);
	CHECK(server.kex_step != 0);

	/* The client takes the server's KEXINIT and answers with its own and its half. */
	move_all(&server, &client);
	error = step(&client, &client_inbox);
	CHECK_EQ(error, EAGAIN);
	CHECK(client.kex_step != 0);

	/* The client's sends are held: its output does not grow. */
	(void)ssh_transport_pending(&client, &before);
	for (index = 0; index < 5U; index++) {
		make_message(message, sizeof(message) - index, 90U + index, index);
		error = ssh_transport_send(&client, message, sizeof(message) - index);
		CHECK_EQ(error, 0);
	}
	(void)ssh_transport_pending(&client, &after);
	CHECK_EQ(after, before);
	CHECK(client.held.size > 0);

	/* The server's sends during its own exchange are held too. */
	make_message(message, 20, 70, 99);
	error = ssh_transport_send(&server, message, 20);
	CHECK_EQ(error, 0);
	CHECK(server.held.size > 0);

	/* Finishes: everything arrives in order, under the new keys, with the same session identifier. */
	error = pump();
	CHECK_EQ(error, 0);
	CHECK_EQ(client.kex_step, 0);
	CHECK_EQ(server.kex_step, 0);
	CHECK_EQ(server_inbox.count, 5);
	for (index = 0; index < 5U && index < server_inbox.count; index++) {
		make_message(message, sizeof(message) - index, 90U + index, index);
		CHECK_EQ(server_inbox.hash[index], fnv(message, sizeof(message) - index));
	}
	CHECK_EQ(client_inbox.count, 1);
	make_message(message, 20, 70, 99);
	CHECK_EQ(client_inbox.hash[0], fnv(message, 20));
	CHECK_BYTES(client.session_id, session, sizeof(session));
	CHECK_BYTES(server.session_id, session, sizeof(session));
	CHECK(memcmp(client.packet.send.cipher.main_key, old_send, sizeof(old_send)) != 0);
	CHECK_BYTES(client.packet.send.cipher.main_key, server.packet.receive.cipher.main_key, SSH_CHACHA20_KEY);
	CHECK_EQ(server.kex_time, TEST_NOW + SSH_TRANSPORT_REKEY_SECONDS);
	CHECK_EQ(client.held.size, 0);
	stop_pair();
}

/* The held messages fill to SSH_TRANSPORT_HELD_MAX exactly, then ENOBUFS adds nothing; all arrive later. */
static void
test_held_full(void)
{
	static uint8_t message[SSH_PAYLOAD_SEND_MAX];
	size_t held;
	size_t last;
	unsigned count;
	unsigned index;
	int error;

	connect_pair();

	/* The client starts a re-exchange, so its sends are held. */
	client.packet.send.bytes = SSH_TRANSPORT_REKEY_BYTES;
	error = ssh_transport_tick(&client, TEST_NOW);
	CHECK_EQ(error, 0);
	CHECK(client.kex_step != 0);

	/* Fills the held room with whole messages. */
	count = 0;
	for (;;) {
		make_message(message, sizeof(message), 94, count);
		error = ssh_transport_send(&client, message, sizeof(message));
		if (error != 0)
			break;
		count++;
	}
	CHECK_EQ(error, ENOBUFS);
	CHECK_EQ(count, SSH_TRANSPORT_HELD_MAX / (4U + sizeof(message)));

	/* The remaining room takes one payload of exactly its size less the length field, and not one byte more. */
	held = client.held.size;
	last = SSH_TRANSPORT_HELD_MAX - held - 4U;
	make_message(message, last + 1U, 94, count);
	error = ssh_transport_send(&client, message, last + 1U);
	CHECK_EQ(error, ENOBUFS);
	CHECK_EQ(client.held.size, held);
	make_message(message, last, 94, count);
	error = ssh_transport_send(&client, message, last);
	CHECK_EQ(error, 0);
	CHECK_EQ(client.held.size, SSH_TRANSPORT_HELD_MAX);
	message[0] = 94;
	error = ssh_transport_send(&client, message, 1);
	CHECK_EQ(error, ENOBUFS);
	CHECK_EQ(client.held.size, SSH_TRANSPORT_HELD_MAX);

	/* Finishes the exchange; the held messages arrive in order even though they do not fit the output at once. */
	error = pump();
	CHECK_EQ(error, 0);
	CHECK_EQ(server_inbox.count, count + 1U);
	for (index = 0; index < count && index < server_inbox.count; index++) {
		make_message(message, sizeof(message), 94, index);
		CHECK_EQ(server_inbox.hash[index], fnv(message, sizeof(message)));
	}
	make_message(message, last, 94, count);
	CHECK_EQ(server_inbox.size[count], last);
	CHECK_EQ(server_inbox.hash[count], fnv(message, last));
	CHECK_EQ(client.held.size, 0);
	stop_pair();
}

/* The output fills (outside the transport's reserve), ENOBUFS adds nothing, and writing makes room again. */
static void
test_output_full(void)
{
	static uint8_t message[SSH_PAYLOAD_SEND_MAX];
	size_t before;
	size_t after;
	unsigned count;
	unsigned index;
	int error;

	connect_pair();

	/* Sends without writing until the output is full. */
	count = 0;
	for (;;) {
		make_message(message, sizeof(message), 94, count);
		error = ssh_transport_send(&client, message, sizeof(message));
		if (error != 0)
			break;
		count++;
	}
	CHECK_EQ(error, ENOBUFS);
	CHECK(count > 100U);
	(void)ssh_transport_pending(&client, &before);
	CHECK(before <= SSH_PACKET_OUTPUT_MAX);
	CHECK(before > SSH_PACKET_OUTPUT_MAX - 128U * 1024U);
	message[0] = 94;
	error = ssh_transport_send(&client, message, 1);
	if (error == 0)
		count++;
	(void)ssh_transport_pending(&client, &after);

	/* The transport still has room for its own messages: a re-exchange starts. */
	client.packet.send.packets = SSH_TRANSPORT_REKEY_PACKETS;
	error = ssh_transport_tick(&client, TEST_NOW);
	CHECK_EQ(error, 0);
	CHECK(client.kex_step != 0);
	CHECK_EQ(client.state, SSH_TRANSPORT_RUNNING);

	/* Everything arrives once written. */
	error = pump();
	CHECK_EQ(error, 0);
	CHECK_EQ(server_inbox.count, count);
	for (index = 0; index < 100U && index < server_inbox.count; index++) {
		make_message(message, sizeof(message), 94, index);
		CHECK_EQ(server_inbox.hash[index], fnv(message, sizeof(message)));
	}
	CHECK_EQ(client.kex_step, 0);
	stop_pair();
}

/* Messages still held while RUNNING (output that was full) keep their place before new ones and go out on written. */
static void
test_held_running(void)
{
	static const uint8_t first[] = { 94, 1, 2, 3 };
	static const uint8_t second[] = { 95, 4, 5 };
	struct ssh_buf out;
	size_t before;
	size_t after;
	unsigned index;
	int error;

	connect_pair();

	/* Leaves one message held, as a flush that ran out of room would. */
	ssh_buf_put_u32(&client.held, sizeof(first));
	ssh_buf_put_bytes(&client.held, first, sizeof(first));
	CHECK_EQ(client.held.error, 0);

	/* Export refuses, and a new message goes behind the held one. */
	ssh_buf_init(&out, 4096, 1);
	error = ssh_transport_export(&client, &out);
	CHECK_EQ(error, EBUSY);
	ssh_buf_free(&out);
	(void)ssh_transport_pending(&client, &before);
	error = ssh_transport_send(&client, second, sizeof(second));
	CHECK_EQ(error, 0);
	(void)ssh_transport_pending(&client, &after);
	CHECK_EQ(after, before);
	CHECK_EQ(client.held.size, 4U + sizeof(first) + 4U + sizeof(second));

	/* written sends both, in order. */
	ssh_transport_written(&client, 0);
	CHECK_EQ(client.held.size, 0);
	error = pump();
	CHECK_EQ(error, 0);
	CHECK_EQ(server_inbox.count, 2);
	CHECK_BYTES(server_inbox.head[0], first, sizeof(first));
	CHECK_BYTES(server_inbox.head[1], second, sizeof(second));
	stop_pair();

	/* A held 4 MiB flushes only up to the upper layer's limit; the rest goes out as the output is written. */
	connect_pair();
	for (index = 0; index < 127U; index++) {
		make_message(big_payload, sizeof(big_payload), 94, index);
		ssh_buf_put_u32(&client.held, sizeof(big_payload));
		ssh_buf_put_bytes(&client.held, big_payload, sizeof(big_payload));
	}
	CHECK_EQ(client.held.error, 0);
	ssh_transport_written(&client, 0);
	(void)ssh_transport_pending(&client, &after);
	CHECK(after <= SSH_PACKET_OUTPUT_MAX - 64U * 1024U);
	CHECK(client.held.size > 0);
	error = pump();
	CHECK_EQ(error, 0);
	CHECK_EQ(client.held.size, 0);
	CHECK_EQ(server_inbox.count, 127);
	for (index = 0; index < 127U && index < server_inbox.count; index++) {
		make_message(big_payload, sizeof(big_payload), 94, index);
		CHECK_EQ(server_inbox.hash[index], fnv(big_payload, sizeof(big_payload)));
	}
	stop_pair();
}

/*
 * The transport's own messages may use the reserve the upper layer may
 * not; once the output passes even SSH_PACKET_OUTPUT_MAX, an answer that
 * cannot be queued ends the connection with EMSGSIZE.
 */
static void
test_output_reserve(void)
{
	static uint8_t message[SSH_PAYLOAD_SEND_MAX];
	uint8_t unknown[4];
	unsigned count;
	size_t size;
	size_t room;
	int error;

	connect_pair();

	/* Fills the upper layer's part of the output with large, then single-byte, messages. */
	make_message(message, sizeof(message), 94, 0);
	for (count = 0; count < 1000U; count++) {
		error = ssh_transport_send(&client, message, sizeof(message));
		if (error != 0)
			break;
	}
	CHECK_EQ(error, ENOBUFS);

	/*
	 * The boundary: a payload is taken when it and the most framing
	 * (TRANSPORT_FRAME_EXTRA, 37 bytes) fit under the limit.
	 */
	(void)ssh_transport_pending(&client, &size);
	room = SSH_PACKET_OUTPUT_MAX - 64U * 1024U - size;
	CHECK(room > 37U && room < sizeof(message));
	error = ssh_transport_send(&client, message, room - 36U);
	CHECK_EQ(error, ENOBUFS);
	error = ssh_transport_send(&client, message, room - 37U);
	CHECK_EQ(error, 0);
	for (count = 0; count < 100000U; count++) {
		error = ssh_transport_send(&client, message, 1);
		if (error != 0)
			break;
	}
	CHECK_EQ(error, ENOBUFS);

	/* No framed message went past the upper layer's limit. */
	(void)ssh_transport_pending(&client, &size);
	CHECK(size <= SSH_PACKET_OUTPUT_MAX - 64U * 1024U);
	CHECK(size > SSH_PACKET_OUTPUT_MAX - 64U * 1024U - 64U);

	/* Unknown messages are still answered, past the upper layer's limit, and the upper layer stays refused. */
	memset(unknown, 0, sizeof(unknown));
	unknown[0] = 8;
	for (count = 0; count < 4U; count++) {
		error = raw_send(&server, unknown, sizeof(unknown));
		CHECK_EQ(error, 0);
		move_all(&server, &client);
		error = step(&client, NULL);
		CHECK_EQ(error, EAGAIN);
	}
	(void)ssh_transport_pending(&client, &size);
	CHECK(size > SSH_PACKET_OUTPUT_MAX - 64U * 1024U);
	error = ssh_transport_send(&client, message, 1);
	CHECK_EQ(error, ENOBUFS);

	/* A flood of unknown messages fills the reserve; the answer that does not fit ends it. */
	error = EAGAIN;
	for (count = 0; count < 10000U && error == EAGAIN; count++) {
		error = raw_send(&server, unknown, sizeof(unknown));
		CHECK_EQ(error, 0);
		move_all(&server, &client);
		error = step(&client, NULL);
	}
	CHECK_EQ(error, EMSGSIZE);
	CHECK_EQ(client.state, SSH_TRANSPORT_CLOSED);
	CHECK(count > 1000U);
	(void)ssh_transport_pending(&client, &size);
	CHECK(size <= SSH_PACKET_OUTPUT_MAX);
	stop_pair();
}

/* A keyed packet with a changed byte is a MAC error: EBADMSG and DISCONNECT 5 (RFC 4253 6.4). */
static void
test_mac_error(void)
{
	static const uint8_t message[] = { 94, 1, 2, 3, 4 };
	uint8_t answer[300];
	size_t size;
	int error;

	connect_pair();

	/* Changes the last byte of the tag. */
	error = ssh_transport_send(&server, message, sizeof(message));
	CHECK_EQ(error, 0);
	server.packet.output.data[server.packet.output.size - 1U] ^= 0x01U;
	move_all(&server, &client);
	error = step(&client, &client_inbox);
	CHECK_EQ(error, EBADMSG);
	CHECK_EQ(client.state, SSH_TRANSPORT_CLOSED);
	CHECK_EQ(client_inbox.count, 0);

	/* The client said why, under its key. */
	size = raw_read(&client, &server, answer, sizeof(answer));
	CHECK(size >= 5U);
	CHECK_EQ(answer[0], 1);
	CHECK_EQ(ssh_load_be32(answer + 1), SSH_DISCONNECT_MAC_ERROR);
	stop_pair();
}

/* The hour of RFC 4253 9: nothing at 3599 seconds, a re-exchange at 3600, nothing for a clock gone back. */
static void
test_rekey_time(void)
{
	uint8_t session[SSH_KEX_HASH_SIZE];
	uint8_t old_receive[SSH_CHACHA20_KEY];
	size_t size;
	int error;

	connect_pair();
	memcpy(session, client.session_id, sizeof(session));
	memcpy(old_receive, client.packet.receive.cipher.header_key, sizeof(old_receive));

	/* Not yet, and not for a time before the last key. */
	error = ssh_transport_tick(&client, TEST_NOW - 1U);
	CHECK_EQ(error, 0);
	CHECK_EQ(client.kex_step, 0);
	error = ssh_transport_tick(&client, TEST_NOW + SSH_TRANSPORT_REKEY_SECONDS - 1U);
	CHECK_EQ(error, 0);
	CHECK_EQ(client.kex_step, 0);
	CHECK(ssh_transport_pending(&client, &size) == NULL);
	CHECK_EQ(size, 0);

	/* At the hour. */
	error = ssh_transport_tick(&client, TEST_NOW + SSH_TRANSPORT_REKEY_SECONDS);
	CHECK_EQ(error, 0);
	CHECK(client.kex_step != 0);

	/* A second tick during the exchange changes nothing. */
	error = ssh_transport_tick(&client, TEST_NOW + 2U * SSH_TRANSPORT_REKEY_SECONDS);
	CHECK_EQ(error, 0);

	/* Finishes with new keys and the same session identifier; the clock restarts at the tick's time. */
	error = pump();
	CHECK_EQ(error, 0);
	CHECK_EQ(client.kex_step, 0);
	CHECK_EQ(client.kex_time, TEST_NOW + 2U * SSH_TRANSPORT_REKEY_SECONDS);
	CHECK_BYTES(client.session_id, session, sizeof(session));
	CHECK(memcmp(client.packet.receive.cipher.header_key, old_receive, sizeof(old_receive)) != 0);
	CHECK_EQ(client.packet.send.bytes < 4096U, 1);

	/* The new keys carry messages. */
	big_payload[0] = 80;
	error = ssh_transport_send(&server, big_payload, 100);
	CHECK_EQ(error, 0);
	error = pump();
	CHECK_EQ(error, 0);
	CHECK_EQ(client_inbox.count, 1);
	stop_pair();
}

/* The byte and packet limits of each direction start a re-exchange at the limit and not one below. */
static void
test_rekey_counters(void)
{
	unsigned which;
	uint64_t *counter;
	uint64_t limit;
	int error;

	connect_pair();

	/* Each of the four counters in turn. */
	for (which = 0; which < 4U; which++) {
		counter = &client.packet.send.bytes;
		limit = SSH_TRANSPORT_REKEY_BYTES;
		if (which == 1U) {
			counter = &client.packet.receive.bytes;
		} else if (which == 2U) {
			counter = &client.packet.send.packets;
			limit = SSH_TRANSPORT_REKEY_PACKETS;
		} else if (which == 3U) {
			counter = &client.packet.receive.packets;
			limit = SSH_TRANSPORT_REKEY_PACKETS;
		}
		*counter = limit - 1U;
		error = ssh_transport_tick(&client, TEST_NOW);
		CHECK_EQ(error, 0);
		CHECK_EQ(client.kex_step, 0);
		*counter = limit;
		error = ssh_transport_tick(&client, TEST_NOW);
		CHECK_EQ(error, 0);
		CHECK(client.kex_step != 0);
		error = pump();
		CHECK_EQ(error, 0);
		CHECK_EQ(client.kex_step, 0);
		CHECK_EQ(server.kex_step, 0);
		CHECK(*counter < limit);
	}

	/* The server's counters too. */
	server.packet.receive.packets = SSH_TRANSPORT_REKEY_PACKETS;
	error = ssh_transport_tick(&server, TEST_NOW);
	CHECK_EQ(error, 0);
	CHECK(server.kex_step != 0);
	error = pump();
	CHECK_EQ(error, 0);
	CHECK_EQ(server.kex_step, 0);
	CHECK_BYTES(client.session_id, server.session_id, SSH_KEX_HASH_SIZE);

	/* Outside RUNNING the tick does nothing. */
	stop_pair();
	start_pair();
	client.packet.send.bytes = SSH_TRANSPORT_REKEY_BYTES;
	error = ssh_transport_tick(&client, TEST_NOW + 99999U);
	CHECK_EQ(error, 0);
	CHECK_EQ(client.kex_step, 0);
	CHECK_EQ(client.state, SSH_TRANSPORT_VERSION);
	stop_pair();
}

/*
 * During this side's exchange, the peer's upper-layer messages from
 * before its KEXINIT are delivered; one it sends between its KEXINIT and
 * its NEWKEYS is answered with UNIMPLEMENTED and dropped (RFC 4253 7.1).
 */
static void
test_rekey_peer_messages(void)
{
	uint8_t message[16];
	int error;

	connect_pair();

	/* The client starts; the server, not yet in the exchange, sends a message. */
	client.packet.send.bytes = SSH_TRANSPORT_REKEY_BYTES;
	error = ssh_transport_tick(&client, TEST_NOW);
	CHECK_EQ(error, 0);
	make_message(message, sizeof(message), 94, 5);
	error = ssh_transport_send(&server, message, sizeof(message));
	CHECK_EQ(error, 0);

	/* The client delivers it although its own exchange is in progress. */
	move_all(&server, &client);
	error = step(&client, &client_inbox);
	CHECK_EQ(error, EAGAIN);
	CHECK_EQ(client_inbox.count, 1);
	CHECK_EQ(client_inbox.hash[0], fnv(message, sizeof(message)));

	/* The server takes the client's KEXINIT; then the client sends an upper message out of turn (bypassing its hold). */
	move_all(&client, &server);
	error = step(&server, &server_inbox);
	CHECK_EQ(error, EAGAIN);
	error = raw_send(&client, message, sizeof(message));
	CHECK_EQ(error, 0);

	/* The server answers it with UNIMPLEMENTED and does not deliver it. */
	move_all(&client, &server);
	error = step(&server, &server_inbox);
	CHECK_EQ(error, EAGAIN);
	CHECK_EQ(server_inbox.count, 0);
	CHECK(server.state != SSH_TRANSPORT_CLOSED);

	/* The exchange still completes. */
	error = pump();
	CHECK_EQ(error, 0);
	CHECK_EQ(client.kex_step, 0);
	CHECK_EQ(server.kex_step, 0);
	CHECK_EQ(server_inbox.count, 0);
	stop_pair();
}

/* Strict key exchange on a client against a peer the test plays (draft-ietf-sshm-strict-kex-02 3.1, 3.2). */
static void
test_strict(void)
{
	static const uint8_t ignore[] = { 2, 0, 0, 0, 0 };
	static const uint8_t debug[] = { 4, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
	static const uint8_t newkeys[] = { 21 };
	int error;

	/* The first packet is IGNORE. */
	start_one(&client, SSH_ROLE_CLIENT);
	feed_text(&client, "SSH-2.0-peer\r\n");
	wire_reset(&frame_wire);
	frame_plain(&frame_wire, ignore, sizeof(ignore));
	feed(&client, &frame_wire);
	error = step(&client, NULL);
	CHECK_EQ(error, EPROTO);
	CHECK_EQ(client.state, SSH_TRANSPORT_CLOSED);
	CHECK_EQ(read_plain(&client, 1), 2);
	CHECK_EQ(plain_out.data[0][0], 20);
	CHECK_EQ(plain_disconnect_code(), SSH_DISCONNECT_PROTOCOL_ERROR);
	ssh_transport_free(&client);

	/* KEXINIT, then DEBUG inside the initial exchange. */
	start_one(&client, SSH_ROLE_CLIENT);
	feed_text(&client, "SSH-2.0-peer\r\n");
	kexinit_wire(&message_wire, 1, "mlkem768x25519-sha256,kex-strict-s-v00@openssh.com", "chacha20-poly1305@openssh.com", 0);
	wire_reset(&frame_wire);
	frame_plain(&frame_wire, message_wire.data, message_wire.size);
	frame_plain(&frame_wire, debug, sizeof(debug));
	feed(&client, &frame_wire);
	error = step(&client, NULL);
	CHECK_EQ(error, EPROTO);
	CHECK_EQ(read_plain(&client, 1), 3);
	CHECK_EQ(plain_out.data[0][0], 20);
	CHECK_EQ(plain_out.data[1][0], 30);
	CHECK_EQ(plain_disconnect_code(), SSH_DISCONNECT_PROTOCOL_ERROR);
	ssh_transport_free(&client);

	/* KEXINIT, then IGNORE. */
	start_one(&client, SSH_ROLE_CLIENT);
	feed_text(&client, "SSH-2.0-peer\r\n");
	wire_reset(&frame_wire);
	frame_plain(&frame_wire, message_wire.data, message_wire.size);
	frame_plain(&frame_wire, ignore, sizeof(ignore));
	feed(&client, &frame_wire);
	error = step(&client, NULL);
	CHECK_EQ(error, EPROTO);
	ssh_transport_free(&client);

	/* A peer without its strict marker is refused with KEY_EXCHANGE_FAILED. */
	start_one(&client, SSH_ROLE_CLIENT);
	feed_text(&client, "SSH-2.0-peer\r\n");
	kexinit_wire(&message_wire, 1, "mlkem768x25519-sha256", "chacha20-poly1305@openssh.com", 0);
	wire_reset(&frame_wire);
	frame_plain(&frame_wire, message_wire.data, message_wire.size);
	feed(&client, &frame_wire);
	error = step(&client, NULL);
	CHECK_EQ(error, EPROTO);
	CHECK_EQ(read_plain(&client, 1), 2);
	CHECK_EQ(plain_disconnect_code(), SSH_DISCONNECT_KEY_EXCHANGE_FAILED);
	ssh_transport_free(&client);

	/* The client's own marker sent by a server does not count. */
	start_one(&client, SSH_ROLE_CLIENT);
	feed_text(&client, "SSH-2.0-peer\r\n");
	kexinit_wire(&message_wire, 1, "mlkem768x25519-sha256,kex-strict-c", "chacha20-poly1305@openssh.com", 0);
	wire_reset(&frame_wire);
	frame_plain(&frame_wire, message_wire.data, message_wire.size);
	feed(&client, &frame_wire);
	error = step(&client, NULL);
	CHECK_EQ(error, EPROTO);
	ssh_transport_free(&client);

	/* NEWKEYS before the reply is out of step. */
	start_one(&client, SSH_ROLE_CLIENT);
	feed_text(&client, "SSH-2.0-peer\r\n");
	kexinit_wire(&message_wire, 1, "mlkem768x25519-sha256,kex-strict-s", "chacha20-poly1305@openssh.com", 0);
	wire_reset(&frame_wire);
	frame_plain(&frame_wire, message_wire.data, message_wire.size);
	frame_plain(&frame_wire, newkeys, sizeof(newkeys));
	feed(&client, &frame_wire);
	error = step(&client, NULL);
	CHECK_EQ(error, EPROTO);
	ssh_transport_free(&client);

	/* A second KEXINIT is out of step. */
	start_one(&client, SSH_ROLE_CLIENT);
	feed_text(&client, "SSH-2.0-peer\r\n");
	wire_reset(&frame_wire);
	frame_plain(&frame_wire, message_wire.data, message_wire.size);
	frame_plain(&frame_wire, message_wire.data, message_wire.size);
	feed(&client, &frame_wire);
	error = step(&client, NULL);
	CHECK_EQ(error, EPROTO);
	ssh_transport_free(&client);

	/* An empty packet (no message number) is EINVAL. */
	start_one(&client, SSH_ROLE_CLIENT);
	feed_text(&client, "SSH-2.0-peer\r\n");
	wire_reset(&frame_wire);
	frame_plain(&frame_wire, NULL, 0);
	feed(&client, &frame_wire);
	error = step(&client, NULL);
	CHECK_EQ(error, EINVAL);
	ssh_transport_free(&client);

	/* A DISCONNECT as the first packet ends the connection with its reason. */
	start_one(&client, SSH_ROLE_CLIENT);
	feed_text(&client, "SSH-2.0-peer\r\n");
	wire_reset(&message_wire);
	wire_u8(&message_wire, 1);
	wire_u32(&message_wire, SSH_DISCONNECT_TOO_MANY_CONNECTIONS);
	wire_text(&message_wire, "busy");
	wire_text(&message_wire, "");
	wire_reset(&frame_wire);
	frame_plain(&frame_wire, message_wire.data, message_wire.size);
	feed(&client, &frame_wire);
	error = step(&client, NULL);
	CHECK_EQ(error, ECONNABORTED);
	CHECK_EQ(client.reason_code, SSH_DISCONNECT_TOO_MANY_CONNECTIONS);
	CHECK(strcmp(client.reason, "busy") == 0);
	CHECK_EQ(read_plain(&client, 1), 1);
	CHECK_EQ(plain_out.data[0][0], 20);
	ssh_transport_free(&client);
}

/* Strict key exchange on a server: SERVICE_REQUEST and a second HYBRID_INIT in the initial exchange. */
static void
test_strict_server(void)
{
	static const uint8_t service[] = { 5, 0, 0, 0, 1, 'x' };
	struct ssh_hybrid_client half;
	uint8_t c_init[SSH_HYBRID_INIT_SIZE];
	int error;

	/* A SERVICE_REQUEST after the client's KEXINIT. */
	start_one(&server, SSH_ROLE_SERVER);
	feed_text(&server, "SSH-2.0-peer\r\n");
	kexinit_wire(&message_wire, 0, "mlkem768x25519-sha256,kex-strict-c", "chacha20-poly1305@openssh.com", 0);
	wire_reset(&frame_wire);
	frame_plain(&frame_wire, message_wire.data, message_wire.size);
	frame_plain(&frame_wire, service, sizeof(service));
	feed(&server, &frame_wire);
	error = step(&server, NULL);
	CHECK_EQ(error, EPROTO);
	CHECK_EQ(read_plain(&server, 1), 2);
	CHECK_EQ(plain_disconnect_code(), SSH_DISCONNECT_PROTOCOL_ERROR);
	ssh_transport_free(&server);

	/* A valid C_INIT once, then again: the reply and NEWKEYS go out for the first, the second ends it. */
	memset(&half, 0, sizeof(half));
	error = ssh_hybrid_client_start(&half, c_init);
	CHECK_EQ(error, 0);
	ssh_hybrid_client_clear(&half);
	start_one(&server, SSH_ROLE_SERVER);
	feed_text(&server, "SSH-2.0-peer\r\n");
	wire_reset(&frame_wire);
	frame_plain(&frame_wire, message_wire.data, message_wire.size);
	wire_reset(&message_wire);
	wire_u8(&message_wire, 30);
	wire_string(&message_wire, c_init, sizeof(c_init));
	frame_plain(&frame_wire, message_wire.data, message_wire.size);
	feed(&server, &frame_wire);
	error = step(&server, NULL);
	CHECK_EQ(error, EAGAIN);
	CHECK_EQ(server_context.sign_calls, 1);
	CHECK_EQ(read_plain(&server, 1), 3);
	CHECK_EQ(plain_out.data[0][0], 20);
	CHECK_EQ(plain_out.data[1][0], 31);
	CHECK_EQ(plain_out.data[2][0], 21);
	CHECK_EQ(plain_out.size[2], 1);
	wire_reset(&frame_wire);
	frame_plain(&frame_wire, message_wire.data, message_wire.size);
	feed(&server, &frame_wire);
	error = step(&server, NULL);
	CHECK_EQ(error, EPROTO);
	CHECK_EQ(server.state, SSH_TRANSPORT_CLOSED);
	CHECK_EQ(server_context.sign_calls, 1);
	ssh_transport_free(&server);

	/* A C_INIT one byte short is refused with KEY_EXCHANGE_FAILED. */
	start_one(&server, SSH_ROLE_SERVER);
	feed_text(&server, "SSH-2.0-peer\r\n");
	kexinit_wire(&message_wire, 0, "mlkem768x25519-sha256,kex-strict-c-v00@openssh.com", "chacha20-poly1305@openssh.com", 0);
	wire_reset(&frame_wire);
	frame_plain(&frame_wire, message_wire.data, message_wire.size);
	wire_reset(&message_wire);
	wire_u8(&message_wire, 30);
	wire_string(&message_wire, c_init, sizeof(c_init) - 1U);
	frame_plain(&frame_wire, message_wire.data, message_wire.size);
	feed(&server, &frame_wire);
	error = step(&server, NULL);
	CHECK_EQ(error, EINVAL);
	CHECK_EQ(read_plain(&server, 1), 2);
	CHECK_EQ(plain_disconnect_code(), SSH_DISCONNECT_KEY_EXCHANGE_FAILED);
	CHECK_EQ(server_context.sign_calls, 0);
	ssh_transport_free(&server);

	/* A HYBRID_REPLY sent to a server is out of step. */
	start_one(&server, SSH_ROLE_SERVER);
	feed_text(&server, "SSH-2.0-peer\r\n");
	kexinit_wire(&message_wire, 0, "mlkem768x25519-sha256,kex-strict-c", "chacha20-poly1305@openssh.com", 0);
	wire_reset(&frame_wire);
	frame_plain(&frame_wire, message_wire.data, message_wire.size);
	wire_reset(&message_wire);
	wire_u8(&message_wire, 31);
	frame_plain(&frame_wire, message_wire.data, message_wire.size);
	feed(&server, &frame_wire);
	error = step(&server, NULL);
	CHECK_EQ(error, EPROTO);
	ssh_transport_free(&server);

	/* A HYBRID_INIT before the client's KEXINIT is refused (it is not the first message). */
	start_one(&server, SSH_ROLE_SERVER);
	feed_text(&server, "SSH-2.0-peer\r\n");
	wire_reset(&message_wire);
	wire_u8(&message_wire, 30);
	wire_string(&message_wire, c_init, sizeof(c_init));
	wire_reset(&frame_wire);
	frame_plain(&frame_wire, message_wire.data, message_wire.size);
	feed(&server, &frame_wire);
	error = step(&server, NULL);
	CHECK_EQ(error, EPROTO);
	ssh_transport_free(&server);
}

/* A sequence number that wraps before the initial exchange ends is refused (strict key exchange 3.2). */
static void
test_wrap(void)
{
	int error;

	/* The receiving number wraps on the server's KEXINIT. */
	start_one(&client, SSH_ROLE_CLIENT);
	feed_text(&client, "SSH-2.0-peer\r\n");
	error = step(&client, NULL);
	CHECK_EQ(error, EAGAIN);
	client.packet.receive.seq = 0xffffffffU;
	kexinit_wire(&message_wire, 1, "mlkem768x25519-sha256,kex-strict-s", "chacha20-poly1305@openssh.com", 0);
	wire_reset(&frame_wire);
	frame_plain(&frame_wire, message_wire.data, message_wire.size);
	feed(&client, &frame_wire);
	error = step(&client, NULL);
	CHECK_EQ(error, EPROTO);
	ssh_transport_free(&client);

	/* The sending number wrapped on the client's KEXINIT. */
	start_one(&client, SSH_ROLE_CLIENT);
	client.packet.send.seq = 0xffffffffU;
	feed_text(&client, "SSH-2.0-peer\r\n");
	error = step(&client, NULL);
	CHECK_EQ(error, EAGAIN);
	CHECK_EQ(client.packet.send.wrapped, 1);
	feed(&client, &frame_wire);
	error = step(&client, NULL);
	CHECK_EQ(error, EPROTO);
	ssh_transport_free(&client);

	/* At 0xfffffffe nothing wraps and the exchange goes on. */
	start_one(&client, SSH_ROLE_CLIENT);
	feed_text(&client, "SSH-2.0-peer\r\n");
	error = step(&client, NULL);
	CHECK_EQ(error, EAGAIN);
	client.packet.receive.seq = 0xfffffffeU;
	feed(&client, &frame_wire);
	error = step(&client, NULL);
	CHECK_EQ(error, EAGAIN);
	CHECK_EQ(client.state, SSH_TRANSPORT_INITIAL_KEX);
	ssh_transport_free(&client);

	/* After the initial exchange a wrap is no error (only strict key exchange's initial phase forbids it). */
	connect_pair();
	client.packet.receive.seq = 0xffffffffU;
	server.packet.send.seq = 0xffffffffU;
	big_payload[0] = 94;
	error = ssh_transport_send(&server, big_payload, 8);
	CHECK_EQ(error, 0);
	error = pump();
	CHECK_EQ(error, 0);
	CHECK_EQ(client_inbox.count, 1);
	stop_pair();
}

/* A wrong guess of the peer (first_kex_packet_follows) drops the packet after its KEXINIT (RFC 4253 7). */
static void
test_guess(void)
{
	static const uint8_t guessed[] = { 30, 1, 2, 3 };
	int error;

	/* The server's first kex algorithm is not the one chosen: its next packet is dropped. */
	start_one(&client, SSH_ROLE_CLIENT);
	feed_text(&client, "SSH-2.0-peer\r\n");
	kexinit_wire(&message_wire, 1, "curve25519-sha256,mlkem768x25519-sha256,kex-strict-s", "chacha20-poly1305@openssh.com", 1);
	wire_reset(&frame_wire);
	frame_plain(&frame_wire, message_wire.data, message_wire.size);
	frame_plain(&frame_wire, guessed, sizeof(guessed));
	feed(&client, &frame_wire);
	error = step(&client, NULL);
	CHECK_EQ(error, EAGAIN);
	CHECK_EQ(client.state, SSH_TRANSPORT_INITIAL_KEX);
	CHECK_EQ(client.choice.ignore_next, 0);

	/* The same packet again is not dropped: a client takes no HYBRID_INIT. */
	wire_reset(&frame_wire);
	frame_plain(&frame_wire, guessed, sizeof(guessed));
	feed(&client, &frame_wire);
	error = step(&client, NULL);
	CHECK_EQ(error, EPROTO);
	ssh_transport_free(&client);

	/* A right guess drops nothing. */
	start_one(&client, SSH_ROLE_CLIENT);
	feed_text(&client, "SSH-2.0-peer\r\n");
	kexinit_wire(&message_wire, 1, "mlkem768x25519-sha256,kex-strict-s", "chacha20-poly1305@openssh.com", 1);
	wire_reset(&frame_wire);
	frame_plain(&frame_wire, message_wire.data, message_wire.size);
	frame_plain(&frame_wire, guessed, sizeof(guessed));
	feed(&client, &frame_wire);
	error = step(&client, NULL);
	CHECK_EQ(error, EPROTO);
	ssh_transport_free(&client);
}

/* The identification lines (RFC 4253 4.2). */
static void
test_version(void)
{
	char line[300];
	unsigned index;
	int error;

	/* A client skips 64 other lines before the server's line. */
	start_pair();
	for (index = 0; index < SSH_VERSION_PRELUDE_LINES; index++)
		feed_text(&client, "a line before the identification\r\n");
	error = pump();
	CHECK_EQ(error, 0);
	CHECK_EQ(client.state, SSH_TRANSPORT_RUNNING);
	CHECK_EQ(client.prelude_lines, SSH_VERSION_PRELUDE_LINES);
	stop_pair();

	/* The 65th other line ends it. */
	start_one(&client, SSH_ROLE_CLIENT);
	for (index = 0; index < SSH_VERSION_PRELUDE_LINES + 1U; index++)
		feed_text(&client, "x\n");
	feed_text(&client, "SSH-2.0-peer\r\n");
	error = step(&client, NULL);
	CHECK_EQ(error, EPROTO);
	CHECK_EQ(client.state, SSH_TRANSPORT_CLOSED);
	ssh_transport_free(&client);

	/* A server takes no other line. */
	start_one(&server, SSH_ROLE_SERVER);
	feed_text(&server, "hello\r\nSSH-2.0-peer\r\n");
	error = step(&server, NULL);
	CHECK_EQ(error, EPROTO);
	ssh_transport_free(&server);

	/* Version 1.5 is refused with PROTOCOL_VERSION_NOT_SUPPORTED. */
	start_one(&client, SSH_ROLE_CLIENT);
	feed_text(&client, "SSH-1.5-old\r\n");
	error = step(&client, NULL);
	CHECK_EQ(error, EPROTO);
	CHECK_EQ(read_plain(&client, 1), 1);
	CHECK_EQ(plain_disconnect_code(), SSH_DISCONNECT_PROTOCOL_VERSION_NOT_SUPPORTED);
	ssh_transport_free(&client);

	/* "SSH-2.0" without the dash, and "SSH-2.00-" are not version 2.0. */
	start_one(&client, SSH_ROLE_CLIENT);
	feed_text(&client, "SSH-2.0\r\n");
	error = step(&client, NULL);
	CHECK_EQ(error, EPROTO);
	ssh_transport_free(&client);
	start_one(&client, SSH_ROLE_CLIENT);
	feed_text(&client, "SSH-2.00-x\r\n");
	error = step(&client, NULL);
	CHECK_EQ(error, EPROTO);
	ssh_transport_free(&client);

	/* 1.99 is version 2; an LF alone ends the line. */
	start_one(&client, SSH_ROLE_CLIENT);
	feed_text(&client, "SSH-1.99-both\n");
	error = step(&client, NULL);
	CHECK_EQ(error, EAGAIN);
	CHECK_EQ(client.state, SSH_TRANSPORT_INITIAL_KEX);
	CHECK(strcmp(client.version_peer, "SSH-1.99-both") == 0);
	CHECK_EQ(read_plain(&client, 1), 1);
	CHECK_EQ(plain_out.data[0][0], 20);
	ssh_transport_free(&client);

	/* 253 characters exactly are taken; 254 are EMSGSIZE. */
	memset(line, 'a', sizeof(line));
	memcpy(line, "SSH-2.0-", 8);
	line[253] = '\r';
	line[254] = '\n';
	line[255] = '\0';
	start_one(&client, SSH_ROLE_CLIENT);
	feed_text(&client, line);
	error = step(&client, NULL);
	CHECK_EQ(error, EAGAIN);
	CHECK_EQ(strlen(client.version_peer), 253);
	ssh_transport_free(&client);
	memset(line, 'a', sizeof(line));
	memcpy(line, "SSH-2.0-", 8);
	line[254] = '\r';
	line[255] = '\n';
	line[256] = '\0';
	start_one(&client, SSH_ROLE_CLIENT);
	feed_text(&client, line);
	error = step(&client, NULL);
	CHECK_EQ(error, EMSGSIZE);
	CHECK_EQ(client.state, SSH_TRANSPORT_CLOSED);
	ssh_transport_free(&client);

	/* A NUL in the line is refused. */
	start_one(&client, SSH_ROLE_CLIENT);
	error = ssh_transport_input(&client, (const uint8_t *)"SSH-2.0-a\0b\r\n", 13);
	CHECK_EQ(error, 0);
	error = step(&client, NULL);
	CHECK_EQ(error, EINVAL);
	ssh_transport_free(&client);

	/* A line that has not ended waits. */
	start_one(&client, SSH_ROLE_CLIENT);
	feed_text(&client, "SSH-2.0-par");
	error = step(&client, NULL);
	CHECK_EQ(error, EAGAIN);
	CHECK_EQ(client.state, SSH_TRANSPORT_VERSION);
	feed_text(&client, "tial\r\n");
	error = step(&client, NULL);
	CHECK_EQ(error, EAGAIN);
	CHECK(strcmp(client.version_peer, "SSH-2.0-partial") == 0);
	ssh_transport_free(&client);
}

/*
 * Runs a pair to where the server's reply waits in its unkeyed output,
 * then hands the reply to modify and the result, with the server's
 * NEWKEYS, to the client.  Returns the client's answer.
 */
static int
exchange_with_reply(
	void (*modify)(struct wire *reply, const uint8_t *payload, size_t size))
{
	int error;

	/* Versions and KEXINITs, then the client's half to the server. */
	start_pair();
	move_all(&client, &server);
	error = step(&server, NULL);
	CHECK_EQ(error, EAGAIN);
	move_all(&server, &client);
	error = step(&client, NULL);
	CHECK_EQ(error, EAGAIN);
	move_all(&client, &server);
	error = step(&server, NULL);
	CHECK_EQ(error, EAGAIN);

	/* The server's output now holds the reply and NEWKEYS, unkeyed. */
	CHECK_EQ(read_plain(&server, 0), 2);
	CHECK_EQ(plain_out.data[0][0], 31);
	CHECK_EQ(plain_out.data[1][0], 21);

	/* Gives the client the changed reply and NEWKEYS. */
	wire_reset(&message_wire);
	modify(&message_wire, plain_out.data[0], plain_out.size[0]);
	wire_reset(&frame_wire);
	frame_plain(&frame_wire, message_wire.data, message_wire.size);
	if (reply_twice != 0)
		frame_plain(&frame_wire, message_wire.data, message_wire.size);
	wire_reset(&export_wire);
	wire_bytes(&export_wire, plain_out.data[1], plain_out.size[1]);
	if (newkeys_extra != 0)
		wire_u8(&export_wire, 0);
	frame_plain(&frame_wire, export_wire.data, export_wire.size);
	feed(&client, &frame_wire);
	error = step(&client, NULL);
	return error;
}

/* Leaves the reply as it is. */
static void
reply_same(
	struct wire *reply,
	const uint8_t *payload,
	size_t size)
{
	/* Copies it. */
	wire_bytes(reply, payload, size);
}

/* Flips one bit of the signature (the last byte of the reply). */
static void
reply_flip_signature(
	struct wire *reply,
	const uint8_t *payload,
	size_t size)
{
	/* Copies it and changes the last byte. */
	wire_bytes(reply, payload, size);
	reply->data[reply->size - 1U] ^= 0x80U;
}

/* Splits a reply into its three strings (number, K_S, S_REPLY, signature). */
static void
reply_split(
	const uint8_t *payload,
	const uint8_t **key,
	size_t *key_size,
	const uint8_t **s_reply,
	size_t *s_reply_size,
	const uint8_t **signature,
	size_t *signature_size)
{
	size_t offset;

	/* Walks the three strings. */
	offset = 1;
	*key_size = ssh_load_be32(payload + offset);
	*key = payload + offset + 4U;
	offset += 4U + *key_size;
	*s_reply_size = ssh_load_be32(payload + offset);
	*s_reply = payload + offset + 4U;
	offset += 4U + *s_reply_size;
	*signature_size = ssh_load_be32(payload + offset);
	*signature = payload + offset + 4U;
}

/* Cuts one byte from S_REPLY. */
static void
reply_short(
	struct wire *reply,
	const uint8_t *payload,
	size_t size)
{
	const uint8_t *key;
	const uint8_t *s_reply;
	const uint8_t *signature;
	size_t key_size;
	size_t s_reply_size;
	size_t signature_size;

	/* Rebuilds the three strings with S_REPLY one byte shorter. */
	(void)size;
	reply_split(payload, &key, &key_size, &s_reply, &s_reply_size, &signature, &signature_size);
	wire_u8(reply, 31);
	wire_string(reply, key, key_size);
	wire_string(reply, s_reply, s_reply_size - 1U);
	wire_string(reply, signature, signature_size);
}

/* Adds one byte to S_REPLY. */
static void
reply_long(
	struct wire *reply,
	const uint8_t *payload,
	size_t size)
{
	const uint8_t *key;
	const uint8_t *s_reply;
	const uint8_t *signature;
	size_t key_size;
	size_t s_reply_size;
	size_t signature_size;

	/* Rebuilds the three strings with S_REPLY one byte longer. */
	(void)size;
	reply_split(payload, &key, &key_size, &s_reply, &s_reply_size, &signature, &signature_size);
	wire_u8(reply, 31);
	wire_string(reply, key, key_size);
	wire_u32(reply, (uint32_t)(s_reply_size + 1U));
	wire_bytes(reply, s_reply, s_reply_size);
	wire_u8(reply, 0);
	wire_string(reply, signature, signature_size);
}

/* Names another key type in K_S. */
static void
reply_other_key(
	struct wire *reply,
	const uint8_t *payload,
	size_t size)
{
	/* Copies it and changes the last letter of "ssh-ed25519" inside K_S (offset 1 + 4 + 4 + 10). */
	wire_bytes(reply, payload, size);
	reply->data[1U + 4U + 4U + 10U] = '8';
}

/* Changes one bit of the server's ML-KEM ciphertext, so K differs on the two sides. */
static void
reply_flip_ciphertext(
	struct wire *reply,
	const uint8_t *payload,
	size_t size)
{
	/* Copies it and changes a byte of S_CT2 (after the number, K_S and the S_REPLY length). */
	wire_bytes(reply, payload, size);
	reply->data[1U + 4U + SSH_KEY_BLOB_SIZE + 4U + 100U] ^= 0x01U;
}

/* Makes the server's X25519 point S_PK1 (the last 32 bytes of S_REPLY) all zeros (RFC 7748 6.1). */
static void
reply_zero_point(
	struct wire *reply,
	const uint8_t *payload,
	size_t size)
{
	size_t offset;

	/* Copies it and clears the 32 bytes before the signature string. */
	wire_bytes(reply, payload, size);
	offset = 1U + 4U + SSH_KEY_BLOB_SIZE + 4U + SSH_HYBRID_REPLY_SIZE - SSH_X25519_SIZE;
	memset(reply->data + offset, 0, SSH_X25519_SIZE);
}

/* The errors of the exchange on a client: signature, host key, S_REPLY length. */
static void
test_exchange_errors(void)
{
	int error;

	/* The unchanged reply completes. */
	error = exchange_with_reply(reply_same);
	CHECK_EQ(error, EAGAIN);
	error = pump();
	CHECK_EQ(error, 0);
	CHECK_EQ(client.state, SSH_TRANSPORT_RUNNING);
	stop_pair();

	/* One bit of the signature. */
	error = exchange_with_reply(reply_flip_signature);
	CHECK_EQ(error, EBADMSG);
	CHECK_EQ(client.state, SSH_TRANSPORT_CLOSED);
	CHECK_EQ(client.have_session_id, 0);
	CHECK_EQ(read_plain(&client, 0), 1);
	CHECK_EQ(plain_disconnect_code(), SSH_DISCONNECT_KEY_EXCHANGE_FAILED);
	stop_pair();

	/* A server that signs wrongly. */
	start_pair();
	server_context.flip = 1;
	error = pump();
	CHECK_EQ(error, EBADMSG);
	CHECK_EQ(client.state, SSH_TRANSPORT_CLOSED);
	stop_pair();

	/* One bit of the ciphertext: K differs, and so the signature does not verify. */
	error = exchange_with_reply(reply_flip_ciphertext);
	CHECK_EQ(error, EBADMSG);
	stop_pair();

	/* The host key refused. */
	start_pair();
	client_context.refuse = 1;
	error = pump();
	CHECK_EQ(error, EACCES);
	CHECK_EQ(client.state, SSH_TRANSPORT_CLOSED);
	CHECK_EQ(client_context.check_calls, 1);
	CHECK_EQ(read_plain(&client, 0), 1);
	CHECK_EQ(plain_disconnect_code(), SSH_DISCONNECT_HOST_KEY_NOT_VERIFIABLE);
	stop_pair();

	/* Another failure of the check is passed on, with PROTOCOL_ERROR. */
	start_pair();
	client_context.fail = 1;
	error = pump();
	CHECK_EQ(error, EIO);
	CHECK_EQ(read_plain(&client, 0), 1);
	CHECK_EQ(plain_disconnect_code(), SSH_DISCONNECT_PROTOCOL_ERROR);
	stop_pair();

	/* S_REPLY one byte short and one byte long: refused before the host key is looked at. */
	error = exchange_with_reply(reply_short);
	CHECK_EQ(error, EINVAL);
	CHECK_EQ(client_context.check_calls, 0);
	CHECK_EQ(read_plain(&client, 0), 1);
	CHECK_EQ(plain_disconnect_code(), SSH_DISCONNECT_KEY_EXCHANGE_FAILED);
	stop_pair();
	error = exchange_with_reply(reply_long);
	CHECK_EQ(error, EINVAL);
	CHECK_EQ(client_context.check_calls, 0);
	stop_pair();

	/* An all-zero X25519 point of the server, after the host key was checked. */
	error = exchange_with_reply(reply_zero_point);
	CHECK_EQ(error, EINVAL);
	CHECK_EQ(client_context.check_calls, 1);
	CHECK_EQ(read_plain(&client, 0), 1);
	CHECK_EQ(plain_disconnect_code(), SSH_DISCONNECT_KEY_EXCHANGE_FAILED);
	stop_pair();

	/* The reply twice: the second is out of step. */
	reply_twice = 1;
	error = exchange_with_reply(reply_same);
	reply_twice = 0;
	CHECK_EQ(error, EPROTO);
	stop_pair();

	/* A client without a host key check, and a server without its operations, fail with EINVAL. */
	start_pair();
	ssh_transport_free(&client);
	error = ssh_transport_init(&client, SSH_ROLE_CLIENT, NULL, TEST_NOW);
	CHECK_EQ(error, 0);
	error = pump();
	CHECK_EQ(error, EINVAL);
	stop_pair();
	start_pair();
	ssh_transport_free(&server);
	error = ssh_transport_init(&server, SSH_ROLE_SERVER, NULL, TEST_NOW);
	CHECK_EQ(error, 0);
	error = pump();
	CHECK_EQ(error, EINVAL);
	CHECK_EQ(read_plain(&server, 0), 1);
	CHECK_EQ(plain_disconnect_code(), SSH_DISCONNECT_PROTOCOL_ERROR);
	stop_pair();

	/* A failing host_key and a failing sign of the server are passed on. */
	start_pair();
	server_context.fail_host_key = 1;
	error = pump();
	CHECK_EQ(error, EIO);
	CHECK_EQ(server.state, SSH_TRANSPORT_CLOSED);
	stop_pair();
	start_pair();
	server_context.fail_sign = 1;
	error = pump();
	CHECK_EQ(error, EIO);
	CHECK_EQ(server.have_session_id, 1);
	stop_pair();

	/* A host key of another type. */
	error = exchange_with_reply(reply_other_key);
	CHECK_EQ(error, ENOTSUP);
	CHECK_EQ(client_context.check_calls, 0);
	CHECK_EQ(read_plain(&client, 0), 1);
	CHECK_EQ(plain_disconnect_code(), SSH_DISCONNECT_KEY_EXCHANGE_FAILED);
	stop_pair();
}

/* A KEXINIT whose lists do not meet ours ends with KEY_EXCHANGE_FAILED naming the kind (RFC 4253 7.1). */
static void
test_kexinit_mismatch(void)
{
	const uint8_t *last;
	size_t length;
	int error;

	/* Only another cipher. */
	start_one(&client, SSH_ROLE_CLIENT);
	feed_text(&client, "SSH-2.0-peer\r\n");
	kexinit_wire(&message_wire, 1, "mlkem768x25519-sha256,kex-strict-s", "aes128-ctr", 0);
	wire_reset(&frame_wire);
	frame_plain(&frame_wire, message_wire.data, message_wire.size);
	feed(&client, &frame_wire);
	error = step(&client, NULL);
	CHECK_EQ(error, ENOTSUP);
	CHECK_EQ(read_plain(&client, 1), 2);
	CHECK_EQ(plain_disconnect_code(), SSH_DISCONNECT_KEY_EXCHANGE_FAILED);
	last = plain_out.data[1];
	length = ssh_load_be32(last + 5);
	CHECK(length > 12U && length < 200U);
	CHECK(memcmp(last + 9, "no matching encryption", 22) == 0);
	ssh_transport_free(&client);

	/* Only another key exchange. */
	start_one(&client, SSH_ROLE_CLIENT);
	feed_text(&client, "SSH-2.0-peer\r\n");
	kexinit_wire(&message_wire, 1, "curve25519-sha256,kex-strict-s", "chacha20-poly1305@openssh.com", 0);
	wire_reset(&frame_wire);
	frame_plain(&frame_wire, message_wire.data, message_wire.size);
	feed(&client, &frame_wire);
	error = step(&client, NULL);
	CHECK_EQ(error, ENOTSUP);
	CHECK_EQ(read_plain(&client, 1), 2);
	CHECK_EQ(plain_disconnect_code(), SSH_DISCONNECT_KEY_EXCHANGE_FAILED);
	ssh_transport_free(&client);

	/* A malformed KEXINIT. */
	start_one(&client, SSH_ROLE_CLIENT);
	feed_text(&client, "SSH-2.0-peer\r\n");
	kexinit_wire(&message_wire, 1, "mlkem768x25519-sha256,kex-strict-s", "chacha20-poly1305@openssh.com", 0);
	wire_u8(&message_wire, 0);
	wire_reset(&frame_wire);
	frame_plain(&frame_wire, message_wire.data, message_wire.size);
	feed(&client, &frame_wire);
	error = step(&client, NULL);
	CHECK_EQ(error, EINVAL);
	CHECK_EQ(read_plain(&client, 1), 2);
	CHECK_EQ(plain_disconnect_code(), SSH_DISCONNECT_PROTOCOL_ERROR);
	ssh_transport_free(&client);
}

/*
 * Numbers outside every layer get UNIMPLEMENTED with the packet's sequence
 * number (RFC 4253 11.4); the generic messages and EXT_INFO are dropped
 * without an answer.
 */
static void
test_unimplemented(void)
{
	static const unsigned unknown[] = { 8, 128, 0, 19, 22, 29, 191 };
	static const unsigned dropped[] = { 2, 3, 4, 7 };
	uint8_t message[8];
	uint8_t answer[64];
	size_t size;
	uint32_t seq;
	unsigned index;
	int error;

	connect_pair();

	/* Each unknown number in turn. */
	for (index = 0; index < sizeof(unknown) / sizeof(unknown[0]); index++) {
		seq = server.packet.send.seq;
		if (index == 0)
			CHECK_EQ(seq, 0);
		memset(message, 0, sizeof(message));
		message[0] = (uint8_t)unknown[index];
		error = raw_send(&server, message, sizeof(message));
		CHECK_EQ(error, 0);
		move_all(&server, &client);
		error = step(&client, &client_inbox);
		CHECK_EQ(error, EAGAIN);
		CHECK_EQ(client_inbox.count, 0);

		/* The answer, read with the server's receiving key: number 3 and the uint32 sequence number. */
		size = raw_read(&client, &server, answer, sizeof(answer));
		wire_reset(&message_wire);
		wire_u8(&message_wire, 3);
		wire_u32(&message_wire, seq);
		CHECK_EQ(size, message_wire.size);
		CHECK_BYTES(answer, message_wire.data, message_wire.size);
	}

	/* The dropped ones leave no answer. */
	for (index = 0; index < sizeof(dropped) / sizeof(dropped[0]); index++) {
		memset(message, 0, sizeof(message));
		message[0] = (uint8_t)dropped[index];
		error = raw_send(&server, message, sizeof(message));
		CHECK_EQ(error, 0);
		move_all(&server, &client);
		error = step(&client, &client_inbox);
		CHECK_EQ(error, EAGAIN);
		(void)ssh_transport_pending(&client, &size);
		CHECK_EQ(size, 0);
	}
	CHECK_EQ(client_inbox.count, 0);

	/* None of them ended the connection. */
	CHECK_EQ(client.state, SSH_TRANSPORT_RUNNING);
	stop_pair();
}

/* Key exchange messages outside an exchange, or out of its step, are EPROTO. */
static void
test_kex_out_of_step(void)
{
	static const unsigned numbers[] = { 30, 31, 21, 32, 40, 49 };
	uint8_t message[8];
	unsigned index;
	int error;

	/* Each to a running client, and to a running server. */
	for (index = 0; index < sizeof(numbers) / sizeof(numbers[0]); index++) {
		connect_pair();
		memset(message, 0, sizeof(message));
		message[0] = (uint8_t)numbers[index];
		error = raw_send(&server, message, sizeof(message));
		CHECK_EQ(error, 0);
		move_all(&server, &client);
		error = step(&client, NULL);
		CHECK_EQ(error, EPROTO);
		CHECK_EQ(client.state, SSH_TRANSPORT_CLOSED);
		stop_pair();

		connect_pair();
		error = raw_send(&client, message, sizeof(message));
		CHECK_EQ(error, 0);
		move_all(&client, &server);
		error = step(&server, NULL);
		CHECK_EQ(error, EPROTO);
		stop_pair();
	}

	/* A NEWKEYS with a byte after it is EINVAL. */
	newkeys_extra = 1;
	error = exchange_with_reply(reply_same);
	newkeys_extra = 0;
	CHECK_EQ(error, EINVAL);
	CHECK_EQ(client.state, SSH_TRANSPORT_CLOSED);
	stop_pair();
}

/* A peer's DISCONNECT: ECONNABORTED, the code and the description kept (printable, cut). */
static void
test_disconnect(void)
{
	char description[400];
	uint8_t answer[1024];
	size_t size;
	int error;

	/* A short reason with a control character. */
	connect_pair();
	ssh_transport_disconnect(&server, SSH_DISCONNECT_BY_APPLICATION, "bye\001now\177");
	CHECK_EQ(server.state, SSH_TRANSPORT_CLOSED);
	error = pump();
	CHECK_EQ(error, ECONNABORTED);
	CHECK_EQ(client.state, SSH_TRANSPORT_CLOSED);
	CHECK_EQ(client.reason_code, SSH_DISCONNECT_BY_APPLICATION);
	CHECK(strcmp(client.reason, "bye?now?") == 0);
	error = step(&client, NULL);
	CHECK_EQ(error, EPIPE);
	big_payload[0] = 94;
	error = ssh_transport_send(&client, big_payload, 4);
	CHECK_EQ(error, EPIPE);
	stop_pair();

	/* A long description: sent cut to SSH_TRANSPORT_REASON_MAX, kept cut to 255 characters. */
	connect_pair();
	memset(description, 'r', sizeof(description) - 1U);
	description[sizeof(description) - 1U] = '\0';
	ssh_transport_disconnect(&server, SSH_DISCONNECT_PROTOCOL_ERROR, description);
	size = raw_read(&server, &client, answer, sizeof(answer));
	CHECK_EQ(size, 1U + 4U + 4U + SSH_TRANSPORT_REASON_MAX + 4U);
	CHECK_EQ(answer[0], 1);
	CHECK_EQ(ssh_load_be32(answer + 1), SSH_DISCONNECT_PROTOCOL_ERROR);
	CHECK_EQ(ssh_load_be32(answer + 5), SSH_TRANSPORT_REASON_MAX);
	CHECK_EQ(ssh_load_be32(answer + 9 + SSH_TRANSPORT_REASON_MAX), 0);
	stop_pair();

	/* The client keeps 255 characters of a long reason. */
	connect_pair();
	ssh_transport_disconnect(&server, SSH_DISCONNECT_PROTOCOL_ERROR, description);
	error = pump();
	CHECK_EQ(error, ECONNABORTED);
	CHECK_EQ(strlen(client.reason), SSH_TRANSPORT_REASON_MAX - 1U);
	stop_pair();

	/* A malformed DISCONNECT still ends the connection. */
	connect_pair();
	answer[0] = 1;
	error = raw_send(&server, answer, 1);
	CHECK_EQ(error, 0);
	move_all(&server, &client);
	error = step(&client, NULL);
	CHECK_EQ(error, ECONNABORTED);
	CHECK_EQ(client.reason_code, 0);
	CHECK_EQ(client.reason[0], '\0');
	stop_pair();

	/* A second disconnect queues nothing more; the pending output stays for the caller to write. */
	connect_pair();
	ssh_transport_disconnect(&client, SSH_DISCONNECT_BY_APPLICATION, NULL);
	(void)ssh_transport_pending(&client, &size);
	CHECK(size > 0);
	ssh_transport_disconnect(&client, SSH_DISCONNECT_BY_APPLICATION, "again");
	error = pump();
	CHECK_EQ(error, ECONNABORTED);
	CHECK_EQ(server.reason_code, SSH_DISCONNECT_BY_APPLICATION);
	CHECK_EQ(server.reason[0], '\0');
	stop_pair();
}

/* Builds the export bytes of a transport from its fields with wire.h (the layout of ssh-transport.c). */
static void
export_from_fields(
	struct wire *out,
	const struct ssh_transport *transport)
{
	const struct ssh_packet_direction *direction;
	unsigned which;

	/* Format, role, lines, session identifier, strict. */
	wire_reset(out);
	wire_u8(out, 1);
	wire_u8(out, (unsigned)transport->role);
	wire_text(out, transport->version_local);
	wire_text(out, transport->version_peer);
	wire_string(out, transport->session_id, SSH_KEX_HASH_SIZE);
	wire_u8(out, 1);

	/* The two directions. */
	for (which = 0; which < 2U; which++) {
		direction = &transport->packet.send;
		if (which == 1U)
			direction = &transport->packet.receive;
		wire_u32(out, 64);
		wire_bytes(out, direction->cipher.main_key, 32);
		wire_bytes(out, direction->cipher.header_key, 32);
		wire_u32(out, direction->seq);
		wire_u8(out, (unsigned)direction->wrapped);
		wire_u64(out, direction->bytes);
		wire_u64(out, direction->packets);
	}

	/* The time and the unprocessed input. */
	wire_u64(out, transport->kex_time);
	wire_string(out, transport->packet.input.data, transport->packet.input.size);
}

/* export and import: the connection goes on in the rebuilt transport, with its unprocessed input. */
static void
test_export(void)
{
	struct ssh_transport_ops ops;
	struct ssh_buf out;
	uint8_t message[50];
	uint8_t session[SSH_KEX_HASH_SIZE];
	unsigned index;
	int error;

	connect_pair();
	memcpy(session, client.session_id, sizeof(session));

	/* Two messages arrive at the client but are not processed. */
	make_message(message, sizeof(message), 94, 1);
	error = ssh_transport_send(&server, message, sizeof(message));
	CHECK_EQ(error, 0);
	make_message(message, 20, 95, 2);
	error = ssh_transport_send(&server, message, 20);
	CHECK_EQ(error, 0);
	move_all(&server, &client);

	/* The export matches the fields in the documented layout. */
	ssh_buf_init(&out, 1024U * 1024U, 1);
	error = ssh_transport_export(&client, &out);
	CHECK_EQ(error, 0);
	export_from_fields(&export_wire, &client);
	CHECK_EQ(out.size, export_wire.size);
	CHECK_BYTES(out.data, export_wire.data, export_wire.size);

	/* Rebuilds the client from it with new operations. */
	ssh_transport_free(&client);
	memset(&ops, 0, sizeof(ops));
	ops.check_host_key = test_check_host_key;
	ops.context = &client_context;
	error = ssh_transport_import(&client, out.data, out.size, &ops, TEST_NOW + 5U);
	CHECK_EQ(error, 0);
	ssh_buf_free(&out);
	CHECK_EQ(client.state, SSH_TRANSPORT_RUNNING);
	CHECK_EQ(client.kex_step, 0);
	CHECK_EQ(client.role, SSH_ROLE_CLIENT);
	CHECK_BYTES(client.session_id, session, sizeof(session));
	CHECK_EQ(client.now, TEST_NOW + 5U);

	/* The unprocessed messages come out, then traffic both ways and a re-exchange. */
	error = step(&client, &client_inbox);
	CHECK_EQ(error, EAGAIN);
	CHECK_EQ(client_inbox.count, 2);
	make_message(message, sizeof(message), 94, 1);
	CHECK_EQ(client_inbox.hash[0], fnv(message, sizeof(message)));
	make_message(message, 20, 95, 2);
	CHECK_EQ(client_inbox.hash[1], fnv(message, 20));
	for (index = 0; index < 3U; index++) {
		make_message(message, sizeof(message), 60U + index, index);
		error = ssh_transport_send(&client, message, sizeof(message));
		CHECK_EQ(error, 0);
	}
	error = ssh_transport_tick(&client, TEST_NOW + SSH_TRANSPORT_REKEY_SECONDS);
	CHECK_EQ(error, 0);
	CHECK(client.kex_step != 0);
	error = pump();
	CHECK_EQ(error, 0);
	CHECK_EQ(server_inbox.count, 3);
	CHECK_EQ(client.kex_step, 0);
	CHECK_EQ(client_context.check_calls, 2);
	CHECK_BYTES(server.session_id, session, sizeof(session));

	/* The server, exported in the middle of nothing, goes on too. */
	ssh_buf_init(&out, 1024U * 1024U, 1);
	error = ssh_transport_export(&server, &out);
	CHECK_EQ(error, 0);
	ssh_transport_free(&server);
	memset(&ops, 0, sizeof(ops));
	ops.host_key = test_host_key;
	ops.sign = test_sign;
	ops.context = &server_context;
	error = ssh_transport_import(&server, out.data, out.size, &ops, TEST_NOW);
	CHECK_EQ(error, 0);
	ssh_buf_free(&out);
	error = ssh_transport_tick(&server, TEST_NOW + 10U * SSH_TRANSPORT_REKEY_SECONDS);
	CHECK_EQ(error, 0);
	make_message(message, 30, 99, 3);
	error = ssh_transport_send(&server, message, 30);
	CHECK_EQ(error, 0);
	error = pump();
	CHECK_EQ(error, 0);
	CHECK_EQ(client_inbox.count, 3);
	CHECK_EQ(server.kex_step, 0);
	stop_pair();

	/* EBUSY: before the initial exchange, during an exchange, with pending output, with held messages. */
	ssh_buf_init(&out, 1024U * 1024U, 1);
	start_pair();
	error = ssh_transport_export(&client, &out);
	CHECK_EQ(error, EBUSY);
	stop_pair();
	connect_pair();
	message[0] = 94;
	error = ssh_transport_send(&client, message, 4);
	CHECK_EQ(error, 0);
	error = ssh_transport_export(&client, &out);
	CHECK_EQ(error, EBUSY);
	error = pump();
	CHECK_EQ(error, 0);
	error = ssh_transport_tick(&client, TEST_NOW + SSH_TRANSPORT_REKEY_SECONDS);
	CHECK_EQ(error, 0);
	ssh_transport_written(&client, SSH_PACKET_OUTPUT_MAX);
	error = ssh_transport_export(&client, &out);
	CHECK_EQ(error, EBUSY);
	error = ssh_transport_send(&client, message, 4);
	CHECK_EQ(error, 0);
	CHECK(client.held.size > 0);
	error = ssh_transport_export(&client, &out);
	CHECK_EQ(error, EBUSY);
	CHECK_EQ(out.size, 0);
	stop_pair();
	ssh_buf_free(&out);
}

/* Tries an import of a changed export and checks it is EINVAL with the transport left freed. */
static void
import_refused(
	const uint8_t *data,
	size_t size)
{
	struct ssh_transport_ops ops;
	int error;

	/* Imports and checks. */
	memset(&ops, 0, sizeof(ops));
	error = ssh_transport_import(&client, data, size, &ops, TEST_NOW);
	CHECK_EQ(error, EINVAL);
	CHECK_EQ(client.packet.input.size, 0);
	CHECK_EQ(client.packet.send.keyed, 0);
	ssh_transport_free(&client);
}

/* Builds export bytes with wire.h, with one field changed by variant (0 for none). */
static void
export_variant(
	struct wire *out,
	unsigned variant)
{
	static const uint8_t key[65] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	static const uint8_t session[33] = { 9, 9, 9 };
	unsigned which;

	/* Format and role. */
	wire_reset(out);
	wire_u8(out, variant == 1U ? 2U : 1U);
	wire_u8(out, variant == 2U ? 2U : 1U);

	/* The lines. */
	if (variant == 3U) {
		wire_text(out, "");
	} else if (variant == 4U) {
		wire_text(out, "SSH-1.5-old");
	} else if (variant == 5U) {
		wire_string(out, "SSH-2.0-a\0b", 11);
	} else if (variant == 6U) {
		wire_u32(out, 254);
		wire_bytes(out, "SSH-2.0-", 8);
		for (which = 8; which < 254U; which++)
			wire_u8(out, 'a');
	} else {
		wire_text(out, SSH_VERSION_STRING);
	}
	wire_text(out, variant == 7U ? "OpenSSH" : "SSH-1.99-peer");

	/* The session identifier and strict. */
	wire_string(out, session, variant == 8U ? 31U : (variant == 9U ? 33U : 32U));
	wire_u8(out, variant == 10U ? 0U : (variant == 11U ? 2U : 1U));

	/* The directions. */
	for (which = 0; which < 2U; which++) {
		wire_string(out, key, variant == 12U + which ? 63U : (variant == 14U + which ? 65U : 64U));
		wire_u32(out, 0x01020304U);
		wire_u8(out, variant == 16U + which ? 2U : 1U);
		wire_u64(out, 77);
		wire_u64(out, 88);
	}

	/* The time and the input. */
	wire_u64(out, 123456);
	wire_text(out, variant == 18U ? "" : "abc");
	if (variant == 19U)
		wire_u8(out, 0);
}

/* import refuses every inconsistent field, every truncation and trailing bytes. */
static void
test_import_errors(void)
{
	struct ssh_transport_ops ops;
	uint8_t *large;
	size_t size;
	unsigned variant;
	int error;

	/* The unchanged bytes are taken, with every field as written. */
	export_variant(&export_wire, 0);
	CHECK(export_wire.overflow == 0);
	memset(&ops, 0, sizeof(ops));
	error = ssh_transport_import(&client, export_wire.data, export_wire.size, &ops, TEST_NOW);
	CHECK_EQ(error, 0);
	CHECK_EQ(client.role, SSH_ROLE_SERVER);
	CHECK(strcmp(client.version_peer, "SSH-1.99-peer") == 0);
	CHECK_EQ(client.packet.send.seq, 0x01020304U);
	CHECK_EQ(client.packet.receive.wrapped, 1);
	CHECK_EQ(client.packet.send.bytes, 77);
	CHECK_EQ(client.packet.receive.packets, 88);
	CHECK_EQ(client.kex_time, 123456U);
	CHECK_EQ(client.packet.input.size, 3);
	CHECK_EQ(client.packet.send.cipher.main_key[0], 1);
	CHECK_EQ(client.packet.send.cipher.header_key[0], 0);
	ssh_transport_free(&client);
	export_variant(&export_wire, 18);
	error = ssh_transport_import(&client, export_wire.data, export_wire.size, &ops, TEST_NOW);
	CHECK_EQ(error, 0);
	ssh_transport_free(&client);

	/* Each changed field. */
	for (variant = 1; variant <= 19U; variant++) {
		if (variant == 18U)
			continue;
		export_variant(&export_wire, variant);
		import_refused(export_wire.data, export_wire.size);
	}

	/* Every truncation. */
	export_variant(&export_wire, 0);
	for (size = 0; size < export_wire.size; size++)
		import_refused(export_wire.data, size);

	/* Unprocessed input above the packet layer's limit. */
	export_variant(&export_wire, 18);
	export_wire.size -= 4U;
	large = malloc(export_wire.size + 4U + SSH_PACKET_INPUT_MAX + 1U);
	CHECK(large != NULL);
	if (large != NULL) {
		memcpy(large, export_wire.data, export_wire.size);
		ssh_store_be32(large + export_wire.size, SSH_PACKET_INPUT_MAX + 1U);
		memset(large + export_wire.size + 4U, 0, SSH_PACKET_INPUT_MAX + 1U);
		import_refused(large, export_wire.size + 4U + SSH_PACKET_INPUT_MAX + 1U);
		ssh_store_be32(large + export_wire.size, SSH_PACKET_INPUT_MAX);
		error = ssh_transport_import(&client, large, export_wire.size + 4U + SSH_PACKET_INPUT_MAX, &ops, TEST_NOW);
		CHECK_EQ(error, 0);
		ssh_transport_free(&client);
	}
	free(large);
}

/* Fills size bytes with random bytes. */
static void
random_bytes(
	uint8_t *out,
	size_t size)
{
	size_t index;

	/* One byte per step. */
	for (index = 0; index < size; index++)
		out[index] = (uint8_t)next_random();
}

/*
 * Random input at each step of the exchange: random lines, random packets
 * framed unkeyed, and random bytes.  Nothing may crash; the answer is
 * EAGAIN, or an error that closes the transport.
 */
static void
test_random(void)
{
	static uint8_t bytes[2048];
	unsigned run;
	unsigned stage;
	unsigned kind;
	size_t size;
	int error;

	/* Each stage of a client and of a server. */
	for (run = 0; run < TEST_RANDOM_RUNS; run++) {
		for (stage = 0; stage < 6U; stage++) {
			start_pair();

			/* Runs the pair to the stage: 0 none, 1 versions, 2 KEXINITs, 3 the client's half, 4 the reply, 5 running. */
			if (stage >= 1U) {
				move_all(&client, &server);
				move_all(&server, &client);
				(void)step(&client, NULL);
				(void)step(&server, NULL);
			}
			if (stage >= 2U) {
				move_all(&client, &server);
				move_all(&server, &client);
				(void)step(&client, NULL);
				(void)step(&server, NULL);
			}
			if (stage >= 3U) {
				move_all(&client, &server);
				(void)step(&server, NULL);
			}
			if (stage >= 4U) {
				move_all(&server, &client);
				(void)step(&client, NULL);
			}
			if (stage >= 5U) {
				error = pump();
				CHECK_EQ(error, 0);
			}

			/* Makes a random input of one of three kinds. */
			size = 1U + next_random() % sizeof(bytes);
			random_bytes(bytes, size);
			kind = next_random() % 3U;
			wire_reset(&frame_wire);
			if (kind == 0U) {
				bytes[size - 1U] = '\n';
				wire_bytes(&frame_wire, bytes, size);
			} else if (kind == 1U) {
				if (size > 600U)
					size = 1U + size % 600U;
				bytes[0] = (uint8_t)(next_random() % 64U);
				frame_plain(&frame_wire, bytes, size);
			} else {
				wire_bytes(&frame_wire, bytes, size);
			}

			/* Gives it to each side and checks that each answers sensibly. */
			if (client.state != SSH_TRANSPORT_CLOSED) {
				error = ssh_transport_input(&client, frame_wire.data, frame_wire.size);
				CHECK(error == 0 || error == EMSGSIZE);
				error = step(&client, NULL);
				if (error != EAGAIN)
					CHECK_EQ(client.state, SSH_TRANSPORT_CLOSED);
			}
			if (server.state != SSH_TRANSPORT_CLOSED) {
				error = ssh_transport_input(&server, frame_wire.data, frame_wire.size);
				CHECK(error == 0 || error == EMSGSIZE);
				error = step(&server, NULL);
				if (error != EAGAIN)
					CHECK_EQ(server.state, SSH_TRANSPORT_CLOSED);
			}
			stop_pair();
		}
	}
}

/*
 * Random messages over a running connection, framed with the peer's keys:
 * random numbers and contents.  Each is delivered, answered, dropped, or
 * ends the connection; after an end a new connection is made.
 */
static void
test_random_running(void)
{
	static uint8_t bytes[1024];
	unsigned run;
	size_t size;
	int error;

	/* Sends random messages from the server to the client. */
	connect_pair();
	for (run = 0; run < TEST_RANDOM_RUNNING; run++) {
		size = 1U + next_random() % sizeof(bytes);
		random_bytes(bytes, size);
		error = raw_send(&server, bytes, size);
		CHECK_EQ(error, 0);
		move_all(&server, &client);
		error = step(&client, &client_inbox);
		if (error != EAGAIN) {
			CHECK_EQ(client.state, SSH_TRANSPORT_CLOSED);
			stop_pair();
			connect_pair();
			continue;
		}
		ssh_transport_written(&client, SSH_PACKET_OUTPUT_MAX);
		client_inbox.count = 0;
	}
	stop_pair();
}
