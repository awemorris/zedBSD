/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Fused multiply-add.
 *
 * fma computes x * y + z exactly in integers and rounds once.  The 106-bit
 * product and the addend are placed in one 128-bit frame where neither
 * loses a bit unless it is so much smaller than the other that only a
 * sticky bit of it can matter, and the sum is handed to the packing helper
 * with everything below 64 bits folded into its sticky bit.
 */

#include <math.h>
#include <stdint.h>

#include "src/libc/math/math-internal.h"

/*
 * A 128-bit unsigned integer as two halves.
 */
struct libm_u128 {
	uint64_t high;
	uint64_t low;
};

static void libm_multiply(uint64_t left, uint64_t right, struct libm_u128 *product);
static void libm_shift_left(struct libm_u128 *value, unsigned int distance);
static void libm_shift_right_jam(struct libm_u128 *value, unsigned int distance);
static int libm_compare(const struct libm_u128 *left, const struct libm_u128 *right);
static void libm_add(struct libm_u128 *sum, const struct libm_u128 *addend);
static void libm_subtract(struct libm_u128 *difference, const struct libm_u128 *subtrahend);
static double libm_round_u128(unsigned int sign, const struct libm_u128 *value, int exponent);
static double libm_fma_special(double x, double y, double z);

/*
 * Returns x * y + z rounded once.
 */
double
fma(
	double x,
	double y,
	double z)
{
	struct libm_unpacked left;
	struct libm_unpacked right;
	struct libm_unpacked addend;
	struct libm_u128 product;
	struct libm_u128 term;
	unsigned int product_sign;
	unsigned int sign;
	int product_unit;
	int term_unit;
	int unit;
	int order;
	uint64_t x_magnitude;
	uint64_t y_magnitude;
	uint64_t z_magnitude;
	double result;

	/*
	 * Infinities and NaNs have rules of their own.  A zero factor makes
	 * the product an exact zero, so the ordinary arithmetic rounds once.
	 */
	x_magnitude = libm_bits(x) & LIBM_DOUBLE_MAGNITUDE;
	y_magnitude = libm_bits(y) & LIBM_DOUBLE_MAGNITUDE;
	z_magnitude = libm_bits(z) & LIBM_DOUBLE_MAGNITUDE;
	if (x_magnitude >= LIBM_DOUBLE_INFINITY ||
	    y_magnitude >= LIBM_DOUBLE_INFINITY ||
	    z_magnitude >= LIBM_DOUBLE_INFINITY)
		return libm_fma_special(x, y, z);
	if (x_magnitude == 0U || y_magnitude == 0U)
		return x * y + z;

	/*
	 * The exact product of two 53-bit significands has 105 or 106 bits.
	 * Shifting it up by 20 puts it in [2^124, 2^126), leaving room for a
	 * carry out of the addition below.
	 */
	__libm_unpack(x, &left);
	__libm_unpack(y, &right);
	product_sign = left.sign ^ right.sign;
	libm_multiply(left.significand, right.significand, &product);
	libm_shift_left(&product, 20U);
	product_unit = left.exponent + right.exponent - 20;

	/* A zero addend leaves the product to be rounded alone. */
	if (z_magnitude == 0U) {
		result = libm_round_u128(product_sign, &product, product_unit);
		return result;
	}

	/* The addend is placed in the same range: 53 bits shifted up by 73. */
	__libm_unpack(z, &addend);
	term.high = addend.significand << 9;
	term.low = 0U;
	term_unit = addend.exponent - 73;

	/*
	 * Aligns the operand with the smaller unit to the larger one.  Each
	 * operand has at least 20 zero bits at the bottom, so an alignment
	 * that drops bits only happens when that operand is far below the
	 * other, and then the sticky bit is all that is needed of it.
	 */
	if (product_unit >= term_unit) {
		libm_shift_right_jam(&term, (unsigned int)(product_unit - term_unit));
		unit = product_unit;
	} else {
		libm_shift_right_jam(&product, (unsigned int)(term_unit - product_unit));
		unit = term_unit;
	}

	/* Operands of the same sign add; the sum stays below 2^127. */
	if (product_sign == addend.sign) {
		libm_add(&product, &term);
		result = libm_round_u128(product_sign, &product, unit);
		return result;
	}

	/* Operands of opposite signs subtract the smaller from the larger. */
	order = libm_compare(&product, &term);
	if (order == 0)
		return 0.0;
	if (order > 0) {
		libm_subtract(&product, &term);
		sign = product_sign;
		result = libm_round_u128(sign, &product, unit);
		return result;
	}

	/* The addend is the larger and gives the sign. */
	libm_subtract(&term, &product);
	sign = addend.sign;
	result = libm_round_u128(sign, &term, unit);

	/* Succeeded: the sum was rounded once. */
	return result;
}

/*
 * Returns x * y + z rounded once to binary32.
 *
 * The product of two floats is exact in double.  Adding z in double
 * rounds, so the sum is rounded to odd instead: when the double sum is
 * inexact and its last bit is even, it is moved one step towards the exact
 * sum.  A double rounded to odd has enough bits past the float precision
 * that rounding it to float gives the correctly rounded result (Boldo and
 * Melquiond, 2008).
 */
float
fmaf(
	float x,
	float y,
	float z)
{
	double product;
	double sum;
	double sum_error;
	double partial;
	uint64_t bits;
	uint64_t magnitude;
	float narrowed;

	/*
	 * The product of two floats is exact in double.  A product that is
	 * zero, infinite or NaN, or a zero addend, leaves one rounding in the
	 * double sum below, which is exact there, and the narrowing is the
	 * only rounding left.
	 */
	product = (double)x * (double)y;
	magnitude = libm_bits(product) & LIBM_DOUBLE_MAGNITUDE;
	if (magnitude >= LIBM_DOUBLE_INFINITY || magnitude == 0U || z == 0.0f)
		return __libm_narrow(product + (double)z);

	/* Adds z and recovers the rounding error of the addition (TwoSum). */
	sum = product + (double)z;
	partial = sum - product;
	sum_error = (product - (sum - partial)) + ((double)z - partial);

	/* An exact sum or one already odd needs no adjustment. */
	bits = libm_bits(sum);
	if (sum_error == 0.0 || (bits & 1U) != 0U)
		return __libm_narrow(sum);

	/*
	 * Steps the even sum one unit towards the exact value, which is on
	 * the side the error points to.  Moving the magnitude up adds to the
	 * encoding and moving it down subtracts.
	 */
	if ((sum_error > 0.0) == (sum > 0.0)) {
		bits++;
	} else {
		bits--;
	}

	/* Rounds to float, which also reports the float range. */
	narrowed = __libm_narrow(libm_from_bits(bits));

	/* Succeeded: the float result. */
	return narrowed;
}

/*
 * Handles an infinite or NaN operand of fma.
 */
static double
libm_fma_special(
	double x,
	double y,
	double z)
{
	uint64_t x_magnitude;
	uint64_t y_magnitude;
	uint64_t z_magnitude;
	uint64_t product_sign;
	uint64_t z_sign;

	/* A NaN operand, unequal to itself, is passed on. */
	if (x != x || y != y || z != z)
		return x * y + z;

	/* A finite product added to an infinity is that infinity. */
	x_magnitude = libm_bits(x) & LIBM_DOUBLE_MAGNITUDE;
	y_magnitude = libm_bits(y) & LIBM_DOUBLE_MAGNITUDE;
	z_magnitude = libm_bits(z) & LIBM_DOUBLE_MAGNITUDE;
	if (x_magnitude < LIBM_DOUBLE_INFINITY && y_magnitude < LIBM_DOUBLE_INFINITY)
		return z;

	/* Zero times infinity is invalid. */
	if (x_magnitude == 0U || y_magnitude == 0U)
		return __libm_invalid();

	/* An infinite product meets an infinite addend of the other sign. */
	product_sign = (libm_bits(x) ^ libm_bits(y)) & LIBM_DOUBLE_SIGN;
	z_sign = libm_bits(z) & LIBM_DOUBLE_SIGN;
	if (z_magnitude == LIBM_DOUBLE_INFINITY && z_sign != product_sign)
		return __libm_invalid();

	/* Succeeded: the infinite product is the result. */
	return x * y;
}

/*
 * Rounds sign * value * 2^exponent to binary64.
 */
static double
libm_round_u128(
	unsigned int sign,
	const struct libm_u128 *value,
	int exponent)
{
	struct libm_u128 normalized;
	uint64_t significand;
	int shift;
	double result;

	/* Moves the leading bit to bit 127. */
	normalized = *value;
	shift = 0;
	while ((normalized.high >> 63) == 0U) {
		libm_shift_left(&normalized, 1U);
		shift++;
	}

	/* The high half becomes the significand, the low half its sticky bit. */
	significand = normalized.high;
	if (normalized.low != 0U)
		significand |= 1U;

	/* The leading bit is worth 2^(exponent + 127 - shift). */
	result = __libm_pack(sign, exponent + 127 - shift, significand);

	/* Succeeded: the packing helper rounded once. */
	return result;
}

/*
 * Multiplies two 64-bit integers into a 128-bit product.
 *
 * The factors are split into 32-bit halves so that each partial product
 * fits in 64 bits on every target, 32-bit ones included.
 */
static void
libm_multiply(
	uint64_t left,
	uint64_t right,
	struct libm_u128 *product)
{
	uint64_t left_high;
	uint64_t left_low;
	uint64_t right_high;
	uint64_t right_low;
	uint64_t low_low;
	uint64_t low_high;
	uint64_t high_low;
	uint64_t high_high;
	uint64_t middle;

	/* Splits both factors into halves. */
	left_high = left >> 32;
	left_low = left & UINT64_C(0xffffffff);
	right_high = right >> 32;
	right_low = right & UINT64_C(0xffffffff);

	/* Forms the four partial products. */
	low_low = left_low * right_low;
	low_high = left_low * right_high;
	high_low = left_high * right_low;
	high_high = left_high * right_high;

	/* Sums the middle column with the carry out of the low one. */
	middle = (low_low >> 32) + (low_high & UINT64_C(0xffffffff)) +
	    (high_low & UINT64_C(0xffffffff));

	/* Assembles the halves. */
	product->low = (middle << 32) | (low_low & UINT64_C(0xffffffff));
	product->high = high_high + (low_high >> 32) + (high_low >> 32) +
	    (middle >> 32);
}

/*
 * Shifts a 128-bit value left by less than 128 bits.
 */
static void
libm_shift_left(
	struct libm_u128 *value,
	unsigned int distance)
{
	/* No shift leaves the value as it is. */
	if (distance == 0U)
		return;

	/* A shift of a half or more moves the low half into the high one. */
	if (distance >= 64U) {
		value->high = value->low << (distance - 64U);
		value->low = 0U;
		return;
	}

	/* A short shift carries the top bits of the low half upwards. */
	value->high = (value->high << distance) | (value->low >> (64U - distance));
	value->low <<= distance;
}

/*
 * Shifts a 128-bit value right, folding every lost bit into bit 0.
 */
static void
libm_shift_right_jam(
	struct libm_u128 *value,
	unsigned int distance)
{
	uint64_t lost;

	/* No shift loses nothing. */
	if (distance == 0U)
		return;

	/* A shift past the whole value leaves only the sticky bit. */
	if (distance >= 128U) {
		lost = value->high | value->low;
		value->high = 0U;
		value->low = 0U;
		if (lost != 0U)
			value->low = 1U;

		/* The value has been reduced to its sticky bit. */
		return;
	}

	/* A shift of a half or more drops the whole low half. */
	if (distance >= 64U) {
		lost = value->low;
		if (distance > 64U)
			lost |= value->high << (128U - distance);
		value->low = value->high >> (distance - 64U);
		value->high = 0U;
		if (lost != 0U)
			value->low |= 1U;

		/* The high half has become the low one. */
		return;
	}

	/* A short shift drops the bottom bits of the low half. */
	lost = value->low << (64U - distance);
	value->low = (value->low >> distance) | (value->high << (64U - distance));
	value->high >>= distance;
	if (lost != 0U)
		value->low |= 1U;
}

/*
 * Orders two 128-bit values: negative, zero or positive.
 */
static int
libm_compare(
	const struct libm_u128 *left,
	const struct libm_u128 *right)
{
	/* The high halves decide unless they are equal. */
	if (left->high != right->high) {
		if (left->high > right->high)
			return 1;

		/* The right high half is the larger. */
		return -1;
	}

	/* Equal high halves leave the decision to the low halves. */
	if (left->low > right->low)
		return 1;
	if (left->low < right->low)
		return -1;

	/* Succeeded: the values are equal. */
	return 0;
}

/*
 * Adds one 128-bit value to another.
 */
static void
libm_add(
	struct libm_u128 *sum,
	const struct libm_u128 *addend)
{
	uint64_t low;

	/* Adds the low halves and carries a wrap-around into the high one. */
	low = sum->low + addend->low;
	sum->high += addend->high;
	if (low < sum->low)
		sum->high++;
	sum->low = low;
}

/*
 * Subtracts a smaller 128-bit value from a larger one.
 */
static void
libm_subtract(
	struct libm_u128 *difference,
	const struct libm_u128 *subtrahend)
{
	/* Borrows from the high half when the low half wraps around. */
	if (difference->low < subtrahend->low)
		difference->high--;
	difference->low -= subtrahend->low;
	difference->high -= subtrahend->high;
}
