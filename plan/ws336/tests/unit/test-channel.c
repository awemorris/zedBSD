/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The test of ssh-channel.c.  Every message the module reads is built with
 * wire.h, written apart from the module, and every message it writes is
 * compared with bytes wire.h builds from RFC 4254 (sections 4, 5, 6 and
 * 9) and OpenSSH's PROTOCOL (sections 2.1 and 2.2).  The window sums are
 * worked out by hand in the comments beside the checks.  A run of random
 * payloads through every reader runs under the sanitizers.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "wire.h"
#include "ssh-channel.h"

/* The message wire.h builds as the module's input; static because it is large. */
static struct wire input;

/* The message wire.h builds as the bytes the module must have written. */
static struct wire expected;

/* The state of the pseudo-random generator of the random run, never zero. */
static uint32_t random_state = 2463534242UL;

/* A pointer value the tests store in a channel's user field and look for again. */
static int user_marker;

static void test_init(void);
static void test_open_session_and_reply(void);
static void test_accept_open(void);
static void test_accept_open_refusals(void);
static void test_data_window(void);
static void test_data_refusals(void);
static void test_consumed(void);
static void test_window_adjust_and_send(void);
static void test_eof_close_free(void);
static void test_requests_parse(void);
static void test_requests_limits(void);
static void test_requests_framing(void);
static void test_requests_put_roundtrip(void);
static void test_reply_messages(void);
static void test_closing_reads(void);
static void test_global_requests(void);
static void test_random(void);
static void build_open(struct wire *message, const char *type, uint32_t sender, uint32_t window, uint32_t packet);
static void build_request(struct wire *message, uint32_t recipient, const char *name, unsigned want_reply);
static void build_data(struct wire *message, uint32_t recipient, const void *bytes, size_t size);
static void build_end(struct wire *message, unsigned number, uint32_t recipient);
static struct ssh_channel *open_server_channel(struct ssh_channels *channels, struct ssh_buf *out, uint32_t sender, uint32_t window, uint32_t packet);
static void check_output(struct ssh_buf *out, const struct wire *want, int line);
static uint32_t next_random(void);

/* Checks that the output holds exactly the bytes in want, then empties it. */
#define CHECK_OUTPUT(out, want) check_output((out), (want), __LINE__)

/*
 * Runs the checks of the module.
 */
int
main(void)
{
	/* Runs each group of checks. */
	test_init();
	test_open_session_and_reply();
	test_accept_open();
	test_accept_open_refusals();
	test_data_window();
	test_data_refusals();
	test_consumed();
	test_window_adjust_and_send();
	test_eof_close_free();
	test_requests_parse();
	test_requests_limits();
	test_requests_framing();
	test_requests_put_roundtrip();
	test_reply_messages();
	test_closing_reads();
	test_global_requests();
	test_random();

	/* Reports the result of the whole test. */
	return check_finish("channel");
}

/*
 * Builds a CHANNEL_OPEN (RFC 4254 5.1): the number, the type, the sender's
 * number, the window and the largest packet.
 */
static void
build_open(
	struct wire *message,
	const char *type,
	uint32_t sender,
	uint32_t window,
	uint32_t packet)
{
	wire_reset(message);
	wire_u8(message, 90);
	wire_text(message, type);
	wire_u32(message, sender);
	wire_u32(message, window);
	wire_u32(message, packet);
}

/*
 * Builds the start of a CHANNEL_REQUEST (RFC 4254 5.4).
 */
static void
build_request(
	struct wire *message,
	uint32_t recipient,
	const char *name,
	unsigned want_reply)
{
	wire_reset(message);
	wire_u8(message, 98);
	wire_u32(message, recipient);
	wire_text(message, name);
	wire_bool(message, want_reply);
}

/*
 * Builds a CHANNEL_DATA (RFC 4254 5.2).
 */
static void
build_data(
	struct wire *message,
	uint32_t recipient,
	const void *bytes,
	size_t size)
{
	wire_reset(message);
	wire_u8(message, 94);
	wire_u32(message, recipient);
	wire_string(message, bytes, size);
}

/*
 * Builds a CHANNEL_EOF (96) or CHANNEL_CLOSE (97) (RFC 4254 5.3).
 */
static void
build_end(
	struct wire *message,
	unsigned number,
	uint32_t recipient)
{
	wire_reset(message);
	wire_u8(message, number);
	wire_u32(message, recipient);
}

/*
 * Accepts a session open from a peer with the given number, window and
 * packet size, checks that it was accepted, and empties the output.
 */
static struct ssh_channel *
open_server_channel(
	struct ssh_channels *channels,
	struct ssh_buf *out,
	uint32_t sender,
	uint32_t window,
	uint32_t packet)
{
	struct ssh_channel *channel;
	int error;

	build_open(&input, "session", sender, window, packet);
	channel = NULL;
	error = ssh_channel_accept_open(channels, input.data, input.size, out, &channel);
	CHECK_EQ(error, 0);
	CHECK(channel != NULL);
	ssh_buf_reset(out);
	return channel;
}

/*
 * Compares the output with the bytes in want, then empties the output.
 */
static void
check_output(
	struct ssh_buf *out,
	const struct wire *want,
	int line)
{
	if (out->size != want->size) {
		check_count++;
		check_failed++;
		printf("FAIL %s:%d: output is %lu bytes, expected %lu\n", __FILE__, line, (unsigned long)out->size, (unsigned long)want->size);
	} else {
		check_record_bytes(out->data, want->data, want->size, __FILE__, line, "output bytes");
	}
	CHECK_EQ(ssh_buf_error(out), 0);

	/* Empties the output for the next comparison. */
	ssh_buf_reset(out);
}

/*
 * A new table has every slot free and numbered by its index.
 */
static void
test_init(void)
{
	struct ssh_channels channels;
	unsigned index;

	/* Fills the table with garbage first, so the init is seen to do its work. */
	memset(&channels, 0xa5, sizeof(channels));
	ssh_channels_init(&channels);
	CHECK_EQ(channels.no_more_sessions, 0);
	for (index = 0; index < SSH_CHANNEL_MAX; index++) {
		CHECK_EQ(channels.channel[index].state, SSH_CHANNEL_FREE);
		CHECK_EQ(channels.channel[index].local_id, index);
		CHECK(channels.channel[index].user == NULL);
		CHECK_EQ(channels.channel[index].eof_sent, 0);
		CHECK_EQ(channels.channel[index].close_received, 0);
		CHECK_EQ(ssh_channel_send_room(&channels.channel[index]), 0);
		CHECK(ssh_channel_find(&channels, index) == NULL);
	}
	CHECK(ssh_channel_find(&channels, SSH_CHANNEL_MAX) == NULL);
	CHECK(ssh_channel_find(&channels, 0xffffffffUL) == NULL);
}

/*
 * The client side: CHANNEL_OPEN "session" out, confirmation or failure in,
 * and the table filling up.
 */
static void
test_open_session_and_reply(void)
{
	struct ssh_channels channels;
	struct ssh_channel *channel;
	struct ssh_channel *second;
	struct ssh_buf out;
	uint32_t reason;
	unsigned index;

	ssh_channels_init(&channels);
	ssh_buf_init(&out, 1U << 20, 0);

	/* RFC 4254 6.1: byte 90, "session", sender 0, window 2 MiB (2097152), packet 32768. */
	channel = NULL;
	CHECK_EQ(ssh_channel_open_session(&channels, &out, &channel), 0);
	CHECK(channel == &channels.channel[0]);
	wire_reset(&expected);
	wire_u8(&expected, 90);
	wire_text(&expected, "session");
	wire_u32(&expected, 0);
	wire_u32(&expected, 2097152UL);
	wire_u32(&expected, 32768UL);
	CHECK_OUTPUT(&out, &expected);
	CHECK_EQ(channel->state, SSH_CHANNEL_OPENING);
	CHECK_EQ(channel->local_id, 0);
	CHECK_EQ(channel->local_window, 2097152UL);

	/* An opening channel is not found, takes no data and has no send room. */
	CHECK(ssh_channel_find(&channels, 0) == NULL);
	CHECK_EQ(ssh_channel_send_room(channel), 0);

	/* A second open takes the next slot. */
	second = NULL;
	CHECK_EQ(ssh_channel_open_session(&channels, &out, &second), 0);
	CHECK(second == &channels.channel[1]);
	ssh_buf_reset(&out);

	/* RFC 4254 5.1: OPEN_CONFIRMATION (91), recipient 0, sender 7, window 65536, packet 16384. */
	wire_reset(&input);
	wire_u8(&input, 91);
	wire_u32(&input, 0);
	wire_u32(&input, 7);
	wire_u32(&input, 65536);
	wire_u32(&input, 16384);
	channel = NULL;
	reason = 99;
	CHECK_EQ(ssh_channel_open_reply(&channels, input.data, input.size, &channel, &reason), 0);
	CHECK(channel == &channels.channel[0]);
	CHECK_EQ(channel->state, SSH_CHANNEL_OPEN);
	CHECK_EQ(channel->remote_id, 7);
	CHECK_EQ(channel->remote_window, 65536);
	CHECK_EQ(channel->remote_packet, 16384);
	CHECK(ssh_channel_find(&channels, 0) == channel);

	/* The send room is the smaller of the window and the packet size: 16384, then 100 once the window is 100. */
	CHECK_EQ(ssh_channel_send_room(channel), 16384);
	channel->remote_window = 100;
	CHECK_EQ(ssh_channel_send_room(channel), 100);
	channel->remote_window = 16384;
	CHECK_EQ(ssh_channel_send_room(channel), 16384);

	/* A second confirmation for the same, now open, channel is a protocol fault. */
	channel = NULL;
	CHECK_EQ(ssh_channel_open_reply(&channels, input.data, input.size, &channel, &reason), EPROTO);

	/* A confirmation with a packet size of 0 cannot be used: EPROTO, and the channel stays waiting. */
	wire_reset(&input);
	wire_u8(&input, 91);
	wire_u32(&input, 1);
	wire_u32(&input, 8);
	wire_u32(&input, 65536);
	wire_u32(&input, 0);
	CHECK_EQ(ssh_channel_open_reply(&channels, input.data, input.size, &channel, &reason), EPROTO);
	CHECK_EQ(channels.channel[1].state, SSH_CHANNEL_OPENING);

	/* A peer packet size above ours is capped at the packet size these programs send (32768). */
	wire_reset(&input);
	wire_u8(&input, 91);
	wire_u32(&input, 1);
	wire_u32(&input, 8);
	wire_u32(&input, 0xffffffffUL);
	wire_u32(&input, 0xffffffffUL);
	CHECK_EQ(ssh_channel_open_reply(&channels, input.data, input.size, &channel, &reason), 0);
	CHECK_EQ(channel->remote_window, 0xffffffffUL);
	CHECK_EQ(channel->remote_packet, 32768);
	CHECK_EQ(ssh_channel_send_room(channel), 32768);

	/* Recipients that are out of the table or not waiting are EPROTO. */
	wire_reset(&input);
	wire_u8(&input, 91);
	wire_u32(&input, 5);
	wire_u32(&input, 8);
	wire_u32(&input, 1);
	wire_u32(&input, 1);
	CHECK_EQ(ssh_channel_open_reply(&channels, input.data, input.size, &channel, &reason), EPROTO);
	wire_reset(&input);
	wire_u8(&input, 91);
	wire_u32(&input, 0xffffffffUL);
	wire_u32(&input, 8);
	wire_u32(&input, 1);
	wire_u32(&input, 1);
	CHECK_EQ(ssh_channel_open_reply(&channels, input.data, input.size, &channel, &reason), EPROTO);

	/* RFC 4254 5.1: OPEN_FAILURE (92), recipient, reason 2 (CONNECT_FAILED), description, language tag. */
	CHECK_EQ(ssh_channel_open_session(&channels, &out, &channel), 0);
	CHECK(channel == &channels.channel[2]);
	ssh_buf_reset(&out);
	/* A number past the table must not wrap onto a slot that is waiting: 12 is not slot 2. */
	wire_reset(&input);
	wire_u8(&input, 91);
	wire_u32(&input, SSH_CHANNEL_MAX + 2);
	wire_u32(&input, 8);
	wire_u32(&input, 1);
	wire_u32(&input, 1);
	CHECK_EQ(ssh_channel_open_reply(&channels, input.data, input.size, &channel, &reason), EPROTO);
	CHECK_EQ(channels.channel[2].state, SSH_CHANNEL_OPENING);

	wire_reset(&input);
	wire_u8(&input, 92);
	wire_u32(&input, 2);
	wire_u32(&input, SSH_OPEN_CONNECT_FAILED);
	wire_text(&input, "no");
	wire_text(&input, "");
	channel = NULL;
	reason = 0;
	CHECK_EQ(ssh_channel_open_reply(&channels, input.data, input.size, &channel, &reason), EACCES);
	CHECK(channel == &channels.channel[2]);
	CHECK_EQ(reason, 2);
	CHECK_EQ(channels.channel[2].state, SSH_CHANNEL_FREE);

	/* A failure for a channel that is not waiting is EPROTO (slot 2 is free again). */
	CHECK_EQ(ssh_channel_open_reply(&channels, input.data, input.size, &channel, &reason), EPROTO);

	/* Truncated answers are EINVAL: every cut of a confirmation and of a failure. */
	CHECK_EQ(ssh_channel_open_session(&channels, &out, &channel), 0);
	ssh_buf_reset(&out);
	for (index = 0; index < 17; index++) {
		wire_reset(&input);
		wire_u8(&input, 91);
		wire_u32(&input, 2);
		wire_u32(&input, 8);
		wire_u32(&input, 1);
		wire_u32(&input, 1);
		CHECK_EQ(ssh_channel_open_reply(&channels, input.data, index, &channel, &reason), EINVAL);
	}
	wire_reset(&input);
	wire_u8(&input, 92);
	wire_u32(&input, 2);
	wire_u32(&input, 2);
	wire_text(&input, "no");
	wire_text(&input, "");
	for (index = 0; index < input.size; index++)
		CHECK_EQ(ssh_channel_open_reply(&channels, input.data, index, &channel, &reason), EINVAL);
	CHECK_EQ(channels.channel[2].state, SSH_CHANNEL_OPENING);

	/* Another message number is EINVAL. */
	wire_reset(&input);
	wire_u8(&input, 94);
	wire_u32(&input, 2);
	wire_u32(&input, 8);
	wire_u32(&input, 1);
	wire_u32(&input, 1);
	CHECK_EQ(ssh_channel_open_reply(&channels, input.data, input.size, &channel, &reason), EINVAL);

	/* The table holds SSH_CHANNEL_MAX (10) channels: slots 3 to 9 are free, then no more. */
	for (index = 3; index < SSH_CHANNEL_MAX; index++)
		CHECK_EQ(ssh_channel_open_session(&channels, &out, &channel), 0);
	ssh_buf_reset(&out);
	channel = &channels.channel[0];
	CHECK_EQ(ssh_channel_open_session(&channels, &out, &channel), EAGAIN);
	CHECK(channel == NULL);
	CHECK_EQ(out.size, 0);

	/* A failed output gives the slot back and reports the error. */
	ssh_channels_init(&channels);
	ssh_buf_free(&out);
	ssh_buf_init(&out, 8, 0);
	CHECK_EQ(ssh_channel_open_session(&channels, &out, &channel), EMSGSIZE);
	CHECK(channel == NULL);
	CHECK_EQ(channels.channel[0].state, SSH_CHANNEL_FREE);
	CHECK_EQ(ssh_channel_open_session(&channels, &out, &channel), EMSGSIZE);
	CHECK_EQ(channels.channel[0].state, SSH_CHANNEL_FREE);

	ssh_buf_free(&out);
}

/*
 * The server side: a session open is confirmed, with the peer's number,
 * window and packet size kept.
 */
static void
test_accept_open(void)
{
	struct ssh_channels channels;
	struct ssh_channel *channel;
	struct ssh_channel *next;
	struct ssh_buf out;

	ssh_channels_init(&channels);
	ssh_buf_init(&out, 1U << 20, 0);

	/* RFC 4254 5.1: OPEN_CONFIRMATION (91), recipient 5 (the sender's number), our number 0, our window and packet size. */
	build_open(&input, "session", 5, 1000, 2000);
	channel = NULL;
	CHECK_EQ(ssh_channel_accept_open(&channels, input.data, input.size, &out, &channel), 0);
	CHECK(channel == &channels.channel[0]);
	wire_reset(&expected);
	wire_u8(&expected, 91);
	wire_u32(&expected, 5);
	wire_u32(&expected, 0);
	wire_u32(&expected, 2097152UL);
	wire_u32(&expected, 32768UL);
	CHECK_OUTPUT(&out, &expected);
	CHECK_EQ(channel->state, SSH_CHANNEL_OPEN);
	CHECK_EQ(channel->local_id, 0);
	CHECK_EQ(channel->remote_id, 5);
	CHECK_EQ(channel->local_window, 2097152UL);
	CHECK_EQ(channel->remote_window, 1000);
	CHECK_EQ(channel->remote_packet, 2000);
	CHECK_EQ(channel->eof_sent, 0);
	CHECK_EQ(channel->close_received, 0);
	CHECK(ssh_channel_find(&channels, 0) == channel);

	/* The second takes slot 1; a stale user pointer in a free slot is cleared on reuse. */
	channels.channel[1].user = &user_marker;
	build_open(&input, "session", 6, 0, 1);
	next = NULL;
	CHECK_EQ(ssh_channel_accept_open(&channels, input.data, input.size, &out, &next), 0);
	CHECK(next == &channels.channel[1]);
	CHECK(next->user == NULL);
	CHECK_EQ(next->remote_window, 0);
	CHECK_EQ(next->remote_packet, 1);
	ssh_buf_reset(&out);

	/* A packet size above what these programs send is capped at 32768; the window is kept whole. */
	build_open(&input, "session", 7, 0xffffffffUL, 0xffffffffUL);
	CHECK_EQ(ssh_channel_accept_open(&channels, input.data, input.size, &out, &next), 0);
	CHECK_EQ(next->remote_window, 0xffffffffUL);
	CHECK_EQ(next->remote_packet, 32768);
	ssh_buf_reset(&out);

	/* Data of the type after the three numbers is not read (a session has none, a lenient open still opens). */
	build_open(&input, "session", 8, 10, 10);
	wire_u32(&input, 0x01020304UL);
	CHECK_EQ(ssh_channel_accept_open(&channels, input.data, input.size, &out, &next), 0);
	CHECK(next != NULL);
	ssh_buf_reset(&out);

	ssh_buf_free(&out);
}

/*
 * The refusals: other types, a full table, no-more-sessions, a packet size
 * of 0, and malformed opens.
 */
static void
test_accept_open_refusals(void)
{
	struct ssh_channels channels;
	struct ssh_channel *channel;
	struct ssh_buf out;
	const char *types[5];
	unsigned index;

	ssh_channels_init(&channels);
	ssh_buf_init(&out, 1U << 20, 0);

	/* RFC 4254 5.1: OPEN_FAILURE (92), the sender's number, reason 1 (ADMINISTRATIVELY_PROHIBITED), a text, an empty tag. */
	types[0] = "direct-tcpip";
	types[1] = "x11";
	types[2] = "auth-agent@openssh.com";
	types[3] = "forwarded-tcpip";
	types[4] = "nonsense-type";
	for (index = 0; index < 5; index++) {
		build_open(&input, types[index], 20 + index, 100, 100);
		wire_text(&input, "127.0.0.1");
		wire_u32(&input, 22);
		channel = &channels.channel[0];
		CHECK_EQ(ssh_channel_accept_open(&channels, input.data, input.size, &out, &channel), 0);
		CHECK(channel == NULL);
		wire_reset(&expected);
		wire_u8(&expected, 92);
		wire_u32(&expected, 20 + index);
		wire_u32(&expected, 1);
		wire_text(&expected, "administratively prohibited");
		wire_text(&expected, "");
		CHECK_OUTPUT(&out, &expected);
		CHECK_EQ(channels.channel[0].state, SSH_CHANNEL_FREE);
	}

	/* Types that only look like "session": longer, shorter, with a NUL, upper case, empty. */
	types[0] = "session2";
	types[1] = "sessio";
	types[2] = "Session";
	types[3] = "";
	for (index = 0; index < 4; index++) {
		build_open(&input, types[index], 30, 100, 100);
		CHECK_EQ(ssh_channel_accept_open(&channels, input.data, input.size, &out, &channel), 0);
		CHECK(channel == NULL);
		CHECK_EQ(out.data[0], 92);
		CHECK_EQ(out.data[8], 1);
		ssh_buf_reset(&out);
	}
	wire_reset(&input);
	wire_u8(&input, 90);
	wire_string(&input, "session\0", 8);
	wire_u32(&input, 31);
	wire_u32(&input, 100);
	wire_u32(&input, 100);
	CHECK_EQ(ssh_channel_accept_open(&channels, input.data, input.size, &out, &channel), 0);
	CHECK(channel == NULL);
	CHECK_EQ(out.data[0], 92);
	ssh_buf_reset(&out);

	/* A session packet size of 0 is EPROTO and nothing is written. */
	build_open(&input, "session", 40, 100, 0);
	CHECK_EQ(ssh_channel_accept_open(&channels, input.data, input.size, &out, &channel), EPROTO);
	CHECK(channel == NULL);
	CHECK_EQ(out.size, 0);
	CHECK_EQ(channels.channel[0].state, SSH_CHANNEL_FREE);

	/* Ten sessions open; the eleventh gets reason 4 (RESOURCE_SHORTAGE). */
	for (index = 0; index < SSH_CHANNEL_MAX; index++)
		(void)open_server_channel(&channels, &out, 100 + index, 10, 10);
	build_open(&input, "session", 99, 10, 10);
	CHECK_EQ(ssh_channel_accept_open(&channels, input.data, input.size, &out, &channel), 0);
	CHECK(channel == NULL);
	wire_reset(&expected);
	wire_u8(&expected, 92);
	wire_u32(&expected, 99);
	wire_u32(&expected, 4);
	wire_text(&expected, "resource shortage");
	wire_text(&expected, "");
	CHECK_OUTPUT(&out, &expected);

	/* A freed slot is taken again by the next open. */
	channels.channel[4].state = SSH_CHANNEL_CLOSING;
	channels.channel[4].close_sent = 1;
	channels.channel[4].close_received = 1;
	CHECK_EQ(ssh_channel_closed(&channels, &channels.channel[4]), 1);
	channel = open_server_channel(&channels, &out, 98, 10, 10);
	CHECK(channel == &channels.channel[4]);
	CHECK_EQ(channel->remote_id, 98);

	/* After no-more-sessions a session is refused with reason 1 even with free slots. */
	ssh_channels_init(&channels);
	channels.no_more_sessions = 1;
	build_open(&input, "session", 50, 10, 10);
	CHECK_EQ(ssh_channel_accept_open(&channels, input.data, input.size, &out, &channel), 0);
	CHECK(channel == NULL);
	wire_reset(&expected);
	wire_u8(&expected, 92);
	wire_u32(&expected, 50);
	wire_u32(&expected, 1);
	wire_text(&expected, "administratively prohibited");
	wire_text(&expected, "");
	CHECK_OUTPUT(&out, &expected);
	CHECK_EQ(channels.channel[0].state, SSH_CHANNEL_FREE);

	/* Malformed opens are EINVAL at every cut, also for a type that is refused. */
	ssh_channels_init(&channels);
	build_open(&input, "session", 60, 10, 10);
	for (index = 0; index < input.size; index++) {
		CHECK_EQ(ssh_channel_accept_open(&channels, input.data, index, &out, &channel), EINVAL);
		CHECK(channel == NULL);
	}
	build_open(&input, "direct-tcpip", 60, 10, 10);
	for (index = 0; index < input.size; index++)
		CHECK_EQ(ssh_channel_accept_open(&channels, input.data, index, &out, &channel), EINVAL);
	CHECK_EQ(out.size, 0);
	CHECK_EQ(channels.channel[0].state, SSH_CHANNEL_FREE);

	/* Another message number is EINVAL. */
	build_open(&input, "session", 60, 10, 10);
	input.data[0] = 91;
	CHECK_EQ(ssh_channel_accept_open(&channels, input.data, input.size, &out, &channel), EINVAL);

	/* A failed output reports the error and gives the slot back. */
	ssh_buf_free(&out);
	ssh_buf_init(&out, 8, 0);
	build_open(&input, "session", 70, 10, 10);
	CHECK_EQ(ssh_channel_accept_open(&channels, input.data, input.size, &out, &channel), EMSGSIZE);
	CHECK(channel == NULL);
	CHECK_EQ(channels.channel[0].state, SSH_CHANNEL_FREE);

	ssh_buf_free(&out);
}

/*
 * Received data is charged to the local window, ordinary and extended.
 */
static void
test_data_window(void)
{
	struct ssh_channels channels;
	struct ssh_channel *channel;
	struct ssh_channel *found;
	struct ssh_buf out;
	const uint8_t *bytes;
	size_t bytes_size;
	uint32_t extended;
	static uint8_t big[SSH_CHANNEL_PACKET + 1];

	ssh_channels_init(&channels);
	ssh_buf_init(&out, 1U << 20, 0);
	channel = open_server_channel(&channels, &out, 5, 1000, 2000);
	memset(big, 'x', sizeof(big));

	/* RFC 4254 5.2: CHANNEL_DATA (94) for our number 0 with "hello": the window goes from 2097152 to 2097147. */
	build_data(&input, 0, "hello", 5);
	found = NULL;
	bytes = NULL;
	bytes_size = 99;
	extended = 99;
	CHECK_EQ(ssh_channel_parse_data(&channels, input.data, input.size, &found, &bytes, &bytes_size, &extended), 0);
	CHECK(found == channel);
	CHECK_EQ(bytes_size, 5);
	CHECK_EQ(extended, 0);
	CHECK(bytes == input.data + 9);
	CHECK_BYTES(bytes, "hello", 5);
	CHECK_EQ(channel->local_window, 2097147UL);

	/* CHANNEL_EXTENDED_DATA (95), type 1 (stderr), "oops": 4 more bytes, 2097143. */
	wire_reset(&input);
	wire_u8(&input, 95);
	wire_u32(&input, 0);
	wire_u32(&input, 1);
	wire_string(&input, "oops", 4);
	CHECK_EQ(ssh_channel_parse_data(&channels, input.data, input.size, &found, &bytes, &bytes_size, &extended), 0);
	CHECK_EQ(extended, 1);
	CHECK_EQ(bytes_size, 4);
	CHECK(bytes == input.data + 13);
	CHECK_BYTES(bytes, "oops", 4);
	CHECK_EQ(channel->local_window, 2097143UL);

	/* Empty data is allowed and costs nothing. */
	build_data(&input, 0, "", 0);
	CHECK_EQ(ssh_channel_parse_data(&channels, input.data, input.size, &found, &bytes, &bytes_size, &extended), 0);
	CHECK_EQ(bytes_size, 0);
	CHECK_EQ(channel->local_window, 2097143UL);

	/* A packet of exactly 32768 bytes is fine (window 2097143 - 32768 = 2064375); 32769 is EPROTO and changes nothing. */
	build_data(&input, 0, big, 32768);
	CHECK_EQ(ssh_channel_parse_data(&channels, input.data, input.size, &found, &bytes, &bytes_size, &extended), 0);
	CHECK_EQ(bytes_size, 32768);
	CHECK_EQ(channel->local_window, 2064375UL);
	build_data(&input, 0, big, 32769);
	CHECK_EQ(ssh_channel_parse_data(&channels, input.data, input.size, &found, &bytes, &bytes_size, &extended), EPROTO);
	CHECK(found == NULL);
	CHECK(bytes == NULL);
	CHECK_EQ(bytes_size, 0);
	CHECK_EQ(channel->local_window, 2064375UL);

	/* The window edge: with 10 bytes left, 10 bytes go to 0, one more is EPROTO, empty data at 0 is fine. */
	channel->local_window = 10;
	build_data(&input, 0, big, 11);
	CHECK_EQ(ssh_channel_parse_data(&channels, input.data, input.size, &found, &bytes, &bytes_size, &extended), EPROTO);
	CHECK_EQ(channel->local_window, 10);
	build_data(&input, 0, big, 10);
	CHECK_EQ(ssh_channel_parse_data(&channels, input.data, input.size, &found, &bytes, &bytes_size, &extended), 0);
	CHECK_EQ(channel->local_window, 0);
	build_data(&input, 0, big, 1);
	CHECK_EQ(ssh_channel_parse_data(&channels, input.data, input.size, &found, &bytes, &bytes_size, &extended), EPROTO);
	CHECK_EQ(channel->local_window, 0);
	build_data(&input, 0, "", 0);
	CHECK_EQ(ssh_channel_parse_data(&channels, input.data, input.size, &found, &bytes, &bytes_size, &extended), 0);

	/* Extended data uses the same window. */
	channel->local_window = 3;
	wire_reset(&input);
	wire_u8(&input, 95);
	wire_u32(&input, 0);
	wire_u32(&input, 1);
	wire_string(&input, "abcd", 4);
	CHECK_EQ(ssh_channel_parse_data(&channels, input.data, input.size, &found, &bytes, &bytes_size, &extended), EPROTO);
	CHECK_EQ(channel->local_window, 3);

	ssh_buf_free(&out);
}

/*
 * Data the module must refuse: unknown channels, after EOF or CLOSE, an
 * extended type of 0, malformed framing.
 */
static void
test_data_refusals(void)
{
	struct ssh_channels channels;
	struct ssh_channel *channel;
	struct ssh_channel *found;
	struct ssh_buf out;
	const uint8_t *bytes;
	size_t bytes_size;
	uint32_t extended;
	size_t index;

	ssh_channels_init(&channels);
	ssh_buf_init(&out, 1U << 20, 0);
	channel = open_server_channel(&channels, &out, 5, 1000, 2000);

	/* Unknown numbers: past the table, the maximum value, a free slot, and a channel still waiting for the answer. */
	build_data(&input, 1, "x", 1);
	CHECK_EQ(ssh_channel_parse_data(&channels, input.data, input.size, &found, &bytes, &bytes_size, &extended), EPROTO);
	build_data(&input, 10, "x", 1);
	CHECK_EQ(ssh_channel_parse_data(&channels, input.data, input.size, &found, &bytes, &bytes_size, &extended), EPROTO);
	build_data(&input, 0xffffffffUL, "x", 1);
	CHECK_EQ(ssh_channel_parse_data(&channels, input.data, input.size, &found, &bytes, &bytes_size, &extended), EPROTO);
	channels.channel[1].state = SSH_CHANNEL_OPENING;
	build_data(&input, 1, "x", 1);
	CHECK_EQ(ssh_channel_parse_data(&channels, input.data, input.size, &found, &bytes, &bytes_size, &extended), EPROTO);
	CHECK_EQ(channels.channel[1].local_window, 0);
	channels.channel[1].state = SSH_CHANNEL_FREE;

	/* Extended type 0 is refused. */
	wire_reset(&input);
	wire_u8(&input, 95);
	wire_u32(&input, 0);
	wire_u32(&input, 0);
	wire_string(&input, "x", 1);
	CHECK_EQ(ssh_channel_parse_data(&channels, input.data, input.size, &found, &bytes, &bytes_size, &extended), EPROTO);
	CHECK_EQ(channel->local_window, 2097152UL);

	/* Malformed framing is EINVAL at every cut, with the window untouched. */
	build_data(&input, 0, "abc", 3);
	for (index = 0; index < input.size; index++)
		CHECK_EQ(ssh_channel_parse_data(&channels, input.data, index, &found, &bytes, &bytes_size, &extended), EINVAL);
	wire_u8(&input, 0);
	CHECK_EQ(ssh_channel_parse_data(&channels, input.data, input.size, &found, &bytes, &bytes_size, &extended), EINVAL);
	build_data(&input, 0, "abc", 3);
	input.data[0] = 93;
	CHECK_EQ(ssh_channel_parse_data(&channels, input.data, input.size, &found, &bytes, &bytes_size, &extended), EINVAL);
	wire_reset(&input);
	wire_u8(&input, 94);
	wire_u32(&input, 0);
	wire_u32(&input, 0xffffffffUL);
	wire_bytes(&input, "abc", 3);
	CHECK_EQ(ssh_channel_parse_data(&channels, input.data, input.size, &found, &bytes, &bytes_size, &extended), EINVAL);
	CHECK_EQ(channel->local_window, 2097152UL);

	/* After the peer's EOF, data is EPROTO. */
	build_end(&expected, 96, 0);
	CHECK_EQ(ssh_channel_parse_end(&channels, expected.data, expected.size, &found), 0);
	build_data(&input, 0, "x", 1);
	CHECK_EQ(ssh_channel_parse_data(&channels, input.data, input.size, &found, &bytes, &bytes_size, &extended), EPROTO);
	CHECK_EQ(channel->local_window, 2097152UL);

	/* After the peer's CLOSE (without an EOF before it), data is EPROTO too. */
	ssh_channels_init(&channels);
	channel = open_server_channel(&channels, &out, 5, 1000, 2000);
	build_end(&expected, 97, 0);
	CHECK_EQ(ssh_channel_parse_end(&channels, expected.data, expected.size, &found), 0);
	CHECK_EQ(channel->state, SSH_CHANNEL_CLOSING);
	build_data(&input, 0, "x", 1);
	CHECK_EQ(ssh_channel_parse_data(&channels, input.data, input.size, &found, &bytes, &bytes_size, &extended), EPROTO);
	CHECK_EQ(channel->local_window, 2097152UL);

	ssh_buf_free(&out);
}

/*
 * Consumed bytes give the window back when it is below half (1048576).
 */
static void
test_consumed(void)
{
	struct ssh_channels channels;
	struct ssh_channel *channel;
	struct ssh_buf out;

	ssh_channels_init(&channels);
	ssh_buf_init(&out, 1U << 20, 0);
	channel = open_server_channel(&channels, &out, 9, 1000, 2000);

	/* A full window needs nothing. */
	ssh_channel_consumed(channel, 100, &out);
	CHECK_EQ(out.size, 0);
	CHECK_EQ(channel->local_window, 2097152UL);

	/* Exactly half (1048576) is not "below half": nothing. */
	channel->local_window = 1048576UL;
	ssh_channel_consumed(channel, 100, &out);
	CHECK_EQ(out.size, 0);
	CHECK_EQ(channel->local_window, 1048576UL);

	/* One byte below half: WINDOW_ADJUST (93) to peer number 9 with 2097152 - 1048575 = 1048577, window back to full. */
	channel->local_window = 1048575UL;
	ssh_channel_consumed(channel, 1, &out);
	wire_reset(&expected);
	wire_u8(&expected, 93);
	wire_u32(&expected, 9);
	wire_u32(&expected, 1048577UL);
	CHECK_OUTPUT(&out, &expected);
	CHECK_EQ(channel->local_window, 2097152UL);

	/* Nothing consumed: no adjustment even when the window is low. */
	channel->local_window = 5;
	ssh_channel_consumed(channel, 0, &out);
	CHECK_EQ(out.size, 0);
	CHECK_EQ(channel->local_window, 5);

	/* An empty window: the adjustment is the whole 2097152. */
	channel->local_window = 0;
	ssh_channel_consumed(channel, 32768, &out);
	wire_reset(&expected);
	wire_u8(&expected, 93);
	wire_u32(&expected, 9);
	wire_u32(&expected, 2097152UL);
	CHECK_OUTPUT(&out, &expected);
	CHECK_EQ(channel->local_window, 2097152UL);

	/* A closing channel needs no more room. */
	channel->local_window = 0;
	channel->state = SSH_CHANNEL_CLOSING;
	ssh_channel_consumed(channel, 100, &out);
	CHECK_EQ(out.size, 0);
	CHECK_EQ(channel->local_window, 0);

	/* A failed output leaves the window as it was. */
	channel->state = SSH_CHANNEL_OPEN;
	ssh_buf_free(&out);
	ssh_buf_init(&out, 4, 0);
	ssh_channel_consumed(channel, 100, &out);
	CHECK_EQ(channel->local_window, 0);

	ssh_buf_free(&out);
}

/*
 * The peer's window: adjustments add to it up to 2^32 - 1, and the data
 * this side sends takes from it.
 */
static void
test_window_adjust_and_send(void)
{
	struct ssh_channels channels;
	struct ssh_channel *channel;
	struct ssh_channel *found;
	struct ssh_buf out;
	size_t index;

	ssh_channels_init(&channels);
	ssh_buf_init(&out, 1U << 20, 0);
	channel = open_server_channel(&channels, &out, 9, 1000, 400);

	/* The send room is min(1000, 400) = 400. */
	CHECK_EQ(ssh_channel_send_room(channel), 400);

	/* Sends 3 bytes: CHANNEL_DATA (94) to the peer's number 9 with a string of 3; the window is 997. */
	CHECK_EQ(ssh_channel_put_data(channel, 0, (const uint8_t *)"abc", 3, &out), 0);
	wire_reset(&expected);
	wire_u8(&expected, 94);
	wire_u32(&expected, 9);
	wire_string(&expected, "abc", 3);
	CHECK_OUTPUT(&out, &expected);
	CHECK_EQ(channel->remote_window, 997);

	/* Extended data (95), type 1: another 2 bytes, window 995. */
	CHECK_EQ(ssh_channel_put_data(channel, SSH_EXTENDED_DATA_STDERR, (const uint8_t *)"er", 2, &out), 0);
	wire_reset(&expected);
	wire_u8(&expected, 95);
	wire_u32(&expected, 9);
	wire_u32(&expected, 1);
	wire_string(&expected, "er", 2);
	CHECK_OUTPUT(&out, &expected);
	CHECK_EQ(channel->remote_window, 995);

	/* More than the room is EINVAL and nothing is written: 401 > 400, and with the window at 100, 101 > 100. */
	{
		static uint8_t block[500];

		memset(block, 'z', sizeof(block));
		CHECK_EQ(ssh_channel_put_data(channel, 0, block, 401, &out), EINVAL);
		CHECK_EQ(out.size, 0);
		CHECK_EQ(channel->remote_window, 995);
		CHECK_EQ(ssh_channel_put_data(channel, 0, block, 400, &out), 0);
		CHECK_EQ(channel->remote_window, 595);
		ssh_buf_reset(&out);
		channel->remote_window = 100;
		CHECK_EQ(ssh_channel_put_data(channel, 0, block, 101, &out), EINVAL);
		CHECK_EQ(ssh_channel_put_data(channel, 0, block, 100, &out), 0);
		CHECK_EQ(channel->remote_window, 0);
		CHECK_EQ(ssh_channel_send_room(channel), 0);
		ssh_buf_reset(&out);
	}

	/* Missing bytes are EINVAL. */
	CHECK_EQ(ssh_channel_put_data(channel, 0, NULL, 1, &out), EINVAL);

	/* WINDOW_ADJUST (93) for our number 0 adds 500: the window is 500. */
	wire_reset(&input);
	wire_u8(&input, 93);
	wire_u32(&input, 0);
	wire_u32(&input, 500);
	found = NULL;
	CHECK_EQ(ssh_channel_parse_window_adjust(&channels, input.data, input.size, &found), 0);
	CHECK(found == channel);
	CHECK_EQ(channel->remote_window, 500);

	/* RFC 4254 5.2: the window must not pass 2^32 - 1.  Up to exactly that is fine, one more is EPROTO and changes nothing. */
	wire_reset(&input);
	wire_u8(&input, 93);
	wire_u32(&input, 0);
	wire_u32(&input, 0xffffffffUL - 500);
	CHECK_EQ(ssh_channel_parse_window_adjust(&channels, input.data, input.size, &found), 0);
	CHECK_EQ(channel->remote_window, 0xffffffffUL);
	wire_reset(&input);
	wire_u8(&input, 93);
	wire_u32(&input, 0);
	wire_u32(&input, 1);
	CHECK_EQ(ssh_channel_parse_window_adjust(&channels, input.data, input.size, &found), EPROTO);
	CHECK(found == NULL);
	CHECK_EQ(channel->remote_window, 0xffffffffUL);
	channel->remote_window = 1;
	wire_reset(&input);
	wire_u8(&input, 93);
	wire_u32(&input, 0);
	wire_u32(&input, 0xffffffffUL);
	CHECK_EQ(ssh_channel_parse_window_adjust(&channels, input.data, input.size, &found), EPROTO);
	CHECK_EQ(channel->remote_window, 1);
	channel->remote_window = 0;
	CHECK_EQ(ssh_channel_parse_window_adjust(&channels, input.data, input.size, &found), 0);
	CHECK_EQ(channel->remote_window, 0xffffffffUL);

	/* The send room with the huge window is the packet size. */
	CHECK_EQ(ssh_channel_send_room(channel), 400);

	/* An unknown recipient is EPROTO; malformed framing is EINVAL at every cut. */
	wire_reset(&input);
	wire_u8(&input, 93);
	wire_u32(&input, 4);
	wire_u32(&input, 1);
	CHECK_EQ(ssh_channel_parse_window_adjust(&channels, input.data, input.size, &found), EPROTO);
	wire_reset(&input);
	wire_u8(&input, 93);
	wire_u32(&input, 0);
	wire_u32(&input, 1);
	for (index = 0; index < input.size; index++)
		CHECK_EQ(ssh_channel_parse_window_adjust(&channels, input.data, index, &found), EINVAL);
	wire_u8(&input, 0);
	CHECK_EQ(ssh_channel_parse_window_adjust(&channels, input.data, input.size, &found), EINVAL);
	input.data[0] = 94;
	CHECK_EQ(ssh_channel_parse_window_adjust(&channels, input.data, input.size - 1, &found), EINVAL);

	/* After EOF sent, data is EPIPE; the room stays what it was. */
	channel->remote_window = 1000;
	ssh_channel_put_eof(channel, &out);
	ssh_buf_reset(&out);
	CHECK_EQ(ssh_channel_put_data(channel, 0, (const uint8_t *)"a", 1, &out), EPIPE);
	CHECK_EQ(out.size, 0);
	CHECK_EQ(channel->remote_window, 1000);

	/* After CLOSE sent, data is EPIPE as well. */
	ssh_channels_init(&channels);
	channel = open_server_channel(&channels, &out, 9, 1000, 400);
	ssh_channel_put_close(channel, &out);
	ssh_buf_reset(&out);
	CHECK_EQ(ssh_channel_put_data(channel, 0, (const uint8_t *)"a", 1, &out), EPIPE);
	CHECK_EQ(ssh_channel_put_data(channel, 0, NULL, 0, &out), EPIPE);
	CHECK_EQ(ssh_channel_send_room(channel), 0);

	/* A failed output leaves the window as it was. */
	ssh_channels_init(&channels);
	channel = open_server_channel(&channels, &out, 9, 1000, 400);
	ssh_buf_free(&out);
	ssh_buf_init(&out, 6, 0);
	CHECK_EQ(ssh_channel_put_data(channel, 0, (const uint8_t *)"abc", 3, &out), EMSGSIZE);
	CHECK_EQ(channel->remote_window, 1000);

	ssh_buf_free(&out);
}

/*
 * EOF, CLOSE, their repeats, and freeing the slot.
 */
static void
test_eof_close_free(void)
{
	struct ssh_channels channels;
	struct ssh_channel *channel;
	struct ssh_channel *found;
	struct ssh_buf out;
	size_t index;
	int marker;

	ssh_channels_init(&channels);
	ssh_buf_init(&out, 1U << 20, 0);
	channel = open_server_channel(&channels, &out, 9, 1000, 400);

	/* RFC 4254 5.3: the peer's EOF (96) sets eof_received; the channel stays open. */
	build_end(&input, 96, 0);
	found = NULL;
	CHECK_EQ(ssh_channel_parse_end(&channels, input.data, input.size, &found), 0);
	CHECK(found == channel);
	CHECK_EQ(channel->eof_received, 1);
	CHECK_EQ(channel->state, SSH_CHANNEL_OPEN);

	/* A second EOF is EPROTO. */
	CHECK_EQ(ssh_channel_parse_end(&channels, input.data, input.size, &found), EPROTO);
	CHECK(found == NULL);

	/* Our EOF (96) to the peer's number 9, once; a repeat writes nothing. */
	ssh_channel_put_eof(channel, &out);
	wire_reset(&expected);
	wire_u8(&expected, 96);
	wire_u32(&expected, 9);
	CHECK_OUTPUT(&out, &expected);
	CHECK_EQ(channel->eof_sent, 1);
	ssh_channel_put_eof(channel, &out);
	CHECK_EQ(out.size, 0);

	/* The peer's CLOSE (97): close_received, state CLOSING, the slot stays since we have not sent ours. */
	build_end(&input, 97, 0);
	CHECK_EQ(ssh_channel_parse_end(&channels, input.data, input.size, &found), 0);
	CHECK_EQ(channel->close_received, 1);
	CHECK_EQ(channel->state, SSH_CHANNEL_CLOSING);
	CHECK_EQ(ssh_channel_closed(&channels, channel), 0);
	CHECK_EQ(channel->state, SSH_CHANNEL_CLOSING);

	/* A second CLOSE and an EOF after CLOSE are EPROTO. */
	CHECK_EQ(ssh_channel_parse_end(&channels, input.data, input.size, &found), EPROTO);
	build_end(&input, 96, 0);
	CHECK_EQ(ssh_channel_parse_end(&channels, input.data, input.size, &found), EPROTO);

	/* Our CLOSE (97), once: the state stays CLOSING, a repeat writes nothing, and now the slot is freed. */
	channel->user = &marker;
	ssh_channel_put_close(channel, &out);
	wire_reset(&expected);
	wire_u8(&expected, 97);
	wire_u32(&expected, 9);
	CHECK_OUTPUT(&out, &expected);
	CHECK_EQ(channel->close_sent, 1);
	ssh_channel_put_close(channel, &out);
	CHECK_EQ(out.size, 0);
	CHECK_EQ(ssh_channel_closed(&channels, channel), 1);
	CHECK_EQ(channel->state, SSH_CHANNEL_FREE);
	CHECK(channel->user == &marker);
	CHECK_EQ(channel->local_id, 0);
	CHECK_EQ(channel->eof_sent, 0);
	CHECK_EQ(channel->close_received, 0);
	CHECK(ssh_channel_find(&channels, 0) == NULL);

	/* A free slot is not freed again, and an address outside the table is not a slot. */
	CHECK_EQ(ssh_channel_closed(&channels, channel), 0);
	CHECK_EQ(ssh_channel_closed(&channels, (struct ssh_channel *)&marker), 0);

	/* The slot is taken again by the next open; its user pointer is cleared. */
	found = open_server_channel(&channels, &out, 11, 5, 5);
	CHECK(found == channel);
	CHECK(found->user == NULL);
	CHECK_EQ(found->remote_id, 11);

	/* Closing from our side first: the CLOSE sent, the peer's answer received, then freed. */
	ssh_channel_put_close(channel, &out);
	ssh_buf_reset(&out);
	CHECK_EQ(channel->state, SSH_CHANNEL_CLOSING);
	CHECK_EQ(ssh_channel_closed(&channels, channel), 0);
	build_end(&input, 97, 0);
	CHECK_EQ(ssh_channel_parse_end(&channels, input.data, input.size, &found), 0);
	CHECK_EQ(ssh_channel_closed(&channels, channel), 1);

	/* A CLOSE for a channel that is not open, or an EOF for one, is EPROTO. */
	CHECK_EQ(ssh_channel_parse_end(&channels, input.data, input.size, &found), EPROTO);
	build_end(&input, 96, 3);
	CHECK_EQ(ssh_channel_parse_end(&channels, input.data, input.size, &found), EPROTO);

	/* Malformed: every cut, a trailing byte, another message number. */
	channel = open_server_channel(&channels, &out, 12, 5, 5);
	build_end(&input, 96, 0);
	for (index = 0; index < input.size; index++)
		CHECK_EQ(ssh_channel_parse_end(&channels, input.data, index, &found), EINVAL);
	wire_u8(&input, 0);
	CHECK_EQ(ssh_channel_parse_end(&channels, input.data, input.size, &found), EINVAL);
	build_end(&input, 95, 0);
	CHECK_EQ(ssh_channel_parse_end(&channels, input.data, input.size, &found), EINVAL);
	CHECK_EQ(channel->eof_received, 0);
	CHECK_EQ(channel->close_received, 0);

	/* The EOF and CLOSE a channel not yet confirmed cannot send: nothing is written. */
	channels.channel[5].state = SSH_CHANNEL_OPENING;
	ssh_channel_put_eof(&channels.channel[5], &out);
	ssh_channel_put_close(&channels.channel[5], &out);
	CHECK_EQ(out.size, 0);
	CHECK_EQ(channels.channel[5].state, SSH_CHANNEL_OPENING);

	ssh_buf_free(&out);
}

/*
 * Each channel request type is read from a message built by wire.h.
 */
static void
test_requests_parse(void)
{
	struct ssh_channels channels;
	struct ssh_channel *channel;
	struct ssh_buf out;
	static struct ssh_channel_request request;
	static uint8_t command[300];
	static const uint8_t modes[] = { 53, 0, 0, 0, 1, 0 };

	ssh_channels_init(&channels);
	ssh_buf_init(&out, 1U << 20, 0);
	channel = open_server_channel(&channels, &out, 9, 1000, 400);
	(void)channel;

	/* RFC 4254 6.2: pty-req with want_reply TRUE, term "xterm", 80 columns, 24 rows, 640 by 480 pixels, modes. */
	build_request(&input, 0, "pty-req", 1);
	wire_text(&input, "xterm");
	wire_u32(&input, 80);
	wire_u32(&input, 24);
	wire_u32(&input, 640);
	wire_u32(&input, 480);
	wire_string(&input, modes, sizeof(modes));
	CHECK_EQ(ssh_channel_parse_request(&channels, input.data, input.size, &request), 0);
	CHECK_EQ(request.type, SSH_CREQ_PTY);
	CHECK_EQ(request.recipient, 0);
	CHECK_EQ(request.want_reply, 1);
	CHECK(strcmp(request.term, "xterm") == 0);
	CHECK_EQ(request.columns, 80);
	CHECK_EQ(request.rows, 24);
	CHECK_EQ(request.width, 640);
	CHECK_EQ(request.height, 480);
	CHECK_EQ(request.modes_size, sizeof(modes));
	CHECK(request.modes == input.data + input.size - sizeof(modes));
	CHECK_BYTES(request.modes, modes, sizeof(modes));

	/* RFC 4254 6.4: env LANG=en_US.UTF-8, want_reply FALSE. */
	build_request(&input, 0, "env", 0);
	wire_text(&input, "LANG");
	wire_text(&input, "en_US.UTF-8");
	CHECK_EQ(ssh_channel_parse_request(&channels, input.data, input.size, &request), 0);
	CHECK_EQ(request.type, SSH_CREQ_ENV);
	CHECK_EQ(request.want_reply, 0);
	CHECK(strcmp(request.name, "LANG") == 0);
	CHECK(strcmp(request.value, "en_US.UTF-8") == 0);

	/* RFC 4254 6.5: shell. */
	build_request(&input, 0, "shell", 1);
	CHECK_EQ(ssh_channel_parse_request(&channels, input.data, input.size, &request), 0);
	CHECK_EQ(request.type, SSH_CREQ_SHELL);
	CHECK_EQ(request.want_reply, 1);

	/* exec with a command that holds a NUL and high bytes: bytes, not text, kept inside the payload. */
	memcpy(command, "echo a\0b\xff\xfe", 10);
	build_request(&input, 0, "exec", 1);
	wire_string(&input, command, 10);
	CHECK_EQ(ssh_channel_parse_request(&channels, input.data, input.size, &request), 0);
	CHECK_EQ(request.type, SSH_CREQ_EXEC);
	CHECK_EQ(request.command_size, 10);
	CHECK(request.command == input.data + input.size - 10);
	CHECK_BYTES(request.command, command, 10);

	/* subsystem "sftp". */
	build_request(&input, 0, "subsystem", 1);
	wire_text(&input, "sftp");
	CHECK_EQ(ssh_channel_parse_request(&channels, input.data, input.size, &request), 0);
	CHECK_EQ(request.type, SSH_CREQ_SUBSYSTEM);
	CHECK(strcmp(request.name, "sftp") == 0);

	/* RFC 4254 6.7: window-change 100 columns, 40 rows, 1000 by 800 pixels, no reply. */
	build_request(&input, 0, "window-change", 0);
	wire_u32(&input, 100);
	wire_u32(&input, 40);
	wire_u32(&input, 1000);
	wire_u32(&input, 800);
	CHECK_EQ(ssh_channel_parse_request(&channels, input.data, input.size, &request), 0);
	CHECK_EQ(request.type, SSH_CREQ_WINDOW_CHANGE);
	CHECK_EQ(request.columns, 100);
	CHECK_EQ(request.rows, 40);
	CHECK_EQ(request.width, 1000);
	CHECK_EQ(request.height, 800);

	/* RFC 4254 6.9: signal "TERM". */
	build_request(&input, 0, "signal", 0);
	wire_text(&input, "TERM");
	CHECK_EQ(ssh_channel_parse_request(&channels, input.data, input.size, &request), 0);
	CHECK_EQ(request.type, SSH_CREQ_SIGNAL);
	CHECK(strcmp(request.name, "TERM") == 0);

	/* RFC 4254 6.10: exit-status 255 (the largest 8-bit status) and the largest uint32. */
	build_request(&input, 0, "exit-status", 0);
	wire_u32(&input, 255);
	CHECK_EQ(ssh_channel_parse_request(&channels, input.data, input.size, &request), 0);
	CHECK_EQ(request.type, SSH_CREQ_EXIT_STATUS);
	CHECK_EQ(request.status, 255);
	build_request(&input, 0, "exit-status", 0);
	wire_u32(&input, 0xffffffffUL);
	CHECK_EQ(ssh_channel_parse_request(&channels, input.data, input.size, &request), 0);
	CHECK_EQ(request.status, 0xffffffffUL);

	/* exit-signal "SEGV", core dumped, message "boom", language tag "en". */
	build_request(&input, 0, "exit-signal", 0);
	wire_text(&input, "SEGV");
	wire_bool(&input, 1);
	wire_text(&input, "boom");
	wire_text(&input, "en");
	CHECK_EQ(ssh_channel_parse_request(&channels, input.data, input.size, &request), 0);
	CHECK_EQ(request.type, SSH_CREQ_EXIT_SIGNAL);
	CHECK(strcmp(request.name, "SEGV") == 0);
	CHECK_EQ(request.core_dumped, 1);
	CHECK_EQ(request.message_size, 4);
	CHECK_BYTES(request.message, "boom", 4);

	/* OpenSSH PROTOCOL 2.1: eow@openssh.com. */
	build_request(&input, 0, "eow@openssh.com", 0);
	CHECK_EQ(ssh_channel_parse_request(&channels, input.data, input.size, &request), 0);
	CHECK_EQ(request.type, SSH_CREQ_EOW);

	/* A boolean other than 0 and 1 is TRUE (RFC 4251 5). */
	build_request(&input, 0, "shell", 7);
	CHECK_EQ(ssh_channel_parse_request(&channels, input.data, input.size, &request), 0);
	CHECK_EQ(request.want_reply, 1);

	/* Requests the module refuses are read only as far as their name: x11-req, auth-agent-req@openssh.com, a made-up one. */
	build_request(&input, 0, "x11-req", 1);
	wire_bool(&input, 0);
	wire_text(&input, "MIT-MAGIC-COOKIE-1");
	wire_text(&input, "0123456789abcdef");
	wire_u32(&input, 0);
	CHECK_EQ(ssh_channel_parse_request(&channels, input.data, input.size, &request), 0);
	CHECK_EQ(request.type, SSH_CREQ_OTHER);
	CHECK_EQ(request.want_reply, 1);
	CHECK(strcmp(request.name, "x11-req") == 0);
	build_request(&input, 0, "auth-agent-req@openssh.com", 1);
	wire_bytes(&input, "\x01\x02 trailing bytes of any shape", 29);
	CHECK_EQ(ssh_channel_parse_request(&channels, input.data, input.size, &request), 0);
	CHECK_EQ(request.type, SSH_CREQ_OTHER);
	CHECK(strcmp(request.name, "auth-agent-req@openssh.com") == 0);
	build_request(&input, 0, "no-such-request", 0);
	CHECK_EQ(ssh_channel_parse_request(&channels, input.data, input.size, &request), 0);
	CHECK_EQ(request.type, SSH_CREQ_OTHER);
	CHECK_EQ(request.want_reply, 0);
	CHECK(strcmp(request.name, "no-such-request") == 0);

	/* Names that only look like a known one are other: case, a suffix, a prefix, an embedded NUL. */
	build_request(&input, 0, "Shell", 1);
	CHECK_EQ(ssh_channel_parse_request(&channels, input.data, input.size, &request), 0);
	CHECK_EQ(request.type, SSH_CREQ_OTHER);
	build_request(&input, 0, "shell2", 1);
	CHECK_EQ(ssh_channel_parse_request(&channels, input.data, input.size, &request), 0);
	CHECK_EQ(request.type, SSH_CREQ_OTHER);
	build_request(&input, 0, "shel", 1);
	CHECK_EQ(ssh_channel_parse_request(&channels, input.data, input.size, &request), 0);
	CHECK_EQ(request.type, SSH_CREQ_OTHER);
	wire_reset(&input);
	wire_u8(&input, 98);
	wire_u32(&input, 0);
	wire_string(&input, "shell\0x", 7);
	wire_bool(&input, 1);
	CHECK_EQ(ssh_channel_parse_request(&channels, input.data, input.size, &request), 0);
	CHECK_EQ(request.type, SSH_CREQ_OTHER);
	CHECK(strlen(request.name) == 7);

	/* An empty request name is other. */
	build_request(&input, 0, "", 0);
	CHECK_EQ(ssh_channel_parse_request(&channels, input.data, input.size, &request), 0);
	CHECK_EQ(request.type, SSH_CREQ_OTHER);
	CHECK_EQ(request.name[0], 0);

	ssh_buf_free(&out);
}

/*
 * The limits of the strings of the known requests: the limit itself is
 * read, one byte more makes the request SSH_CREQ_OTHER (the connection is
 * kept), and so does a name of 100 bytes.
 */
static void
test_requests_limits(void)
{
	struct ssh_channels channels;
	struct ssh_buf out;
	static struct ssh_channel_request request;
	static char text[SSH_CHANNEL_COMMAND_MAX + 2];
	char expected_name[SSH_NAME_MAX + 2];

	ssh_channels_init(&channels);
	ssh_buf_init(&out, 1U << 20, 0);
	(void)open_server_channel(&channels, &out, 9, 1000, 400);
	memset(text, 'a', sizeof(text));
	text[sizeof(text) - 1] = '\0';

	/* A request name of 100 bytes is cut to its first 64 and is SSH_CREQ_OTHER. */
	text[100] = '\0';
	build_request(&input, 0, text, 1);
	wire_u32(&input, 1);
	CHECK_EQ(ssh_channel_parse_request(&channels, input.data, input.size, &request), 0);
	CHECK_EQ(request.type, SSH_CREQ_OTHER);
	CHECK_EQ(request.want_reply, 1);
	memset(expected_name, 'a', 64);
	expected_name[64] = '\0';
	CHECK(strcmp(request.name, expected_name) == 0);
	memset(text, 'a', sizeof(text) - 1);

	/* A request name of exactly 64 bytes is not cut (and is other, since it is no known name). */
	text[64] = '\0';
	build_request(&input, 0, text, 0);
	CHECK_EQ(ssh_channel_parse_request(&channels, input.data, input.size, &request), 0);
	CHECK(strcmp(request.name, expected_name) == 0);
	memset(text, 'a', sizeof(text) - 1);

	/* pty-req: a terminal type of 64 is read, 65 makes the request other (name "pty-req"). */
	text[64] = '\0';
	build_request(&input, 0, "pty-req", 1);
	wire_text(&input, text);
	wire_u32(&input, 80);
	wire_u32(&input, 24);
	wire_u32(&input, 0);
	wire_u32(&input, 0);
	wire_string(&input, "", 0);
	CHECK_EQ(ssh_channel_parse_request(&channels, input.data, input.size, &request), 0);
	CHECK_EQ(request.type, SSH_CREQ_PTY);
	CHECK(strlen(request.term) == 64);
	memset(text, 'a', sizeof(text) - 1);
	text[65] = '\0';
	build_request(&input, 0, "pty-req", 1);
	wire_text(&input, text);
	wire_u32(&input, 80);
	wire_u32(&input, 24);
	wire_u32(&input, 0);
	wire_u32(&input, 0);
	wire_string(&input, "", 0);
	CHECK_EQ(ssh_channel_parse_request(&channels, input.data, input.size, &request), 0);
	CHECK_EQ(request.type, SSH_CREQ_OTHER);
	CHECK(strcmp(request.name, "pty-req") == 0);
	CHECK_EQ(request.columns, 0);
	CHECK_EQ(request.modes_size, 0);
	memset(text, 'a', sizeof(text) - 1);

	/* env: a name of 64 and a value of 1024 are read; a name of 65 or a value of 1025 makes the request other. */
	text[64] = '\0';
	build_request(&input, 0, "env", 0);
	wire_text(&input, text);
	wire_text(&input, "v");
	CHECK_EQ(ssh_channel_parse_request(&channels, input.data, input.size, &request), 0);
	CHECK_EQ(request.type, SSH_CREQ_ENV);
	CHECK(strlen(request.name) == 64);
	memset(text, 'a', sizeof(text) - 1);
	text[65] = '\0';
	build_request(&input, 0, "env", 0);
	wire_text(&input, text);
	wire_text(&input, "v");
	CHECK_EQ(ssh_channel_parse_request(&channels, input.data, input.size, &request), 0);
	CHECK_EQ(request.type, SSH_CREQ_OTHER);
	CHECK(strcmp(request.name, "env") == 0);
	memset(text, 'a', sizeof(text) - 1);
	text[1024] = '\0';
	build_request(&input, 0, "env", 0);
	wire_text(&input, "N");
	wire_text(&input, text);
	CHECK_EQ(ssh_channel_parse_request(&channels, input.data, input.size, &request), 0);
	CHECK_EQ(request.type, SSH_CREQ_ENV);
	CHECK(strlen(request.value) == 1024);
	memset(text, 'a', sizeof(text) - 1);
	text[1025] = '\0';
	build_request(&input, 0, "env", 0);
	wire_text(&input, "N");
	wire_text(&input, text);
	CHECK_EQ(ssh_channel_parse_request(&channels, input.data, input.size, &request), 0);
	CHECK_EQ(request.type, SSH_CREQ_OTHER);
	CHECK(strcmp(request.name, "env") == 0);
	CHECK_EQ(request.value[0], 0);
	memset(text, 'a', sizeof(text) - 1);

	/* env with a NUL inside the name or the value is other. */
	build_request(&input, 0, "env", 0);
	wire_string(&input, "A\0B", 3);
	wire_text(&input, "v");
	CHECK_EQ(ssh_channel_parse_request(&channels, input.data, input.size, &request), 0);
	CHECK_EQ(request.type, SSH_CREQ_OTHER);
	build_request(&input, 0, "env", 0);
	wire_text(&input, "A");
	wire_string(&input, "v\0w", 3);
	CHECK_EQ(ssh_channel_parse_request(&channels, input.data, input.size, &request), 0);
	CHECK_EQ(request.type, SSH_CREQ_OTHER);

	/* exec: 65536 bytes are read, 65537 make the request other. */
	build_request(&input, 0, "exec", 1);
	wire_string(&input, text, 65536);
	CHECK_EQ(ssh_channel_parse_request(&channels, input.data, input.size, &request), 0);
	CHECK_EQ(request.type, SSH_CREQ_EXEC);
	CHECK_EQ(request.command_size, 65536);
	build_request(&input, 0, "exec", 1);
	wire_string(&input, text, 65537);
	CHECK_EQ(ssh_channel_parse_request(&channels, input.data, input.size, &request), 0);
	CHECK_EQ(request.type, SSH_CREQ_OTHER);
	CHECK(strcmp(request.name, "exec") == 0);
	CHECK_EQ(request.want_reply, 1);
	CHECK(request.command == NULL);

	/* subsystem: 64 read, 65 other. */
	text[64] = '\0';
	build_request(&input, 0, "subsystem", 1);
	wire_text(&input, text);
	CHECK_EQ(ssh_channel_parse_request(&channels, input.data, input.size, &request), 0);
	CHECK_EQ(request.type, SSH_CREQ_SUBSYSTEM);
	memset(text, 'a', sizeof(text) - 1);
	text[65] = '\0';
	build_request(&input, 0, "subsystem", 1);
	wire_text(&input, text);
	CHECK_EQ(ssh_channel_parse_request(&channels, input.data, input.size, &request), 0);
	CHECK_EQ(request.type, SSH_CREQ_OTHER);
	memset(text, 'a', sizeof(text) - 1);

	/* signal and exit-signal: 32 read, 33 other. */
	text[32] = '\0';
	build_request(&input, 0, "signal", 0);
	wire_text(&input, text);
	CHECK_EQ(ssh_channel_parse_request(&channels, input.data, input.size, &request), 0);
	CHECK_EQ(request.type, SSH_CREQ_SIGNAL);
	CHECK(strlen(request.name) == 32);
	memset(text, 'a', sizeof(text) - 1);
	text[33] = '\0';
	build_request(&input, 0, "signal", 0);
	wire_text(&input, text);
	CHECK_EQ(ssh_channel_parse_request(&channels, input.data, input.size, &request), 0);
	CHECK_EQ(request.type, SSH_CREQ_OTHER);
	memset(text, 'a', sizeof(text) - 1);
	text[32] = '\0';
	build_request(&input, 0, "exit-signal", 0);
	wire_text(&input, text);
	wire_bool(&input, 0);
	wire_text(&input, "");
	wire_text(&input, "");
	CHECK_EQ(ssh_channel_parse_request(&channels, input.data, input.size, &request), 0);
	CHECK_EQ(request.type, SSH_CREQ_EXIT_SIGNAL);
	memset(text, 'a', sizeof(text) - 1);
	text[33] = '\0';
	build_request(&input, 0, "exit-signal", 0);
	wire_text(&input, text);
	wire_bool(&input, 0);
	wire_text(&input, "");
	wire_text(&input, "");
	CHECK_EQ(ssh_channel_parse_request(&channels, input.data, input.size, &request), 0);
	CHECK_EQ(request.type, SSH_CREQ_OTHER);
	CHECK(strcmp(request.name, "exit-signal") == 0);

	ssh_buf_free(&out);
}

/*
 * Framing: a payload too short for a known request is EINVAL at every
 * cut; a request for an unknown channel is EPROTO; the channel may be
 * CLOSING.
 */
static void
test_requests_framing(void)
{
	struct ssh_channels channels;
	struct ssh_channel *channel;
	struct ssh_buf out;
	static struct ssh_channel_request request;
	static struct wire messages[8];
	size_t index;
	size_t cut;

	ssh_channels_init(&channels);
	ssh_buf_init(&out, 1U << 20, 0);
	channel = open_server_channel(&channels, &out, 9, 1000, 400);

	/* One message of each known type that has fields (and shell, which has none). */
	build_request(&messages[0], 0, "pty-req", 1);
	wire_text(&messages[0], "xterm");
	wire_u32(&messages[0], 80);
	wire_u32(&messages[0], 24);
	wire_u32(&messages[0], 640);
	wire_u32(&messages[0], 480);
	wire_string(&messages[0], "\x35\0\0\0\1\0", 6);
	build_request(&messages[1], 0, "env", 0);
	wire_text(&messages[1], "A");
	wire_text(&messages[1], "B");
	build_request(&messages[2], 0, "exec", 1);
	wire_text(&messages[2], "ls");
	build_request(&messages[3], 0, "subsystem", 1);
	wire_text(&messages[3], "sftp");
	build_request(&messages[4], 0, "window-change", 0);
	wire_u32(&messages[4], 1);
	wire_u32(&messages[4], 2);
	wire_u32(&messages[4], 3);
	wire_u32(&messages[4], 4);
	build_request(&messages[5], 0, "signal", 0);
	wire_text(&messages[5], "INT");
	build_request(&messages[6], 0, "exit-status", 0);
	wire_u32(&messages[6], 3);
	build_request(&messages[7], 0, "exit-signal", 0);
	wire_text(&messages[7], "SEGV");
	wire_bool(&messages[7], 0);
	wire_text(&messages[7], "m");
	wire_text(&messages[7], "en");

	/* Every proper prefix is EINVAL; the whole message is read. */
	for (index = 0; index < 8; index++) {
		for (cut = 0; cut < messages[index].size; cut++)
			CHECK_EQ(ssh_channel_parse_request(&channels, messages[index].data, cut, &request), EINVAL);
		CHECK_EQ(ssh_channel_parse_request(&channels, messages[index].data, messages[index].size, &request), 0);
		CHECK(request.type != SSH_CREQ_OTHER);
	}

	/* A shell request cut before want_reply is EINVAL. */
	build_request(&input, 0, "shell", 1);
	CHECK_EQ(ssh_channel_parse_request(&channels, input.data, input.size - 1, &request), EINVAL);

	/* A lenient read: bytes after the fields of a known request are not looked at. */
	build_request(&input, 0, "shell", 1);
	wire_bytes(&input, "tail", 4);
	CHECK_EQ(ssh_channel_parse_request(&channels, input.data, input.size, &request), 0);
	CHECK_EQ(request.type, SSH_CREQ_SHELL);

	/* An unknown channel: past the table, the maximum, a free slot. */
	build_request(&input, 1, "shell", 1);
	CHECK_EQ(ssh_channel_parse_request(&channels, input.data, input.size, &request), EPROTO);
	build_request(&input, 10, "shell", 1);
	CHECK_EQ(ssh_channel_parse_request(&channels, input.data, input.size, &request), EPROTO);
	build_request(&input, 0xffffffffUL, "shell", 1);
	CHECK_EQ(ssh_channel_parse_request(&channels, input.data, input.size, &request), EPROTO);

	/* A channel waiting for its answer is not one to request on. */
	channels.channel[1].state = SSH_CHANNEL_OPENING;
	build_request(&input, 1, "shell", 1);
	CHECK_EQ(ssh_channel_parse_request(&channels, input.data, input.size, &request), EPROTO);
	channels.channel[1].state = SSH_CHANNEL_FREE;

	/* Another message number is EINVAL. */
	build_request(&input, 0, "shell", 1);
	input.data[0] = 99;
	CHECK_EQ(ssh_channel_parse_request(&channels, input.data, input.size, &request), EINVAL);

	/* A closing channel is read (the caller drops the request and answers FAILURE). */
	channel->state = SSH_CHANNEL_CLOSING;
	channel->close_sent = 1;
	build_request(&input, 0, "shell", 1);
	CHECK_EQ(ssh_channel_parse_request(&channels, input.data, input.size, &request), 0);
	CHECK_EQ(request.type, SSH_CREQ_SHELL);

	ssh_buf_free(&out);
}

/*
 * The messages a client writes, compared with bytes built by wire.h, and
 * then read by a server table: the whole opening handshake included.
 */
static void
test_requests_put_roundtrip(void)
{
	struct ssh_channels client;
	struct ssh_channels server;
	struct ssh_channel *client_channel;
	struct ssh_channel *server_channel;
	struct ssh_buf out;
	struct ssh_buf wire_out;
	static struct ssh_channel_request request;
	static const uint8_t modes[] = { 53, 0, 0, 0, 1, 0 };
	uint32_t reason;
	int success;

	ssh_channels_init(&client);
	ssh_channels_init(&server);
	ssh_buf_init(&out, 1U << 20, 0);
	ssh_buf_init(&wire_out, 1U << 20, 0);

	/* The handshake: the client opens, the server accepts and confirms, the client reads the confirmation. */
	client_channel = NULL;
	CHECK_EQ(ssh_channel_open_session(&client, &out, &client_channel), 0);
	server_channel = NULL;
	CHECK_EQ(ssh_channel_accept_open(&server, out.data, out.size, &wire_out, &server_channel), 0);
	CHECK(server_channel != NULL);
	ssh_buf_reset(&out);
	CHECK_EQ(ssh_channel_open_reply(&client, wire_out.data, wire_out.size, &client_channel, &reason), 0);
	ssh_buf_reset(&wire_out);
	CHECK_EQ(client_channel->state, SSH_CHANNEL_OPEN);
	CHECK_EQ(client_channel->remote_id, server_channel->local_id);
	CHECK_EQ(client_channel->remote_window, SSH_CHANNEL_WINDOW);
	CHECK_EQ(client_channel->remote_packet, SSH_CHANNEL_PACKET);
	CHECK_EQ(server_channel->remote_id, client_channel->local_id);

	/* pty-req: bytes as RFC 4254 6.2, want_reply TRUE. */
	ssh_channel_put_pty(client_channel, "vt100", 132, 43, 1056, 688, modes, sizeof(modes), &out);
	build_request(&expected, 0, "pty-req", 1);
	wire_text(&expected, "vt100");
	wire_u32(&expected, 132);
	wire_u32(&expected, 43);
	wire_u32(&expected, 1056);
	wire_u32(&expected, 688);
	wire_string(&expected, modes, sizeof(modes));
	CHECK_EQ(out.size, expected.size);
	CHECK_BYTES(out.data, expected.data, expected.size);
	CHECK_EQ(ssh_channel_parse_request(&server, out.data, out.size, &request), 0);
	CHECK_EQ(request.type, SSH_CREQ_PTY);
	CHECK_EQ(request.want_reply, 1);
	CHECK(strcmp(request.term, "vt100") == 0);
	CHECK_EQ(request.columns, 132);
	CHECK_EQ(request.rows, 43);
	CHECK_EQ(request.width, 1056);
	CHECK_EQ(request.height, 688);
	CHECK_EQ(request.modes_size, sizeof(modes));
	CHECK_BYTES(request.modes, modes, sizeof(modes));
	ssh_buf_reset(&out);

	/* env: want_reply FALSE. */
	ssh_channel_put_env(client_channel, "LC_ALL", "C", &out);
	build_request(&expected, 0, "env", 0);
	wire_text(&expected, "LC_ALL");
	wire_text(&expected, "C");
	CHECK_EQ(out.size, expected.size);
	CHECK_BYTES(out.data, expected.data, expected.size);
	CHECK_EQ(ssh_channel_parse_request(&server, out.data, out.size, &request), 0);
	CHECK_EQ(request.type, SSH_CREQ_ENV);
	CHECK(strcmp(request.name, "LC_ALL") == 0);
	CHECK(strcmp(request.value, "C") == 0);
	ssh_buf_reset(&out);

	/* shell: want_reply TRUE. */
	ssh_channel_put_shell(client_channel, &out);
	build_request(&expected, 0, "shell", 1);
	CHECK_OUTPUT(&out, &expected);
	ssh_channel_put_shell(client_channel, &out);
	CHECK_EQ(ssh_channel_parse_request(&server, out.data, out.size, &request), 0);
	CHECK_EQ(request.type, SSH_CREQ_SHELL);
	CHECK_EQ(request.want_reply, 1);
	ssh_buf_reset(&out);

	/* exec with bytes that are not text. */
	ssh_channel_put_exec(client_channel, (const uint8_t *)"a\0b", 3, &out);
	build_request(&expected, 0, "exec", 1);
	wire_string(&expected, "a\0b", 3);
	CHECK_EQ(out.size, expected.size);
	CHECK_BYTES(out.data, expected.data, expected.size);
	CHECK_EQ(ssh_channel_parse_request(&server, out.data, out.size, &request), 0);
	CHECK_EQ(request.type, SSH_CREQ_EXEC);
	CHECK_EQ(request.command_size, 3);
	CHECK_BYTES(request.command, "a\0b", 3);
	ssh_buf_reset(&out);

	/* subsystem. */
	ssh_channel_put_subsystem(client_channel, "sftp", &out);
	build_request(&expected, 0, "subsystem", 1);
	wire_text(&expected, "sftp");
	CHECK_EQ(out.size, expected.size);
	CHECK_BYTES(out.data, expected.data, expected.size);
	CHECK_EQ(ssh_channel_parse_request(&server, out.data, out.size, &request), 0);
	CHECK_EQ(request.type, SSH_CREQ_SUBSYSTEM);
	CHECK(strcmp(request.name, "sftp") == 0);
	ssh_buf_reset(&out);

	/* window-change: want_reply FALSE, columns, rows, width, height in that order. */
	ssh_channel_put_window_change(client_channel, 100, 50, 800, 600, &out);
	build_request(&expected, 0, "window-change", 0);
	wire_u32(&expected, 100);
	wire_u32(&expected, 50);
	wire_u32(&expected, 800);
	wire_u32(&expected, 600);
	CHECK_EQ(out.size, expected.size);
	CHECK_BYTES(out.data, expected.data, expected.size);
	CHECK_EQ(ssh_channel_parse_request(&server, out.data, out.size, &request), 0);
	CHECK_EQ(request.type, SSH_CREQ_WINDOW_CHANGE);
	CHECK_EQ(request.columns, 100);
	CHECK_EQ(request.rows, 50);
	CHECK_EQ(request.width, 800);
	CHECK_EQ(request.height, 600);
	ssh_buf_reset(&out);

	/* The server's side: exit-status and exit-signal for the client's number. */
	ssh_channel_put_exit_status(server_channel, 7, &wire_out);
	build_request(&expected, 0, "exit-status", 0);
	wire_u32(&expected, 7);
	CHECK_EQ(wire_out.size, expected.size);
	CHECK_BYTES(wire_out.data, expected.data, expected.size);
	CHECK_EQ(ssh_channel_parse_request(&client, wire_out.data, wire_out.size, &request), 0);
	CHECK_EQ(request.type, SSH_CREQ_EXIT_STATUS);
	CHECK_EQ(request.status, 7);
	ssh_buf_reset(&wire_out);
	ssh_channel_put_exit_signal(server_channel, "TERM", 1, &wire_out);
	build_request(&expected, 0, "exit-signal", 0);
	wire_text(&expected, "TERM");
	wire_bool(&expected, 1);
	wire_text(&expected, "");
	wire_text(&expected, "");
	CHECK_EQ(wire_out.size, expected.size);
	CHECK_BYTES(wire_out.data, expected.data, expected.size);
	CHECK_EQ(ssh_channel_parse_request(&client, wire_out.data, wire_out.size, &request), 0);
	CHECK_EQ(request.type, SSH_CREQ_EXIT_SIGNAL);
	CHECK(strcmp(request.name, "TERM") == 0);
	CHECK_EQ(request.core_dumped, 1);
	CHECK_EQ(request.message_size, 0);
	ssh_buf_reset(&wire_out);
	ssh_channel_put_exit_signal(server_channel, "KILL", 0, &wire_out);
	CHECK_EQ(ssh_channel_parse_request(&client, wire_out.data, wire_out.size, &request), 0);
	CHECK_EQ(request.core_dumped, 0);
	ssh_buf_reset(&wire_out);

	/* The replies: CHANNEL_SUCCESS (99) and CHANNEL_FAILURE (100) for the peer's number, and their reading. */
	ssh_channel_put_reply(server_channel, 1, &wire_out);
	wire_reset(&expected);
	wire_u8(&expected, 99);
	wire_u32(&expected, 0);
	CHECK_EQ(wire_out.size, expected.size);
	CHECK_BYTES(wire_out.data, expected.data, expected.size);
	success = 9;
	CHECK_EQ(ssh_channel_parse_reply(&client, wire_out.data, wire_out.size, &client_channel, &success), 0);
	CHECK_EQ(success, 1);
	ssh_buf_reset(&wire_out);
	ssh_channel_put_reply(server_channel, 0, &wire_out);
	wire_reset(&expected);
	wire_u8(&expected, 100);
	wire_u32(&expected, 0);
	CHECK_EQ(wire_out.size, expected.size);
	CHECK_BYTES(wire_out.data, expected.data, expected.size);
	CHECK_EQ(ssh_channel_parse_reply(&client, wire_out.data, wire_out.size, &client_channel, &success), 0);
	CHECK_EQ(success, 0);
	ssh_buf_reset(&wire_out);

	/* Requests on a channel that is not confirmed record EINVAL in the output and write nothing meaningful. */
	ssh_buf_reset(&out);
	client.channel[3].state = SSH_CHANNEL_OPENING;
	ssh_channel_put_shell(&client.channel[3], &out);
	CHECK_EQ(ssh_buf_error(&out), EINVAL);
	CHECK_EQ(out.size, 0);
	ssh_buf_reset(&out);
	ssh_channel_put_reply(&client.channel[3], 1, &out);
	CHECK_EQ(ssh_buf_error(&out), EINVAL);
	ssh_buf_reset(&out);

	/* A string past the output limit leaves the error of the buffer. */
	ssh_buf_free(&out);
	ssh_buf_init(&out, 10, 0);
	ssh_channel_put_exec(client_channel, (const uint8_t *)"0123456789", 10, &out);
	CHECK_EQ(ssh_buf_error(&out), EMSGSIZE);

	ssh_buf_free(&out);
	ssh_buf_free(&wire_out);
}

/*
 * CHANNEL_SUCCESS and CHANNEL_FAILURE read from the wire, with the
 * mistakes.
 */
static void
test_reply_messages(void)
{
	struct ssh_channels channels;
	struct ssh_channel *channel;
	struct ssh_channel *found;
	struct ssh_buf out;
	size_t index;
	int success;

	ssh_channels_init(&channels);
	ssh_buf_init(&out, 1U << 20, 0);
	channel = open_server_channel(&channels, &out, 9, 1000, 400);

	/* RFC 4254 5.4: SUCCESS (99) and FAILURE (100) for our number 0. */
	build_end(&input, 99, 0);
	success = 5;
	found = NULL;
	CHECK_EQ(ssh_channel_parse_reply(&channels, input.data, input.size, &found, &success), 0);
	CHECK(found == channel);
	CHECK_EQ(success, 1);
	build_end(&input, 100, 0);
	CHECK_EQ(ssh_channel_parse_reply(&channels, input.data, input.size, &found, &success), 0);
	CHECK_EQ(success, 0);

	/* An unknown channel is EPROTO; cuts, a trailing byte, another number are EINVAL. */
	build_end(&input, 99, 1);
	CHECK_EQ(ssh_channel_parse_reply(&channels, input.data, input.size, &found, &success), EPROTO);
	build_end(&input, 99, 0);
	for (index = 0; index < input.size; index++)
		CHECK_EQ(ssh_channel_parse_reply(&channels, input.data, index, &found, &success), EINVAL);
	wire_u8(&input, 0);
	CHECK_EQ(ssh_channel_parse_reply(&channels, input.data, input.size, &found, &success), EINVAL);
	build_end(&input, 98, 0);
	CHECK_EQ(ssh_channel_parse_reply(&channels, input.data, input.size, &found, &success), EINVAL);

	ssh_buf_free(&out);
}

/*
 * RFC 4254 5.3: after this side sends CLOSE the peer may still send data,
 * EOF and requests until its own CLOSE; they are read, and the data is
 * charged to the window.
 */
static void
test_closing_reads(void)
{
	struct ssh_channels channels;
	struct ssh_channel *channel;
	struct ssh_channel *found;
	struct ssh_buf out;
	static struct ssh_channel_request request;
	const uint8_t *bytes;
	size_t bytes_size;
	uint32_t extended;

	ssh_channels_init(&channels);
	ssh_buf_init(&out, 1U << 20, 0);
	channel = open_server_channel(&channels, &out, 9, 1000, 400);
	ssh_channel_put_close(channel, &out);
	ssh_buf_reset(&out);
	CHECK_EQ(channel->state, SSH_CHANNEL_CLOSING);
	CHECK(ssh_channel_find(&channels, 0) == channel);

	/* Data is read and charged: 2097152 - 6 = 2097146. */
	build_data(&input, 0, "abcdef", 6);
	CHECK_EQ(ssh_channel_parse_data(&channels, input.data, input.size, &found, &bytes, &bytes_size, &extended), 0);
	CHECK_EQ(bytes_size, 6);
	CHECK_EQ(channel->local_window, 2097146UL);

	/* Data past the window is still a fault. */
	channel->local_window = 2;
	build_data(&input, 0, "abc", 3);
	CHECK_EQ(ssh_channel_parse_data(&channels, input.data, input.size, &found, &bytes, &bytes_size, &extended), EPROTO);

	/* A window-change request is read, and the program answers a wanted reply with FAILURE (100). */
	build_request(&input, 0, "window-change", 1);
	wire_u32(&input, 1);
	wire_u32(&input, 2);
	wire_u32(&input, 3);
	wire_u32(&input, 4);
	CHECK_EQ(ssh_channel_parse_request(&channels, input.data, input.size, &request), 0);
	CHECK_EQ(request.type, SSH_CREQ_WINDOW_CHANGE);
	CHECK_EQ(request.want_reply, 1);
	ssh_channel_put_reply(channel, 0, &out);
	wire_reset(&expected);
	wire_u8(&expected, 100);
	wire_u32(&expected, 9);
	CHECK_OUTPUT(&out, &expected);

	/* The peer's EOF is read, then its CLOSE; then the slot is freed, since our CLOSE went before. */
	build_end(&input, 96, 0);
	CHECK_EQ(ssh_channel_parse_end(&channels, input.data, input.size, &found), 0);
	CHECK_EQ(ssh_channel_closed(&channels, channel), 0);
	build_end(&input, 97, 0);
	CHECK_EQ(ssh_channel_parse_end(&channels, input.data, input.size, &found), 0);
	CHECK_EQ(ssh_channel_closed(&channels, channel), 1);

	/* No more reads on the freed slot. */
	build_data(&input, 0, "x", 1);
	CHECK_EQ(ssh_channel_parse_data(&channels, input.data, input.size, &found, &bytes, &bytes_size, &extended), EPROTO);

	ssh_buf_free(&out);
}

/*
 * Global requests (RFC 4254 4 and OpenSSH PROTOCOL 2.2).
 */
static void
test_global_requests(void)
{
	struct ssh_channels channels;
	struct ssh_channel *channel;
	struct ssh_buf out;
	char name[SSH_NAME_MAX + 1];
	char long_name[101];
	int want_reply;
	size_t index;

	ssh_channels_init(&channels);
	ssh_buf_init(&out, 1U << 20, 0);

	/* keepalive@openssh.com with want_reply TRUE is refused: REQUEST_FAILURE (82). */
	wire_reset(&input);
	wire_u8(&input, 80);
	wire_text(&input, "keepalive@openssh.com");
	wire_bool(&input, 1);
	want_reply = 0;
	CHECK_EQ(ssh_channel_global_request(&channels, input.data, input.size, name, &want_reply, &out), 0);
	CHECK(strcmp(name, "keepalive@openssh.com") == 0);
	CHECK_EQ(want_reply, 1);
	wire_reset(&expected);
	wire_u8(&expected, 82);
	CHECK_OUTPUT(&out, &expected);
	CHECK_EQ(channels.no_more_sessions, 0);

	/* With want_reply FALSE nothing is written. */
	wire_reset(&input);
	wire_u8(&input, 80);
	wire_text(&input, "keepalive@openssh.com");
	wire_bool(&input, 0);
	CHECK_EQ(ssh_channel_global_request(&channels, input.data, input.size, name, &want_reply, &out), 0);
	CHECK_EQ(want_reply, 0);
	CHECK_EQ(out.size, 0);

	/* A boolean of 2 is TRUE. */
	wire_reset(&input);
	wire_u8(&input, 80);
	wire_text(&input, "tcpip-forward");
	wire_bool(&input, 2);
	wire_text(&input, "0.0.0.0");
	wire_u32(&input, 8080);
	CHECK_EQ(ssh_channel_global_request(&channels, input.data, input.size, name, &want_reply, &out), 0);
	CHECK_EQ(want_reply, 1);
	CHECK(strcmp(name, "tcpip-forward") == 0);
	wire_reset(&expected);
	wire_u8(&expected, 82);
	CHECK_OUTPUT(&out, &expected);

	/* hostkeys-00@openssh.com (OpenSSH PROTOCOL 2.5) carries key blobs after want_reply FALSE; they are not read. */
	wire_reset(&input);
	wire_u8(&input, 80);
	wire_text(&input, "hostkeys-00@openssh.com");
	wire_bool(&input, 0);
	wire_string(&input, "\0\0\0\x0bssh-ed25519\0\0\0\x20" "01234567890123456789012345678901", 47);
	wire_string(&input, "second key blob", 15);
	CHECK_EQ(ssh_channel_global_request(&channels, input.data, input.size, name, &want_reply, &out), 0);
	CHECK(strcmp(name, "hostkeys-00@openssh.com") == 0);
	CHECK_EQ(want_reply, 0);
	CHECK_EQ(out.size, 0);

	/* A name of 100 bytes is cut to 64. */
	memset(long_name, 'n', 100);
	long_name[100] = '\0';
	wire_reset(&input);
	wire_u8(&input, 80);
	wire_text(&input, long_name);
	wire_bool(&input, 1);
	CHECK_EQ(ssh_channel_global_request(&channels, input.data, input.size, name, &want_reply, &out), 0);
	CHECK_EQ(strlen(name), 64);
	CHECK_BYTES(name, long_name, 64);
	wire_reset(&expected);
	wire_u8(&expected, 82);
	CHECK_OUTPUT(&out, &expected);

	/* A name of exactly 64 is kept whole. */
	long_name[64] = '\0';
	wire_reset(&input);
	wire_u8(&input, 80);
	wire_text(&input, long_name);
	wire_bool(&input, 0);
	CHECK_EQ(ssh_channel_global_request(&channels, input.data, input.size, name, &want_reply, &out), 0);
	CHECK_EQ(strlen(name), 64);

	/* A name with a NUL or a control byte shows as '?' and is not mistaken for no-more-sessions. */
	wire_reset(&input);
	wire_u8(&input, 80);
	wire_string(&input, "no-more-sessions@openssh.com\0", 29);
	wire_bool(&input, 0);
	CHECK_EQ(ssh_channel_global_request(&channels, input.data, input.size, name, &want_reply, &out), 0);
	CHECK_EQ(channels.no_more_sessions, 0);
	CHECK_EQ(name[28], '?');
	CHECK_EQ(name[29], 0);

	/* OpenSSH PROTOCOL 2.2: no-more-sessions@openssh.com sets the mark; no reply wanted, none written. */
	wire_reset(&input);
	wire_u8(&input, 80);
	wire_text(&input, "no-more-sessions@openssh.com");
	wire_bool(&input, 0);
	CHECK_EQ(ssh_channel_global_request(&channels, input.data, input.size, name, &want_reply, &out), 0);
	CHECK_EQ(channels.no_more_sessions, 1);
	CHECK_EQ(out.size, 0);
	CHECK(strcmp(name, "no-more-sessions@openssh.com") == 0);

	/* The next session open is refused (reason 1). */
	build_open(&input, "session", 3, 10, 10);
	channel = &channels.channel[0];
	CHECK_EQ(ssh_channel_accept_open(&channels, input.data, input.size, &out, &channel), 0);
	CHECK(channel == NULL);
	CHECK_EQ(out.data[0], 92);
	CHECK_EQ(out.data[8], 1);
	ssh_buf_reset(&out);

	/* When a reply is wanted for the honoured request, it is answered with REQUEST_SUCCESS (81). */
	ssh_channels_init(&channels);
	wire_reset(&input);
	wire_u8(&input, 80);
	wire_text(&input, "no-more-sessions@openssh.com");
	wire_bool(&input, 1);
	CHECK_EQ(ssh_channel_global_request(&channels, input.data, input.size, name, &want_reply, &out), 0);
	CHECK_EQ(channels.no_more_sessions, 1);
	wire_reset(&expected);
	wire_u8(&expected, 81);
	CHECK_OUTPUT(&out, &expected);

	/* Malformed: every cut is EINVAL; a wrong number is EINVAL. */
	wire_reset(&input);
	wire_u8(&input, 80);
	wire_text(&input, "keepalive@openssh.com");
	wire_bool(&input, 1);
	for (index = 0; index < input.size; index++)
		CHECK_EQ(ssh_channel_global_request(&channels, input.data, index, name, &want_reply, &out), EINVAL);
	input.data[0] = 81;
	CHECK_EQ(ssh_channel_global_request(&channels, input.data, input.size, name, &want_reply, &out), EINVAL);
	CHECK_EQ(out.size, 0);

	/* put_global_request: keepalive with a reply wanted (RFC 4254 4: byte 80, name, boolean). */
	ssh_channel_put_global_request("keepalive@openssh.com", 1, &out);
	wire_reset(&expected);
	wire_u8(&expected, 80);
	wire_text(&expected, "keepalive@openssh.com");
	wire_bool(&expected, 1);
	CHECK_OUTPUT(&out, &expected);
	ssh_channel_put_global_request("x", 0, &out);
	wire_reset(&expected);
	wire_u8(&expected, 80);
	wire_text(&expected, "x");
	wire_bool(&expected, 0);
	CHECK_OUTPUT(&out, &expected);

	/* put_no_more_sessions: want_reply FALSE. */
	ssh_channel_put_no_more_sessions(&out);
	wire_reset(&expected);
	wire_u8(&expected, 80);
	wire_text(&expected, "no-more-sessions@openssh.com");
	wire_bool(&expected, 0);
	CHECK_OUTPUT(&out, &expected);

	/* A failed output is reported: a buffer limited to one byte, already full, cannot take REQUEST_FAILURE. */
	ssh_buf_free(&out);
	ssh_buf_init(&out, 1, 0);
	ssh_buf_put_u8(&out, 0);
	ssh_channels_init(&channels);
	wire_reset(&input);
	wire_u8(&input, 80);
	wire_text(&input, "keepalive@openssh.com");
	wire_bool(&input, 1);
	CHECK_EQ(ssh_channel_global_request(&channels, input.data, input.size, name, &want_reply, &out), EMSGSIZE);

	ssh_buf_free(&out);
}

/*
 * Random payloads through every reader, under the sanitizers: nothing
 * reads out of bounds, the windows stay within their limits, and the
 * states stay valid.
 */
static void
test_random(void)
{
	struct ssh_channels channels;
	struct ssh_channel *channel;
	struct ssh_channel *found;
	struct ssh_buf out;
	static struct ssh_channel_request request;
	static const uint8_t numbers[] = { 80, 81, 90, 91, 92, 93, 94, 95, 96, 97, 98, 99, 100 };
	uint8_t payload[160];
	const uint8_t *bytes;
	size_t bytes_size;
	uint32_t extended;
	uint32_t reason;
	char name[SSH_NAME_MAX + 1];
	int want_reply;
	int success;
	size_t size;
	size_t index;
	unsigned round;
	unsigned slot;

	ssh_channels_init(&channels);
	ssh_buf_init(&out, 1U << 20, 0);

	/* Runs many random payloads against a table that keeps changing. */
	for (round = 0; round < 40000; round++) {
		if (round % 64 == 0) {
			ssh_channels_init(&channels);
			ssh_buf_reset(&out);
			(void)open_server_channel(&channels, &out, 1, 5000, 100);
			(void)open_server_channel(&channels, &out, 2, 0xffffff00UL, 32768);
			channel = NULL;
			(void)ssh_channel_open_session(&channels, &out, &channel);
			ssh_buf_reset(&out);
		}

		/* A payload whose first byte is usually a message number, with small numbers and a few big ones after it. */
		size = next_random() % (sizeof(payload) + 1);
		for (index = 0; index < size; index++) {
			switch (next_random() % 6) {
			case 0:
				payload[index] = 0xff;
				break;
			case 1:
				payload[index] = (uint8_t)(next_random() & 0xff);
				break;
			default:
				payload[index] = (uint8_t)(next_random() % 4);
				break;
			}
		}
		if (size > 0 && next_random() % 8 != 0)
			payload[0] = numbers[next_random() % sizeof(numbers)];

		/* Offers the payload to each reader; a reader of another message answers EINVAL. */
		(void)ssh_channel_accept_open(&channels, payload, size, &out, &channel);
		(void)ssh_channel_open_reply(&channels, payload, size, &found, &reason);
		(void)ssh_channel_parse_data(&channels, payload, size, &found, &bytes, &bytes_size, &extended);
		(void)ssh_channel_parse_window_adjust(&channels, payload, size, &found);
		(void)ssh_channel_parse_end(&channels, payload, size, &found);
		(void)ssh_channel_parse_request(&channels, payload, size, &request);
		(void)ssh_channel_parse_reply(&channels, payload, size, &found, &success);
		(void)ssh_channel_global_request(&channels, payload, size, name, &want_reply, &out);
		ssh_buf_reset(&out);

		/* The reply side moves too: close, eof, consume and send on a random slot. */
		slot = next_random() % SSH_CHANNEL_MAX;
		switch (next_random() % 4) {
		case 0:
			ssh_channel_put_close(&channels.channel[slot], &out);
			break;
		case 1:
			ssh_channel_put_eof(&channels.channel[slot], &out);
			break;
		case 2:
			ssh_channel_consumed(&channels.channel[slot], 1, &out);
			break;
		default:
			(void)ssh_channel_closed(&channels, &channels.channel[slot]);
			break;
		}
		ssh_buf_reset(&out);

		/* The invariants: states are valid, local windows are within the full window, packet sizes within the cap. */
		for (index = 0; index < SSH_CHANNEL_MAX; index++) {
			CHECK(channels.channel[index].state <= SSH_CHANNEL_CLOSING);
			CHECK(channels.channel[index].local_window <= SSH_CHANNEL_WINDOW);
			CHECK(channels.channel[index].remote_packet <= SSH_CHANNEL_PACKET);
			CHECK_EQ(channels.channel[index].local_id, index);
		}
	}

	ssh_buf_free(&out);
}

/*
 * Steps the pseudo-random generator (xorshift32) and returns the next value.
 */
static uint32_t
next_random(void)
{
	random_state ^= random_state << 13;
	random_state ^= random_state >> 17;
	random_state ^= random_state << 5;
	return random_state;
}
