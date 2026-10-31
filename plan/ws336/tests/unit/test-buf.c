/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The test of ssh-buf.c: the writer is compared with the bytes wire.h
 * builds (written apart from the module), the reader reads bytes wire.h
 * built, the limits and the sticky errors are checked at their edges, and a
 * run of random inputs read through every reader function runs under the
 * sanitizers.  Expected values come from RFC 4251 section 5 or from the
 * hand computation in the comment beside them.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "wire.h"
#include "ssh-buf.h"

/* The message wire.h builds for one comparison; static because it is large. */
static struct wire expected;

/* A second message, for the inputs of the reader checks. */
static struct wire input;

/* The state of the pseudo-random generator of the random runs, never zero. */
static uint32_t random_state = 2463534242UL;

static void test_writer_types(void);
static void test_writer_growth(void);
static void test_writer_limit(void);
static void test_writer_sticky(void);
static void test_writer_space_and_consume(void);
static void test_writer_secret(void);
static void test_reader_types(void);
static void test_reader_strings(void);
static void test_reader_cstring(void);
static void test_reader_namelist(void);
static void test_reader_finish(void);
static void test_roundtrip_random(void);
static void test_reader_random(void);
static uint32_t next_random(void);

/*
 * Runs the checks of the module.
 */
int
main(void)
{
	/* Runs each group of checks. */
	test_writer_types();
	test_writer_growth();
	test_writer_limit();
	test_writer_sticky();
	test_writer_space_and_consume();
	test_writer_secret();
	test_reader_types();
	test_reader_strings();
	test_reader_cstring();
	test_reader_namelist();
	test_reader_finish();
	test_roundtrip_random();
	test_reader_random();

	/* Reports the result of the whole test. */
	return check_finish("buf");
}

/*
 * Checks every writer function against the bytes of wire.h.
 */
static void
test_writer_types(void)
{
	struct ssh_buf buf;
	uint64_t big;

	/* A new buffer allocates nothing and records no error. */
	ssh_buf_init(&buf, 1000, 0);
	CHECK(buf.data == NULL);
	CHECK_EQ(buf.size, 0);
	CHECK_EQ(buf.capacity, 0);
	CHECK_EQ(buf.limit, 1000);
	CHECK_EQ(buf.error, 0);
	CHECK_EQ(buf.secret, 0);
	CHECK_EQ(ssh_buf_error(&buf), 0);

	/* Frees a buffer that never held memory. */
	ssh_buf_free(&buf);
	CHECK(buf.data == NULL);

	/* Writes a byte, booleans (RFC 4251 5: any non-zero value is written as 1), and integers. */
	big = ((uint64_t)0x01020304UL << 32) | 0x05060708UL;
	ssh_buf_put_u8(&buf, 0x00);
	ssh_buf_put_u8(&buf, 0xff);
	ssh_buf_put_u8(&buf, 0x7f);
	ssh_buf_put_bool(&buf, 0);
	ssh_buf_put_bool(&buf, 1);
	ssh_buf_put_bool(&buf, 2);
	ssh_buf_put_bool(&buf, -1);
	ssh_buf_put_u32(&buf, 0);
	ssh_buf_put_u32(&buf, 0xffffffffUL);
	ssh_buf_put_u32(&buf, 0x29b7f4aaUL);
	ssh_buf_put_u64(&buf, 0);
	ssh_buf_put_u64(&buf, big);
	ssh_buf_put_u64(&buf, ((uint64_t)0xffffffffUL << 32) | 0xffffffffUL);
	CHECK_EQ(ssh_buf_error(&buf), 0);

	/* The same message built apart from the module. */
	wire_reset(&expected);
	wire_u8(&expected, 0x00);
	wire_u8(&expected, 0xff);
	wire_u8(&expected, 0x7f);
	wire_bool(&expected, 0);
	wire_bool(&expected, 1);
	wire_bool(&expected, 1);
	wire_bool(&expected, 1);
	wire_u32(&expected, 0);
	wire_u32(&expected, 0xffffffffUL);
	wire_u32(&expected, 0x29b7f4aaUL);
	wire_u64(&expected, 0);
	wire_u64(&expected, big);
	wire_u64(&expected, ((uint64_t)0xffffffffUL << 32) | 0xffffffffUL);
	CHECK_EQ(buf.size, expected.size);
	CHECK_BYTES(buf.data, expected.data, expected.size);
	ssh_buf_free(&buf);

	/* Strings: the example of RFC 4251 5 ("testing"), an empty string, a string with a NUL, raw bytes, a C string. */
	ssh_buf_init(&buf, 1000, 0);
	ssh_buf_put_string(&buf, "testing", 7);
	ssh_buf_put_string(&buf, NULL, 0);
	ssh_buf_put_string(&buf, "a\0b", 3);
	ssh_buf_put_bytes(&buf, "xyz", 3);
	ssh_buf_put_bytes(&buf, NULL, 0);
	ssh_buf_put_cstring(&buf, "zlib,none");
	ssh_buf_put_cstring(&buf, "");
	wire_reset(&expected);
	wire_string(&expected, "testing", 7);
	wire_string(&expected, NULL, 0);
	wire_string(&expected, "a\0b", 3);
	wire_bytes(&expected, "xyz", 3);
	wire_text(&expected, "zlib,none");
	wire_text(&expected, "");
	CHECK_EQ(ssh_buf_error(&buf), 0);
	CHECK_EQ(buf.size, expected.size);
	CHECK_BYTES(buf.data, expected.data, expected.size);
	CHECK_BYTES(buf.data, "\0\0\0\7testing", 11);
	ssh_buf_free(&buf);

	/* Missing bytes with a size above 0 are refused and nothing is written. */
	ssh_buf_init(&buf, 1000, 0);
	ssh_buf_put_bytes(&buf, NULL, 3);
	CHECK_EQ(ssh_buf_error(&buf), EINVAL);
	CHECK_EQ(buf.size, 0);
	ssh_buf_reset(&buf);
	ssh_buf_put_string(&buf, NULL, 3);
	CHECK_EQ(ssh_buf_error(&buf), EINVAL);
	CHECK_EQ(buf.size, 0);
	ssh_buf_reset(&buf);
	ssh_buf_put_cstring(&buf, NULL);
	CHECK_EQ(ssh_buf_error(&buf), EINVAL);
	CHECK_EQ(buf.size, 0);
	ssh_buf_free(&buf);
}

/*
 * Checks that the buffer grows without losing what was written, byte by byte
 * and in large steps.
 */
static void
test_writer_growth(void)
{
	struct ssh_buf buf;
	uint8_t chunk[5000];
	size_t index;
	int intact;

	/* Writes 10000 bytes one at a time; the content stays right at every size. */
	ssh_buf_init(&buf, 100000, 0);
	intact = 1;
	for (index = 0; index < 10000; index++) {
		ssh_buf_put_u8(&buf, (uint8_t)(index % 251));
		if (buf.size != index + 1 || buf.data[index] != (uint8_t)(index % 251))
			intact = 0;
	}
	CHECK_EQ(intact, 1);
	CHECK_EQ(ssh_buf_error(&buf), 0);
	CHECK(buf.capacity >= buf.size);
	CHECK(buf.capacity <= buf.limit);
	intact = 1;
	for (index = 0; index < 10000; index++) {
		if (buf.data[index] != (uint8_t)(index % 251))
			intact = 0;
	}
	CHECK_EQ(intact, 1);

	/* A reset keeps the memory and empties the used part. */
	index = buf.capacity;
	ssh_buf_reset(&buf);
	CHECK_EQ(buf.size, 0);
	CHECK_EQ(buf.capacity, index);
	CHECK(buf.data != NULL);

	/* A free gives the memory back and the buffer is usable again. */
	ssh_buf_free(&buf);
	CHECK(buf.data == NULL);
	CHECK_EQ(buf.capacity, 0);
	CHECK_EQ(buf.limit, 100000);
	ssh_buf_put_u32(&buf, 0x01020304UL);
	CHECK_EQ(buf.size, 4);
	CHECK_BYTES(buf.data, "\1\2\3\4", 4);

	/* A large write at once. */
	for (index = 0; index < sizeof(chunk); index++)
		chunk[index] = (uint8_t)(index * 7);
	ssh_buf_put_bytes(&buf, chunk, sizeof(chunk));
	ssh_buf_put_bytes(&buf, chunk, sizeof(chunk));
	CHECK_EQ(ssh_buf_error(&buf), 0);
	CHECK_EQ(buf.size, 4 + 2 * sizeof(chunk));
	CHECK_BYTES(buf.data + 4, chunk, sizeof(chunk));
	CHECK_BYTES(buf.data + 4 + sizeof(chunk), chunk, sizeof(chunk));
	ssh_buf_free(&buf);

	/* The capacity never passes the limit: 65 bytes under a limit of 100 doubles 64 to 128, which is cut to 100. */
	ssh_buf_init(&buf, 100, 0);
	ssh_buf_put_bytes(&buf, chunk, 65);
	CHECK_EQ(ssh_buf_error(&buf), 0);
	CHECK_EQ(buf.capacity, 100);
	ssh_buf_put_bytes(&buf, chunk, 35);
	CHECK_EQ(ssh_buf_error(&buf), 0);
	CHECK_EQ(buf.size, 100);
	CHECK_EQ(buf.capacity, 100);
	ssh_buf_free(&buf);

	/* A limit below the first capacity. */
	ssh_buf_init(&buf, 3, 0);
	ssh_buf_put_u8(&buf, 1);
	ssh_buf_put_u8(&buf, 2);
	ssh_buf_put_u8(&buf, 3);
	CHECK_EQ(ssh_buf_error(&buf), 0);
	CHECK_EQ(buf.capacity, 3);
	ssh_buf_put_u8(&buf, 4);
	CHECK_EQ(ssh_buf_error(&buf), EMSGSIZE);
	CHECK_EQ(buf.size, 3);
	ssh_buf_free(&buf);
}

/*
 * Checks the limit at its edge, for every writer: a write that fits exactly
 * succeeds and one byte more fails with EMSGSIZE and leaves nothing behind.
 */
static void
test_writer_limit(void)
{
	struct ssh_buf buf;
	int error;

	/* A byte: limit 1 takes one and refuses the second. */
	ssh_buf_init(&buf, 1, 0);
	ssh_buf_put_u8(&buf, 9);
	CHECK_EQ(ssh_buf_error(&buf), 0);
	ssh_buf_put_u8(&buf, 9);
	CHECK_EQ(ssh_buf_error(&buf), EMSGSIZE);
	CHECK_EQ(buf.size, 1);
	ssh_buf_free(&buf);

	/* A uint32 needs 4. */
	ssh_buf_init(&buf, 4, 0);
	ssh_buf_put_u32(&buf, 1);
	CHECK_EQ(ssh_buf_error(&buf), 0);
	ssh_buf_free(&buf);
	ssh_buf_init(&buf, 3, 0);
	ssh_buf_put_u32(&buf, 1);
	CHECK_EQ(ssh_buf_error(&buf), EMSGSIZE);
	CHECK_EQ(buf.size, 0);
	ssh_buf_free(&buf);

	/* A uint64 needs 8. */
	ssh_buf_init(&buf, 8, 0);
	ssh_buf_put_u64(&buf, 1);
	CHECK_EQ(ssh_buf_error(&buf), 0);
	ssh_buf_free(&buf);
	ssh_buf_init(&buf, 7, 0);
	ssh_buf_put_u64(&buf, 1);
	CHECK_EQ(ssh_buf_error(&buf), EMSGSIZE);
	CHECK_EQ(buf.size, 0);
	ssh_buf_free(&buf);

	/* A boolean needs 1. */
	ssh_buf_init(&buf, 1, 0);
	ssh_buf_put_bool(&buf, 1);
	ssh_buf_put_bool(&buf, 1);
	CHECK_EQ(ssh_buf_error(&buf), EMSGSIZE);
	CHECK_EQ(buf.size, 1);
	ssh_buf_free(&buf);

	/* A string of 3 bytes needs 4 + 3 = 7, and the length is not written when the bytes do not fit. */
	ssh_buf_init(&buf, 7, 0);
	ssh_buf_put_string(&buf, "abc", 3);
	CHECK_EQ(ssh_buf_error(&buf), 0);
	CHECK_EQ(buf.size, 7);
	ssh_buf_free(&buf);
	ssh_buf_init(&buf, 6, 0);
	ssh_buf_put_string(&buf, "abc", 3);
	CHECK_EQ(ssh_buf_error(&buf), EMSGSIZE);
	CHECK_EQ(buf.size, 0);
	ssh_buf_free(&buf);

	/* Raw bytes. */
	ssh_buf_init(&buf, 3, 0);
	ssh_buf_put_bytes(&buf, "abc", 3);
	CHECK_EQ(ssh_buf_error(&buf), 0);
	ssh_buf_put_bytes(&buf, "", 0);
	CHECK_EQ(ssh_buf_error(&buf), 0);
	ssh_buf_put_bytes(&buf, "d", 1);
	CHECK_EQ(ssh_buf_error(&buf), EMSGSIZE);
	CHECK_EQ(buf.size, 3);
	CHECK_BYTES(buf.data, "abc", 3);
	ssh_buf_free(&buf);

	/* A C string. */
	ssh_buf_init(&buf, 6, 0);
	ssh_buf_put_cstring(&buf, "ab");
	CHECK_EQ(ssh_buf_error(&buf), 0);
	ssh_buf_put_cstring(&buf, "");
	CHECK_EQ(ssh_buf_error(&buf), EMSGSIZE);
	CHECK_EQ(buf.size, 6);
	ssh_buf_free(&buf);

	/* A string longer than a uint32 can count is refused before its bytes are read (only where size_t is wider than 32 bits). */
	if (sizeof(size_t) > 4) {
		ssh_buf_init(&buf, 1000, 0);
		ssh_buf_put_string(&buf, "a", ((size_t)0x10000 << 16) + 0);
		CHECK_EQ(ssh_buf_error(&buf), EMSGSIZE);
		CHECK_EQ(buf.size, 0);
		ssh_buf_free(&buf);
	}

	/* Sizes near the largest size_t do not wrap: with 5 bytes used, SIZE_MAX - 4 is one more than the room. */
	ssh_buf_init(&buf, SIZE_MAX, 0);
	ssh_buf_put_bytes(&buf, "abcde", 5);
	CHECK_EQ(ssh_buf_error(&buf), 0);
	error = ssh_buf_reserve(&buf, SIZE_MAX - 4);
	CHECK_EQ(error, EMSGSIZE);
	CHECK_EQ(buf.error, EMSGSIZE);
	CHECK_EQ(buf.size, 5);
	ssh_buf_reset(&buf);
	ssh_buf_put_bytes(&buf, "abcde", 5);
	error = ssh_buf_reserve(&buf, SIZE_MAX);
	CHECK_EQ(error, EMSGSIZE);
	ssh_buf_reset(&buf);
	ssh_buf_put_bytes(&buf, "abcde", 5);
	ssh_buf_put_string(&buf, "a", 1);
	CHECK_EQ(ssh_buf_error(&buf), 0);
	CHECK_EQ(buf.size, 10);
	ssh_buf_free(&buf);

	/* The reserve itself, at the limit. */
	ssh_buf_init(&buf, 10, 0);
	CHECK_EQ(ssh_buf_reserve(&buf, 10), 0);
	CHECK(buf.capacity >= 10);
	CHECK_EQ(buf.size, 0);
	CHECK_EQ(ssh_buf_reserve(&buf, 11), EMSGSIZE);
	ssh_buf_free(&buf);
}

/*
 * Checks that the first failure is kept and every later write does nothing.
 */
static void
test_writer_sticky(void)
{
	struct ssh_buf buf;
	uint8_t *space;

	/* After a failure at the limit, every writer leaves the buffer as it was and the first error stays. */
	ssh_buf_init(&buf, 4, 0);
	ssh_buf_put_u32(&buf, 0x01020304UL);
	ssh_buf_put_u8(&buf, 5);
	CHECK_EQ(ssh_buf_error(&buf), EMSGSIZE);
	ssh_buf_put_u8(&buf, 6);
	ssh_buf_put_bool(&buf, 1);
	ssh_buf_put_u32(&buf, 7);
	ssh_buf_put_u64(&buf, 8);
	ssh_buf_put_bytes(&buf, "abc", 3);
	ssh_buf_put_bytes(&buf, NULL, 3);
	ssh_buf_put_string(&buf, "a", 1);
	ssh_buf_put_string(&buf, NULL, 3);
	ssh_buf_put_cstring(&buf, "x");
	ssh_buf_put_cstring(&buf, NULL);
	CHECK_EQ(ssh_buf_error(&buf), EMSGSIZE);
	CHECK_EQ(buf.size, 4);
	CHECK_BYTES(buf.data, "\1\2\3\4", 4);
	CHECK_EQ(ssh_buf_reserve(&buf, 0), EMSGSIZE);
	CHECK(ssh_buf_put_space(&buf, 0) == NULL);
	CHECK(ssh_buf_put_space(&buf, 1) == NULL);
	CHECK_EQ(buf.size, 4);
	CHECK_EQ(ssh_buf_error(&buf), EMSGSIZE);

	/* A reset clears the error and the buffer can be written again. */
	ssh_buf_reset(&buf);
	CHECK_EQ(ssh_buf_error(&buf), 0);
	ssh_buf_put_u8(&buf, 0x42);
	CHECK_EQ(buf.size, 1);
	CHECK_EQ(buf.data[0], 0x42);

	/* A free clears the error too. */
	ssh_buf_put_u64(&buf, 1);
	CHECK_EQ(ssh_buf_error(&buf), EMSGSIZE);
	ssh_buf_free(&buf);
	CHECK_EQ(ssh_buf_error(&buf), 0);

	/* The error of a bad argument is also the first one kept. */
	ssh_buf_put_bytes(&buf, NULL, 1);
	CHECK_EQ(ssh_buf_error(&buf), EINVAL);
	ssh_buf_put_u32(&buf, 1);
	CHECK_EQ(ssh_buf_error(&buf), EINVAL);
	CHECK_EQ(buf.size, 0);
	ssh_buf_free(&buf);

	/* A failing put_space reports NULL and records the error. */
	ssh_buf_init(&buf, 8, 0);
	space = ssh_buf_put_space(&buf, 9);
	CHECK(space == NULL);
	CHECK_EQ(ssh_buf_error(&buf), EMSGSIZE);
	ssh_buf_free(&buf);
}

/*
 * Checks ssh_buf_put_space and ssh_buf_consume.
 */
static void
test_writer_space_and_consume(void)
{
	struct ssh_buf buf;
	uint8_t *space;
	uint8_t bytes[300];
	size_t index;

	/* Reserved space is counted as used, is writable, and later writes follow it. */
	ssh_buf_init(&buf, 1000, 0);
	ssh_buf_put_u8(&buf, 1);
	space = ssh_buf_put_space(&buf, 3);
	CHECK(space != NULL);
	CHECK_EQ(buf.size, 4);
	CHECK(space == buf.data + 1);
	space[0] = 2;
	space[1] = 3;
	space[2] = 4;
	ssh_buf_put_u8(&buf, 5);
	CHECK_EQ(buf.size, 5);
	CHECK_BYTES(buf.data, "\1\2\3\4\5", 5);

	/* Space large enough to move the memory keeps the bytes before it. */
	space = ssh_buf_put_space(&buf, 700);
	CHECK(space != NULL);
	CHECK_EQ(buf.size, 705);
	memset(space, 0xab, 700);
	CHECK_BYTES(buf.data, "\1\2\3\4\5", 5);
	CHECK_EQ(buf.data[704], 0xab);
	ssh_buf_free(&buf);

	/* Space of 0 on a buffer with no memory still gives a pointer, and counts nothing. */
	ssh_buf_init(&buf, 1000, 0);
	space = ssh_buf_put_space(&buf, 0);
	CHECK(space != NULL);
	CHECK_EQ(buf.size, 0);
	CHECK_EQ(ssh_buf_error(&buf), 0);
	ssh_buf_free(&buf);

	/* Consuming nothing, part, all, and more than all. */
	for (index = 0; index < 10; index++)
		bytes[index] = (uint8_t)(index + 1);
	ssh_buf_init(&buf, 1000, 0);
	ssh_buf_put_bytes(&buf, bytes, 10);
	ssh_buf_consume(&buf, 0);
	CHECK_EQ(buf.size, 10);
	CHECK_BYTES(buf.data, bytes, 10);
	ssh_buf_consume(&buf, 3);
	CHECK_EQ(buf.size, 7);
	CHECK_BYTES(buf.data, bytes + 3, 7);
	ssh_buf_consume(&buf, 6);
	CHECK_EQ(buf.size, 1);
	CHECK_EQ(buf.data[0], 10);
	ssh_buf_consume(&buf, 1);
	CHECK_EQ(buf.size, 0);
	ssh_buf_put_bytes(&buf, bytes, 10);
	ssh_buf_consume(&buf, 11);
	CHECK_EQ(buf.size, 0);
	ssh_buf_put_bytes(&buf, bytes, 10);
	ssh_buf_consume(&buf, SIZE_MAX);
	CHECK_EQ(buf.size, 0);
	CHECK_EQ(ssh_buf_error(&buf), 0);

	/* The buffer is usable after a consume, and an empty one with no memory can be consumed. */
	ssh_buf_put_u8(&buf, 77);
	CHECK_EQ(buf.size, 1);
	CHECK_EQ(buf.data[0], 77);
	ssh_buf_free(&buf);
	ssh_buf_consume(&buf, 5);
	CHECK_EQ(buf.size, 0);

	/* A consume of the front of a long message, like a sender that sent part of its output. */
	for (index = 0; index < sizeof(bytes); index++)
		bytes[index] = (uint8_t)(index * 3);
	ssh_buf_init(&buf, 1000, 0);
	ssh_buf_put_bytes(&buf, bytes, sizeof(bytes));
	ssh_buf_consume(&buf, 100);
	CHECK_EQ(buf.size, 200);
	CHECK_BYTES(buf.data, bytes + 100, 200);
	ssh_buf_free(&buf);
}

/*
 * Checks the buffer of a secret: its contents survive growth and reset,
 * and consume moves them correctly.  Whether the old memory is wiped needs
 * ssh_wipe, which the crypto module provides; this test does not look at
 * freed memory.
 */
static void
test_writer_secret(void)
{
	struct ssh_buf buf;
	uint8_t bytes[2000];
	size_t index;
	size_t before;
	int intact;

	for (index = 0; index < sizeof(bytes); index++)
		bytes[index] = (uint8_t)(index * 13 + 1);

	/* Writes in small steps so that the buffer grows several times; the content stays right after each growth. */
	ssh_buf_init(&buf, 100000, 1);
	CHECK_EQ(buf.secret, 1);
	intact = 1;
	for (index = 0; index < sizeof(bytes); index++) {
		before = buf.capacity;
		ssh_buf_put_u8(&buf, bytes[index]);
		if (buf.size != index + 1)
			intact = 0;
		if (memcmp(buf.data, bytes, buf.size) != 0)
			intact = 0;
		if (before != 0 && buf.capacity < before)
			intact = 0;
	}
	CHECK_EQ(intact, 1);
	CHECK_EQ(ssh_buf_error(&buf), 0);

	/* A consume moves the rest to the front. */
	ssh_buf_consume(&buf, 1500);
	CHECK_EQ(buf.size, 500);
	CHECK_BYTES(buf.data, bytes + 1500, 500);

	/* A reset keeps the memory and the secret flag, and the buffer is written again. */
	before = buf.capacity;
	ssh_buf_reset(&buf);
	CHECK_EQ(buf.size, 0);
	CHECK_EQ(buf.capacity, before);
	CHECK_EQ(buf.secret, 1);
	ssh_buf_put_string(&buf, "key", 3);
	CHECK_BYTES(buf.data, "\0\0\0\3key", 7);

	/* A free leaves the buffer empty, still secret, and usable. */
	ssh_buf_free(&buf);
	CHECK(buf.data == NULL);
	CHECK_EQ(buf.secret, 1);
	ssh_buf_put_u32(&buf, 0xdeadbeefUL);
	CHECK_BYTES(buf.data, "\xde\xad\xbe\xef", 4);
	ssh_buf_free(&buf);
}

/*
 * Checks every reader function against bytes built by wire.h.
 */
static void
test_reader_types(void)
{
	struct ssh_reader reader;
	uint8_t empty[1];
	const uint8_t *bytes;

	/* One byte of each interesting value, read back. */
	wire_reset(&input);
	wire_u8(&input, 0);
	wire_u8(&input, 0xff);
	wire_u8(&input, 0x80);
	ssh_reader_init(&reader, input.data, input.size);
	CHECK_EQ(ssh_reader_get_u8(&reader), 0);
	CHECK_EQ(ssh_reader_get_u8(&reader), 0xff);
	CHECK_EQ(ssh_reader_get_u8(&reader), 0x80);
	CHECK_EQ(ssh_reader_left(&reader), 0);
	CHECK_EQ(ssh_reader_finish(&reader), 0);
	CHECK_EQ(ssh_reader_error(&reader), 0);

	/* A boolean is 0 for a zero byte and 1 for every other value (RFC 4251 5). */
	wire_reset(&input);
	wire_bool(&input, 0);
	wire_bool(&input, 1);
	wire_bool(&input, 2);
	wire_bool(&input, 0x80);
	wire_bool(&input, 0xff);
	ssh_reader_init(&reader, input.data, input.size);
	CHECK_EQ(ssh_reader_get_bool(&reader), 0);
	CHECK_EQ(ssh_reader_get_bool(&reader), 1);
	CHECK_EQ(ssh_reader_get_bool(&reader), 1);
	CHECK_EQ(ssh_reader_get_bool(&reader), 1);
	CHECK_EQ(ssh_reader_get_bool(&reader), 1);
	CHECK_EQ(ssh_reader_finish(&reader), 0);

	/* The uint32 of RFC 4251 5 (0x29b7f4aa) and the extremes. */
	wire_reset(&input);
	wire_u32(&input, 0x29b7f4aaUL);
	wire_u32(&input, 0);
	wire_u32(&input, 0xffffffffUL);
	wire_u32(&input, 0x80000000UL);
	ssh_reader_init(&reader, input.data, input.size);
	CHECK(ssh_reader_get_u32(&reader) == 0x29b7f4aaUL);
	CHECK(ssh_reader_get_u32(&reader) == 0);
	CHECK(ssh_reader_get_u32(&reader) == 0xffffffffUL);
	CHECK(ssh_reader_get_u32(&reader) == 0x80000000UL);
	CHECK_EQ(ssh_reader_finish(&reader), 0);

	/* The uint64 with a different value in every byte, and the extremes. */
	wire_reset(&input);
	wire_u64(&input, ((uint64_t)0x01020304UL << 32) | 0x05060708UL);
	wire_u64(&input, 0);
	wire_u64(&input, ((uint64_t)0xffffffffUL << 32) | 0xffffffffUL);
	wire_u64(&input, ((uint64_t)0x80000000UL << 32) | 0x00000001UL);
	ssh_reader_init(&reader, input.data, input.size);
	CHECK(ssh_reader_get_u64(&reader) == (((uint64_t)0x01020304UL << 32) | 0x05060708UL));
	CHECK(ssh_reader_get_u64(&reader) == 0);
	CHECK(ssh_reader_get_u64(&reader) == (((uint64_t)0xffffffffUL << 32) | 0xffffffffUL));
	CHECK(ssh_reader_get_u64(&reader) == (((uint64_t)0x80000000UL << 32) | 0x00000001UL));
	CHECK_EQ(ssh_reader_finish(&reader), 0);

	/* A read past the end fails with EINVAL, returns 0 and does not move the offset. */
	ssh_reader_init(&reader, (const uint8_t *)"\1\2\3", 3);
	CHECK_EQ(ssh_reader_get_u32(&reader), 0);
	CHECK_EQ(ssh_reader_error(&reader), EINVAL);
	CHECK_EQ(reader.offset, 0);
	CHECK_EQ(ssh_reader_left(&reader), 0);
	ssh_reader_init(&reader, (const uint8_t *)"\1\2\3\4\5\6\7", 7);
	CHECK(ssh_reader_get_u64(&reader) == 0);
	CHECK_EQ(ssh_reader_error(&reader), EINVAL);
	CHECK_EQ(reader.offset, 0);
	ssh_reader_init(&reader, empty, 0);
	CHECK_EQ(ssh_reader_get_u8(&reader), 0);
	CHECK_EQ(ssh_reader_error(&reader), EINVAL);
	ssh_reader_init(&reader, empty, 0);
	CHECK_EQ(ssh_reader_get_bool(&reader), 0);
	CHECK_EQ(ssh_reader_error(&reader), EINVAL);

	/* After an error every read returns zero without moving, even when bytes are there. */
	ssh_reader_init(&reader, (const uint8_t *)"\1\2\3\4\5\6\7\x08\x09", 9);
	CHECK(ssh_reader_get_bytes(&reader, 100) == NULL);
	CHECK_EQ(ssh_reader_error(&reader), EINVAL);
	CHECK_EQ(ssh_reader_get_u8(&reader), 0);
	CHECK_EQ(ssh_reader_get_bool(&reader), 0);
	CHECK_EQ(ssh_reader_get_u32(&reader), 0);
	CHECK(ssh_reader_get_u64(&reader) == 0);
	CHECK(ssh_reader_get_bytes(&reader, 1) == NULL);
	CHECK_EQ(reader.offset, 0);
	CHECK_EQ(ssh_reader_left(&reader), 0);
	CHECK_EQ(ssh_reader_error(&reader), EINVAL);
	CHECK_EQ(ssh_reader_finish(&reader), EINVAL);

	/* Raw bytes: a position inside the data, the end exactly, one past, and a size of the largest size_t. */
	ssh_reader_init(&reader, (const uint8_t *)"abcdef", 6);
	bytes = ssh_reader_get_bytes(&reader, 2);
	CHECK(bytes == reader.data);
	bytes = ssh_reader_get_bytes(&reader, 0);
	CHECK(bytes == reader.data + 2);
	CHECK_EQ(ssh_reader_error(&reader), 0);
	bytes = ssh_reader_get_bytes(&reader, 4);
	CHECK(bytes == reader.data + 2);
	CHECK_EQ(ssh_reader_left(&reader), 0);
	CHECK_EQ(ssh_reader_error(&reader), 0);
	bytes = ssh_reader_get_bytes(&reader, 1);
	CHECK(bytes == NULL);
	CHECK_EQ(ssh_reader_error(&reader), EINVAL);
	ssh_reader_init(&reader, (const uint8_t *)"abcdef", 6);
	(void)ssh_reader_get_u8(&reader);
	CHECK(ssh_reader_get_bytes(&reader, SIZE_MAX) == NULL);
	CHECK_EQ(ssh_reader_error(&reader), EINVAL);
	ssh_reader_init(&reader, (const uint8_t *)"abcdef", 6);
	(void)ssh_reader_get_u8(&reader);
	CHECK(ssh_reader_get_bytes(&reader, 6) == NULL);
	CHECK_EQ(ssh_reader_error(&reader), EINVAL);

	/* A reader over no bytes may have no data; bytes claimed without data are refused. */
	ssh_reader_init(&reader, NULL, 0);
	CHECK_EQ(ssh_reader_error(&reader), 0);
	CHECK_EQ(ssh_reader_finish(&reader), 0);
	CHECK(ssh_reader_get_bytes(&reader, 0) == NULL);
	CHECK_EQ(ssh_reader_error(&reader), 0);
	CHECK_EQ(ssh_reader_get_u8(&reader), 0);
	CHECK_EQ(ssh_reader_error(&reader), EINVAL);
	ssh_reader_init(&reader, NULL, 5);
	CHECK_EQ(ssh_reader_error(&reader), EINVAL);
	CHECK_EQ(ssh_reader_left(&reader), 0);
	CHECK_EQ(ssh_reader_get_u8(&reader), 0);
}

/*
 * Checks ssh_reader_get_string: lengths at, below and above what remains.
 */
static void
test_reader_strings(void)
{
	struct ssh_reader reader;
	const uint8_t *data;
	size_t size;

	/* The example of RFC 4251 5: "testing". */
	wire_reset(&input);
	wire_text(&input, "testing");
	wire_text(&input, "");
	wire_string(&input, "a\0b", 3);
	ssh_reader_init(&reader, input.data, input.size);
	ssh_reader_get_string(&reader, &data, &size);
	CHECK_EQ(size, 7);
	CHECK(data == input.data + 4);
	CHECK_BYTES(data, "testing", 7);
	ssh_reader_get_string(&reader, &data, &size);
	CHECK_EQ(size, 0);
	CHECK(data != NULL);
	CHECK_EQ(ssh_reader_error(&reader), 0);
	ssh_reader_get_string(&reader, &data, &size);
	CHECK_EQ(size, 3);
	CHECK_BYTES(data, "a\0b", 3);
	CHECK_EQ(ssh_reader_finish(&reader), 0);

	/* A length that is exactly what remains is read. */
	ssh_reader_init(&reader, (const uint8_t *)"\0\0\0\3abc", 7);
	ssh_reader_get_string(&reader, &data, &size);
	CHECK_EQ(size, 3);
	CHECK_EQ(ssh_reader_finish(&reader), 0);

	/* A length one more than what remains fails, with data NULL and size 0, and does not move. */
	size = 99;
	data = (const uint8_t *)&size;
	ssh_reader_init(&reader, (const uint8_t *)"\0\0\0\4abc", 7);
	ssh_reader_get_string(&reader, &data, &size);
	CHECK(data == NULL);
	CHECK_EQ(size, 0);
	CHECK_EQ(ssh_reader_error(&reader), EINVAL);
	CHECK_EQ(reader.offset, 0);

	/* The largest length. */
	ssh_reader_init(&reader, (const uint8_t *)"\xff\xff\xff\xff" "abc", 7);
	ssh_reader_get_string(&reader, &data, &size);
	CHECK(data == NULL);
	CHECK_EQ(size, 0);
	CHECK_EQ(ssh_reader_error(&reader), EINVAL);
	ssh_reader_init(&reader, (const uint8_t *)"\x80\0\0\0" "abc", 7);
	ssh_reader_get_string(&reader, &data, &size);
	CHECK_EQ(ssh_reader_error(&reader), EINVAL);

	/* A truncated length field. */
	ssh_reader_init(&reader, (const uint8_t *)"\0\0\0", 3);
	ssh_reader_get_string(&reader, &data, &size);
	CHECK(data == NULL);
	CHECK_EQ(size, 0);
	CHECK_EQ(ssh_reader_error(&reader), EINVAL);
	ssh_reader_init(&reader, (const uint8_t *)"", 0);
	ssh_reader_get_string(&reader, &data, &size);
	CHECK_EQ(ssh_reader_error(&reader), EINVAL);

	/* After an error a string read returns nothing even when a good string follows. */
	wire_reset(&input);
	wire_text(&input, "ok");
	ssh_reader_init(&reader, input.data, input.size);
	(void)ssh_reader_get_bytes(&reader, 1000);
	data = input.data;
	size = 5;
	ssh_reader_get_string(&reader, &data, &size);
	CHECK(data == NULL);
	CHECK_EQ(size, 0);
	CHECK_EQ(reader.offset, 0);
}

/*
 * Checks ssh_reader_get_cstring.
 */
static void
test_reader_cstring(void)
{
	struct ssh_reader reader;
	char out[16];

	/* A string that fits with its NUL: the capacity is the length plus one. */
	wire_reset(&input);
	wire_text(&input, "testing");
	ssh_reader_init(&reader, input.data, input.size);
	memset(out, 0x55, sizeof(out));
	ssh_reader_get_cstring(&reader, out, 8);
	CHECK_EQ(ssh_reader_error(&reader), 0);
	CHECK(strcmp(out, "testing") == 0);
	CHECK_EQ(out[8], 0x55);
	CHECK_EQ(ssh_reader_finish(&reader), 0);

	/* One byte less room is EMSGSIZE and leaves the empty string. */
	ssh_reader_init(&reader, input.data, input.size);
	memset(out, 0x55, sizeof(out));
	ssh_reader_get_cstring(&reader, out, 7);
	CHECK_EQ(ssh_reader_error(&reader), EMSGSIZE);
	CHECK_EQ(out[0], 0);
	CHECK_EQ(ssh_reader_left(&reader), 0);

	/* An empty string in a room of one. */
	wire_reset(&input);
	wire_text(&input, "");
	ssh_reader_init(&reader, input.data, input.size);
	memset(out, 0x55, sizeof(out));
	ssh_reader_get_cstring(&reader, out, 1);
	CHECK_EQ(ssh_reader_error(&reader), 0);
	CHECK_EQ(out[0], 0);
	CHECK_EQ(out[1], 0x55);

	/* A NUL inside the string is EINVAL, whatever the room, at the start, in the middle and at the end. */
	wire_reset(&input);
	wire_string(&input, "ab\0cd", 5);
	ssh_reader_init(&reader, input.data, input.size);
	memset(out, 0x55, sizeof(out));
	ssh_reader_get_cstring(&reader, out, sizeof(out));
	CHECK_EQ(ssh_reader_error(&reader), EINVAL);
	CHECK_EQ(out[0], 0);
	wire_reset(&input);
	wire_string(&input, "\0", 1);
	ssh_reader_init(&reader, input.data, input.size);
	memset(out, 0x55, sizeof(out));
	ssh_reader_get_cstring(&reader, out, sizeof(out));
	CHECK_EQ(ssh_reader_error(&reader), EINVAL);
	CHECK_EQ(out[0], 0);
	wire_reset(&input);
	wire_string(&input, "ab\0", 3);
	ssh_reader_init(&reader, input.data, input.size);
	ssh_reader_get_cstring(&reader, out, sizeof(out));
	CHECK_EQ(ssh_reader_error(&reader), EINVAL);

	/* A string that runs past the end is EINVAL and the output is empty. */
	ssh_reader_init(&reader, (const uint8_t *)"\0\0\0\5abc", 7);
	memset(out, 0x55, sizeof(out));
	ssh_reader_get_cstring(&reader, out, sizeof(out));
	CHECK_EQ(ssh_reader_error(&reader), EINVAL);
	CHECK_EQ(out[0], 0);

	/* After an error the output is the empty string again. */
	ssh_reader_init(&reader, (const uint8_t *)"\0\0\0\2hi", 6);
	(void)ssh_reader_get_bytes(&reader, 99);
	memset(out, 0x55, sizeof(out));
	ssh_reader_get_cstring(&reader, out, sizeof(out));
	CHECK_EQ(out[0], 0);

	/* A room of 0 cannot hold even the NUL: EINVAL, and nothing is written. */
	wire_reset(&input);
	wire_text(&input, "hi");
	ssh_reader_init(&reader, input.data, input.size);
	memset(out, 0x55, sizeof(out));
	ssh_reader_get_cstring(&reader, out, 0);
	CHECK_EQ(ssh_reader_error(&reader), EINVAL);
	CHECK_EQ(out[0], 0x55);

	/* Two strings in a row: the first read does not disturb the second. */
	wire_reset(&input);
	wire_text(&input, "ab");
	wire_text(&input, "cde");
	ssh_reader_init(&reader, input.data, input.size);
	ssh_reader_get_cstring(&reader, out, sizeof(out));
	CHECK(strcmp(out, "ab") == 0);
	ssh_reader_get_cstring(&reader, out, sizeof(out));
	CHECK(strcmp(out, "cde") == 0);
	CHECK_EQ(ssh_reader_finish(&reader), 0);
}

/*
 * Checks ssh_reader_get_namelist.
 */
static void
test_reader_namelist(void)
{
	struct ssh_reader reader;
	const char *list;
	size_t size;
	size_t index;
	static char text[SSH_NAMELIST_MAX + 8];
	static const char *const bad[] = { "a,,b", ",a", "a,", ",", "a b", "a\x80", "a\x7f", "a\x01" };

	/* The example of RFC 4251 5: ("zlib", "none") is the string "zlib,none". */
	wire_reset(&input);
	wire_text(&input, "zlib,none");
	ssh_reader_init(&reader, input.data, input.size);
	ssh_reader_get_namelist(&reader, &list, &size);
	CHECK_EQ(size, 9);
	CHECK(list == (const char *)input.data + 4);
	CHECK_BYTES(list, "zlib,none", 9);
	CHECK_EQ(ssh_reader_finish(&reader), 0);

	/* An empty list is valid (RFC 4251 5), a one-name list too. */
	wire_reset(&input);
	wire_text(&input, "");
	wire_text(&input, "none");
	ssh_reader_init(&reader, input.data, input.size);
	ssh_reader_get_namelist(&reader, &list, &size);
	CHECK_EQ(size, 0);
	CHECK(list != NULL);
	CHECK_EQ(ssh_reader_error(&reader), 0);
	ssh_reader_get_namelist(&reader, &list, &size);
	CHECK_EQ(size, 4);
	CHECK_EQ(ssh_reader_finish(&reader), 0);

	/* A list with an empty name, a leading or trailing comma, a byte out of range, or a 65-character name is refused. */
	for (index = 0; index < sizeof(bad) / sizeof(bad[0]); index++) {
		wire_reset(&input);
		wire_text(&input, bad[index]);
		ssh_reader_init(&reader, input.data, input.size);
		list = "x";
		size = 5;
		ssh_reader_get_namelist(&reader, &list, &size);
		CHECK_EQ(ssh_reader_error(&reader), EINVAL);
		CHECK(list == NULL);
		CHECK_EQ(size, 0);
		CHECK_EQ(reader.offset, 0);
	}
	memset(text, 'n', 65);
	wire_reset(&input);
	wire_string(&input, text, 65);
	ssh_reader_init(&reader, input.data, input.size);
	ssh_reader_get_namelist(&reader, &list, &size);
	CHECK_EQ(ssh_reader_error(&reader), EINVAL);
	wire_reset(&input);
	wire_string(&input, text, 64);
	ssh_reader_init(&reader, input.data, input.size);
	ssh_reader_get_namelist(&reader, &list, &size);
	CHECK_EQ(ssh_reader_error(&reader), 0);
	CHECK_EQ(size, 64);

	/* A NUL byte inside a name is refused. */
	wire_reset(&input);
	wire_string(&input, "a\0b", 3);
	ssh_reader_init(&reader, input.data, input.size);
	ssh_reader_get_namelist(&reader, &list, &size);
	CHECK_EQ(ssh_reader_error(&reader), EINVAL);

	/* The list of exactly SSH_NAMELIST_MAX bytes is read and one byte more is EMSGSIZE. */
	for (index = 0; index < sizeof(text); index++) {
		if (index % 9 == 8)
			text[index] = ',';
		else
			text[index] = 'a';
	}
	wire_reset(&input);
	wire_string(&input, text, SSH_NAMELIST_MAX);
	ssh_reader_init(&reader, input.data, input.size);
	ssh_reader_get_namelist(&reader, &list, &size);
	CHECK_EQ(ssh_reader_error(&reader), 0);
	CHECK_EQ(size, SSH_NAMELIST_MAX);
	wire_reset(&input);
	wire_string(&input, text, SSH_NAMELIST_MAX + 1);
	ssh_reader_init(&reader, input.data, input.size);
	ssh_reader_get_namelist(&reader, &list, &size);
	CHECK_EQ(ssh_reader_error(&reader), EMSGSIZE);
	CHECK(list == NULL);
	CHECK_EQ(size, 0);

	/* A string that runs past the end is EINVAL. */
	ssh_reader_init(&reader, (const uint8_t *)"\0\0\0\5abc", 7);
	ssh_reader_get_namelist(&reader, &list, &size);
	CHECK_EQ(ssh_reader_error(&reader), EINVAL);
	CHECK(list == NULL);

	/* After an error nothing is read. */
	wire_reset(&input);
	wire_text(&input, "a");
	ssh_reader_init(&reader, input.data, input.size);
	(void)ssh_reader_get_bytes(&reader, 1000);
	ssh_reader_get_namelist(&reader, &list, &size);
	CHECK(list == NULL);
	CHECK_EQ(size, 0);
}

/*
 * Checks ssh_reader_finish and ssh_reader_left.
 */
static void
test_reader_finish(void)
{
	struct ssh_reader reader;
	char out[3];

	/* Bytes left over are a malformed message, and the left count follows the offset. */
	ssh_reader_init(&reader, (const uint8_t *)"\1\2\3", 3);
	CHECK_EQ(ssh_reader_left(&reader), 3);
	CHECK_EQ(ssh_reader_finish(&reader), EINVAL);
	CHECK_EQ(ssh_reader_error(&reader), 0);
	(void)ssh_reader_get_u8(&reader);
	CHECK_EQ(ssh_reader_left(&reader), 2);
	CHECK_EQ(ssh_reader_finish(&reader), EINVAL);
	(void)ssh_reader_get_u8(&reader);
	(void)ssh_reader_get_u8(&reader);
	CHECK_EQ(ssh_reader_left(&reader), 0);
	CHECK_EQ(ssh_reader_finish(&reader), 0);

	/* After an error the recorded error is reported, whether or not bytes remain. */
	ssh_reader_init(&reader, (const uint8_t *)"\1\2\3", 3);
	(void)ssh_reader_get_u32(&reader);
	CHECK_EQ(ssh_reader_finish(&reader), EINVAL);
	CHECK_EQ(ssh_reader_error(&reader), EINVAL);
	CHECK_EQ(ssh_reader_left(&reader), 0);

	/* A recorded EMSGSIZE is reported as it is. */
	wire_reset(&input);
	wire_text(&input, "abcdef");
	ssh_reader_init(&reader, input.data, input.size);
	ssh_reader_get_cstring(&reader, out, sizeof(out));
	CHECK_EQ(ssh_reader_finish(&reader), EMSGSIZE);
	CHECK_EQ(ssh_reader_error(&reader), EMSGSIZE);
}

/*
 * Returns the next pseudo-random 32-bit number (xorshift32).
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
 * Writes random sequences of values and reads them back: what the reader
 * returns is what the writer was given.
 */
static void
test_roundtrip_random(void)
{
	enum { KIND_COUNT = 7, STEP_MAX = 8 };
	struct ssh_buf buf;
	struct ssh_reader reader;
	uint32_t kinds[STEP_MAX];
	uint32_t values32[STEP_MAX];
	uint64_t values64[STEP_MAX];
	uint8_t strings[STEP_MAX][20];
	size_t lengths[STEP_MAX];
	const uint8_t *data;
	size_t size;
	uint32_t steps;
	uint32_t step;
	uint32_t round;
	uint32_t index;
	int mismatches;

	mismatches = 0;

	/* Runs many random messages. */
	for (round = 0; round < 20000; round++) {
		steps = next_random() % STEP_MAX + 1;
		ssh_buf_init(&buf, 4096, round % 2);

		/* Writes the steps, remembering what was written. */
		for (step = 0; step < steps; step++) {
			kinds[step] = next_random() % KIND_COUNT;
			values32[step] = next_random();
			values64[step] = ((uint64_t)next_random() << 32) | next_random();
			lengths[step] = next_random() % 20;
			for (index = 0; index < 20; index++)
				strings[step][index] = (uint8_t)next_random();
			if (kinds[step] == 0)
				ssh_buf_put_u8(&buf, (uint8_t)values32[step]);
			else if (kinds[step] == 1)
				ssh_buf_put_bool(&buf, (int)(values32[step] & 1U));
			else if (kinds[step] == 2)
				ssh_buf_put_u32(&buf, values32[step]);
			else if (kinds[step] == 3)
				ssh_buf_put_u64(&buf, values64[step]);
			else if (kinds[step] == 4)
				ssh_buf_put_string(&buf, strings[step], lengths[step]);
			else if (kinds[step] == 5)
				ssh_buf_put_bytes(&buf, strings[step], lengths[step]);
			else
				ssh_buf_put_u32(&buf, 0);
		}
		if (ssh_buf_error(&buf) != 0)
			mismatches++;

		/* Reads the steps back in the same order. */
		ssh_reader_init(&reader, buf.data, buf.size);
		for (step = 0; step < steps; step++) {
			if (kinds[step] == 0) {
				if (ssh_reader_get_u8(&reader) != (uint8_t)values32[step])
					mismatches++;
			} else if (kinds[step] == 1) {
				if (ssh_reader_get_bool(&reader) != (int)(values32[step] & 1U))
					mismatches++;
			} else if (kinds[step] == 2) {
				if (ssh_reader_get_u32(&reader) != values32[step])
					mismatches++;
			} else if (kinds[step] == 3) {
				if (ssh_reader_get_u64(&reader) != values64[step])
					mismatches++;
			} else if (kinds[step] == 4) {
				ssh_reader_get_string(&reader, &data, &size);
				if (size != lengths[step] || (size > 0 && memcmp(data, strings[step], size) != 0))
					mismatches++;
			} else if (kinds[step] == 5) {
				data = ssh_reader_get_bytes(&reader, lengths[step]);
				if (lengths[step] > 0 && (data == NULL || memcmp(data, strings[step], lengths[step]) != 0))
					mismatches++;
			} else {
				if (ssh_reader_get_u32(&reader) != 0)
					mismatches++;
			}
		}
		if (ssh_reader_finish(&reader) != 0)
			mismatches++;
		ssh_buf_free(&buf);
	}
	CHECK_EQ(mismatches, 0);
}

/*
 * Reads 100000 random inputs with random sequences of every reader
 * function.  The input is a heap block of exactly its size, so the
 * address sanitizer sees any read past the end.  The checks are the
 * invariants: the offset stays within the data, an error is sticky and
 * freezes the offset, and what a read returns lies inside the data.
 */
static void
test_reader_random(void)
{
	struct ssh_reader reader;
	uint8_t *data;
	size_t length;
	size_t index;
	uint32_t round;
	uint32_t calls;
	uint32_t call;
	uint32_t kind;
	int failures;
	int had_error;
	size_t offset_before;
	const uint8_t *bytes;
	const char *list;
	size_t size;
	char text[20];
	uint32_t mode;

	failures = 0;

	/* Runs many random inputs. */
	for (round = 0; round < 100000; round++) {
		/* Makes the input: short, often with small values so that string lengths are sometimes valid. */
		length = next_random() % 48;
		data = malloc(length == 0 ? 1 : length);
		if (data == NULL) {
			failures++;
			continue;
		}
		mode = next_random() % 3;
		for (index = 0; index < length; index++) {
			if (mode == 0)
				data[index] = (uint8_t)next_random();
			else if (mode == 1)
				data[index] = (uint8_t)(next_random() % 8);
			else
				data[index] = (uint8_t)(0x61 + next_random() % 8);
		}

		/* Starts the reader; a zero length is given no data now and then. */
		if (length == 0 && (next_random() & 1U) != 0)
			ssh_reader_init(&reader, NULL, 0);
		else
			ssh_reader_init(&reader, data, length);

		/* Calls the reader functions in a random order. */
		calls = next_random() % 12 + 1;
		for (call = 0; call < calls; call++) {
			kind = next_random() % 9;
			had_error = ssh_reader_error(&reader);
			offset_before = reader.offset;
			if (kind == 0)
				(void)ssh_reader_get_u8(&reader);
			else if (kind == 1)
				(void)ssh_reader_get_bool(&reader);
			else if (kind == 2)
				(void)ssh_reader_get_u32(&reader);
			else if (kind == 3)
				(void)ssh_reader_get_u64(&reader);
			else if (kind == 4) {
				bytes = ssh_reader_get_bytes(&reader, next_random() % 20);
				if (bytes != NULL && (bytes < data || bytes > data + length))
					failures++;
			} else if (kind == 5) {
				ssh_reader_get_string(&reader, &bytes, &size);
				if (size > length || (size > 0 && (bytes < data || bytes + size > data + length)))
					failures++;
				if (ssh_reader_error(&reader) != 0 && (bytes != NULL || size != 0))
					failures++;
			} else if (kind == 6) {
				ssh_reader_get_cstring(&reader, text, next_random() % (sizeof(text) + 1));
			} else if (kind == 7) {
				ssh_reader_get_namelist(&reader, &list, &size);
				if (ssh_reader_error(&reader) == 0 && size > 0 && ssh_namelist_check(list, size) != 0)
					failures++;
			} else {
				(void)ssh_reader_left(&reader);
				(void)ssh_reader_finish(&reader);
			}

			/* The offset stays inside the data. */
			if (reader.offset > length)
				failures++;

			/* An error that was there stays, and the offset does not move after it. */
			if (had_error != 0 && (ssh_reader_error(&reader) != had_error || reader.offset != offset_before))
				failures++;

			/* A reader with an error has nothing left. */
			if (ssh_reader_error(&reader) != 0 && ssh_reader_left(&reader) != 0)
				failures++;
		}
		free(data);
	}
	CHECK_EQ(failures, 0);
}
