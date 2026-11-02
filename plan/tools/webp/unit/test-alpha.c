/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The tests of alpha.c: the inverse filters against hand-computed values,
 * and whole ALPH payloads built here from RFC 9649 2.7.1.2: the header
 * byte, then raw values or a lossless image-stream written with the
 * lbitwriter (flat prefix codes, so any green value can be written), the
 * values first passed through the forward filter written here from the
 * RFC's formulas.
 */

#include "check.h"
#include "lbitwriter.h"
#include "alpha.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The most pixels of a test image. */
#define TEST_PIXELS_MAX		4096U

/* The stream a test writes. */
static struct lbitwriter stream;

/* The generator of the random values of the tests. */
static unsigned long test_state = 0x1b873593UL;

/* The order the code length code's lengths are written in (RFC 9649 3.7.2.1.2). */
static const uint8_t test_order[12] = { 17, 18, 0, 1, 2, 3, 4, 5, 16, 6, 7, 8 };

static void test_unfilter_known(void);
static void test_unfilter_edges(void);
static void test_unfilter_invalid(void);
static void test_raw(void);
static void test_header_bits(void);
static void test_bad_arguments(void);
static void test_lossless(void);
static void test_lossless_indexing(void);
static void test_lossless_truncated(void);
static void test_roundtrip_filters(void);
static void test_random_payloads(void);
static unsigned long next_random(void);
static unsigned predictor(const uint8_t *values, unsigned width, unsigned x, unsigned y, unsigned method);
static void forward_filter(const uint8_t *values, unsigned width, unsigned height, unsigned method, uint8_t *stored);
static void put_normal_code(struct lbitwriter *writer, int green);
static void put_image(struct lbitwriter *writer, const uint32_t *pixels, unsigned width, unsigned height, int role);
static size_t lossless_payload(unsigned header, const uint8_t *stored, unsigned width, unsigned height, uint8_t *payload);
static int same(const uint8_t *first, const uint8_t *second, size_t count);

int
main(void)
{
	/* Runs every group of checks, then prints the summary line. */
	test_unfilter_known();
	test_unfilter_edges();
	test_unfilter_invalid();
	test_raw();
	test_header_bits();
	test_bad_arguments();
	test_lossless();
	test_lossless_indexing();
	test_lossless_truncated();
	test_roundtrip_filters();
	test_random_payloads();
	return check_finish("alpha");
}

/* Gives the next 32-bit pseudo-random value. */
static unsigned long
next_random(void)
{
	/* One step of the shared generator. */
	return check_random(&test_state);
}

/*
 * Gives the predictor of the value at (x, y) as RFC 9649 2.7.1.2 says it
 * (A the left value, B the one above, C the one above-left), from values:
 * the original values, which the decoder's restored ones equal.
 */
static unsigned
predictor(
	const uint8_t *values,
	unsigned width,
	unsigned x,
	unsigned y,
	unsigned method)
{
	int a;
	int b;
	int c;
	int sum;

	/* Method 0 predicts 0; so does every method at the first pixel. */
	if (method == 0U)
		return 0U;
	if (x == 0U && y == 0U)
		return 0U;

	/* The top row is predicted from the left, the left column from above (the RFC's special cases). */
	if (y == 0U)
		return values[x - 1U];
	if (x == 0U)
		return values[(size_t)(y - 1U) * width];

	/* Inside: A, B or the clipped A + B - C. */
	a = values[(size_t)y * width + x - 1U];
	b = values[(size_t)(y - 1U) * width + x];
	c = values[(size_t)(y - 1U) * width + x - 1U];
	if (method == 1U)
		return (unsigned)a;
	if (method == 2U)
		return (unsigned)b;
	sum = a + b - c;
	if (sum < 0)
		sum = 0;
	if (sum > 255)
		sum = 255;
	return (unsigned)sum;
}

/* Writes into stored the values as an encoder stores them: each minus its predictor, modulo 256. */
static void
forward_filter(
	const uint8_t *values,
	unsigned width,
	unsigned height,
	unsigned method,
	uint8_t *stored)
{
	unsigned x;
	unsigned y;
	unsigned prediction;

	/* Every value minus what the decoder will predict for it. */
	for (y = 0U; y < height; y++) {
		for (x = 0U; x < width; x++) {
			prediction = predictor(values, width, x, y, method);
			stored[(size_t)y * width + x] = (uint8_t)((values[(size_t)y * width + x] + 256U - prediction) & 0xffU);
		}
	}
}

/* Reports whether count bytes of two arrays are equal. */
static int
same(
	const uint8_t *first,
	const uint8_t *second,
	size_t count)
{
	/* A byte comparison. */
	if (memcmp(first, second, count) == 0)
		return 1;
	return 0;
}

/*
 * Writes one flat prefix code as a normal code (RFC 9649 3.7.2): every
 * literal symbol 0..255 a codeword of 8 bits.  The green code's alphabet
 * is larger (it has the length-and-distance prefix symbols after the 256
 * literals), so its max_symbol field limits it to the first 256 symbols.
 */
static void
put_normal_code(
	struct lbitwriter *writer,
	int green)
{
	unsigned index;

	/* Not a simple code, 12 code length code lengths follow (4 + 8). */
	lbitwriter_put(writer, 0U, 1U);
	lbitwriter_put(writer, 8U, 4U);
	for (index = 0; index < 12U; index++) {
		if (test_order[index] == 0U || test_order[index] == 8U)
			lbitwriter_put(writer, 1U, 3U);
		else
			lbitwriter_put(writer, 0U, 3U);
	}

	/* max_symbol: all the alphabet for a 256-symbol code; 256 (2 + 254, in 2 + 2 * 4 = 10 bits) for green. */
	if (green) {
		lbitwriter_put(writer, 1U, 1U);
		lbitwriter_put(writer, 4U, 3U);
		lbitwriter_put(writer, 254U, 10U);
	} else {
		lbitwriter_put(writer, 0U, 1U);
	}

	/* 256 times the code length 8 (symbol 8 of the code length code is the codeword 1). */
	for (index = 0; index < 256U; index++)
		lbitwriter_put_code(writer, 1U, 1U);
}

/*
 * Writes an entropy-coded image of width by height pixels: no colour cache,
 * no meta prefix code (role 1 only has the bit), one group of flat codes,
 * then every pixel as four 8-bit codewords in the order green, red, blue,
 * alpha.
 */
static void
put_image(
	struct lbitwriter *writer,
	const uint32_t *pixels,
	unsigned width,
	unsigned height,
	int role)
{
	size_t index;
	size_t count;

	/* The colour cache bit (0) and, for the main image, the meta prefix bit (0). */
	lbitwriter_put(writer, 0U, 1U);
	if (role == 1)
		lbitwriter_put(writer, 0U, 1U);

	/* The group: green, red, blue, alpha as flat codes, the distance code as a simple code of symbol 0. */
	put_normal_code(writer, 1);
	put_normal_code(writer, 0);
	put_normal_code(writer, 0);
	put_normal_code(writer, 0);
	lbitwriter_put(writer, 1U, 1U);
	lbitwriter_put(writer, 0U, 1U);
	lbitwriter_put(writer, 0U, 1U);
	lbitwriter_put(writer, 0U, 1U);

	/* The pixels: with a flat code the codeword of a symbol is its value. */
	count = (size_t)width * height;
	for (index = 0; index < count; index++) {
		lbitwriter_put_code(writer, (pixels[index] >> 8) & 0xffU, 8U);
		lbitwriter_put_code(writer, (pixels[index] >> 16) & 0xffU, 8U);
		lbitwriter_put_code(writer, pixels[index] & 0xffU, 8U);
		lbitwriter_put_code(writer, (pixels[index] >> 24) & 0xffU, 8U);
	}
}

/*
 * Builds an ALPH payload of compression method 1: the header byte, then a
 * lossless image-stream with no transform whose green values are stored and
 * whose other channels are random.  Gives the payload's size.
 */
static size_t
lossless_payload(
	unsigned header,
	const uint8_t *stored,
	unsigned width,
	unsigned height,
	uint8_t *payload)
{
	static uint32_t pixels[TEST_PIXELS_MAX];
	size_t index;
	size_t count;
	size_t bytes;

	/* The pixels carry the stored value in green and noise elsewhere. */
	count = (size_t)width * height;
	for (index = 0; index < count; index++) {
		pixels[index] = ((uint32_t)stored[index] << 8) | ((uint32_t)(next_random() & 0xffUL) << 16) |
		    (uint32_t)(next_random() & 0xffUL) | ((uint32_t)(next_random() & 0xffUL) << 24);
	}

	/* The stream: transform list ended at once, then the image. */
	lbitwriter_init(&stream);
	lbitwriter_put(&stream, 0U, 1U);
	put_image(&stream, pixels, width, height, 1);

	/* The header byte then the stream's bytes. */
	bytes = lbitwriter_size(&stream);
	payload[0] = (uint8_t)header;
	memcpy(payload + 1, stream.bytes, bytes);
	return bytes + 1U;
}

/* Checks the four inverse filters on 3x3 stored values, with the results worked out by hand from Figure 11. */
static void
test_unfilter_known(void)
{
	static const uint8_t stored[9] = { 1, 2, 3, 4, 5, 6, 7, 8, 9 };
	static const uint8_t horizontal[9] = { 1, 3, 6, 5, 10, 16, 12, 20, 29 };
	static const uint8_t vertical[9] = { 1, 3, 6, 5, 8, 12, 12, 16, 21 };
	static const uint8_t gradient[9] = { 1, 3, 6, 5, 12, 21, 12, 27, 45 };
	static const uint8_t low_clip_in[4] = { 250, 10, 20, 100 };
	static const uint8_t low_clip_out[4] = { 250, 4, 14, 100 };
	static const uint8_t high_clip_in[4] = { 200, 50, 10, 100 };
	static const uint8_t high_clip_out[4] = { 200, 250, 210, 99 };
	uint8_t work[9];

	/* Method 0 leaves the values alone. */
	memcpy(work, stored, 9U);
	webpc_alpha_unfilter(work, 3U, 3U, WEBPC_ALPHA_FILTER_NONE);
	CHECK(same(work, stored, 9U));

	/* Horizontal: each value is the one on its left plus the stored one; the left column adds the value above. */
	memcpy(work, stored, 9U);
	webpc_alpha_unfilter(work, 3U, 3U, WEBPC_ALPHA_FILTER_HORIZONTAL);
	CHECK(same(work, horizontal, 9U));

	/* Vertical: each value is the one above plus the stored one; the top row adds the value on the left. */
	memcpy(work, stored, 9U);
	webpc_alpha_unfilter(work, 3U, 3U, WEBPC_ALPHA_FILTER_VERTICAL);
	CHECK(same(work, vertical, 9U));

	/* Gradient: A + B - C inside, the edges as the other two. */
	memcpy(work, stored, 9U);
	webpc_alpha_unfilter(work, 3U, 3U, WEBPC_ALPHA_FILTER_GRADIENT);
	CHECK(same(work, gradient, 9U));

	/* Gradient clips A + B - C below 0 (and wraps 250 + 10 and 250 + 20 modulo 256). */
	memcpy(work, low_clip_in, 4U);
	webpc_alpha_unfilter(work, 2U, 2U, WEBPC_ALPHA_FILTER_GRADIENT);
	CHECK(same(work, low_clip_out, 4U));

	/* Gradient clips A + B - C above 255 (210 + 250 - 200 is 260) and wraps the sum with the stored value. */
	memcpy(work, high_clip_in, 4U);
	webpc_alpha_unfilter(work, 2U, 2U, WEBPC_ALPHA_FILTER_GRADIENT);
	CHECK(same(work, high_clip_out, 4U));
}

/* Checks the one-pixel-wide and one-pixel-high images, where every method uses only the edge rules. */
static void
test_unfilter_edges(void)
{
	static const uint8_t stored[4] = { 5, 6, 7, 8 };
	static const uint8_t summed[4] = { 5, 11, 18, 26 };
	static const uint8_t wrap_in[4] = { 250, 10, 10, 10 };
	static const uint8_t wrap_out[4] = { 250, 4, 14, 24 };
	uint8_t work[4];
	unsigned method;

	/* The first value is stored as is by every method (predicted 0). */
	for (method = 1U; method <= 3U; method++) {
		memcpy(work, stored, 4U);
		webpc_alpha_unfilter(work, 1U, 1U, method);
		CHECK(work[0] == 5U);
	}

	/* 1x4 (a column): methods 1, 2 and 3 all predict from the value above. */
	for (method = 1U; method <= 3U; method++) {
		memcpy(work, stored, 4U);
		webpc_alpha_unfilter(work, 1U, 4U, method);
		CHECK(same(work, summed, 4U));
	}

	/* 4x1 (a row): methods 1, 2 and 3 all predict from the value on the left. */
	for (method = 1U; method <= 3U; method++) {
		memcpy(work, stored, 4U);
		webpc_alpha_unfilter(work, 4U, 1U, method);
		CHECK(same(work, summed, 4U));
	}

	/* The sum wraps modulo 256: 250 + 10 is 4. */
	memcpy(work, wrap_in, 4U);
	webpc_alpha_unfilter(work, 4U, 1U, WEBPC_ALPHA_FILTER_HORIZONTAL);
	CHECK(same(work, wrap_out, 4U));
}

/* Checks the inputs the unfilter leaves alone: an unknown method, an empty image, no buffer. */
static void
test_unfilter_invalid(void)
{
	static const uint8_t stored[4] = { 5, 6, 7, 8 };
	uint8_t work[4];

	/* A method above 3 changes nothing. */
	memcpy(work, stored, 4U);
	webpc_alpha_unfilter(work, 2U, 2U, 4U);
	CHECK(same(work, stored, 4U));

	/* A zero side changes nothing. */
	memcpy(work, stored, 4U);
	webpc_alpha_unfilter(work, 0U, 2U, 1U);
	CHECK(same(work, stored, 4U));
	webpc_alpha_unfilter(work, 2U, 0U, 1U);
	CHECK(same(work, stored, 4U));

	/* No buffer does not fault. */
	webpc_alpha_unfilter(NULL, 2U, 2U, 1U);
	CHECK(1);
}

/* Checks the raw method: exact size, one byte short, extra bytes, and the smallest payloads. */
static void
test_raw(void)
{
	static const uint8_t payload[8] = { 0x00, 10, 20, 30, 40, 50, 60, 70 };
	uint8_t *alpha;
	uint8_t guard[1];
	uint8_t one[2];

	/* A 2x3 image takes 6 raw values after the header byte (an exact allocation, so ASan sees an overrun). */
	alpha = (uint8_t *)malloc(6U);
	CHECK(alpha != NULL);
	if (alpha == NULL)
		return;
	CHECK(webpc_alpha_decode(payload, 7U, 2U, 3U, alpha) == 0);
	CHECK(alpha[0] == 10U && alpha[1] == 20U && alpha[2] == 30U && alpha[3] == 40U && alpha[4] == 50U &&
	    alpha[5] == 60U);

	/* One byte short is EINVAL. */
	CHECK(webpc_alpha_decode(payload, 6U, 2U, 3U, alpha) == EINVAL);

	/* A longer payload: the rest is ignored. */
	CHECK(webpc_alpha_decode(payload, 8U, 2U, 3U, alpha) == 0);
	CHECK(alpha[5] == 60U);
	free(alpha);

	/* A 1x1 image is the smallest: the header byte and one value. */
	one[0] = 0x00U;
	one[1] = 77U;
	guard[0] = 0U;
	CHECK(webpc_alpha_decode(one, 2U, 1U, 1U, guard) == 0);
	CHECK(guard[0] == 77U);

	/* The header byte alone (size 1) and nothing (size 0) are EINVAL. */
	CHECK(webpc_alpha_decode(one, 1U, 1U, 1U, guard) == EINVAL);
	CHECK(webpc_alpha_decode(one, 0U, 1U, 1U, guard) == EINVAL);
}

/* Checks the header byte's fields: Rsv and P ignored, F in bits 2..3, C in bits 0..1. */
static void
test_header_bits(void)
{
	static const uint8_t values[6] = { 1, 2, 3, 4, 5, 6 };
	static const uint8_t horizontal[6] = { 1, 3, 6, 5, 10, 16 };
	static const uint8_t vertical[6] = { 1, 3, 6, 5, 8, 12 };
	static const uint8_t gradient[6] = { 1, 3, 6, 5, 12, 21 };
	const uint8_t *expected[4];
	uint8_t payload[7];
	uint8_t alpha[6];
	unsigned upper;
	unsigned filter;
	unsigned compression;

	/* The expected result of each filter method on the 3x2 stored values. */
	expected[0] = values;
	expected[1] = horizontal;
	expected[2] = vertical;
	expected[3] = gradient;
	memcpy(payload + 1, values, 6U);

	/* Every Rsv and P combination (bits 4..7) and every F gives the same values as with them 0. */
	for (upper = 0U; upper < 16U; upper++) {
		for (filter = 0U; filter < 4U; filter++) {
			payload[0] = (uint8_t)((upper << 4) | (filter << 2));
			memset(alpha, 0, 6U);
			CHECK(webpc_alpha_decode(payload, 7U, 3U, 2U, alpha) == 0);
			CHECK(same(alpha, expected[filter], 6U));
		}
	}

	/* The F field is the two bits above C: 0x04 is horizontal, 0x08 vertical, 0x0c gradient. */
	payload[0] = 0x04U;
	CHECK(webpc_alpha_decode(payload, 7U, 3U, 2U, alpha) == 0);
	CHECK(same(alpha, horizontal, 6U));
	payload[0] = 0x08U;
	CHECK(webpc_alpha_decode(payload, 7U, 3U, 2U, alpha) == 0);
	CHECK(same(alpha, vertical, 6U));
	payload[0] = 0x0cU;
	CHECK(webpc_alpha_decode(payload, 7U, 3U, 2U, alpha) == 0);
	CHECK(same(alpha, gradient, 6U));

	/* Compression methods 2 and 3 are EINVAL, whatever the other fields. */
	for (upper = 0U; upper < 16U; upper++) {
		for (filter = 0U; filter < 4U; filter++) {
			for (compression = 2U; compression <= 3U; compression++) {
				payload[0] = (uint8_t)((upper << 4) | (filter << 2) | compression);
				CHECK(webpc_alpha_decode(payload, 7U, 3U, 2U, alpha) == EINVAL);
			}
		}
	}
}

/* Checks the arguments the decoder refuses before reading anything. */
static void
test_bad_arguments(void)
{
	static const uint8_t payload[3] = { 0x00, 1, 2 };
	static const uint8_t lossless_only[1] = { 0x01 };
	uint8_t alpha[4];
	uint8_t *wide;
	uint8_t *wide_alpha;

	/* No payload or no output. */
	CHECK(webpc_alpha_decode(NULL, 3U, 1U, 2U, alpha) == EINVAL);
	CHECK(webpc_alpha_decode(payload, 3U, 1U, 2U, NULL) == EINVAL);

	/* A side of 0, or above the lossy limit 16383, with no huge allocation made. */
	CHECK(webpc_alpha_decode(payload, 3U, 0U, 2U, alpha) == EINVAL);
	CHECK(webpc_alpha_decode(payload, 3U, 1U, 0U, alpha) == EINVAL);
	CHECK(webpc_alpha_decode(payload, 3U, 16384U, 1U, alpha) == EINVAL);
	CHECK(webpc_alpha_decode(payload, 3U, 1U, 16384U, alpha) == EINVAL);
	CHECK(webpc_alpha_decode(payload, 3U, 0xffffffffU, 0xffffffffU, alpha) == EINVAL);

	/* A side of 16384 is refused even when the payload holds all its values (the lossless format would allow it). */
	wide = (uint8_t *)malloc(16385U);
	wide_alpha = (uint8_t *)malloc(16384U);
	CHECK(wide != NULL && wide_alpha != NULL);
	if (wide != NULL && wide_alpha != NULL) {
		memset(wide, 0, 16385U);
		CHECK(webpc_alpha_decode(wide, 16385U, 16384U, 1U, wide_alpha) == EINVAL);
		CHECK(webpc_alpha_decode(wide, 16385U, 1U, 16384U, wide_alpha) == EINVAL);
		CHECK(webpc_alpha_decode(wide, 16385U, 16383U, 1U, wide_alpha) == 0);
	}
	free(wide);
	free(wide_alpha);

	/* A lossless header byte alone is EINVAL as well (not the ENOBUFS an empty stream would give). */
	CHECK(webpc_alpha_decode(lossless_only, 1U, 1U, 1U, alpha) == EINVAL);
}

/* Checks the lossless method: streams written with flat codes, with and without a filter, in several sizes. */
static void
test_lossless(void)
{
	static const unsigned sizes[6][2] = { { 1, 1 }, { 3, 2 }, { 1, 9 }, { 9, 1 }, { 17, 13 }, { 64, 40 } };
	static uint8_t values[TEST_PIXELS_MAX];
	static uint8_t stored[TEST_PIXELS_MAX];
	static uint8_t payload[LBITWRITER_BYTES + 1U];
	static uint8_t alpha[TEST_PIXELS_MAX];
	size_t size;
	size_t index;
	unsigned shape;
	unsigned method;
	unsigned width;
	unsigned height;

	/* Every size with every filter method: the values are recovered from the green channel. */
	for (shape = 0U; shape < 6U; shape++) {
		width = sizes[shape][0];
		height = sizes[shape][1];
		for (method = 0U; method < 4U; method++) {
			/* Random alpha values, stored through the forward filter of this method. */
			for (index = 0U; index < (size_t)width * height; index++)
				values[index] = (uint8_t)(next_random() & 0xffUL);
			forward_filter(values, width, height, method, stored);

			/* Builds the payload (F in bits 2..3, C 1, random Rsv and P) and decodes it. */
			size = lossless_payload((unsigned)((next_random() & 0xf0UL) | (method << 2) | 1U), stored, width, height,
			    payload);
			memset(alpha, 0, sizeof(alpha));
			CHECK(webpc_alpha_decode(payload, size, width, height, alpha) == 0);
			CHECK(same(alpha, values, (size_t)width * height));
		}
	}
}

/*
 * Checks a lossless stream with a colour indexing transform: two colours,
 * so eight indices are bundled into each green value (RFC 9649 3.5.4), the
 * palette's green values being the alpha values.
 */
static void
test_lossless_indexing(void)
{
	static uint8_t payload[LBITWRITER_BYTES + 1U];
	static uint8_t alpha[11U * 3U];
	uint32_t table[2];
	uint32_t packed[2U * 3U];
	unsigned x;
	unsigned y;
	unsigned index;
	size_t bytes;

	/* The colour table as the stream stores it: the first colour, then the second minus the first, per channel. */
	table[0] = 10U << 8;
	table[1] = (200U - 10U) << 8;

	/* 11 indices a row bundled 8 to a pixel: 2 pixels a row; index i is at bit i % 8 of the green byte. */
	memset(packed, 0, sizeof(packed));
	for (y = 0U; y < 3U; y++) {
		for (x = 0U; x < 11U; x++) {
			index = (x + y) % 2U;
			if (((x * 7U + y) % 3U) == 0U)
				index = 1U - index;
			packed[y * 2U + x / 8U] |= (uint32_t)index << (8U + x % 8U);
		}
	}

	/* The stream: a transform (1 bit), colour indexing (type 3), 2 colours, the table, no more transforms, the image. */
	lbitwriter_init(&stream);
	lbitwriter_put(&stream, 1U, 1U);
	lbitwriter_put(&stream, 3U, 2U);
	lbitwriter_put(&stream, 1U, 8U);
	put_image(&stream, table, 2U, 1U, 0);
	lbitwriter_put(&stream, 0U, 1U);
	put_image(&stream, packed, 2U, 3U, 1);
	bytes = lbitwriter_size(&stream);
	payload[0] = 0x01U;
	memcpy(payload + 1, stream.bytes, bytes);

	/* Decodes at the full width 11 and expects the palette's green for each index. */
	memset(alpha, 0, sizeof(alpha));
	CHECK(webpc_alpha_decode(payload, bytes + 1U, 11U, 3U, alpha) == 0);
	for (y = 0U; y < 3U; y++) {
		for (x = 0U; x < 11U; x++) {
			index = (x + y) % 2U;
			if (((x * 7U + y) % 3U) == 0U)
				index = 1U - index;
			CHECK(alpha[y * 11U + x] == (index == 0U ? 10U : 200U));
		}
	}
}

/* Checks that a lossless stream cut short is refused and that nothing is written outside the output. */
static void
test_lossless_truncated(void)
{
	static uint8_t values[8U * 8U];
	static uint8_t payload[LBITWRITER_BYTES + 1U];
	uint8_t *alpha;
	size_t size;
	size_t cut;
	size_t index;

	/* A good 8x8 payload, no filter. */
	for (index = 0U; index < sizeof(values); index++)
		values[index] = (uint8_t)(next_random() & 0xffUL);
	size = lossless_payload(0x01U, values, 8U, 8U, payload);
	alpha = (uint8_t *)malloc(sizeof(values));
	CHECK(alpha != NULL);
	if (alpha == NULL)
		return;
	CHECK(webpc_alpha_decode(payload, size, 8U, 8U, alpha) == 0);
	CHECK(same(alpha, values, sizeof(values)));

	/* Every shorter payload with at least the header byte and one more byte fails with ENOBUFS (not read past its end). */
	for (cut = 2U; cut < size; cut += 7U)
		CHECK(webpc_alpha_decode(payload, cut, 8U, 8U, alpha) == ENOBUFS);
	CHECK(webpc_alpha_decode(payload, size - 1U, 8U, 8U, alpha) == ENOBUFS);
	free(alpha);
}

/* Checks the whole decode against the forward filter on random sizes and values, raw and lossless. */
static void
test_roundtrip_filters(void)
{
	static uint8_t values[TEST_PIXELS_MAX];
	static uint8_t stored[TEST_PIXELS_MAX];
	static uint8_t payload[LBITWRITER_BYTES + 1U];
	static uint8_t alpha[TEST_PIXELS_MAX];
	unsigned round;
	unsigned width;
	unsigned height;
	unsigned method;
	unsigned mode;
	size_t count;
	size_t index;
	size_t size;

	/* Random shapes up to 40 x 40 pixels, values smooth or noisy so every filter branch is taken. */
	for (round = 0U; round < 200U; round++) {
		width = 1U + (unsigned)(next_random() % 40UL);
		height = 1U + (unsigned)(next_random() % 40UL);
		method = (unsigned)(next_random() & 3UL);
		mode = (unsigned)(next_random() & 1UL);
		count = (size_t)width * height;
		for (index = 0U; index < count; index++) {
			if (mode == 0U)
				values[index] = (uint8_t)(next_random() & 0xffUL);
			else
				values[index] = (uint8_t)(((index % width) * 5U + (index / width) * 3U) & 0xffU);
		}
		forward_filter(values, width, height, method, stored);

		/* Raw: the header byte (random Rsv and P, F, C 0) and the stored values. */
		payload[0] = (uint8_t)((next_random() & 0xf0UL) | (method << 2));
		memcpy(payload + 1, stored, count);
		memset(alpha, 0, sizeof(alpha));
		CHECK(webpc_alpha_decode(payload, count + 1U, width, height, alpha) == 0);
		CHECK(same(alpha, values, count));

		/* Lossless, for the smaller images (the flat codes make a stream large). */
		if (count <= 400U) {
			size = lossless_payload((unsigned)((next_random() & 0xf0UL) | (method << 2) | 1U), stored, width, height,
			    payload);
			memset(alpha, 0, sizeof(alpha));
			CHECK(webpc_alpha_decode(payload, size, width, height, alpha) == 0);
			CHECK(same(alpha, values, count));
		}
	}
}

/* Checks random payloads of random sizes: only the documented errors come back, and nothing is read or written outside. */
static void
test_random_payloads(void)
{
	static uint8_t payload[512];
	uint8_t *alpha;
	uint8_t *copy;
	unsigned round;
	unsigned width;
	unsigned height;
	size_t size;
	size_t index;
	int error;

	/* Random bytes, a header byte biased to C 0 and 1, in exact-size copies so ASan sees any overrun. */
	for (round = 0U; round < 600U; round++) {
		size = (size_t)(next_random() % 300UL);
		width = 1U + (unsigned)(next_random() % 12UL);
		height = 1U + (unsigned)(next_random() % 12UL);
		for (index = 0U; index < size; index++)
			payload[index] = (uint8_t)(next_random() & 0xffUL);
		if (size != 0U && (round & 1U) != 0U)
			payload[0] = (uint8_t)(payload[0] & 0xfdU);

		/* An exact-size copy of the payload (a one-byte array when empty) and of the output. */
		copy = (uint8_t *)malloc(size + 1U);
		alpha = (uint8_t *)malloc((size_t)width * height);
		CHECK(copy != NULL && alpha != NULL);
		if (copy == NULL || alpha == NULL) {
			free(copy);
			free(alpha);
			return;
		}
		memcpy(copy, payload, size);

		/* The result is success or one of the documented failures. */
		error = webpc_alpha_decode(copy, size, width, height, alpha);
		CHECK(error == 0 || error == EINVAL || error == ENOBUFS || error == ENOMEM);
		free(copy);
		free(alpha);
	}
}
