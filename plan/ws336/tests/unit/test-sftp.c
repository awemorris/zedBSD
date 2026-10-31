/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The test of ssh-sftp.c.  Requests are built with wire.h (written apart
 * from the module) and read back; replies are written by the module and
 * compared with bytes wire.h builds; the long name is compared with
 * strings written by hand.  Expected values come from
 * draft-ietf-secsh-filexfer-02 (the packet layouts of sections 3 to 7, the
 * longname format of section 7 with its example line), the header's
 * contract, or the hand computation in the comment beside them.  Every
 * packet is also parsed from a heap block of exactly its size so that the
 * sanitizers see a read past the end, and random and mutated packets run
 * through every function.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "wire.h"
#include "ssh-sftp.h"

/* The request being built; static because a wire is large. */
static struct wire request_wire;

/* The expected bytes of a reply or of a stream; static because a wire is large. */
static struct wire expected;

/* The heap block holding a copy of the packet last parsed, valid until the next parse_exact. */
static uint8_t *exact_packet;

/* A packet-sized work area for the mutated copies of a request. */
static uint8_t trial[WIRE_MAX];

/* The state of the pseudo-random generator of the random runs, never zero. */
static uint32_t random_state = 2463534242UL;

/*
 * The values written into the fields of attributes built by wire_attrs,
 * chosen so that every byte of each differs from its neighbours.
 */
#define VALUE_SIZE_HIGH		0x01020304UL
#define VALUE_SIZE_LOW		0x05060708UL
#define VALUE_UID		0x000003e8UL
#define VALUE_GID		0x000007d0UL
#define VALUE_PERMISSIONS	0x000081a4UL
#define VALUE_ATIME		0x11223344UL
#define VALUE_MTIME		0x55667788UL

/*
 * The first fields of a long name for an unknown mode, one link, owner
 * "o", group "g" and size 0, each field written at its width from the
 * draft's section 7 (10, 3, 8, 8 and 8 characters, one space between).
 */
/* The fields after the mode for link count 1, owner "o", group "g", size 0, 1970-01-01 00:00 and name "n". */
#define LN_AFTER_MODE	" " "  1" " " "o       " " " "g       " " " "       0" " " "Jan  1 00:00" " " "n"

#define LN_Q_O_G_0	"?---------" " " "  1" " " "o       " " " "g       " " " "       0" " "

/* Checks that two texts are equal, printing both on a failure. */
#define CHECK_TEXT(actual, expected) check_text((actual), (expected), __LINE__)

static void check_text(const char *actual, const char *want, int line);
static void test_next_packet(void);
static void test_next_packet_stream(void);
static void test_next_packet_random(void);
static void test_init(void);
static void test_path_requests(void);
static void test_handle_requests(void);
static void test_attrs_requests(void);
static void test_open(void);
static void test_read_write(void);
static void test_two_paths(void);
static void test_attrs_flags(void);
static void test_extended_pairs(void);
static void test_path_limits(void);
static void test_handle_limits(void);
static void test_unknown_types(void);
static void test_failed_requests(void);
static void test_prefixes_and_trailing(void);
static void test_parse_random(void);
static void test_put_version(void);
static void test_put_status(void);
static void test_put_handle(void);
static void test_put_data(void);
static void test_put_attrs(void);
static void test_name_packet(void);
static void test_end_packet(void);
static void test_put_limits(void);
static void test_roundtrip_framing(void);
static void test_long_name_forms(void);
static void test_long_name_permissions(void);
static void test_long_name_dates(void);
static void test_long_name_fields(void);
static void test_long_name_cut(void);
static void test_long_name_random(void);
static uint32_t next_random(void);
static void wire_header(struct wire *wire, unsigned type, uint32_t id);
static void wire_attrs(struct wire *wire, uint32_t flags, uint32_t pairs);
static int parse_exact(const uint8_t *bytes, size_t size, struct ssh_sftp_request *out);
static void expect_packet(const struct ssh_buf *actual, const struct wire *want_body, const char *what);
static void check_attrs_values(const struct ssh_sftp_attrs *attrs, uint32_t flags);
static void check_prefixes_invalid(const struct wire *wire);
static void make_long_name(const struct ssh_sftp_attrs *attrs, uint32_t links, const char *owner, const char *group, const char *name, int64_t now, char *out, size_t capacity);

/*
 * Runs the checks of the module.
 */
int
main(void)
{
	/* Runs each group of checks. */
	test_next_packet();
	test_next_packet_stream();
	test_next_packet_random();
	test_init();
	test_path_requests();
	test_handle_requests();
	test_attrs_requests();
	test_open();
	test_read_write();
	test_two_paths();
	test_attrs_flags();
	test_extended_pairs();
	test_path_limits();
	test_handle_limits();
	test_unknown_types();
	test_failed_requests();
	test_prefixes_and_trailing();
	test_parse_random();
	test_put_version();
	test_put_status();
	test_put_handle();
	test_put_data();
	test_put_attrs();
	test_name_packet();
	test_end_packet();
	test_put_limits();
	test_roundtrip_framing();
	test_long_name_forms();
	test_long_name_permissions();
	test_long_name_dates();
	test_long_name_fields();
	test_long_name_cut();
	test_long_name_random();

	/* Releases the last packet copy. */
	free(exact_packet);

	/* Reports the result of the whole test. */
	return check_finish("sftp");
}

/*
 * Records a check that two texts are equal, printing both on a failure.
 */
static void
check_text(
	const char *actual,
	const char *want,
	int line)
{
	/* Counts the check. */
	check_count++;

	/* Reports a difference with both texts. */
	if (strcmp(actual, want) != 0) {
		check_failed++;
		printf("FAIL %s:%d: got [%s], expected [%s]\n", __FILE__, line, actual, want);
	}
}

/*
 * Starts a request in a wire: the type and the id (draft section 3).
 */
static void
wire_header(
	struct wire *wire,
	unsigned type,
	uint32_t id)
{
	/* Starts empty, then the type byte and the request id. */
	wire_reset(wire);
	wire_u8(wire, type);
	wire_u32(wire, id);
}

/*
 * Appends an ATTRS (draft section 5) with the given flags and the fixed
 * values above in the order of the draft's layout; when the flags have
 * EXTENDED, an extended_count of pairs and that many pairs follow.
 */
static void
wire_attrs(
	struct wire *wire,
	uint32_t flags,
	uint32_t pairs)
{
	uint32_t index;
	char name[32];

	/* The flags field first. */
	wire_u32(wire, flags);

	/* Each field in the draft's order, present only when its flag is. */
	if ((flags & 0x00000001UL) != 0) {
		wire_u32(wire, VALUE_SIZE_HIGH);
		wire_u32(wire, VALUE_SIZE_LOW);
	}
	if ((flags & 0x00000002UL) != 0) {
		wire_u32(wire, VALUE_UID);
		wire_u32(wire, VALUE_GID);
	}
	if ((flags & 0x00000004UL) != 0)
		wire_u32(wire, VALUE_PERMISSIONS);
	if ((flags & 0x00000008UL) != 0) {
		wire_u32(wire, VALUE_ATIME);
		wire_u32(wire, VALUE_MTIME);
	}

	/* The extended count and the pairs. */
	if ((flags & 0x80000000UL) != 0) {
		wire_u32(wire, pairs);
		for (index = 0; index < pairs; index++) {
			name[0] = 'n';
			name[1] = (char)('a' + (int)(index % 26U));
			name[2] = '@';
			name[3] = 'x';
			name[4] = '\0';
			wire_text(wire, name);
			if ((index % 3U) == 0)
				wire_string(wire, "", 0);
			else
				wire_text(wire, "data");
		}
	}
}

/*
 * Parses a copy of the bytes held in a block of exactly their size, so a
 * read past the end is an address-sanitizer failure.  The request is
 * pre-filled with 0xaa so that a field the module forgets to set shows.
 * The copy stays in exact_packet until the next call.
 */
static int
parse_exact(
	const uint8_t *bytes,
	size_t size,
	struct ssh_sftp_request *out)
{
	int error;

	/* Replaces the previous copy by one of exactly the size. */
	free(exact_packet);
	exact_packet = malloc(size > 0 ? size : 1);
	if (exact_packet == NULL)
		abort();
	if (size > 0)
		memcpy(exact_packet, bytes, size);

	/* Fills the request with a pattern, then parses. */
	memset(out, 0xaa, sizeof(*out));
	error = ssh_sftp_parse_request(exact_packet, size, out);

	/* Reports the result of the parse. */
	return error;
}

/*
 * Checks the attributes parsed from a wire_attrs of the given flags: the
 * fields the flags name hold the values written, the others are 0, and
 * the flags field holds the flags without EXTENDED.
 */
static void
check_attrs_values(
	const struct ssh_sftp_attrs *attrs,
	uint32_t flags)
{
	uint64_t size;

	/* The flags without the extended bit, which is not kept. */
	CHECK((attrs->flags) == (flags & 0x0000000fUL));

	/* The size, or 0 when not present. */
	size = 0;
	if ((flags & 1UL) != 0)
		size = ((uint64_t)VALUE_SIZE_HIGH << 32) | (uint64_t)VALUE_SIZE_LOW;
	CHECK(attrs->size == size);

	/* The owner and the group. */
	CHECK(attrs->uid == (((flags & 2UL) != 0) ? VALUE_UID : 0UL));
	CHECK(attrs->gid == (((flags & 2UL) != 0) ? VALUE_GID : 0UL));

	/* The permissions. */
	CHECK(attrs->permissions == (((flags & 4UL) != 0) ? VALUE_PERMISSIONS : 0UL));

	/* The times. */
	CHECK(attrs->atime == (((flags & 8UL) != 0) ? VALUE_ATIME : 0UL));
	CHECK(attrs->mtime == (((flags & 8UL) != 0) ? VALUE_MTIME : 0UL));
}

/*
 * Checks that every strict prefix of a request is EINVAL (a request that
 * ends early is malformed) and that one extra byte at the end is EINVAL.
 */
static void
check_prefixes_invalid(
	const struct wire *wire)
{
	struct ssh_sftp_request out;
	size_t length;
	int failures;
	int error;

	/* Counts the prefixes that were not refused. */
	failures = 0;
	for (length = 0; length < wire->size; length++) {
		error = parse_exact(wire->data, length, &out);
		if (error != EINVAL)
			failures++;
	}
	CHECK_EQ(failures, 0);

	/* One extra byte after the last field is refused. */
	memcpy(trial, wire->data, wire->size);
	trial[wire->size] = 0;
	error = parse_exact(trial, wire->size + 1, &out);
	CHECK_EQ(error, EINVAL);
}

/*
 * Compares the bytes appended to a buffer with the packet body a wire
 * holds: the length field is the body's length (draft section 3), then
 * the body.
 */
static void
expect_packet(
	const struct ssh_buf *actual,
	const struct wire *want_body,
	const char *what)
{
	uint8_t length[4];

	/* Computes the length field by hand from the body. */
	length[0] = (uint8_t)(want_body->size >> 24);
	length[1] = (uint8_t)(want_body->size >> 16);
	length[2] = (uint8_t)(want_body->size >> 8);
	length[3] = (uint8_t)want_body->size;

	/* The buffer holds exactly the length field and the body. */
	check_record_long((long)actual->size, (long)(want_body->size + 4), __FILE__, __LINE__, what);
	if (actual->size == want_body->size + 4) {
		CHECK_BYTES(actual->data, length, 4);
		CHECK_BYTES(actual->data + 4, want_body->data, want_body->size);
	}
	CHECK_EQ(actual->error, 0);
}

/*
 * Returns the next value of a xorshift generator (the state is never 0).
 */
static uint32_t
next_random(void)
{
	/* Mixes the state with three shifts and xors. */
	random_state ^= random_state << 13;
	random_state ^= random_state >> 17;
	random_state ^= random_state << 5;

	/* Reports the new state. */
	return random_state;
}

/*
 * Checks the framing of ssh_sftp_next_packet on single packets.
 */
static void
test_next_packet(void)
{
	struct ssh_buf input;
	struct ssh_buf packet;
	uint32_t length;
	size_t index;
	int error;

	/* An empty input and every prefix of the length field wait for more. */
	ssh_buf_init(&input, 1024UL * 1024UL, 0);
	ssh_buf_init(&packet, 1024UL * 1024UL, 0);
	error = ssh_sftp_next_packet(&input, &packet);
	CHECK_EQ(error, EAGAIN);
	for (index = 1; index < 4; index++) {
		ssh_buf_put_u8(&input, 0);
		error = ssh_sftp_next_packet(&input, &packet);
		CHECK_EQ(error, EAGAIN);
		CHECK_EQ(input.size, index);
		CHECK_EQ(packet.size, 0);
	}

	/* A length of 0 is refused, and the input is left. */
	ssh_buf_put_u8(&input, 0);
	CHECK_EQ(input.size, 4);
	error = ssh_sftp_next_packet(&input, &packet);
	CHECK_EQ(error, EINVAL);
	CHECK_EQ(input.size, 4);
	CHECK_EQ(packet.size, 0);

	/* A length above the limit is refused as soon as the field is whole, without waiting for the bytes. */
	ssh_buf_reset(&input);
	wire_reset(&expected);
	wire_u32(&expected, SSH_SFTP_PACKET_MAX + 1U);
	ssh_buf_put_bytes(&input, expected.data, expected.size);
	error = ssh_sftp_next_packet(&input, &packet);
	CHECK_EQ(error, EINVAL);
	ssh_buf_reset(&input);
	wire_reset(&expected);
	wire_u32(&expected, 0xffffffffUL);
	ssh_buf_put_bytes(&input, expected.data, expected.size);
	error = ssh_sftp_next_packet(&input, &packet);
	CHECK_EQ(error, EINVAL);
	ssh_buf_reset(&input);
	wire_reset(&expected);
	wire_u32(&expected, 0x80000000UL);
	ssh_buf_put_bytes(&input, expected.data, expected.size);
	error = ssh_sftp_next_packet(&input, &packet);
	CHECK_EQ(error, EINVAL);

	/* A packet whose body has not all arrived waits, at every missing count. */
	for (length = 1; length <= 6; length++) {
		for (index = 0; index < length; index++) {
			ssh_buf_reset(&input);
			wire_reset(&expected);
			wire_u32(&expected, length);
			for (error = 0; (uint32_t)error < index; error++)
				wire_u8(&expected, 0x40 + (unsigned)error);
			ssh_buf_put_bytes(&input, expected.data, expected.size);
			error = ssh_sftp_next_packet(&input, &packet);
			CHECK_EQ(error, EAGAIN);
			CHECK_EQ(input.size, 4 + index);
			CHECK_EQ(packet.size, 0);
		}
	}

	/* A packet of 5 bytes whole is moved, type first, and removed from the input. */
	ssh_buf_reset(&input);
	wire_reset(&expected);
	wire_u32(&expected, 5);
	wire_bytes(&expected, "abcde", 5);
	ssh_buf_put_bytes(&input, expected.data, expected.size);
	error = ssh_sftp_next_packet(&input, &packet);
	CHECK_EQ(error, 0);
	CHECK_EQ(packet.size, 5);
	CHECK_BYTES(packet.data, "abcde", 5);
	CHECK_EQ(input.size, 0);

	/* A packet of 1 byte (a type alone) is valid framing. */
	ssh_buf_reset(&input);
	ssh_buf_reset(&packet);
	wire_reset(&expected);
	wire_u32(&expected, 1);
	wire_u8(&expected, 0x7e);
	ssh_buf_put_bytes(&input, expected.data, expected.size);
	error = ssh_sftp_next_packet(&input, &packet);
	CHECK_EQ(error, 0);
	CHECK_EQ(packet.size, 1);
	CHECK_EQ(packet.data[0], 0x7e);

	/* The limit itself is accepted whole, and one byte short of it waits. */
	ssh_buf_reset(&input);
	ssh_buf_reset(&packet);
	wire_reset(&expected);
	wire_u32(&expected, SSH_SFTP_PACKET_MAX);
	for (index = 0; index < SSH_SFTP_PACKET_MAX; index++)
		wire_u8(&expected, (unsigned)(index * 7U + 3U));
	ssh_buf_put_bytes(&input, expected.data, expected.size - 1);
	error = ssh_sftp_next_packet(&input, &packet);
	CHECK_EQ(error, EAGAIN);
	CHECK_EQ(input.size, expected.size - 1);
	ssh_buf_put_u8(&input, expected.data[expected.size - 1]);
	error = ssh_sftp_next_packet(&input, &packet);
	CHECK_EQ(error, 0);
	CHECK_EQ(packet.size, SSH_SFTP_PACKET_MAX);
	CHECK_BYTES(packet.data, expected.data + 4, SSH_SFTP_PACKET_MAX);
	CHECK_EQ(input.size, 0);

	/* A packet_out that cannot hold the bytes reports its error and leaves the input whole. */
	ssh_buf_free(&packet);
	ssh_buf_init(&packet, 4, 0);
	ssh_buf_reset(&input);
	wire_reset(&expected);
	wire_u32(&expected, 5);
	wire_bytes(&expected, "abcde", 5);
	ssh_buf_put_bytes(&input, expected.data, expected.size);
	error = ssh_sftp_next_packet(&input, &packet);
	CHECK_EQ(error, EMSGSIZE);
	CHECK_EQ(input.size, 9);
	CHECK_BYTES(input.data, expected.data, 9);

	/* Frees the buffers. */
	ssh_buf_free(&input);
	ssh_buf_free(&packet);
}

/*
 * Checks a stream of several packets taken one at a time, with a partial
 * packet after them.
 */
static void
test_next_packet_stream(void)
{
	struct ssh_buf input;
	struct ssh_buf packet;
	int error;

	/* Three packets of different sizes and a fourth that is cut. */
	ssh_buf_init(&input, 4096, 0);
	ssh_buf_init(&packet, 4096, 0);
	wire_reset(&expected);
	wire_u32(&expected, 3);
	wire_bytes(&expected, "one", 3);
	wire_u32(&expected, 1);
	wire_bytes(&expected, "2", 1);
	wire_u32(&expected, 6);
	wire_bytes(&expected, "three!", 6);
	wire_u32(&expected, 4);
	wire_bytes(&expected, "fo", 2);
	ssh_buf_put_bytes(&input, expected.data, expected.size);

	/* The first packet. */
	error = ssh_sftp_next_packet(&input, &packet);
	CHECK_EQ(error, 0);
	CHECK_EQ(packet.size, 3);
	CHECK_BYTES(packet.data, "one", 3);

	/* The second, after the caller reset the output. */
	ssh_buf_reset(&packet);
	error = ssh_sftp_next_packet(&input, &packet);
	CHECK_EQ(error, 0);
	CHECK_EQ(packet.size, 1);
	CHECK_BYTES(packet.data, "2", 1);

	/* The third. */
	ssh_buf_reset(&packet);
	error = ssh_sftp_next_packet(&input, &packet);
	CHECK_EQ(error, 0);
	CHECK_EQ(packet.size, 6);
	CHECK_BYTES(packet.data, "three!", 6);

	/* The fourth is not whole: wait, with its six bytes still in the input. */
	ssh_buf_reset(&packet);
	error = ssh_sftp_next_packet(&input, &packet);
	CHECK_EQ(error, EAGAIN);
	CHECK_EQ(input.size, 6);
	CHECK_EQ(packet.size, 0);

	/* The rest of the fourth arrives and it is taken. */
	ssh_buf_put_bytes(&input, "rt", 2);
	error = ssh_sftp_next_packet(&input, &packet);
	CHECK_EQ(error, 0);
	CHECK_EQ(packet.size, 4);
	CHECK_BYTES(packet.data, "fort", 4);
	CHECK_EQ(input.size, 0);

	/* Frees the buffers. */
	ssh_buf_free(&input);
	ssh_buf_free(&packet);
}

/*
 * Runs random streams through ssh_sftp_next_packet: the result is one of
 * the three documented, the output equals the bytes after the length
 * field, and the input shrinks by exactly the packet.
 */
static void
test_next_packet_random(void)
{
	struct ssh_buf input;
	struct ssh_buf packet;
	uint8_t bytes[300];
	size_t count;
	size_t index;
	size_t before;
	uint32_t length;
	int failures;
	int run;
	int error;

	/* Counts every violated expectation. */
	failures = 0;
	for (run = 0; run < 20000; run++) {
		/* Random bytes, with small lengths in the first field so that some packets are whole. */
		count = next_random() % 40U;
		for (index = 0; index < count; index++)
			bytes[index] = (uint8_t)next_random();
		if (count >= 4) {
			bytes[0] = 0;
			bytes[1] = 0;
			bytes[2] = 0;
			if ((next_random() % 8U) == 0)
				bytes[0] = (uint8_t)next_random();
			bytes[3] = (uint8_t)(next_random() % 40U);
		}
		ssh_buf_init(&input, 4096, 0);
		ssh_buf_init(&packet, 4096, 0);
		ssh_buf_put_bytes(&input, bytes, count);
		before = input.size;

		/* Takes a packet. */
		error = ssh_sftp_next_packet(&input, &packet);
		if (error != 0 && error != EAGAIN && error != EINVAL)
			failures++;
		if (error == 0) {
			length = ssh_load_be32(bytes);
			if (packet.size != length || input.size != before - 4 - length)
				failures++;
			if (length > 0 && memcmp(packet.data, bytes + 4, length) != 0)
				failures++;
		} else if (input.size != before || packet.size != 0) {
			failures++;
		}
		ssh_buf_free(&input);
		ssh_buf_free(&packet);
	}
	CHECK_EQ(failures, 0);
}

/*
 * Checks the INIT request: the version, and the extension pairs ignored.
 */
static void
test_init(void)
{
	struct ssh_sftp_request out;
	int error;

	/* INIT with version 3 and no extensions (draft section 4). */
	wire_reset(&request_wire);
	wire_u8(&request_wire, 1);
	wire_u32(&request_wire, 3);
	error = parse_exact(request_wire.data, request_wire.size, &out);
	CHECK_EQ(error, 0);
	CHECK_EQ(out.type, SSH_FXP_INIT);
	CHECK_EQ(out.version, 3);
	CHECK_EQ(out.id, 0);
	CHECK(out.path == NULL);
	CHECK(out.handle == NULL);
	CHECK_EQ(out.attrs.flags, 0);
	check_prefixes_invalid(&request_wire);

	/* Version values at the ends of the range. */
	wire_reset(&request_wire);
	wire_u8(&request_wire, 1);
	wire_u32(&request_wire, 0);
	error = parse_exact(request_wire.data, request_wire.size, &out);
	CHECK_EQ(error, 0);
	CHECK_EQ(out.version, 0);
	wire_reset(&request_wire);
	wire_u8(&request_wire, 1);
	wire_u32(&request_wire, 0xffffffffUL);
	error = parse_exact(request_wire.data, request_wire.size, &out);
	CHECK_EQ(error, 0);
	CHECK(out.version == 0xffffffffUL);

	/* Extension pairs are silently ignored, one with an empty data string and one with data. */
	wire_reset(&request_wire);
	wire_u8(&request_wire, 1);
	wire_u32(&request_wire, 6);
	wire_text(&request_wire, "vendor@example.com");
	wire_string(&request_wire, "", 0);
	wire_text(&request_wire, "other@example.org");
	wire_text(&request_wire, "1");
	error = parse_exact(request_wire.data, request_wire.size, &out);
	CHECK_EQ(error, 0);
	CHECK_EQ(out.type, SSH_FXP_INIT);
	CHECK_EQ(out.version, 6);

	/* A pair with a name and no data string is malformed (both strings must be present). */
	wire_reset(&request_wire);
	wire_u8(&request_wire, 1);
	wire_u32(&request_wire, 3);
	wire_text(&request_wire, "vendor@example.com");
	error = parse_exact(request_wire.data, request_wire.size, &out);
	CHECK_EQ(error, EINVAL);
	CHECK_EQ(out.type, SSH_FXP_INIT);

	/* A pair whose string runs past the end is malformed. */
	wire_reset(&request_wire);
	wire_u8(&request_wire, 1);
	wire_u32(&request_wire, 3);
	wire_u32(&request_wire, 100);
	wire_bytes(&request_wire, "abc", 3);
	error = parse_exact(request_wire.data, request_wire.size, &out);
	CHECK_EQ(error, EINVAL);

	/* One to three trailing bytes after the version are malformed. */
	wire_reset(&request_wire);
	wire_u8(&request_wire, 1);
	wire_u32(&request_wire, 3);
	wire_u8(&request_wire, 0);
	error = parse_exact(request_wire.data, request_wire.size, &out);
	CHECK_EQ(error, EINVAL);
	wire_u8(&request_wire, 0);
	error = parse_exact(request_wire.data, request_wire.size, &out);
	CHECK_EQ(error, EINVAL);
	wire_u8(&request_wire, 0);
	error = parse_exact(request_wire.data, request_wire.size, &out);
	CHECK_EQ(error, EINVAL);

	/* A version cut short is malformed, as is a type alone. */
	wire_reset(&request_wire);
	wire_u8(&request_wire, 1);
	error = parse_exact(request_wire.data, request_wire.size, &out);
	CHECK_EQ(error, EINVAL);
	wire_u32(&request_wire, 3);
	error = parse_exact(request_wire.data, request_wire.size - 1, &out);
	CHECK_EQ(error, EINVAL);
}

/*
 * Checks the requests that carry one path (draft sections 6.5 to 6.11).
 */
static void
test_path_requests(void)
{
	static const unsigned types[7] = {
		SSH_FXP_LSTAT, SSH_FXP_OPENDIR, SSH_FXP_REMOVE, SSH_FXP_RMDIR,
		SSH_FXP_REALPATH, SSH_FXP_STAT, SSH_FXP_READLINK
	};
	struct ssh_sftp_request out;
	unsigned which;
	int error;

	/* Each type with an ordinary path (the draft's layout: id, then a string). */
	for (which = 0; which < 7; which++) {
		wire_header(&request_wire, types[which], 0xdeadbeefUL);
		wire_text(&request_wire, "/tmp/a b");
		error = parse_exact(request_wire.data, request_wire.size, &out);
		CHECK_EQ(error, 0);
		CHECK_EQ(out.type, types[which]);
		CHECK(out.id == 0xdeadbeefUL);
		CHECK(out.path == exact_packet + 9);
		CHECK_EQ(out.path_size, 8);
		CHECK_BYTES(out.path, "/tmp/a b", 8);
		CHECK(out.target == NULL);
		CHECK_EQ(out.target_size, 0);
		CHECK(out.handle == NULL);
		CHECK_EQ(out.handle_size, 0);
		CHECK(out.data == NULL);
		CHECK_EQ(out.pflags, 0);
		CHECK_EQ(out.length, 0);
		CHECK(out.offset == 0);
		CHECK_EQ(out.attrs.flags, 0);
		check_prefixes_invalid(&request_wire);
	}

	/* An empty path is valid (draft section 6.2: it names the default directory). */
	wire_header(&request_wire, SSH_FXP_REALPATH, 1);
	wire_string(&request_wire, "", 0);
	error = parse_exact(request_wire.data, request_wire.size, &out);
	CHECK_EQ(error, 0);
	CHECK_EQ(out.path_size, 0);

	/* Bytes above 0x7f, a dot path and a backslash are passed as they are. */
	wire_header(&request_wire, SSH_FXP_STAT, 2);
	wire_string(&request_wire, "\xe3\x81\x82/..\\./\x80\xff", 11);
	error = parse_exact(request_wire.data, request_wire.size, &out);
	CHECK_EQ(error, 0);
	CHECK_EQ(out.path_size, 11);
	CHECK_BYTES(out.path, "\xe3\x81\x82/..\\./\x80\xff", 11);
}

/*
 * Checks the requests that carry one handle (CLOSE, FSTAT, READDIR).
 */
static void
test_handle_requests(void)
{
	static const unsigned types[3] = {
		SSH_FXP_CLOSE, SSH_FXP_FSTAT, SSH_FXP_READDIR
	};
	static const uint8_t handle[5] = { 0x00, 0x01, 0x02, 0xff, 0x00 };
	struct ssh_sftp_request out;
	unsigned which;
	int error;

	/* Each type with a handle that holds NUL bytes (a handle is opaque bytes). */
	for (which = 0; which < 3; which++) {
		wire_header(&request_wire, types[which], 77);
		wire_string(&request_wire, handle, 5);
		error = parse_exact(request_wire.data, request_wire.size, &out);
		CHECK_EQ(error, 0);
		CHECK_EQ(out.type, types[which]);
		CHECK_EQ(out.id, 77);
		CHECK(out.handle == exact_packet + 9);
		CHECK_EQ(out.handle_size, 5);
		CHECK_BYTES(out.handle, handle, 5);
		CHECK(out.path == NULL);
		CHECK(out.target == NULL);
		CHECK(out.data == NULL);
		check_prefixes_invalid(&request_wire);
	}

	/* An empty handle is parsed (whether it names anything is the server's decision). */
	wire_header(&request_wire, SSH_FXP_CLOSE, 5);
	wire_string(&request_wire, "", 0);
	error = parse_exact(request_wire.data, request_wire.size, &out);
	CHECK_EQ(error, 0);
	CHECK_EQ(out.handle_size, 0);
}

/*
 * Checks SETSTAT, FSETSTAT and MKDIR, which carry a path or a handle and
 * attributes.
 */
static void
test_attrs_requests(void)
{
	struct ssh_sftp_request out;
	int error;

	/* SETSTAT: id, path, attrs (draft section 6.9). */
	wire_header(&request_wire, SSH_FXP_SETSTAT, 11);
	wire_text(&request_wire, "/file");
	wire_attrs(&request_wire, 0x0000000fUL, 0);
	error = parse_exact(request_wire.data, request_wire.size, &out);
	CHECK_EQ(error, 0);
	CHECK_EQ(out.type, SSH_FXP_SETSTAT);
	CHECK_EQ(out.id, 11);
	CHECK(out.path == exact_packet + 9);
	CHECK_EQ(out.path_size, 5);
	CHECK(out.handle == NULL);
	check_attrs_values(&out.attrs, 0x0000000fUL);
	check_prefixes_invalid(&request_wire);

	/* MKDIR: id, path, attrs (draft section 6.6), here with the permissions only. */
	wire_header(&request_wire, SSH_FXP_MKDIR, 12);
	wire_text(&request_wire, "newdir");
	wire_attrs(&request_wire, 0x00000004UL, 0);
	error = parse_exact(request_wire.data, request_wire.size, &out);
	CHECK_EQ(error, 0);
	CHECK_EQ(out.type, SSH_FXP_MKDIR);
	CHECK_EQ(out.path_size, 6);
	check_attrs_values(&out.attrs, 0x00000004UL);
	check_prefixes_invalid(&request_wire);

	/* FSETSTAT: id, handle, attrs (draft section 6.9). */
	wire_header(&request_wire, SSH_FXP_FSETSTAT, 13);
	wire_string(&request_wire, "\x01\x02\x03\x04", 4);
	wire_attrs(&request_wire, 0x00000001UL, 0);
	error = parse_exact(request_wire.data, request_wire.size, &out);
	CHECK_EQ(error, 0);
	CHECK_EQ(out.type, SSH_FXP_FSETSTAT);
	CHECK(out.handle == exact_packet + 9);
	CHECK_EQ(out.handle_size, 4);
	CHECK(out.path == NULL);
	check_attrs_values(&out.attrs, 0x00000001UL);
	check_prefixes_invalid(&request_wire);

	/* The attrs of an FSETSTAT with no flag is just the flags field. */
	wire_header(&request_wire, SSH_FXP_FSETSTAT, 14);
	wire_string(&request_wire, "h", 1);
	wire_attrs(&request_wire, 0, 0);
	error = parse_exact(request_wire.data, request_wire.size, &out);
	CHECK_EQ(error, 0);
	check_attrs_values(&out.attrs, 0);
}

/*
 * Checks OPEN (draft section 6.3): path, pflags, attrs.
 */
static void
test_open(void)
{
	struct ssh_sftp_request out;
	int error;

	/* OPEN with read, write and create, and the permissions of the new file. */
	wire_header(&request_wire, SSH_FXP_OPEN, 21);
	wire_text(&request_wire, "dir/new.txt");
	wire_u32(&request_wire, 0x0000000bUL);
	wire_attrs(&request_wire, 0x00000004UL, 0);
	error = parse_exact(request_wire.data, request_wire.size, &out);
	CHECK_EQ(error, 0);
	CHECK_EQ(out.type, SSH_FXP_OPEN);
	CHECK_EQ(out.id, 21);
	CHECK(out.path == exact_packet + 9);
	CHECK_EQ(out.path_size, 11);
	CHECK_BYTES(out.path, "dir/new.txt", 11);
	CHECK_EQ(out.pflags, SSH_FXF_READ | SSH_FXF_WRITE | SSH_FXF_CREAT);
	check_attrs_values(&out.attrs, 0x00000004UL);
	check_prefixes_invalid(&request_wire);

	/* Each defined open flag alone, and all of them together, arrive unchanged. */
	wire_header(&request_wire, SSH_FXP_OPEN, 22);
	wire_text(&request_wire, "f");
	wire_u32(&request_wire, 0x0000003fUL);
	wire_attrs(&request_wire, 0, 0);
	error = parse_exact(request_wire.data, request_wire.size, &out);
	CHECK_EQ(error, 0);
	CHECK_EQ(out.pflags, 0x3fUL);

	/* The flag word is passed as it is, including bits the draft does not define. */
	wire_header(&request_wire, SSH_FXP_OPEN, 23);
	wire_text(&request_wire, "f");
	wire_u32(&request_wire, 0xffffffc0UL);
	wire_attrs(&request_wire, 0, 0);
	error = parse_exact(request_wire.data, request_wire.size, &out);
	CHECK_EQ(error, 0);
	CHECK(out.pflags == 0xffffffc0UL);

	/* A path with a NUL, or too long, fails the whole OPEN. */
	wire_header(&request_wire, SSH_FXP_OPEN, 24);
	wire_string(&request_wire, "a\0b", 3);
	wire_u32(&request_wire, 1);
	wire_attrs(&request_wire, 0, 0);
	error = parse_exact(request_wire.data, request_wire.size, &out);
	CHECK_EQ(error, EINVAL);
	CHECK_EQ(out.type, SSH_FXP_OPEN);
	CHECK_EQ(out.id, 24);
	CHECK(out.path == NULL);
}

/*
 * Checks READ and WRITE (draft section 6.4).
 */
static void
test_read_write(void)
{
	struct ssh_sftp_request out;
	static uint8_t big_data[200000];
	size_t index;
	int error;

	/* READ: id, handle, uint64 offset, uint32 length. */
	wire_header(&request_wire, SSH_FXP_READ, 31);
	wire_string(&request_wire, "HANDLE", 6);
	wire_u64(&request_wire, 0x0102030405060708ULL);
	wire_u32(&request_wire, 4096);
	error = parse_exact(request_wire.data, request_wire.size, &out);
	CHECK_EQ(error, 0);
	CHECK_EQ(out.type, SSH_FXP_READ);
	CHECK_EQ(out.id, 31);
	CHECK(out.handle == exact_packet + 9);
	CHECK_EQ(out.handle_size, 6);
	CHECK(out.offset == 0x0102030405060708ULL);
	CHECK_EQ(out.length, 4096);
	CHECK(out.data == NULL);
	check_prefixes_invalid(&request_wire);

	/* The offset is a full 64-bit value. */
	wire_header(&request_wire, SSH_FXP_READ, 32);
	wire_string(&request_wire, "H", 1);
	wire_u64(&request_wire, 0xffffffffffffffffULL);
	wire_u32(&request_wire, 0);
	error = parse_exact(request_wire.data, request_wire.size, &out);
	CHECK_EQ(error, 0);
	CHECK(out.offset == 0xffffffffffffffffULL);
	CHECK_EQ(out.length, 0);

	/* A length up to SSH_SFTP_READ_MAX is kept; a larger one is cut to it. */
	wire_header(&request_wire, SSH_FXP_READ, 33);
	wire_string(&request_wire, "H", 1);
	wire_u64(&request_wire, 0);
	wire_u32(&request_wire, SSH_SFTP_READ_MAX);
	error = parse_exact(request_wire.data, request_wire.size, &out);
	CHECK_EQ(error, 0);
	CHECK_EQ(out.length, SSH_SFTP_READ_MAX);
	wire_header(&request_wire, SSH_FXP_READ, 34);
	wire_string(&request_wire, "H", 1);
	wire_u64(&request_wire, 0);
	wire_u32(&request_wire, SSH_SFTP_READ_MAX - 1U);
	error = parse_exact(request_wire.data, request_wire.size, &out);
	CHECK_EQ(out.length, SSH_SFTP_READ_MAX - 1U);
	wire_header(&request_wire, SSH_FXP_READ, 35);
	wire_string(&request_wire, "H", 1);
	wire_u64(&request_wire, 0);
	wire_u32(&request_wire, SSH_SFTP_READ_MAX + 1U);
	error = parse_exact(request_wire.data, request_wire.size, &out);
	CHECK_EQ(error, 0);
	CHECK_EQ(out.length, SSH_SFTP_READ_MAX);
	wire_header(&request_wire, SSH_FXP_READ, 36);
	wire_string(&request_wire, "H", 1);
	wire_u64(&request_wire, 0);
	wire_u32(&request_wire, 0xffffffffUL);
	error = parse_exact(request_wire.data, request_wire.size, &out);
	CHECK_EQ(error, 0);
	CHECK_EQ(out.length, SSH_SFTP_READ_MAX);

	/* WRITE: id, handle, uint64 offset, string data. */
	wire_header(&request_wire, SSH_FXP_WRITE, 41);
	wire_string(&request_wire, "HANDLE", 6);
	wire_u64(&request_wire, 0x1122334455667788ULL);
	wire_string(&request_wire, "hello\0world", 11);
	error = parse_exact(request_wire.data, request_wire.size, &out);
	CHECK_EQ(error, 0);
	CHECK_EQ(out.type, SSH_FXP_WRITE);
	CHECK_EQ(out.id, 41);
	CHECK(out.handle == exact_packet + 9);
	CHECK_EQ(out.handle_size, 6);
	CHECK(out.offset == 0x1122334455667788ULL);
	CHECK(out.data == exact_packet + 9 + 6 + 8 + 4);
	CHECK_EQ(out.data_size, 11);
	CHECK_BYTES(out.data, "hello\0world", 11);
	CHECK_EQ(out.length, 0);
	check_prefixes_invalid(&request_wire);

	/* Empty data is valid. */
	wire_header(&request_wire, SSH_FXP_WRITE, 42);
	wire_string(&request_wire, "H", 1);
	wire_u64(&request_wire, 5);
	wire_string(&request_wire, "", 0);
	error = parse_exact(request_wire.data, request_wire.size, &out);
	CHECK_EQ(error, 0);
	CHECK_EQ(out.data_size, 0);

	/* Data of 200000 bytes (inside the packet limit) is kept whole, as the bytes written. */
	for (index = 0; index < sizeof(big_data); index++)
		big_data[index] = (uint8_t)(index * 31U + 7U);
	wire_header(&request_wire, SSH_FXP_WRITE, 43);
	wire_string(&request_wire, "H", 1);
	wire_u64(&request_wire, 0);
	wire_string(&request_wire, big_data, sizeof(big_data));
	error = parse_exact(request_wire.data, request_wire.size, &out);
	CHECK_EQ(error, 0);
	CHECK_EQ(out.data_size, sizeof(big_data));
	CHECK_BYTES(out.data, big_data, sizeof(big_data));

	/* A data length larger than what remains is malformed, at 1 more and at the maximum. */
	wire_header(&request_wire, SSH_FXP_WRITE, 44);
	wire_string(&request_wire, "H", 1);
	wire_u64(&request_wire, 0);
	wire_u32(&request_wire, 4);
	wire_bytes(&request_wire, "abc", 3);
	error = parse_exact(request_wire.data, request_wire.size, &out);
	CHECK_EQ(error, EINVAL);
	CHECK_EQ(out.type, SSH_FXP_WRITE);
	CHECK_EQ(out.id, 44);
	CHECK(out.data == NULL);
	CHECK_EQ(out.data_size, 0);
	wire_header(&request_wire, SSH_FXP_WRITE, 45);
	wire_string(&request_wire, "H", 1);
	wire_u64(&request_wire, 0);
	wire_u32(&request_wire, 0xffffffffUL);
	wire_bytes(&request_wire, "abc", 3);
	error = parse_exact(request_wire.data, request_wire.size, &out);
	CHECK_EQ(error, EINVAL);
}

/*
 * Checks RENAME and SYMLINK, which carry two paths (draft sections 6.5
 * and 6.10), SYMLINK in the order OpenSSH sends them as the header says.
 */
static void
test_two_paths(void)
{
	struct ssh_sftp_request out;
	int error;

	/* RENAME: id, oldpath, newpath. */
	wire_header(&request_wire, SSH_FXP_RENAME, 51);
	wire_text(&request_wire, "old/name");
	wire_text(&request_wire, "new");
	error = parse_exact(request_wire.data, request_wire.size, &out);
	CHECK_EQ(error, 0);
	CHECK_EQ(out.type, SSH_FXP_RENAME);
	CHECK_EQ(out.id, 51);
	CHECK(out.path == exact_packet + 9);
	CHECK_EQ(out.path_size, 8);
	CHECK_BYTES(out.path, "old/name", 8);
	CHECK(out.target == exact_packet + 9 + 8 + 4);
	CHECK_EQ(out.target_size, 3);
	CHECK_BYTES(out.target, "new", 3);
	check_prefixes_invalid(&request_wire);

	/* SYMLINK: the first string is path and the second target, as they come. */
	wire_header(&request_wire, SSH_FXP_SYMLINK, 52);
	wire_text(&request_wire, "first");
	wire_text(&request_wire, "second-one");
	error = parse_exact(request_wire.data, request_wire.size, &out);
	CHECK_EQ(error, 0);
	CHECK_EQ(out.type, SSH_FXP_SYMLINK);
	CHECK_EQ(out.path_size, 5);
	CHECK_BYTES(out.path, "first", 5);
	CHECK_EQ(out.target_size, 10);
	CHECK_BYTES(out.target, "second-one", 10);
	check_prefixes_invalid(&request_wire);

	/* Empty paths are valid in both places. */
	wire_header(&request_wire, SSH_FXP_RENAME, 53);
	wire_string(&request_wire, "", 0);
	wire_string(&request_wire, "", 0);
	error = parse_exact(request_wire.data, request_wire.size, &out);
	CHECK_EQ(error, 0);
	CHECK_EQ(out.path_size, 0);
	CHECK_EQ(out.target_size, 0);

	/* A NUL in the second path fails the request, with the id reported. */
	wire_header(&request_wire, SSH_FXP_SYMLINK, 54);
	wire_text(&request_wire, "ok");
	wire_string(&request_wire, "x\0y", 3);
	error = parse_exact(request_wire.data, request_wire.size, &out);
	CHECK_EQ(error, EINVAL);
	CHECK_EQ(out.id, 54);
	CHECK(out.path == NULL);
	CHECK(out.target == NULL);

	/* A second path of SSH_SFTP_PATH_MAX + 1 bytes fails. */
	memset(trial, 'p', SSH_SFTP_PATH_MAX + 1U);
	wire_header(&request_wire, SSH_FXP_RENAME, 55);
	wire_text(&request_wire, "ok");
	wire_string(&request_wire, trial, SSH_SFTP_PATH_MAX + 1U);
	error = parse_exact(request_wire.data, request_wire.size, &out);
	CHECK_EQ(error, EINVAL);

	/* ... and one of SSH_SFTP_PATH_MAX bytes passes. */
	wire_header(&request_wire, SSH_FXP_RENAME, 56);
	wire_text(&request_wire, "ok");
	wire_string(&request_wire, trial, SSH_SFTP_PATH_MAX);
	error = parse_exact(request_wire.data, request_wire.size, &out);
	CHECK_EQ(error, 0);
	CHECK_EQ(out.target_size, SSH_SFTP_PATH_MAX);
}

/*
 * Checks every combination of the four attribute flags (draft section 5).
 */
static void
test_attrs_flags(void)
{
	struct ssh_sftp_request out;
	uint32_t flags;
	int error;

	/* All 16 combinations of SIZE, UIDGID, PERMISSIONS and ACMODTIME, in a SETSTAT. */
	for (flags = 0; flags < 16; flags++) {
		wire_header(&request_wire, SSH_FXP_SETSTAT, 61);
		wire_text(&request_wire, "p");
		wire_attrs(&request_wire, flags, 0);
		error = parse_exact(request_wire.data, request_wire.size, &out);
		CHECK_EQ(error, 0);
		check_attrs_values(&out.attrs, flags);
		CHECK_EQ(out.path_size, 1);
		check_prefixes_invalid(&request_wire);
	}

	/* The same combinations with the EXTENDED flag and zero pairs: the bit is not kept. */
	for (flags = 0; flags < 16; flags++) {
		wire_header(&request_wire, SSH_FXP_SETSTAT, 62);
		wire_text(&request_wire, "p");
		wire_attrs(&request_wire, flags | 0x80000000UL, 0);
		error = parse_exact(request_wire.data, request_wire.size, &out);
		CHECK_EQ(error, 0);
		check_attrs_values(&out.attrs, flags | 0x80000000UL);
	}

	/* Each flag bit the draft does not define is refused, bits 4 to 30 one at a time. */
	for (flags = 4; flags <= 30; flags++) {
		wire_header(&request_wire, SSH_FXP_MKDIR, 63);
		wire_text(&request_wire, "p");
		wire_u32(&request_wire, 1UL << flags);
		error = parse_exact(request_wire.data, request_wire.size, &out);
		CHECK_EQ(error, EINVAL);
		CHECK_EQ(out.type, SSH_FXP_MKDIR);
		CHECK_EQ(out.id, 63);
		CHECK_EQ(out.attrs.flags, 0);
		CHECK(out.path == NULL);
	}

	/* An undefined bit together with defined ones, with the fields of the defined ones present, is refused. */
	wire_header(&request_wire, SSH_FXP_MKDIR, 64);
	wire_text(&request_wire, "p");
	wire_attrs(&request_wire, 0x00000010UL | 0x0000000fUL, 0);
	error = parse_exact(request_wire.data, request_wire.size, &out);
	CHECK_EQ(error, EINVAL);
	wire_header(&request_wire, SSH_FXP_MKDIR, 65);
	wire_text(&request_wire, "p");
	wire_u32(&request_wire, 0xffffffffUL);
	error = parse_exact(request_wire.data, request_wire.size, &out);
	CHECK_EQ(error, EINVAL);

	/* A flags field cut short, or missing, is refused. */
	wire_header(&request_wire, SSH_FXP_MKDIR, 66);
	wire_text(&request_wire, "p");
	wire_bytes(&request_wire, "\0\0\0", 3);
	error = parse_exact(request_wire.data, request_wire.size, &out);
	CHECK_EQ(error, EINVAL);
	wire_header(&request_wire, SSH_FXP_MKDIR, 67);
	wire_text(&request_wire, "p");
	error = parse_exact(request_wire.data, request_wire.size, &out);
	CHECK_EQ(error, EINVAL);

	/* A field the flags name but the packet lacks is refused: SIZE with 7 of its 8 bytes. */
	wire_header(&request_wire, SSH_FXP_MKDIR, 68);
	wire_text(&request_wire, "p");
	wire_u32(&request_wire, 1);
	wire_bytes(&request_wire, "1234567", 7);
	error = parse_exact(request_wire.data, request_wire.size, &out);
	CHECK_EQ(error, EINVAL);

	/* A field present without its flag is trailing data and is refused. */
	wire_header(&request_wire, SSH_FXP_MKDIR, 69);
	wire_text(&request_wire, "p");
	wire_u32(&request_wire, 0);
	wire_u32(&request_wire, 0x1234);
	error = parse_exact(request_wire.data, request_wire.size, &out);
	CHECK_EQ(error, EINVAL);
}

/*
 * Checks the extended attribute pairs: skipped, at most
 * SSH_SFTP_EXTENDED_MAX of them.
 */
static void
test_extended_pairs(void)
{
	struct ssh_sftp_request out;
	uint32_t pairs;
	int error;

	/* 0, 1, 2 and the maximum number of pairs are accepted and the other fields still read right. */
	for (pairs = 0; pairs <= SSH_SFTP_EXTENDED_MAX; pairs++) {
		wire_header(&request_wire, SSH_FXP_SETSTAT, 71);
		wire_text(&request_wire, "p");
		wire_attrs(&request_wire, 0x8000000fUL, pairs);
		error = parse_exact(request_wire.data, request_wire.size, &out);
		CHECK_EQ(error, 0);
		check_attrs_values(&out.attrs, 0x8000000fUL);
	}

	/* Every strict prefix of an attrs with pairs is malformed. */
	wire_header(&request_wire, SSH_FXP_SETSTAT, 72);
	wire_text(&request_wire, "p");
	wire_attrs(&request_wire, 0x8000000fUL, 3);
	check_prefixes_invalid(&request_wire);

	/* One pair more than the maximum is refused, and so is a count that is huge. */
	wire_header(&request_wire, SSH_FXP_SETSTAT, 73);
	wire_text(&request_wire, "p");
	wire_attrs(&request_wire, 0x8000000fUL, SSH_SFTP_EXTENDED_MAX + 1U);
	error = parse_exact(request_wire.data, request_wire.size, &out);
	CHECK_EQ(error, EINVAL);
	CHECK_EQ(out.type, SSH_FXP_SETSTAT);
	CHECK_EQ(out.id, 73);
	CHECK_EQ(out.attrs.flags, 0);
	CHECK_EQ(out.attrs.size, 0);
	CHECK(out.path == NULL);
	wire_header(&request_wire, SSH_FXP_SETSTAT, 74);
	wire_text(&request_wire, "p");
	wire_u32(&request_wire, 0x80000000UL);
	wire_u32(&request_wire, 0xffffffffUL);
	error = parse_exact(request_wire.data, request_wire.size, &out);
	CHECK_EQ(error, EINVAL);

	/* A count with fewer pairs than it says is refused, at the cut inside a name and a data string. */
	wire_header(&request_wire, SSH_FXP_SETSTAT, 75);
	wire_text(&request_wire, "p");
	wire_u32(&request_wire, 0x80000000UL);
	wire_u32(&request_wire, 2);
	wire_text(&request_wire, "a@b");
	wire_text(&request_wire, "x");
	wire_text(&request_wire, "c@d");
	error = parse_exact(request_wire.data, request_wire.size, &out);
	CHECK_EQ(error, EINVAL);

	/* A count smaller than the pairs present leaves bytes over, which is refused. */
	wire_header(&request_wire, SSH_FXP_SETSTAT, 76);
	wire_text(&request_wire, "p");
	wire_u32(&request_wire, 0x80000000UL);
	wire_u32(&request_wire, 1);
	wire_text(&request_wire, "a@b");
	wire_text(&request_wire, "x");
	wire_text(&request_wire, "c@d");
	wire_text(&request_wire, "y");
	error = parse_exact(request_wire.data, request_wire.size, &out);
	CHECK_EQ(error, EINVAL);

	/* The EXTENDED flag without any count is refused. */
	wire_header(&request_wire, SSH_FXP_SETSTAT, 77);
	wire_text(&request_wire, "p");
	wire_u32(&request_wire, 0x80000000UL);
	error = parse_exact(request_wire.data, request_wire.size, &out);
	CHECK_EQ(error, EINVAL);

	/* A pair whose string length lies is refused. */
	wire_header(&request_wire, SSH_FXP_SETSTAT, 78);
	wire_text(&request_wire, "p");
	wire_u32(&request_wire, 0x80000000UL);
	wire_u32(&request_wire, 1);
	wire_u32(&request_wire, 0xffffffffUL);
	wire_bytes(&request_wire, "abcd", 4);
	error = parse_exact(request_wire.data, request_wire.size, &out);
	CHECK_EQ(error, EINVAL);
}

/*
 * Checks the path limits (draft section 6.2 and the header): a NUL in a
 * path, 4096 and 4097 bytes, for every request that has a path.
 */
static void
test_path_limits(void)
{
	static const unsigned types[7] = {
		SSH_FXP_LSTAT, SSH_FXP_OPENDIR, SSH_FXP_REMOVE, SSH_FXP_RMDIR,
		SSH_FXP_REALPATH, SSH_FXP_STAT, SSH_FXP_READLINK
	};
	struct ssh_sftp_request out;
	unsigned which;
	size_t position;
	int error;

	/* The longest path passes and one more byte does not, for every type with a path. */
	memset(trial, 'a', SSH_SFTP_PATH_MAX + 1U);
	for (which = 0; which < 7; which++) {
		wire_header(&request_wire, types[which], 81);
		wire_string(&request_wire, trial, SSH_SFTP_PATH_MAX);
		error = parse_exact(request_wire.data, request_wire.size, &out);
		CHECK_EQ(error, 0);
		CHECK_EQ(out.path_size, SSH_SFTP_PATH_MAX);
		CHECK(out.path == exact_packet + 9);
		wire_header(&request_wire, types[which], 82);
		wire_string(&request_wire, trial, SSH_SFTP_PATH_MAX + 1U);
		error = parse_exact(request_wire.data, request_wire.size, &out);
		CHECK_EQ(error, EINVAL);
		CHECK_EQ(out.id, 82);
		CHECK(out.path == NULL);
		CHECK_EQ(out.path_size, 0);
	}

	/* A NUL at the start, in the middle and at the end of a path is refused. */
	for (position = 0; position < 3; position++) {
		memcpy(trial, "abc", 3);
		trial[position * 1U] = 0;
		wire_header(&request_wire, SSH_FXP_STAT, 83);
		wire_string(&request_wire, trial, 3);
		error = parse_exact(request_wire.data, request_wire.size, &out);
		CHECK_EQ(error, EINVAL);
		CHECK(out.path == NULL);
	}

	/* A NUL as the whole path, and a NUL as the last byte of a 4096-byte path, are refused. */
	wire_header(&request_wire, SSH_FXP_STAT, 84);
	wire_string(&request_wire, "", 1);
	error = parse_exact(request_wire.data, request_wire.size, &out);
	CHECK_EQ(error, EINVAL);
	memset(trial, 'a', SSH_SFTP_PATH_MAX);
	trial[SSH_SFTP_PATH_MAX - 1U] = 0;
	wire_header(&request_wire, SSH_FXP_STAT, 85);
	wire_string(&request_wire, trial, SSH_SFTP_PATH_MAX);
	error = parse_exact(request_wire.data, request_wire.size, &out);
	CHECK_EQ(error, EINVAL);

	/* A NUL in the path of SETSTAT, MKDIR and RENAME's first path is refused too. */
	wire_header(&request_wire, SSH_FXP_SETSTAT, 86);
	wire_string(&request_wire, "a\0", 2);
	wire_attrs(&request_wire, 0, 0);
	error = parse_exact(request_wire.data, request_wire.size, &out);
	CHECK_EQ(error, EINVAL);
	wire_header(&request_wire, SSH_FXP_MKDIR, 87);
	wire_string(&request_wire, "\0a", 2);
	wire_attrs(&request_wire, 0, 0);
	error = parse_exact(request_wire.data, request_wire.size, &out);
	CHECK_EQ(error, EINVAL);
	wire_header(&request_wire, SSH_FXP_RENAME, 88);
	wire_string(&request_wire, "a\0b", 3);
	wire_text(&request_wire, "c");
	error = parse_exact(request_wire.data, request_wire.size, &out);
	CHECK_EQ(error, EINVAL);

	/* A path length that is huge and not backed by bytes is refused. */
	wire_header(&request_wire, SSH_FXP_STAT, 89);
	wire_u32(&request_wire, 0xffffffffUL);
	wire_bytes(&request_wire, "abc", 3);
	error = parse_exact(request_wire.data, request_wire.size, &out);
	CHECK_EQ(error, EINVAL);
	wire_header(&request_wire, SSH_FXP_STAT, 90);
	wire_u32(&request_wire, 0x80000000UL);
	error = parse_exact(request_wire.data, request_wire.size, &out);
	CHECK_EQ(error, EINVAL);
}

/*
 * Checks the handle limit (draft section 6: at most 256 bytes) on every
 * request that has a handle.
 */
static void
test_handle_limits(void)
{
	static const unsigned types[3] = {
		SSH_FXP_CLOSE, SSH_FXP_FSTAT, SSH_FXP_READDIR
	};
	struct ssh_sftp_request out;
	unsigned which;
	int error;

	/* A handle of 256 bytes passes and one of 257 does not, for the handle-only requests. */
	memset(trial, 'h', 257);
	for (which = 0; which < 3; which++) {
		wire_header(&request_wire, types[which], 91);
		wire_string(&request_wire, trial, 256);
		error = parse_exact(request_wire.data, request_wire.size, &out);
		CHECK_EQ(error, 0);
		CHECK_EQ(out.handle_size, 256);
		wire_header(&request_wire, types[which], 92);
		wire_string(&request_wire, trial, 257);
		error = parse_exact(request_wire.data, request_wire.size, &out);
		CHECK_EQ(error, EINVAL);
		CHECK_EQ(out.id, 92);
		CHECK(out.handle == NULL);
		CHECK_EQ(out.handle_size, 0);
	}

	/* The same limit holds for READ, WRITE and FSETSTAT. */
	wire_header(&request_wire, SSH_FXP_READ, 93);
	wire_string(&request_wire, trial, 257);
	wire_u64(&request_wire, 0);
	wire_u32(&request_wire, 1);
	error = parse_exact(request_wire.data, request_wire.size, &out);
	CHECK_EQ(error, EINVAL);
	wire_header(&request_wire, SSH_FXP_WRITE, 94);
	wire_string(&request_wire, trial, 257);
	wire_u64(&request_wire, 0);
	wire_string(&request_wire, "x", 1);
	error = parse_exact(request_wire.data, request_wire.size, &out);
	CHECK_EQ(error, EINVAL);
	wire_header(&request_wire, SSH_FXP_FSETSTAT, 95);
	wire_string(&request_wire, trial, 257);
	wire_attrs(&request_wire, 0, 0);
	error = parse_exact(request_wire.data, request_wire.size, &out);
	CHECK_EQ(error, EINVAL);
	wire_header(&request_wire, SSH_FXP_READ, 96);
	wire_string(&request_wire, trial, 256);
	wire_u64(&request_wire, 0);
	wire_u32(&request_wire, 1);
	error = parse_exact(request_wire.data, request_wire.size, &out);
	CHECK_EQ(error, 0);

	/* A handle length that is huge and not backed by bytes is refused. */
	wire_header(&request_wire, SSH_FXP_CLOSE, 97);
	wire_u32(&request_wire, 0xffffffffUL);
	error = parse_exact(request_wire.data, request_wire.size, &out);
	CHECK_EQ(error, EINVAL);
}

/*
 * Checks types that are not requests: every value of the type byte
 * other than the 19 request types and INIT.
 */
static void
test_unknown_types(void)
{
	struct ssh_sftp_request out;
	unsigned type;
	int error;
	int wrong;

	/* Each type with an id: the reply types, the gaps, 0, EXTENDED and the rest answer ENOTSUP with the id. */
	wrong = 0;
	for (type = 0; type < 256; type++) {
		if (type == SSH_FXP_INIT)
			continue;
		if (type >= SSH_FXP_OPEN && type <= SSH_FXP_SYMLINK)
			continue;
		wire_header(&request_wire, type, 0x01020304UL);
		wire_bytes(&request_wire, "\x01\x02\x03", 3);
		error = parse_exact(request_wire.data, request_wire.size, &out);
		if (error != ENOTSUP || out.type != type || out.id != 0x01020304UL)
			wrong++;
		wire_header(&request_wire, type, 0x0a0b0c0dUL);
		error = parse_exact(request_wire.data, request_wire.size, &out);
		if (error != ENOTSUP || out.type != type || out.id != 0x0a0b0c0dUL)
			wrong++;
		if (out.path != NULL || out.handle != NULL || out.data != NULL)
			wrong++;
	}
	CHECK_EQ(wrong, 0);

	/* An unknown type with fewer than the 4 bytes of an id is malformed rather than unsupported. */
	for (type = 0; type < 256; type++) {
		if (type == SSH_FXP_INIT)
			continue;
		if (type >= SSH_FXP_OPEN && type <= SSH_FXP_SYMLINK)
			continue;
		wire_reset(&request_wire);
		wire_u8(&request_wire, type);
		wire_bytes(&request_wire, "\x01\x02\x03", 3);
		error = parse_exact(request_wire.data, request_wire.size, &out);
		if (error != EINVAL || out.type != type)
			wrong++;
		error = parse_exact(request_wire.data, 1, &out);
		if (error != EINVAL || out.type != type)
			wrong++;
	}
	CHECK_EQ(wrong, 0);

	/* EXTENDED with a name and request-specific bytes is answered OP_UNSUPPORTED, not parsed further. */
	wire_header(&request_wire, SSH_FXP_EXTENDED, 99);
	wire_text(&request_wire, "posix-rename@openssh.com");
	wire_text(&request_wire, "from");
	wire_text(&request_wire, "to");
	error = parse_exact(request_wire.data, request_wire.size, &out);
	CHECK_EQ(error, ENOTSUP);
	CHECK_EQ(out.type, SSH_FXP_EXTENDED);
	CHECK_EQ(out.id, 99);

	/* A reply type sent to the server (STATUS) is also unsupported. */
	wire_header(&request_wire, SSH_FXP_STATUS, 100);
	wire_u32(&request_wire, 0);
	error = parse_exact(request_wire.data, request_wire.size, &out);
	CHECK_EQ(error, ENOTSUP);
	CHECK_EQ(out.id, 100);

	/* A packet of size 0, and one given as NULL with size 0, is malformed. */
	error = parse_exact(request_wire.data, 0, &out);
	CHECK_EQ(error, EINVAL);
	memset(&out, 0xaa, sizeof(out));
	error = ssh_sftp_parse_request(NULL, 0, &out);
	CHECK_EQ(error, EINVAL);
	CHECK_EQ(out.type, 0);
	CHECK_EQ(out.id, 0);
}

/*
 * Checks what a request holds after a failure: the type, the id when it
 * could be read, and nothing else.
 */
static void
test_failed_requests(void)
{
	struct ssh_sftp_request out;
	struct ssh_sftp_request zero;
	int error;

	/* A request with a good id and a bad path leaves only type and id. */
	memset(&zero, 0, sizeof(zero));
	wire_header(&request_wire, SSH_FXP_WRITE, 0x5a5a5a5aUL);
	wire_string(&request_wire, "H", 1);
	wire_u64(&request_wire, 0x0102030405060708ULL);
	wire_u32(&request_wire, 100);
	wire_bytes(&request_wire, "short", 5);
	error = parse_exact(request_wire.data, request_wire.size, &out);
	CHECK_EQ(error, EINVAL);
	CHECK_EQ(out.type, SSH_FXP_WRITE);
	CHECK_EQ(out.id, 0x5a5a5a5aUL);
	out.type = 0;
	out.id = 0;
	CHECK_BYTES(&out, &zero, sizeof(out));

	/* A request whose id is cut leaves only the type. */
	wire_reset(&request_wire);
	wire_u8(&request_wire, SSH_FXP_STAT);
	wire_bytes(&request_wire, "\0\0", 2);
	error = parse_exact(request_wire.data, request_wire.size, &out);
	CHECK_EQ(error, EINVAL);
	CHECK_EQ(out.type, SSH_FXP_STAT);
	out.type = 0;
	CHECK_BYTES(&out, &zero, sizeof(out));

	/* A request with trailing bytes (all fields read) leaves only type and id, not the fields it read. */
	wire_header(&request_wire, SSH_FXP_STAT, 7);
	wire_text(&request_wire, "path");
	wire_u8(&request_wire, 1);
	error = parse_exact(request_wire.data, request_wire.size, &out);
	CHECK_EQ(error, EINVAL);
	CHECK_EQ(out.id, 7);
	CHECK(out.path == NULL);
	CHECK_EQ(out.path_size, 0);

	/* A success replaces every field of a pre-filled request. */
	wire_header(&request_wire, SSH_FXP_CLOSE, 8);
	wire_string(&request_wire, "h", 1);
	error = parse_exact(request_wire.data, request_wire.size, &out);
	CHECK_EQ(error, 0);
	CHECK(out.path == NULL);
	CHECK(out.target == NULL);
	CHECK(out.data == NULL);
	CHECK_EQ(out.path_size, 0);
	CHECK_EQ(out.target_size, 0);
	CHECK_EQ(out.data_size, 0);
	CHECK_EQ(out.version, 0);
	CHECK_EQ(out.pflags, 0);
	CHECK_EQ(out.length, 0);
	CHECK(out.offset == 0);
	CHECK_EQ(out.attrs.flags, 0);
}

/*
 * Checks that every strict prefix of one request of each type is
 * malformed, and one more byte at the end is too.
 */
static void
test_prefixes_and_trailing(void)
{
	/* READ with a handle, offset and length. */
	wire_header(&request_wire, SSH_FXP_READ, 1);
	wire_string(&request_wire, "abc", 3);
	wire_u64(&request_wire, 9);
	wire_u32(&request_wire, 10);
	check_prefixes_invalid(&request_wire);

	/* WRITE with a handle, offset and data. */
	wire_header(&request_wire, SSH_FXP_WRITE, 1);
	wire_string(&request_wire, "abc", 3);
	wire_u64(&request_wire, 9);
	wire_string(&request_wire, "dat", 3);
	check_prefixes_invalid(&request_wire);

	/* OPEN with every attribute and two extended pairs. */
	wire_header(&request_wire, SSH_FXP_OPEN, 1);
	wire_text(&request_wire, "path");
	wire_u32(&request_wire, 0x1b);
	wire_attrs(&request_wire, 0x8000000fUL, 2);
	check_prefixes_invalid(&request_wire);

	/* FSETSTAT with every attribute. */
	wire_header(&request_wire, SSH_FXP_FSETSTAT, 1);
	wire_string(&request_wire, "abc", 3);
	wire_attrs(&request_wire, 0x0000000fUL, 0);
	check_prefixes_invalid(&request_wire);

	/* RENAME and SYMLINK with two paths. */
	wire_header(&request_wire, SSH_FXP_RENAME, 1);
	wire_text(&request_wire, "abc");
	wire_text(&request_wire, "def");
	check_prefixes_invalid(&request_wire);
	wire_header(&request_wire, SSH_FXP_SYMLINK, 1);
	wire_text(&request_wire, "abc");
	wire_text(&request_wire, "def");
	check_prefixes_invalid(&request_wire);
}

/*
 * Runs random and mutated packets through the parser: nothing crashes,
 * a success points inside the packet and respects the limits, and a
 * failure leaves only the type and id.
 */
static void
test_parse_random(void)
{
	struct ssh_sftp_request out;
	static const unsigned types[21] = {
		3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 1, 200, 2
	};
	uint8_t bytes[96];
	size_t count;
	size_t index;
	size_t shorter;
	unsigned type;
	int failures;
	int run;
	int error;

	/* Counts every violated expectation. */
	failures = 0;
	for (run = 0; run < 150000; run++) {
		/* A random packet with a likely type, a small id and fields that are mostly short strings. */
		type = types[next_random() % 21U];
		count = 5 + next_random() % (sizeof(bytes) - 5U);
		bytes[0] = (uint8_t)type;
		for (index = 1; index < count; index++)
			bytes[index] = (uint8_t)next_random();
		if ((next_random() % 4U) != 0) {
			/* Makes the first string's length plausible. */
			bytes[5] = 0;
			bytes[6] = 0;
			bytes[7] = 0;
			bytes[8] = (uint8_t)(next_random() % 12U);
		}
		if ((next_random() % 4U) == 0)
			bytes[0] = (uint8_t)next_random();

		/* Parses a copy of exactly the length, then a random shorter copy. */
		error = parse_exact(bytes, count, &out);
		if (error != 0 && error != EINVAL && error != ENOTSUP)
			failures++;
		if (error != 0) {
			if (out.path != NULL || out.handle != NULL || out.data != NULL || out.target != NULL)
				failures++;
		} else {
			if (out.path != NULL && (out.path < exact_packet || out.path + out.path_size > exact_packet + count))
				failures++;
			if (out.target != NULL && (out.target < exact_packet || out.target + out.target_size > exact_packet + count))
				failures++;
			if (out.handle != NULL && (out.handle < exact_packet || out.handle + out.handle_size > exact_packet + count))
				failures++;
			if (out.data != NULL && (out.data < exact_packet || out.data + out.data_size > exact_packet + count))
				failures++;
			if (out.path_size > SSH_SFTP_PATH_MAX || out.target_size > SSH_SFTP_PATH_MAX)
				failures++;
			if (out.handle_size > 256 || out.length > SSH_SFTP_READ_MAX)
				failures++;
			if ((out.attrs.flags & ~0x0fUL) != 0)
				failures++;
			if (out.path != NULL && memchr(out.path, 0, out.path_size) != NULL)
				failures++;
			if (out.target != NULL && memchr(out.target, 0, out.target_size) != NULL)
				failures++;
		}
		shorter = next_random() % (count + 1U);
		error = parse_exact(bytes, shorter, &out);
		if (error != 0 && error != EINVAL && error != ENOTSUP)
			failures++;
	}
	CHECK_EQ(failures, 0);

	/* A valid request mutated in one byte (every position, a few values) never crashes the parser. */
	wire_header(&request_wire, SSH_FXP_OPEN, 1);
	wire_text(&request_wire, "path");
	wire_u32(&request_wire, 0x1b);
	wire_attrs(&request_wire, 0x8000000fUL, 2);
	for (index = 0; index < request_wire.size; index++) {
		for (type = 0; type < 6; type++) {
			memcpy(trial, request_wire.data, request_wire.size);
			trial[index] = (uint8_t)(type == 0 ? 0 : type == 1 ? 0xff : type == 2 ? trial[index] + 1 : type == 3 ? trial[index] - 1 : type == 4 ? 0x80 : 0x7f);
			error = parse_exact(trial, request_wire.size, &out);
			if (error != 0 && error != EINVAL && error != ENOTSUP)
				failures++;
		}
	}
	CHECK_EQ(failures, 0);
}

/*
 * Checks the VERSION packet: length 5, type 2, version 3, nothing more.
 */
static void
test_put_version(void)
{
	struct ssh_buf out;
	static const uint8_t want[9] = { 0, 0, 0, 5, 2, 0, 0, 0, 3 };

	/* The packet byte for byte (draft sections 3 and 4). */
	ssh_buf_init(&out, 1024, 0);
	ssh_sftp_put_version(&out);
	CHECK_EQ(out.size, 9);
	CHECK_BYTES(out.data, want, 9);
	CHECK_EQ(out.error, 0);

	/* A second packet appends after the first, with its own length. */
	ssh_sftp_put_version(&out);
	CHECK_EQ(out.size, 18);
	CHECK_BYTES(out.data + 9, want, 9);
	ssh_buf_free(&out);
}

/*
 * Checks the STATUS packet (draft section 7): id, code, message, an empty
 * language tag.
 */
static void
test_put_status(void)
{
	struct ssh_buf out;
	uint32_t code;

	/* The packet for each defined code with a message. */
	for (code = 0; code <= 8; code++) {
		ssh_buf_init(&out, 1024, 0);
		ssh_sftp_put_status(0x01020304UL, code, "No such file", &out);
		wire_reset(&expected);
		wire_u8(&expected, 101);
		wire_u32(&expected, 0x01020304UL);
		wire_u32(&expected, code);
		wire_text(&expected, "No such file");
		wire_string(&expected, "", 0);
		expect_packet(&out, &expected, "status packet");
		ssh_buf_free(&out);
	}

	/* An empty message and a NULL message are both the empty string. */
	ssh_buf_init(&out, 1024, 0);
	ssh_sftp_put_status(9, SSH_FX_OK, "", &out);
	wire_reset(&expected);
	wire_u8(&expected, 101);
	wire_u32(&expected, 9);
	wire_u32(&expected, 0);
	wire_string(&expected, "", 0);
	wire_string(&expected, "", 0);
	expect_packet(&out, &expected, "status with an empty message");
	ssh_buf_reset(&out);
	ssh_sftp_put_status(9, SSH_FX_OK, NULL, &out);
	expect_packet(&out, &expected, "status with a NULL message");

	/* The length is 4 + 4 + 4 + 4 + 4 + 1 = 17 for an empty message: type, id, code, two lengths. */
	CHECK_EQ(out.size, 4 + 1 + 4 + 4 + 4 + 4);
	CHECK_EQ(out.data[3], 17);
	ssh_buf_free(&out);

	/* The largest id and code survive. */
	ssh_buf_init(&out, 1024, 0);
	ssh_sftp_put_status(0xffffffffUL, 0xffffffffUL, "x", &out);
	wire_reset(&expected);
	wire_u8(&expected, 101);
	wire_u32(&expected, 0xffffffffUL);
	wire_u32(&expected, 0xffffffffUL);
	wire_text(&expected, "x");
	wire_string(&expected, "", 0);
	expect_packet(&out, &expected, "status with the largest values");
	ssh_buf_free(&out);
}

/*
 * Checks the HANDLE packet (draft section 7), including the 256-byte limit.
 */
static void
test_put_handle(void)
{
	struct ssh_buf out;
	uint8_t handle[257];
	size_t index;

	/* A short handle with NUL bytes. */
	ssh_buf_init(&out, 4096, 0);
	ssh_sftp_put_handle(0x0a0b0c0dUL, (const uint8_t *)"\x00\x01\x00\xff", 4, &out);
	wire_reset(&expected);
	wire_u8(&expected, 102);
	wire_u32(&expected, 0x0a0b0c0dUL);
	wire_string(&expected, "\x00\x01\x00\xff", 4);
	expect_packet(&out, &expected, "handle packet");
	ssh_buf_free(&out);

	/* An empty handle. */
	ssh_buf_init(&out, 4096, 0);
	ssh_sftp_put_handle(1, NULL, 0, &out);
	wire_reset(&expected);
	wire_u8(&expected, 102);
	wire_u32(&expected, 1);
	wire_string(&expected, "", 0);
	expect_packet(&out, &expected, "empty handle packet");
	ssh_buf_free(&out);

	/* 256 bytes is allowed. */
	for (index = 0; index < sizeof(handle); index++)
		handle[index] = (uint8_t)(index + 1U);
	ssh_buf_init(&out, 4096, 0);
	ssh_sftp_put_handle(2, handle, 256, &out);
	wire_reset(&expected);
	wire_u8(&expected, 102);
	wire_u32(&expected, 2);
	wire_string(&expected, handle, 256);
	expect_packet(&out, &expected, "256-byte handle packet");
	ssh_buf_free(&out);

	/* 257 bytes is refused with EMSGSIZE and nothing is written. */
	ssh_buf_init(&out, 4096, 0);
	ssh_sftp_put_handle(3, handle, 257, &out);
	CHECK_EQ(out.error, EMSGSIZE);
	CHECK_EQ(out.size, 0);
	ssh_buf_free(&out);

	/* The refusal does not leave a half packet after an earlier packet. */
	ssh_buf_init(&out, 4096, 0);
	ssh_sftp_put_version(&out);
	ssh_sftp_put_handle(3, handle, 300, &out);
	CHECK_EQ(out.error, EMSGSIZE);
	CHECK_EQ(out.size, 9);
	ssh_buf_free(&out);
}

/*
 * Checks the DATA packet (draft section 7).
 */
static void
test_put_data(void)
{
	struct ssh_buf out;
	static uint8_t block[SSH_SFTP_READ_MAX];
	size_t index;

	/* Five bytes. */
	ssh_buf_init(&out, 1024UL * 1024UL, 0);
	ssh_sftp_put_data(0x11223344UL, (const uint8_t *)"hello", 5, &out);
	wire_reset(&expected);
	wire_u8(&expected, 103);
	wire_u32(&expected, 0x11223344UL);
	wire_string(&expected, "hello", 5);
	expect_packet(&out, &expected, "data packet");
	ssh_buf_free(&out);

	/* No bytes, with a NULL pointer. */
	ssh_buf_init(&out, 1024, 0);
	ssh_sftp_put_data(4, NULL, 0, &out);
	wire_reset(&expected);
	wire_u8(&expected, 103);
	wire_u32(&expected, 4);
	wire_string(&expected, "", 0);
	expect_packet(&out, &expected, "empty data packet");
	ssh_buf_free(&out);

	/* A full 64 KiB read reply. */
	for (index = 0; index < sizeof(block); index++)
		block[index] = (uint8_t)(index * 13U + 1U);
	ssh_buf_init(&out, 1024UL * 1024UL, 0);
	ssh_sftp_put_data(5, block, sizeof(block), &out);
	wire_reset(&expected);
	wire_u8(&expected, 103);
	wire_u32(&expected, 5);
	wire_string(&expected, block, sizeof(block));
	expect_packet(&out, &expected, "64 KiB data packet");
	ssh_buf_free(&out);
}

/*
 * Checks the ATTRS packet (draft sections 5 and 7) for every
 * combination of flags, and that flags this module does not write are
 * dropped.
 */
static void
test_put_attrs(void)
{
	struct ssh_buf out;
	struct ssh_sftp_attrs attrs;
	uint32_t flags;

	/* All 16 combinations of the four flags: the fields follow the flags in the draft's order. */
	for (flags = 0; flags < 16; flags++) {
		attrs.flags = flags;
		attrs.size = ((uint64_t)VALUE_SIZE_HIGH << 32) | (uint64_t)VALUE_SIZE_LOW;
		attrs.uid = VALUE_UID;
		attrs.gid = VALUE_GID;
		attrs.permissions = VALUE_PERMISSIONS;
		attrs.atime = VALUE_ATIME;
		attrs.mtime = VALUE_MTIME;
		ssh_buf_init(&out, 1024, 0);
		ssh_sftp_put_attrs(0x0badf00dUL, &attrs, &out);
		wire_reset(&expected);
		wire_u8(&expected, 105);
		wire_u32(&expected, 0x0badf00dUL);
		wire_attrs(&expected, flags, 0);
		expect_packet(&out, &expected, "attrs packet");
		ssh_buf_free(&out);
	}

	/* The EXTENDED bit and the undefined bits are not written, and no extended count is. */
	attrs.flags = 0xffffffffUL;
	ssh_buf_init(&out, 1024, 0);
	ssh_sftp_put_attrs(1, &attrs, &out);
	wire_reset(&expected);
	wire_u8(&expected, 105);
	wire_u32(&expected, 1);
	wire_attrs(&expected, 0x0000000fUL, 0);
	expect_packet(&out, &expected, "attrs packet with extra flags");
	ssh_buf_free(&out);
	attrs.flags = 0x80000000UL;
	ssh_buf_init(&out, 1024, 0);
	ssh_sftp_put_attrs(1, &attrs, &out);
	wire_reset(&expected);
	wire_u8(&expected, 105);
	wire_u32(&expected, 1);
	wire_u32(&expected, 0);
	expect_packet(&out, &expected, "attrs packet with only EXTENDED");
	ssh_buf_free(&out);
}

/*
 * Checks the NAME packet built with begin_name, entries and end_packet
 * (draft section 7).
 */
static void
test_name_packet(void)
{
	struct ssh_buf out;
	struct ssh_sftp_attrs full;
	struct ssh_sftp_attrs none;
	size_t start;
	int error;

	/* Two entries, one with every attribute and one with none. */
	full.flags = 0x0000000fUL;
	full.size = 0x0000000100000002ULL;
	full.uid = 3;
	full.gid = 4;
	full.permissions = 0100644UL;
	full.atime = 5;
	full.mtime = 6;
	memset(&none, 0, sizeof(none));
	ssh_buf_init(&out, 4096, 0);
	ssh_sftp_begin_name(0x55667788UL, 2, &out, &start);
	CHECK_EQ(start, 0);
	ssh_sftp_put_name_entry("file.txt", "-rw-r--r-- 1 u g 4 Jan 1 1970 file.txt", &full, &out);
	ssh_sftp_put_name_entry("dir", "ld", &none, &out);
	error = ssh_sftp_end_packet(&out, start);
	CHECK_EQ(error, 0);
	wire_reset(&expected);
	wire_u8(&expected, 104);
	wire_u32(&expected, 0x55667788UL);
	wire_u32(&expected, 2);
	wire_text(&expected, "file.txt");
	wire_text(&expected, "-rw-r--r-- 1 u g 4 Jan 1 1970 file.txt");
	wire_u32(&expected, 0x0000000fUL);
	wire_u32(&expected, 1);
	wire_u32(&expected, 2);
	wire_u32(&expected, 3);
	wire_u32(&expected, 4);
	wire_u32(&expected, 0100644UL);
	wire_u32(&expected, 5);
	wire_u32(&expected, 6);
	wire_text(&expected, "dir");
	wire_text(&expected, "ld");
	wire_u32(&expected, 0);
	expect_packet(&out, &expected, "name packet");
	ssh_buf_free(&out);

	/* No entries: type, id and a count of 0. */
	ssh_buf_init(&out, 4096, 0);
	ssh_sftp_begin_name(1, 0, &out, &start);
	error = ssh_sftp_end_packet(&out, start);
	CHECK_EQ(error, 0);
	wire_reset(&expected);
	wire_u8(&expected, 104);
	wire_u32(&expected, 1);
	wire_u32(&expected, 0);
	expect_packet(&out, &expected, "empty name packet");
	ssh_buf_free(&out);

	/* A packet that starts after other bytes records its start, and the earlier bytes are not touched. */
	ssh_buf_init(&out, 4096, 0);
	ssh_sftp_put_version(&out);
	ssh_sftp_begin_name(2, 1, &out, &start);
	CHECK_EQ(start, 9);
	ssh_sftp_put_name_entry("/", "d", &none, &out);
	error = ssh_sftp_end_packet(&out, start);
	CHECK_EQ(error, 0);
	CHECK_EQ(out.size, 9 + 4 + 1 + 4 + 4 + (4 + 1) + (4 + 1) + 4);
	CHECK_BYTES(out.data, "\x00\x00\x00\x05\x02\x00\x00\x00\x03", 9);
	CHECK_EQ(out.data[12], out.size - 9 - 4);
	CHECK_EQ(out.data[9], 0);
	CHECK_EQ(out.data[10], 0);
	CHECK_EQ(out.data[11], 0);
	ssh_buf_free(&out);

	/* A NULL filename or longname is the empty string. */
	ssh_buf_init(&out, 4096, 0);
	ssh_sftp_begin_name(3, 1, &out, &start);
	ssh_sftp_put_name_entry(NULL, NULL, &none, &out);
	error = ssh_sftp_end_packet(&out, start);
	CHECK_EQ(error, 0);
	wire_reset(&expected);
	wire_u8(&expected, 104);
	wire_u32(&expected, 3);
	wire_u32(&expected, 1);
	wire_string(&expected, "", 0);
	wire_string(&expected, "", 0);
	wire_u32(&expected, 0);
	expect_packet(&out, &expected, "name packet with empty names");
	ssh_buf_free(&out);
}

/*
 * Checks ssh_sftp_end_packet on its own: bad starts, a recorded error and
 * the packet limit.
 */
static void
test_end_packet(void)
{
	struct ssh_buf out;
	static char name[SSH_SFTP_PACKET_MAX];
	struct ssh_sftp_attrs none;
	size_t start;
	int error;

	/* An empty buffer, and a start beyond the end, are not packets. */
	ssh_buf_init(&out, 1024UL * 1024UL, 0);
	error = ssh_sftp_end_packet(&out, 0);
	CHECK_EQ(error, EINVAL);
	ssh_buf_put_bytes(&out, "\0\0\0\0\x68", 5);
	error = ssh_sftp_end_packet(&out, 1);
	CHECK_EQ(error, EINVAL);
	error = ssh_sftp_end_packet(&out, 5);
	CHECK_EQ(error, EINVAL);
	error = ssh_sftp_end_packet(&out, 6);
	CHECK_EQ(error, EINVAL);
	error = ssh_sftp_end_packet(&out, (size_t)-1);
	CHECK_EQ(error, EINVAL);

	/* A length field and a type are the smallest packet: its length is 1. */
	error = ssh_sftp_end_packet(&out, 0);
	CHECK_EQ(error, 0);
	CHECK_BYTES(out.data, "\x00\x00\x00\x01\x68", 5);
	ssh_buf_free(&out);

	/* A recorded error is reported and the length is not written. */
	ssh_buf_init(&out, 8, 0);
	ssh_buf_put_bytes(&out, "\0\0\0\0\x68", 5);
	ssh_buf_put_bytes(&out, "123456789", 9);
	CHECK_EQ(out.error, EMSGSIZE);
	error = ssh_sftp_end_packet(&out, 0);
	CHECK_EQ(error, EMSGSIZE);
	CHECK_BYTES(out.data, "\x00\x00\x00\x00\x68", 5);
	ssh_buf_free(&out);

	/* A name packet of exactly SSH_SFTP_PACKET_MAX bytes after its length field is accepted: 9 + (4 + f) + 4 + 4 = 21 + f. */
	memset(name, 'n', sizeof(name) - 1U);
	name[SSH_SFTP_PACKET_MAX - 21U] = '\0';
	memset(&none, 0, sizeof(none));
	ssh_buf_init(&out, 1024UL * 1024UL, 0);
	ssh_sftp_begin_name(1, 1, &out, &start);
	ssh_sftp_put_name_entry(name, "", &none, &out);
	CHECK_EQ(out.size, 4U + SSH_SFTP_PACKET_MAX);
	error = ssh_sftp_end_packet(&out, start);
	CHECK_EQ(error, 0);
	CHECK(ssh_load_be32(out.data) == SSH_SFTP_PACKET_MAX);
	ssh_buf_free(&out);

	/* One byte more is too long to be accepted by the reading side. */
	memset(name, 'n', sizeof(name) - 1U);
	name[SSH_SFTP_PACKET_MAX - 20U] = '\0';
	ssh_buf_init(&out, 1024UL * 1024UL, 0);
	ssh_sftp_begin_name(1, 1, &out, &start);
	ssh_sftp_put_name_entry(name, "", &none, &out);
	CHECK_EQ(out.size, 4U + SSH_SFTP_PACKET_MAX + 1U);
	error = ssh_sftp_end_packet(&out, start);
	CHECK_EQ(error, EMSGSIZE);
	ssh_buf_free(&out);
}

/*
 * Checks that the self-contained put functions report a packet that is
 * too long or a buffer that is too small through the buffer's error.
 */
static void
test_put_limits(void)
{
	struct ssh_buf out;
	static uint8_t block[SSH_SFTP_PACKET_MAX];
	int error;

	/* A DATA packet of exactly SSH_SFTP_PACKET_MAX after the length field: 1 + 4 + 4 + n. */
	memset(block, 0x5a, sizeof(block));
	ssh_buf_init(&out, 1024UL * 1024UL, 0);
	ssh_sftp_put_data(1, block, SSH_SFTP_PACKET_MAX - 9U, &out);
	CHECK_EQ(out.error, 0);
	CHECK(ssh_load_be32(out.data) == SSH_SFTP_PACKET_MAX);
	ssh_buf_free(&out);

	/* One byte more records EMSGSIZE in the buffer. */
	ssh_buf_init(&out, 1024UL * 1024UL, 0);
	ssh_sftp_put_data(1, block, SSH_SFTP_PACKET_MAX - 8U, &out);
	CHECK_EQ(out.error, EMSGSIZE);
	ssh_buf_free(&out);

	/* A buffer whose own limit is too small keeps its error, for every put function. */
	ssh_buf_init(&out, 10, 0);
	ssh_sftp_put_status(1, 0, "message long enough", &out);
	CHECK_EQ(out.error, EMSGSIZE);
	error = ssh_sftp_end_packet(&out, 0);
	CHECK_EQ(error, EMSGSIZE);
	ssh_buf_free(&out);
	ssh_buf_init(&out, 8, 0);
	ssh_sftp_put_version(&out);
	CHECK_EQ(out.error, EMSGSIZE);
	ssh_buf_free(&out);
	ssh_buf_init(&out, 12, 0);
	ssh_sftp_put_data(1, block, 100, &out);
	CHECK_EQ(out.error, EMSGSIZE);
	ssh_buf_free(&out);
	ssh_buf_init(&out, 12, 0);
	ssh_sftp_put_handle(1, block, 100, &out);
	CHECK_EQ(out.error, EMSGSIZE);
	ssh_buf_free(&out);

	/* The limit exactly met by a version packet is fine. */
	ssh_buf_init(&out, 9, 0);
	ssh_sftp_put_version(&out);
	CHECK_EQ(out.error, 0);
	CHECK_EQ(out.size, 9);
	ssh_buf_free(&out);
}

/*
 * Checks that the packets the module writes are framed the way
 * next_packet reads: written, then taken back as the body without the
 * length.
 */
static void
test_roundtrip_framing(void)
{
	struct ssh_buf out;
	struct ssh_buf packet;
	int error;

	/* A STATUS then a DATA packet in one stream, taken one by one. */
	ssh_buf_init(&out, 4096, 0);
	ssh_buf_init(&packet, 4096, 0);
	ssh_sftp_put_status(7, SSH_FX_EOF, "End of file", &out);
	ssh_sftp_put_data(8, (const uint8_t *)"xyz", 3, &out);

	/* The status first. */
	error = ssh_sftp_next_packet(&out, &packet);
	CHECK_EQ(error, 0);
	wire_reset(&expected);
	wire_u8(&expected, 101);
	wire_u32(&expected, 7);
	wire_u32(&expected, 1);
	wire_text(&expected, "End of file");
	wire_string(&expected, "", 0);
	CHECK_EQ(packet.size, expected.size);
	CHECK_BYTES(packet.data, expected.data, expected.size);

	/* The data second. */
	ssh_buf_reset(&packet);
	error = ssh_sftp_next_packet(&out, &packet);
	CHECK_EQ(error, 0);
	wire_reset(&expected);
	wire_u8(&expected, 103);
	wire_u32(&expected, 8);
	wire_string(&expected, "xyz", 3);
	CHECK_EQ(packet.size, expected.size);
	CHECK_BYTES(packet.data, expected.data, expected.size);
	CHECK_EQ(out.size, 0);
	ssh_buf_free(&out);
	ssh_buf_free(&packet);
}

/*
 * Builds a long name into out (capacity bytes), a thin step so the
 * checks below read as their expected strings.
 */
static void
make_long_name(
	const struct ssh_sftp_attrs *attrs,
	uint32_t links,
	const char *owner,
	const char *group,
	const char *name,
	int64_t now,
	char *out,
	size_t capacity)
{
	/* Builds the long name. */
	ssh_sftp_long_name(attrs, links, owner, group, name, now, out, capacity);
}

/*
 * Checks the longname of draft section 7 on its own example line, and a
 * directory.
 */
static void
test_long_name_forms(void)
{
	struct ssh_sftp_attrs attrs;
	char out[200];
	int64_t mtime;

	/*
	 * The draft's example "-rwxr-xr-x   1 mjos     staff      348911 Mar 25 14:29 t-filexfer".
	 * 2001-03-25 14:29:00 UTC is day 11406 after 1970-01-01 (31 years of
	 * 365 days plus 8 leap days is 11323 to 2001-01-01, then 31 + 28 + 24
	 * days), so 11406 * 86400 + 14 * 3600 + 29 * 60 = 985530540.
	 */
	mtime = 985530540;
	memset(&attrs, 0, sizeof(attrs));
	attrs.flags = SSH_FILEXFER_ATTR_SIZE | SSH_FILEXFER_ATTR_PERMISSIONS | SSH_FILEXFER_ATTR_ACMODTIME;
	attrs.size = 348911;
	attrs.permissions = 0100755UL;
	attrs.mtime = (uint32_t)mtime;
	make_long_name(&attrs, 1, "mjos", "staff", "t-filexfer", mtime + 86400 * 10, out, sizeof(out));
	CHECK_TEXT(out, "-rwxr-xr-x   1 mjos     staff      348911 Mar 25 14:29 t-filexfer");

	/* A directory with a long owner (the field grows), a 4-digit link count and no size. */
	memset(&attrs, 0, sizeof(attrs));
	attrs.flags = SSH_FILEXFER_ATTR_PERMISSIONS | SSH_FILEXFER_ATTR_ACMODTIME;
	attrs.permissions = 040755UL;
	attrs.mtime = (uint32_t)mtime;
	make_long_name(&attrs, 1234, "verylongowner", "g", "sub dir", mtime, out, sizeof(out));
	CHECK_TEXT(out, "drwxr-xr-x" " " "1234" " " "verylongowner" " " "g       " " " "       0" " " "Mar 25 14:29" " " "sub dir");
}

/*
 * Checks every file kind and the special permission bits.
 */
static void
test_long_name_permissions(void)
{
	struct ssh_sftp_attrs attrs;
	char out[200];
	size_t index;
	static const uint32_t modes[] = {
		0140000UL | 0777, 0120000UL | 0777, 0100000UL | 0644, 0060000UL | 0660,
		0040000UL | 0755, 0020000UL | 0620, 0010000UL | 0600, 0100000UL | 0000,
		0100000UL | 04755, 0100000UL | 04644, 0100000UL | 02755, 0100000UL | 02705,
		0100000UL | 01777, 0100000UL | 01776, 0100000UL | 07777, 0100000UL | 07000,
		0170000UL | 0777, 0000000UL | 0644
	};
	static const char *const want[] = {
		"srwxrwxrwx", "lrwxrwxrwx", "-rw-r--r--", "brw-rw----",
		"drwxr-xr-x", "crw--w----", "prw-------", "----------",
		"-rwsr-xr-x", "-rwSr--r--", "-rwxr-sr-x", "-rwx--Sr-x",
		"-rwxrwxrwt", "-rwxrwxrwT", "-rwsrwsrwt", "---S--S--T",
		"?rwxrwxrwx", "?rw-r--r--"
	};
	char expected_text[200];

	/* Each mode becomes its ten characters, computed by hand from the bit layout. */
	for (index = 0; index < sizeof(modes) / sizeof(modes[0]); index++) {
		memset(&attrs, 0, sizeof(attrs));
		attrs.flags = SSH_FILEXFER_ATTR_PERMISSIONS;
		attrs.permissions = modes[index];
		make_long_name(&attrs, 1, "o", "g", "n", 0, out, sizeof(out));
		strcpy(expected_text, want[index]);
		strcat(expected_text, LN_AFTER_MODE);
		CHECK_TEXT(out, expected_text);
	}

	/* A mode with bits above the file-type field (a stray high bit) is shown by its low bits only. */
	memset(&attrs, 0, sizeof(attrs));
	attrs.flags = SSH_FILEXFER_ATTR_PERMISSIONS;
	attrs.permissions = 0xfff00000UL | 0100000UL | 0644;
	make_long_name(&attrs, 1, "o", "g", "n", 0, out, sizeof(out));
	CHECK(strncmp(out, "-rw-r--r--   1", 14) == 0);

	/* Without the permissions flag the mode is unknown. */
	memset(&attrs, 0, sizeof(attrs));
	attrs.permissions = 0100755UL;
	make_long_name(&attrs, 1, "o", "g", "n", 0, out, sizeof(out));
	CHECK(strncmp(out, "?---------   1", 14) == 0);
}

/*
 * Checks the date field: the time of day within six months, the year
 * beyond, and the calendar edges.
 */
static void
test_long_name_dates(void)
{
	struct ssh_sftp_attrs attrs;
	char out[200];
	int64_t mtime;
	int64_t half_year;

	/* Six months is 15778476 seconds (half of a mean year of 31556952): 15778475 seconds back shows the time. */
	half_year = 15778476;
	mtime = 985530540;
	memset(&attrs, 0, sizeof(attrs));
	attrs.flags = SSH_FILEXFER_ATTR_ACMODTIME;
	attrs.mtime = (uint32_t)mtime;
	make_long_name(&attrs, 1, "o", "g", "n", mtime + half_year - 1, out, sizeof(out));
	CHECK_TEXT(out, LN_Q_O_G_0 "Mar 25 14:29 n");

	/* ... and exactly six months back shows the year. */
	make_long_name(&attrs, 1, "o", "g", "n", mtime + half_year, out, sizeof(out));
	CHECK_TEXT(out, LN_Q_O_G_0 "Mar 25  2001 n");
	make_long_name(&attrs, 1, "o", "g", "n", mtime + half_year + 1, out, sizeof(out));
	CHECK_TEXT(out, LN_Q_O_G_0 "Mar 25  2001 n");

	/* A time equal to now is recent; one second in the future is not. */
	make_long_name(&attrs, 1, "o", "g", "n", mtime, out, sizeof(out));
	CHECK_TEXT(out, LN_Q_O_G_0 "Mar 25 14:29 n");
	make_long_name(&attrs, 1, "o", "g", "n", mtime - 1, out, sizeof(out));
	CHECK_TEXT(out, LN_Q_O_G_0 "Mar 25  2001 n");

	/* Extreme values of now give the year form without overflow. */
	make_long_name(&attrs, 1, "o", "g", "n", (-9223372036854775807LL - 1LL), out, sizeof(out));
	CHECK_TEXT(out, LN_Q_O_G_0 "Mar 25  2001 n");
	make_long_name(&attrs, 1, "o", "g", "n", 9223372036854775807LL, out, sizeof(out));
	CHECK_TEXT(out, LN_Q_O_G_0 "Mar 25  2001 n");

	/* The epoch itself: day 1, "Jan  1", at 00:00, a single-digit day padded with a space. */
	attrs.mtime = 0;
	make_long_name(&attrs, 1, "o", "g", "n", 100, out, sizeof(out));
	CHECK_TEXT(out, LN_Q_O_G_0 "Jan  1 00:00 n");
	make_long_name(&attrs, 1, "o", "g", "n", 100000000, out, sizeof(out));
	CHECK_TEXT(out, LN_Q_O_G_0 "Jan  1  1970 n");

	/* 1999-12-31 23:59:59 is 2000-01-01 (10957 days, 946684800 s) minus a second. */
	attrs.mtime = 946684799UL;
	make_long_name(&attrs, 1, "o", "g", "n", 946684799, out, sizeof(out));
	CHECK_TEXT(out, LN_Q_O_G_0 "Dec 31 23:59 n");
	make_long_name(&attrs, 1, "o", "g", "n", 946684799 + 100000000, out, sizeof(out));
	CHECK_TEXT(out, LN_Q_O_G_0 "Dec 31  1999 n");

	/* The leap day of 2000 (a year divisible by 400): day 11016, 951782400 s; the next day is Mar 1. */
	attrs.mtime = 951782400UL;
	make_long_name(&attrs, 1, "o", "g", "n", 951782400, out, sizeof(out));
	CHECK_TEXT(out, LN_Q_O_G_0 "Feb 29 00:00 n");
	attrs.mtime = 951782400UL + 86400UL;
	make_long_name(&attrs, 1, "o", "g", "n", 951782400 + 86400, out, sizeof(out));
	CHECK_TEXT(out, LN_Q_O_G_0 "Mar  1 00:00 n");

	/* 1900 is not a leap year but is before the epoch; 2100 is a year divisible by 100 and not 400: Mar 1 2100 follows Feb 28. */
	/* 2100-02-28 00:00:00 UTC is day 47540 (130 years of 365 days plus 32 leap days is 47482 days to 2100-01-01, then 31 + 27). */
	attrs.mtime = (uint32_t)(47540UL * 86400UL);
	make_long_name(&attrs, 1, "o", "g", "n", (int64_t)attrs.mtime, out, sizeof(out));
	CHECK_TEXT(out, LN_Q_O_G_0 "Feb 28 00:00 n");
	attrs.mtime = (uint32_t)(47541UL * 86400UL);
	make_long_name(&attrs, 1, "o", "g", "n", (int64_t)attrs.mtime, out, sizeof(out));
	CHECK_TEXT(out, LN_Q_O_G_0 "Mar  1 00:00 n");

	/* The largest 32-bit time is 2106-02-07 06:28:15 UTC. */
	attrs.mtime = 0xffffffffUL;
	make_long_name(&attrs, 1, "o", "g", "n", 4294967295LL, out, sizeof(out));
	CHECK_TEXT(out, LN_Q_O_G_0 "Feb  7 06:28 n");
	make_long_name(&attrs, 1, "o", "g", "n", 0, out, sizeof(out));
	CHECK_TEXT(out, LN_Q_O_G_0 "Feb  7  2106 n");

	/* Minutes and hours with a leading zero: 1970-01-01 01:02 is 3720 seconds. */
	attrs.mtime = 3720;
	make_long_name(&attrs, 1, "o", "g", "n", 3720, out, sizeof(out));
	CHECK_TEXT(out, LN_Q_O_G_0 "Jan  1 01:02 n");

	/* Each month name once: the first day of each month of 1971 (365 days, non-leap), hand-summed day numbers. */
	{
		static const unsigned first_day[12] = {
			365, 396, 424, 455, 485, 516, 546, 577, 608, 638, 669, 699
		};
		static const char *const names[12] = {
			"Jan", "Feb", "Mar", "Apr", "May", "Jun",
			"Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
		};
		char want[100];
		unsigned month;

		/* Each first day, at 12:00 so that a wrong day shows. */
		for (month = 0; month < 12; month++) {
			attrs.mtime = first_day[month] * 86400U + 12U * 3600U;
			make_long_name(&attrs, 1, "o", "g", "n", (int64_t)attrs.mtime, out, sizeof(out));
			strcpy(want, LN_Q_O_G_0 "");
			strcat(want, names[month]);
			strcat(want, "  1 12:00 n");
			CHECK_TEXT(out, want);
		}
	}
}

/*
 * Checks the other fields: link count, owner, group, size, name.
 */
static void
test_long_name_fields(void)
{
	struct ssh_sftp_attrs attrs;
	char out[300];

	/* The largest size and link count are written in full, 20 and 10 digits. */
	memset(&attrs, 0, sizeof(attrs));
	attrs.flags = SSH_FILEXFER_ATTR_SIZE | SSH_FILEXFER_ATTR_PERMISSIONS | SSH_FILEXFER_ATTR_ACMODTIME;
	attrs.size = 0xffffffffffffffffULL;
	attrs.permissions = 0100600UL;
	attrs.mtime = 0;
	make_long_name(&attrs, 0xffffffffUL, "root", "wheel", "big", 1000, out, sizeof(out));
	CHECK_TEXT(out, "-rw-------" " " "4294967295" " " "root    " " " "wheel   " " " "18446744073709551615" " " "Jan  1 00:00" " " "big");

	/* A zero size and a link count of 0 are padded to their widths. */
	attrs.size = 0;
	make_long_name(&attrs, 0, "root", "wheel", "z", 1000, out, sizeof(out));
	CHECK_TEXT(out, "-rw-------" " " "  0" " " "root    " " " "wheel   " " " "       0" " " "Jan  1 00:00" " " "z");

	/* Sizes of 7, 8 and 9 digits around the width of 8. */
	attrs.size = 1234567;
	make_long_name(&attrs, 1, "root", "wheel", "s", 1000, out, sizeof(out));
	CHECK_TEXT(out, "-rw-------" " " "  1" " " "root    " " " "wheel   " " " " 1234567" " " "Jan  1 00:00" " " "s");
	attrs.size = 12345678;
	make_long_name(&attrs, 1, "root", "wheel", "s", 1000, out, sizeof(out));
	CHECK_TEXT(out, "-rw-------" " " "  1" " " "root    " " " "wheel   " " " "12345678" " " "Jan  1 00:00" " " "s");
	attrs.size = 123456789;
	make_long_name(&attrs, 1, "root", "wheel", "s", 1000, out, sizeof(out));
	CHECK_TEXT(out, "-rw-------" " " "  1" " " "root    " " " "wheel   " " " "123456789" " " "Jan  1 00:00" " " "s");

	/* Owner and group of exactly 8 characters take no padding, 9 grow the field. */
	attrs.size = 5;
	make_long_name(&attrs, 1, "12345678", "abcdefgh", "s", 1000, out, sizeof(out));
	CHECK_TEXT(out, "-rw-------" " " "  1" " " "12345678" " " "abcdefgh" " " "       5" " " "Jan  1 00:00" " " "s");
	make_long_name(&attrs, 1, "123456789", "abcdefghi", "s", 1000, out, sizeof(out));
	CHECK_TEXT(out, "-rw-------" " " "  1" " " "123456789" " " "abcdefghi" " " "       5" " " "Jan  1 00:00" " " "s");

	/* Numeric owner and group, and an empty owner (padded as nothing), and NULL (shown as a question mark). */
	make_long_name(&attrs, 1, "1000", "1000", "s", 1000, out, sizeof(out));
	CHECK_TEXT(out, "-rw-------" " " "  1" " " "1000    " " " "1000    " " " "       5" " " "Jan  1 00:00" " " "s");
	make_long_name(&attrs, 1, "", "", "s", 1000, out, sizeof(out));
	CHECK_TEXT(out, "-rw-------" " " "  1" " " "        " " " "        " " " "       5" " " "Jan  1 00:00" " " "s");
	make_long_name(&attrs, 1, NULL, NULL, "s", 1000, out, sizeof(out));
	CHECK_TEXT(out, "-rw-------" " " "  1" " " "?       " " " "?       " " " "       5" " " "Jan  1 00:00" " " "s");

	/* The name is last, with spaces and bytes above 0x7f kept as they are, and a NULL name is empty. */
	make_long_name(&attrs, 1, "o", "g", "a b\xc3\xa9", 1000, out, sizeof(out));
	CHECK_TEXT(out, "-rw-------" " " "  1" " " "o       " " " "g       " " " "       5" " " "Jan  1 00:00" " " "a b\xc3\xa9");
	make_long_name(&attrs, 1, "o", "g", "", 1000, out, sizeof(out));
	CHECK_TEXT(out, "-rw-------" " " "  1" " " "o       " " " "g       " " " "       5" " " "Jan  1 00:00" " " "");
	make_long_name(&attrs, 1, "o", "g", NULL, 1000, out, sizeof(out));
	CHECK_TEXT(out, "-rw-------" " " "  1" " " "o       " " " "g       " " " "       5" " " "Jan  1 00:00" " " "");

	/* A size in the attributes without the SIZE flag is not shown. */
	attrs.flags = SSH_FILEXFER_ATTR_PERMISSIONS | SSH_FILEXFER_ATTR_ACMODTIME;
	attrs.size = 99;
	make_long_name(&attrs, 1, "o", "g", "n", 1000, out, sizeof(out));
	CHECK_TEXT(out, "-rw-------" " " "  1" " " "o       " " " "g       " " " "       0" " " "Jan  1 00:00" " " "n");

	/* A time in the attributes without the ACMODTIME flag is not shown. */
	attrs.flags = SSH_FILEXFER_ATTR_PERMISSIONS;
	attrs.mtime = 86400;
	make_long_name(&attrs, 1, "o", "g", "n", 1000, out, sizeof(out));
	CHECK_TEXT(out, "-rw-------" " " "  1" " " "o       " " " "g       " " " "       0" " " "Jan  1 00:00" " " "n");
}

/*
 * Checks the cut at the capacity: a capacity of 0 leaves the array,
 * 1 gives the empty string, and each capacity gives the full text cut at
 * capacity - 1 characters.
 */
static void
test_long_name_cut(void)
{
	struct ssh_sftp_attrs attrs;
	char full[200];
	char out[220];
	char guard[220];
	size_t length;
	size_t capacity;
	int failures;

	/* The full text of a sample. */
	memset(&attrs, 0, sizeof(attrs));
	attrs.flags = SSH_FILEXFER_ATTR_SIZE | SSH_FILEXFER_ATTR_PERMISSIONS | SSH_FILEXFER_ATTR_ACMODTIME;
	attrs.size = 12;
	attrs.permissions = 0100644UL;
	attrs.mtime = 0;
	make_long_name(&attrs, 2, "alice", "users", "readme.txt", 5, full, sizeof(full));
	CHECK_TEXT(full, "-rw-r--r--" " " "  2" " " "alice   " " " "users   " " " "      12" " " "Jan  1 00:00" " " "readme.txt");
	length = strlen(full);

	/* A capacity of 0 writes nothing at all, not even a NUL. */
	memset(out, 0x5a, sizeof(out));
	make_long_name(&attrs, 2, "alice", "users", "readme.txt", 5, out, 0);
	CHECK_EQ(out[0], 0x5a);

	/* Every capacity from 1 to one past the full length: the text cut to capacity - 1, a NUL after it, and nothing written after the capacity. */
	failures = 0;
	for (capacity = 1; capacity <= length + 3U; capacity++) {
		memset(out, 0x5a, sizeof(out));
		memset(guard, 0x5a, sizeof(guard));
		make_long_name(&attrs, 2, "alice", "users", "readme.txt", 5, out, capacity);
		if (capacity > length) {
			if (strcmp(out, full) != 0)
				failures++;
		} else {
			if (strlen(out) != capacity - 1U || memcmp(out, full, capacity - 1U) != 0)
				failures++;
		}
		if (memcmp(out + capacity, guard + capacity, sizeof(out) - capacity) != 0)
			failures++;
	}
	CHECK_EQ(failures, 0);

	/* The array of exactly length + 1 holds the whole text; length gives the text without its last character. */
	make_long_name(&attrs, 2, "alice", "users", "readme.txt", 5, out, length + 1U);
	CHECK_TEXT(out, full);
	make_long_name(&attrs, 2, "alice", "users", "readme.txt", 5, out, length);
	CHECK_EQ(strlen(out), length - 1U);
}

/*
 * Runs random attributes, names and capacities through the long name:
 * the output is NUL-terminated within the capacity, nothing is written
 * past it, and a shorter capacity gives a prefix of the full text.
 */
static void
test_long_name_random(void)
{
	struct ssh_sftp_attrs attrs;
	char full[600];
	char out[600];
	char name[100];
	char owner[40];
	size_t index;
	size_t capacity;
	size_t name_length;
	int failures;
	int run;
	int64_t now;

	/* Counts every violated expectation. */
	failures = 0;
	for (run = 0; run < 20000; run++) {
		/* Random attributes and strings. */
		attrs.flags = next_random() & 0x8000000fUL;
		attrs.size = ((uint64_t)next_random() << 32) | next_random();
		attrs.uid = next_random();
		attrs.gid = next_random();
		attrs.permissions = next_random();
		attrs.atime = next_random();
		attrs.mtime = next_random();
		name_length = next_random() % 60U;
		for (index = 0; index < name_length; index++)
			name[index] = (char)(1U + next_random() % 255U);
		name[name_length] = '\0';
		name_length = next_random() % 20U;
		for (index = 0; index < name_length; index++)
			owner[index] = (char)(1U + next_random() % 255U);
		owner[name_length] = '\0';
		now = (int64_t)(((uint64_t)next_random() << 32) | next_random());
		if ((next_random() % 3U) == 0)
			now = (int64_t)attrs.mtime + (int64_t)(next_random() % 40000000U) - 20000000;

		/* The full text, then a random capacity. */
		memset(full, 0, sizeof(full));
		ssh_sftp_long_name(&attrs, next_random(), owner, owner, name, now, full, sizeof(full));
		if (strlen(full) >= sizeof(full))
			failures++;
		capacity = 1 + next_random() % 140U;
		memset(out, 0x5a, sizeof(out));
		ssh_sftp_long_name(&attrs, 0, owner, owner, name, now, out, capacity);
		memset(full, 0, sizeof(full));
		ssh_sftp_long_name(&attrs, 0, owner, owner, name, now, full, sizeof(full));
		if (memchr(out, 0, capacity) == NULL)
			failures++;
		if (strncmp(out, full, capacity - 1U) != 0)
			failures++;
		if (strlen(out) > capacity - 1U)
			failures++;
		for (index = capacity; index < sizeof(out); index++) {
			if (out[index] != 0x5a)
				failures++;
		}
	}
	CHECK_EQ(failures, 0);
}
