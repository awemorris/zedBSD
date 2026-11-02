/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The tests of common.c: byte order, the clamp, the floor shift and the
 * 16-bit wrap, against values worked out by hand.
 */

#include "check.h"
#include "common.h"

#include <limits.h>

static void test_bytes(void);
static void test_clamp(void);
static void test_asr(void);
static void test_wrap16(void);

/*
 * Runs the tests of common.c.
 */
int
main(
	void)
{
	int status;

	/* Each helper in turn. */
	test_bytes();
	test_clamp();
	test_asr();
	test_wrap16();

	/* The summary line. */
	status = check_finish("common");
	return status;
}

/* Checks the little-endian reads. */
static void
test_bytes(void)
{
	static const uint8_t bytes[4] = { 0x01U, 0x80U, 0xfeU, 0xffU };

	/* The low byte first, every byte value kept unsigned. */
	CHECK(webpc_le16(bytes) == 0x8001UL);
	CHECK(webpc_le24(bytes) == 0xfe8001UL);
	CHECK(webpc_le32(bytes) == 0xfffe8001UL);
}

/* Checks the clamp at and around its ends. */
static void
test_clamp(void)
{
	/* Inside, at the ends and past them. */
	CHECK(webpc_clamp255(-1000) == 0);
	CHECK(webpc_clamp255(-1) == 0);
	CHECK(webpc_clamp255(0) == 0);
	CHECK(webpc_clamp255(128) == 128);
	CHECK(webpc_clamp255(255) == 255);
	CHECK(webpc_clamp255(256) == 255);
	CHECK(webpc_clamp255(INT_MAX) == 255);
	CHECK(webpc_clamp255(INT_MIN) == 0);
}

/* Checks the floor shift against the floor of the quotient. */
static void
test_asr(void)
{
	int32_t value;
	unsigned shift;
	int32_t want;
	int32_t divisor;
	int mismatches;

	/* Hand-worked values. */
	CHECK(webpc_asr(-1, 1) == -1);
	CHECK(webpc_asr(-2, 1) == -1);
	CHECK(webpc_asr(-3, 1) == -2);
	CHECK(webpc_asr(-8, 3) == -1);
	CHECK(webpc_asr(-9, 3) == -2);
	CHECK(webpc_asr(7, 3) == 0);
	CHECK(webpc_asr(-2147483647 - 1, 31) == -1);
	CHECK(webpc_asr(-2147483647 - 1, 1) == -1073741824);
	CHECK(webpc_asr(2147483647, 31) == 0);
	CHECK(webpc_asr(-5, 0) == -5);

	/* Every value in a window, every shift up to 16, against the floor of value / 2^shift. */
	mismatches = 0;
	for (shift = 0; shift <= 16U; shift++) {
		divisor = (int32_t)1 << shift;
		for (value = -70000; value <= 70000; value += 7) {
			want = value / divisor;
			if (value % divisor != 0 && value < 0)
				want--;
			if (webpc_asr(value, shift) != want)
				mismatches++;
		}
	}
	CHECK(mismatches == 0);
}

/* Checks the 16-bit wrap. */
static void
test_wrap16(void)
{
	/* Inside the range, at its ends, and one and many periods past them. */
	CHECK(webpc_wrap16(0) == 0);
	CHECK(webpc_wrap16(32767) == 32767);
	CHECK(webpc_wrap16(-32768) == -32768);
	CHECK(webpc_wrap16(32768) == -32768);
	CHECK(webpc_wrap16(-32769) == 32767);
	CHECK(webpc_wrap16(65536 + 5) == 5);
	CHECK(webpc_wrap16(-65536 - 5) == -5);
	CHECK(webpc_wrap16(331898) == 331898 - 5 * 65536);
	CHECK(webpc_wrap16(2147483647) == -1);
	CHECK(webpc_wrap16(-2147483647 - 1) == 0);
}
