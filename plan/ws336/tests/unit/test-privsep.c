/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The test of ssh-privsep.c: the frame writer is compared with the bytes
 * wire.h builds (written apart from the module), the parser reads bytes
 * wire.h built, the limits are checked at both sides of their edges, and
 * the blocking call is run against a forked peer over a socketpair.
 * Expected values come from the header's contract or from the hand
 * computation in the comment beside them.
 */

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "check.h"
#include "wire.h"
#include "ssh-privsep.h"

/* The frame wire.h builds for one comparison; static because it is large. */
static struct wire expected;

/* A second frame, for the inputs of the parser and the peer's replies. */
static struct wire input;

/* The state of the pseudo-random generator of the random runs, never zero. */
static uint32_t random_state = 2463534242UL;

/* How many times the interval timer's handler ran (set from the signal handler). */
static volatile sig_atomic_t alarm_count;

/* How long the next forked peer waits before it reads the request, in microseconds (0 for none). */
static unsigned child_start_delay;

/* One body large enough for every limit, filled with a pattern by the tests that need it. */
static uint8_t big_body[SSH_MON_STATE_MAX + 16U];

static void test_limit_values(void);
static void test_put_frame_types(void);
static void test_put_frame_limits(void);
static void test_put_frame_errors(void);
static void test_next_frame_types(void);
static void test_next_frame_incomplete(void);
static void test_next_frame_refused(void);
static void test_next_frame_sequence(void);
static void test_next_frame_secret(void);
static void test_next_frame_body_limit(void);
static void test_call_roundtrip(void);
static void test_call_pieces_and_extra(void);
static void test_call_large(void);
static void test_call_peer_closes(void);
static void test_call_interrupted(void);
static void test_call_hostile_reply(void);
static void test_call_errors(void);
static void test_random_frames(void);
static void test_mutated_frames(void);
static void build_frame(struct wire *wire, unsigned type, const uint8_t *body, size_t size);
static void child_serve(int descriptor, unsigned reply_type, const uint8_t *reply_body, size_t reply_size, int mode);
static int read_full(int descriptor, uint8_t *data, size_t size);
static void write_full(int descriptor, const uint8_t *data, size_t size);
static int finish_child(pid_t child);
static void on_alarm(int signal_number);
static uint32_t next_random(void);

/*
 * Runs the checks of the module.
 */
int
main(void)
{
	size_t index;

	/* Prints each failure as it happens, so a crash does not lose the lines before it. */
	setvbuf(stdout, NULL, _IONBF, 0);

	/* Ignores SIGPIPE, as the process that calls ssh_mon_call does. */
	signal(SIGPIPE, SIG_IGN);

	/* Fills the shared body with a pattern that is not constant. */
	for (index = 0; index < sizeof(big_body); index++)
		big_body[index] = (uint8_t)(index * 7U + (index >> 8));

	/* Runs each group of checks. */
	test_limit_values();
	test_put_frame_types();
	test_put_frame_limits();
	test_put_frame_errors();
	test_next_frame_types();
	test_next_frame_incomplete();
	test_next_frame_refused();
	test_next_frame_sequence();
	test_next_frame_secret();
	test_next_frame_body_limit();
	test_call_roundtrip();
	test_call_pieces_and_extra();
	test_call_large();
	test_call_peer_closes();
	test_call_interrupted();
	test_call_hostile_reply();
	test_call_errors();
	test_random_frames();
	test_mutated_frames();

	/* Reports the result of the whole test. */
	return check_finish("privsep");
}

/*
 * Checks the limits against the values worked out by hand from the header.
 */
static void
test_limit_values(void)
{
	/* SSH_MON_FRAME_MAX is 64 KiB: 64 * 1024 = 65536. */
	CHECK_EQ(SSH_MON_FRAME_MAX, 65536);

	/*
	 * SSH_MON_STATE_MAX is SSH_PACKET_INPUT_MAX + 64 KiB, and
	 * SSH_PACKET_INPUT_MAX is 262144 + 4 + 16 + 4096 = 266260, so the
	 * state limit is 266260 + 65536 = 331796.
	 */
	CHECK_EQ(SSH_MON_STATE_MAX, 331796);

	/* The type values are those of the table in the header. */
	CHECK_EQ(SSH_MON_SIGN, 1);
	CHECK_EQ(SSH_MON_SESSION_START, 11);
}

/*
 * Checks that every type is written as length, type and body, with the
 * bytes wire.h builds, including a body of 0 bytes.
 */
static void
test_put_frame_types(void)
{
	struct ssh_buf out;
	unsigned type;
	uint8_t body[16];
	size_t body_size;
	int error;

	/* Writes each type with a body that depends on the type. */
	for (type = 1; type <= 11; type++) {
		body_size = type;
		memset(body, (int)(0x40 + type), sizeof(body));

		/* The frame wire.h builds: the length counts the type byte and the body. */
		wire_reset(&expected);
		wire_u32(&expected, (uint32_t)(1 + body_size));
		wire_u8(&expected, type);
		wire_bytes(&expected, body, body_size);

		ssh_buf_init(&out, 1000, 0);
		error = ssh_mon_put_frame((enum ssh_mon_type)type, body, body_size, &out);
		CHECK_EQ(error, 0);
		CHECK_EQ(out.size, expected.size);
		CHECK_BYTES(out.data, expected.data, expected.size);
		ssh_buf_free(&out);
	}

	/* A body of 0 bytes is the five bytes 00 00 00 01 <type>; the body pointer may be NULL. */
	ssh_buf_init(&out, 1000, 0);
	error = ssh_mon_put_frame(SSH_MON_RESULT, NULL, 0, &out);
	CHECK_EQ(error, 0);
	CHECK_EQ(out.size, 5);
	wire_reset(&expected);
	wire_u32(&expected, 1);
	wire_u8(&expected, 9);
	CHECK_BYTES(out.data, expected.data, 5);

	/* A second frame follows the first. */
	error = ssh_mon_put_frame(SSH_MON_SIGN, (const uint8_t *)"abc", 3, &out);
	CHECK_EQ(error, 0);
	CHECK_EQ(out.size, 5 + 8);
	wire_u32(&expected, 4);
	wire_u8(&expected, 1);
	wire_bytes(&expected, "abc", 3);
	CHECK_BYTES(out.data, expected.data, expected.size);
	ssh_buf_free(&out);

	/* A body built as an RFC 4251 string (a RESULT of uint32 0) is carried unchanged. */
	wire_reset(&input);
	wire_u32(&input, 0);
	ssh_buf_init(&out, 1000, 0);
	error = ssh_mon_put_frame(SSH_MON_RESULT, input.data, input.size, &out);
	CHECK_EQ(error, 0);
	wire_hex("00000005 09 00000000", expected.data, sizeof(expected.data));
	CHECK_BYTES(out.data, expected.data, 9);
	ssh_buf_free(&out);
}

/*
 * Checks the limit of each type on both sides of its edge: the length
 * (the type byte and the body) may reach the limit and not pass it.
 */
static void
test_put_frame_limits(void)
{
	struct ssh_buf out;
	unsigned type;
	unsigned limit;
	int error;
	size_t before;

	/* Tries every type with a body one byte under, at, and one over the largest the limit allows. */
	for (type = 1; type <= 11; type++) {
		limit = SSH_MON_FRAME_MAX;
		if (type == SSH_MON_CONFIG || type == SSH_MON_AUTH_DONE || type == SSH_MON_SESSION_START)
			limit = SSH_MON_STATE_MAX;

		/* A body of limit - 1 bytes makes a length of limit: allowed. */
		ssh_buf_init(&out, 4U + SSH_MON_STATE_MAX, 0);
		error = ssh_mon_put_frame((enum ssh_mon_type)type, big_body, limit - 1, &out);
		CHECK_EQ(error, 0);
		CHECK_EQ(out.size, 4U + limit);
		wire_reset(&expected);
		wire_u32(&expected, limit);
		wire_u8(&expected, type);
		CHECK_BYTES(out.data, expected.data, 5);
		CHECK_BYTES(out.data + 5, big_body, limit - 1);
		ssh_buf_free(&out);

		/* A body of limit - 2 bytes is allowed. */
		ssh_buf_init(&out, 4U + SSH_MON_STATE_MAX, 0);
		error = ssh_mon_put_frame((enum ssh_mon_type)type, big_body, limit - 2, &out);
		CHECK_EQ(error, 0);
		ssh_buf_free(&out);

		/* A body of limit bytes makes a length of limit + 1: EMSGSIZE, and out is left as it was. */
		ssh_buf_init(&out, 4U + SSH_MON_STATE_MAX + 100U, 0);
		ssh_buf_put_u8(&out, 0x5a);
		before = out.size;
		error = ssh_mon_put_frame((enum ssh_mon_type)type, big_body, limit, &out);
		CHECK_EQ(error, EMSGSIZE);
		CHECK_EQ(out.size, before);
		CHECK_EQ(out.error, 0);

		/* The same out is still usable after the refusal. */
		error = ssh_mon_put_frame((enum ssh_mon_type)type, big_body, 1, &out);
		CHECK_EQ(error, 0);
		CHECK_EQ(out.size, before + 6);
		ssh_buf_free(&out);
	}
}

/*
 * Checks the refusals of the writer that are not about the limits.
 */
static void
test_put_frame_errors(void)
{
	struct ssh_buf out;
	int error;

	/* A type outside the enumeration is EINVAL, and nothing is written. */
	ssh_buf_init(&out, 1000, 0);
	error = ssh_mon_put_frame((enum ssh_mon_type)0, (const uint8_t *)"x", 1, &out);
	CHECK_EQ(error, EINVAL);
	error = ssh_mon_put_frame((enum ssh_mon_type)12, (const uint8_t *)"x", 1, &out);
	CHECK_EQ(error, EINVAL);
	error = ssh_mon_put_frame((enum ssh_mon_type)255, (const uint8_t *)"x", 1, &out);
	CHECK_EQ(error, EINVAL);
	error = ssh_mon_put_frame((enum ssh_mon_type)256, (const uint8_t *)"x", 1, &out);
	CHECK_EQ(error, EINVAL);
	error = ssh_mon_put_frame((enum ssh_mon_type)-1, (const uint8_t *)"x", 1, &out);
	CHECK_EQ(error, EINVAL);
	CHECK_EQ(out.size, 0);
	CHECK_EQ(out.error, 0);

	/* A missing body of a non-zero size is EINVAL. */
	error = ssh_mon_put_frame(SSH_MON_SIGN, NULL, 1, &out);
	CHECK_EQ(error, EINVAL);
	CHECK_EQ(out.size, 0);

	/* An out that is too small returns its own EMSGSIZE and leaves none of the frame (room is made first). */
	ssh_buf_free(&out);
	ssh_buf_init(&out, 8, 0);
	error = ssh_mon_put_frame(SSH_MON_SIGN, (const uint8_t *)"abcd", 4, &out);
	CHECK_EQ(error, EMSGSIZE);
	CHECK_EQ(out.size, 0);
	CHECK_EQ(out.error, EMSGSIZE);

	/* An out with an error recorded returns that error and writes nothing. */
	error = ssh_mon_put_frame(SSH_MON_SIGN, (const uint8_t *)"a", 1, &out);
	CHECK_EQ(error, EMSGSIZE);
	CHECK_EQ(out.size, 0);
	ssh_buf_free(&out);

	/* A frame that exactly fills the limit of out fits. */
	ssh_buf_init(&out, 8, 0);
	error = ssh_mon_put_frame(SSH_MON_SIGN, (const uint8_t *)"abc", 3, &out);
	CHECK_EQ(error, 0);
	CHECK_EQ(out.size, 8);
	ssh_buf_free(&out);
}

/*
 * Checks that the parser reads the frames wire.h builds, for every type,
 * and leaves the bytes after the frame.
 */
static void
test_next_frame_types(void)
{
	struct ssh_buf in;
	struct ssh_buf body;
	enum ssh_mon_type type;
	unsigned wanted;
	uint8_t pattern[40];
	int error;

	/* Reads each type with a body that depends on the type, and one byte after the frame. */
	for (wanted = 1; wanted <= 11; wanted++) {
		memset(pattern, (int)(0x10 + wanted), sizeof(pattern));
		wire_reset(&input);
		build_frame(&input, wanted, pattern, wanted * 3U);
		wire_u8(&input, 0xee);

		ssh_buf_init(&in, 1000, 0);
		ssh_buf_put_bytes(&in, input.data, input.size);
		ssh_buf_init(&body, 1000, 1);
		type = (enum ssh_mon_type)0;
		error = ssh_mon_next_frame(&in, &type, &body);
		CHECK_EQ(error, 0);
		CHECK_EQ(type, wanted);
		CHECK_EQ(body.size, wanted * 3U);
		CHECK_BYTES(body.data, pattern, wanted * 3U);

		/* Only the byte after the frame is left. */
		CHECK_EQ(in.size, 1);
		CHECK_EQ(in.data[0], 0xee);
		ssh_buf_free(&in);
		ssh_buf_free(&body);
	}

	/* A frame of length 1 has an empty body. */
	wire_reset(&input);
	wire_hex("00000001 09", input.data, sizeof(input.data));
	input.size = 5;
	ssh_buf_init(&in, 1000, 0);
	ssh_buf_put_bytes(&in, input.data, 5);
	ssh_buf_init(&body, 1000, 1);
	error = ssh_mon_next_frame(&in, &type, &body);
	CHECK_EQ(error, 0);
	CHECK_EQ(type, SSH_MON_RESULT);
	CHECK_EQ(body.size, 0);
	CHECK_EQ(in.size, 0);
	ssh_buf_free(&in);
	ssh_buf_free(&body);

	/* An empty input is incomplete, and an input with no memory is too. */
	ssh_buf_init(&in, 1000, 0);
	ssh_buf_init(&body, 1000, 1);
	error = ssh_mon_next_frame(&in, &type, &body);
	CHECK_EQ(error, EAGAIN);
	ssh_buf_free(&body);
}

/*
 * Checks that every prefix of a valid frame is incomplete and changes
 * nothing, and that the limits are applied as soon as the bytes that
 * decide them have arrived.
 */
static void
test_next_frame_incomplete(void)
{
	struct ssh_buf in;
	struct ssh_buf body;
	enum ssh_mon_type type;
	size_t prefix;
	int error;

	/* Every shorter prefix of a 6-byte-body frame answers EAGAIN, leaving the input as it was. */
	wire_reset(&input);
	build_frame(&input, SSH_MON_PASSWORD_LOGIN, (const uint8_t *)"secret", 6);
	ssh_buf_init(&body, 1000, 1);
	for (prefix = 0; prefix < input.size; prefix++) {
		ssh_buf_init(&in, 1000, 0);
		ssh_buf_put_bytes(&in, input.data, prefix);

		/* Fills the unused room with zeros, so a parser that reads past the bytes it has sees a length 0 or a type 0 and refuses. */
		if (in.data != NULL)
			memset(in.data + in.size, 0, in.capacity - in.size);
		error = ssh_mon_next_frame(&in, &type, &body);
		CHECK_EQ(error, EAGAIN);
		CHECK_EQ(in.size, prefix);
		CHECK_EQ(body.size, 0);
		ssh_buf_free(&in);
	}

	/* The whole frame is complete. */
	ssh_buf_init(&in, 1000, 0);
	ssh_buf_put_bytes(&in, input.data, input.size);
	error = ssh_mon_next_frame(&in, &type, &body);
	CHECK_EQ(error, 0);
	CHECK_EQ(type, SSH_MON_PASSWORD_LOGIN);
	CHECK_EQ(body.size, 6);
	CHECK_BYTES(body.data, "secret", 6);
	ssh_buf_free(&in);

	/* A header at the limit of SIGN (length 65536) with no body yet waits. */
	wire_reset(&input);
	wire_u32(&input, 65536);
	wire_u8(&input, SSH_MON_SIGN);
	ssh_buf_init(&in, 1000, 0);
	ssh_buf_put_bytes(&in, input.data, input.size);
	error = ssh_mon_next_frame(&in, &type, &body);
	CHECK_EQ(error, EAGAIN);
	ssh_buf_free(&in);

	/* A header at the limit of AUTH_DONE (length 331796) with no body yet waits. */
	wire_reset(&input);
	wire_u32(&input, 331796);
	wire_u8(&input, SSH_MON_AUTH_DONE);
	ssh_buf_init(&in, 1000, 0);
	ssh_buf_put_bytes(&in, input.data, input.size);
	error = ssh_mon_next_frame(&in, &type, &body);
	CHECK_EQ(error, EAGAIN);
	ssh_buf_free(&in);
	ssh_buf_free(&body);
}

/*
 * Checks the frames the parser refuses: no type byte, an unknown type, a
 * length over the limit of its type, and the earliest moment each is
 * known.
 */
static void
test_next_frame_refused(void)
{
	struct ssh_buf in;
	struct ssh_buf body;
	enum ssh_mon_type type;
	unsigned bad_type;
	unsigned wanted;
	int error;

	ssh_buf_init(&body, 1000, 1);

	/* A length of 0 is EINVAL as soon as its four bytes are there. */
	ssh_buf_init(&in, 1000, 0);
	ssh_buf_put_bytes(&in, "\0\0\0\0", 4);
	error = ssh_mon_next_frame(&in, &type, &body);
	CHECK_EQ(error, EINVAL);
	CHECK_EQ(in.size, 4);
	ssh_buf_put_bytes(&in, "\1\1\1\1\1\1", 6);
	error = ssh_mon_next_frame(&in, &type, &body);
	CHECK_EQ(error, EINVAL);
	ssh_buf_free(&in);

	/* A length over the largest limit of any type (331796 + 1, and 0xffffffff) is EINVAL from four bytes. */
	wire_reset(&input);
	wire_u32(&input, 331797);
	ssh_buf_init(&in, 1000, 0);
	ssh_buf_put_bytes(&in, input.data, 4);
	error = ssh_mon_next_frame(&in, &type, &body);
	CHECK_EQ(error, EINVAL);
	ssh_buf_free(&in);
	wire_reset(&input);
	wire_u32(&input, 0xffffffffUL);
	ssh_buf_init(&in, 1000, 0);
	ssh_buf_put_bytes(&in, input.data, 4);
	error = ssh_mon_next_frame(&in, &type, &body);
	CHECK_EQ(error, EINVAL);
	ssh_buf_free(&in);

	/* The byte after the largest length, 0x80000000, is a length too: no wrap to a small value. */
	wire_reset(&input);
	wire_u32(&input, 0x80000000UL);
	wire_u8(&input, SSH_MON_AUTH_DONE);
	ssh_buf_init(&in, 1000, 0);
	ssh_buf_put_bytes(&in, input.data, input.size);
	error = ssh_mon_next_frame(&in, &type, &body);
	CHECK_EQ(error, EINVAL);
	ssh_buf_free(&in);

	/* A type byte outside 1..11 is EINVAL as soon as the type byte is there, with a short body. */
	for (bad_type = 0; bad_type < 256; bad_type++) {
		if (bad_type >= 1 && bad_type <= 11)
			continue;
		wire_reset(&input);
		wire_u32(&input, 10);
		wire_u8(&input, bad_type);
		ssh_buf_init(&in, 1000, 0);
		ssh_buf_put_bytes(&in, input.data, input.size);
		error = ssh_mon_next_frame(&in, &type, &body);
		CHECK_EQ(error, EINVAL);
		CHECK_EQ(in.size, 5);
		ssh_buf_free(&in);
	}

	/* Every type other than the three large ones is refused at length 65537 and accepted as far as a header at 65536. */
	for (wanted = 1; wanted <= 11; wanted++) {
		wire_reset(&input);
		wire_u32(&input, 65537);
		wire_u8(&input, wanted);
		ssh_buf_init(&in, 1000, 0);
		ssh_buf_put_bytes(&in, input.data, input.size);
		error = ssh_mon_next_frame(&in, &type, &body);
		if (wanted == SSH_MON_CONFIG || wanted == SSH_MON_AUTH_DONE || wanted == SSH_MON_SESSION_START)
			CHECK_EQ(error, EAGAIN);
		else
			CHECK_EQ(error, EINVAL);
		ssh_buf_free(&in);

		/* One over the largest limit is refused for every type (the length itself is refused). */
		wire_reset(&input);
		wire_u32(&input, 331797);
		wire_u8(&input, wanted);
		ssh_buf_init(&in, 1000, 0);
		ssh_buf_put_bytes(&in, input.data, input.size);
		error = ssh_mon_next_frame(&in, &type, &body);
		CHECK_EQ(error, EINVAL);
		ssh_buf_free(&in);
	}

	/* A complete frame at exactly the limit of SIGN is accepted, one byte more is not. */
	wire_reset(&input);
	wire_u32(&input, 65536);
	wire_u8(&input, SSH_MON_SIGN);
	wire_bytes(&input, big_body, 65535);
	ssh_buf_init(&in, 100000, 0);
	ssh_buf_put_bytes(&in, input.data, input.size);
	error = ssh_mon_next_frame(&in, &type, &body);
	CHECK_EQ(error, EMSGSIZE);
	ssh_buf_free(&in);
	ssh_buf_free(&body);

	ssh_buf_init(&body, 100000, 1);
	ssh_buf_init(&in, 100000, 0);
	ssh_buf_put_bytes(&in, input.data, input.size);
	error = ssh_mon_next_frame(&in, &type, &body);
	CHECK_EQ(error, 0);
	CHECK_EQ(body.size, 65535);
	CHECK_BYTES(body.data, big_body, 65535);
	CHECK_EQ(in.size, 0);
	ssh_buf_free(&in);
	ssh_buf_free(&body);
}

/*
 * Checks two frames in a row, and a stream taken apart one byte at a time.
 */
static void
test_next_frame_sequence(void)
{
	struct ssh_buf in;
	struct ssh_buf body;
	enum ssh_mon_type type;
	size_t index;
	int error;
	int frames;

	/* Two frames in one input come out one after the other. */
	wire_reset(&input);
	build_frame(&input, SSH_MON_PUBKEY_QUERY, (const uint8_t *)"alice", 5);
	build_frame(&input, SSH_MON_RESULT, (const uint8_t *)"\0\0\0\0", 4);
	ssh_buf_init(&in, 1000, 0);
	ssh_buf_put_bytes(&in, input.data, input.size);
	ssh_buf_init(&body, 1000, 1);

	error = ssh_mon_next_frame(&in, &type, &body);
	CHECK_EQ(error, 0);
	CHECK_EQ(type, SSH_MON_PUBKEY_QUERY);
	CHECK_EQ(body.size, 5);
	CHECK_BYTES(body.data, "alice", 5);
	CHECK_EQ(in.size, 9);

	error = ssh_mon_next_frame(&in, &type, &body);
	CHECK_EQ(error, 0);
	CHECK_EQ(type, SSH_MON_RESULT);
	CHECK_EQ(body.size, 4);
	CHECK_BYTES(body.data, "\0\0\0\0", 4);
	CHECK_EQ(in.size, 0);

	/* Nothing is left, so the next call waits. */
	error = ssh_mon_next_frame(&in, &type, &body);
	CHECK_EQ(error, EAGAIN);
	ssh_buf_free(&in);

	/* The same two frames fed one byte at a time give the same two frames. */
	ssh_buf_init(&in, 1000, 0);
	frames = 0;
	for (index = 0; index < input.size; index++) {
		ssh_buf_put_u8(&in, input.data[index]);
		error = ssh_mon_next_frame(&in, &type, &body);
		if (error == 0) {
			frames++;
			if (frames == 1) {
				CHECK_EQ(type, SSH_MON_PUBKEY_QUERY);
				CHECK_EQ(index, 9);
			} else {
				CHECK_EQ(type, SSH_MON_RESULT);
				CHECK_EQ(index, 18);
			}
		} else {
			CHECK_EQ(error, EAGAIN);
		}
	}
	CHECK_EQ(frames, 2);
	ssh_buf_free(&in);
	ssh_buf_free(&body);
}

/*
 * Checks that the body is a secret buffer whatever the caller passed, that
 * a body left from an earlier frame does not accumulate, and that a
 * secret input is wiped where the frame was.
 */
static void
test_next_frame_secret(void)
{
	struct ssh_buf in;
	struct ssh_buf body;
	enum ssh_mon_type type;
	int error;

	/* A body passed as non-secret is made secret. */
	wire_reset(&input);
	build_frame(&input, SSH_MON_PASSWORD_LOGIN, (const uint8_t *)"hunter2", 7);
	ssh_buf_init(&in, 1000, 1);
	ssh_buf_put_bytes(&in, input.data, input.size);
	ssh_buf_init(&body, 1000, 0);
	CHECK_EQ(body.secret, 0);
	error = ssh_mon_next_frame(&in, &type, &body);
	CHECK_EQ(error, 0);
	CHECK_EQ(body.secret, 1);
	CHECK_BYTES(body.data, "hunter2", 7);

	/*
	 * The consumed frame is gone from a secret input.  The wipe of its
	 * memory is ssh_wipe's, which the crypto module provides; this test
	 * does not look at the memory.
	 */
	CHECK_EQ(in.size, 0);
	ssh_buf_free(&in);

	/* Reading a second frame into the same body replaces the first body. */
	wire_reset(&input);
	build_frame(&input, SSH_MON_PUBKEY_QUERY, (const uint8_t *)"bob", 3);
	ssh_buf_init(&in, 1000, 1);
	ssh_buf_put_bytes(&in, input.data, input.size);
	error = ssh_mon_next_frame(&in, &type, &body);
	CHECK_EQ(error, 0);
	CHECK_EQ(body.size, 3);
	CHECK_BYTES(body.data, "bob", 3);
	ssh_buf_free(&in);

	/* A frame that is not yet complete empties the body (no old secret stays in it). */
	ssh_buf_init(&in, 1000, 1);
	error = ssh_mon_next_frame(&in, &type, &body);
	CHECK_EQ(error, EAGAIN);
	CHECK_EQ(body.size, 0);
	ssh_buf_free(&in);
	ssh_buf_free(&body);
}

/*
 * Checks that a body buffer whose limit is smaller than the body returns
 * its error, leaves the input untouched and the body empty.
 */
static void
test_next_frame_body_limit(void)
{
	struct ssh_buf in;
	struct ssh_buf body;
	enum ssh_mon_type type;
	int error;

	/* A body limit of 4 and a body of 5 bytes: EMSGSIZE; the frame stays in the input. */
	wire_reset(&input);
	build_frame(&input, SSH_MON_SIGN, (const uint8_t *)"12345", 5);
	ssh_buf_init(&in, 1000, 0);
	ssh_buf_put_bytes(&in, input.data, input.size);
	ssh_buf_init(&body, 4, 1);
	error = ssh_mon_next_frame(&in, &type, &body);
	CHECK_EQ(error, EMSGSIZE);
	CHECK_EQ(in.size, input.size);
	CHECK_BYTES(in.data, input.data, input.size);
	CHECK_EQ(body.size, 0);

	/* The buffer is usable again (the error was cleared) and a body of exactly 4 bytes fits. */
	ssh_buf_free(&in);
	wire_reset(&input);
	build_frame(&input, SSH_MON_SIGN, (const uint8_t *)"1234", 4);
	ssh_buf_init(&in, 1000, 0);
	ssh_buf_put_bytes(&in, input.data, input.size);
	error = ssh_mon_next_frame(&in, &type, &body);
	CHECK_EQ(error, 0);
	CHECK_EQ(body.size, 4);
	ssh_buf_free(&in);
	ssh_buf_free(&body);
}

/*
 * Checks one request and reply over a socketpair against a forked peer
 * that compares the request with the bytes wire.h builds.
 */
static void
test_call_roundtrip(void)
{
	int pair[2];
	pid_t child;
	struct ssh_buf reply;
	enum ssh_mon_type reply_type;
	int error;

	/* The request is a PASSWORD_LOGIN of two strings, the reply a RESULT of uint32 13 (EACCES). */
	wire_reset(&input);
	wire_text(&input, "alice");
	wire_text(&input, "pa55word");
	wire_reset(&expected);
	build_frame(&expected, SSH_MON_PASSWORD_LOGIN, input.data, input.size);

	/* The peer expects exactly those bytes and answers with the frame of the RESULT. */
	CHECK_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
	child = fork();
	if (child == 0) {
		close(pair[0]);
		child_serve(pair[1], SSH_MON_RESULT, (const uint8_t *)"\0\0\0\15", 4, 0);
	}
	close(pair[1]);

	ssh_buf_init(&reply, 1000, 1);
	reply_type = (enum ssh_mon_type)0;
	error = ssh_mon_call(pair[0], SSH_MON_PASSWORD_LOGIN, input.data, input.size, &reply_type, &reply);
	CHECK_EQ(error, 0);
	CHECK_EQ(reply_type, SSH_MON_RESULT);
	CHECK_EQ(reply.size, 4);
	CHECK_BYTES(reply.data, "\0\0\0\15", 4);
	ssh_buf_free(&reply);
	close(pair[0]);
	CHECK_EQ(finish_child(child), 0);
}

/*
 * Checks a reply that arrives in pieces, and that the bytes after the
 * reply frame stay on the descriptor for the next reader.
 */
static void
test_call_pieces_and_extra(void)
{
	int pair[2];
	pid_t child;
	struct ssh_buf reply;
	enum ssh_mon_type reply_type;
	uint8_t after[9];
	int error;
	int status;

	/* The request is a SIGN of 32 bytes. */
	wire_reset(&input);
	wire_bytes(&input, big_body, 32);
	wire_reset(&expected);
	build_frame(&expected, SSH_MON_SIGN, input.data, input.size);

	/* Mode 1: the peer writes the reply one byte at a time with pauses, then a second frame at once. */
	CHECK_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
	child = fork();
	if (child == 0) {
		close(pair[0]);
		child_serve(pair[1], SSH_MON_SIGNATURE, big_body, 100, 1);
	}
	close(pair[1]);

	ssh_buf_init(&reply, 1000, 1);
	error = ssh_mon_call(pair[0], SSH_MON_SIGN, input.data, input.size, &reply_type, &reply);
	CHECK_EQ(error, 0);
	CHECK_EQ(reply_type, SSH_MON_SIGNATURE);
	CHECK_EQ(reply.size, 100);
	CHECK_BYTES(reply.data, big_body, 100);

	/* The second frame (00 00 00 05 09 00 00 00 00) was not consumed by the call. */
	status = read_full(pair[0], after, sizeof(after));
	CHECK_EQ(status, 0);
	CHECK_BYTES(after, "\0\0\0\5\11\0\0\0\0", 9);
	ssh_buf_free(&reply);
	close(pair[0]);
	CHECK_EQ(finish_child(child), 0);
}

/*
 * Checks a request and a reply of the size of an exported transport, which
 * need many reads and writes.
 */
static void
test_call_large(void)
{
	int pair[2];
	pid_t child;
	struct ssh_buf reply;
	enum ssh_mon_type reply_type;
	struct sigaction action;
	struct sigaction old_action;
	struct itimerval timer;
	struct itimerval stop;
	int error;

	/*
	 * The request is an AUTH_DONE with a body of 250000 bytes (wire.h
	 * holds 266240), the reply a CONFIG of 331795 bytes, the largest
	 * a frame of the state limit can carry (length 331796).
	 */
	wire_reset(&expected);
	build_frame(&expected, SSH_MON_AUTH_DONE, big_body, 250000);

	/*
	 * The peer waits 300 ms before reading, so the 250000-byte write fills
	 * the socket and blocks; a timer signal every 5 ms (a handler without
	 * SA_RESTART) interrupts it, which gives a short write that the call
	 * must continue from where it stopped.
	 */
	memset(&action, 0, sizeof(action));
	action.sa_handler = on_alarm;
	sigemptyset(&action.sa_mask);
	action.sa_flags = 0;
	CHECK_EQ(sigaction(SIGALRM, &action, &old_action), 0);
	alarm_count = 0;
	child_start_delay = 300000;

	CHECK_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
	child = fork();
	if (child == 0) {
		close(pair[0]);
		child_serve(pair[1], SSH_MON_CONFIG, big_body, SSH_MON_STATE_MAX - 1U, 0);
	}
	close(pair[1]);
	child_start_delay = 0;

	memset(&timer, 0, sizeof(timer));
	timer.it_interval.tv_usec = 5000;
	timer.it_value.tv_usec = 5000;
	CHECK_EQ(setitimer(ITIMER_REAL, &timer, NULL), 0);

	ssh_buf_init(&reply, SSH_MON_STATE_MAX, 1);
	error = ssh_mon_call(pair[0], SSH_MON_AUTH_DONE, big_body, 250000, &reply_type, &reply);

	memset(&stop, 0, sizeof(stop));
	CHECK_EQ(setitimer(ITIMER_REAL, &stop, NULL), 0);
	CHECK_EQ(sigaction(SIGALRM, &old_action, NULL), 0);

	CHECK_EQ(error, 0);
	CHECK_EQ(reply_type, SSH_MON_CONFIG);
	CHECK_EQ(reply.size, SSH_MON_STATE_MAX - 1U);
	if (reply.data != NULL)
		CHECK_BYTES(reply.data, big_body, SSH_MON_STATE_MAX - 1U);

	/* The signals did arrive during the call. */
	CHECK(alarm_count >= 3);
	ssh_buf_free(&reply);
	close(pair[0]);
	CHECK_EQ(finish_child(child), 0);
}

/*
 * Checks the failures when the other side closes: before the reply, in the
 * middle of the reply, and before the request was sent.
 */
static void
test_call_peer_closes(void)
{
	int pair[2];
	pid_t child;
	struct ssh_buf reply;
	enum ssh_mon_type reply_type;
	int error;

	/* Mode 2: the peer reads the request and closes without a reply. */
	wire_reset(&input);
	wire_text(&input, "bob");
	wire_reset(&expected);
	build_frame(&expected, SSH_MON_PUBKEY_QUERY, input.data, input.size);
	CHECK_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
	child = fork();
	if (child == 0) {
		close(pair[0]);
		child_serve(pair[1], 0, NULL, 0, 2);
	}
	close(pair[1]);
	ssh_buf_init(&reply, 1000, 1);
	error = ssh_mon_call(pair[0], SSH_MON_PUBKEY_QUERY, input.data, input.size, &reply_type, &reply);
	CHECK_EQ(error, EIO);
	ssh_buf_free(&reply);
	close(pair[0]);
	CHECK_EQ(finish_child(child), 0);

	/* Mode 3: the peer reads the request, writes 7 bytes of a reply of 20, and closes. */
	CHECK_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
	child = fork();
	if (child == 0) {
		close(pair[0]);
		child_serve(pair[1], SSH_MON_RESULT, big_body, 15, 3);
	}
	close(pair[1]);
	ssh_buf_init(&reply, 1000, 1);
	error = ssh_mon_call(pair[0], SSH_MON_PUBKEY_QUERY, input.data, input.size, &reply_type, &reply);
	CHECK_EQ(error, EIO);
	CHECK_EQ(reply.size, 0);
	ssh_buf_free(&reply);
	close(pair[0]);
	CHECK_EQ(finish_child(child), 0);

	/* The peer's end is closed before the request: the write fails (SIGPIPE is ignored) with EIO. */
	CHECK_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
	close(pair[1]);
	ssh_buf_init(&reply, 1000, 1);
	error = ssh_mon_call(pair[0], SSH_MON_PUBKEY_QUERY, input.data, input.size, &reply_type, &reply);
	CHECK_EQ(error, EIO);
	ssh_buf_free(&reply);
	close(pair[0]);
}

/*
 * Checks that a signal that interrupts the blocking read does not end the
 * call: the peer waits 300 ms, an interval timer fires every 20 ms with a
 * handler installed without SA_RESTART.
 */
static void
test_call_interrupted(void)
{
	int pair[2];
	pid_t child;
	struct ssh_buf reply;
	enum ssh_mon_type reply_type;
	struct sigaction action;
	struct sigaction old_action;
	struct itimerval timer;
	struct itimerval stop;
	int error;

	wire_reset(&input);
	wire_text(&input, "carol");
	wire_reset(&expected);
	build_frame(&expected, SSH_MON_PUBKEY_QUERY, input.data, input.size);

	/* Installs the handler without SA_RESTART, so a blocked read returns EINTR. */
	memset(&action, 0, sizeof(action));
	action.sa_handler = on_alarm;
	sigemptyset(&action.sa_mask);
	action.sa_flags = 0;
	CHECK_EQ(sigaction(SIGALRM, &action, &old_action), 0);
	alarm_count = 0;

	/* Mode 4: the peer pauses 300 ms after reading the request, then answers. */
	CHECK_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
	child = fork();
	if (child == 0) {
		close(pair[0]);
		child_serve(pair[1], SSH_MON_RESULT, (const uint8_t *)"\0\0\0\0", 4, 4);
	}
	close(pair[1]);

	/* Starts the timer in the parent only (a child made before this does not inherit it). */
	memset(&timer, 0, sizeof(timer));
	timer.it_interval.tv_usec = 20000;
	timer.it_value.tv_usec = 20000;
	CHECK_EQ(setitimer(ITIMER_REAL, &timer, NULL), 0);

	ssh_buf_init(&reply, 1000, 1);
	error = ssh_mon_call(pair[0], SSH_MON_PUBKEY_QUERY, input.data, input.size, &reply_type, &reply);

	/* Stops the timer and restores the handler. */
	memset(&stop, 0, sizeof(stop));
	CHECK_EQ(setitimer(ITIMER_REAL, &stop, NULL), 0);
	CHECK_EQ(sigaction(SIGALRM, &old_action, NULL), 0);

	CHECK_EQ(error, 0);
	CHECK_EQ(reply_type, SSH_MON_RESULT);
	CHECK_EQ(reply.size, 4);

	/* The signal did arrive during the call (at 20 ms steps over 300 ms, many times; at least 3 to be sure). */
	CHECK(alarm_count >= 3);
	ssh_buf_free(&reply);
	close(pair[0]);
	CHECK_EQ(finish_child(child), 0);
}

/*
 * Checks replies a hostile peer could send: a length of 0, an unknown
 * type, a length over the limit of its type.  The call refuses them
 * without waiting for a body that never comes.
 */
static void
test_call_hostile_reply(void)
{
	/*
	 * The replies as hex.
	 *
	 * Values referenced:
	 *   a frame's length is a uint32 that counts the type byte and the body;
	 *   0x00010001 = 65537 is one over the limit of the types other than
	 *   CONFIG, AUTH_DONE and SESSION_START (65536);
	 *   0x00051015 = 5 * 65536 + 4117 = 331797 is one over the largest limit.
	 */
	static const char *const replies[] = {
		"00000000",		/* length 0 and then more bytes */
		"00000003 00 0102",	/* type 0 */
		"00000003 0c 0102",	/* type 12 */
		"00000003 ff 0102",	/* type 255 */
		"00010001 01",		/* a SIGN-type reply of length 65537, whose body never comes */
		"00051015 01",		/* length 331797 */
		"ffffffff 0b"		/* the largest length */
	};
	int pair[2];
	pid_t child;
	struct ssh_buf reply;
	enum ssh_mon_type reply_type;
	uint8_t raw[64];
	size_t index;
	size_t count;
	int error;

	wire_reset(&input);
	wire_text(&input, "dave");
	wire_reset(&expected);
	build_frame(&expected, SSH_MON_PUBKEY_QUERY, input.data, input.size);

	/* Mode 5: the peer reads the request, writes the count raw bytes of the reply as they are, then waits for the caller to close. */
	for (index = 0; index < sizeof(replies) / sizeof(replies[0]); index++) {
		count = wire_hex(replies[index], raw, sizeof(raw));
		CHECK(count > 0);

		CHECK_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
		child = fork();
		if (child == 0) {
			close(pair[0]);
			child_serve(pair[1], 0, raw, count, 5);
		}
		close(pair[1]);

		ssh_buf_init(&reply, 1000, 1);
		error = ssh_mon_call(pair[0], SSH_MON_PUBKEY_QUERY, input.data, input.size, &reply_type, &reply);
		CHECK_EQ(error, EINVAL);
		CHECK_EQ(reply.size, 0);
		ssh_buf_free(&reply);
		close(pair[0]);
		CHECK_EQ(finish_child(child), 0);
	}
}

/*
 * Checks the refusals before anything is sent, and the reply buffer's own
 * limit.
 */
static void
test_call_errors(void)
{
	int pair[2];
	pid_t child;
	struct ssh_buf reply;
	enum ssh_mon_type reply_type;
	int error;

	/*
	 * A request over the limit of its type is EMSGSIZE before anything is
	 * written: the peer's end is already closed, so a write would give EIO
	 * (and an accepted request that no one answers would not hang).
	 */
	CHECK_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
	close(pair[1]);
	ssh_buf_init(&reply, 1000, 1);
	error = ssh_mon_call(pair[0], SSH_MON_SIGN, big_body, 65536, &reply_type, &reply);
	CHECK_EQ(error, EMSGSIZE);
	close(pair[0]);

	/* A request of an unknown type is EINVAL and sends nothing. */
	CHECK_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
	close(pair[1]);
	error = ssh_mon_call(pair[0], (enum ssh_mon_type)0, NULL, 0, &reply_type, &reply);
	CHECK_EQ(error, EINVAL);
	close(pair[0]);

	/* A descriptor that is not open fails with EIO. */
	error = ssh_mon_call(-1, SSH_MON_SIGN, (const uint8_t *)"x", 1, &reply_type, &reply);
	CHECK_EQ(error, EIO);
	ssh_buf_free(&reply);

	/* A reply buffer with a limit of 3 and a reply body of 4 bytes: the buffer's EMSGSIZE. */
	wire_reset(&input);
	wire_text(&input, "x");
	wire_reset(&expected);
	build_frame(&expected, SSH_MON_PUBKEY_QUERY, input.data, input.size);
	CHECK_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
	child = fork();
	if (child == 0) {
		close(pair[0]);
		child_serve(pair[1], SSH_MON_RESULT, (const uint8_t *)"\0\0\0\0", 4, 0);
	}
	close(pair[1]);
	ssh_buf_init(&reply, 3, 1);
	error = ssh_mon_call(pair[0], SSH_MON_PUBKEY_QUERY, input.data, input.size, &reply_type, &reply);
	CHECK_EQ(error, EMSGSIZE);
	CHECK_EQ(reply.size, 0);
	ssh_buf_free(&reply);
	close(pair[0]);
	CHECK_EQ(finish_child(child), 0);
}

/*
 * Runs the parser over a hundred thousand and more random byte strings,
 * of lengths from 0 to 40, many starting like a frame: it must end, never
 * touch memory outside the buffers, and on a failure leave the input
 * alone and the body empty.
 */
static void
test_random_frames(void)
{
	struct ssh_buf in;
	struct ssh_buf body;
	enum ssh_mon_type type;
	uint8_t bytes[48];
	size_t count;
	size_t index;
	size_t before;
	uint32_t round;
	uint32_t shape;
	int error;
	int steps;
	int bad;
	int accepted;
	int refused;
	int incomplete;

	bad = 0;
	accepted = 0;
	refused = 0;
	incomplete = 0;
	ssh_buf_init(&body, 100, 1);

	/* Tries many inputs. */
	for (round = 0; round < 150000; round++) {
		count = next_random() % 41;
		for (index = 0; index < count; index++)
			bytes[index] = (uint8_t)next_random();

		/* A third of the inputs get a plausible header: a small length and a type in 0..13. */
		shape = next_random() % 3;
		if (shape == 0 && count >= 5) {
			bytes[0] = 0;
			bytes[1] = 0;
			bytes[2] = 0;
			bytes[3] = (uint8_t)(next_random() % 12);
			bytes[4] = (uint8_t)(next_random() % 14);
		}

		ssh_buf_init(&in, 1000, (int)(round % 2));
		ssh_buf_put_bytes(&in, bytes, count);

		/* Takes frames until one fails, bounded by the input size. */
		steps = 0;
		for (;;) {
			before = in.size;
			error = ssh_mon_next_frame(&in, &type, &body);
			if (error != 0) {
				/* A failure leaves the input and empties the body. */
				if (in.size != before || body.size != 0)
					bad++;
				if (error != EAGAIN && error != EINVAL && error != EMSGSIZE)
					bad++;
				if (error == EAGAIN)
					incomplete++;
				else
					refused++;
				break;
			}

			/* A success consumed at least five bytes and gave a type of the enumeration. */
			if (in.size >= before || before - in.size < 5)
				bad++;
			if ((int)type < 1 || (int)type > 11)
				bad++;
			accepted++;
			steps++;
			if (steps > 40) {
				bad++;
				break;
			}
		}
		ssh_buf_free(&in);
	}
	CHECK_EQ(bad, 0);

	/* The inputs covered all three outcomes, so the run was not all one kind. */
	CHECK(accepted > 1000);
	CHECK(refused > 1000);
	CHECK(incomplete > 1000);
	ssh_buf_free(&body);
}

/*
 * Mutates valid frames (one byte changed, truncated, doubled) and checks
 * that the parser answers the same as for any other input: a frame, EAGAIN
 * or EINVAL, never a crash.
 */
static void
test_mutated_frames(void)
{
	struct ssh_buf in;
	struct ssh_buf body;
	enum ssh_mon_type type;
	uint8_t frame[64];
	size_t frame_size;
	size_t position;
	size_t cut;
	uint32_t round;
	int error;
	int bad;

	bad = 0;
	ssh_buf_init(&body, 1000, 1);

	/* A valid frame of 30 body bytes: 00 00 00 1f 04 <30 bytes>. */
	wire_reset(&input);
	build_frame(&input, SSH_MON_PUBKEY_LOGIN, big_body, 30);
	frame_size = input.size;
	CHECK_EQ(frame_size, 35);
	memcpy(frame, input.data, frame_size);

	/* Changes each byte to each of 256 values, and cuts the frame at each place. */
	for (round = 0; round < 35U * 256U; round++) {
		memcpy(frame, input.data, frame_size);
		position = round / 256U;
		frame[position] = (uint8_t)(round % 256U);
		cut = (size_t)(next_random() % (frame_size + 1));

		ssh_buf_init(&in, 1000, 0);
		ssh_buf_put_bytes(&in, frame, cut);
		error = ssh_mon_next_frame(&in, &type, &body);
		if (error != 0 && error != EAGAIN && error != EINVAL)
			bad++;

		/* The whole mutated frame: when accepted, the body is as long as the mutated length says less the type byte. */
		ssh_buf_reset(&in);
		ssh_buf_put_bytes(&in, frame, frame_size);
		error = ssh_mon_next_frame(&in, &type, &body);
		if (error != 0 && error != EINVAL && error != EAGAIN)
			bad++;
		if (error == 0 && (size_t)body.size + 1 != (size_t)(((uint32_t)frame[0] << 24) | ((uint32_t)frame[1] << 16) |
		    ((uint32_t)frame[2] << 8) | (uint32_t)frame[3]))
			bad++;

		/* A change inside the body never makes the frame fail, and keeps its type. */
		if (position >= 5 && (error != 0 || body.size != 30 || type != SSH_MON_PUBKEY_LOGIN))
			bad++;

		ssh_buf_free(&in);
	}
	CHECK_EQ(bad, 0);
	ssh_buf_free(&body);
}

/*
 * Appends the frame of type, body and size to wire, built with wire.h.
 */
static void
build_frame(
	struct wire *wire,
	unsigned type,
	const uint8_t *body,
	size_t size)
{
	/* The length counts the type byte and the body. */
	wire_u32(wire, (uint32_t)(size + 1));
	wire_u8(wire, type);
	if (size > 0)
		wire_bytes(wire, body, size);
}

/*
 * The forked peer of a call test; does not return.  It reads
 * expected.size bytes, compares them with expected, and then acts by mode:
 *   0  writes the reply frame at once;
 *   1  writes the reply frame one byte at a time for its first 7 bytes
 *      with a pause after each, then the rest, then a second frame of
 *      type RESULT with the body 00 00 00 00;
 *   2  closes;
 *   3  writes the first 7 bytes of the reply frame and closes;
 *   4  pauses 300 ms, then writes the reply frame at once;
 *   5  writes reply_size raw bytes from reply_body (no frame) and keeps
 *      the descriptor open for 500 ms.
 * The exit status is 0 when the request matched.
 */
static void
child_serve(
	int descriptor,
	unsigned reply_type,
	const uint8_t *reply_body,
	size_t reply_size,
	int mode)
{
	static struct wire reply;
	static uint8_t request[SSH_MON_STATE_MAX + 64U];
	size_t index;
	int status;

	/* Ends the peer by SIGALRM after 20 s if it is stuck, so a fault of the module in the test shows as a failure and not a hang. */
	signal(SIGALRM, SIG_DFL);
	alarm(20);

	/* Waits before reading when the test asked for it, so the caller's write fills the socket and blocks. */
	if (child_start_delay != 0)
		usleep(child_start_delay);

	/* Reads the request of the exact size expected and compares it. */
	status = read_full(descriptor, request, expected.size);
	if (status != 0)
		_exit(10);
	if (memcmp(request, expected.data, expected.size) != 0)
		_exit(11);

	/* A large reply (mode 0 only) is written as its header and then its body, since wire.h cannot hold it. */
	if (mode == 0 && reply_size > 100000) {
		wire_reset(&reply);
		wire_u32(&reply, (uint32_t)(reply_size + 1));
		wire_u8(&reply, reply_type);
		write_full(descriptor, reply.data, reply.size);
		write_full(descriptor, reply_body, reply_size);
		close(descriptor);
		_exit(0);
	}

	/* Builds the reply frame. */
	wire_reset(&reply);
	if (mode == 5)
		wire_bytes(&reply, reply_body, reply_size);
	else
		build_frame(&reply, reply_type, reply_body, reply_size);

	/* Acts by mode. */
	if (mode == 0) {
		write_full(descriptor, reply.data, reply.size);
	} else if (mode == 1) {
		for (index = 0; index < 7; index++) {
			write_full(descriptor, reply.data + index, 1);
			usleep(2000);
		}
		write_full(descriptor, reply.data + 7, reply.size - 7);
		wire_reset(&reply);
		build_frame(&reply, SSH_MON_RESULT, (const uint8_t *)"\0\0\0\0", 4);
		write_full(descriptor, reply.data, reply.size);
	} else if (mode == 3) {
		write_full(descriptor, reply.data, 7);
	} else if (mode == 4) {
		usleep(300000);
		write_full(descriptor, reply.data, reply.size);
	} else if (mode == 5) {
		write_full(descriptor, reply.data, reply.size);

		/* Holds the descriptor open for a while: a caller that refuses the reply returns at once, one that waits for more waits in vain. */
		usleep(500000);
	}

	/* Closes and ends with success (mode 2 does nothing else). */
	close(descriptor);
	_exit(0);
}

/*
 * Reads exactly size bytes from descriptor, retrying; returns 0, or 1 at
 * the end of the stream or on a failure.
 */
static int
read_full(
	int descriptor,
	uint8_t *data,
	size_t size)
{
	size_t done;
	ssize_t got;

	/* Reads until all have come. */
	done = 0;
	while (done < size) {
		got = read(descriptor, data + done, size - done);
		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0)
			return 1;
		done += (size_t)got;
	}

	/* Succeeded. */
	return 0;
}

/*
 * Writes exactly size bytes to descriptor in the peer, ending the peer with
 * status 12 on a failure.
 */
static void
write_full(
	int descriptor,
	const uint8_t *data,
	size_t size)
{
	size_t done;
	ssize_t written;

	/* Writes until all have gone. */
	done = 0;
	while (done < size) {
		written = write(descriptor, data + done, size - done);
		if (written < 0 && errno == EINTR)
			continue;
		if (written <= 0)
			_exit(12);
		done += (size_t)written;
	}
}

/*
 * Waits for the forked peer and returns its exit status, or 100 when it
 * did not exit normally.
 */
static int
finish_child(
	pid_t child)
{
	int status;
	pid_t waited;

	/* Waits for the peer to end. */
	waited = waitpid(child, &status, 0);
	if (waited != child)
		return 101;
	if (!WIFEXITED(status))
		return 100;

	/* Reports the status the peer exited with. */
	return WEXITSTATUS(status);
}

/*
 * Counts a SIGALRM.
 */
static void
on_alarm(
	int signal_number)
{
	/* Counts the signal. */
	(void)signal_number;
	alarm_count++;
}

/*
 * Returns the next pseudo-random number (xorshift32; never zero).
 */
static uint32_t
next_random(void)
{
	/* Mixes the state. */
	random_state ^= random_state << 13;
	random_state ^= random_state >> 17;
	random_state ^= random_state << 5;
	return random_state;
}
