/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The tests of lbits.c: the reader against streams worked out by hand and
 * against streams the lbitwriter makes from random widths and values.
 */

#include "check.h"
#include "lbits.h"
#include "lbitwriter.h"

/* The most fields of one random stream. */
#define FIELDS_MAX	400U

/* The stream the random tests write, kept out of the stack (it is 64 KiB). */
static struct lbitwriter stream;

static void test_by_hand(void);
static void test_empty(void);
static void test_end(void);
static void test_peek_skip(void);
static void test_random_read(unsigned long seed);
static void test_random_mixed(unsigned long seed);

/*
 * Runs the tests of lbits.c.
 */
int
main(
	void)
{
	unsigned long seed;
	int status;

	/* The streams worked out by hand, then the edges of the data. */
	test_by_hand();
	test_empty();
	test_end();
	test_peek_skip();

	/* Random streams, each from its own seed. */
	for (seed = 1; seed <= 300UL; seed++) {
		test_random_read(seed);
		test_random_mixed(seed + 1000UL);
	}

	/* The summary line. */
	status = check_finish("lbits");
	return status;
}

/* Checks reads against bytes whose bits are known. */
static void
test_by_hand(void)
{
	static const uint8_t bytes[6] = { 0x01U, 0x02U, 0xb4U, 0xffU, 0x00U, 0x80U };
	struct webpc_lbits reader;

	/* The first bit is bit 0 of the first byte: 0x01 is 1, then seven zeros. */
	webpc_lbits_init(&reader, bytes, sizeof(bytes));
	CHECK(webpc_lbits_read(&reader, 1) == 1UL);
	CHECK(webpc_lbits_read(&reader, 7) == 0UL);

	/* The second byte, then 0xb4 = 1011 0100 read 4 bits (0100) and then 4 bits (1011). */
	CHECK(webpc_lbits_read(&reader, 8) == 0x02UL);
	CHECK(webpc_lbits_read(&reader, 4) == 0x4UL);
	CHECK(webpc_lbits_read(&reader, 4) == 0xbUL);

	/*
	 * A read across a byte boundary: 0xff then 0x00 is 8 ones then 8 zeros,
	 * so 3 bits leave 5 ones and a read of 10 bits is 5 ones and 5 zeros.
	 */
	CHECK(webpc_lbits_read(&reader, 3) == 7UL);
	CHECK(webpc_lbits_read(&reader, 10) == 0x1fUL);

	/* The rest: 3 zeros of 0x00, then 0x80 whose bit 7 is the last bit of the data. */
	CHECK(webpc_lbits_read(&reader, 6) == 0UL);
	CHECK(webpc_lbits_read(&reader, 4) == 0UL);
	CHECK(webpc_lbits_read(&reader, 1) == 1UL);
	CHECK(webpc_lbits_ended(&reader) == 0);

	/* The widest read takes three bytes, the first in the low byte. */
	webpc_lbits_init(&reader, bytes, sizeof(bytes));
	CHECK(webpc_lbits_read(&reader, 24) == 0xb40201UL);
	CHECK(webpc_lbits_read(&reader, 24) == 0x8000ffUL);
	CHECK(webpc_lbits_ended(&reader) == 0);
}

/* Checks a reader over no data. */
static void
test_empty(void)
{
	static const uint8_t none[1] = { 0 };
	struct webpc_lbits reader;

	/* Size 0 with a NULL pointer: 0 bits is fine and the end is not reached. */
	webpc_lbits_init(&reader, NULL, 0);
	CHECK(webpc_lbits_read(&reader, 0) == 0UL);
	CHECK(webpc_lbits_peek(&reader, 24) == 0UL);
	CHECK(webpc_lbits_ended(&reader) == 0);
	webpc_lbits_skip(&reader, 0);
	CHECK(webpc_lbits_ended(&reader) == 0);

	/* One bit consumed is one past the end, which reads 0, and the end stays noted. */
	CHECK(webpc_lbits_read(&reader, 1) == 0UL);
	CHECK(webpc_lbits_ended(&reader) == 1);
	CHECK(webpc_lbits_read(&reader, 24) == 0UL);
	CHECK(webpc_lbits_ended(&reader) == 1);

	/* Size 0 with a pointer to some bytes reads nothing of them. */
	webpc_lbits_init(&reader, none, 0);
	CHECK(webpc_lbits_peek(&reader, 8) == 0UL);
	webpc_lbits_skip(&reader, 1);
	CHECK(webpc_lbits_ended(&reader) == 1);
}

/* Checks the exact end of the data and one bit past it. */
static void
test_end(void)
{
	static const uint8_t one[1] = { 0xa5U };
	static const uint8_t three[3] = { 0xffU, 0xffU, 0xffU };
	struct webpc_lbits reader;
	unsigned total;

	/* Reading exactly the last bit does not pass the end. */
	webpc_lbits_init(&reader, one, sizeof(one));
	CHECK(webpc_lbits_read(&reader, 8) == 0xa5UL);
	CHECK(webpc_lbits_ended(&reader) == 0);
	CHECK(webpc_lbits_read(&reader, 0) == 0UL);
	CHECK(webpc_lbits_ended(&reader) == 0);

	/* The next bit is past the end: it reads 0. */
	CHECK(webpc_lbits_read(&reader, 1) == 0UL);
	CHECK(webpc_lbits_ended(&reader) == 1);

	/* A read that crosses the end gives the bits that exist and zeros after them. */
	webpc_lbits_init(&reader, one, sizeof(one));
	CHECK(webpc_lbits_read(&reader, 9) == 0xa5UL);
	CHECK(webpc_lbits_ended(&reader) == 1);

	/* A wide read over 3 ones bytes: 24 bits, then 1 more past the end. */
	webpc_lbits_init(&reader, three, sizeof(three));
	CHECK(webpc_lbits_read(&reader, 20) == 0xfffffUL);
	CHECK(webpc_lbits_ended(&reader) == 0);
	CHECK(webpc_lbits_read(&reader, 5) == 0xfUL);
	CHECK(webpc_lbits_ended(&reader) == 1);

	/* Exactly the 24 bits of the data read in two pieces pass no end, and one more passes it. */
	for (total = 1; total <= 24U; total++) {
		webpc_lbits_init(&reader, three, sizeof(three));
		webpc_lbits_skip(&reader, 24U - total);
		webpc_lbits_skip(&reader, total);
		CHECK(webpc_lbits_ended(&reader) == 0);
		webpc_lbits_skip(&reader, 1);
		CHECK(webpc_lbits_ended(&reader) == 1);
	}
}

/* Checks that a peek consumes nothing, and a skip consumes what it says. */
static void
test_peek_skip(void)
{
	static const uint8_t bytes[2] = { 0x6bU, 0x3cU };
	struct webpc_lbits reader;

	/* A peek, however often, leaves the position and the end alone. */
	webpc_lbits_init(&reader, bytes, sizeof(bytes));
	CHECK(webpc_lbits_peek(&reader, 16) == 0x3c6bUL);
	CHECK(webpc_lbits_peek(&reader, 16) == 0x3c6bUL);
	CHECK(webpc_lbits_peek(&reader, 4) == 0xbUL);

	/* A peek past the end gives zeros above the data and does not note the end. */
	CHECK(webpc_lbits_peek(&reader, 24) == 0x3c6bUL);
	CHECK(webpc_lbits_ended(&reader) == 0);
	CHECK(webpc_lbits_read(&reader, 16) == 0x3c6bUL);
	CHECK(webpc_lbits_peek(&reader, 24) == 0UL);
	CHECK(webpc_lbits_ended(&reader) == 0);

	/* A skip moves the position by its bits. */
	webpc_lbits_init(&reader, bytes, sizeof(bytes));
	webpc_lbits_skip(&reader, 4);
	CHECK(webpc_lbits_peek(&reader, 12) == 0x3c6UL);
	webpc_lbits_skip(&reader, 0);
	CHECK(webpc_lbits_read(&reader, 12) == 0x3c6UL);
	CHECK(webpc_lbits_ended(&reader) == 0);

	/* A skip past the end notes the end. */
	webpc_lbits_skip(&reader, 1);
	CHECK(webpc_lbits_ended(&reader) == 1);
}

/* Writes a random stream of widths and values and reads every field back with read. */
static void
test_random_read(
	unsigned long seed)
{
	unsigned long state;
	unsigned widths[FIELDS_MAX];
	uint32_t values[FIELDS_MAX];
	unsigned fields;
	unsigned index;
	struct webpc_lbits reader;
	unsigned long limit;

	/* A stream of random fields, each of 0 to 24 bits and a value that fits. */
	state = seed * 2654435761UL + 12345UL;
	if ((state & 0xffffffffUL) == 0UL)
		state = 1UL;
	lbitwriter_init(&stream);
	fields = (unsigned)(check_random(&state) % FIELDS_MAX) + 1U;
	for (index = 0; index < fields; index++) {
		widths[index] = (unsigned)(check_random(&state) % 25UL);
		limit = 1UL << widths[index];
		values[index] = (uint32_t)(check_random(&state) % limit);
		lbitwriter_put(&stream, values[index], widths[index]);
	}
	CHECK(stream.overflow == 0);

	/* Every field reads back as written, and the end is not passed. */
	webpc_lbits_init(&reader, stream.bytes, lbitwriter_size(&stream));
	for (index = 0; index < fields; index++)
		CHECK(webpc_lbits_read(&reader, widths[index]) == values[index]);
	CHECK(webpc_lbits_ended(&reader) == 0);

	/* The padding bits of the last byte are zero, and past them is the end. */
	webpc_lbits_skip(&reader, (unsigned)(lbitwriter_size(&stream) * 8U - stream.bits));
	CHECK(webpc_lbits_ended(&reader) == 0);
	webpc_lbits_skip(&reader, 1);
	CHECK(webpc_lbits_ended(&reader) == 1);
}

/* Reads a random stream with a random mix of peek, skip and read. */
static void
test_random_mixed(
	unsigned long seed)
{
	unsigned long state;
	unsigned widths[FIELDS_MAX];
	uint32_t values[FIELDS_MAX];
	unsigned fields;
	unsigned index;
	unsigned operation;
	struct webpc_lbits reader;
	unsigned long limit;

	/* A stream of random fields, as the read test makes them. */
	state = seed * 2654435761UL + 777UL;
	if ((state & 0xffffffffUL) == 0UL)
		state = 1UL;
	lbitwriter_init(&stream);
	fields = (unsigned)(check_random(&state) % FIELDS_MAX) + 1U;
	for (index = 0; index < fields; index++) {
		widths[index] = (unsigned)(check_random(&state) % 25UL);
		limit = 1UL << widths[index];
		values[index] = (uint32_t)(check_random(&state) % limit);
		lbitwriter_put(&stream, values[index], widths[index]);
	}

	/*
	 * Takes each field by a read, or by peeks that agree and then a read, or
	 * by a skip (whose value the writer knows); the position stays right.
	 */
	webpc_lbits_init(&reader, stream.bytes, lbitwriter_size(&stream));
	for (index = 0; index < fields; index++) {
		operation = (unsigned)(check_random(&state) % 3UL);
		if (operation == 0U) {
			CHECK(webpc_lbits_read(&reader, widths[index]) == values[index]);
		} else if (operation == 1U) {
			CHECK(webpc_lbits_peek(&reader, widths[index]) == values[index]);
			CHECK(webpc_lbits_peek(&reader, widths[index]) == values[index]);
			CHECK(webpc_lbits_read(&reader, widths[index]) == values[index]);
		} else {
			webpc_lbits_skip(&reader, widths[index]);
		}
		CHECK(webpc_lbits_ended(&reader) == 0);
	}
}
