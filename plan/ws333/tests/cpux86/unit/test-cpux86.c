/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The checks of the bit-field helpers: both ends of each register, every
 * position the helpers must refuse, and the powers of two around each
 * width.  The expected values are worked out by hand.
 */

#include "check.h"
#include "cpux86.h"

/*
 * Runs the helper checks.
 */
int
main(void)
{
	uint32_t low;
	uint32_t width;
	uint64_t pattern;

	/* A 32-bit field at both ends and in the middle. */
	CHECK(cpux86_field32(0xDEADBEEFU, 0U, 32U) == 0xDEADBEEFU);
	CHECK(cpux86_field32(0x80000000U, 31U, 1U) == 1U);
	CHECK(cpux86_field32(0x7FFFFFFFU, 31U, 1U) == 0U);
	CHECK(cpux86_field32(0x000906A4U, 4U, 4U) == 0xAU);
	CHECK(cpux86_field32(0x000906A4U, 16U, 4U) == 9U);
	CHECK(cpux86_field32(0xFFFFFFFFU, 0U, 1U) == 1U);

	/* A 32-bit field the register does not have reads as 0. */
	CHECK(cpux86_field32(0xFFFFFFFFU, 0U, 0U) == 0U);
	CHECK(cpux86_field32(0xFFFFFFFFU, 0U, 33U) == 0U);
	CHECK(cpux86_field32(0xFFFFFFFFU, 32U, 1U) == 0U);
	CHECK(cpux86_field32(0xFFFFFFFFU, 31U, 2U) == 0U);
	CHECK(cpux86_field32(0xFFFFFFFFU, 0xFFFFFFFFU, 2U) == 0U);
	CHECK(cpux86_field32(0xFFFFFFFFU, 1U, 0xFFFFFFFFU) == 0U);

	/* A 64-bit field at both ends and in the middle. */
	pattern = ((uint64_t)0x08040439U << 32) | 0xF0811900U;
	CHECK(cpux86_field64(pattern, 0U, 64U) == pattern);
	CHECK(cpux86_field64(pattern, 8U, 8U) == 0x19U);
	CHECK(cpux86_field64(pattern, 40U, 8U) == 0x04U);
	CHECK(cpux86_field64(pattern, 56U, 8U) == 0x08U);
	CHECK(cpux86_field64((uint64_t)1U << 63, 63U, 1U) == 1U);
	CHECK(cpux86_field64(~(uint64_t)0U, 32U, 32U) == 0xFFFFFFFFU);

	/* A 64-bit field the register does not have reads as 0. */
	CHECK(cpux86_field64(~(uint64_t)0U, 0U, 0U) == 0U);
	CHECK(cpux86_field64(~(uint64_t)0U, 0U, 65U) == 0U);
	CHECK(cpux86_field64(~(uint64_t)0U, 64U, 1U) == 0U);
	CHECK(cpux86_field64(~(uint64_t)0U, 63U, 2U) == 0U);
	CHECK(cpux86_field64(~(uint64_t)0U, 0xFFFFFFFFU, 1U) == 0U);

	/* Replacing a field keeps every other bit and drops the extra high bits. */
	CHECK(cpux86_insert64(0U, 8U, 8U, 0x19U) == 0x1900U);
	CHECK(cpux86_insert64(~(uint64_t)0U, 8U, 8U, 0U) == ~(uint64_t)0xFF00U);
	CHECK(cpux86_insert64(0U, 8U, 4U, 0xABU) == 0xB00U);
	CHECK(cpux86_insert64(0U, 63U, 1U, 1U) == (uint64_t)1U << 63);
	CHECK(cpux86_insert64(0x1234U, 0U, 64U, 0x5678U) == 0x5678U);
	CHECK(cpux86_insert64(0U, 32U, 10U, 0x3FFU) == (uint64_t)0x3FFU << 32);

	/* A field the register does not have leaves it unchanged. */
	CHECK(cpux86_insert64(0x1234U, 0U, 0U, 1U) == 0x1234U);
	CHECK(cpux86_insert64(0x1234U, 64U, 1U, 1U) == 0x1234U);
	CHECK(cpux86_insert64(0x1234U, 60U, 5U, 1U) == 0x1234U);
	CHECK(cpux86_insert64(0x1234U, 0U, 65U, 1U) == 0x1234U);

	/* Every in-range position round-trips through insert and extract. */
	for (low = 0U; low < 64U; low++) {
		for (width = 1U; width <= 64U - low; width++) {
			pattern = cpux86_insert64(0U, low, width, ~(uint64_t)0U);
			CHECK(cpux86_field64(pattern, low, width) == cpux86_field64(~(uint64_t)0U, 0U, width));
			if (low + width < 64U)
				CHECK(cpux86_field64(pattern, low + width, 64U - low - width) == 0U);
		}
	}

	/* The width that tells count instances apart. */
	CHECK(cpux86_ceil_log2(0U) == 0U);
	CHECK(cpux86_ceil_log2(1U) == 0U);
	CHECK(cpux86_ceil_log2(2U) == 1U);
	CHECK(cpux86_ceil_log2(3U) == 2U);
	CHECK(cpux86_ceil_log2(4U) == 2U);
	CHECK(cpux86_ceil_log2(5U) == 3U);
	CHECK(cpux86_ceil_log2(4096U) == 12U);
	CHECK(cpux86_ceil_log2(4097U) == 13U);
	CHECK(cpux86_ceil_log2((uint32_t)1U << 31) == 31U);
	CHECK(cpux86_ceil_log2(((uint32_t)1U << 31) + 1U) == 32U);
	CHECK(cpux86_ceil_log2(0xFFFFFFFFU) == 32U);

	/* Reports the summary line. */
	return check_finish("cpux86");
}
