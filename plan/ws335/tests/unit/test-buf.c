/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks the reader and writer of the TLS presentation language
 * (sslc-buf.h).  Expected values come from RFC 8446 section 3 (the byte
 * order of 3.1 and 3.3, the vector rules of 3.4 and the examples there),
 * from byte patterns written out by hand, from the ClientHello,
 * ServerHello and HelloRetryRequest of RFC 8448 (taken from the RFC's
 * text; parsed with the reader and rewritten with the writer), and from a
 * model of the stream rules written in this file for random operations.
 * Every input is a heap block of exactly its size, so a read or write
 * outside it is caught by AddressSanitizer.
 */

#include "check.h"

#include "userland/base/libssl-compat/sslc-buf.h"

/* The state of the deterministic generator of the test inputs. */
static uint32_t random_state = 0x6d2b79f5U;

/* Returns the next pseudo-random 32-bit value (xorshift; not a secret). */
static uint32_t random_next(void);

/* Checks the integer examples of RFC 8446 3.3 and the byte order of 3.1. */
static void test_integers(void);

/* Checks reads that run past the end: zeros, the error, and the error staying. */
static void test_short_reads(void);

/* Checks sslc_reader_bytes. */
static void test_reader_bytes(void);

/* Checks sslc_reader_vector, including the examples of RFC 8446 3.4. */
static void test_reader_vector(void);

/* Checks sslc_reader_finish. */
static void test_reader_finish(void);

/* Checks the integer writers: the low bits, the byte order and the room. */
static void test_writer_integers(void);

/* Checks sslc_writer_bytes. */
static void test_writer_bytes(void);

/* Checks vectors written with begin and end, including the prefix limits. */
static void test_writer_vectors(void);

/* Checks the open-vector stack: the depth, an end without a begin, and a vector left open. */
static void test_writer_depth(void);

/* Parses and rewrites the three hellos of RFC 8448. */
static void test_rfc8448(void);

/* Runs the reader over random bytes of random sizes against the model. */
static void test_random_reader(void);

/* Runs random writer operations on a guarded buffer and the model, and compares the bytes and the status. */
static void test_random_writer(void);

/* Writes random trees of vectors and integers, reads them back with the reader, and compares. */
static void test_round_trip(void);

/* Checks that a writer rejects, and a reader refuses, structure that does not fit a given prefix width. */
static void test_prefix_fit(void);

/* Returns the next pseudo-random 32-bit value (xorshift; not a secret). */
static uint32_t
random_next(void)
{
	/* The three xorshift steps. */
	random_state ^= random_state << 13;
	random_state ^= random_state >> 17;
	random_state ^= random_state << 5;
	return random_state;
}

/* Returns a pseudo-random number below limit (limit above 0). */
static uint32_t random_below(uint32_t limit);

/* Returns a heap block of exactly size bytes holding a copy of source (size 0 gives a 1-byte block never read). */
static uint8_t * exact_copy(const uint8_t *source, size_t size);

/* Writes value as a big-endian integer of width bytes into out, by shifts written out here. */
static void put_integer(uint8_t *out, size_t width, uint32_t value);

/* Reads one integer of width bytes (1 to 4) with the reader. */
static uint32_t read_integer(struct sslc_reader *reader, size_t width);

/* Builds a vector in a block: a prefix of width bytes, then length bytes counting up from seed. */
static uint8_t * make_vector(size_t width, size_t length, size_t extra, uint8_t seed, size_t *total);

/* Fills a guarded buffer: guard bytes of 0xa5 around a region of room bytes at offset 8 (room at most 120). */
static void guarded_init(uint8_t *buffer, size_t room);

/* Returns 1 when every byte outside the region (the guard) and after the used part still has the fill value. */
static int guarded_intact(const uint8_t *buffer, size_t room, size_t used);

/* Returns a pseudo-random number below limit (limit above 0). */
static uint32_t
random_below(
	uint32_t limit)
{
	/* The high bits are the better ones. */
	return (random_next() >> 8) % limit;
}

/* Returns a heap block of exactly size bytes holding a copy of source (size 0 gives a 1-byte block never read). */
static uint8_t *
exact_copy(
	const uint8_t *source,
	size_t size)
{
	uint8_t *block;
	size_t room;

	/* A block that ends where the bytes end. */
	room = 1;
	if (size > 0)
		room = size;
	block = (uint8_t *)malloc(room);
	if (block == NULL)
		abort();
	if (size > 0)
		memcpy(block, source, size);
	return block;
}

/* Writes value as a big-endian integer of width bytes into out, by shifts written out here. */
static void
put_integer(
	uint8_t *out,
	size_t width,
	uint32_t value)
{
	size_t index;

	/* Most significant byte first (RFC 8446 3.1). */
	for (index = 0; index < width; index++)
		out[index] = (uint8_t)((value >> (8U * (unsigned)(width - 1U - index))) & 0xffU);
}

/* Reads one integer of width bytes (1 to 4) with the reader. */
static uint32_t
read_integer(
	struct sslc_reader *reader,
	size_t width)
{
	/* The reader function of the width. */
	if (width == 1U)
		return sslc_reader_u8(reader);
	if (width == 2U)
		return sslc_reader_u16(reader);
	if (width == 3U)
		return sslc_reader_u24(reader);
	return sslc_reader_u32(reader);
}

/* Checks the integer examples of RFC 8446 3.3 and the byte order of 3.1. */
static void
test_integers(void)
{
	static const uint8_t example[4] = { 0x01, 0x02, 0x03, 0x04 };
	static const uint32_t values[] = {
		0x00000000U, 0x00000001U, 0x0000007fU, 0x00000080U, 0x000000ffU,
		0x00000100U, 0x00007fffU, 0x00008000U, 0x0000ffffU, 0x00010000U,
		0x007fffffU, 0x00800000U, 0x00ffffffU, 0x01000000U, 0x7fffffffU,
		0x80000000U, 0xffffffffU, 0x01020304U, 0xdeadbeefU, 0x00ff00ffU
	};
	static const uint8_t sequence[10] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa };
	struct sslc_reader reader;
	uint8_t *block;
	uint8_t pattern[4];
	size_t which;
	size_t width;
	uint32_t expected;
	uint32_t mask;

	/* The uint32 of 01 02 03 04 is 16909060 (RFC 8446 3.3). */
	block = exact_copy(example, 4);
	sslc_reader_init(&reader, block, 4);
	CHECK(sslc_reader_u32(&reader) == 16909060U);
	CHECK(reader.error == 0);
	CHECK(sslc_reader_left(&reader) == 0U);
	CHECK(sslc_reader_finish(&reader) == 0);
	free(block);

	/* The same bytes as a 1-, 2- and 3-byte integer (3.1: byte[0] is the most significant). */
	block = exact_copy(example, 4);
	sslc_reader_init(&reader, block, 4);
	CHECK(sslc_reader_u8(&reader) == 0x01U);
	CHECK(sslc_reader_left(&reader) == 3U);
	sslc_reader_init(&reader, block, 4);
	CHECK(sslc_reader_u16(&reader) == 0x0102U);
	CHECK(sslc_reader_left(&reader) == 2U);
	sslc_reader_init(&reader, block, 4);
	CHECK(sslc_reader_u24(&reader) == 0x010203U);
	CHECK(sslc_reader_left(&reader) == 1U);
	free(block);

	/* Every width over boundary values, the bytes made by hand. */
	for (which = 0; which < sizeof(values) / sizeof(values[0]); which++) {
		for (width = 1; width <= 4U; width++) {
			mask = 0xffffffffU;
			if (width < 4U)
				mask = (1U << (8U * (unsigned)width)) - 1U;
			expected = values[which] & mask;
			put_integer(pattern, width, expected);
			block = exact_copy(pattern, width);
			sslc_reader_init(&reader, block, width);
			CHECK(read_integer(&reader, width) == expected);
			CHECK(reader.error == 0);
			CHECK(sslc_reader_finish(&reader) == 0);
			free(block);
		}
	}

	/* Consecutive reads advance: 1 + 2 + 3 + 4 bytes out of 10. */
	block = exact_copy(sequence, 10);
	sslc_reader_init(&reader, block, 10);
	CHECK(sslc_reader_left(&reader) == 10U);
	CHECK(sslc_reader_u8(&reader) == 0x11U);
	CHECK(sslc_reader_left(&reader) == 9U);
	CHECK(sslc_reader_u16(&reader) == 0x2233U);
	CHECK(sslc_reader_left(&reader) == 7U);
	CHECK(sslc_reader_u24(&reader) == 0x445566U);
	CHECK(sslc_reader_left(&reader) == 4U);
	CHECK(sslc_reader_finish(&reader) == EINVAL);
	CHECK(sslc_reader_u32(&reader) == 0x778899aaU);
	CHECK(sslc_reader_left(&reader) == 0U);
	CHECK(reader.error == 0);
	CHECK(sslc_reader_finish(&reader) == 0);
	free(block);
}

/* Checks reads that run past the end: zeros, the error, and the error staying. */
static void
test_short_reads(void)
{
	static const uint8_t source[8] = { 0xff, 0xee, 0xdd, 0xcc, 0xbb, 0xaa, 0x99, 0x88 };
	struct sslc_reader reader;
	uint8_t *block;
	uint8_t *view;
	size_t width;
	size_t length;

	/* Each width over every shorter input: the value is 0 and the error is set. */
	for (width = 1; width <= 4U; width++) {
		for (length = 0; length < width; length++) {
			block = exact_copy(source, length);
			view = NULL;
			if (length > 0)
				view = block;
			sslc_reader_init(&reader, view, length);
			CHECK(read_integer(&reader, width) == 0U);
			CHECK(reader.error != 0);
			CHECK(sslc_reader_left(&reader) == 0U);
			CHECK(sslc_reader_finish(&reader) == EINVAL);

			/* The error stays: no later read returns a byte, even one that remains. */
			CHECK(sslc_reader_u8(&reader) == 0U);
			CHECK(sslc_reader_u16(&reader) == 0U);
			CHECK(sslc_reader_u24(&reader) == 0U);
			CHECK(sslc_reader_u32(&reader) == 0U);
			CHECK(sslc_reader_bytes(&reader, 0) == NULL);
			CHECK(sslc_reader_bytes(&reader, 1) == NULL);
			CHECK(reader.error != 0);
			free(block);
		}
	}

	/* A read of 2 bytes with 1 left fails, and the 1 byte is not read afterwards. */
	block = exact_copy(source, 1);
	sslc_reader_init(&reader, block, 1);
	CHECK(sslc_reader_u16(&reader) == 0U);
	CHECK(sslc_reader_u8(&reader) == 0U);
	CHECK(reader.error != 0);
	free(block);

	/* An exact read at the end succeeds and a further one fails. */
	block = exact_copy(source, 3);
	sslc_reader_init(&reader, block, 3);
	CHECK(sslc_reader_u24(&reader) == 0xffeeddU);
	CHECK(reader.error == 0);
	CHECK(sslc_reader_u8(&reader) == 0U);
	CHECK(reader.error != 0);
	free(block);

	/* NULL data with a size above 0 is an empty reader that has failed. */
	sslc_reader_init(&reader, NULL, 5);
	CHECK(reader.error != 0);
	CHECK(sslc_reader_left(&reader) == 0U);
	CHECK(sslc_reader_u8(&reader) == 0U);
	CHECK(sslc_reader_bytes(&reader, 0) == NULL);
	CHECK(sslc_reader_finish(&reader) == EINVAL);
}

/* Checks sslc_reader_bytes. */
static void
test_reader_bytes(void)
{
	static const uint8_t source[6] = { 1, 2, 3, 4, 5, 6 };
	struct sslc_reader reader;
	uint8_t *block;
	const uint8_t *view;

	/* A view of the first bytes, then of the rest. */
	block = exact_copy(source, 6);
	sslc_reader_init(&reader, block, 6);
	view = sslc_reader_bytes(&reader, 2);
	CHECK(view == block);
	CHECK_BYTES(view, "\x01\x02", 2);
	CHECK(sslc_reader_left(&reader) == 4U);
	view = sslc_reader_bytes(&reader, 4);
	CHECK(view == block + 2);
	CHECK_BYTES(view, "\x03\x04\x05\x06", 4);
	CHECK(sslc_reader_left(&reader) == 0U);
	CHECK(reader.error == 0);
	CHECK(sslc_reader_finish(&reader) == 0);

	/* An empty view at the end is not an error. */
	view = sslc_reader_bytes(&reader, 0);
	CHECK(view != NULL);
	CHECK(reader.error == 0);

	/* One byte past the end fails and sets the error, and the reader gives nothing more. */
	CHECK(sslc_reader_bytes(&reader, 1) == NULL);
	CHECK(reader.error != 0);
	CHECK(sslc_reader_bytes(&reader, 0) == NULL);
	free(block);

	/* A size larger than anything left (including the largest size_t) fails without wrapping. */
	block = exact_copy(source, 6);
	sslc_reader_init(&reader, block, 6);
	CHECK(sslc_reader_u8(&reader) == 1U);
	CHECK(sslc_reader_bytes(&reader, 6) == NULL);
	CHECK(reader.error != 0);
	sslc_reader_init(&reader, block, 6);
	CHECK(sslc_reader_bytes(&reader, (size_t)-1) == NULL);
	CHECK(reader.error != 0);
	sslc_reader_init(&reader, block, 6);
	CHECK(sslc_reader_bytes(&reader, (size_t)-1 - 1U) == NULL);
	CHECK(reader.error != 0);
	free(block);

	/* An empty reader over no memory: a size 0 view is NULL without an error. */
	sslc_reader_init(&reader, NULL, 0);
	CHECK(reader.error == 0);
	CHECK(sslc_reader_left(&reader) == 0U);
	CHECK(sslc_reader_bytes(&reader, 0) == NULL);
	CHECK(reader.error == 0);
	CHECK(sslc_reader_finish(&reader) == 0);
	CHECK(sslc_reader_bytes(&reader, 1) == NULL);
	CHECK(reader.error != 0);
}

/* Builds a vector in a block: a prefix of width bytes, then length bytes counting up from seed. */
static uint8_t *
make_vector(
	size_t width,
	size_t length,
	size_t extra,
	uint8_t seed,
	size_t *total)
{
	uint8_t *block;
	size_t index;
	size_t room;

	/* The prefix, the contents and extra bytes after them. */
	*total = width + length + extra;
	room = 1;
	if (*total > 0)
		room = *total;
	block = (uint8_t *)malloc(room);
	if (block == NULL)
		abort();
	put_integer(block, width, (uint32_t)length);
	for (index = 0; index < length + extra; index++)
		block[width + index] = (uint8_t)(seed + index);
	return block;
}

/* Checks sslc_reader_vector, including the examples of RFC 8446 3.4. */
static void
test_reader_vector(void)
{
	struct sslc_reader reader;
	struct sslc_reader inner;
	struct sslc_reader deeper;
	uint8_t *block;
	size_t total;
	size_t width;
	size_t length;
	size_t index;
	int good;
	int status;
	int expected;
	static const size_t lengths[] = { 0, 1, 2, 254, 255, 256, 299, 300, 301, 399, 400, 401, 799, 800, 801 };
	static const uint8_t nested[] = { 0x00, 0x05, 0x02, 0xaa, 0xbb, 0x01, 0xcc, 0x7f };

	/* opaque mandatory<300..400>: a 2-byte prefix, 300 to 400 bytes (RFC 8446 3.4). */
	for (index = 0; index < sizeof(lengths) / sizeof(lengths[0]); index++) {
		length = lengths[index];
		block = make_vector(2, length, 0, 0x10, &total);
		sslc_reader_init(&reader, block, total);
		expected = EINVAL;
		if (length >= 300U && length <= 400U)
			expected = 0;
		CHECK(sslc_reader_vector(&reader, 2, 300, 400, &inner) == expected);
		if (length >= 300U && length <= 400U) {
			CHECK(reader.error == 0);
			CHECK(inner.error == 0);
			CHECK(sslc_reader_left(&inner) == length);
			CHECK(sslc_reader_left(&reader) == 0U);
			CHECK(inner.data == block + 2);
			CHECK(sslc_reader_u8(&inner) == 0x10U);
		} else {
			CHECK(reader.error != 0);
			CHECK(inner.error != 0);
			CHECK(sslc_reader_left(&inner) == 0U);
			CHECK(sslc_reader_u8(&inner) == 0U);
		}

		/* Frees the block. */
		free(block);
	}

	/* uint16 longer<0..800>: may be empty; 801 bytes are too many (RFC 8446 3.4). */
	for (index = 0; index < sizeof(lengths) / sizeof(lengths[0]); index++) {
		length = lengths[index];
		block = make_vector(2, length, 0, 0x20, &total);
		sslc_reader_init(&reader, block, total);
		good = sslc_reader_vector(&reader, 2, 0, 800, &inner) == 0;
		CHECK(good == (length <= 800U));
		if (good) {
			CHECK(sslc_reader_left(&inner) == length);
			CHECK(sslc_reader_finish(&reader) == 0);
		}

		/* Frees the block. */
		free(block);
	}

	/* The three prefix widths: the bounds are inclusive at both ends. */
	for (width = 1; width <= 3U; width++) {
		for (length = 0; length <= 5U; length++) {
			block = make_vector(width, length, 3, 0x30, &total);
			sslc_reader_init(&reader, block, total - 3U);
			CHECK((sslc_reader_vector(&reader, (unsigned)width, 2, 4, &inner) == 0) == (length >= 2U && length <= 4U));
			free(block);
		}
	}

	/* A length that does not fit what is left (the prefix says 5, 4 bytes follow). */
	block = make_vector(2, 5, 0, 0x40, &total);
	sslc_reader_init(&reader, block, total - 1U);
	CHECK(sslc_reader_vector(&reader, 2, 0, 100, &inner) == EINVAL);
	CHECK(reader.error != 0);
	CHECK(inner.error != 0);
	CHECK(sslc_reader_left(&inner) == 0U);
	free(block);

	/* The largest length field of each width over a short input fails without reading outside. */
	for (width = 1; width <= 3U; width++) {
		block = make_vector(width, 0, 0, 0x50, &total);
		put_integer(block, width, 0xffffffffU);
		sslc_reader_init(&reader, block, total);
		CHECK(sslc_reader_vector(&reader, (unsigned)width, 0, (size_t)-1, &inner) == EINVAL);
		CHECK(reader.error != 0);
		free(block);
	}

	/* A truncated prefix (2-byte prefix with 1 byte; 3-byte with 2; any with none). */
	block = exact_copy((const uint8_t *)"\x00\x00\x00", 3);
	for (width = 1; width <= 3U; width++) {
		sslc_reader_init(&reader, block, width - 1U);
		CHECK(sslc_reader_vector(&reader, (unsigned)width, 0, 10, &inner) == EINVAL);
		CHECK(reader.error != 0);
		CHECK(inner.error != 0);
	}

	/* Frees the block. */
	free(block);

	/* A prefix size other than 1, 2 or 3 is refused, even over bytes that would be a valid empty vector (all zeros). */
	block = exact_copy((const uint8_t *)"\x00\x00\x00\x00\x00\x00\x00\x00", 8);
	for (width = 0; width <= 6U; width++) {
		sslc_reader_init(&reader, block, 8);
		status = sslc_reader_vector(&reader, (unsigned)width, 0, 10, &inner);
		CHECK((status == 0) == (width >= 1U && width <= 3U));
		CHECK((reader.error == 0) == (width >= 1U && width <= 3U));
		CHECK((inner.error == 0) == (width >= 1U && width <= 3U));
	}

	/* Frees the block. */
	free(block);

	/* A reader that has failed refuses a vector, and the inner reader is empty and failed. */
	block = make_vector(1, 2, 0, 0x70, &total);
	sslc_reader_init(&reader, block, total);
	CHECK(sslc_reader_u32(&reader) == 0U);
	CHECK(reader.error != 0);
	CHECK(sslc_reader_vector(&reader, 1, 0, 10, &inner) == EINVAL);
	CHECK(inner.error != 0);
	CHECK(sslc_reader_left(&inner) == 0U);
	free(block);

	/* min above max accepts nothing. */
	block = make_vector(1, 3, 0, 0x80, &total);
	sslc_reader_init(&reader, block, total);
	CHECK(sslc_reader_vector(&reader, 1, 5, 2, &inner) == EINVAL);
	free(block);

	/* A vector inside a vector: 00 05 | 02 aa bb | 01 cc, then 7f outside both. */
	block = exact_copy(nested, sizeof(nested));
	sslc_reader_init(&reader, block, sizeof(nested));
	CHECK(sslc_reader_vector(&reader, 2, 0, 100, &inner) == 0);
	CHECK(sslc_reader_left(&inner) == 5U);
	CHECK(sslc_reader_vector(&inner, 1, 0, 10, &deeper) == 0);
	CHECK(sslc_reader_left(&deeper) == 2U);
	CHECK(sslc_reader_u8(&deeper) == 0xaaU);
	CHECK(sslc_reader_u8(&deeper) == 0xbbU);
	CHECK(sslc_reader_finish(&deeper) == 0);
	CHECK(sslc_reader_vector(&inner, 1, 0, 10, &deeper) == 0);
	CHECK(sslc_reader_u8(&deeper) == 0xccU);
	CHECK(sslc_reader_finish(&deeper) == 0);
	CHECK(sslc_reader_finish(&inner) == 0);
	CHECK(sslc_reader_u8(&reader) == 0x7fU);
	CHECK(sslc_reader_finish(&reader) == 0);

	/* An inner reader cannot read past its vector into the outer bytes. */
	sslc_reader_init(&reader, block, sizeof(nested));
	CHECK(sslc_reader_vector(&reader, 2, 0, 100, &inner) == 0);
	CHECK(sslc_reader_vector(&inner, 1, 0, 10, &deeper) == 0);
	CHECK(sslc_reader_u24(&deeper) == 0U);
	CHECK(deeper.error != 0);
	CHECK(inner.error == 0);
	CHECK(reader.error == 0);
	free(block);
}

/* Checks sslc_reader_finish. */
static void
test_reader_finish(void)
{
	static const uint8_t source[3] = { 1, 2, 3 };
	struct sslc_reader reader;
	uint8_t *block;

	/* An empty reader is at its end. */
	sslc_reader_init(&reader, NULL, 0);
	CHECK(sslc_reader_finish(&reader) == 0);

	/* Unread bytes are trailing bytes. */
	block = exact_copy(source, 3);
	sslc_reader_init(&reader, block, 3);
	CHECK(sslc_reader_finish(&reader) == EINVAL);
	CHECK(sslc_reader_u16(&reader) == 0x0102U);
	CHECK(sslc_reader_finish(&reader) == EINVAL);
	CHECK(sslc_reader_u8(&reader) == 3U);
	CHECK(sslc_reader_finish(&reader) == 0);

	/* A failed reader never finishes cleanly, even with no bytes left over. */
	CHECK(sslc_reader_u8(&reader) == 0U);
	CHECK(sslc_reader_finish(&reader) == EINVAL);
	free(block);
}

/* Fills a guarded buffer: guard bytes of 0xa5 around a region of room bytes at offset 8 (room at most 120). */
static void
guarded_init(
	uint8_t *buffer,
	size_t room)
{
	/* Everything 0xa5, so a write outside the region or a zero byte written inside shows. */
	memset(buffer, 0xa5, 8U + 120U + 8U);
	(void)room;
}

/* Returns 1 when every byte outside the region (the guard) and after the used part still has the fill value. */
static int
guarded_intact(
	const uint8_t *buffer,
	size_t room,
	size_t used)
{
	size_t index;

	/* The 8 bytes before the region and everything after it. */
	for (index = 0; index < 8U; index++) {
		if (buffer[index] != 0xa5U)
			return 0;
	}

	/* The guard after the usable room is untouched. */
	for (index = 8U + room; index < 8U + 120U + 8U; index++) {
		if (buffer[index] != 0xa5U)
			return 0;
	}

	/* Bytes inside the region that were not written (past the used part, not under an open vector). */
	for (index = 8U + used; index < 8U + room; index++) {
		if (buffer[index] != 0xa5U)
			return 0;
	}

	/* Succeeded: both guards are intact. */
	return 1;
}

/* Checks the integer writers: the low bits, the byte order and the room. */
static void
test_writer_integers(void)
{
	struct sslc_writer writer;
	uint8_t buffer[8 + 120 + 8];
	size_t room;

	/* Values wider than the field keep their low bits. */
	guarded_init(buffer, 16);
	sslc_writer_init(&writer, buffer + 8, 16);
	sslc_writer_u8(&writer, 0x12345678U);
	sslc_writer_u16(&writer, 0x12345678U);
	sslc_writer_u24(&writer, 0x12345678U);
	sslc_writer_u24(&writer, 0xffffffffU);
	sslc_writer_u8(&writer, 0x1ffU);
	sslc_writer_u16(&writer, 0x10001U);
	CHECK(writer.used == 12U);
	CHECK_BYTES(buffer + 8, "\x78\x56\x78\x34\x56\x78\xff\xff\xff\xff\x00\x01", 12);
	CHECK(sslc_writer_finish(&writer) == 0);
	CHECK(guarded_intact(buffer, 16, 12));

	/* The examples of 3.3: 01 02 03 04 as bytes of 3 and 1 and as 2 and 2. */
	sslc_writer_init(&writer, buffer + 8, 16);
	sslc_writer_u24(&writer, 0x010203U);
	sslc_writer_u8(&writer, 0x04U);
	CHECK_BYTES(buffer + 8, "\x01\x02\x03\x04", 4);
	sslc_writer_init(&writer, buffer + 8, 16);
	sslc_writer_u16(&writer, 0x0102U);
	sslc_writer_u16(&writer, 0x0304U);
	CHECK_BYTES(buffer + 8, "\x01\x02\x03\x04", 4);

	/* Exactly filling the room is not an error. */
	sslc_writer_init(&writer, buffer + 8, 3);
	sslc_writer_u24(&writer, 0xabcdefU);
	CHECK(writer.error == 0);
	CHECK(writer.used == 3U);
	CHECK(sslc_writer_finish(&writer) == 0);

	/* One byte too many: ENOBUFS, nothing of the failing write is written, the writer stays failed. */
	for (room = 0; room < 3U; room++) {
		guarded_init(buffer, room);
		sslc_writer_init(&writer, buffer + 8, room);
		sslc_writer_u24(&writer, 0xabcdefU);
		CHECK(writer.error == ENOBUFS);
		CHECK(writer.used == 0U);
		CHECK(sslc_writer_finish(&writer) == ENOBUFS);
		CHECK(guarded_intact(buffer, room, 0));
	}

	/* A writer over a room of 5 bytes. */
	guarded_init(buffer, 5);
	sslc_writer_init(&writer, buffer + 8, 5);
	sslc_writer_u24(&writer, 0x010203U);
	sslc_writer_u24(&writer, 0x040506U);
	CHECK(writer.error == ENOBUFS);
	CHECK(writer.used == 3U);
	sslc_writer_u8(&writer, 0x77U);
	sslc_writer_bytes(&writer, (const uint8_t *)"\x88", 1);
	CHECK(writer.used == 3U);
	CHECK_BYTES(buffer + 8, "\x01\x02\x03", 3);
	CHECK(guarded_intact(buffer, 5, 3));
	CHECK(sslc_writer_finish(&writer) == ENOBUFS);

	/* NULL data with room above 0 is an empty writer that has failed with EINVAL. */
	sslc_writer_init(&writer, NULL, 4);
	CHECK(sslc_writer_finish(&writer) == EINVAL);
	sslc_writer_u8(&writer, 1);
	CHECK(writer.used == 0U);

	/* NULL data with no room is fine until something is written. */
	sslc_writer_init(&writer, NULL, 0);
	CHECK(sslc_writer_finish(&writer) == 0);
	sslc_writer_bytes(&writer, NULL, 0);
	CHECK(sslc_writer_finish(&writer) == 0);
	sslc_writer_u8(&writer, 1);
	CHECK(sslc_writer_finish(&writer) == ENOBUFS);
}

/* Checks sslc_writer_bytes. */
static void
test_writer_bytes(void)
{
	struct sslc_writer writer;
	uint8_t buffer[8 + 120 + 8];
	uint8_t source[130];
	size_t index;
	size_t room;

	/* The bytes land after those written, in order. */
	for (index = 0; index < sizeof(source); index++)
		source[index] = (uint8_t)(index + 1U);
	guarded_init(buffer, 10);
	sslc_writer_init(&writer, buffer + 8, 10);
	sslc_writer_u8(&writer, 0xf0U);
	sslc_writer_bytes(&writer, source, 4);
	sslc_writer_bytes(&writer, source + 4, 0);
	sslc_writer_bytes(&writer, source + 4, 5);
	CHECK(writer.used == 10U);
	CHECK(writer.error == 0);
	CHECK_BYTES(buffer + 8, "\xf0\x01\x02\x03\x04\x05\x06\x07\x08\x09", 10);
	CHECK(sslc_writer_finish(&writer) == 0);
	CHECK(guarded_intact(buffer, 10, 10));

	/* A length that does not fit by one byte fails with ENOBUFS and writes nothing, for room 0 to 12. */
	for (room = 0; room <= 12U; room++) {
		guarded_init(buffer, room);
		sslc_writer_init(&writer, buffer + 8, room);
		sslc_writer_bytes(&writer, source, room + 1U);
		CHECK(writer.error == ENOBUFS);
		CHECK(writer.used == 0U);
		CHECK(guarded_intact(buffer, room, 0));
		CHECK(sslc_writer_finish(&writer) == ENOBUFS);
		sslc_writer_init(&writer, buffer + 8, room);
		sslc_writer_bytes(&writer, source, room);
		CHECK(writer.error == 0);
		CHECK(writer.used == room);
	}

	/* A size near the largest size_t does not wrap the room check. */
	guarded_init(buffer, 8);
	sslc_writer_init(&writer, buffer + 8, 8);
	sslc_writer_u8(&writer, 1);
	sslc_writer_bytes(&writer, source, (size_t)-1);
	CHECK(writer.error == ENOBUFS);
	CHECK(writer.used == 1U);
	CHECK(guarded_intact(buffer, 8, 1));

	/* NULL data with a size above 0 is EINVAL; with size 0 it is nothing. */
	sslc_writer_init(&writer, buffer + 8, 8);
	sslc_writer_bytes(&writer, NULL, 0);
	CHECK(writer.error == 0);
	sslc_writer_bytes(&writer, NULL, 3);
	CHECK(writer.error == EINVAL);
	CHECK(writer.used == 0U);
	CHECK(sslc_writer_finish(&writer) == EINVAL);
}

/* Checks vectors written with begin and end, including the prefix limits. */
static void
test_writer_vectors(void)
{
	static const size_t widths[] = { 1, 2, 3 };
	static const uint32_t limits[] = { 256U, 65536U, 16777216U };
	struct sslc_writer writer;
	uint8_t buffer[8 + 120 + 8];
	uint8_t *large;
	size_t which;
	size_t limit;
	size_t index;
	size_t room;

	/* An empty vector of each prefix width. */
	guarded_init(buffer, 12);
	sslc_writer_init(&writer, buffer + 8, 12);
	sslc_writer_begin(&writer, 1);
	sslc_writer_end(&writer);
	sslc_writer_begin(&writer, 2);
	sslc_writer_end(&writer);
	sslc_writer_begin(&writer, 3);
	sslc_writer_end(&writer);
	CHECK(writer.used == 6U);
	CHECK(sslc_writer_finish(&writer) == 0);
	CHECK_BYTES(buffer + 8, "\x00\x00\x00\x00\x00\x00", 6);

	/* A server_name-like structure by hand: the extension, its list, one name (the nesting of the header comment). */
	guarded_init(buffer, 60);
	sslc_writer_init(&writer, buffer + 8, 60);
	sslc_writer_u16(&writer, 0x0000U);
	sslc_writer_begin(&writer, 2);
	sslc_writer_begin(&writer, 2);
	sslc_writer_u8(&writer, 0x00U);
	sslc_writer_begin(&writer, 2);
	sslc_writer_bytes(&writer, (const uint8_t *)"server", 6);
	sslc_writer_end(&writer);
	sslc_writer_end(&writer);
	sslc_writer_end(&writer);
	CHECK(sslc_writer_finish(&writer) == 0);
	CHECK(writer.used == 2U + 2U + 2U + 1U + 2U + 6U);
	CHECK_BYTES(buffer + 8, "\x00\x00\x00\x0b\x00\x09\x00\x00\x06server", 15);

	/* Vectors of a prefix size 4 or 0 are refused with EINVAL. */
	sslc_writer_init(&writer, buffer + 8, 60);
	sslc_writer_begin(&writer, 0);
	CHECK(sslc_writer_finish(&writer) == EINVAL);
	sslc_writer_init(&writer, buffer + 8, 60);
	sslc_writer_begin(&writer, 4);
	CHECK(sslc_writer_finish(&writer) == EINVAL);
	CHECK(writer.used == 0U);

	/* The largest and the first too-large vector of the 1- and 2-byte prefixes. */
	large = (uint8_t *)malloc(70000U);
	if (large == NULL)
		abort();
	for (which = 0; which < 2U; which++) {
		limit = limits[which];
		sslc_writer_init(&writer, large, 70000U);
		sslc_writer_begin(&writer, (unsigned)widths[which]);
		for (index = 0; index < limit - 1U; index++)
			sslc_writer_u8(&writer, 0xc3U);
		sslc_writer_end(&writer);
		CHECK(sslc_writer_finish(&writer) == 0);
		CHECK(writer.used == widths[which] + limit - 1U);
		if (which == 0U) {
			CHECK(large[0] == 0xffU);
		} else {
			CHECK(large[0] == 0xffU);
			CHECK(large[1] == 0xffU);
		}

		/* The same, with a width that cannot hold the length. */
		sslc_writer_init(&writer, large, 70000U);
		sslc_writer_begin(&writer, (unsigned)widths[which]);
		for (index = 0; index < limit; index++)
			sslc_writer_u8(&writer, 0xc3U);
		sslc_writer_end(&writer);
		CHECK(writer.error == EINVAL);
		CHECK(sslc_writer_finish(&writer) == EINVAL);
	}

	/* Frees the large block. */
	free(large);

	/* The 3-byte prefix: 2^24 - 1 bytes are the most (a block of that size is made once). */
	large = (uint8_t *)malloc(16777216U + 8U);
	if (large == NULL)
		abort();
	sslc_writer_init(&writer, large, 16777216U + 8U);
	sslc_writer_begin(&writer, 3);
	memset(large + 3, 0x5a, 16777215U);
	writer.used += 16777215U;
	sslc_writer_end(&writer);
	CHECK(sslc_writer_finish(&writer) == 0);
	CHECK(large[0] == 0xffU);
	CHECK(large[1] == 0xffU);
	CHECK(large[2] == 0xffU);
	sslc_writer_init(&writer, large, 16777216U + 8U);
	sslc_writer_begin(&writer, 3);
	memset(large + 3, 0x5a, 16777216U);
	writer.used += 16777216U;
	sslc_writer_end(&writer);
	CHECK(sslc_writer_finish(&writer) == EINVAL);
	free(large);

	/* A vector of 0 to 6 bytes: the length lands in the prefix of each width. */
	for (which = 0; which < 3U; which++) {
		for (index = 0; index <= 6U; index++) {
			guarded_init(buffer, 20);
			sslc_writer_init(&writer, buffer + 8, 20);
			sslc_writer_begin(&writer, (unsigned)widths[which]);
			sslc_writer_bytes(&writer, (const uint8_t *)"abcdef", index);
			sslc_writer_end(&writer);
			CHECK(sslc_writer_finish(&writer) == 0);
			CHECK(writer.used == widths[which] + index);
			CHECK(buffer[8 + widths[which] - 1U] == (uint8_t)index);
			if (widths[which] > 1U)
				CHECK(buffer[8] == 0U);
			if (widths[which] > 2U)
				CHECK(buffer[9] == 0U);
			CHECK(guarded_intact(buffer, 20, writer.used));
		}
	}

	/* A begin that does not fit the room: ENOBUFS, nothing written. */
	for (room = 0; room < 3U; room++) {
		guarded_init(buffer, room);
		sslc_writer_init(&writer, buffer + 8, room);
		sslc_writer_begin(&writer, 3);
		CHECK(writer.error == ENOBUFS);
		CHECK(writer.used == 0U);
		CHECK(guarded_intact(buffer, room, 0));
	}

	/* Content that runs out of room inside a vector keeps the first error (ENOBUFS), whatever follows. */
	guarded_init(buffer, 4);
	sslc_writer_init(&writer, buffer + 8, 4);
	sslc_writer_begin(&writer, 2);
	sslc_writer_bytes(&writer, (const uint8_t *)"abc", 3);
	CHECK(writer.error == ENOBUFS);
	sslc_writer_end(&writer);
	sslc_writer_end(&writer);
	CHECK(sslc_writer_finish(&writer) == ENOBUFS);
	CHECK(guarded_intact(buffer, 4, 2));

	/* A too-long vector keeps EINVAL even if the room then runs out. */
	large = (uint8_t *)malloc(300U);
	if (large == NULL)
		abort();
	sslc_writer_init(&writer, large, 300U);
	sslc_writer_begin(&writer, 1);
	for (index = 0; index < 256U; index++)
		sslc_writer_u8(&writer, 1);
	sslc_writer_end(&writer);
	CHECK(writer.error == EINVAL);
	sslc_writer_bytes(&writer, large, 299U);
	CHECK(sslc_writer_finish(&writer) == EINVAL);
	free(large);
}

/* Checks the open-vector stack: the depth, an end without a begin, and a vector left open. */
static void
test_writer_depth(void)
{
	struct sslc_writer writer;
	uint8_t buffer[8 + 120 + 8];
	unsigned level;

	/* SSLC_WRITER_DEPTH nested vectors work and close in order. */
	guarded_init(buffer, 60);
	sslc_writer_init(&writer, buffer + 8, 60);
	for (level = 0; level < SSLC_WRITER_DEPTH; level++)
		sslc_writer_begin(&writer, 1);
	CHECK(writer.error == 0);
	CHECK(writer.depth == SSLC_WRITER_DEPTH);
	sslc_writer_u8(&writer, 0xeeU);
	for (level = 0; level < SSLC_WRITER_DEPTH; level++)
		sslc_writer_end(&writer);
	CHECK(sslc_writer_finish(&writer) == 0);
	CHECK(writer.depth == 0U);
	CHECK(writer.used == SSLC_WRITER_DEPTH + 1U);
	CHECK_BYTES(buffer + 8, "\x06\x05\x04\x03\x02\x01\xee", 7);

	/* One more fails with EINVAL and writes nothing for it. */
	sslc_writer_init(&writer, buffer + 8, 60);
	for (level = 0; level < SSLC_WRITER_DEPTH; level++)
		sslc_writer_begin(&writer, 1);
	sslc_writer_begin(&writer, 1);
	CHECK(writer.error == EINVAL);
	CHECK(writer.used == SSLC_WRITER_DEPTH);
	CHECK(sslc_writer_finish(&writer) == EINVAL);

	/* An end with nothing begun is EINVAL. */
	sslc_writer_init(&writer, buffer + 8, 60);
	sslc_writer_end(&writer);
	CHECK(sslc_writer_finish(&writer) == EINVAL);
	sslc_writer_init(&writer, buffer + 8, 60);
	sslc_writer_begin(&writer, 1);
	sslc_writer_end(&writer);
	sslc_writer_end(&writer);
	CHECK(sslc_writer_finish(&writer) == EINVAL);

	/* A vector left open: finish says EINVAL, with no error set. */
	sslc_writer_init(&writer, buffer + 8, 60);
	sslc_writer_begin(&writer, 2);
	sslc_writer_begin(&writer, 2);
	sslc_writer_end(&writer);
	CHECK(writer.error == 0);
	CHECK(sslc_writer_finish(&writer) == EINVAL);

	/* The stack is reused: begin and end 20 times in a row. */
	sslc_writer_init(&writer, buffer + 8, 100);
	for (level = 0; level < 20U; level++) {
		sslc_writer_begin(&writer, 1);
		sslc_writer_u8(&writer, (uint32_t)level);
		sslc_writer_end(&writer);
	}

	/* Closes the outermost vector. */
	CHECK(sslc_writer_finish(&writer) == 0);
	CHECK(writer.used == 40U);
	CHECK(buffer[8 + 38] == 1U);
	CHECK(buffer[8 + 39] == 19U);
}

/* The extensions found in a hello: type, and the data as a view. */
struct hello_extension {
	uint32_t type;
	const uint8_t *data;
	size_t size;
};

/* The fields of a ClientHello, ServerHello or HelloRetryRequest as read. */
struct hello_fields {
	uint32_t message_type;
	uint32_t body_size;
	uint32_t version;
	const uint8_t *random;
	size_t session_size;
	const uint8_t *session;
	size_t suites_size;
	const uint8_t *suites;
	uint32_t suite;
	size_t compression_size;
	const uint8_t *compression;
	uint32_t compression_method;
	size_t extension_count;
	struct hello_extension extensions[16];
};

static int parse_hello(const uint8_t *message, size_t size, int client, struct hello_fields *fields);

/* Writes the fields of a hello back with the writer, into out; returns the writer's finish status and sets *size. */
static int write_hello(const struct hello_fields *fields, int client, uint8_t *out, size_t room, size_t *size);

/*
 * Parses a ClientHello (client nonzero) or a ServerHello (4.1.2, 4.1.3)
 * with the reader.  Returns 0 or the error of the first failed step.
 */
static int
parse_hello(
	const uint8_t *message,
	size_t size,
	int client,
	struct hello_fields *fields)
{
	struct sslc_reader reader;
	struct sslc_reader body;
	struct sslc_reader session;
	struct sslc_reader suites;
	struct sslc_reader compression;
	struct sslc_reader extensions;
	struct sslc_reader data;
	size_t left;
	int status;

	/* The handshake header: a type and a 3-byte length that must cover the rest exactly. */
	memset(fields, 0, sizeof(*fields));
	sslc_reader_init(&reader, message, size);
	fields->message_type = sslc_reader_u8(&reader);
	status = sslc_reader_vector(&reader, 3, 0, 0xffffffU, &body);
	if (status != 0)
		return status;
	status = sslc_reader_finish(&reader);
	if (status != 0)
		return status;
	fields->body_size = (uint32_t)(sslc_reader_left(&body));

	/* legacy_version and the 32-byte random. */
	fields->version = sslc_reader_u16(&body);
	fields->random = sslc_reader_bytes(&body, 32);

	/* legacy_session_id<0..32>. */
	status = sslc_reader_vector(&body, 1, 0, 32, &session);
	if (status != 0)
		return status;
	fields->session_size = sslc_reader_left(&session);
	fields->session = sslc_reader_bytes(&session, fields->session_size);

	/* The cipher suites: a list in a ClientHello, one suite in a ServerHello. */
	if (client) {
		status = sslc_reader_vector(&body, 2, 2, 0xfffeU, &suites);
		if (status != 0)
			return status;
		fields->suites_size = sslc_reader_left(&suites);
		fields->suites = sslc_reader_bytes(&suites, fields->suites_size);
	} else {
		fields->suite = sslc_reader_u16(&body);
	}

	/* The compression methods: a list<1..2^8-1> in a ClientHello, one byte in a ServerHello. */
	if (client) {
		status = sslc_reader_vector(&body, 1, 1, 255, &compression);
		if (status != 0)
			return status;
		fields->compression_size = sslc_reader_left(&compression);
		fields->compression = sslc_reader_bytes(&compression, fields->compression_size);
	} else {
		fields->compression_method = sslc_reader_u8(&body);
	}

	/* The extensions<8..2^16-1>, each a type and data<0..2^16-1>. */
	status = sslc_reader_vector(&body, 2, 6, 0xffffU, &extensions);
	if (status != 0)
		return status;
	status = sslc_reader_finish(&body);
	if (status != 0)
		return status;
	for (;;) {
		left = sslc_reader_left(&extensions);
		if (left == 0U)
			break;
		if (fields->extension_count >= 16U)
			return ERANGE;
		fields->extensions[fields->extension_count].type = sslc_reader_u16(&extensions);
		status = sslc_reader_vector(&extensions, 2, 0, 0xffffU, &data);
		if (status != 0)
			return status;
		fields->extensions[fields->extension_count].size = sslc_reader_left(&data);
		fields->extensions[fields->extension_count].data = sslc_reader_bytes(&data, fields->extensions[fields->extension_count].size);
		fields->extension_count++;
	}

	/* Checks that the extensions block was read to its end. */
	status = sslc_reader_finish(&extensions);
	if (status != 0)
		return status;

	/* Succeeded: every byte of the message was accounted for. */
	return 0;
}

/* Writes the fields of a hello back with the writer, into out; returns the writer's finish status and sets *size. */
static int
write_hello(
	const struct hello_fields *fields,
	int client,
	uint8_t *out,
	size_t room,
	size_t *size)
{
	struct sslc_writer writer;
	size_t index;

	/* The handshake header and body. */
	sslc_writer_init(&writer, out, room);
	sslc_writer_u8(&writer, fields->message_type);
	sslc_writer_begin(&writer, 3);
	sslc_writer_u16(&writer, fields->version);
	sslc_writer_bytes(&writer, fields->random, 32);
	sslc_writer_begin(&writer, 1);
	sslc_writer_bytes(&writer, fields->session, fields->session_size);
	sslc_writer_end(&writer);
	if (client) {
		sslc_writer_begin(&writer, 2);
		sslc_writer_bytes(&writer, fields->suites, fields->suites_size);
		sslc_writer_end(&writer);
		sslc_writer_begin(&writer, 1);
		sslc_writer_bytes(&writer, fields->compression, fields->compression_size);
		sslc_writer_end(&writer);
	} else {
		sslc_writer_u16(&writer, fields->suite);
		sslc_writer_u8(&writer, fields->compression_method);
	}

	/* The extensions. */
	sslc_writer_begin(&writer, 2);
	for (index = 0; index < fields->extension_count; index++) {
		sslc_writer_u16(&writer, fields->extensions[index].type);
		sslc_writer_begin(&writer, 2);
		sslc_writer_bytes(&writer, fields->extensions[index].data, fields->extensions[index].size);
		sslc_writer_end(&writer);
	}

	/* Closes the extensions and the message. */
	sslc_writer_end(&writer);
	sslc_writer_end(&writer);
	*size = writer.used;
	return sslc_writer_finish(&writer);
}

/* One expected extension of a hello of RFC 8448: its type and the length of its data. */
struct expected_extension {
	uint32_t type;
	size_t size;
};

static void check_hello(const char *hex, size_t size, int client, uint32_t message_type, uint32_t suite, const struct expected_extension *expected, size_t expected_count);

/* The ClientHello of RFC 8448 section 3, "{client} send handshake record", payload (196 octets). */
static const char client_hello_hex[] =
	"010000c00303cb34ecb1e78163ba1c38c6dacb196a6dffa21a8d9912ec18a2ef"
	"6283024dece7000006130113031302010000910000000b000900000673657276"
	"6572ff01000100000a00140012001d0017001800190100010101020103010400"
	"230000003300260024001d002099381de560e4bd43d23d8e435a7dbafeb3c06e"
	"51c13cae4d5413691e529aaf2c002b0003020304000d0020001e040305030603"
	"020308040805080604010501060102010402050206020202002d00020101001c"
	"00024001";

/* The ServerHello of RFC 8448 section 3, payload (90 octets). */
static const char server_hello_hex[] =
	"020000560303a6af06a4121860dc5e6e60249cd34c95930c8ac5cb1434dac155"
	"772ed3e2692800130100002e00330024001d0020c9828876112095fe66762bdb"
	"f7c672e156d6cc253b833df1dd69b1b04e751f0f002b00020304";

/* The HelloRetryRequest of RFC 8448 section 5, payload (176 octets). */
static const char retry_request_hex[] =
	"020000ac0303cf21ad74e59a6111be1d8c021e65b891c2a211167abb8c5e079e"
	"09e2c8a8339c001301000084003300020017002c0074007271dcd04bb88bc318"
	"9119398a00000000eefafc76c146b823b096f8aacad365dd0030953f4edf6256"
	"36e5f21bb2e23fcc654b1b5b40318d10d137abcbb87574e36e8a1f025f7dfa5d"
	"6e50781b5eda4aa15b0c8be778257d16aa3030e9e7841dd9e4c0342267e8ca0c"
	"af571fb2b7cff0f934b0002b00020304";

/*
 * Parses one hello of RFC 8448, checks the fields against the values read
 * off the RFC text, and rewrites it byte for byte.
 */
static void
check_hello(
	const char *hex,
	size_t size,
	int client,
	uint32_t message_type,
	uint32_t suite,
	const struct expected_extension *expected,
	size_t expected_count)
{
	uint8_t message[512];
	uint8_t rewritten[512];
	uint8_t *block;
	struct hello_fields fields;
	size_t rewritten_size;
	size_t index;
	size_t bytes;
	int status;

	/* The message as bytes, in a block of exactly its size. */
	bytes = hex_decode(hex, message, sizeof(message));
	CHECK(bytes == size);
	block = exact_copy(message, size);

	/* The reader accepts it and finds the fields. */
	status = parse_hello(block, size, client, &fields);
	CHECK(status == 0);
	CHECK(fields.message_type == message_type);
	CHECK(fields.body_size == size - 4U);
	CHECK(fields.version == 0x0303U);
	CHECK(fields.session_size == 0U);
	if (client) {
		CHECK(fields.suites_size == 6U);
		CHECK_BYTES(fields.suites, "\x13\x01\x13\x03\x13\x02", 6);
		CHECK(fields.compression_size == 1U);
		CHECK(fields.compression[0] == 0U);
	} else {
		CHECK(fields.suite == suite);
		CHECK(fields.compression_method == 0U);
	}

	/* Checks that every extension was read. */
	CHECK(fields.extension_count == expected_count);
	for (index = 0; index < expected_count && index < fields.extension_count; index++) {
		CHECK(fields.extensions[index].type == expected[index].type);
		CHECK(fields.extensions[index].size == expected[index].size);
	}

	/* The writer rebuilds the same bytes. */
	status = write_hello(&fields, client, rewritten, sizeof(rewritten), &rewritten_size);
	CHECK(status == 0);
	CHECK(rewritten_size == size);
	CHECK_BYTES(rewritten, message, size);

	/* Every shorter prefix of the message is refused with an error and no read outside the block. */
	free(block);
	for (index = 0; index < size; index++) {
		block = exact_copy(message, index);
		status = parse_hello(block, index, client, &fields);
		CHECK(status != 0);
		free(block);
	}

	/* The message cut short with its length field made to match: a field inside is then cut off, and the reader refuses it. */
	for (index = 4; index < size; index++) {
		block = exact_copy(message, index);
		block[1] = 0;
		block[2] = (uint8_t)(((index - 4U) >> 8) & 0xffU);
		block[3] = (uint8_t)((index - 4U) & 0xffU);
		status = parse_hello(block, index, client, &fields);
		CHECK(status != 0);
		free(block);
	}

	/* The writer given too little room reports ENOBUFS for every smaller room. */
	block = exact_copy(message, size);
	status = parse_hello(block, size, client, &fields);
	CHECK(status == 0);
	for (index = 0; index < size; index += 7U) {
		status = write_hello(&fields, client, rewritten, index, &rewritten_size);
		CHECK(status == ENOBUFS);
	}

	/* Frees the block. */
	free(block);
}

/* Parses and rewrites the three hellos of RFC 8448. */
static void
test_rfc8448(void)
{
	/* The extensions of the ClientHello in the order of the RFC's trace. */
	static const struct expected_extension client_extensions[] = {
		{ 0x0000U, 11U }, { 0xff01U, 1U }, { 0x000aU, 20U }, { 0x0023U, 0U }, { 0x0033U, 38U },
		{ 0x002bU, 3U }, { 0x000dU, 32U }, { 0x002dU, 2U }, { 0x001cU, 2U }
	};
	static const struct expected_extension server_extensions[] = {
		{ 0x0033U, 36U }, { 0x002bU, 2U }
	};
	static const struct expected_extension retry_extensions[] = {
		{ 0x0033U, 2U }, { 0x002cU, 116U }, { 0x002bU, 2U }
	};

	/* Checks the three messages. */
	check_hello(client_hello_hex, 196U, 1, 1U, 0U, client_extensions, 9U);
	check_hello(server_hello_hex, 90U, 0, 2U, 0x1301U, server_extensions, 2U);
	check_hello(retry_request_hex, 176U, 0, 2U, 0x1301U, retry_extensions, 3U);
}

/* A model of the reader's rules, written from the header's comments, to compare with on random input. */
struct reader_model {
	const uint8_t *data;
	size_t size;
	size_t position;
	int failed;
};

/* The model's integer of width bytes. */
static uint32_t model_integer(struct reader_model *model, size_t width);

static void random_reader_run(struct sslc_reader *reader, struct reader_model *model, int depth, int steps);

/* The model's integer of width bytes. */
static uint32_t
model_integer(
	struct reader_model *model,
	size_t width)
{
	uint32_t value;
	size_t index;

	/* A failed stream reads zeros; a short one fails. */
	if (model->failed)
		return 0;
	if (model->size - model->position < width) {
		model->failed = 1;
		return 0;
	}

	/* Reads the value, most significant byte first. */
	value = 0;
	for (index = 0; index < width; index++)
		value = value * 256U + model->data[model->position + index];
	model->position += width;
	return value;
}

/*
 * Runs random operations on a reader and its model together, recursing
 * into vectors, and checks that they agree after every step.
 */
static void
random_reader_run(
	struct sslc_reader *reader,
	struct reader_model *model,
	int depth,
	int steps)
{
	struct sslc_reader inner;
	struct reader_model inner_model;
	const uint8_t *view;
	uint32_t got;
	uint32_t want;
	size_t amount;
	size_t min;
	size_t max;
	unsigned prefix;
	int step;
	int operation;
	int status;
	int expect_ok;
	int before;
	unsigned draw;

	/* A few operations of a random kind. */
	for (step = 0; step < steps; step++) {
		operation = (int)random_below(8U);
		before = model->failed;
		if (operation < 4) {
			amount = (size_t)operation + 1U;
			want = model_integer(model, amount);
			got = read_integer(reader, amount);
			CHECK(got == want);
		} else if (operation == 4) {
			amount = random_below(12U);
			draw = random_below(20U);
			if (draw == 0U)
				amount = (size_t)-1 - random_below(3U);
			if (!model->failed && amount <= model->size - model->position) {
				view = sslc_reader_bytes(reader, amount);
				CHECK(model->data == NULL || view == model->data + model->position);
				model->position += amount;
			} else {
				view = sslc_reader_bytes(reader, amount);
				CHECK(view == NULL);
				model->failed = 1;
			}
		} else {
			prefix = 1U + random_below(3U);
			min = random_below(6U);
			max = min + random_below(40U);
			want = model_integer(model, prefix);
			expect_ok = !model->failed && want >= min && want <= max && want <= model->size - model->position;
			status = sslc_reader_vector(reader, prefix, min, max, &inner);
			CHECK((status == 0) == expect_ok);
			if (expect_ok) {
				inner_model.data = model->data + model->position;
				inner_model.size = want;
				inner_model.position = 0;
				inner_model.failed = 0;
				model->position += want;
				CHECK(inner.size == want);
				CHECK(inner.error == 0);
				if (depth > 0)
					random_reader_run(&inner, &inner_model, depth - 1, 4);
				CHECK(inner.position <= inner.size);
			} else {
				model->failed = 1;
				CHECK(inner.error != 0);
				CHECK(sslc_reader_left(&inner) == 0U);
			}
		}

		/* The reader and the model agree on the state; a failure never goes away. */
		CHECK((reader->error != 0) == (model->failed != 0));
		CHECK(reader->position <= reader->size);
		if (before)
			CHECK(model->failed);
		if (model->failed) {
			CHECK(sslc_reader_left(reader) == 0U);
			CHECK(sslc_reader_finish(reader) == EINVAL);
		} else {
			CHECK(sslc_reader_left(reader) == model->size - model->position);
			CHECK((sslc_reader_finish(reader) == 0) == (model->position == model->size));
		}
	}
}

/* Runs the reader over random bytes of random sizes against the model. */
static void
test_random_reader(void)
{
	struct sslc_reader reader;
	struct reader_model model;
	uint8_t source[80];
	uint8_t *block;
	uint8_t *view;
	size_t size;
	size_t index;
	unsigned draw;
	int round;

	/* Many inputs, each with its own run of operations. */
	for (round = 0; round < 6000; round++) {
		size = random_below(70U);
		for (index = 0; index < size; index++)
			source[index] = (uint8_t)(random_next() >> 24);

		/* Some inputs are made of small bytes so that the vector lengths are plausible. */
		draw = random_below(2U);
		if (draw == 0U) {
			for (index = 0; index < size; index++)
				source[index] = (uint8_t)(source[index] % 9U);
		}

		/* Makes an exact-size copy and starts the reader and its model over it. */
		block = exact_copy(source, size);
		view = NULL;
		if (size > 0U)
			view = block;
		sslc_reader_init(&reader, view, size);
		model.data = view;
		model.size = size;
		model.position = 0;
		model.failed = 0;
		random_reader_run(&reader, &model, 3, 8);
		free(block);
	}
}

/* A model of the writer's rules. */
struct writer_model {
	uint8_t bytes[160];
	size_t room;
	size_t used;
	size_t open[SSLC_WRITER_DEPTH];
	unsigned prefix[SSLC_WRITER_DEPTH];
	unsigned depth;
	int error;
};

/* The model's write of count bytes of the pattern: the first error is kept, a failed stream writes nothing. */
static void model_write(struct writer_model *model, const uint8_t *bytes, size_t count);

/* The model's begin of a vector. */
static void model_begin(struct writer_model *model, unsigned prefix);

/* The model's end of the innermost vector. */
static void model_end(struct writer_model *model);

/* The model's write of count bytes of the pattern: the first error is kept, a failed stream writes nothing. */
static void
model_write(
	struct writer_model *model,
	const uint8_t *bytes,
	size_t count)
{
	/* Nothing more once failed; a write past the room fails whole. */
	if (model->error != 0)
		return;
	if (count > model->room - model->used) {
		model->error = ENOBUFS;
		return;
	}

	/* Copies the bytes in, if there are any. */
	if (count > 0U)
		memcpy(model->bytes + model->used, bytes, count);
	model->used += count;
}

/* The model's begin of a vector. */
static void
model_begin(
	struct writer_model *model,
	unsigned prefix)
{
	static const uint8_t zeros[3] = { 0, 0, 0 };

	/* The checks in the order of the header: the width, the depth, the room. */
	if (model->error != 0)
		return;
	if (prefix < 1U || prefix > 3U) {
		model->error = EINVAL;
		return;
	}

	/* Refuses a vector nested deeper than the writer allows. */
	if (model->depth >= SSLC_WRITER_DEPTH) {
		model->error = EINVAL;
		return;
	}

	/* Refuses a prefix that does not fit in the room. */
	if (prefix > model->room - model->used) {
		model->error = ENOBUFS;
		return;
	}

	/* Opens the vector. */
	model->open[model->depth] = model->used;
	model->prefix[model->depth] = prefix;
	model->depth++;
	model_write(model, zeros, prefix);
}

/* The model's end of the innermost vector. */
static void
model_end(
	struct writer_model *model)
{
	size_t length;
	size_t start;
	unsigned width;
	unsigned index;

	/* An end with nothing open, or a length that the prefix cannot hold, is EINVAL. */
	if (model->error != 0)
		return;
	if (model->depth == 0U) {
		model->error = EINVAL;
		return;
	}

	/* Closes the innermost vector and fills its length. */
	model->depth--;
	start = model->open[model->depth];
	width = model->prefix[model->depth];
	length = model->used - start - width;
	if ((width == 1U && length > 255U) || (width == 2U && length > 65535U) || (width == 3U && length > 16777215U)) {
		model->error = EINVAL;
		return;
	}

	/* Writes the length big-endian into the prefix. */
	for (index = 0; index < width; index++)
		model->bytes[start + index] = (uint8_t)((length >> (8U * (width - 1U - index))) & 0xffU);
}

/* Runs random writer operations on a guarded buffer and the model, and compares the bytes and the status. */
static void
test_random_writer(void)
{
	struct sslc_writer writer;
	struct writer_model model;
	uint8_t buffer[8 + 120 + 8];
	uint8_t pattern[40];
	uint8_t one;
	size_t room;
	size_t count;
	size_t index;
	uint32_t value;
	unsigned prefix;
	unsigned draw;
	int round;
	int step;
	int operation;
	int expected_status;

	/* Many runs, each with a random room. */
	for (round = 0; round < 8000; round++) {
		room = random_below(60U);
		guarded_init(buffer, room);
		sslc_writer_init(&writer, buffer + 8, room);
		memset(&model, 0, sizeof(model));
		model.room = room;
		for (step = 0; step < 24; step++) {
			operation = (int)random_below(9U);
			value = random_next();
			if (operation == 0) {
				sslc_writer_u8(&writer, value);
				one = (uint8_t)(value & 0xffU);
				model_write(&model, &one, 1);
			} else if (operation == 1) {
				sslc_writer_u16(&writer, value);
				pattern[0] = (uint8_t)((value >> 8) & 0xffU);
				pattern[1] = (uint8_t)(value & 0xffU);
				model_write(&model, pattern, 2);
			} else if (operation == 2) {
				sslc_writer_u24(&writer, value);
				pattern[0] = (uint8_t)((value >> 16) & 0xffU);
				pattern[1] = (uint8_t)((value >> 8) & 0xffU);
				pattern[2] = (uint8_t)(value & 0xffU);
				model_write(&model, pattern, 3);
			} else if (operation == 3 || operation == 4) {
				count = random_below(14U);
				for (index = 0; index < count; index++)
					pattern[index] = (uint8_t)(random_next() >> 24);
				sslc_writer_bytes(&writer, pattern, count);
				model_write(&model, pattern, count);
			} else if (operation == 5 || operation == 6) {
				draw = random_below(20U);
				if (draw == 0U)
					prefix = random_below(6U);
				else
					prefix = 1U + random_below(3U);
				sslc_writer_begin(&writer, prefix);
				model_begin(&model, prefix);
			} else {
				sslc_writer_end(&writer);
				model_end(&model);
			}

			/* The writer and the model agree after every step. */
			CHECK(writer.used == model.used);
			CHECK((writer.error == 0) == (model.error == 0));
			CHECK(writer.error == model.error);
			CHECK(writer.depth == model.depth);
			CHECK(writer.used <= writer.room);
			CHECK(guarded_intact(buffer, room, writer.used));
		}

		/* The final status and the finished bytes (the guard outside the room is untouched). */
		expected_status = model.error;
		if (expected_status == 0 && model.depth != 0U)
			expected_status = EINVAL;
		CHECK(sslc_writer_finish(&writer) == expected_status);
		CHECK_BYTES(buffer + 8, model.bytes, writer.used);
		CHECK(guarded_intact(buffer, room, room));
	}
}

/* Writes random trees of vectors and integers, reads them back with the reader, and compares. */
static void
test_round_trip(void)
{
	struct sslc_writer writer;
	struct sslc_reader reader;
	struct sslc_reader inner;
	struct sslc_reader deeper;
	uint8_t buffer[512];
	uint8_t chunk[24];
	uint8_t *block;
	const uint8_t *view;
	size_t round;
	size_t count;
	size_t index;
	size_t blocks;
	size_t length;
	size_t outer_length;
	uint32_t a;
	uint32_t b;
	uint32_t c;
	unsigned prefix;

	/* A message of an integer of each width, a vector holding a vector and bytes, and a trailing integer. */
	for (round = 0; round < 3000U; round++) {
		a = random_next();
		b = random_next();
		c = random_next();
		prefix = 1U + random_below(3U);
		blocks = random_below(4U);
		count = random_below(20U);
		for (index = 0; index < sizeof(chunk); index++)
			chunk[index] = (uint8_t)(random_next() >> 24);

		/* Writes: u8 a, u16 b, u24 c, vector{ vector{ chunk bytes } x blocks, u32 a }, u8 b. */
		sslc_writer_init(&writer, buffer, sizeof(buffer));
		sslc_writer_u8(&writer, a);
		sslc_writer_u16(&writer, b);
		sslc_writer_u24(&writer, c);
		sslc_writer_begin(&writer, prefix);
		for (index = 0; index < blocks; index++) {
			sslc_writer_begin(&writer, 1);
			sslc_writer_bytes(&writer, chunk, count);
			sslc_writer_end(&writer);
		}

		/* Closes the two vectors and writes the rest. */
		sslc_writer_u16(&writer, a);
		sslc_writer_end(&writer);
		sslc_writer_u8(&writer, b);
		CHECK(sslc_writer_finish(&writer) == 0);

		/* The length of the outer vector, worked out by hand: blocks of (1 + count) bytes and a 2-byte integer. */
		outer_length = blocks * (1U + count) + 2U;

		/* Reads it back from a block of exactly the written size. */
		block = exact_copy(buffer, writer.used);
		sslc_reader_init(&reader, block, writer.used);
		CHECK(sslc_reader_u8(&reader) == (a & 0xffU));
		CHECK(sslc_reader_u16(&reader) == (b & 0xffffU));
		CHECK(sslc_reader_u24(&reader) == (c & 0xffffffU));
		CHECK(sslc_reader_vector(&reader, prefix, 0, 0xffffffU, &inner) == 0);
		CHECK(sslc_reader_left(&inner) == outer_length);
		for (index = 0; index < blocks; index++) {
			CHECK(sslc_reader_vector(&inner, 1, 0, 255, &deeper) == 0);
			length = sslc_reader_left(&deeper);
			CHECK(length == count);
			view = sslc_reader_bytes(&deeper, length);
			CHECK(count == 0U || view != NULL);
			if (count > 0U && view != NULL)
				CHECK_BYTES(view, chunk, count);
			CHECK(sslc_reader_finish(&deeper) == 0);
		}

		/* Checks the values read back. */
		CHECK(sslc_reader_u16(&inner) == (a & 0xffffU));
		CHECK(sslc_reader_finish(&inner) == 0);
		CHECK(sslc_reader_u8(&reader) == (b & 0xffU));
		CHECK(sslc_reader_finish(&reader) == 0);
		free(block);
	}
}

/* Checks that a writer rejects, and a reader refuses, structure that does not fit a given prefix width. */
static void
test_prefix_fit(void)
{
	struct sslc_writer writer;
	struct sslc_reader reader;
	struct sslc_reader inner;
	uint8_t buffer[400];
	uint8_t *block;
	size_t count;

	/* A vector of 255 bytes writes with a 1-byte prefix and reads back; with 256 the writer fails. */
	sslc_writer_init(&writer, buffer, sizeof(buffer));
	sslc_writer_begin(&writer, 1);
	for (count = 0; count < 255U; count++)
		sslc_writer_u8(&writer, (uint32_t)count);
	sslc_writer_end(&writer);
	CHECK(sslc_writer_finish(&writer) == 0);
	CHECK(buffer[0] == 255U);
	block = exact_copy(buffer, writer.used);
	sslc_reader_init(&reader, block, writer.used);
	CHECK(sslc_reader_vector(&reader, 1, 255, 255, &inner) == 0);
	CHECK(sslc_reader_left(&inner) == 255U);
	CHECK(sslc_reader_finish(&reader) == 0);
	free(block);
	sslc_writer_init(&writer, buffer, sizeof(buffer));
	sslc_writer_begin(&writer, 1);
	for (count = 0; count < 256U; count++)
		sslc_writer_u8(&writer, (uint32_t)count);
	sslc_writer_end(&writer);
	CHECK(sslc_writer_finish(&writer) == EINVAL);
}

int
main(void)
{
	/* Each group of checks. */
	test_integers();
	test_short_reads();
	test_reader_bytes();
	test_reader_vector();
	test_reader_finish();
	test_writer_integers();
	test_writer_bytes();
	test_writer_vectors();
	test_writer_depth();
	test_rfc8448();
	test_random_reader();
	test_random_writer();
	test_round_trip();
	test_prefix_fit();

	/* The last line run-unit.sh reads. */
	return check_finish("test-buf");
}
