/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The 32-bit division helpers of the ARM EABI, for the kernel.
 *
 * The Cortex-A9 has no divide instruction in the ARM instruction set, so
 * the compiler calls these for every 32-bit division and remainder.  The
 * division-and-remainder forms return the quotient in r0 and the
 * remainder in r1, which is how the calling convention returns a 64-bit
 * value whose low word is the quotient.
 *
 * The object is built without link-time optimization, as the other
 * compiler helpers are, and without the compiler's own built-in calls: a
 * division written here would call itself.  It uses nothing of the HAL
 * but __aeabi_idiv0, so the host test builds it as it is.
 *
 * Values referenced: ARM IHI 0043 (Run-time ABI for the ARM Architecture),
 * the names and results of __aeabi_uidiv, __aeabi_uidivmod, __aeabi_idiv,
 * __aeabi_idivmod and __aeabi_idiv0.
 */

#include <stdint.h>

/* The sign bit of a 32-bit two's-complement word. */
#define AEABI_SIGN_BIT	0x80000000U

void __aeabi_idiv0(void);
uint32_t __aeabi_uidiv(uint32_t numerator, uint32_t denominator);
uint64_t __aeabi_uidivmod(uint32_t numerator, uint32_t denominator);
int32_t __aeabi_idiv(int32_t numerator, int32_t denominator);
uint64_t __aeabi_idivmod(int32_t numerator, int32_t denominator);

static uint32_t unsigned_divide(uint32_t numerator, uint32_t denominator, uint32_t *remainder);
static uint32_t magnitude(int32_t value);
static uint64_t pack(uint32_t quotient, uint32_t remainder);

/*
 * Divides two unsigned words and reports the quotient.
 */
uint32_t
__aeabi_uidiv(
	uint32_t numerator,
	uint32_t denominator)
{
	uint32_t quotient;
	uint32_t remainder;

	/* The long division keeps the remainder, which this form drops. */
	quotient = unsigned_divide(numerator, denominator, &remainder);

	/* Succeeded: the quotient. */
	return quotient;
}

/*
 * Divides two unsigned words and reports the quotient in the low word and
 * the remainder in the high word, which are r0 and r1.
 */
uint64_t
__aeabi_uidivmod(
	uint32_t numerator,
	uint32_t denominator)
{
	uint32_t quotient;
	uint32_t remainder;
	uint64_t both;

	/* Divides once for both results. */
	quotient = unsigned_divide(numerator, denominator, &remainder);
	both = pack(quotient, remainder);

	/* Succeeded: the quotient in r0 and the remainder in r1. */
	return both;
}

/*
 * Divides two signed words and reports the quotient, rounded toward zero.
 */
int32_t
__aeabi_idiv(
	int32_t numerator,
	int32_t denominator)
{
	uint64_t both;
	uint32_t quotient;

	/* The quotient is the low word of the division with remainder. */
	both = __aeabi_idivmod(numerator, denominator);
	quotient = (uint32_t)both;

	/* Succeeded: the quotient. */
	return (int32_t)quotient;
}

/*
 * Divides two signed words and reports the quotient, rounded toward zero,
 * in the low word, and the remainder, which has the dividend's sign, in
 * the high word.
 */
uint64_t
__aeabi_idivmod(
	int32_t numerator,
	int32_t denominator)
{
	uint32_t quotient;
	uint32_t remainder;
	uint32_t numerator_magnitude;
	uint32_t denominator_magnitude;
	uint64_t both;
	int negative_quotient;

	/* Divides the magnitudes; the signs decide the results' signs afterwards. */
	numerator_magnitude = magnitude(numerator);
	denominator_magnitude = magnitude(denominator);
	quotient = unsigned_divide(numerator_magnitude, denominator_magnitude, &remainder);

	/* The quotient is negative when exactly one operand is. */
	negative_quotient = 0;
	if (numerator < 0)
		negative_quotient = !negative_quotient;
	if (denominator < 0)
		negative_quotient = !negative_quotient;
	if (negative_quotient)
		quotient = 0U - quotient;

	/* The remainder takes the dividend's sign. */
	if (numerator < 0)
		remainder = 0U - remainder;
	both = pack(quotient, remainder);

	/* Succeeded: the quotient in r0 and the remainder in r1. */
	return both;
}

/*
 * Divides by shifting and subtracting, one quotient bit at a time from
 * the top.  A zero divisor is reported through __aeabi_idiv0; if that
 * returns, the quotient is all ones and the remainder the dividend.
 */
static uint32_t
unsigned_divide(
	uint32_t numerator,
	uint32_t denominator,
	uint32_t *remainder)
{
	uint32_t quotient;
	uint32_t rest;
	int bit;

	/* A zero divisor is the run-time ABI's to report. */
	if (denominator == 0U) {
		__aeabi_idiv0();
		*remainder = numerator;
		return ~(uint32_t)0;
	}

	/* Brings the dividend's bits down one at a time into the rest. */
	quotient = 0;
	rest = 0;
	for (bit = 31; bit >= 0; bit--) {
		rest = (rest << 1) | ((numerator >> bit) & 1U);
		quotient <<= 1;
		if (rest >= denominator) {
			rest -= denominator;
			quotient |= 1U;
		}
	}

	/* Succeeded: the quotient, with the remainder left in the rest. */
	*remainder = rest;
	return quotient;
}

/* Reports the magnitude of a signed word, which the most negative one has too. */
static uint32_t
magnitude(
	int32_t value)
{
	uint32_t bits;

	/* A negative value's magnitude is its two's complement. */
	bits = (uint32_t)value;
	if ((bits & AEABI_SIGN_BIT) != 0U)
		return 0U - bits;

	/* A positive value is its own magnitude. */
	return bits;
}

/* Puts the quotient in the low word and the remainder in the high word. */
static uint64_t
pack(
	uint32_t quotient,
	uint32_t remainder)
{
	/* The low word is returned in r0 and the high word in r1. */
	return ((uint64_t)remainder << 32) | quotient;
}
