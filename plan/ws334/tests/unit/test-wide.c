/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The unit test of the UTF-16 and UTF-8 conversions (win32/wide.c) and of
 * the WM_CHAR unit assembler.
 *
 * Expected bytes and units are written out by hand from the Unicode
 * Standard (section 3.9), or made by the small encoders at the end of this
 * file, which follow the bit layouts of UTF-8 and UTF-16 and share nothing
 * with the code under test.  Every input is first copied to a block of
 * exactly its size, so that a read past its end is caught by the address
 * sanitizer on the host.
 */

#include "check.h"

#include "userland/desktop/libkeiland/win32/wide.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The number of random cases of each random test. */
#define RANDOM_CASES		100000U

/* The state of the pseudo-random generator; a fixed seed makes each run the same. */
static uint32_t random_state = 20261101U;

static void test_to_utf8_basic(void);
static void test_to_utf8_surrogates(void);
static void test_to_utf8_limits(void);
static void test_to_utf8_errors(void);
static void test_to_utf16_basic(void);
static void test_to_utf16_invalid(void);
static void test_to_utf16_limits(void);
static void test_to_utf16_errors(void);
static void test_char_unit(void);
static void test_random_round_trip(void);
static void test_random_cut(void);
static void test_random_bytes(void);
static int to_utf8(const uint16_t *units, size_t count, char *buffer, size_t size, size_t *consumed);
static int to_utf16(const char *text, size_t length, uint16_t *units, size_t capacity, size_t *count);
static int utf16_equals(const uint16_t *units, size_t count, const uint16_t *expected, size_t expected_count);
static int bytes_equal(const char *actual, const char *expected, size_t length);
static size_t encode_utf8(uint32_t code_point, char *out);
static size_t encode_utf16(uint32_t code_point, uint16_t *out);
static uint32_t random_next(void);
static uint32_t random_scalar(void);
static int well_formed_utf16(const uint16_t *units, size_t count);

int
main(
	int argc,
	char **argv)
{
	/* The harness passes the system's name. */
	CHECK(argc == 2);
	(void)argv;

	test_to_utf8_basic();
	test_to_utf8_surrogates();
	test_to_utf8_limits();
	test_to_utf8_errors();
	test_to_utf16_basic();
	test_to_utf16_invalid();
	test_to_utf16_limits();
	test_to_utf16_errors();
	test_char_unit();
	test_random_round_trip();
	test_random_cut();
	test_random_bytes();

	return check_finish("wide");
}

/*
 * Converts units through exact-size blocks; the text is not NUL-terminated
 * and the units are copied so that a read past count is caught.
 */
static int
to_utf8(
	const uint16_t *units,
	size_t count,
	char *buffer,
	size_t size,
	size_t *consumed)
{
	uint16_t *copy;
	int error;

	/* NULL stays NULL; anything else is copied to a block of count units. */
	if (units == NULL)
		return keiui_win32_utf8_from_utf16(NULL, count, buffer, size, consumed);

	copy = malloc((count == 0 ? 1 : count) * sizeof(*copy));
	CHECK(copy != NULL);
	if (count > 0)
		memcpy(copy, units, count * sizeof(*copy));

	error = keiui_win32_utf8_from_utf16(copy, count, buffer, size, consumed);

	free(copy);
	return error;
}

/*
 * Converts UTF-8 through an exact-size block.
 */
static int
to_utf16(
	const char *text,
	size_t length,
	uint16_t *units,
	size_t capacity,
	size_t *count)
{
	char *copy;
	int error;

	/* NULL stays NULL; anything else is copied to a block of length bytes. */
	if (text == NULL)
		return keiui_win32_utf16_from_utf8(NULL, length, units, capacity, count);

	copy = malloc(length == 0 ? 1 : length);
	CHECK(copy != NULL);
	if (length > 0)
		memcpy(copy, text, length);

	error = keiui_win32_utf16_from_utf8(copy, length, units, capacity, count);

	free(copy);
	return error;
}

/* Tells whether units[0..count) are the same as expected[0..expected_count). */
static int
utf16_equals(
	const uint16_t *units,
	size_t count,
	const uint16_t *expected,
	size_t expected_count)
{
	if (count != expected_count)
		return 0;
	if (count == 0)
		return 1;
	return memcmp(units, expected, count * sizeof(*units)) == 0;
}

/* Tells whether the first length bytes are the same. */
static int
bytes_equal(
	const char *actual,
	const char *expected,
	size_t length)
{
	if (length == 0)
		return 1;
	return memcmp(actual, expected, length) == 0;
}

/* Converts units and compares the whole result, the NUL and the consumed count. */
#define CHECK_TO_UTF8(units_array, expected_text)					\
	do {										\
		char got[64];								\
		size_t used = 99;							\
		size_t n = sizeof(units_array) / sizeof((units_array)[0]);		\
		memset(got, 'x', sizeof(got));						\
		CHECK_EQ(to_utf8((units_array), n, got, sizeof(got), &used), 0);	\
		CHECK_EQ(strlen(got), strlen(expected_text));				\
		CHECK(strcmp(got, (expected_text)) == 0);				\
		CHECK_EQ(used, n);							\
	} while (0)

/*
 * ASCII, two-, three- and four-byte characters, from hand-made tables.
 */
static void
test_to_utf8_basic(void)
{
	static const uint16_t ascii[] = { 0x48, 0x65, 0x6C, 0x6C, 0x6F };
	static const uint16_t two[] = { 0x00A9, 0x07FF, 0x0080 };
	static const uint16_t three[] = { 0x0800, 0x20AC, 0x3042, 0xFFFF, 0xE000, 0xD7FF };
	static const uint16_t four[] = { 0xD800, 0xDC00, 0xD83D, 0xDE00, 0xDBFF, 0xDFFF };
	char buffer[8];
	size_t used;
	int error;

	/* ASCII is one byte each. */
	CHECK_TO_UTF8(ascii, "Hello");

	/* U+00A9 is C2 A9, U+07FF is DF BF, U+0080 is C2 80. */
	CHECK_TO_UTF8(two, "\xC2\xA9\xDF\xBF\xC2\x80");

	/* U+0800 E0 A0 80, U+20AC E2 82 AC, U+3042 E3 81 82, U+FFFF EF BF BF, U+E000 EE 80 80, U+D7FF ED 9F BF. */
	CHECK_TO_UTF8(three, "\xE0\xA0\x80\xE2\x82\xAC\xE3\x81\x82\xEF\xBF\xBF\xEE\x80\x80\xED\x9F\xBF");

	/* U+10000 F0 90 80 80, U+1F600 F0 9F 98 80, U+10FFFF F4 8F BF BF. */
	CHECK_TO_UTF8(four, "\xF0\x90\x80\x80\xF0\x9F\x98\x80\xF4\x8F\xBF\xBF");

	/* An empty text gives an empty string and consumes nothing. */
	memset(buffer, 'x', sizeof(buffer));
	used = 99;
	error = to_utf8(NULL, 0, buffer, sizeof(buffer), &used);
	CHECK_EQ(error, 0);
	CHECK_EQ(buffer[0], 0);
	CHECK_EQ(used, 0);

	/* consumed may be NULL. */
	memset(buffer, 'x', sizeof(buffer));
	error = to_utf8(ascii, 2, buffer, sizeof(buffer), NULL);
	CHECK_EQ(error, 0);
	CHECK(strcmp(buffer, "He") == 0);
}

/*
 * Unpaired surrogates become U+FFFD (EF BF BD) wherever they are.
 */
static void
test_to_utf8_surrogates(void)
{
	static const uint16_t lone_high_start[] = { 0xD800, 0x41 };
	static const uint16_t lone_high_end[] = { 0x41, 0xDBFF };
	static const uint16_t lone_high_middle[] = { 0x41, 0xD83D, 0x42 };
	static const uint16_t lone_low_start[] = { 0xDC00, 0x41 };
	static const uint16_t lone_low_end[] = { 0x41, 0xDFFF };
	static const uint16_t lone_low_middle[] = { 0x41, 0xDE00, 0x42 };
	static const uint16_t low_then_high[] = { 0xDC00, 0xD800 };
	static const uint16_t high_high_low[] = { 0xD800, 0xD83D, 0xDE00 };
	static const uint16_t high_low_low[] = { 0xD83D, 0xDE00, 0xDE00 };
	static const uint16_t pair_after_high[] = { 0xD800, 0xD800, 0xDC00 };
	static const uint16_t only_high[] = { 0xD800 };
	static const uint16_t only_low[] = { 0xDC00 };
	char buffer[64];
	size_t used;
	int error;

	CHECK_TO_UTF8(lone_high_start, "\xEF\xBF\xBD" "A");
	CHECK_TO_UTF8(lone_high_end, "A\xEF\xBF\xBD");
	CHECK_TO_UTF8(lone_high_middle, "A\xEF\xBF\xBD" "B");
	CHECK_TO_UTF8(lone_low_start, "\xEF\xBF\xBD" "A");
	CHECK_TO_UTF8(lone_low_end, "A\xEF\xBF\xBD");
	CHECK_TO_UTF8(lone_low_middle, "A\xEF\xBF\xBD" "B");
	CHECK_TO_UTF8(low_then_high, "\xEF\xBF\xBD\xEF\xBF\xBD");
	CHECK_TO_UTF8(only_high, "\xEF\xBF\xBD");
	CHECK_TO_UTF8(only_low, "\xEF\xBF\xBD");

	/* A high surrogate followed by a good pair: the first is alone, the pair is U+1F600. */
	CHECK_TO_UTF8(high_high_low, "\xEF\xBF\xBD\xF0\x9F\x98\x80");

	/* A pair followed by a low surrogate: the low one is alone. */
	CHECK_TO_UTF8(high_low_low, "\xF0\x9F\x98\x80\xEF\xBF\xBD");

	/* A high one, then the pair D800 DC00 = U+10000. */
	CHECK_TO_UTF8(pair_after_high, "\xEF\xBF\xBD\xF0\x90\x80\x80");

	/* A high surrogate at the end of the count is alone even if memory after it holds a low one. */
	{
		static const uint16_t after[] = { 0xD83D, 0xDE00 };

		memset(buffer, 'x', sizeof(buffer));
		used = 99;
		error = to_utf8(after, 1, buffer, sizeof(buffer), &used);
		CHECK_EQ(error, 0);
		CHECK(strcmp(buffer, "\xEF\xBF\xBD") == 0);
		CHECK_EQ(used, 1);
	}
}

/*
 * The buffer's size cuts the text at a character, never inside one, and a
 * NUL unit ends the text.
 */
static void
test_to_utf8_limits(void)
{
	static const uint16_t text[] = { 0x41, 0x20AC, 0x42 };
	static const uint16_t pair_text[] = { 0x41, 0xD83D, 0xDE00 };
	static const uint16_t with_nul[] = { 0x41, 0x42, 0x0000, 0x43 };
	static const uint16_t nul_after_high[] = { 0xD83D, 0x0000, 0xDE00 };
	static const uint16_t only_euro[] = { 0x20AC };
	char buffer[16];
	size_t used;
	int error;

	/* "A" + E2 82 AC + "B" is 5 bytes; 6 fits with the NUL. */
	memset(buffer, 'x', sizeof(buffer));
	used = 99;
	error = to_utf8(text, 3, buffer, 6, &used);
	CHECK_EQ(error, 0);
	CHECK(bytes_equal(buffer, "A\xE2\x82\xAC" "B\0", 6));
	CHECK_EQ(used, 3);

	/* Size 5 has room for "A" and the 3-byte character but not the B and the NUL: stops before B. */
	memset(buffer, 'x', sizeof(buffer));
	used = 99;
	error = to_utf8(text, 3, buffer, 5, &used);
	CHECK_EQ(error, ERANGE);
	CHECK(bytes_equal(buffer, "A\xE2\x82\xAC\0", 5));
	CHECK_EQ(used, 2);

	/* Size 4 cannot hold A, the 3-byte character and the NUL: stops before the character. */
	memset(buffer, 'x', sizeof(buffer));
	used = 99;
	error = to_utf8(text, 3, buffer, 4, &used);
	CHECK_EQ(error, ERANGE);
	CHECK(bytes_equal(buffer, "A\0", 2));
	CHECK_EQ(used, 1);

	/* Size 2 holds "A" and the NUL. */
	memset(buffer, 'x', sizeof(buffer));
	used = 99;
	error = to_utf8(text, 3, buffer, 2, &used);
	CHECK_EQ(error, ERANGE);
	CHECK(bytes_equal(buffer, "A\0", 2));
	CHECK_EQ(used, 1);

	/* Size 1 is the empty string. */
	memset(buffer, 'x', sizeof(buffer));
	used = 99;
	error = to_utf8(text, 3, buffer, 1, &used);
	CHECK_EQ(error, ERANGE);
	CHECK_EQ(buffer[0], 0);
	CHECK_EQ(buffer[1], 'x');
	CHECK_EQ(used, 0);

	/* A single 3-byte character needs size 4. */
	memset(buffer, 'x', sizeof(buffer));
	used = 99;
	error = to_utf8(only_euro, 1, buffer, 3, &used);
	CHECK_EQ(error, ERANGE);
	CHECK_EQ(buffer[0], 0);
	CHECK_EQ(used, 0);
	memset(buffer, 'x', sizeof(buffer));
	error = to_utf8(only_euro, 1, buffer, 4, &used);
	CHECK_EQ(error, 0);
	CHECK(bytes_equal(buffer, "\xE2\x82\xAC\0", 4));
	CHECK_EQ(used, 1);

	/* A pair (4 bytes) is not consumed half-way: with room for 4 bytes but no NUL it stays out. */
	memset(buffer, 'x', sizeof(buffer));
	used = 99;
	error = to_utf8(pair_text, 3, buffer, 5, &used);
	CHECK_EQ(error, ERANGE);
	CHECK(bytes_equal(buffer, "A\0", 2));
	CHECK_EQ(used, 1);
	memset(buffer, 'x', sizeof(buffer));
	error = to_utf8(pair_text, 3, buffer, 6, &used);
	CHECK_EQ(error, 0);
	CHECK(bytes_equal(buffer, "A\xF0\x9F\x98\x80\0", 6));
	CHECK_EQ(used, 3);

	/* A size larger than the text changes nothing after the NUL. */
	memset(buffer, 'x', sizeof(buffer));
	error = to_utf8(text, 1, buffer, sizeof(buffer), &used);
	CHECK_EQ(error, 0);
	CHECK_EQ(buffer[1], 0);
	CHECK_EQ(buffer[2], 'x');

	/* A NUL unit ends the text early and is not counted; the units after it are not read for output. */
	memset(buffer, 'x', sizeof(buffer));
	used = 99;
	error = to_utf8(with_nul, 4, buffer, sizeof(buffer), &used);
	CHECK_EQ(error, 0);
	CHECK(strcmp(buffer, "AB") == 0);
	CHECK_EQ(used, 2);

	/* A NUL right after a high surrogate: the high one is alone. */
	memset(buffer, 'x', sizeof(buffer));
	used = 99;
	error = to_utf8(nul_after_high, 3, buffer, sizeof(buffer), &used);
	CHECK_EQ(error, 0);
	CHECK(strcmp(buffer, "\xEF\xBF\xBD") == 0);
	CHECK_EQ(used, 1);

	/* A text that is only a NUL unit is empty. */
	memset(buffer, 'x', sizeof(buffer));
	used = 99;
	error = to_utf8(with_nul + 2, 1, buffer, sizeof(buffer), &used);
	CHECK_EQ(error, 0);
	CHECK_EQ(buffer[0], 0);
	CHECK_EQ(used, 0);

	/* An empty text fits size 1 without error. */
	memset(buffer, 'x', sizeof(buffer));
	used = 99;
	error = to_utf8(text, 0, buffer, 1, &used);
	CHECK_EQ(error, 0);
	CHECK_EQ(buffer[0], 0);
	CHECK_EQ(used, 0);
}

/* The arguments that are refused. */
static void
test_to_utf8_errors(void)
{
	static const uint16_t text[] = { 0x41 };
	char buffer[4];
	size_t used;

	memset(buffer, 'x', sizeof(buffer));

	/* A NULL buffer. */
	used = 99;
	CHECK_EQ(keiui_win32_utf8_from_utf16(text, 1, NULL, 4, &used), EINVAL);

	/* Size 0 writes nothing, not even a NUL. */
	CHECK_EQ(keiui_win32_utf8_from_utf16(text, 1, buffer, 0, &used), EINVAL);
	CHECK_EQ(buffer[0], 'x');

	/* NULL units with a count. */
	CHECK_EQ(keiui_win32_utf8_from_utf16(NULL, 1, buffer, 4, &used), EINVAL);

	/* NULL units with count 0 is an empty text. */
	CHECK_EQ(keiui_win32_utf8_from_utf16(NULL, 0, buffer, 4, &used), 0);
	CHECK_EQ(buffer[0], 0);
	CHECK_EQ(used, 0);
}

/* Converts UTF-8 and compares the whole result and the NUL unit. */
#define CHECK_TO_UTF16(bytes_text, expected_units)					\
	do {										\
		uint16_t got[64];							\
		size_t used = 99;							\
		size_t n = sizeof(expected_units) / sizeof((expected_units)[0]);	\
		memset(got, 0x55, sizeof(got));						\
		CHECK_EQ(to_utf16((bytes_text), sizeof(bytes_text) - 1, got, 64, &used), 0); \
		CHECK(utf16_equals(got, used, (expected_units), n));			\
		CHECK_EQ(got[used], 0);							\
	} while (0)

/*
 * Well-formed text of each length.
 */
static void
test_to_utf16_basic(void)
{
	static const uint16_t ascii[] = { 0x48, 0x69 };
	static const uint16_t mixed[] = { 0x41, 0x00A9, 0x20AC, 0xD83D, 0xDE00, 0x42 };
	static const uint16_t edges[] = {
		0x007F, 0x0080, 0x07FF, 0x0800, 0xD7FF, 0xE000, 0xFFFF,
		0xD800, 0xDC00, 0xDBFF, 0xDFFF
	};
	uint16_t got[8];
	size_t used;
	int error;

	CHECK_TO_UTF16("Hi", ascii);
	CHECK_TO_UTF16("A\xC2\xA9\xE2\x82\xAC\xF0\x9F\x98\x80" "B", mixed);

	/* The edges: 7F, C2 80, DF BF, E0 A0 80, ED 9F BF, EE 80 80, EF BF BF, F0 90 80 80, F4 8F BF BF. */
	CHECK_TO_UTF16("\x7F\xC2\x80\xDF\xBF\xE0\xA0\x80\xED\x9F\xBF\xEE\x80\x80\xEF\xBF\xBF\xF0\x90\x80\x80\xF4\x8F\xBF\xBF", edges);

	/* Empty text: count 0 and a NUL unit. */
	memset(got, 0x55, sizeof(got));
	used = 99;
	error = to_utf16("", 0, got, 8, &used);
	CHECK_EQ(error, 0);
	CHECK_EQ(used, 0);
	CHECK_EQ(got[0], 0);

	/* Empty text from a NULL pointer. */
	memset(got, 0x55, sizeof(got));
	error = to_utf16(NULL, 0, got, 8, &used);
	CHECK_EQ(error, 0);
	CHECK_EQ(used, 0);
	CHECK_EQ(got[0], 0);

	/* A NUL byte inside ends the text. */
	memset(got, 0x55, sizeof(got));
	error = to_utf16("ab\0cd", 5, got, 8, &used);
	CHECK_EQ(error, 0);
	CHECK_EQ(used, 2);
	CHECK_EQ(got[0], 'a');
	CHECK_EQ(got[1], 'b');
	CHECK_EQ(got[2], 0);

	/* A NUL byte in the middle of a sequence ends the subpart before it. */
	memset(got, 0x55, sizeof(got));
	error = to_utf16("\xE2\x82\0\xAC", 4, got, 8, &used);
	CHECK_EQ(error, 0);
	CHECK_EQ(used, 1);
	CHECK_EQ(got[0], 0xFFFD);
	CHECK_EQ(got[1], 0);
}

/*
 * Ill-formed UTF-8: one U+FFFD for each maximal subpart.
 */
static void
test_to_utf16_invalid(void)
{
	static const uint16_t table_3_8[] = {
		0x0061, 0xFFFD, 0xFFFD, 0xFFFD, 0x0062, 0xFFFD, 0x0063, 0xFFFD, 0xFFFD, 0x0064
	};
	static const uint16_t one[] = { 0xFFFD };
	static const uint16_t two[] = { 0xFFFD, 0xFFFD };
	static const uint16_t three[] = { 0xFFFD, 0xFFFD, 0xFFFD };
	static const uint16_t four[] = { 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD };
	static const uint16_t five[] = { 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD };
	static const uint16_t one_then_a[] = { 0xFFFD, 0x0041 };
	static const uint16_t a_then_one[] = { 0x0041, 0xFFFD };
	uint16_t got[16];
	size_t used;
	int error;
	unsigned lead;

	/* The example of table 3-8: 61 F1 80 80 E1 80 C2 62 80 63 80 BF 64. */
	CHECK_TO_UTF16("\x61\xF1\x80\x80\xE1\x80\xC2\x62\x80\x63\x80\xBF\x64", table_3_8);

	/* A continuation byte alone. */
	CHECK_TO_UTF16("\x80", one);
	CHECK_TO_UTF16("\xBF", one);
	CHECK_TO_UTF16("\x80\x80", two);
	CHECK_TO_UTF16("\x80\xBF\x80", three);

	/* Bytes that never begin a sequence: C0, C1, F5 to FF. */
	CHECK_TO_UTF16("\xC0", one);
	CHECK_TO_UTF16("\xC1", one);
	CHECK_TO_UTF16("\xC0\x80", two);
	CHECK_TO_UTF16("\xC1\xBF", two);
	CHECK_TO_UTF16("\xF5", one);
	CHECK_TO_UTF16("\xF5\x80\x80\x80", four);
	CHECK_TO_UTF16("\xFF", one);
	CHECK_TO_UTF16("\xFE\xFF", two);
	CHECK_TO_UTF16("\xF8\x88\x80\x80\x80", five);

	/* Overlong forms: E0 80 80 (3 bytes), F0 80 80 80, E0 9F BF (one past the last overlong). */
	CHECK_TO_UTF16("\xE0\x80\x80", three);
	CHECK_TO_UTF16("\xF0\x80\x80\x80", four);
	CHECK_TO_UTF16("\xF0\x8F\xBF\xBF", four);
	CHECK_TO_UTF16("\xE0\x9F\xBF", three);

	/* Surrogates encoded in UTF-8: ED A0 80 (D800) and ED BF BF (DFFF). */
	CHECK_TO_UTF16("\xED\xA0\x80", three);
	CHECK_TO_UTF16("\xED\xBF\xBF", three);

	/* Above U+10FFFF: F4 90 80 80. */
	CHECK_TO_UTF16("\xF4\x90\x80\x80", four);

	/* A truncated sequence at the end is one U+FFFD for what is there. */
	CHECK_TO_UTF16("\xE2", one);
	CHECK_TO_UTF16("\xE2\x82", one);
	CHECK_TO_UTF16("\xF0\x9F\x98", one);
	CHECK_TO_UTF16("\xF0", one);
	CHECK_TO_UTF16("\xC2", one);

	/* A truncated sequence followed by a good character. */
	CHECK_TO_UTF16("\xE2\x82" "A", one_then_a);
	CHECK_TO_UTF16("\xF0\x9F" "A", one_then_a);
	CHECK_TO_UTF16("A\xC2", a_then_one);

	/* A lead byte followed by another lead byte: each is its own subpart. */
	CHECK_TO_UTF16("\xE2\xE2\x82\xAC", ((const uint16_t[]) { 0xFFFD, 0x20AC }));

	/* Lead byte followed by a continuation out of its second-byte range (the second byte starts the next subpart). */
	CHECK_TO_UTF16("\xE0\xA0" "A", one_then_a);
	CHECK_TO_UTF16("\xED\xA0" "A", ((const uint16_t[]) { 0xFFFD, 0xFFFD, 0x0041 }));
	CHECK_TO_UTF16("\xF4\x90" "A", ((const uint16_t[]) { 0xFFFD, 0xFFFD, 0x0041 }));
	CHECK_TO_UTF16("\xF0\x80" "A", ((const uint16_t[]) { 0xFFFD, 0xFFFD, 0x0041 }));

	/* A bad third byte keeps the first two as one subpart, and a bad fourth keeps three. */
	CHECK_TO_UTF16("\xE2\x82" "AB", ((const uint16_t[]) { 0xFFFD, 0x0041, 0x0042 }));
	CHECK_TO_UTF16("\xF0\x9F\x98" "AB", ((const uint16_t[]) { 0xFFFD, 0x0041, 0x0042 }));

	/* Every byte from 80 to FF alone, and every lead byte from C2 to F4 followed by a space. */
	for (lead = 0x80; lead <= 0xFF; lead++) {
		char text[2];

		text[0] = (char)lead;
		text[1] = ' ';
		memset(got, 0x55, sizeof(got));
		error = to_utf16(text, 2, got, 16, &used);
		CHECK_EQ(error, 0);
		CHECK_EQ(used, 2);
		CHECK_EQ(got[0], 0xFFFD);
		CHECK_EQ(got[1], 0x20);
	}

	/* A character that is well-formed after an invalid byte is kept. */
	CHECK_TO_UTF16("\xFF\xE2\x82\xAC", ((const uint16_t[]) { 0xFFFD, 0x20AC }));
}

/*
 * The capacity: the count that is needed, and the empty string left behind.
 */
static void
test_to_utf16_limits(void)
{
	uint16_t got[16];
	size_t used;
	int error;
	size_t capacity;

	/* "A" + U+1F600 + "B" takes 4 units and a NUL. */
	for (capacity = 1; capacity <= 4; capacity++) {
		memset(got, 0x55, sizeof(got));
		used = 99;
		error = to_utf16("A\xF0\x9F\x98\x80" "B", 6, got, capacity, &used);
		CHECK_EQ(error, ERANGE);
		CHECK_EQ(used, 4);
		CHECK_EQ(got[0], 0);
		CHECK_EQ(got[1], 0x5555);
	}
	memset(got, 0x55, sizeof(got));
	used = 99;
	error = to_utf16("A\xF0\x9F\x98\x80" "B", 6, got, 5, &used);
	CHECK_EQ(error, 0);
	CHECK_EQ(used, 4);
	CHECK_EQ(got[0], 'A');
	CHECK_EQ(got[1], 0xD83D);
	CHECK_EQ(got[2], 0xDE00);
	CHECK_EQ(got[3], 'B');
	CHECK_EQ(got[4], 0);
	CHECK_EQ(got[5], 0x5555);

	/* Nothing is written past the capacity. */
	memset(got, 0x55, sizeof(got));
	error = to_utf16("ab", 2, got, 3, &used);
	CHECK_EQ(error, 0);
	CHECK_EQ(got[3], 0x5555);

	/* An empty text fits capacity 1; a text of one unit does not. */
	memset(got, 0x55, sizeof(got));
	error = to_utf16("", 0, got, 1, &used);
	CHECK_EQ(error, 0);
	CHECK_EQ(used, 0);
	CHECK_EQ(got[0], 0);
	memset(got, 0x55, sizeof(got));
	error = to_utf16("a", 1, got, 1, &used);
	CHECK_EQ(error, ERANGE);
	CHECK_EQ(used, 1);
	CHECK_EQ(got[0], 0);

	/* The needed count counts the replacement characters. */
	memset(got, 0x55, sizeof(got));
	error = to_utf16("\xFF\xFF\xFF", 3, got, 2, &used);
	CHECK_EQ(error, ERANGE);
	CHECK_EQ(used, 3);
	CHECK_EQ(got[0], 0);

	/* A NUL byte ends the count too. */
	memset(got, 0x55, sizeof(got));
	error = to_utf16("ab\0cd", 5, got, 2, &used);
	CHECK_EQ(error, ERANGE);
	CHECK_EQ(used, 2);

	/* The caller can allocate what was asked for and call again. */
	memset(got, 0x55, sizeof(got));
	error = to_utf16("\xE2\x82\xAC" "xyz", 6, got, 1, &used);
	CHECK_EQ(error, ERANGE);
	CHECK_EQ(used, 4);
	memset(got, 0x55, sizeof(got));
	error = to_utf16("\xE2\x82\xAC" "xyz", 6, got, used + 1, &used);
	CHECK_EQ(error, 0);
	CHECK_EQ(used, 4);
	CHECK_EQ(got[4], 0);
}

/* The arguments that are refused. */
static void
test_to_utf16_errors(void)
{
	uint16_t got[4];
	size_t used;

	memset(got, 0x55, sizeof(got));

	/* NULL text with a length. */
	used = 99;
	CHECK_EQ(keiui_win32_utf16_from_utf8(NULL, 1, got, 4, &used), EINVAL);

	/* NULL units. */
	CHECK_EQ(keiui_win32_utf16_from_utf8("a", 1, NULL, 4, &used), EINVAL);

	/* NULL count. */
	CHECK_EQ(keiui_win32_utf16_from_utf8("a", 1, got, 4, NULL), EINVAL);

	/* Capacity 0 writes nothing. */
	CHECK_EQ(keiui_win32_utf16_from_utf8("a", 1, got, 0, &used), EINVAL);
	CHECK_EQ(got[0], 0x5555);
}

/*
 * The WM_CHAR assembler.
 */
static void
test_char_unit(void)
{
	struct keiui_win32_surrogate state;
	uint32_t code_point;
	int got;

	memset(&state, 0, sizeof(state));

	/* A BMP unit is a character at once, including ASCII and the edges of the surrogate range. */
	code_point = 0;
	got = keiui_win32_char_unit(&state, 0x41, &code_point);
	CHECK_EQ(got, 1);
	CHECK_EQ(code_point, 0x41);
	got = keiui_win32_char_unit(&state, 0xD7FF, &code_point);
	CHECK_EQ(got, 1);
	CHECK_EQ(code_point, 0xD7FF);
	got = keiui_win32_char_unit(&state, 0xE000, &code_point);
	CHECK_EQ(got, 1);
	CHECK_EQ(code_point, 0xE000);
	got = keiui_win32_char_unit(&state, 0xFFFF, &code_point);
	CHECK_EQ(got, 1);
	CHECK_EQ(code_point, 0xFFFF);
	CHECK_EQ(state.high, 0);

	/* A pair: the first unit waits and writes nothing. */
	code_point = 0x1234;
	got = keiui_win32_char_unit(&state, 0xD83D, &code_point);
	CHECK_EQ(got, 0);
	CHECK_EQ(code_point, 0x1234);
	CHECK_EQ(state.high, 0xD83D);
	got = keiui_win32_char_unit(&state, 0xDE00, &code_point);
	CHECK_EQ(got, 1);
	CHECK_EQ(code_point, 0x1F600);
	CHECK_EQ(state.high, 0);

	/* The smallest and the largest pairs. */
	keiui_win32_char_unit(&state, 0xD800, &code_point);
	got = keiui_win32_char_unit(&state, 0xDC00, &code_point);
	CHECK_EQ(got, 1);
	CHECK_EQ(code_point, 0x10000);
	keiui_win32_char_unit(&state, 0xDBFF, &code_point);
	got = keiui_win32_char_unit(&state, 0xDFFF, &code_point);
	CHECK_EQ(got, 1);
	CHECK_EQ(code_point, 0x10FFFF);

	/* A high surrogate after a high surrogate: U+FFFD now, and the new one waits. */
	keiui_win32_char_unit(&state, 0xD800, &code_point);
	code_point = 0;
	got = keiui_win32_char_unit(&state, 0xD83D, &code_point);
	CHECK_EQ(got, 1);
	CHECK_EQ(code_point, 0xFFFD);
	CHECK_EQ(state.high, 0xD83D);
	got = keiui_win32_char_unit(&state, 0xDE00, &code_point);
	CHECK_EQ(got, 1);
	CHECK_EQ(code_point, 0x1F600);

	/* A low surrogate alone. */
	code_point = 0;
	got = keiui_win32_char_unit(&state, 0xDC00, &code_point);
	CHECK_EQ(got, 1);
	CHECK_EQ(code_point, 0xFFFD);
	CHECK_EQ(state.high, 0);

	/* A pair is used up: a second low unit is alone. */
	keiui_win32_char_unit(&state, 0xD83D, &code_point);
	keiui_win32_char_unit(&state, 0xDE00, &code_point);
	code_point = 0;
	got = keiui_win32_char_unit(&state, 0xDE00, &code_point);
	CHECK_EQ(got, 1);
	CHECK_EQ(code_point, 0xFFFD);

	/* A plain unit after a waiting high one is reported; the high one is not kept. */
	keiui_win32_char_unit(&state, 0xD83D, &code_point);
	code_point = 0;
	got = keiui_win32_char_unit(&state, 0x42, &code_point);
	CHECK_EQ(got, 1);
	CHECK_EQ(code_point, 0x42);
	CHECK_EQ(state.high, 0);
	got = keiui_win32_char_unit(&state, 0xDE00, &code_point);
	CHECK_EQ(got, 1);
	CHECK_EQ(code_point, 0xFFFD);

	/* NULL arguments return 0 and change nothing. */
	keiui_win32_char_unit(&state, 0xD83D, &code_point);
	code_point = 0x77;
	CHECK_EQ(keiui_win32_char_unit(NULL, 0x41, &code_point), 0);
	CHECK_EQ(code_point, 0x77);
	CHECK_EQ(keiui_win32_char_unit(&state, 0xDE00, NULL), 0);
	CHECK_EQ(state.high, 0xD83D);
	CHECK_EQ(keiui_win32_char_unit(NULL, 0x41, NULL), 0);
}

/*
 * Random characters: UTF-16 to UTF-8 and back agree with the encoders of
 * this file.
 */
static void
test_random_round_trip(void)
{
	unsigned round;

	/* Each round converts a random string of 1 to 12 scalar values both ways. */
	for (round = 0; round < RANDOM_CASES; round++) {
		uint16_t units[32];
		uint16_t back[40];
		char expected[64];
		char buffer[64];
		size_t unit_count = 0;
		size_t byte_count = 0;
		size_t used;
		size_t back_count;
		size_t characters = 1 + random_next() % 12;
		size_t index;

		for (index = 0; index < characters; index++) {
			uint32_t code_point = random_scalar();

			unit_count += encode_utf16(code_point, units + unit_count);
			byte_count += encode_utf8(code_point, expected + byte_count);
		}

		CHECK_EQ(to_utf8(units, unit_count, buffer, sizeof(buffer), &used), 0);
		CHECK_EQ(used, unit_count);
		CHECK_EQ(strlen(buffer), byte_count);
		CHECK(bytes_equal(buffer, expected, byte_count));

		CHECK_EQ(to_utf16(expected, byte_count, back, 40, &back_count), 0);
		CHECK(utf16_equals(back, back_count, units, unit_count));
		CHECK_EQ(back[back_count], 0);
	}
}

/*
 * Random sizes: the output is always a prefix of the whole at a character
 * boundary and consumed says how far.
 */
static void
test_random_cut(void)
{
	unsigned round;

	for (round = 0; round < RANDOM_CASES; round++) {
		uint16_t units[32];
		char whole[64];
		char buffer[64];
		size_t unit_count = 0;
		size_t byte_count = 0;
		size_t used;
		size_t size;
		size_t characters = 1 + random_next() % 8;
		size_t index;
		size_t boundary_bytes;
		size_t boundary_units;
		int error;

		for (index = 0; index < characters; index++) {
			uint32_t code_point = random_scalar();

			unit_count += encode_utf16(code_point, units + unit_count);
			byte_count += encode_utf8(code_point, whole + byte_count);
		}

		size = 1 + random_next() % (byte_count + 3);
		memset(buffer, 'x', sizeof(buffer));
		used = 99;
		error = to_utf8(units, unit_count, buffer, size, &used);

		/* The expected cut: the most whole characters that leave room for the NUL. */
		boundary_bytes = 0;
		boundary_units = 0;
		for (index = 0; index < characters; index++) {
			uint32_t code_point;
			char one_utf8[4];
			uint16_t one_utf16[2];
			size_t one_bytes;
			size_t one_units;

			/* Decodes the character at boundary_units by hand from the units. */
			if (units[boundary_units] >= 0xD800 && units[boundary_units] <= 0xDBFF) {
				code_point = 0x10000 + (((uint32_t)units[boundary_units] - 0xD800) << 10) +
				    (uint32_t)(units[boundary_units + 1] - 0xDC00);
			} else {
				code_point = units[boundary_units];
			}
			one_bytes = encode_utf8(code_point, one_utf8);
			one_units = encode_utf16(code_point, one_utf16);
			if (boundary_bytes + one_bytes + 1 > size)
				break;
			boundary_bytes += one_bytes;
			boundary_units += one_units;
		}

		CHECK_EQ(error, boundary_units == unit_count ? 0 : ERANGE);
		CHECK_EQ(used, boundary_units);
		CHECK_EQ(strlen(buffer), boundary_bytes);
		CHECK(bytes_equal(buffer, whole, boundary_bytes));
		CHECK_EQ(buffer[boundary_bytes], 0);
		CHECK_EQ(buffer[boundary_bytes + 1], 'x');
	}
}

/*
 * Random bytes, short and mostly UTF-8-like: the result is well-formed
 * UTF-16 in the number of units the count says, the size query agrees with
 * the conversion, and converting the result to UTF-8 and back changes
 * nothing.
 */
static void
test_random_bytes(void)
{
	static const unsigned char interesting[] = {
		0x00, 0x41, 0x7F, 0x80, 0x8F, 0x90, 0x9F, 0xA0, 0xBF, 0xC0, 0xC1, 0xC2, 0xDF,
		0xE0, 0xE1, 0xEC, 0xED, 0xEE, 0xEF, 0xF0, 0xF1, 0xF3, 0xF4, 0xF5, 0xFF
	};
	unsigned round;

	for (round = 0; round < RANDOM_CASES; round++) {
		char bytes[24];
		uint16_t units[32];
		uint16_t again[32];
		char utf8[128];
		size_t length = random_next() % sizeof(bytes);
		size_t count;
		size_t again_count;
		size_t used;
		size_t index;
		size_t zero_at;
		int error;

		for (index = 0; index < length; index++) {
			if (random_next() % 4 == 0)
				bytes[index] = (char)(random_next() & 0xFF);
			else
				bytes[index] = (char)interesting[random_next() % sizeof(interesting)];
			/* Zero bytes are common in the table; keep most texts whole by thinning them. */
			if (bytes[index] == 0 && random_next() % 4 != 0)
				bytes[index] = 'z';
		}

		/* The text ends at the first NUL byte, if any. */
		zero_at = length;
		for (index = 0; index < length; index++) {
			if (bytes[index] == 0) {
				zero_at = index;
				break;
			}
		}

		/* The size query agrees with a conversion into a large buffer. */
		error = to_utf16(bytes, length, units, 1, &count);
		CHECK(error == 0 || error == ERANGE);
		error = to_utf16(bytes, length, units, 32, &used);
		CHECK_EQ(error, 0);
		CHECK_EQ(used, count);
		CHECK_EQ(units[used], 0);

		/* Every output unit is part of a well-formed UTF-16 string. */
		CHECK(well_formed_utf16(units, used));

		/* The text is no longer than its bytes (a unit for each byte at most). */
		CHECK(used <= zero_at);

		/* Converting back and forth is stable: invalid input is gone after one pass. */
		CHECK_EQ(to_utf8(units, used, utf8, sizeof(utf8), &index), 0);
		CHECK_EQ(index, used);
		CHECK_EQ(to_utf16(utf8, strlen(utf8), again, 32, &again_count), 0);
		CHECK(utf16_equals(again, again_count, units, used));
	}
}

/* Tells whether every surrogate unit is in a pair and none of the units is NUL. */
static int
well_formed_utf16(
	const uint16_t *units,
	size_t count)
{
	size_t index;

	for (index = 0; index < count; index++) {
		if (units[index] >= 0xD800 && units[index] <= 0xDBFF) {
			if (index + 1 >= count)
				return 0;
			if (units[index + 1] < 0xDC00 || units[index + 1] > 0xDFFF)
				return 0;
			index++;
		} else if (units[index] >= 0xDC00 && units[index] <= 0xDFFF) {
			return 0;
		}
	}
	return 1;
}

/* Encodes a scalar value in UTF-8 by the layout of the Standard's table 3-6. */
static size_t
encode_utf8(
	uint32_t code_point,
	char *out)
{
	if (code_point <= 0x7F) {
		out[0] = (char)code_point;
		return 1;
	}
	if (code_point <= 0x7FF) {
		out[0] = (char)(0xC0 | (code_point >> 6));
		out[1] = (char)(0x80 | (code_point & 0x3F));
		return 2;
	}
	if (code_point <= 0xFFFF) {
		out[0] = (char)(0xE0 | (code_point >> 12));
		out[1] = (char)(0x80 | ((code_point >> 6) & 0x3F));
		out[2] = (char)(0x80 | (code_point & 0x3F));
		return 3;
	}
	out[0] = (char)(0xF0 | (code_point >> 18));
	out[1] = (char)(0x80 | ((code_point >> 12) & 0x3F));
	out[2] = (char)(0x80 | ((code_point >> 6) & 0x3F));
	out[3] = (char)(0x80 | (code_point & 0x3F));
	return 4;
}

/* Encodes a scalar value in UTF-16 by D800 + (v - 10000) / 400 and DC00 + (v - 10000) % 400. */
static size_t
encode_utf16(
	uint32_t code_point,
	uint16_t *out)
{
	uint32_t v;

	if (code_point <= 0xFFFF) {
		out[0] = (uint16_t)code_point;
		return 1;
	}
	v = code_point - 0x10000;
	out[0] = (uint16_t)(0xD800 + v / 0x400);
	out[1] = (uint16_t)(0xDC00 + v % 0x400);
	return 2;
}

/* A simple linear congruential generator; the quality does not matter here. */
static uint32_t
random_next(void)
{
	random_state = random_state * 1664525U + 1013904223U;
	return random_state >> 8;
}

/*
 * A random Unicode scalar value (never a surrogate, never NUL), biased to
 * the edges of each encoded length.
 */
static uint32_t
random_scalar(void)
{
	static const uint32_t edges[] = {
		0x01, 0x7F, 0x80, 0x7FF, 0x800, 0xD7FF, 0xE000, 0xFFFD, 0xFFFF, 0x10000, 0x10FFFF
	};
	uint32_t value;

	switch (random_next() % 5) {
	case 0:
		return edges[random_next() % (sizeof(edges) / sizeof(edges[0]))];
	case 1:
		return 1 + random_next() % 0x7F;
	case 2:
		return 0x80 + random_next() % 0x780;
	case 3:
		value = 0x800 + random_next() % 0xF800;
		if (value >= 0xD800 && value <= 0xDFFF)
			value = 0xE000 + (value - 0xD800);
		return value;
	default:
		return 0x10000 + random_next() % 0x100000;
	}
}
