/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The trigonometric functions: sin, cos and tan.
 *
 * x is reduced to k pi/2 + r with |r| <= pi/4, r a double-double.  Below
 * 2^19 the reduction takes k times pi/2 away in four pieces (Cody and
 * Waite), the first three short enough that their products with k are
 * exact.  Above it, x times 2/pi is formed in integers from the bits of
 * 2/pi that matter to x's exponent (Payne and Hanek): the bits above them
 * only add multiples of four quarter turns, and 192 bits below them leave
 * over a hundred bits of r even when x is as close to a multiple of pi/2 as
 * a double can be.
 *
 * On r, sin and cos come from a table of sin(i/64) and cos(i/64) and the
 * Taylor series of sin d and cos d - 1 for d = r - i/64, |d| <= 1/128,
 * all in double-double; tan divides the two.  The result is rounded once.
 */

#include <math.h>
#include <stdint.h>

#include "src/libc/math/math-internal.h"

/* The encoding of pi/4: below it no reduction is needed. */
#define LIBM_QUARTER_PI_BITS UINT64_C(0x3fe921fb54442d18)

/* The encoding of 2^19: below it the reduction of Cody and Waite is exact. */
#define LIBM_MEDIUM_BITS UINT64_C(0x4120000000000000)

/* The encoding of 2^-27: below it sin x and tan x round to x, cos x to 1. */
#define LIBM_TRIG_TINY UINT64_C(0x3e40000000000000)

/* The number of 32-bit limbs of the window of 2/pi used for a large x. */
#define LIBM_WINDOW_LIMBS 6

/* The number of 32-bit limbs of the product of x and that window. */
#define LIBM_PRODUCT_LIMBS 8

/*
 * The three trigonometric functions this file computes.
 */
enum libm_trig_function {
	LIBM_SINE,
	LIBM_COSINE,
	LIBM_TANGENT
};

static int libm_reduce(double x, struct libm_dd *reduced);
static int libm_reduce_large(double magnitude, struct libm_dd *reduced);
static uint32_t libm_two_over_pi_word(int first_bit);
static uint64_t libm_product_bits(const uint32_t *limbs, int top);
static double libm_trig(double x, enum libm_trig_function function);
static double libm_power_of_two(int exponent);

/*
 * Returns the sine of x, in radians.
 */
double
sin(
	double x)
{
	double result;

	/* Evaluates the shared reduction and kernel. */
	result = libm_trig(x, LIBM_SINE);

	/* Succeeded: sin x rounded once. */
	return result;
}

/*
 * Returns the cosine of x, in radians.
 */
double
cos(
	double x)
{
	double result;

	/* Evaluates the shared reduction and kernel. */
	result = libm_trig(x, LIBM_COSINE);

	/* Succeeded: cos x rounded once. */
	return result;
}

/*
 * Returns the tangent of x, in radians.
 */
double
tan(
	double x)
{
	double result;

	/* Evaluates the shared reduction and kernel. */
	result = libm_trig(x, LIBM_TANGENT);

	/* Succeeded: tan x rounded once. */
	return result;
}

/*
 * Returns the sine of x in binary32.
 */
float
sinf(
	float x)
{
	double value;

	/* Computes in double. */
	value = sin((double)x);

	/* Succeeded: a sine is inside the float range. */
	return (float)value;
}

/*
 * Returns the cosine of x in binary32.
 */
float
cosf(
	float x)
{
	double value;

	/* Computes in double. */
	value = cos((double)x);

	/* Succeeded: a cosine is inside the float range. */
	return (float)value;
}

/*
 * Returns the tangent of x in binary32.
 */
float
tanf(
	float x)
{
	double value;
	float narrowed;

	/* Computes in double. */
	value = tan((double)x);

	/* Rounds to float, which also reports the float range. */
	narrowed = __libm_narrow(value);

	/* Succeeded: the float result. */
	return narrowed;
}

/*
 * Computes sin r and cos r as double-doubles for |r| <= about pi/4.
 *
 * r = i/64 + d with |d| <= 1/128, and
 *   sin r = sin(i/64) + (sin(i/64) (cos d - 1) + cos(i/64) sin d),
 *   cos r = cos(i/64) + (cos(i/64) (cos d - 1) - sin(i/64) sin d).
 * sin d is d + d^3 (-1/6 + ... + d^6/9!) and cos d - 1 is
 * -d^2/2 + d^4 (1/24 - ... + d^4/8!); the first terms left out are below
 * 2^-100 of the result.
 */
void
__libm_sin_cos_dd(
	struct libm_dd reduced,
	struct libm_dd *sine,
	struct libm_dd *cosine)
{
	struct libm_dd magnitude;
	struct libm_dd offset;
	struct libm_dd square;
	struct libm_dd sin_offset;
	struct libm_dd cos_offset;
	struct libm_dd table_sine;
	struct libm_dd table_cosine;
	struct libm_dd first;
	struct libm_dd second;
	const double *coefficients;
	double square_high;
	double tail;
	int negative;
	int index;

	/* Works on |r|; sin is odd and cos even. */
	magnitude = reduced;
	negative = reduced.high < 0.0;
	if (negative) {
		magnitude.high = -reduced.high;
		magnitude.low = -reduced.low;
	}

	/* The nearest table point, and the exact offset from it. */
	index = (int)(magnitude.high * 64.0 + 0.5);
	offset = libm_two_sum(magnitude.high - (double)index * 0.015625, magnitude.low);

	/* Squares the offset. */
	square = libm_two_product(offset.high, offset.high);
	square.low += 2.0 * offset.high * offset.low;
	square_high = square.high;

	/* sin d = d + d^3 (-1/6 + d^2/5! - d^4/7! + d^6/9!). */
	coefficients = __libm_sin_coefficients;
	tail = coefficients[3];
	tail = coefficients[2] + square_high * tail;
	tail = coefficients[1] + square_high * tail;
	tail = coefficients[0] + square_high * tail;
	sin_offset = libm_dd_add_double(offset, offset.high * square_high * tail);

	/* cos d - 1 = -d^2/2 + d^4 (1/4! - d^2/6! + d^4/8!). */
	coefficients = __libm_cos_coefficients;
	tail = coefficients[3];
	tail = coefficients[2] + square_high * tail;
	tail = coefficients[1] + square_high * tail;
	cos_offset = libm_fast_two_sum(-0.5 * square.high,
	    -0.5 * square.low + square_high * square_high * tail);

	/* Combines with the table by the addition formulas. */
	table_sine = __libm_sin_cos_table[2 * index];
	table_cosine = __libm_sin_cos_table[2 * index + 1];
	first = libm_dd_multiply(table_sine, cos_offset);
	second = libm_dd_multiply(table_cosine, sin_offset);
	*sine = libm_dd_add(table_sine, libm_dd_add(first, second));
	first = libm_dd_multiply(table_cosine, cos_offset);
	second = libm_dd_multiply(table_sine, sin_offset);
	second.high = -second.high;
	second.low = -second.low;
	*cosine = libm_dd_add(table_cosine, libm_dd_add(first, second));

	/* A negative r gives the sine its sign back. */
	if (negative) {
		sine->high = -sine->high;
		sine->low = -sine->low;
	}
}

/*
 * Evaluates one of the three functions at x.
 */
static double
libm_trig(
	double x,
	enum libm_trig_function function)
{
	struct libm_dd reduced;
	struct libm_dd sine;
	struct libm_dd cosine;
	struct libm_dd value;
	uint64_t magnitude;
	int quadrant;
	int turns;

	/* A NaN, unequal to itself, is passed on; an infinity has no angle. */
	if (x != x)
		return x + x;
	magnitude = libm_bits(x) & LIBM_DOUBLE_MAGNITUDE;
	if (magnitude == LIBM_DOUBLE_INFINITY)
		return __libm_invalid();

	/* A tiny angle: sin and tan round to x, cos to one. */
	if (magnitude < LIBM_TRIG_TINY) {
		if (function == LIBM_COSINE)
			return 1.0;
		if (magnitude == 0U)
			return x;

		/* The result is x, inexact. */
		return __libm_check_underflow(x);
	}

	/* Reduces x to the quadrant and r, and evaluates both kernels. */
	quadrant = libm_reduce(x, &reduced);
	__libm_sin_cos_dd(reduced, &sine, &cosine);

	/* tan is sin/cos in an even quadrant and -cos/sin in an odd one. */
	if (function == LIBM_TANGENT) {
		if ((quadrant & 1) != 0) {
			value = libm_dd_divide(cosine, sine);
			value.high = -value.high;
			value.low = -value.low;
		} else {
			value = libm_dd_divide(sine, cosine);
		}

		/* The quotient rounded once. */
		return value.high + value.low;
	}

	/*
	 * Each quarter turn rotates the pair, sin(r + pi/2) being cos r and
	 * sin(r + pi) being -sin r; cos x is the sine a quarter turn later.
	 */
	turns = quadrant;
	if (function == LIBM_COSINE)
		turns = quadrant + 1;
	value = sine;
	if ((turns & 1) != 0)
		value = cosine;
	if ((turns & 2) != 0) {
		value.high = -value.high;
		value.low = -value.low;
	}

	/* Succeeded: the pair rounded once. */
	return value.high + value.low;
}

/*
 * Reduces x to k pi/2 + r and returns k mod 4.
 */
static int
libm_reduce(
	double x,
	struct libm_dd *reduced)
{
	struct libm_dd pair;
	uint64_t magnitude;
	double steps;
	double remainder;
	int step;
	int quadrant;

	/* Up to pi/4 x is its own remainder. */
	magnitude = libm_bits(x) & LIBM_DOUBLE_MAGNITUDE;
	if (magnitude <= LIBM_QUARTER_PI_BITS) {
		reduced->high = x;
		reduced->low = 0.0;
		return 0;
	}

	/* A large x is reduced in integers, on its magnitude. */
	if (magnitude >= LIBM_MEDIUM_BITS) {
		quadrant = libm_reduce_large(libm_from_bits(magnitude), reduced);
		if (x > 0.0)
			return quadrant;

		/* For a negative x the remainder and the quadrant change sign. */
		reduced->high = -reduced->high;
		reduced->low = -reduced->low;
		return (4 - quadrant) & 3;
	}

	/* The nearest number of quarter turns, rounded without a conversion. */
	steps = x * LIBM_TWO_OVER_PI + LIBM_ROUNDING_SHIFTER;
	steps -= LIBM_ROUNDING_SHIFTER;
	step = (int)steps;

	/*
	 * Takes the quarter turns away in four pieces.  The first three have
	 * 33 bits each, so their products with a step count below 2^19 are
	 * exact, and the first difference is exact as well.
	 */
	remainder = x - steps * LIBM_HALF_PI_1;
	pair = libm_two_sum(remainder, -steps * LIBM_HALF_PI_2);
	pair = libm_dd_add_double(pair, -steps * LIBM_HALF_PI_3);
	pair.low -= steps * LIBM_HALF_PI_4;
	*reduced = libm_two_sum(pair.high, pair.low);

	/* Succeeded: the quadrant is the step count mod 4. */
	return step & 3;
}

/*
 * Reduces a positive x of at least 2^19 and returns k mod 4 (Payne-Hanek).
 *
 * x = M 2^E with an integer M of 53 bits.  The bits of 2/pi before bit
 * E - 1 give x times 2/pi only multiples of 4, so a window of 192 bits
 * starting there is multiplied by M in 32-bit limbs.  The product holds
 * the quadrant in its two integer bits and the remainder in the rest.
 */
static int
libm_reduce_large(
	double magnitude,
	struct libm_dd *reduced)
{
	struct libm_unpacked unpacked;
	struct libm_dd fraction;
	struct libm_dd half_pi;
	uint32_t window[LIBM_WINDOW_LIMBS];
	uint32_t product[LIBM_PRODUCT_LIMBS];
	uint32_t factor[2];
	uint64_t partial;
	uint64_t carry;
	uint64_t high_bits;
	uint64_t low_bits;
	int first_bit;
	int point;
	int quadrant;
	int negative;
	int leading;
	int row;
	int column;
	int limb;

	/* The first bit of 2/pi that can matter, and where the binary point falls. */
	__libm_unpack(magnitude, &unpacked);
	first_bit = 1;
	if (unpacked.exponent >= 2)
		first_bit = unpacked.exponent - 1;
	point = first_bit + 191 - unpacked.exponent;

	/* Reads the window, least significant limb first. */
	for (limb = 0; limb < LIBM_WINDOW_LIMBS; limb++)
		window[LIBM_WINDOW_LIMBS - 1 - limb] = libm_two_over_pi_word(first_bit + 32 * limb);

	/* Multiplies the window by M, row by row. */
	factor[0] = (uint32_t)(unpacked.significand & UINT64_C(0xffffffff));
	factor[1] = (uint32_t)(unpacked.significand >> 32);
	for (limb = 0; limb < LIBM_PRODUCT_LIMBS; limb++)
		product[limb] = 0U;
	for (row = 0; row < LIBM_WINDOW_LIMBS; row++) {
		carry = 0U;
		for (column = 0; column < 2; column++) {
			partial = (uint64_t)window[row] * factor[column] +
			    product[row + column] + carry;
			product[row + column] = (uint32_t)partial;
			carry = partial >> 32;
		}

		/* The last carry starts the next limb, which no row has touched yet. */
		product[row + 2] = (uint32_t)carry;
	}

	/* The two bits above the binary point are the quadrant. */
	quadrant = (int)(libm_product_bits(product, point + 1) >> 62);

	/*
	 * A fraction of one half or more belongs to the next quadrant: the
	 * remainder is then the fraction minus one, negative.
	 */
	negative = (int)((libm_product_bits(product, point - 1) >> 63) & 1U);
	if (negative) {
		quadrant = (quadrant + 1) & 3;

		/* Negates the fraction bits: 2^point - f, in place. */
		carry = 1U;
		for (limb = 0; limb < LIBM_PRODUCT_LIMBS; limb++) {
			partial = (uint64_t)(uint32_t)~product[limb] + carry;
			product[limb] = (uint32_t)partial;
			carry = partial >> 32;
		}
	}

	/* Clears the integer bits, keeping only the fraction below the point. */
	for (limb = 0; limb < LIBM_PRODUCT_LIMBS; limb++) {
		if (32 * limb >= point) {
			product[limb] = 0U;
		} else if (32 * limb + 32 > point) {
			product[limb] &= (UINT32_C(1) << (point - 32 * limb)) - 1U;
		}
	}

	/* Finds the leading bit of the fraction. */
	leading = point - 1;
	for (;;) {
		high_bits = libm_product_bits(product, leading);
		if ((high_bits >> 63) != 0U || leading < 0)
			break;

		/* The bit is zero; the leading one is further down. */
		leading--;
	}

	/*
	 * The 53 bits from the leading one are exact in a double; the next 64
	 * are the low part, rounded.  The fraction is worth them times
	 * 2^-point.
	 */
	high_bits = libm_product_bits(product, leading) >> 11;
	low_bits = libm_product_bits(product, leading - 53);
	fraction.high = (double)high_bits * libm_power_of_two(leading - 52 - point);
	fraction.low = (double)low_bits * libm_power_of_two(leading - 116 - point);
	fraction = libm_fast_two_sum(fraction.high, fraction.low);

	/* r is the fraction of a quarter turn times pi/2. */
	half_pi.high = LIBM_HALF_PI_HIGH;
	half_pi.low = LIBM_HALF_PI_LOW;
	*reduced = libm_dd_multiply(fraction, half_pi);
	if (negative) {
		reduced->high = -reduced->high;
		reduced->low = -reduced->low;
	}

	/* Succeeded: the quadrant of x. */
	return quadrant;
}

/*
 * Returns 32 bits of 2/pi starting at a bit, counted from 1 after the
 * binary point.
 */
static uint32_t
libm_two_over_pi_word(
	int first_bit)
{
	int word;
	int shift;
	uint32_t bits;

	/* Locates the word and the offset of the first bit in it. */
	word = (first_bit - 1) / 32;
	shift = (first_bit - 1) % 32;

	/* An aligned run is one word; otherwise two words are joined. */
	bits = __libm_two_over_pi_bits[word];
	if (shift == 0)
		return bits;

	/* Succeeded: the tail of one word and the head of the next. */
	return (bits << shift) | (__libm_two_over_pi_bits[word + 1] >> (32 - shift));
}

/*
 * Returns the 64 bits of a multi-limb number whose top bit is at a given
 * position; positions below zero read as zero.
 */
static uint64_t
libm_product_bits(
	const uint32_t *limbs,
	int top)
{
	uint64_t bits;
	int position;
	int index;

	/* Gathers the bits one at a time, from the top. */
	bits = 0U;
	for (index = 0; index < 64; index++) {
		position = top - index;
		bits <<= 1;
		if (position >= 0 && position < 32 * LIBM_PRODUCT_LIMBS)
			bits |= (limbs[position / 32] >> (position % 32)) & 1U;
	}

	/* Succeeded: the bits from the top position down. */
	return bits;
}

/*
 * Returns 2^exponent for an exponent inside the normal range.
 */
static double
libm_power_of_two(
	int exponent)
{
	/* The biased exponent alone encodes the power. */
	return libm_from_bits((uint64_t)(exponent + LIBM_DOUBLE_BIAS) << 52);
}
