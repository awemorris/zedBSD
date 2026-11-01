/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The test of ssh-packet.c.  Unkeyed frames the module receives are built
 * here with wire.h from RFC 4253 section 6 (uint32 packet_length, byte
 * padding_length, payload, padding), apart from the module; frames it
 * sends are taken apart here the same way.  Keyed frames are made or
 * opened with ssh_chachapoly_seal and ssh_chachapoly_open, whose result
 * test-cipher.c checks against draft-ietf-sshm-chacha20-poly1305-04
 * Appendix A.  The identification line follows RFC 4253 section 4.2 (255
 * characters with CR LF, so 253 of content), the sequence numbers section
 * 6.4, and the reset draft-ietf-sshm-strict-kex-02 section 3.3.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "wire.h"
#include "ssh-packet.h"

/* The number of random feeds of the robustness check. */
#define TEST_RANDOM_RUNS	100000U

/* The largest payload ssh_packet_send takes (ssh-packet.h: SSH_PACKET_LIMIT - 255 - 1). */
#define TEST_PAYLOAD_MAX	(SSH_PACKET_LIMIT - 255U - 1U)

/* The room of the buffers that collect payloads. */
#define TEST_PAYLOAD_LIMIT	(1024U * 1024U)

/* One wire buffer for the frames the test builds (large: static). */
static struct wire frame_wire;

/* One big buffer for payloads and output. */
static uint8_t big_payload[SSH_PACKET_LIMIT + 16U];
static uint8_t big_bytes[SSH_PACKET_OUTPUT_MAX + 16U];

/* The state of the pseudo random generator that makes the random inputs. */
static uint32_t random_state = 0x2545f491U;

static void test_unkeyed_send(void);
static void test_unkeyed_send_max(void);
static void test_send_errors(void);
static void test_unkeyed_receive(void);
static void test_unkeyed_receive_errors(void);
static void test_keyed_roundtrip(void);
static void test_keyed_tamper(void);
static void test_keyed_errors(void);
static void test_keyed_partial(void);
static void test_counters(void);
static void test_seq_wrap(void);
static void test_take_line(void);
static void test_take_line_limits(void);
static void test_raw_and_pending(void);
static void test_limits(void);
static void test_random_feed(void);
static void make_key(uint8_t *key, uint8_t seed);
static uint32_t next_random(void);
static void build_frame(struct wire *wire, uint32_t length, unsigned padding_field, const uint8_t *payload, size_t size, size_t padding_bytes);
static size_t craft_keyed(const struct ssh_chachapoly *cipher, uint32_t seq, uint32_t length, unsigned padding_field, const uint8_t *payload, size_t size, size_t padding_bytes, uint8_t *out);
static int take_payload(struct ssh_packet *packet, uint8_t *out, size_t *size);
static int move_output(struct ssh_packet *from, struct ssh_packet *to);
static void feed_text(struct ssh_packet *packet, const char *text);

/* Runs every check of the packet layer and prints the last line. */
int
main(void)
{
	/* Runs each group of checks. */
	test_unkeyed_send();
	test_unkeyed_send_max();
	test_send_errors();
	test_unkeyed_receive();
	test_unkeyed_receive_errors();
	test_keyed_roundtrip();
	test_keyed_tamper();
	test_keyed_errors();
	test_keyed_partial();
	test_counters();
	test_seq_wrap();
	test_take_line();
	test_take_line_limits();
	test_raw_and_pending();
	test_limits();
	test_random_feed();

	/* Prints the verdict. */
	return check_finish("packet");
}

/* Fills a 64-byte key with bytes that depend on the seed. */
static void
make_key(
	uint8_t *key,
	uint8_t seed)
{
	size_t index;

	/* Makes each byte depend on its place. */
	for (index = 0; index < SSH_CHACHAPOLY_KEY; index++)
		key[index] = (uint8_t)(seed * 13U + index * 5U + 1U);
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

/*
 * Builds an unkeyed frame with wire.h: packet_length, padding_length, the
 * payload and padding_bytes bytes of 0xa5.  The two lengths are given, not
 * computed, so a test can build a frame that breaks the rules.
 */
static void
build_frame(
	struct wire *wire,
	uint32_t length,
	unsigned padding_field,
	const uint8_t *payload,
	size_t size,
	size_t padding_bytes)
{
	size_t index;

	/* Writes the fields in the order of RFC 4253 6. */
	wire_reset(wire);
	wire_u32(wire, length);
	wire_u8(wire, padding_field);
	wire_bytes(wire, payload, size);
	for (index = 0; index < padding_bytes; index++)
		wire_u8(wire, 0xa5);
}

/*
 * Builds a keyed frame (encrypted, with the tag) in out for sequence
 * number seq the same way as build_frame, and returns its size.
 */
static size_t
craft_keyed(
	const struct ssh_chachapoly *cipher,
	uint32_t seq,
	uint32_t length,
	unsigned padding_field,
	const uint8_t *payload,
	size_t size,
	size_t padding_bytes,
	uint8_t *out)
{
	size_t total;
	int error;

	/* Builds the plain frame, then encrypts it in place and appends the tag. */
	build_frame(&frame_wire, length, padding_field, payload, size, padding_bytes);
	memcpy(out, frame_wire.data, frame_wire.size);
	total = frame_wire.size;
	error = ssh_chachapoly_seal(cipher, seq, out, total, out + total);
	CHECK_EQ(error, 0);
	return total + SSH_CHACHAPOLY_TAG;
}

/* Takes one payload out of the packet layer into out (size bytes) and returns the result of ssh_packet_next. */
static int
take_payload(
	struct ssh_packet *packet,
	uint8_t *out,
	size_t *size)
{
	struct ssh_buf payload;
	int error;

	ssh_buf_init(&payload, TEST_PAYLOAD_LIMIT, 0);
	error = ssh_packet_next(packet, &payload);
	*size = 0;
	if (error == 0) {
		*size = payload.size;
		if (payload.size > 0)
			memcpy(out, payload.data, payload.size);
	} else {
		CHECK_EQ(payload.size, 0);
	}
	ssh_buf_free(&payload);
	return error;
}

/* Feeds everything the sender has framed to the receiver and marks it written; returns the feed's result. */
static int
move_output(
	struct ssh_packet *from,
	struct ssh_packet *to)
{
	const uint8_t *data;
	size_t size;
	int error;

	data = ssh_packet_pending(from, &size);
	if (data == NULL)
		return 0;
	error = ssh_packet_feed(to, data, size);
	ssh_packet_written(from, size);
	return error;
}

/* Feeds a NUL-terminated text to the input. */
static void
feed_text(
	struct ssh_packet *packet,
	const char *text)
{
	int error;

	error = ssh_packet_feed(packet, (const uint8_t *)text, strlen(text));
	CHECK_EQ(error, 0);
}

/* Checks the frames an unkeyed sender makes for payloads of 1 to 40 bytes. */
static void
test_unkeyed_send(void)
{
	struct ssh_packet packet;
	uint8_t payload[64];
	const uint8_t *out;
	size_t out_size;
	size_t size;
	size_t index;
	size_t padding;
	size_t length;
	size_t nonzero;
	int error;

	ssh_packet_init(&packet);
	nonzero = 0;
	for (size = 1; size <= 40; size++) {
		for (index = 0; index < size; index++)
			payload[index] = (uint8_t)(0x40 + index + size);
		error = ssh_packet_send(&packet, payload, size);
		CHECK_EQ(error, 0);
		out = ssh_packet_pending(&packet, &out_size);
		CHECK(out != NULL);
		if (out == NULL)
			break;

		/* The frame: length, padding_length, payload, padding. */
		padding = out[4];
		length = 1U + size + padding;
		CHECK(padding >= 4U);
		CHECK(padding <= 11U);

		/* Builds the expected leading bytes apart from the module and compares. */
		build_frame(&frame_wire, (uint32_t)length, (unsigned)padding, payload, size, 0);
		CHECK_EQ(out_size, 4U + length);
		CHECK_BYTES(out, frame_wire.data, frame_wire.size);

		/* Unkeyed, the four bytes of the length count in the alignment. */
		CHECK_EQ(out_size % 8U, 0);

		/* Collects whether the padding is random (not all zero). */
		for (index = 0; index < padding; index++) {
			if (out[5 + size + index] != 0)
				nonzero++;
		}
		ssh_packet_written(&packet, out_size);
	}
	CHECK(nonzero > 0);
	CHECK_EQ(packet.send.seq, 40);
	ssh_packet_free(&packet);
}

/* Checks the largest payload: sent, framed, and received again. */
static void
test_unkeyed_send_max(void)
{
	struct ssh_packet sender;
	struct ssh_packet receiver;
	const uint8_t *out;
	size_t out_size;
	size_t size;
	size_t index;
	int error;

	for (index = 0; index < TEST_PAYLOAD_MAX; index++)
		big_payload[index] = (uint8_t)(index * 7U + 3U);

	ssh_packet_init(&sender);
	ssh_packet_init(&receiver);
	error = ssh_packet_send(&sender, big_payload, TEST_PAYLOAD_MAX);
	CHECK_EQ(error, 0);
	out = ssh_packet_pending(&sender, &out_size);
	CHECK(out != NULL);
	if (out != NULL) {
		CHECK(ssh_load_be32(out) <= SSH_PACKET_LIMIT);
		CHECK_EQ(out_size % 8U, 0);
		CHECK_EQ(ssh_load_be32(out), out_size - 4U);
	}
	error = move_output(&sender, &receiver);
	CHECK_EQ(error, 0);
	error = take_payload(&receiver, big_bytes, &size);
	CHECK_EQ(error, 0);
	CHECK_EQ(size, TEST_PAYLOAD_MAX);
	CHECK_BYTES(big_bytes, big_payload, TEST_PAYLOAD_MAX);
	ssh_packet_free(&sender);
	ssh_packet_free(&receiver);
}

/* Checks the payload sizes ssh_packet_send refuses, keyed and unkeyed, and that nothing changes. */
static void
test_send_errors(void)
{
	struct ssh_packet packet;
	uint8_t key[SSH_CHACHAPOLY_KEY];
	uint8_t payload[4];
	size_t out_size;
	int pass;
	int error;

	memset(payload, 1, sizeof(payload));
	make_key(key, 2);
	for (pass = 0; pass < 2; pass++) {
		ssh_packet_init(&packet);
		if (pass == 1)
			ssh_packet_set_send_key(&packet, key);

		/* Refuses a missing, an empty and a too long payload. */
		error = ssh_packet_send(&packet, NULL, 1);
		CHECK_EQ(error, EINVAL);
		error = ssh_packet_send(&packet, payload, 0);
		CHECK_EQ(error, EINVAL);
		error = ssh_packet_send(&packet, big_payload, TEST_PAYLOAD_MAX + 1U);
		CHECK_EQ(error, EINVAL);
		CHECK(ssh_packet_pending(&packet, &out_size) == NULL);
		CHECK_EQ(out_size, 0);
		CHECK_EQ(packet.send.seq, 0);
		CHECK_EQ(packet.send.packets, 0);

		/* The payload of one byte works. */
		error = ssh_packet_send(&packet, payload, 1);
		CHECK_EQ(error, 0);
		CHECK_EQ(packet.send.seq, 1);
		ssh_packet_free(&packet);
	}
}

/* Checks receiving unkeyed frames built with wire.h: whole, a byte at a time, and two at once. */
static void
test_unkeyed_receive(void)
{
	struct ssh_packet packet;
	static uint8_t second_frame[64];
	uint8_t first[16];
	uint8_t second[3];
	uint8_t out[32];
	size_t first_size;
	size_t second_size;
	size_t size;
	size_t index;
	int error;

	for (index = 0; index < sizeof(first); index++)
		first[index] = (uint8_t)(0x10 + index);
	second[0] = 0x99;
	second[1] = 0x98;
	second[2] = 0x97;

	/* A frame of 16 payload bytes: packet_length 1 + 16 + 11. */
	build_frame(&frame_wire, 28, 11, first, sizeof(first), 11);
	first_size = frame_wire.size;
	CHECK_EQ(first_size % 8U, 0);

	/* Fed whole. */
	ssh_packet_init(&packet);
	error = ssh_packet_feed(&packet, frame_wire.data, first_size);
	CHECK_EQ(error, 0);
	error = take_payload(&packet, out, &size);
	CHECK_EQ(error, 0);
	CHECK_EQ(size, sizeof(first));
	CHECK_BYTES(out, first, sizeof(first));
	CHECK_EQ(packet.receive.seq, 1);
	CHECK_EQ(packet.input.size, 0);
	error = take_payload(&packet, out, &size);
	CHECK_EQ(error, EAGAIN);
	ssh_packet_free(&packet);

	/* Fed a byte at a time: EAGAIN until the last byte, then the same payload. */
	ssh_packet_init(&packet);
	for (index = 0; index < first_size; index++) {
		error = ssh_packet_feed(&packet, frame_wire.data + index, 1);
		CHECK_EQ(error, 0);
		error = take_payload(&packet, out, &size);
		if (index + 1U < first_size) {
			CHECK_EQ(error, EAGAIN);
		} else {
			CHECK_EQ(error, 0);
			CHECK_EQ(size, sizeof(first));
			CHECK_BYTES(out, first, sizeof(first));
		}
	}
	ssh_packet_free(&packet);

	/* Two frames in one feed, then a third byte pair of the next. */
	build_frame(&frame_wire, 8, 4, second, sizeof(second), 4);
	second_size = frame_wire.size;
	memcpy(second_frame, frame_wire.data, second_size);
	build_frame(&frame_wire, 28, 11, first, sizeof(first), 11);
	memcpy(big_bytes, frame_wire.data, first_size);
	memcpy(big_bytes + first_size, second_frame, second_size);
	ssh_packet_init(&packet);
	error = ssh_packet_feed(&packet, big_bytes, first_size + second_size);
	CHECK_EQ(error, 0);
	error = take_payload(&packet, out, &size);
	CHECK_EQ(error, 0);
	CHECK_EQ(size, sizeof(first));
	CHECK_BYTES(out, first, sizeof(first));
	error = take_payload(&packet, out, &size);
	CHECK_EQ(error, 0);
	CHECK_EQ(size, sizeof(second));
	CHECK_BYTES(out, second, sizeof(second));
	CHECK_EQ(packet.receive.seq, 2);
	error = take_payload(&packet, out, &size);
	CHECK_EQ(error, EAGAIN);
	ssh_packet_free(&packet);

	/* The smallest packet_length (5: padding_length 4 and no payload) gives an empty payload. */
	build_frame(&frame_wire, 5, 4, NULL, 0, 4);
	ssh_packet_init(&packet);
	error = ssh_packet_feed(&packet, frame_wire.data, frame_wire.size);
	CHECK_EQ(error, 0);
	error = take_payload(&packet, out, &size);
	CHECK_EQ(error, 0);
	CHECK_EQ(size, 0);
	ssh_packet_free(&packet);

	/* A frame that is not aligned is received (the receiver does not check alignment). */
	build_frame(&frame_wire, 10, 6, second, sizeof(second), 6);
	ssh_packet_init(&packet);
	error = ssh_packet_feed(&packet, frame_wire.data, frame_wire.size);
	CHECK_EQ(error, 0);
	error = take_payload(&packet, out, &size);
	CHECK_EQ(error, 0);
	CHECK_EQ(size, sizeof(second));
	ssh_packet_free(&packet);

	/* The largest packet_length is accepted: payload of SSH_PACKET_LIMIT - 1 - 4 bytes. */
	for (index = 0; index < SSH_PACKET_LIMIT - 5U; index++)
		big_payload[index] = (uint8_t)(index ^ 0x3c);
	build_frame(&frame_wire, SSH_PACKET_LIMIT, 4, big_payload, SSH_PACKET_LIMIT - 5U, 4);
	ssh_packet_init(&packet);
	error = ssh_packet_feed(&packet, frame_wire.data, frame_wire.size);
	CHECK_EQ(error, 0);
	error = take_payload(&packet, big_bytes, &size);
	CHECK_EQ(error, 0);
	CHECK_EQ(size, SSH_PACKET_LIMIT - 5U);
	CHECK_BYTES(big_bytes, big_payload, SSH_PACKET_LIMIT - 5U);
	ssh_packet_free(&packet);
}

/* Checks the unkeyed frames the receiver refuses. */
static void
test_unkeyed_receive_errors(void)
{
	struct ssh_packet packet;
	uint8_t payload[8];
	uint8_t out[16];
	size_t size;
	int error;

	memset(payload, 0x33, sizeof(payload));

	/* packet_length 4 is below the smallest packet. */
	build_frame(&frame_wire, 4, 4, NULL, 0, 0);
	ssh_packet_init(&packet);
	error = ssh_packet_feed(&packet, frame_wire.data, frame_wire.size);
	CHECK_EQ(error, 0);
	error = take_payload(&packet, out, &size);
	CHECK_EQ(error, EMSGSIZE);
	ssh_packet_free(&packet);

	/* packet_length 0 as well. */
	build_frame(&frame_wire, 0, 0, NULL, 0, 0);
	ssh_packet_init(&packet);
	error = ssh_packet_feed(&packet, frame_wire.data, frame_wire.size);
	CHECK_EQ(error, 0);
	error = take_payload(&packet, out, &size);
	CHECK_EQ(error, EMSGSIZE);
	ssh_packet_free(&packet);

	/* packet_length above the limit is refused as soon as the four bytes are there, without the rest. */
	wire_reset(&frame_wire);
	wire_u32(&frame_wire, SSH_PACKET_LIMIT + 1U);
	ssh_packet_init(&packet);
	error = ssh_packet_feed(&packet, frame_wire.data, frame_wire.size);
	CHECK_EQ(error, 0);
	error = take_payload(&packet, out, &size);
	CHECK_EQ(error, EMSGSIZE);
	ssh_packet_free(&packet);

	/* padding_length 3 is below the minimum. */
	build_frame(&frame_wire, 12, 3, payload, sizeof(payload), 3);
	ssh_packet_init(&packet);
	error = ssh_packet_feed(&packet, frame_wire.data, frame_wire.size);
	CHECK_EQ(error, 0);
	error = take_payload(&packet, out, &size);
	CHECK_EQ(error, EINVAL);
	ssh_packet_free(&packet);

	/* padding_length equal to packet_length leaves no room for its own byte. */
	build_frame(&frame_wire, 8, 8, payload, 3, 4);
	ssh_packet_init(&packet);
	error = ssh_packet_feed(&packet, frame_wire.data, frame_wire.size);
	CHECK_EQ(error, 0);
	error = take_payload(&packet, out, &size);
	CHECK_EQ(error, EINVAL);
	ssh_packet_free(&packet);

	/* padding_length 255 in a short packet is refused as well. */
	build_frame(&frame_wire, 16, 255, payload, sizeof(payload), 7);
	ssh_packet_init(&packet);
	error = ssh_packet_feed(&packet, frame_wire.data, frame_wire.size);
	CHECK_EQ(error, 0);
	error = take_payload(&packet, out, &size);
	CHECK_EQ(error, EINVAL);
	ssh_packet_free(&packet);

	/* padding_length one below packet_length is the empty payload; one above is refused (checked above). */
	build_frame(&frame_wire, 8, 7, NULL, 0, 7);
	ssh_packet_init(&packet);
	error = ssh_packet_feed(&packet, frame_wire.data, frame_wire.size);
	CHECK_EQ(error, 0);
	error = take_payload(&packet, out, &size);
	CHECK_EQ(error, 0);
	CHECK_EQ(size, 0);
	ssh_packet_free(&packet);
}

/* Checks a keyed round trip between two layers in both directions, with the sequence numbers. */
static void
test_keyed_roundtrip(void)
{
	struct ssh_packet a;
	struct ssh_packet b;
	uint8_t key_ab[SSH_CHACHAPOLY_KEY];
	uint8_t key_ba[SSH_CHACHAPOLY_KEY];
	uint8_t payload[200];
	uint8_t out[256];
	const uint8_t *frame;
	size_t frame_size;
	size_t size;
	size_t count;
	size_t index;
	int error;

	make_key(key_ab, 11);
	make_key(key_ba, 12);
	ssh_packet_init(&a);
	ssh_packet_init(&b);
	ssh_packet_set_send_key(&a, key_ab);
	ssh_packet_set_receive_key(&b, key_ab);
	ssh_packet_set_send_key(&b, key_ba);
	ssh_packet_set_receive_key(&a, key_ba);

	for (count = 1; count <= 30; count++) {
		for (index = 0; index < count; index++)
			payload[index] = (uint8_t)(count * 3U + index);

		/* a to b. */
		error = ssh_packet_send(&a, payload, count);
		CHECK_EQ(error, 0);
		frame = ssh_packet_pending(&a, &frame_size);
		CHECK(frame != NULL);
		if (frame != NULL) {
			/* The tag is 16 bytes and the rest, without the length, is a multiple of 8. */
			CHECK(frame_size > SSH_CHACHAPOLY_TAG + 4U);
			CHECK_EQ((frame_size - 4U - SSH_CHACHAPOLY_TAG) % 8U, 0);
		}
		error = move_output(&a, &b);
		CHECK_EQ(error, 0);
		error = take_payload(&b, out, &size);
		CHECK_EQ(error, 0);
		CHECK_EQ(size, count);
		CHECK_BYTES(out, payload, count);
		CHECK_EQ(a.send.seq, count);
		CHECK_EQ(b.receive.seq, count);

		/* b to a, with another key. */
		error = ssh_packet_send(&b, payload, count);
		CHECK_EQ(error, 0);
		error = move_output(&b, &a);
		CHECK_EQ(error, 0);
		error = take_payload(&a, out, &size);
		CHECK_EQ(error, 0);
		CHECK_EQ(size, count);
		CHECK_BYTES(out, payload, count);
	}
	CHECK_EQ(a.receive.seq, 30);
	CHECK_EQ(b.send.seq, 30);

	/* The wire bytes are not the plaintext. */
	error = ssh_packet_send(&a, payload, 100);
	CHECK_EQ(error, 0);
	frame = ssh_packet_pending(&a, &frame_size);
	CHECK(frame != NULL && memcmp(frame + 5, payload, 100) != 0);
	ssh_packet_free(&a);
	ssh_packet_free(&b);
}

/* Checks that one changed bit of a keyed frame is refused. */
static void
test_keyed_tamper(void)
{
	struct ssh_packet sender;
	struct ssh_packet receiver;
	uint8_t key[SSH_CHACHAPOLY_KEY];
	uint8_t payload[20];
	uint8_t frame[128];
	uint8_t out[64];
	const uint8_t *pending;
	size_t frame_size;
	size_t size;
	size_t bit;
	int error;

	make_key(key, 21);
	memset(payload, 0x6d, sizeof(payload));
	ssh_packet_init(&sender);
	ssh_packet_set_send_key(&sender, key);
	error = ssh_packet_send(&sender, payload, sizeof(payload));
	CHECK_EQ(error, 0);
	pending = ssh_packet_pending(&sender, &frame_size);
	CHECK(pending != NULL && frame_size <= sizeof(frame));
	if (pending == NULL || frame_size > sizeof(frame)) {
		ssh_packet_free(&sender);
		return;
	}
	memcpy(frame, pending, frame_size);
	ssh_packet_free(&sender);

	/* Each bit of the whole frame: a change in the body or tag is EBADMSG; a change in the length never delivers. */
	for (bit = 0; bit < frame_size * 8U; bit++) {
		ssh_packet_init(&receiver);
		ssh_packet_set_receive_key(&receiver, key);
		frame[bit / 8U] ^= (uint8_t)(1U << (bit % 8U));
		error = ssh_packet_feed(&receiver, frame, frame_size);
		CHECK_EQ(error, 0);
		error = take_payload(&receiver, out, &size);
		if (bit < 32U)
			CHECK(error == EAGAIN || error == EBADMSG || error == EMSGSIZE);
		else
			CHECK_EQ(error, EBADMSG);
		CHECK_EQ(receiver.receive.seq, 0);
		frame[bit / 8U] ^= (uint8_t)(1U << (bit % 8U));
		ssh_packet_free(&receiver);
	}

	/* The untouched frame is received. */
	ssh_packet_init(&receiver);
	ssh_packet_set_receive_key(&receiver, key);
	error = ssh_packet_feed(&receiver, frame, frame_size);
	CHECK_EQ(error, 0);
	error = take_payload(&receiver, out, &size);
	CHECK_EQ(error, 0);
	CHECK_EQ(size, sizeof(payload));
	CHECK_BYTES(out, payload, sizeof(payload));
	ssh_packet_free(&receiver);

	/* The same frame under another key, and at another sequence number, is refused. */
	make_key(key, 22);
	ssh_packet_init(&receiver);
	ssh_packet_set_receive_key(&receiver, key);
	error = ssh_packet_feed(&receiver, frame, frame_size);
	CHECK_EQ(error, 0);
	error = take_payload(&receiver, out, &size);
	CHECK(error == EAGAIN || error == EBADMSG || error == EMSGSIZE);
	ssh_packet_free(&receiver);
	make_key(key, 21);
	ssh_packet_init(&receiver);
	ssh_packet_set_receive_key(&receiver, key);
	receiver.receive.seq = 1;
	error = ssh_packet_feed(&receiver, frame, frame_size);
	CHECK_EQ(error, 0);
	error = take_payload(&receiver, out, &size);
	CHECK(error == EAGAIN || error == EBADMSG || error == EMSGSIZE);
	ssh_packet_free(&receiver);
}

/* Checks the keyed frames whose lengths break the rules (made with the cipher, so the tag is right). */
static void
test_keyed_errors(void)
{
	struct ssh_packet packet;
	struct ssh_chachapoly cipher;
	uint8_t key[SSH_CHACHAPOLY_KEY];
	uint8_t payload[8];
	uint8_t frame[64];
	uint8_t out[16];
	size_t frame_size;
	size_t size;
	int error;

	make_key(key, 31);
	ssh_chachapoly_init(&cipher, key);
	memset(payload, 0x44, sizeof(payload));

	/* packet_length 4 is refused. */
	frame_size = craft_keyed(&cipher, 0, 4, 4, NULL, 0, 0, frame);
	ssh_packet_init(&packet);
	ssh_packet_set_receive_key(&packet, key);
	error = ssh_packet_feed(&packet, frame, frame_size);
	CHECK_EQ(error, 0);
	error = take_payload(&packet, out, &size);
	CHECK_EQ(error, EMSGSIZE);
	ssh_packet_free(&packet);

	/* packet_length SSH_PACKET_LIMIT + 1 is refused from the four length bytes alone. */
	frame_size = craft_keyed(&cipher, 0, SSH_PACKET_LIMIT + 1U, 4, NULL, 0, 0, frame);
	ssh_packet_init(&packet);
	ssh_packet_set_receive_key(&packet, key);
	error = ssh_packet_feed(&packet, frame, 4);
	CHECK_EQ(error, 0);
	error = take_payload(&packet, out, &size);
	CHECK_EQ(error, EMSGSIZE);
	ssh_packet_free(&packet);

	/* padding_length 3 is refused after the tag is checked. */
	frame_size = craft_keyed(&cipher, 0, 12, 3, payload, sizeof(payload), 3, frame);
	ssh_packet_init(&packet);
	ssh_packet_set_receive_key(&packet, key);
	error = ssh_packet_feed(&packet, frame, frame_size);
	CHECK_EQ(error, 0);
	error = take_payload(&packet, out, &size);
	CHECK_EQ(error, EINVAL);
	ssh_packet_free(&packet);

	/* padding_length equal to packet_length is refused. */
	frame_size = craft_keyed(&cipher, 0, 8, 8, payload, 3, 4, frame);
	ssh_packet_init(&packet);
	ssh_packet_set_receive_key(&packet, key);
	error = ssh_packet_feed(&packet, frame, frame_size);
	CHECK_EQ(error, 0);
	error = take_payload(&packet, out, &size);
	CHECK_EQ(error, EINVAL);
	ssh_packet_free(&packet);

	/* A good minimal frame (padding_length 4, no payload) is received. */
	frame_size = craft_keyed(&cipher, 0, 5, 4, NULL, 0, 4, frame);
	ssh_packet_init(&packet);
	ssh_packet_set_receive_key(&packet, key);
	error = ssh_packet_feed(&packet, frame, frame_size);
	CHECK_EQ(error, 0);
	error = take_payload(&packet, out, &size);
	CHECK_EQ(error, 0);
	CHECK_EQ(size, 0);
	ssh_packet_free(&packet);

	ssh_chachapoly_clear(&cipher);
}

/* Checks that a keyed frame waits (EAGAIN) in the body and in the tag, and completes when the last byte comes. */
static void
test_keyed_partial(void)
{
	struct ssh_packet sender;
	struct ssh_packet receiver;
	uint8_t key[SSH_CHACHAPOLY_KEY];
	uint8_t payload[30];
	uint8_t frame[128];
	uint8_t out[64];
	const uint8_t *pending;
	size_t frame_size;
	size_t size;
	size_t index;
	int error;

	make_key(key, 41);
	for (index = 0; index < sizeof(payload); index++)
		payload[index] = (uint8_t)(index + 90U);
	ssh_packet_init(&sender);
	ssh_packet_set_send_key(&sender, key);
	error = ssh_packet_send(&sender, payload, sizeof(payload));
	CHECK_EQ(error, 0);
	pending = ssh_packet_pending(&sender, &frame_size);
	CHECK(pending != NULL && frame_size <= sizeof(frame));
	if (pending == NULL || frame_size > sizeof(frame)) {
		ssh_packet_free(&sender);
		return;
	}
	memcpy(frame, pending, frame_size);
	ssh_packet_free(&sender);

	/* A byte at a time: every prefix waits, the whole frame is received. */
	ssh_packet_init(&receiver);
	ssh_packet_set_receive_key(&receiver, key);
	for (index = 0; index < frame_size; index++) {
		error = ssh_packet_feed(&receiver, frame + index, 1);
		CHECK_EQ(error, 0);
		error = take_payload(&receiver, out, &size);
		if (index + 1U < frame_size) {
			CHECK_EQ(error, EAGAIN);
		} else {
			CHECK_EQ(error, 0);
			CHECK_EQ(size, sizeof(payload));
			CHECK_BYTES(out, payload, sizeof(payload));
		}
	}
	ssh_packet_free(&receiver);

	/* Two frames in one feed. */
	ssh_packet_init(&sender);
	ssh_packet_set_send_key(&sender, key);
	ssh_packet_init(&receiver);
	ssh_packet_set_receive_key(&receiver, key);
	error = ssh_packet_send(&sender, payload, 5);
	CHECK_EQ(error, 0);
	error = ssh_packet_send(&sender, payload + 5, 7);
	CHECK_EQ(error, 0);
	error = move_output(&sender, &receiver);
	CHECK_EQ(error, 0);
	error = take_payload(&receiver, out, &size);
	CHECK_EQ(error, 0);
	CHECK_EQ(size, 5);
	CHECK_BYTES(out, payload, 5);
	error = take_payload(&receiver, out, &size);
	CHECK_EQ(error, 0);
	CHECK_EQ(size, 7);
	CHECK_BYTES(out, payload + 5, 7);
	error = take_payload(&receiver, out, &size);
	CHECK_EQ(error, EAGAIN);
	ssh_packet_free(&sender);
	ssh_packet_free(&receiver);
}

/* Checks the byte and packet counters, and that setting a key restarts them. */
static void
test_counters(void)
{
	struct ssh_packet a;
	struct ssh_packet b;
	uint8_t key[SSH_CHACHAPOLY_KEY];
	uint8_t payload[32];
	uint8_t out[64];
	const uint8_t *pending;
	size_t frame_size;
	size_t total;
	size_t size;
	int count;
	int error;

	make_key(key, 51);
	memset(payload, 9, sizeof(payload));
	ssh_packet_init(&a);
	ssh_packet_init(&b);
	CHECK_EQ(a.send.keyed, 0);
	CHECK_EQ(a.receive.keyed, 0);

	/* Unkeyed packets count too. */
	error = ssh_packet_send(&a, payload, 10);
	CHECK_EQ(error, 0);
	pending = ssh_packet_pending(&a, &frame_size);
	CHECK(pending != NULL);
	CHECK_EQ(a.send.bytes, frame_size);
	CHECK_EQ(a.send.packets, 1);
	error = move_output(&a, &b);
	CHECK_EQ(error, 0);
	error = take_payload(&b, out, &size);
	CHECK_EQ(error, 0);
	CHECK_EQ(b.receive.bytes, frame_size);
	CHECK_EQ(b.receive.packets, 1);

	/* A length read under the null cipher is forgotten when the receiving key is set. */
	build_frame(&frame_wire, 28, 11, payload, 16, 11);
	error = ssh_packet_feed(&b, frame_wire.data, 4);
	CHECK_EQ(error, 0);
	error = take_payload(&b, out, &size);
	CHECK_EQ(error, EAGAIN);
	CHECK_EQ(b.length, 28);

	/* A new key restarts the counters and keys the direction. */
	ssh_packet_set_send_key(&a, key);
	ssh_packet_set_receive_key(&b, key);
	CHECK_EQ(b.length, 0);
	ssh_buf_reset(&b.input);
	CHECK_EQ(a.send.keyed, 1);
	CHECK_EQ(b.receive.keyed, 1);
	CHECK_EQ(a.send.bytes, 0);
	CHECK_EQ(a.send.packets, 0);
	CHECK_EQ(b.receive.bytes, 0);
	CHECK_EQ(b.receive.packets, 0);
	CHECK_EQ(a.send.seq, 1);

	/* Keyed packets count the tag too. */
	total = 0;
	for (count = 0; count < 5; count++) {
		error = ssh_packet_send(&a, payload, 10U + (size_t)count * 3U);
		CHECK_EQ(error, 0);
		pending = ssh_packet_pending(&a, &frame_size);
		CHECK(pending != NULL);
		total += frame_size;
		error = move_output(&a, &b);
		CHECK_EQ(error, 0);
		error = take_payload(&b, out, &size);
		CHECK_EQ(error, 0);
	}
	CHECK_EQ(a.send.bytes, total);
	CHECK_EQ(a.send.packets, 5);
	CHECK_EQ(b.receive.bytes, total);
	CHECK_EQ(b.receive.packets, 5);
	ssh_packet_free(&a);
	ssh_packet_free(&b);
}

/* Checks the wrap of the sequence numbers and the reset. */
static void
test_seq_wrap(void)
{
	struct ssh_packet a;
	struct ssh_packet b;
	uint8_t key[SSH_CHACHAPOLY_KEY];
	uint8_t payload[6];
	uint8_t out[16];
	size_t size;
	int pass;
	int error;

	make_key(key, 61);
	memset(payload, 0x21, sizeof(payload));
	for (pass = 0; pass < 2; pass++) {
		ssh_packet_init(&a);
		ssh_packet_init(&b);
		if (pass == 1) {
			ssh_packet_set_send_key(&a, key);
			ssh_packet_set_receive_key(&b, key);
		}
		a.send.seq = 0xffffffffU;
		b.receive.seq = 0xffffffffU;
		CHECK_EQ(a.send.wrapped, 0);

		/* The packet with the last sequence number wraps the counter; the next one uses 0. */
		error = ssh_packet_send(&a, payload, sizeof(payload));
		CHECK_EQ(error, 0);
		CHECK_EQ(a.send.seq, 0);
		CHECK_EQ(a.send.wrapped, 1);
		error = ssh_packet_send(&a, payload, sizeof(payload));
		CHECK_EQ(error, 0);
		CHECK_EQ(a.send.seq, 1);
		CHECK_EQ(a.send.wrapped, 1);
		error = move_output(&a, &b);
		CHECK_EQ(error, 0);
		error = take_payload(&b, out, &size);
		CHECK_EQ(error, 0);
		CHECK_EQ(b.receive.seq, 0);
		CHECK_EQ(b.receive.wrapped, 1);
		error = take_payload(&b, out, &size);
		CHECK_EQ(error, 0);
		CHECK_EQ(b.receive.seq, 1);
		CHECK_BYTES(out, payload, sizeof(payload));

		/* A reset sets 0 and clears the flag, one direction at a time. */
		ssh_packet_reset_seq(&a, 1);
		CHECK_EQ(a.send.seq, 0);
		CHECK_EQ(a.send.wrapped, 0);
		CHECK_EQ(b.receive.wrapped, 1);
		ssh_packet_reset_seq(&b, 0);
		CHECK_EQ(b.receive.seq, 0);
		CHECK_EQ(b.receive.wrapped, 0);
		CHECK_EQ(b.send.seq, 0);

		/* After the reset the two sides agree on sequence number 0. */
		error = ssh_packet_send(&a, payload, sizeof(payload));
		CHECK_EQ(error, 0);
		error = move_output(&a, &b);
		CHECK_EQ(error, 0);
		error = take_payload(&b, out, &size);
		CHECK_EQ(error, 0);
		CHECK_EQ(b.receive.seq, 1);
		CHECK_BYTES(out, payload, sizeof(payload));

		/* Resetting only one side makes the keyed frame fail (the nonce differs). */
		if (pass == 1) {
			ssh_packet_reset_seq(&a, 1);
			error = ssh_packet_send(&a, payload, sizeof(payload));
			CHECK_EQ(error, 0);
			error = move_output(&a, &b);
			CHECK_EQ(error, 0);
			error = take_payload(&b, out, &size);
			CHECK(error == EAGAIN || error == EBADMSG || error == EMSGSIZE);
		}
		ssh_packet_free(&a);
		ssh_packet_free(&b);
	}
}

/* Checks ssh_packet_take_line on the line endings and the cases that wait or fail. */
static void
test_take_line(void)
{
	struct ssh_packet packet;
	struct ssh_packet sender;
	uint8_t payload[3];
	uint8_t out[16];
	char line[254];
	size_t length;
	size_t size;
	int error;

	/* CR LF: the line without the ending, and the bytes after it stay. */
	ssh_packet_init(&packet);
	feed_text(&packet, "SSH-2.0-test\r\nREST");
	error = ssh_packet_take_line(&packet, line, sizeof(line), &length);
	CHECK_EQ(error, 0);
	CHECK_EQ(length, 12);
	CHECK(strcmp(line, "SSH-2.0-test") == 0);
	CHECK_EQ(packet.input.size, 4);
	CHECK_BYTES(packet.input.data, "REST", 4);
	ssh_packet_free(&packet);

	/* LF alone. */
	ssh_packet_init(&packet);
	feed_text(&packet, "abc\nxy");
	error = ssh_packet_take_line(&packet, line, sizeof(line), &length);
	CHECK_EQ(error, 0);
	CHECK_EQ(length, 3);
	CHECK(strcmp(line, "abc") == 0);
	CHECK_EQ(packet.input.size, 2);
	ssh_packet_free(&packet);

	/* Empty lines. */
	ssh_packet_init(&packet);
	feed_text(&packet, "\r\n\n");
	error = ssh_packet_take_line(&packet, line, sizeof(line), &length);
	CHECK_EQ(error, 0);
	CHECK_EQ(length, 0);
	CHECK_EQ(line[0], 0);
	error = ssh_packet_take_line(&packet, line, sizeof(line), &length);
	CHECK_EQ(error, 0);
	CHECK_EQ(length, 0);
	CHECK_EQ(packet.input.size, 0);
	ssh_packet_free(&packet);

	/* A CR that is not before the LF stays in the line; only one CR is dropped. */
	ssh_packet_init(&packet);
	feed_text(&packet, "a\rb\r\r\n");
	error = ssh_packet_take_line(&packet, line, sizeof(line), &length);
	CHECK_EQ(error, 0);
	CHECK_EQ(length, 4);
	CHECK_BYTES(line, "a\rb\r", 4);
	ssh_packet_free(&packet);

	/* No LF yet: EAGAIN with an empty answer and the input kept; the rest completes the line. */
	ssh_packet_init(&packet);
	feed_text(&packet, "SSH-2.0-te");
	memset(line, 'z', sizeof(line));
	error = ssh_packet_take_line(&packet, line, sizeof(line), &length);
	CHECK_EQ(error, EAGAIN);
	CHECK_EQ(length, 0);
	CHECK_EQ(line[0], 0);
	CHECK_EQ(packet.input.size, 10);
	feed_text(&packet, "st\r");
	error = ssh_packet_take_line(&packet, line, sizeof(line), &length);
	CHECK_EQ(error, EAGAIN);
	feed_text(&packet, "\n");
	error = ssh_packet_take_line(&packet, line, sizeof(line), &length);
	CHECK_EQ(error, 0);
	CHECK(strcmp(line, "SSH-2.0-test") == 0);
	ssh_packet_free(&packet);

	/* A NUL inside the line is refused; a NUL after the LF is not part of the line. */
	ssh_packet_init(&packet);
	wire_reset(&frame_wire);
	wire_bytes(&frame_wire, "ab", 2);
	wire_u8(&frame_wire, 0);
	wire_bytes(&frame_wire, "cd\r\n", 4);
	error = ssh_packet_feed(&packet, frame_wire.data, frame_wire.size);
	CHECK_EQ(error, 0);
	error = ssh_packet_take_line(&packet, line, sizeof(line), &length);
	CHECK_EQ(error, EINVAL);
	ssh_packet_free(&packet);
	ssh_packet_init(&packet);
	wire_reset(&frame_wire);
	wire_bytes(&frame_wire, "ab\n", 3);
	wire_u8(&frame_wire, 0);
	error = ssh_packet_feed(&packet, frame_wire.data, frame_wire.size);
	CHECK_EQ(error, 0);
	error = ssh_packet_take_line(&packet, line, sizeof(line), &length);
	CHECK_EQ(error, 0);
	CHECK_EQ(length, 2);
	CHECK_EQ(packet.input.size, 1);
	ssh_packet_free(&packet);

	/* A line, then a packet in the same feed: the packet's bytes are left for ssh_packet_next. */
	payload[0] = 0xaa;
	payload[1] = 0xbb;
	payload[2] = 0xcc;
	ssh_packet_init(&sender);
	ssh_packet_init(&packet);
	feed_text(&packet, "SSH-2.0-x\r\n");
	error = ssh_packet_send(&sender, payload, sizeof(payload));
	CHECK_EQ(error, 0);
	error = move_output(&sender, &packet);
	CHECK_EQ(error, 0);
	error = ssh_packet_take_line(&packet, line, sizeof(line), &length);
	CHECK_EQ(error, 0);
	CHECK(strcmp(line, "SSH-2.0-x") == 0);
	error = take_payload(&packet, out, &size);
	CHECK_EQ(error, 0);
	CHECK_EQ(size, sizeof(payload));
	CHECK_BYTES(out, payload, sizeof(payload));
	ssh_packet_free(&packet);
	ssh_packet_free(&sender);

	/* A buffer of one byte holds only the empty line. */
	ssh_packet_init(&packet);
	feed_text(&packet, "\na\n");
	error = ssh_packet_take_line(&packet, line, 1, &length);
	CHECK_EQ(error, 0);
	CHECK_EQ(length, 0);
	error = ssh_packet_take_line(&packet, line, 1, &length);
	CHECK_EQ(error, EMSGSIZE);
	ssh_packet_free(&packet);

	/* A buffer of no byte is refused. */
	ssh_packet_init(&packet);
	feed_text(&packet, "a\n");
	error = ssh_packet_take_line(&packet, line, 0, &length);
	CHECK_EQ(error, EINVAL);
	ssh_packet_free(&packet);
}

/* Checks the line length limit: 253 characters of content, as the transport asks (capacity 254). */
static void
test_take_line_limits(void)
{
	struct ssh_packet packet;
	char line[254];
	char text[300];
	size_t length;
	size_t fill;
	int error;

	/* Content of 253 characters with CR LF and with LF is taken. */
	memset(text, 'a', sizeof(text));
	ssh_packet_init(&packet);
	error = ssh_packet_feed(&packet, (const uint8_t *)text, 253);
	CHECK_EQ(error, 0);
	error = ssh_packet_feed(&packet, (const uint8_t *)"\r\n", 2);
	CHECK_EQ(error, 0);
	error = ssh_packet_take_line(&packet, line, sizeof(line), &length);
	CHECK_EQ(error, 0);
	CHECK_EQ(length, 253);
	CHECK_EQ(line[253], 0);
	ssh_packet_free(&packet);
	ssh_packet_init(&packet);
	error = ssh_packet_feed(&packet, (const uint8_t *)text, 253);
	CHECK_EQ(error, 0);
	error = ssh_packet_feed(&packet, (const uint8_t *)"\n", 1);
	CHECK_EQ(error, 0);
	error = ssh_packet_take_line(&packet, line, sizeof(line), &length);
	CHECK_EQ(error, 0);
	CHECK_EQ(length, 253);
	ssh_packet_free(&packet);

	/* 254 characters with CR LF and with LF are too long. */
	ssh_packet_init(&packet);
	error = ssh_packet_feed(&packet, (const uint8_t *)text, 254);
	CHECK_EQ(error, 0);
	error = ssh_packet_feed(&packet, (const uint8_t *)"\r\n", 2);
	CHECK_EQ(error, 0);
	error = ssh_packet_take_line(&packet, line, sizeof(line), &length);
	CHECK_EQ(error, EMSGSIZE);
	ssh_packet_free(&packet);
	ssh_packet_init(&packet);
	error = ssh_packet_feed(&packet, (const uint8_t *)text, 254);
	CHECK_EQ(error, 0);
	error = ssh_packet_feed(&packet, (const uint8_t *)"\n", 1);
	CHECK_EQ(error, 0);
	error = ssh_packet_take_line(&packet, line, sizeof(line), &length);
	CHECK_EQ(error, EMSGSIZE);
	ssh_packet_free(&packet);

	/* Without an LF: 253 characters still wait, also with a CR at the end; 254 are already too long. */
	ssh_packet_init(&packet);
	error = ssh_packet_feed(&packet, (const uint8_t *)text, 253);
	CHECK_EQ(error, 0);
	error = ssh_packet_take_line(&packet, line, sizeof(line), &length);
	CHECK_EQ(error, EAGAIN);
	error = ssh_packet_feed(&packet, (const uint8_t *)"\r", 1);
	CHECK_EQ(error, 0);
	error = ssh_packet_take_line(&packet, line, sizeof(line), &length);
	CHECK_EQ(error, EAGAIN);
	error = ssh_packet_feed(&packet, (const uint8_t *)"x", 1);
	CHECK_EQ(error, 0);
	error = ssh_packet_take_line(&packet, line, sizeof(line), &length);
	CHECK_EQ(error, EMSGSIZE);
	ssh_packet_free(&packet);
	ssh_packet_init(&packet);
	error = ssh_packet_feed(&packet, (const uint8_t *)text, 254);
	CHECK_EQ(error, 0);
	error = ssh_packet_take_line(&packet, line, sizeof(line), &length);
	CHECK_EQ(error, EMSGSIZE);
	ssh_packet_free(&packet);

	/* A long run of bytes with no LF is refused at once, however much was fed. */
	ssh_packet_init(&packet);
	for (fill = 0; fill < 10; fill++) {
		error = ssh_packet_feed(&packet, (const uint8_t *)text, sizeof(text));
		CHECK_EQ(error, 0);
	}
	error = ssh_packet_take_line(&packet, line, sizeof(line), &length);
	CHECK_EQ(error, EMSGSIZE);
	ssh_packet_free(&packet);
}

/* Checks send_raw, pending, written and the output limit. */
static void
test_raw_and_pending(void)
{
	struct ssh_packet packet;
	const uint8_t *data;
	uint8_t payload[4];
	size_t size;
	int error;

	ssh_packet_init(&packet);
	memset(payload, 1, sizeof(payload));

	/* Nothing pending at the start. */
	size = 77;
	data = ssh_packet_pending(&packet, &size);
	CHECK(data == NULL);
	CHECK_EQ(size, 0);

	/* Raw bytes are appended as they are and counted by nothing. */
	error = ssh_packet_send_raw(&packet, (const uint8_t *)"SSH-2.0-zed\r\n", 13);
	CHECK_EQ(error, 0);
	data = ssh_packet_pending(&packet, &size);
	CHECK(data != NULL);
	CHECK_EQ(size, 13);
	CHECK_BYTES(data, "SSH-2.0-zed\r\n", 13);
	CHECK_EQ(packet.send.seq, 0);
	CHECK_EQ(packet.send.packets, 0);

	/* A framed packet goes after them. */
	error = ssh_packet_send(&packet, payload, sizeof(payload));
	CHECK_EQ(error, 0);
	data = ssh_packet_pending(&packet, &size);
	CHECK(data != NULL);
	CHECK(size > 13);
	CHECK_BYTES(data, "SSH-2.0-zed\r\n", 13);

	/* Written bytes leave from the front. */
	ssh_packet_written(&packet, 5);
	data = ssh_packet_pending(&packet, &size);
	CHECK(data != NULL);
	CHECK_BYTES(data, ".0-zed\r\n", 8);
	ssh_packet_written(&packet, 7);
	data = ssh_packet_pending(&packet, &size);
	CHECK_BYTES(data, "\n", 1);
	ssh_packet_written(&packet, 1);

	/* More than there is empties it. */
	ssh_packet_written(&packet, 100000);
	data = ssh_packet_pending(&packet, &size);
	CHECK(data == NULL);
	CHECK_EQ(size, 0);

	/* Argument checks. */
	error = ssh_packet_send_raw(&packet, NULL, 3);
	CHECK_EQ(error, EINVAL);
	error = ssh_packet_send_raw(&packet, NULL, 0);
	CHECK_EQ(error, 0);
	error = ssh_packet_feed(&packet, NULL, 3);
	CHECK_EQ(error, EINVAL);
	error = ssh_packet_feed(&packet, NULL, 0);
	CHECK_EQ(error, 0);
	ssh_packet_free(&packet);
}

/* Checks the limits of the output and the input at their exact sizes. */
static void
test_limits(void)
{
	struct ssh_packet packet;
	uint8_t payload[100];
	const uint8_t *data;
	size_t size;
	size_t index;
	int error;

	memset(big_bytes, 0x77, sizeof(big_bytes));
	memset(payload, 5, sizeof(payload));

	/* The output takes exactly its limit and refuses one byte more, then takes it after a write. */
	ssh_packet_init(&packet);
	error = ssh_packet_send_raw(&packet, big_bytes, SSH_PACKET_OUTPUT_MAX);
	CHECK_EQ(error, 0);
	error = ssh_packet_send_raw(&packet, big_bytes, 1);
	CHECK_EQ(error, ENOBUFS);
	data = ssh_packet_pending(&packet, &size);
	CHECK(data != NULL);
	CHECK_EQ(size, SSH_PACKET_OUTPUT_MAX);
	error = ssh_packet_send(&packet, payload, sizeof(payload));
	CHECK_EQ(error, ENOBUFS);
	CHECK_EQ(packet.send.seq, 0);
	CHECK_EQ(packet.send.packets, 0);
	ssh_packet_written(&packet, 1);
	error = ssh_packet_send_raw(&packet, big_bytes, 1);
	CHECK_EQ(error, 0);
	ssh_packet_written(&packet, SSH_PACKET_OUTPUT_MAX);
	error = ssh_packet_send(&packet, payload, sizeof(payload));
	CHECK_EQ(error, 0);
	CHECK_EQ(packet.send.seq, 1);
	ssh_packet_free(&packet);

	/* A packet that does not fit by a byte fails; the output keeps what it had. */
	ssh_packet_init(&packet);
	error = ssh_packet_send(&packet, payload, sizeof(payload));
	CHECK_EQ(error, 0);
	data = ssh_packet_pending(&packet, &size);
	CHECK(data != NULL);
	index = size;
	error = ssh_packet_send_raw(&packet, big_bytes, SSH_PACKET_OUTPUT_MAX - index);
	CHECK_EQ(error, 0);
	error = ssh_packet_send_raw(&packet, big_bytes, 1);
	CHECK_EQ(error, ENOBUFS);
	data = ssh_packet_pending(&packet, &size);
	CHECK_EQ(size, SSH_PACKET_OUTPUT_MAX);
	ssh_packet_free(&packet);

	/* The input takes exactly its limit and refuses one byte more without changing. */
	ssh_packet_init(&packet);
	error = ssh_packet_feed(&packet, big_bytes, SSH_PACKET_INPUT_MAX);
	CHECK_EQ(error, 0);
	CHECK_EQ(packet.input.size, SSH_PACKET_INPUT_MAX);
	error = ssh_packet_feed(&packet, big_bytes, 1);
	CHECK_EQ(error, EMSGSIZE);
	CHECK_EQ(packet.input.size, SSH_PACKET_INPUT_MAX);
	ssh_packet_free(&packet);

	/* The largest payload is not refused by the limits of the buffers: one frame fits the input. */
	CHECK(SSH_PACKET_INPUT_MAX >= SSH_PACKET_LIMIT + 4U + SSH_CHACHAPOLY_TAG);
}

/* Feeds random bytes, and valid frames with a few random changes, to unkeyed and keyed layers and checks nothing breaks. */
static void
test_random_feed(void)
{
	struct ssh_packet packet;
	struct ssh_packet sender;
	uint8_t key[SSH_CHACHAPOLY_KEY];
	uint8_t chunk[96];
	uint8_t out[512];
	const uint8_t *pending;
	size_t pending_size;
	size_t size;
	size_t length;
	size_t index;
	unsigned run;
	unsigned changes;
	unsigned mode;
	unsigned delivered;
	int error;
	int ok;

	make_key(key, 71);
	delivered = 0;
	ok = 1;
	for (run = 0; run < TEST_RANDOM_RUNS; run++) {
		mode = run % 4U;
		ssh_packet_init(&packet);
		if (mode >= 2U)
			ssh_packet_set_receive_key(&packet, key);

		/* Builds the bytes: random, or a valid frame with some bytes changed. */
		length = 1U + next_random() % sizeof(chunk);
		if (mode == 0U || mode == 2U) {
			for (index = 0; index < length; index++)
				chunk[index] = (uint8_t)next_random();

			/* Half of the unkeyed runs start with a plausible packet_length. */
			if (mode == 0U && (run & 4U) != 0 && length >= 8U) {
				chunk[0] = 0;
				chunk[1] = 0;
				chunk[2] = 0;
				chunk[3] = (uint8_t)(5U + next_random() % 60U);
			}
		} else {
			ssh_packet_init(&sender);
			if (mode == 3U)
				ssh_packet_set_send_key(&sender, key);
			memset(chunk, 0x5b, 60);
			error = ssh_packet_send(&sender, chunk, 1U + next_random() % 60U);
			if (error != 0)
				ok = 0;
			pending = ssh_packet_pending(&sender, &pending_size);
			if (pending == NULL || pending_size > sizeof(chunk)) {
				ok = 0;
				ssh_packet_free(&sender);
				ssh_packet_free(&packet);
				continue;
			}
			length = pending_size;
			memcpy(chunk, pending, length);
			ssh_packet_free(&sender);
			changes = next_random() % 4U;
			while (changes > 0) {
				chunk[next_random() % length] ^= (uint8_t)(1U << (next_random() % 8U));
				changes--;
			}
		}

		/* Feeds in pieces and takes what comes; every result is one of the documented ones. */
		for (index = 0; index < length;) {
			size_t piece;

			piece = 1U + next_random() % 17U;
			if (piece > length - index)
				piece = length - index;
			error = ssh_packet_feed(&packet, chunk + index, piece);
			if (error != 0)
				ok = 0;
			index += piece;
			error = take_payload(&packet, out, &size);
			if (error == 0) {
				delivered++;
				continue;
			}
			if (error != EAGAIN && error != EMSGSIZE && error != EINVAL && error != EBADMSG)
				ok = 0;
			if (error != EAGAIN)
				break;
		}
		ssh_packet_free(&packet);
	}
	CHECK(ok);

	/* The valid frames with no change were delivered at least sometimes (unchanged runs exist). */
	CHECK(delivered > 0);
}
