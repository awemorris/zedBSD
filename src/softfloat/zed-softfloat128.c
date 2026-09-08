/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Integer-only IEEE 754 binary128 arithmetic for SPARC V9.
 *
 * SPARC V9 names quad-precision operations in its ABI that no zedBSD target
 * implements in hardware, so they are carried out here on a 256-bit integer
 * built from four 64-bit limbs.  That width leaves room for the 113-bit
 * significand, the three guard bits the rounding uses, and the intermediate
 * products of a multiply.
 *
 * Values cross this boundary as their object representations, so the
 * implementation cannot recurse through the compiler helpers that call it.
 * Conversions to and from the narrower formats finish in zed-softfloat.c, so
 * that rounding lives in one place per format.
 */

#include <fenv.h>
#include <stdint.h>

#include "src/softfloat/zed-softfloat.h"
#include "src/softfloat/zed-softfloat128.h"

/* zedBSD's freestanding stdint.h intentionally keeps the namespace small. */
#ifndef UINT64_C
#define UINT64_C(value) value##ULL
#endif
#ifndef UINT32_C
#define UINT32_C(value) value##U
#endif

#define QUAD_LIMBS		4U
#define QUAD_WIDTH		256U
#define QUAD_FRACTION_BITS	112U
#define QUAD_HIDDEN_BIT		112U
#define QUAD_GUARDED_BIT	115U
#define QUAD_BIAS		16383
#define QUAD_MINIMUM_EXPONENT	(-16382)
#define QUAD_MAXIMUM_EXPONENT	16383

/*
 * A 256-bit unsigned value, least significant limb first.
 *
 * It holds a significand together with the three guard bits the rounding
 * uses, and is wide enough for the exact product of two significands.
 */
struct quad_uint {
	uint64_t limb[QUAD_LIMBS];
};

/*
 * What kind of value a binary128 representation stands for.
 */
enum quad_class {
	QUAD_ZERO,
	QUAD_FINITE,
	QUAD_INFINITY,
	QUAD_NAN
};

/*
 * One unpacked binary128 value.
 *
 * The significand carries the hidden bit for a normal number, and the
 * exponent is already unbiased.  A subnormal is unpacked with the exponent of
 * the smallest normal, so normalization can shift it into place.
 */
struct quad_number {
	struct quad_uint significand;
	int exponent;
	unsigned int sign;
	enum quad_class classification;
};

static int quad_zero(struct quad_uint value);
static int quad_compare_uint(struct quad_uint left, struct quad_uint right);
static void quad_shift_left_one(struct quad_uint *value);
static void quad_shift_left(struct quad_uint *value, unsigned int distance);
static void quad_shift_right_jam(struct quad_uint *value, unsigned int distance);
static void quad_shift_right(struct quad_uint *value, unsigned int distance);
static void quad_add_uint(struct quad_uint *left, struct quad_uint right);
static void quad_sub_uint(struct quad_uint *left, struct quad_uint right);
static int quad_highest_bit(struct quad_uint value);
static int quad_bit(struct quad_uint value, unsigned int bit);
static void quad_set_bit(struct quad_uint *value, unsigned int bit);
static struct quad_number quad_unpack(struct zsf128 bits);
static void quad_normalize(struct quad_number *number);
static struct zsf128 quad_nan(void);
static struct zsf128 quad_infinity(unsigned int sign);
static struct zsf128 quad_signed_zero(unsigned int sign);
static struct zsf128 quad_pack(unsigned int sign, int exponent, struct quad_uint significand);
static struct zsf128 quad_add_bits(struct zsf128 left_bits, struct zsf128 right_bits);
static struct quad_uint quad_multiply_significands(struct quad_uint left, struct quad_uint right);
static struct zsf128 small_to_quad(uint64_t fraction, int exponent, unsigned int source_fraction_bits, unsigned int sign);

/*
 * Adds two binary128 values.
 */
struct zsf128
zsf128_add(
	struct zsf128 left,
	struct zsf128 right)
{
	/* Reports the sum. */
	return quad_add_bits(left, right);
}

/*
 * Subtracts one binary128 value from another.
 */
struct zsf128
zsf128_sub(
	struct zsf128 left,
	struct zsf128 right)
{
	/* Subtraction is addition of the operand with its sign bit flipped. */
	right.high ^= UINT64_C(0x8000000000000000);

	/* Reports the difference. */
	return quad_add_bits(left, right);
}

/*
 * Multiplies two binary128 values.
 */
struct zsf128
zsf128_mul(
	struct zsf128 left,
	struct zsf128 right)
{
	struct quad_number left_number;
	struct quad_number right_number;
	struct quad_uint product;
	unsigned int sign;

	/* Classifies both operands; the product's sign is theirs combined. */
	left_number = quad_unpack(left);
	right_number = quad_unpack(right);
	sign = left_number.sign ^ right_number.sign;

	/* A NaN operand gives this unit's quiet NaN. */
	if (left_number.classification == QUAD_NAN ||
	    right_number.classification == QUAD_NAN)
		return quad_nan();

	/* An infinity times a zero has no value. */
	if ((left_number.classification == QUAD_INFINITY &&
	    right_number.classification == QUAD_ZERO) ||
	    (right_number.classification == QUAD_INFINITY &&
	    left_number.classification == QUAD_ZERO)) {
		(void)feraiseexcept(FE_INVALID);

		return quad_nan();
	}

	/* Any other infinity gives a signed infinity. */
	if (left_number.classification == QUAD_INFINITY ||
	    right_number.classification == QUAD_INFINITY)
		return quad_infinity(sign);

	/* Any other zero gives a signed zero. */
	if (left_number.classification == QUAD_ZERO ||
	    right_number.classification == QUAD_ZERO)
		return quad_signed_zero(sign);

	/*
	 * Multiplies the significands exactly, then brings the product back
	 * onto the guarded scale the packing expects.
	 */
	quad_normalize(&left_number);
	quad_normalize(&right_number);
	product = quad_multiply_significands(left_number.significand,
	    right_number.significand);
	quad_shift_right_jam(&product, 109U);

	/* Rounds the guarded product into the format. */
	return quad_pack(sign, left_number.exponent + right_number.exponent,
	    product);
}

/*
 * Divides one binary128 value by another.
 */
struct zsf128
zsf128_div(
	struct zsf128 left,
	struct zsf128 right)
{
	struct quad_number left_number;
	struct quad_number right_number;
	struct quad_uint quotient;
	struct quad_uint remainder;
	unsigned int sign;
	unsigned int count;
	int exponent;

	/* Classifies both operands; the quotient's sign is theirs combined. */
	left_number = quad_unpack(left);
	right_number = quad_unpack(right);
	sign = left_number.sign ^ right_number.sign;
	quotient.limb[0] = 1U;
	quotient.limb[1] = 0U;
	quotient.limb[2] = 0U;
	quotient.limb[3] = 0U;

	/* A NaN operand gives this unit's quiet NaN. */
	if (left_number.classification == QUAD_NAN ||
	    right_number.classification == QUAD_NAN)
		return quad_nan();

	/* Zero over zero and infinity over infinity have no value. */
	if ((left_number.classification == QUAD_ZERO &&
	    right_number.classification == QUAD_ZERO) ||
	    (left_number.classification == QUAD_INFINITY &&
	    right_number.classification == QUAD_INFINITY)) {
		(void)feraiseexcept(FE_INVALID);

		return quad_nan();
	}

	/* An infinite dividend gives a signed infinity. */
	if (left_number.classification == QUAD_INFINITY)
		return quad_infinity(sign);

	/* An infinite divisor, or a zero dividend, gives a signed zero. */
	if (right_number.classification == QUAD_INFINITY ||
	    left_number.classification == QUAD_ZERO)
		return quad_signed_zero(sign);

	/* A zero divisor gives a signed infinity and raises the flag. */
	if (right_number.classification == QUAD_ZERO) {
		(void)feraiseexcept(FE_DIVBYZERO);

		return quad_infinity(sign);
	}

	/* Places the dividend above the divisor so the first bit is one. */
	quad_normalize(&left_number);
	quad_normalize(&right_number);
	exponent = left_number.exponent - right_number.exponent;
	if (quad_compare_uint(left_number.significand,
	    right_number.significand) < 0) {
		quad_shift_left_one(&left_number.significand);
		exponent--;
	}

	/* Produces one quotient bit per pass by restoring division. */
	remainder = left_number.significand;
	quad_sub_uint(&remainder, right_number.significand);
	for (count = 0U; count < QUAD_GUARDED_BIT; count++) {
		quad_shift_left_one(&quotient);
		quad_shift_left_one(&remainder);
		if (quad_compare_uint(remainder,
		    right_number.significand) >= 0) {
			quad_sub_uint(&remainder, right_number.significand);
			quotient.limb[0] |= 1U;
		}
	}

	/* A remainder that never cleared makes the quotient sticky. */
	if (!quad_zero(remainder))
		quotient.limb[0] |= 1U;

	/* Rounds the guarded quotient into the format. */
	return quad_pack(sign, exponent, quotient);
}

/*
 * Orders two binary128 values, reporting whether they compare at all.
 *
 * The result is negative, zero or positive as the left operand is less than,
 * equal to or greater than the right one.  A NaN on either side makes the
 * pair unordered, which the caller must test before reading the result.
 */
int
zsf128_compare(
	struct zsf128 left,
	struct zsf128 right,
	int *unordered)
{
	struct quad_number left_number;
	struct quad_number right_number;
	uint64_t left_high;
	uint64_t right_high;
	int magnitude;

	/* Takes the magnitudes, which order like integers within a sign. */
	left_number = quad_unpack(left);
	right_number = quad_unpack(right);
	left_high = left.high & UINT64_C(0x7fffffffffffffff);
	right_high = right.high & UINT64_C(0x7fffffffffffffff);

	/* A NaN on either side leaves the pair unordered. */
	*unordered = left_number.classification == QUAD_NAN ||
	    right_number.classification == QUAD_NAN;
	if (*unordered)
		return 0;

	/* The two zeroes compare equal despite their different signs. */
	if ((left_high | left.low) == 0U && (right_high | right.low) == 0U)
		return 0;

	/* A difference in sign settles the order by itself. */
	if (left_number.sign != right_number.sign)
		return left_number.sign ? -1 : 1;

	/* Within one sign the magnitudes order like a 127-bit integer. */
	if (left_high == right_high)
		magnitude = left.low == right.low ?
		    0 : (left.low < right.low ? -1 : 1);
	else
		magnitude = left_high < right_high ? -1 : 1;

	/* Two negative values order the other way round. */
	return left_number.sign ? -magnitude : magnitude;
}

/*
 * Widens a binary32 value to binary128.
 */
struct zsf128
zsf32_to_128(
	uint32_t value)
{
	unsigned int exponent_field;
	unsigned int sign;
	uint64_t fraction;

	/* Takes the three fields apart. */
	exponent_field = (value >> 23) & 0xffU;
	sign = value >> 31;
	fraction = value & 0x7fffffU;

	/* An all-ones exponent is an infinity or a quiet NaN. */
	if (exponent_field == 0xffU)
		return (struct zsf128){ ((uint64_t)sign << 63) |
		    UINT64_C(0x7fff000000000000) |
		    (fraction ? UINT64_C(0x0000800000000000) : 0), 0 };

	/* A zero exponent with no fraction is a signed zero. */
	if (exponent_field == 0U && fraction == 0U)
		return quad_signed_zero(sign);

	/* A subnormal is shifted up until its leading bit is in place. */
	if (exponent_field == 0U) {
		exponent_field = 1U;
		while ((fraction & 0x800000U) == 0U) {
			fraction <<= 1;
			exponent_field--;
		}
	} else {
		fraction |= 0x800000U;
	}

	/* Every binary32 value is exact in binary128. */
	return small_to_quad(fraction, (int)exponent_field - 127, 23U, sign);
}

/*
 * Widens a binary64 value to binary128.
 */
struct zsf128
zsf64_to_128(
	uint64_t value)
{
	unsigned int exponent_field;
	unsigned int sign;
	uint64_t fraction;

	/* Takes the three fields apart. */
	exponent_field = (unsigned int)((value >> 52) & 0x7ffU);
	sign = (unsigned int)(value >> 63);
	fraction = value & UINT64_C(0xfffffffffffff);

	/* An all-ones exponent is an infinity or a quiet NaN. */
	if (exponent_field == 0x7ffU)
		return (struct zsf128){ ((uint64_t)sign << 63) |
		    UINT64_C(0x7fff000000000000) |
		    (fraction ? UINT64_C(0x0000800000000000) : 0), 0 };

	/* A zero exponent with no fraction is a signed zero. */
	if (exponent_field == 0U && fraction == 0U)
		return quad_signed_zero(sign);

	/* A subnormal is shifted up until its leading bit is in place. */
	if (exponent_field == 0U) {
		exponent_field = 1U;
		while ((fraction & UINT64_C(0x10000000000000)) == 0U) {
			fraction <<= 1;
			exponent_field--;
		}
	} else {
		fraction |= UINT64_C(0x10000000000000);
	}

	/* Every binary64 value is exact in binary128. */
	return small_to_quad(fraction, (int)exponent_field - 1023, 52U, sign);
}

/*
 * Narrows a binary128 value to binary32.
 */
uint32_t
zsf128_to_32(
	struct zsf128 value)
{
	struct quad_number number;

	/* Classifies the operand. */
	number = quad_unpack(value);

	/* The three special classes convert to their binary32 counterparts. */
	if (number.classification == QUAD_NAN)
		return UINT32_C(0x7fc00000) | (number.sign << 31);
	if (number.classification == QUAD_INFINITY)
		return UINT32_C(0x7f800000) | (number.sign << 31);
	if (number.classification == QUAD_ZERO)
		return number.sign << 31;

	/* Brings the significand onto binary32's guarded scale. */
	quad_normalize(&number);
	quad_shift_right_jam(&number.significand, 86U);

	/* Rounds it into binary32 where that format's rounding lives. */
	return zsf32_round_pack(number.sign, number.exponent,
	    number.significand.limb[0]);
}

/*
 * Narrows a binary128 value to binary64.
 */
uint64_t
zsf128_to_64(
	struct zsf128 value)
{
	struct quad_number number;

	/* Classifies the operand. */
	number = quad_unpack(value);

	/* The three special classes convert to their binary64 counterparts. */
	if (number.classification == QUAD_NAN)
		return UINT64_C(0x7ff8000000000000) |
		    ((uint64_t)number.sign << 63);
	if (number.classification == QUAD_INFINITY)
		return UINT64_C(0x7ff0000000000000) |
		    ((uint64_t)number.sign << 63);
	if (number.classification == QUAD_ZERO)
		return (uint64_t)number.sign << 63;

	/* Brings the significand onto binary64's guarded scale. */
	quad_normalize(&number);
	quad_shift_right_jam(&number.significand, 57U);

	/* Rounds it into binary64 where that format's rounding lives. */
	return zsf64_round_pack(number.sign, number.exponent,
	    number.significand.limb[0]);
}

/*
 * Converts a signed integer to binary128.
 */
struct zsf128
zsf_i64_to_128(
	int64_t value)
{
	uint64_t magnitude;
	struct zsf128 result;

	/*
	 * Negating the most negative value directly would overflow, so the
	 * magnitude is built from one more than the negated successor.
	 */
	if (value < 0)
		magnitude = (uint64_t)(-(value + 1)) + 1U;
	else
		magnitude = (uint64_t)value;

	/* Converts the magnitude and applies the sign afterwards. */
	result = zsf_u64_to_128(magnitude);
	if (value < 0)
		result.high |= UINT64_C(0x8000000000000000);

	return result;
}

/*
 * Converts an unsigned integer to binary128.
 */
struct zsf128
zsf_u64_to_128(
	uint64_t value)
{
	struct quad_uint significand;
	uint64_t scan;
	int highest;

	/* A zero magnitude is a positive zero. */
	if (value == 0U)
		return (struct zsf128){ 0, 0 };

	/* Finds the position of the highest set bit, which is the exponent. */
	highest = 0;
	scan = value;
	while (scan >>= 1)
		highest++;

	/*
	 * Every 64-bit integer fits exactly in the 113-bit significand, so
	 * the magnitude only has to be shifted onto the guarded scale.
	 */
	significand.limb[0] = value;
	significand.limb[1] = 0U;
	significand.limb[2] = 0U;
	significand.limb[3] = 0U;
	quad_shift_left(&significand,
	    (unsigned int)((int)QUAD_GUARDED_BIT - highest));

	/* Packs the exact value. */
	return quad_pack(0U, highest, significand);
}

/* Tests whether a 256-bit value is zero. */
static int
quad_zero(
	struct quad_uint value)
{
	/* Every limb has to be empty. */
	return (value.limb[0] | value.limb[1] |
	    value.limb[2] | value.limb[3]) == 0U;
}

/* Orders two 256-bit values, reporting -1, 0 or 1. */
static int
quad_compare_uint(
	struct quad_uint left,
	struct quad_uint right)
{
	int index;

	/* The most significant limb that differs settles the order. */
	for (index = (int)QUAD_LIMBS - 1; index >= 0; index--) {
		if (left.limb[index] != right.limb[index])
			return left.limb[index] < right.limb[index] ? -1 : 1;
	}

	/* Every limb matched. */
	return 0;
}

/* Shifts a 256-bit value left by one bit. */
static void
quad_shift_left_one(
	struct quad_uint *value)
{
	unsigned int index;
	uint64_t carry;
	uint64_t next;

	/* Carries the top bit of each limb into the one above it. */
	carry = 0U;
	for (index = 0U; index < QUAD_LIMBS; index++) {
		next = value->limb[index] >> 63;
		value->limb[index] = (value->limb[index] << 1) | carry;
		carry = next;
	}
}

/* Shifts a 256-bit value left by a distance. */
static void
quad_shift_left(
	struct quad_uint *value,
	unsigned int distance)
{
	/* The distances used here are small, so one bit at a time will do. */
	while (distance-- != 0U)
		quad_shift_left_one(value);
}

/*
 * Shifts a 256-bit value right, keeping a sticky bit.
 *
 * The lowest bit of the result records whether any one bit was shifted out,
 * which is what lets the rounding stay exact.
 */
static void
quad_shift_right_jam(
	struct quad_uint *value,
	unsigned int distance)
{
	unsigned int index;
	uint64_t carry;
	uint64_t next;
	int jam;

	/* A shift past the width leaves nothing but the sticky bit. */
	jam = 0;
	if (distance >= QUAD_WIDTH) {
		jam = !quad_zero(*value);
		value->limb[0] = (uint64_t)jam;
		value->limb[1] = 0U;
		value->limb[2] = 0U;
		value->limb[3] = 0U;

		return;
	}

	/* Otherwise each pass remembers the bit it drops. */
	while (distance-- != 0U) {
		jam |= (value->limb[0] & 1U) != 0U;
		carry = 0U;
		for (index = QUAD_LIMBS; index-- != 0U; ) {
			next = value->limb[index] << 63;
			value->limb[index] = (value->limb[index] >> 1) | carry;
			carry = next;
		}
	}

	/* Publishes whatever was dropped as the sticky bit. */
	if (jam)
		value->limb[0] |= 1U;
}

/* Shifts a 256-bit value right by a distance, dropping what falls off. */
static void
quad_shift_right(
	struct quad_uint *value,
	unsigned int distance)
{
	unsigned int index;
	uint64_t carry;
	uint64_t next;

	/* Carries the low bit of each limb into the one below it. */
	while (distance-- != 0U) {
		carry = 0U;
		for (index = QUAD_LIMBS; index-- != 0U; ) {
			next = value->limb[index] << 63;
			value->limb[index] = (value->limb[index] >> 1) | carry;
			carry = next;
		}
	}
}

/* Adds one 256-bit value into another. */
static void
quad_add_uint(
	struct quad_uint *left,
	struct quad_uint right)
{
	unsigned int index;
	uint64_t augend;
	uint64_t sum;
	uint64_t carry;

	/*
	 * A limb carries when the sum wrapped, and also when an incoming
	 * carry made the sum exactly equal to the augend.
	 */
	carry = 0U;
	for (index = 0U; index < QUAD_LIMBS; index++) {
		augend = left->limb[index];
		sum = augend + right.limb[index] + carry;
		left->limb[index] = sum;
		carry = (sum < augend) | (carry && sum == augend);
	}
}

/* Subtracts one 256-bit value from another. */
static void
quad_sub_uint(
	struct quad_uint *left,
	struct quad_uint right)
{
	unsigned int index;
	uint64_t minuend;
	uint64_t subtrahend;
	uint64_t borrow;

	/*
	 * A limb borrows when the subtrahend was larger, and also when an
	 * incoming borrow made the subtrahend wrap to zero.
	 */
	borrow = 0U;
	for (index = 0U; index < QUAD_LIMBS; index++) {
		minuend = left->limb[index];
		subtrahend = right.limb[index] + borrow;
		left->limb[index] = minuend - subtrahend;
		borrow = (minuend < subtrahend) |
		    (borrow && subtrahend == 0U);
	}
}

/* Reports the position of the highest set bit, or -1 for zero. */
static int
quad_highest_bit(
	struct quad_uint value)
{
	int limb;
	int bit;
	uint64_t remaining;

	/* Searches downwards for the first limb that holds anything. */
	for (limb = (int)QUAD_LIMBS - 1; limb >= 0; limb--) {
		if (value.limb[limb] == 0U)
			continue;

		/* Counts the shifts that limb's highest bit survives. */
		remaining = value.limb[limb];
		bit = 0;
		while (remaining >>= 1)
			bit++;

		return limb * 64 + bit;
	}

	/* Reports that the value is zero. */
	return -1;
}

/* Reports one bit of a 256-bit value. */
static int
quad_bit(
	struct quad_uint value,
	unsigned int bit)
{
	/* Selects the limb and then the bit inside it. */
	return (int)((value.limb[bit / 64U] >> (bit % 64U)) & 1U);
}

/* Sets one bit of a 256-bit value. */
static void
quad_set_bit(
	struct quad_uint *value,
	unsigned int bit)
{
	/* Selects the limb and then the bit inside it. */
	value->limb[bit / 64U] |= UINT64_C(1) << (bit % 64U);
}

/* Splits a representation into its sign, exponent, significand and class. */
static struct quad_number
quad_unpack(
	struct zsf128 bits)
{
	struct quad_number number;
	unsigned int exponent_field;

	/* Takes the three fields apart. */
	number.significand.limb[0] = bits.low;
	number.significand.limb[1] = bits.high & UINT64_C(0x0000ffffffffffff);
	number.significand.limb[2] = 0U;
	number.significand.limb[3] = 0U;
	number.exponent = 0;
	number.sign = (unsigned int)(bits.high >> 63);
	number.classification = QUAD_FINITE;
	exponent_field = (unsigned int)((bits.high >> 48) & 0x7fffU);

	/* An all-ones exponent is an infinity or a NaN. */
	if (exponent_field == 0x7fffU) {
		number.classification = quad_zero(number.significand) ?
		    QUAD_INFINITY : QUAD_NAN;

		return number;
	}

	/*
	 * A zero exponent is a zero or a subnormal.  The subnormal takes the
	 * exponent of the smallest normal, so normalization can shift its
	 * significand up into place.
	 */
	if (exponent_field == 0U) {
		number.exponent = QUAD_MINIMUM_EXPONENT;
		number.classification = quad_zero(number.significand) ?
		    QUAD_ZERO : QUAD_FINITE;

		return number;
	}

	/* Anything else is normal, and carries a hidden leading bit. */
	number.exponent = (int)exponent_field - QUAD_BIAS;
	quad_set_bit(&number.significand, QUAD_HIDDEN_BIT);

	return number;
}

/* Shifts a subnormal significand up until its hidden bit is in place. */
static void
quad_normalize(
	struct quad_number *number)
{
	int highest;

	/* Each shift buys one bit of significand at the cost of an exponent. */
	highest = quad_highest_bit(number->significand);
	if (highest >= 0 && highest < (int)QUAD_HIDDEN_BIT) {
		quad_shift_left(&number->significand,
		    (unsigned int)((int)QUAD_HIDDEN_BIT - highest));
		number->exponent -= (int)QUAD_HIDDEN_BIT - highest;
	}
}

/* Reports the quiet NaN this unit produces for an invalid operation. */
static struct zsf128
quad_nan(
	void)
{
	/* An all-ones exponent with the quiet bit set and no payload. */
	return (struct zsf128){ UINT64_C(0x7fff800000000000), 0 };
}

/* Reports the infinity of the given sign. */
static struct zsf128
quad_infinity(
	unsigned int sign)
{
	/* An all-ones exponent with an empty significand. */
	return (struct zsf128){ ((uint64_t)sign << 63) |
	    UINT64_C(0x7fff000000000000), 0 };
}

/* Reports the zero of the given sign. */
static struct zsf128
quad_signed_zero(
	unsigned int sign)
{
	/* Nothing but the sign bit. */
	return (struct zsf128){ (uint64_t)sign << 63, 0 };
}

/*
 * Rounds a guarded significand and packs it into a representation.
 *
 * The significand arrives with three bits below the hidden bit: a guard bit,
 * a round bit, and a sticky bit.  Rounding is to nearest, ties to even, and
 * every flag IEEE 754 requires is raised here.
 */
static struct zsf128
quad_pack(
	unsigned int sign,
	int exponent,
	struct quad_uint significand)
{
	struct quad_uint one;
	uint64_t round_bits;
	uint64_t exponent_field;
	unsigned int distance;
	int highest;

	/* An empty significand is a signed zero, whatever the exponent says. */
	highest = quad_highest_bit(significand);
	if (highest < 0)
		return quad_signed_zero(sign);

	/* Brings the significand onto the guarded scale. */
	if (highest > (int)QUAD_GUARDED_BIT) {
		quad_shift_right_jam(&significand,
		    (unsigned int)(highest - (int)QUAD_GUARDED_BIT));
		exponent += highest - (int)QUAD_GUARDED_BIT;
	} else if (highest < (int)QUAD_GUARDED_BIT &&
	    exponent > QUAD_MINIMUM_EXPONENT) {
		distance = (unsigned int)((int)QUAD_GUARDED_BIT - highest);
		if (exponent - (int)distance < QUAD_MINIMUM_EXPONENT)
			distance = (unsigned int)(exponent -
			    QUAD_MINIMUM_EXPONENT);
		quad_shift_left(&significand, distance);
		exponent -= (int)distance;
	}

	/* An exponent below the range is represented by shifting instead. */
	if (exponent < QUAD_MINIMUM_EXPONENT) {
		quad_shift_right_jam(&significand,
		    (unsigned int)(QUAD_MINIMUM_EXPONENT - exponent));
		exponent = QUAD_MINIMUM_EXPONENT;
	}

	/* Rounds to nearest, breaking a tie towards an even significand. */
	round_bits = significand.limb[0] & 7U;
	quad_shift_right(&significand, 3U);
	if (round_bits > 4U ||
	    (round_bits == 4U && (significand.limb[0] & 1U) != 0U)) {
		one.limb[0] = 1U;
		one.limb[1] = 0U;
		one.limb[2] = 0U;
		one.limb[3] = 0U;
		quad_add_uint(&significand, one);
	}

	/* Anything dropped makes the result inexact. */
	if (round_bits)
		(void)feraiseexcept(FE_INEXACT);

	/* Rounding up may carry into the next binade. */
	if (quad_bit(significand, QUAD_HIDDEN_BIT + 1U)) {
		quad_shift_right_jam(&significand, 1U);
		exponent++;
	}

	/* An exponent past the range becomes a signed infinity. */
	if (exponent > QUAD_MAXIMUM_EXPONENT) {
		(void)feraiseexcept(FE_OVERFLOW | FE_INEXACT);

		return quad_infinity(sign);
	}

	/*
	 * A result that stayed below the hidden bit at the smallest exponent
	 * is subnormal, and is encoded with a zero exponent field.  Losing
	 * bits there is an underflow as well as an inexact result.
	 */
	if (exponent == QUAD_MINIMUM_EXPONENT &&
	    !quad_bit(significand, QUAD_HIDDEN_BIT)) {
		exponent_field = 0U;
		if (round_bits)
			(void)feraiseexcept(FE_UNDERFLOW);
	} else {
		exponent_field = (uint64_t)(exponent + QUAD_BIAS);
	}

	/* Assembles the three fields across the two halves. */
	significand.limb[1] &= UINT64_C(0x0000ffffffffffff);

	return (struct zsf128){ ((uint64_t)sign << 63) |
	    (exponent_field << 48) | significand.limb[1],
	    significand.limb[0] };
}

/* Adds two representations, treating subtraction as a flipped sign. */
static struct zsf128
quad_add_bits(
	struct zsf128 left_bits,
	struct zsf128 right_bits)
{
	struct quad_number left;
	struct quad_number right;
	struct quad_number temporary;
	struct quad_uint result;

	/* Classifies both operands. */
	left = quad_unpack(left_bits);
	right = quad_unpack(right_bits);

	/* A NaN operand gives this unit's quiet NaN. */
	if (left.classification == QUAD_NAN ||
	    right.classification == QUAD_NAN)
		return quad_nan();

	/* Opposite infinities are invalid; otherwise the infinity wins. */
	if (left.classification == QUAD_INFINITY ||
	    right.classification == QUAD_INFINITY) {
		if (left.classification == QUAD_INFINITY &&
		    right.classification == QUAD_INFINITY &&
		    left.sign != right.sign) {
			(void)feraiseexcept(FE_INVALID);

			return quad_nan();
		}

		return left.classification == QUAD_INFINITY ?
		    left_bits : right_bits;
	}

	/* Two zeroes of unlike sign make a positive zero. */
	if (left.classification == QUAD_ZERO)
		return right.classification == QUAD_ZERO &&
		    left.sign != right.sign ?
		    (struct zsf128){ 0, 0 } : right_bits;

	/* Adding a zero to anything reports that other operand unchanged. */
	if (right.classification == QUAD_ZERO)
		return left_bits;

	/* Puts the larger magnitude on the left so the result cannot borrow. */
	quad_normalize(&left);
	quad_normalize(&right);
	if (left.exponent < right.exponent ||
	    (left.exponent == right.exponent &&
	    quad_compare_uint(left.significand, right.significand) < 0)) {
		temporary = left;
		left = right;
		right = temporary;
	}

	/* Brings the smaller operand onto the larger one's exponent. */
	quad_shift_left(&left.significand, 3U);
	quad_shift_left(&right.significand, 3U);
	quad_shift_right_jam(&right.significand,
	    (unsigned int)(left.exponent - right.exponent));
	result = left.significand;

	/* Like signs add; unlike signs subtract. */
	if (left.sign == right.sign)
		quad_add_uint(&result, right.significand);
	else
		quad_sub_uint(&result, right.significand);

	/* Rounds the guarded result into the format. */
	return quad_pack(left.sign, left.exponent, result);
}

/* Multiplies two significands into an exact 256-bit product. */
static struct quad_uint
quad_multiply_significands(
	struct quad_uint left,
	struct quad_uint right)
{
	struct quad_uint result;
	struct quad_uint shifted;
	unsigned int bit;

	/* Adds a shifted copy of the left operand for each bit of the right. */
	result.limb[0] = 0U;
	result.limb[1] = 0U;
	result.limb[2] = 0U;
	result.limb[3] = 0U;
	shifted = left;
	for (bit = 0U; bit <= QUAD_FRACTION_BITS; bit++) {
		if (quad_bit(right, bit))
			quad_add_uint(&result, shifted);
		quad_shift_left_one(&shifted);
	}

	/* Reports the exact product. */
	return result;
}

/* Packs a narrower format's significand and exponent as binary128. */
static struct zsf128
small_to_quad(
	uint64_t fraction,
	int exponent,
	unsigned int source_fraction_bits,
	unsigned int sign)
{
	struct quad_uint significand;

	/* Moves the fraction onto the guarded scale this format uses. */
	significand.limb[0] = fraction;
	significand.limb[1] = 0U;
	significand.limb[2] = 0U;
	significand.limb[3] = 0U;
	quad_shift_left(&significand,
	    QUAD_GUARDED_BIT - source_fraction_bits);

	/* Packs the exact value. */
	return quad_pack(sign, exponent, significand);
}
