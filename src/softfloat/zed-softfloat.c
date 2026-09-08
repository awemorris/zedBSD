/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Integer-only IEEE 754 binary32 and binary64 arithmetic.
 *
 * Every value crosses this boundary as its object representation rather than
 * as a C floating-point type, so the implementation cannot recurse through
 * the compiler helpers that call it.  One set of routines serves both widths:
 * the format descriptor carries the field sizes and the exponent bias, and
 * the public wrappers name which descriptor to use.
 *
 * Arithmetic runs on a significand shifted left by three guard bits, the
 * lowest of which is sticky.  Rounding is round-to-nearest-even, and the
 * floating-point exception flags are raised through <fenv.h>.
 */

#include <fenv.h>
#include <stdint.h>

#include "src/softfloat/zed-softfloat.h"

/* zedBSD's freestanding stdint.h intentionally keeps the namespace small. */
#ifndef UINT32_C
#define UINT32_C(value) value##U
#endif
#ifndef UINT64_C
#define UINT64_C(value) value##ULL
#endif

/*
 * What kind of value a representation stands for.
 *
 * Unpacking classifies once, so the arithmetic below decides on this rather
 * than on the exponent and fraction fields.
 */
enum zsf_class {
	ZSF_ZERO,
	ZSF_FINITE,
	ZSF_INFINITY,
	ZSF_NAN
};

/*
 * One unpacked value.
 *
 * The fraction carries the hidden bit for a normal number, and the exponent
 * is already unbiased.  A subnormal is unpacked with the exponent of the
 * smallest normal, so normalization can shift it into place.
 */
struct zsf_number {
	uint64_t fraction;
	int exponent;
	unsigned int sign;
	enum zsf_class classification;
};

/*
 * A 128-bit unsigned value, as the exact product of two 64-bit significands.
 */
struct zsf_u128 {
	uint64_t high;
	uint64_t low;
};

/*
 * The field layout of one binary format.
 *
 * A pointer to one of the two constants below is what makes a routine work
 * on binary32 or on binary64.
 */
struct zsf_format {
	unsigned int fraction_bits;
	unsigned int exponent_bits;
	int bias;
};

/*
 * The IEEE 754 binary32 layout: 23 fraction bits, 8 exponent bits, bias 127.
 */
static const struct zsf_format zsf_binary32 = { 23U, 8U, 127 };

/*
 * The IEEE 754 binary64 layout: 52 fraction bits, 11 exponent bits, bias 1023.
 */
static const struct zsf_format zsf_binary64 = { 52U, 11U, 1023 };

static uint64_t zsf_mask(unsigned int bits);
static uint64_t zsf_shift_right_jam(uint64_t value, unsigned int distance);
static uint64_t zsf_u128_shift_right_jam(struct zsf_u128 value, unsigned int distance);
static struct zsf_u128 zsf_multiply64(uint64_t left, uint64_t right);
static struct zsf_number zsf_unpack(uint64_t bits, const struct zsf_format *format);
static uint64_t zsf_default_nan(const struct zsf_format *format);
static uint64_t zsf_propagate_nan(uint64_t left, uint64_t right, const struct zsf_format *format);
static void zsf_normalize(struct zsf_number *number, const struct zsf_format *format);
static uint64_t zsf_round_pack(unsigned int sign, int exponent, uint64_t significand, const struct zsf_format *format);
static uint64_t zsf_add(uint64_t left_bits, uint64_t right_bits, const struct zsf_format *format);
static uint64_t zsf_multiply(uint64_t left_bits, uint64_t right_bits, const struct zsf_format *format);
static uint64_t zsf_divide(uint64_t left_bits, uint64_t right_bits, const struct zsf_format *format);
static int zsf_compare_bits(uint64_t left_bits, uint64_t right_bits, const struct zsf_format *format, int *unordered);
static uint64_t zsf_convert_format(uint64_t bits, const struct zsf_format *source, const struct zsf_format *destination);
static uint64_t zsf_unsigned_to_format(uint64_t value, unsigned int sign, const struct zsf_format *format);
static int64_t zsf_to_integer(uint64_t bits, const struct zsf_format *format, unsigned int width, int unsigned_result);
static uint64_t zsf_signed_magnitude(int64_t value);

/*
 * Adds two binary32 values.
 */
uint32_t
zsf32_add(
	uint32_t left,
	uint32_t right)
{
	/* Reports the sum in the caller's width. */
	return (uint32_t)zsf_add(left, right, &zsf_binary32);
}

/*
 * Subtracts one binary32 value from another.
 */
uint32_t
zsf32_sub(
	uint32_t left,
	uint32_t right)
{
	/* Subtraction is addition of the operand with its sign bit flipped. */
	return (uint32_t)zsf_add(left, right ^ UINT32_C(0x80000000),
	    &zsf_binary32);
}

/*
 * Multiplies two binary32 values.
 */
uint32_t
zsf32_mul(
	uint32_t left,
	uint32_t right)
{
	/* Reports the product in the caller's width. */
	return (uint32_t)zsf_multiply(left, right, &zsf_binary32);
}

/*
 * Divides one binary32 value by another.
 */
uint32_t
zsf32_div(
	uint32_t left,
	uint32_t right)
{
	/* Reports the quotient in the caller's width. */
	return (uint32_t)zsf_divide(left, right, &zsf_binary32);
}

/*
 * Orders two binary32 values, reporting whether they compare at all.
 */
int
zsf32_compare(
	uint32_t left,
	uint32_t right,
	int *unordered)
{
	/* Reports the ordering, or zero when either operand is a NaN. */
	return zsf_compare_bits(left, right, &zsf_binary32, unordered);
}

/*
 * Adds two binary64 values.
 */
uint64_t
zsf64_add(
	uint64_t left,
	uint64_t right)
{
	/* Reports the sum. */
	return zsf_add(left, right, &zsf_binary64);
}

/*
 * Subtracts one binary64 value from another.
 */
uint64_t
zsf64_sub(
	uint64_t left,
	uint64_t right)
{
	/* Subtraction is addition of the operand with its sign bit flipped. */
	return zsf_add(left, right ^ UINT64_C(0x8000000000000000),
	    &zsf_binary64);
}

/*
 * Multiplies two binary64 values.
 */
uint64_t
zsf64_mul(
	uint64_t left,
	uint64_t right)
{
	/* Reports the product. */
	return zsf_multiply(left, right, &zsf_binary64);
}

/*
 * Divides one binary64 value by another.
 */
uint64_t
zsf64_div(
	uint64_t left,
	uint64_t right)
{
	/* Reports the quotient. */
	return zsf_divide(left, right, &zsf_binary64);
}

/*
 * Orders two binary64 values, reporting whether they compare at all.
 */
int
zsf64_compare(
	uint64_t left,
	uint64_t right,
	int *unordered)
{
	/* Reports the ordering, or zero when either operand is a NaN. */
	return zsf_compare_bits(left, right, &zsf_binary64, unordered);
}

/*
 * Widens a binary32 value to binary64.
 */
uint64_t
zsf32_to_64(
	uint32_t value)
{
	/* Every binary32 value is exact in binary64. */
	return zsf_convert_format(value, &zsf_binary32, &zsf_binary64);
}

/*
 * Narrows a binary64 value to binary32.
 */
uint32_t
zsf64_to_32(
	uint64_t value)
{
	/* The narrowing rounds, and may overflow or lose precision. */
	return (uint32_t)zsf_convert_format(value, &zsf_binary64,
	    &zsf_binary32);
}

/*
 * Converts a signed integer to binary32.
 */
uint32_t
zsf_i64_to_32(
	int64_t value)
{
	/* Converts the magnitude and applies the sign separately. */
	return (uint32_t)zsf_unsigned_to_format(zsf_signed_magnitude(value),
	    value < 0, &zsf_binary32);
}

/*
 * Converts an unsigned integer to binary32.
 */
uint32_t
zsf_u64_to_32(
	uint64_t value)
{
	/* An unsigned value is always positive. */
	return (uint32_t)zsf_unsigned_to_format(value, 0U, &zsf_binary32);
}

/*
 * Converts a signed integer to binary64.
 */
uint64_t
zsf_i64_to_64(
	int64_t value)
{
	/* Converts the magnitude and applies the sign separately. */
	return zsf_unsigned_to_format(zsf_signed_magnitude(value), value < 0,
	    &zsf_binary64);
}

/*
 * Converts an unsigned integer to binary64.
 */
uint64_t
zsf_u64_to_64(
	uint64_t value)
{
	/* An unsigned value is always positive. */
	return zsf_unsigned_to_format(value, 0U, &zsf_binary64);
}

/*
 * Converts a binary32 value to an integer of the named width.
 */
int64_t
zsf32_to_i64(
	uint32_t value,
	unsigned int width,
	int unsigned_result)
{
	/* Reports the truncated integer, saturating where it does not fit. */
	return zsf_to_integer(value, &zsf_binary32, width, unsigned_result);
}

/*
 * Converts a binary64 value to an integer of the named width.
 */
int64_t
zsf64_to_i64(
	uint64_t value,
	unsigned int width,
	int unsigned_result)
{
	/* Reports the truncated integer, saturating where it does not fit. */
	return zsf_to_integer(value, &zsf_binary64, width, unsigned_result);
}

/*
 * Rounds and packs a binary32 result built by a caller.
 *
 * libc's own conversions build a significand with the same three guard bits
 * this unit uses, and finish it here so that rounding stays in one place.
 */
uint32_t
zsf32_round_pack(
	unsigned int sign,
	int exponent,
	uint64_t significand)
{
	/* Reports the packed value in the caller's width. */
	return (uint32_t)zsf_round_pack(sign, exponent, significand,
	    &zsf_binary32);
}

/*
 * Rounds and packs a binary64 result built by a caller.
 */
uint64_t
zsf64_round_pack(
	unsigned int sign,
	int exponent,
	uint64_t significand)
{
	/* Reports the packed value. */
	return zsf_round_pack(sign, exponent, significand, &zsf_binary64);
}

/* Reports a mask of the low bits, where 64 means every bit. */
static uint64_t
zsf_mask(
	unsigned int bits)
{
	/* A 64-bit shift is undefined, so that width is named directly. */
	return bits == 64U ? UINT64_MAX : ((UINT64_C(1) << bits) - 1U);
}

/*
 * Shifts a significand right, keeping a sticky bit.
 *
 * The lowest bit of the result records whether any one bit was shifted out,
 * which is what lets the rounding below stay exact.
 */
static uint64_t
zsf_shift_right_jam(
	uint64_t value,
	unsigned int distance)
{
	/* A zero distance loses nothing. */
	if (distance == 0U)
		return value;

	/* An ordinary shift keeps the bits it drops in the sticky bit. */
	if (distance < 64U)
		return (value >> distance) |
		    ((value << (64U - distance)) != 0U);

	/* A wider shift leaves nothing but the sticky bit. */
	return value != 0U;
}

/*
 * Shifts a 128-bit product right into 64 bits, keeping a sticky bit.
 */
static uint64_t
zsf_u128_shift_right_jam(
	struct zsf_u128 value,
	unsigned int distance)
{
	uint64_t result;

	/* A zero distance keeps the low half unchanged. */
	if (distance == 0U)
		return value.low;

	/* A short shift draws bits from both halves. */
	if (distance < 64U) {
		result = (value.high << (64U - distance)) |
		    (value.low >> distance);

		return result | ((value.low << (64U - distance)) != 0U);
	}

	/* At exactly 64 the high half is the result and the low half sticks. */
	if (distance == 64U)
		return value.high | (value.low != 0U);

	/* A longer shift keeps what is left of the high half. */
	if (distance < 128U)
		return (value.high >> (distance - 64U)) |
		    (((value.high << (128U - distance)) | value.low) != 0U);

	/* Beyond the width nothing is left but the sticky bit. */
	return (value.high | value.low) != 0U;
}

/* Multiplies two 64-bit values into an exact 128-bit product. */
static struct zsf_u128
zsf_multiply64(
	uint64_t left,
	uint64_t right)
{
	uint64_t left_low;
	uint64_t left_high;
	uint64_t right_low;
	uint64_t right_high;
	uint64_t low_product;
	uint64_t middle1;
	uint64_t middle2;
	uint64_t high_product;
	uint64_t carry;
	struct zsf_u128 result;

	/* Splits both operands into halves the machine can multiply exactly. */
	left_low = (uint32_t)left;
	left_high = left >> 32;
	right_low = (uint32_t)right;
	right_high = right >> 32;

	/* Forms the four partial products. */
	low_product = left_low * right_low;
	middle1 = left_high * right_low;
	middle2 = left_low * right_high;
	high_product = left_high * right_high;

	/* Adds them up, carrying from the middle column into the high one. */
	carry = (low_product >> 32) + (uint32_t)middle1 +
	    (uint32_t)middle2;
	result.low = (low_product & UINT64_C(0xffffffff)) | (carry << 32);
	result.high = high_product + (middle1 >> 32) + (middle2 >> 32) +
	    (carry >> 32);

	/* Reports the exact product. */
	return result;
}

/* Splits a representation into its sign, exponent, fraction and class. */
static struct zsf_number
zsf_unpack(
	uint64_t bits,
	const struct zsf_format *format)
{
	uint64_t fraction_mask;
	uint64_t exponent_mask;
	uint64_t exponent_field;
	struct zsf_number number;

	/* Takes the three fields apart. */
	fraction_mask = zsf_mask(format->fraction_bits);
	exponent_mask = zsf_mask(format->exponent_bits);
	exponent_field = (bits >> format->fraction_bits) & exponent_mask;
	number.sign = (unsigned int)(bits >>
	    (format->fraction_bits + format->exponent_bits));
	number.fraction = bits & fraction_mask;

	/* An all-ones exponent is an infinity or a NaN. */
	if (exponent_field == exponent_mask) {
		number.classification = number.fraction == 0U ?
		    ZSF_INFINITY : ZSF_NAN;
		number.exponent = 0;

		return number;
	}

	/*
	 * A zero exponent is a zero or a subnormal.  The subnormal takes the
	 * exponent of the smallest normal, so normalization can shift its
	 * fraction up into place.
	 */
	if (exponent_field == 0U) {
		number.classification = number.fraction == 0U ?
		    ZSF_ZERO : ZSF_FINITE;
		number.exponent = 1 - format->bias;

		return number;
	}

	/* Anything else is normal, and carries a hidden leading bit. */
	number.classification = ZSF_FINITE;
	number.exponent = (int)exponent_field - format->bias;
	number.fraction |= UINT64_C(1) << format->fraction_bits;

	return number;
}

/* Reports the quiet NaN this unit produces for an invalid operation. */
static uint64_t
zsf_default_nan(
	const struct zsf_format *format)
{
	/* An all-ones exponent with the quiet bit set and no payload. */
	return (zsf_mask(format->exponent_bits) << format->fraction_bits) |
	    (UINT64_C(1) << (format->fraction_bits - 1U));
}

/*
 * Reports the NaN an operation on a NaN operand produces.
 *
 * A signalling operand raises the invalid flag on its way through, and the
 * result is always quiet.
 */
static uint64_t
zsf_propagate_nan(
	uint64_t left,
	uint64_t right,
	const struct zsf_format *format)
{
	uint64_t exponent_mask;
	uint64_t fraction_mask;
	uint64_t quiet;
	uint64_t selected;

	/* Prefers the left operand when it is the NaN. */
	exponent_mask = zsf_mask(format->exponent_bits) <<
	    format->fraction_bits;
	fraction_mask = zsf_mask(format->fraction_bits);
	quiet = UINT64_C(1) << (format->fraction_bits - 1U);
	selected = (left & fraction_mask) != 0U &&
	    (left & exponent_mask) == exponent_mask ? left : right;

	/* A signalling NaN raises the invalid flag as it is quieted. */
	if ((selected & quiet) == 0U)
		(void)feraiseexcept(FE_INVALID);

	/* Reports the payload with the quiet bit set. */
	return (selected | quiet) &
	    zsf_mask(1U + format->exponent_bits + format->fraction_bits);
}

/* Shifts a subnormal fraction up until its hidden bit is in place. */
static void
zsf_normalize(
	struct zsf_number *number,
	const struct zsf_format *format)
{
	uint64_t hidden;

	/* Each shift buys one bit of significand at the cost of an exponent. */
	hidden = UINT64_C(1) << format->fraction_bits;
	while (number->fraction != 0U && number->fraction < hidden) {
		number->fraction <<= 1;
		number->exponent--;
	}
}

/*
 * Rounds a guarded significand and packs it into a representation.
 *
 * The significand arrives shifted left by three bits: a guard bit, a round
 * bit, and a sticky bit.  Rounding is to nearest, ties to even, and every
 * flag IEEE 754 requires is raised here.
 */
static uint64_t
zsf_round_pack(
	unsigned int sign,
	int exponent,
	uint64_t significand,
	const struct zsf_format *format)
{
	uint64_t hidden;
	uint64_t overflow_bit;
	uint64_t rounded;
	uint64_t round_bits;
	uint64_t exponent_field;
	int minimum_exponent;
	int maximum_exponent;

	/* Names the places the significand may not cross. */
	hidden = UINT64_C(1) << format->fraction_bits;
	overflow_bit = hidden << 1;
	minimum_exponent = 1 - format->bias;
	maximum_exponent = format->bias;

	/* A zero significand is a signed zero, whatever the exponent says. */
	if (significand == 0U)
		return (uint64_t)sign <<
	    (format->fraction_bits + format->exponent_bits);

	/* Brings the significand back inside the guarded range. */
	while (significand >= (overflow_bit << 3)) {
		significand = zsf_shift_right_jam(significand, 1U);
		exponent++;
	}
	while (significand < (hidden << 3) && exponent > minimum_exponent) {
		significand <<= 1;
		exponent--;
	}

	/* An exponent below the range is represented by shifting instead. */
	if (exponent < minimum_exponent) {
		significand = zsf_shift_right_jam(significand,
		    (unsigned int)(minimum_exponent - exponent));
		exponent = minimum_exponent;
	}

	/* Rounds to nearest, breaking a tie towards an even significand. */
	round_bits = significand & 7U;
	rounded = significand >> 3;
	if (round_bits > 4U || (round_bits == 4U && (rounded & 1U) != 0U))
		rounded++;

	/* Anything dropped makes the result inexact. */
	if (round_bits != 0U)
		(void)feraiseexcept(FE_INEXACT);

	/* Rounding up may carry into the next binade. */
	if (rounded >= overflow_bit) {
		rounded >>= 1;
		exponent++;
	}

	/* An exponent past the range becomes a signed infinity. */
	if (exponent > maximum_exponent) {
		(void)feraiseexcept(FE_OVERFLOW | FE_INEXACT);

		return ((uint64_t)sign <<
	    (format->fraction_bits + format->exponent_bits)) |
	    (zsf_mask(format->exponent_bits) << format->fraction_bits);
	}

	/*
	 * A result that stayed below the hidden bit at the smallest exponent
	 * is subnormal, and is encoded with a zero exponent field.  Losing
	 * bits there is an underflow as well as an inexact result.
	 */
	if (exponent == minimum_exponent && rounded < hidden) {
		exponent_field = 0U;
		if (round_bits != 0U)
			(void)feraiseexcept(FE_UNDERFLOW);
	} else {
		exponent_field = (uint64_t)(exponent + format->bias);
	}

	/* Assembles the three fields. */
	return ((uint64_t)sign <<
	    (format->fraction_bits + format->exponent_bits)) |
	    (exponent_field << format->fraction_bits) |
	    (rounded & (hidden - 1U));
}

/* Adds two representations, treating subtraction as a flipped sign. */
static uint64_t
zsf_add(
	uint64_t left_bits,
	uint64_t right_bits,
	const struct zsf_format *format)
{
	struct zsf_number left;
	struct zsf_number right;
	struct zsf_number temporary;
	uint64_t left_significand;
	uint64_t right_significand;
	uint64_t result;
	unsigned int sign;

	/* Classifies both operands. */
	left = zsf_unpack(left_bits, format);
	right = zsf_unpack(right_bits, format);

	/* A NaN operand carries through. */
	if (left.classification == ZSF_NAN || right.classification == ZSF_NAN)
		return zsf_propagate_nan(left_bits, right_bits, format);

	/* Opposite infinities are invalid; otherwise the infinity wins. */
	if (left.classification == ZSF_INFINITY ||
	    right.classification == ZSF_INFINITY) {
		if (left.classification == ZSF_INFINITY &&
		    right.classification == ZSF_INFINITY &&
		    left.sign != right.sign) {
			(void)feraiseexcept(FE_INVALID);

			return zsf_default_nan(format);
		}

		return left.classification == ZSF_INFINITY ?
		    left_bits : right_bits;
	}

	/* Two zeroes of unlike sign make a positive zero. */
	if (left.classification == ZSF_ZERO &&
	    right.classification == ZSF_ZERO)
		return left.sign == right.sign ? left_bits : 0U;

	/* Adding a zero to anything reports that other operand unchanged. */
	if (left.classification == ZSF_ZERO)
		return right_bits;
	if (right.classification == ZSF_ZERO)
		return left_bits;

	/* Puts the larger magnitude on the left so the result cannot borrow. */
	zsf_normalize(&left, format);
	zsf_normalize(&right, format);
	if (left.exponent < right.exponent ||
	    (left.exponent == right.exponent &&
	    left.fraction < right.fraction)) {
		temporary = left;
		left = right;
		right = temporary;
	}

	/* Brings the smaller operand onto the larger one's exponent. */
	left_significand = left.fraction << 3;
	right_significand = zsf_shift_right_jam(right.fraction << 3,
	    (unsigned int)(left.exponent - right.exponent));
	sign = left.sign;

	/* Like signs add; unlike signs subtract, and may cancel exactly. */
	if (left.sign == right.sign) {
		result = left_significand + right_significand;
	} else {
		result = left_significand - right_significand;
		if (result == 0U)
			return 0U;
	}

	/* Rounds the guarded result into the format. */
	return zsf_round_pack(sign, left.exponent, result, format);
}

/* Multiplies two representations. */
static uint64_t
zsf_multiply(
	uint64_t left_bits,
	uint64_t right_bits,
	const struct zsf_format *format)
{
	struct zsf_number left;
	struct zsf_number right;
	struct zsf_u128 product;
	unsigned int sign;

	/* Classifies both operands; the product's sign is theirs combined. */
	left = zsf_unpack(left_bits, format);
	right = zsf_unpack(right_bits, format);
	sign = left.sign ^ right.sign;

	/* A NaN operand carries through. */
	if (left.classification == ZSF_NAN || right.classification == ZSF_NAN)
		return zsf_propagate_nan(left_bits, right_bits, format);

	/* An infinity times a zero has no value. */
	if ((left.classification == ZSF_INFINITY &&
	    right.classification == ZSF_ZERO) ||
	    (right.classification == ZSF_INFINITY &&
	    left.classification == ZSF_ZERO)) {
		(void)feraiseexcept(FE_INVALID);

		return zsf_default_nan(format);
	}

	/* Any other infinity gives a signed infinity. */
	if (left.classification == ZSF_INFINITY ||
	    right.classification == ZSF_INFINITY)
		return ((uint64_t)sign <<
	    (format->fraction_bits + format->exponent_bits)) |
	    (zsf_mask(format->exponent_bits) << format->fraction_bits);

	/* Any other zero gives a signed zero. */
	if (left.classification == ZSF_ZERO ||
	    right.classification == ZSF_ZERO)
		return (uint64_t)sign <<
	    (format->fraction_bits + format->exponent_bits);

	/* Multiplies the significands exactly and rounds the product. */
	zsf_normalize(&left, format);
	zsf_normalize(&right, format);
	product = zsf_multiply64(left.fraction, right.fraction);

	return zsf_round_pack(sign, left.exponent + right.exponent,
	    zsf_u128_shift_right_jam(product, format->fraction_bits - 3U),
	    format);
}

/* Divides one representation by another. */
static uint64_t
zsf_divide(
	uint64_t left_bits,
	uint64_t right_bits,
	const struct zsf_format *format)
{
	struct zsf_number left;
	struct zsf_number right;
	uint64_t quotient;
	uint64_t remainder;
	unsigned int count;
	unsigned int sign;
	int exponent;

	/* Classifies both operands; the quotient's sign is theirs combined. */
	left = zsf_unpack(left_bits, format);
	right = zsf_unpack(right_bits, format);
	quotient = 1U;
	sign = left.sign ^ right.sign;

	/* A NaN operand carries through. */
	if (left.classification == ZSF_NAN || right.classification == ZSF_NAN)
		return zsf_propagate_nan(left_bits, right_bits, format);

	/* Zero over zero and infinity over infinity have no value. */
	if ((left.classification == ZSF_ZERO &&
	    right.classification == ZSF_ZERO) ||
	    (left.classification == ZSF_INFINITY &&
	    right.classification == ZSF_INFINITY)) {
		(void)feraiseexcept(FE_INVALID);

		return zsf_default_nan(format);
	}

	/* An infinite dividend gives a signed infinity. */
	if (left.classification == ZSF_INFINITY)
		return ((uint64_t)sign <<
	    (format->fraction_bits + format->exponent_bits)) |
	    (zsf_mask(format->exponent_bits) << format->fraction_bits);

	/* An infinite divisor, or a zero dividend, gives a signed zero. */
	if (right.classification == ZSF_INFINITY ||
	    left.classification == ZSF_ZERO)
		return (uint64_t)sign <<
	    (format->fraction_bits + format->exponent_bits);

	/* A zero divisor gives a signed infinity and raises the flag. */
	if (right.classification == ZSF_ZERO) {
		(void)feraiseexcept(FE_DIVBYZERO);

		return ((uint64_t)sign <<
	    (format->fraction_bits + format->exponent_bits)) |
	    (zsf_mask(format->exponent_bits) << format->fraction_bits);
	}

	/* Places the dividend above the divisor so the first bit is one. */
	zsf_normalize(&left, format);
	zsf_normalize(&right, format);
	exponent = left.exponent - right.exponent;
	if (left.fraction < right.fraction) {
		left.fraction <<= 1;
		exponent--;
	}

	/* Produces one quotient bit per pass by restoring division. */
	remainder = left.fraction - right.fraction;
	for (count = 0U; count < format->fraction_bits + 3U; count++) {
		quotient <<= 1;
		remainder <<= 1;
		if (remainder >= right.fraction) {
			remainder -= right.fraction;
			quotient |= 1U;
		}
	}

	/* A remainder that never cleared makes the quotient sticky. */
	if (remainder != 0U)
		quotient |= 1U;

	/* Rounds the guarded quotient into the format. */
	return zsf_round_pack(sign, exponent, quotient, format);
}

/*
 * Orders two representations.
 *
 * The result is negative, zero or positive as the left operand is less than,
 * equal to or greater than the right one.  A NaN on either side makes the
 * pair unordered, which the caller must test before reading the result.
 */
static int
zsf_compare_bits(
	uint64_t left_bits,
	uint64_t right_bits,
	const struct zsf_format *format,
	int *unordered)
{
	struct zsf_number left;
	struct zsf_number right;
	uint64_t magnitude_mask;
	uint64_t left_magnitude;
	uint64_t right_magnitude;

	/* Takes the magnitudes, which order like integers within a sign. */
	left = zsf_unpack(left_bits, format);
	right = zsf_unpack(right_bits, format);
	magnitude_mask = zsf_mask(format->fraction_bits +
	    format->exponent_bits);
	left_magnitude = left_bits & magnitude_mask;
	right_magnitude = right_bits & magnitude_mask;

	/* A NaN on either side leaves the pair unordered. */
	*unordered = left.classification == ZSF_NAN ||
	    right.classification == ZSF_NAN;
	if (*unordered)
		return 0;

	/* The two zeroes compare equal despite their different signs. */
	if (left_magnitude == 0U && right_magnitude == 0U)
		return 0;

	/* A difference in sign settles the order by itself. */
	if (left.sign != right.sign)
		return left.sign ? -1 : 1;

	/* Equal magnitudes of equal sign are the same value. */
	if (left_magnitude == right_magnitude)
		return 0;

	/* Within one sign the magnitudes order like integers. */
	if (left.sign)
		return left_magnitude > right_magnitude ? -1 : 1;

	return left_magnitude < right_magnitude ? -1 : 1;
}

/* Converts a representation from one format to the other. */
static uint64_t
zsf_convert_format(
	uint64_t bits,
	const struct zsf_format *source,
	const struct zsf_format *destination)
{
	struct zsf_number number;
	uint64_t significand;
	unsigned int total_destination;

	/* Classifies the operand in its own format. */
	number = zsf_unpack(bits, source);
	total_destination = destination->fraction_bits +
	    destination->exponent_bits;

	/* A NaN becomes this unit's quiet NaN, keeping only its sign. */
	if (number.classification == ZSF_NAN)
		return zsf_default_nan(destination) |
		    ((uint64_t)number.sign << total_destination);

	/* An infinity and a zero convert to their counterparts. */
	if (number.classification == ZSF_INFINITY)
		return ((uint64_t)number.sign << total_destination) |
		    (zsf_mask(destination->exponent_bits) <<
		    destination->fraction_bits);
	if (number.classification == ZSF_ZERO)
		return (uint64_t)number.sign << total_destination;

	/*
	 * Moves the significand onto the destination's guarded scale, which
	 * is exact when widening and rounds when narrowing.
	 */
	zsf_normalize(&number, source);
	if (source->fraction_bits <= destination->fraction_bits + 3U)
		significand = number.fraction <<
		    (destination->fraction_bits + 3U - source->fraction_bits);
	else
		significand = zsf_shift_right_jam(number.fraction,
		    source->fraction_bits - destination->fraction_bits - 3U);

	/* Rounds the guarded significand into the destination format. */
	return zsf_round_pack(number.sign, number.exponent, significand,
	    destination);
}

/* Converts an integer magnitude and a separate sign to a representation. */
static uint64_t
zsf_unsigned_to_format(
	uint64_t value,
	unsigned int sign,
	const struct zsf_format *format)
{
	unsigned int highest;
	uint64_t scan;
	uint64_t significand;

	/* A zero magnitude is a signed zero. */
	if (value == 0U)
		return (uint64_t)sign <<
	    (format->fraction_bits + format->exponent_bits);

	/* Finds the position of the highest set bit, which is the exponent. */
	highest = 0U;
	scan = value;
	while (scan >>= 1)
		highest++;

	/* Moves the magnitude onto the guarded scale, rounding if it is wide. */
	if (highest <= format->fraction_bits + 3U)
		significand = value << (format->fraction_bits + 3U - highest);
	else
		significand = zsf_shift_right_jam(value,
		    highest - format->fraction_bits - 3U);

	/* Rounds the guarded significand into the format. */
	return zsf_round_pack(sign, (int)highest, significand, format);
}

/* Reports the magnitude of a signed integer, including the most negative. */
static uint64_t
zsf_signed_magnitude(
	int64_t value)
{
	/*
	 * Negating the most negative value directly would overflow, so the
	 * magnitude is built from one more than the negated successor.
	 */
	if (value < 0)
		return (uint64_t)(-(value + 1)) + 1U;

	return (uint64_t)value;
}

/*
 * Converts a representation to an integer of the named width.
 *
 * The conversion truncates towards zero.  A value that does not fit, and any
 * value that is not finite, saturates and raises the invalid flag.
 */
static int64_t
zsf_to_integer(
	uint64_t bits,
	const struct zsf_format *format,
	unsigned int width,
	int unsigned_result)
{
	struct zsf_number number;
	uint64_t magnitude;
	uint64_t maximum;
	int shift;

	/* Classifies the operand and names the widest value that fits. */
	number = zsf_unpack(bits, format);
	maximum = width == 64U ? UINT64_MAX : ((UINT64_C(1) << width) - 1U);

	/* An infinity or a NaN has no integer value. */
	if (number.classification != ZSF_FINITE) {
		(void)feraiseexcept(FE_INVALID);

		return unsigned_result ? (int64_t)maximum :
		    (number.sign ? INT64_MIN : INT64_MAX);
	}

	/* Anything below one truncates to zero. */
	zsf_normalize(&number, format);
	if (number.exponent < 0)
		return 0;

	/* Scales the significand to an integer, saturating if it will not fit. */
	shift = number.exponent - (int)format->fraction_bits;
	if (shift >= 0) {
		if (shift >= 64 || number.fraction > (maximum >> shift)) {
			(void)feraiseexcept(FE_INVALID);

			return unsigned_result ? (int64_t)maximum :
			    (number.sign ? INT64_MIN : INT64_MAX);
		}

		magnitude = number.fraction << shift;
	} else {
		magnitude = number.fraction >> -shift;
	}

	/* An unsigned result rejects a negative value of any magnitude. */
	if (unsigned_result) {
		if (number.sign && magnitude != 0U) {
			(void)feraiseexcept(FE_INVALID);

			return 0;
		}

		return (int64_t)magnitude;
	}

	/* A signed result reaches one further in the negative direction. */
	maximum = width == 64U ? UINT64_C(0x7fffffffffffffff) :
	    ((UINT64_C(1) << (width - 1U)) - 1U);
	if ((!number.sign && magnitude > maximum) ||
	    (number.sign && magnitude > maximum + 1U)) {
		(void)feraiseexcept(FE_INVALID);

		return number.sign ? INT64_MIN : INT64_MAX;
	}

	/* Applies the sign, naming the most negative value directly. */
	if (number.sign)
		return magnitude == (UINT64_C(1) << 63) ? INT64_MIN :
		    -(int64_t)magnitude;

	return (int64_t)magnitude;
}
