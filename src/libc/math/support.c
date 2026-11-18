/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Error reporting and exact packing shared by the mathematical library.
 *
 * math_errhandling promises both errno and the exception flags, so every
 * error path goes through one of these helpers and reports both.
 */

#include <errno.h>
#include <fenv.h>
#include <math.h>
#include <stdint.h>

#include "src/libc/math/math-internal.h"

/* The encoding of the default quiet NaN. */
#define LIBM_QUIET_NAN UINT64_C(0x7ff8000000000000)

/* The magnitude of the smallest normal binary64 value. */
#define LIBM_DOUBLE_MIN_NORMAL UINT64_C(0x0010000000000000)

/* The magnitude of the smallest normal binary32 value. */
#define LIBM_FLOAT_MIN_NORMAL UINT32_C(0x00800000)

/*
 * Reports a domain error and returns the quiet NaN that stands for it.
 */
double
__libm_invalid(void)
{
	/* Records the domain error in both places math_errhandling names. */
	errno = EDOM;
	(void)feraiseexcept(FE_INVALID);

	/* The result of a domain error is a quiet NaN. */
	return libm_from_bits(LIBM_QUIET_NAN);
}

/*
 * Reports a pole and returns the infinity of the given sign.
 */
double
__libm_pole(
	unsigned int sign)
{
	/* An exact infinite result from finite input is a division by zero. */
	errno = ERANGE;
	(void)feraiseexcept(FE_DIVBYZERO);

	/* Builds the infinity whose sign the caller chose. */
	return libm_from_bits(((uint64_t)sign << 63) | LIBM_DOUBLE_INFINITY);
}

/*
 * Reports an overflow and returns the infinity of the given sign.
 */
double
__libm_overflow(
	unsigned int sign)
{
	/* A finite result too large to represent rounds to infinity. */
	errno = ERANGE;
	(void)feraiseexcept(FE_OVERFLOW | FE_INEXACT);

	/* Round-to-nearest carries an overflow all the way to infinity. */
	return libm_from_bits(((uint64_t)sign << 63) | LIBM_DOUBLE_INFINITY);
}

/*
 * Reports an underflow and returns the zero of the given sign.
 */
double
__libm_underflow(
	unsigned int sign)
{
	/* A nonzero result below half the smallest subnormal rounds to zero. */
	errno = ERANGE;
	(void)feraiseexcept(FE_UNDERFLOW | FE_INEXACT);

	/* Builds the zero whose sign the caller chose. */
	return libm_from_bits((uint64_t)sign << 63);
}

/*
 * Reports an underflow when an inexact result is tiny.
 *
 * The caller has already rounded the result and knows it is inexact; a
 * subnormal or zero result is then an underflow.
 */
double
__libm_check_underflow(
	double value)
{
	uint64_t magnitude;

	/* A normal result is not tiny. */
	magnitude = libm_bits(value) & LIBM_DOUBLE_MAGNITUDE;
	if (magnitude >= LIBM_DOUBLE_MIN_NORMAL)
		return value;

	/* Records the range error. */
	errno = ERANGE;
	(void)feraiseexcept(FE_UNDERFLOW | FE_INEXACT);

	/* The rounded value itself is the result. */
	return value;
}

/*
 * Rounds an inexact binary64 result to binary32 and reports range errors.
 *
 * The functions of float compute in double and round once here, so the
 * range of float is only checked at this point.
 */
float
__libm_narrow(
	double value)
{
	float narrowed;
	uint32_t magnitude;
	uint64_t source_magnitude;

	/* Rounds to the nearest float. */
	narrowed = (float)value;
	magnitude = libm_float_bits(narrowed) & LIBM_FLOAT_MAGNITUDE;
	source_magnitude = libm_bits(value) & LIBM_DOUBLE_MAGNITUDE;

	/* A finite value that became infinite overflowed the float range. */
	if (magnitude == LIBM_FLOAT_INFINITY &&
	    source_magnitude < LIBM_DOUBLE_INFINITY) {
		errno = ERANGE;
		(void)feraiseexcept(FE_OVERFLOW | FE_INEXACT);
		return narrowed;
	}

	/* A normal float is inside the range. */
	if (magnitude >= LIBM_FLOAT_MIN_NORMAL)
		return narrowed;

	/* A tiny float that lost bits underflowed. */
	if ((double)narrowed != value) {
		errno = ERANGE;
		(void)feraiseexcept(FE_UNDERFLOW | FE_INEXACT);
	}

	/* Succeeded: the rounded float is the result. */
	return narrowed;
}

/*
 * Rounds sign * significand * 2^(exponent - 63) to the nearest binary64.
 *
 * The significand must have its bit 63 set; its low bits carry everything
 * the caller computed below the result's precision, with any further lost
 * bits folded into bit 0 as a sticky bit.  Ties go to the even value, a
 * result past the range overflows, and an inexact tiny result underflows.
 */
double
__libm_pack(
	unsigned int sign,
	int exponent,
	uint64_t significand)
{
	int biased;
	unsigned int shift;
	uint64_t kept;
	uint64_t dropped;
	uint64_t half;
	uint64_t field;
	uint64_t bits;
	double result;

	/*
	 * A normal result keeps the top 53 bits.  A subnormal one keeps fewer,
	 * because its last bit is always worth 2^-1074.
	 */
	biased = exponent + LIBM_DOUBLE_BIAS;
	shift = 11U;

	/* A leading bit past the largest finite binade overflows before rounding. */
	if (biased > 2046)
		return __libm_overflow(sign);
	if (biased < 1) {
		/*
		 * Past this distance the value is below 2^-1075, half the
		 * smallest subnormal, and rounds to zero.
		 */
		if (biased < -52)
			return __libm_underflow(sign);

		/* Each binade below the normal range drops one more bit. */
		shift = (unsigned int)(11 + 1 - biased);
	}

	/* Splits the significand into the kept bits and the dropped ones. */
	if (shift == 64U) {
		kept = 0U;
		dropped = significand;
	} else {
		kept = significand >> shift;
		dropped = significand & ((UINT64_C(1) << shift) - 1U);
	}

	/* Rounds to nearest, sending a tie to the even neighbour. */
	half = UINT64_C(1) << (shift - 1U);
	if (dropped > half) {
		kept++;
	} else if (dropped == half && (kept & 1U) != 0U) {
		kept++;
	}

	/*
	 * The exponent field is one less than the biased exponent because the
	 * leading bit of the kept significand adds one to it.  A carry out of
	 * rounding therefore moves the exponent up by itself, and a subnormal
	 * that rounds up to 2^-1022 becomes the smallest normal.
	 */
	field = 0U;
	if (biased >= 1)
		field = (uint64_t)(biased - 1);
	bits = (field << 52) + kept;

	/* An exponent past the largest finite binade is an overflow. */
	if (bits >= LIBM_DOUBLE_INFINITY)
		return __libm_overflow(sign);

	/* Assembles the rounded value. */
	result = libm_from_bits(bits | ((uint64_t)sign << 63));

	/* An exact result raises nothing. */
	if (dropped == 0U)
		return result;

	/* A tiny inexact result underflowed. */
	if (biased < 1) {
		errno = ERANGE;
		(void)feraiseexcept(FE_UNDERFLOW | FE_INEXACT);
		return result;
	}

	/* Succeeded: the result is inexact but inside the range. */
	(void)feraiseexcept(FE_INEXACT);
	return result;
}

/*
 * Splits a finite nonzero binary64 value into sign, exponent and a
 * normalized 53-bit significand.
 */
void
__libm_unpack(
	double value,
	struct libm_unpacked *unpacked)
{
	uint64_t bits;
	int field;
	uint64_t fraction;

	/* Separates the three fields. */
	bits = libm_bits(value);
	unpacked->sign = (unsigned int)(bits >> 63);
	field = (int)((bits >> 52) & 0x7ffU);
	fraction = bits & LIBM_DOUBLE_FRACTION;

	/* A normal value has the hidden bit and needs no shifting. */
	if (field != 0) {
		unpacked->significand = fraction | LIBM_DOUBLE_HIDDEN;
		unpacked->exponent = field - LIBM_DOUBLE_BIAS - 52;
		return;
	}

	/* A subnormal value is shifted up until its leading bit is the hidden one. */
	unpacked->exponent = 1 - LIBM_DOUBLE_BIAS - 52;
	while ((fraction & LIBM_DOUBLE_HIDDEN) == 0U) {
		fraction <<= 1;
		unpacked->exponent--;
	}

	/* The shifted fraction now carries its leading bit where the hidden bit is. */
	unpacked->significand = fraction;
}

/*
 * Returns (value.high + value.low) * 2^exponent rounded once.
 *
 * The value is a nonzero double-double near one, the result of a kernel
 * that works on a reduced argument.  A normal result is the rounded pair
 * with its exponent moved, which is exact.  A subnormal result has fewer
 * bits, and rounding the pair first would round twice; instead the pair
 * is lifted into [0, 1) and added to one, whose binade has the same last
 * bit as the subnormal range, so the one rounding lands on the right grid.
 */
double
__libm_scale(
	struct libm_dd value,
	int exponent)
{
	struct libm_dd lifted;
	struct libm_dd sum;
	double rounded;
	double factor;
	double anchor;
	double result;
	uint64_t bits;
	unsigned int sign;
	int binade;

	/* Rounds the pair and reads the binade of the rounded value. */
	rounded = value.high + value.low;
	bits = libm_bits(rounded);
	sign = (unsigned int)(bits >> 63);
	binade = (int)((bits >> 52) & 0x7ffU) - LIBM_DOUBLE_BIAS;

	/* Past the largest finite binade the result overflows. */
	if (binade + exponent > LIBM_DOUBLE_BIAS)
		return __libm_overflow(sign);

	/* Inside the normal range moving the exponent is exact. */
	if (binade + exponent >= 1 - LIBM_DOUBLE_BIAS)
		return libm_from_bits(bits + ((uint64_t)exponent << 52));

	/* Far below the subnormal range the result is zero. */
	if (binade + exponent < -1080)
		return __libm_underflow(sign);

	/* Lifts the pair by 2^(exponent + 1022), which keeps both parts exact. */
	factor = libm_from_bits((uint64_t)(exponent + 1022 + LIBM_DOUBLE_BIAS) << 52);
	lifted.high = value.high * factor;
	lifted.low = value.low * factor;

	/*
	 * Adds the lifted pair to one of its sign.  The sum lies in [1, 2),
	 * whose last bit is worth 2^-52, as 2^-1074 is in the subnormal range
	 * lifted by 2^1022; the addition of the low parts rounds once.
	 */
	anchor = 1.0;
	if (sign != 0U)
		anchor = -1.0;
	sum = libm_two_sum(anchor, lifted.high);
	sum.low += lifted.low;
	rounded = sum.high + sum.low;

	/*
	 * Takes the one away again, exactly, and lowers the result back.  A
	 * result that rounded to zero keeps the sign of the value.
	 */
	result = (rounded - anchor) * 2.2250738585072014e-308;
	if (result == 0.0)
		result = libm_from_bits((uint64_t)sign << 63);

	/* A sum that needed no rounding is an exact subnormal. */
	if (sum.low == 0.0)
		return result;

	/* An inexact tiny result is an underflow. */
	result = __libm_check_underflow(result);

	/* Succeeded: the subnormal or zero result. */
	return result;
}
