/*
 * Integer-only IEEE 754 binary32/binary64 implementation.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
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

enum zsf_class {
	ZSF_ZERO,
	ZSF_FINITE,
	ZSF_INFINITY,
	ZSF_NAN
};

struct zsf_number {
	uint64_t fraction;
	int exponent;
	unsigned int sign;
	enum zsf_class classification;
};

struct zsf_u128 {
	uint64_t high;
	uint64_t low;
};

struct zsf_format {
	unsigned int fraction_bits;
	unsigned int exponent_bits;
	int bias;
};

static const struct zsf_format zsf_binary32 = { 23U, 8U, 127 };
static const struct zsf_format zsf_binary64 = { 52U, 11U, 1023 };

static uint64_t
zsf_mask(unsigned int bits)
{
	return bits == 64U ? UINT64_MAX : ((UINT64_C(1) << bits) - 1U);
}

static uint64_t
zsf_shift_right_jam(uint64_t value, unsigned int distance)
{
	if (distance == 0U)
		return value;
	if (distance < 64U)
		return (value >> distance) |
		    ((value << (64U - distance)) != 0U);
	return value != 0U;
}

static uint64_t
zsf_u128_shift_right_jam(struct zsf_u128 value, unsigned int distance)
{
	uint64_t result;

	if (distance == 0U)
		return value.low;
	if (distance < 64U) {
		result = (value.high << (64U - distance)) |
		    (value.low >> distance);
		return result | ((value.low << (64U - distance)) != 0U);
	}
	if (distance == 64U)
		return value.high | (value.low != 0U);
	if (distance < 128U)
		return (value.high >> (distance - 64U)) |
		    (((value.high << (128U - distance)) | value.low) != 0U);
	return (value.high | value.low) != 0U;
}

static struct zsf_u128
zsf_multiply64(uint64_t left, uint64_t right)
{
	uint64_t left_low = (uint32_t)left;
	uint64_t left_high = left >> 32;
	uint64_t right_low = (uint32_t)right;
	uint64_t right_high = right >> 32;
	uint64_t low_product = left_low * right_low;
	uint64_t middle1 = left_high * right_low;
	uint64_t middle2 = left_low * right_high;
	uint64_t high_product = left_high * right_high;
	uint64_t carry;
	struct zsf_u128 result;

	carry = (low_product >> 32) + (uint32_t)middle1 +
	    (uint32_t)middle2;
	result.low = (low_product & UINT64_C(0xffffffff)) | (carry << 32);
	result.high = high_product + (middle1 >> 32) + (middle2 >> 32) +
	    (carry >> 32);
	return result;
}

static struct zsf_number
zsf_unpack(uint64_t bits, const struct zsf_format *format)
{
	uint64_t fraction_mask = zsf_mask(format->fraction_bits);
	uint64_t exponent_mask = zsf_mask(format->exponent_bits);
	uint64_t exponent_field =
	    (bits >> format->fraction_bits) & exponent_mask;
	struct zsf_number number;

	number.sign = (unsigned int)(bits >>
	    (format->fraction_bits + format->exponent_bits));
	number.fraction = bits & fraction_mask;
	if (exponent_field == exponent_mask) {
		number.classification = number.fraction == 0U ?
		    ZSF_INFINITY : ZSF_NAN;
		number.exponent = 0;
		return number;
	}
	if (exponent_field == 0U) {
		number.classification = number.fraction == 0U ?
		    ZSF_ZERO : ZSF_FINITE;
		number.exponent = 1 - format->bias;
		return number;
	}
	number.classification = ZSF_FINITE;
	number.exponent = (int)exponent_field - format->bias;
	number.fraction |= UINT64_C(1) << format->fraction_bits;
	return number;
}

static uint64_t
zsf_default_nan(const struct zsf_format *format)
{
	return (zsf_mask(format->exponent_bits) << format->fraction_bits) |
	    (UINT64_C(1) << (format->fraction_bits - 1U));
}

static uint64_t
zsf_propagate_nan(uint64_t left, uint64_t right,
    const struct zsf_format *format)
{
	uint64_t exponent_mask = zsf_mask(format->exponent_bits) <<
	    format->fraction_bits;
	uint64_t fraction_mask = zsf_mask(format->fraction_bits);
	uint64_t quiet = UINT64_C(1) << (format->fraction_bits - 1U);
	uint64_t selected = (left & fraction_mask) != 0U &&
	    (left & exponent_mask) == exponent_mask ? left : right;

	if ((selected & quiet) == 0U)
		(void)feraiseexcept(FE_INVALID);
	return (selected | quiet) &
	    zsf_mask(1U + format->exponent_bits + format->fraction_bits);
}

static void
zsf_normalize(struct zsf_number *number, const struct zsf_format *format)
{
	uint64_t hidden = UINT64_C(1) << format->fraction_bits;

	while (number->fraction != 0U && number->fraction < hidden) {
		number->fraction <<= 1;
		number->exponent--;
	}
}

static uint64_t
zsf_round_pack(unsigned int sign, int exponent, uint64_t significand,
    const struct zsf_format *format)
{
	uint64_t hidden = UINT64_C(1) << format->fraction_bits;
	uint64_t overflow_bit = hidden << 1;
	uint64_t rounded;
	uint64_t round_bits;
	uint64_t exponent_field;
	int minimum_exponent = 1 - format->bias;
	int maximum_exponent = format->bias;

	if (significand == 0U)
		return (uint64_t)sign <<
		    (format->fraction_bits + format->exponent_bits);
	while (significand >= (overflow_bit << 3)) {
		significand = zsf_shift_right_jam(significand, 1U);
		exponent++;
	}
	while (significand < (hidden << 3) && exponent > minimum_exponent) {
		significand <<= 1;
		exponent--;
	}
	if (exponent < minimum_exponent) {
		significand = zsf_shift_right_jam(significand,
		    (unsigned int)(minimum_exponent - exponent));
		exponent = minimum_exponent;
	}
	round_bits = significand & 7U;
	rounded = significand >> 3;
	if (round_bits > 4U || (round_bits == 4U && (rounded & 1U) != 0U))
		rounded++;
	if (round_bits != 0U)
		(void)feraiseexcept(FE_INEXACT);
	if (rounded >= overflow_bit) {
		rounded >>= 1;
		exponent++;
	}
	if (exponent > maximum_exponent) {
		(void)feraiseexcept(FE_OVERFLOW | FE_INEXACT);
		return ((uint64_t)sign <<
		    (format->fraction_bits + format->exponent_bits)) |
		    (zsf_mask(format->exponent_bits) << format->fraction_bits);
	}
	if (exponent == minimum_exponent && rounded < hidden) {
		exponent_field = 0U;
		if (round_bits != 0U)
			(void)feraiseexcept(FE_UNDERFLOW);
	} else {
		exponent_field = (uint64_t)(exponent + format->bias);
	}
	return ((uint64_t)sign <<
	    (format->fraction_bits + format->exponent_bits)) |
	    (exponent_field << format->fraction_bits) |
	    (rounded & (hidden - 1U));
}

static uint64_t
zsf_add(uint64_t left_bits, uint64_t right_bits,
    const struct zsf_format *format)
{
	struct zsf_number left = zsf_unpack(left_bits, format);
	struct zsf_number right = zsf_unpack(right_bits, format);
	struct zsf_number temporary;
	uint64_t left_significand;
	uint64_t right_significand;
	uint64_t result;
	unsigned int sign;

	if (left.classification == ZSF_NAN || right.classification == ZSF_NAN)
		return zsf_propagate_nan(left_bits, right_bits, format);
	if (left.classification == ZSF_INFINITY ||
	    right.classification == ZSF_INFINITY) {
		if (left.classification == ZSF_INFINITY &&
		    right.classification == ZSF_INFINITY &&
		    left.sign != right.sign) {
			(void)feraiseexcept(FE_INVALID);
			return zsf_default_nan(format);
		}
		return left.classification == ZSF_INFINITY ? left_bits : right_bits;
	}
	if (left.classification == ZSF_ZERO && right.classification == ZSF_ZERO)
		return left.sign == right.sign ? left_bits : 0U;
	if (left.classification == ZSF_ZERO)
		return right_bits;
	if (right.classification == ZSF_ZERO)
		return left_bits;
	zsf_normalize(&left, format);
	zsf_normalize(&right, format);
	if (left.exponent < right.exponent ||
	    (left.exponent == right.exponent && left.fraction < right.fraction)) {
		temporary = left;
		left = right;
		right = temporary;
	}
	left_significand = left.fraction << 3;
	right_significand = zsf_shift_right_jam(right.fraction << 3,
	    (unsigned int)(left.exponent - right.exponent));
	sign = left.sign;
	if (left.sign == right.sign) {
		result = left_significand + right_significand;
	} else {
		result = left_significand - right_significand;
		if (result == 0U)
			return 0U;
	}
	return zsf_round_pack(sign, left.exponent, result, format);
}

static uint64_t
zsf_multiply(uint64_t left_bits, uint64_t right_bits,
    const struct zsf_format *format)
{
	struct zsf_number left = zsf_unpack(left_bits, format);
	struct zsf_number right = zsf_unpack(right_bits, format);
	struct zsf_u128 product;
	unsigned int sign = left.sign ^ right.sign;

	if (left.classification == ZSF_NAN || right.classification == ZSF_NAN)
		return zsf_propagate_nan(left_bits, right_bits, format);
	if ((left.classification == ZSF_INFINITY &&
	    right.classification == ZSF_ZERO) ||
	    (right.classification == ZSF_INFINITY &&
	    left.classification == ZSF_ZERO)) {
		(void)feraiseexcept(FE_INVALID);
		return zsf_default_nan(format);
	}
	if (left.classification == ZSF_INFINITY ||
	    right.classification == ZSF_INFINITY)
		return ((uint64_t)sign <<
		    (format->fraction_bits + format->exponent_bits)) |
		    (zsf_mask(format->exponent_bits) << format->fraction_bits);
	if (left.classification == ZSF_ZERO || right.classification == ZSF_ZERO)
		return (uint64_t)sign <<
		    (format->fraction_bits + format->exponent_bits);
	zsf_normalize(&left, format);
	zsf_normalize(&right, format);
	product = zsf_multiply64(left.fraction, right.fraction);
	return zsf_round_pack(sign, left.exponent + right.exponent,
	    zsf_u128_shift_right_jam(product, format->fraction_bits - 3U),
	    format);
}

static uint64_t
zsf_divide(uint64_t left_bits, uint64_t right_bits,
    const struct zsf_format *format)
{
	struct zsf_number left = zsf_unpack(left_bits, format);
	struct zsf_number right = zsf_unpack(right_bits, format);
	uint64_t quotient = 1U;
	uint64_t remainder;
	unsigned int count;
	unsigned int sign = left.sign ^ right.sign;
	int exponent;

	if (left.classification == ZSF_NAN || right.classification == ZSF_NAN)
		return zsf_propagate_nan(left_bits, right_bits, format);
	if ((left.classification == ZSF_ZERO && right.classification == ZSF_ZERO) ||
	    (left.classification == ZSF_INFINITY &&
	    right.classification == ZSF_INFINITY)) {
		(void)feraiseexcept(FE_INVALID);
		return zsf_default_nan(format);
	}
	if (left.classification == ZSF_INFINITY)
		return ((uint64_t)sign <<
		    (format->fraction_bits + format->exponent_bits)) |
		    (zsf_mask(format->exponent_bits) << format->fraction_bits);
	if (right.classification == ZSF_INFINITY || left.classification == ZSF_ZERO)
		return (uint64_t)sign <<
		    (format->fraction_bits + format->exponent_bits);
	if (right.classification == ZSF_ZERO) {
		(void)feraiseexcept(FE_DIVBYZERO);
		return ((uint64_t)sign <<
		    (format->fraction_bits + format->exponent_bits)) |
		    (zsf_mask(format->exponent_bits) << format->fraction_bits);
	}
	zsf_normalize(&left, format);
	zsf_normalize(&right, format);
	exponent = left.exponent - right.exponent;
	if (left.fraction < right.fraction) {
		left.fraction <<= 1;
		exponent--;
	}
	remainder = left.fraction - right.fraction;
	for (count = 0U; count < format->fraction_bits + 3U; count++) {
		quotient <<= 1;
		remainder <<= 1;
		if (remainder >= right.fraction) {
			remainder -= right.fraction;
			quotient |= 1U;
		}
	}
	if (remainder != 0U)
		quotient |= 1U;
	return zsf_round_pack(sign, exponent, quotient, format);
}

static int
zsf_compare_bits(uint64_t left_bits, uint64_t right_bits,
    const struct zsf_format *format, int *unordered)
{
	struct zsf_number left = zsf_unpack(left_bits, format);
	struct zsf_number right = zsf_unpack(right_bits, format);
	uint64_t magnitude_mask = zsf_mask(format->fraction_bits +
	    format->exponent_bits);
	uint64_t left_magnitude = left_bits & magnitude_mask;
	uint64_t right_magnitude = right_bits & magnitude_mask;

	*unordered = left.classification == ZSF_NAN ||
	    right.classification == ZSF_NAN;
	if (*unordered)
		return 0;
	if (left_magnitude == 0U && right_magnitude == 0U)
		return 0;
	if (left.sign != right.sign)
		return left.sign ? -1 : 1;
	if (left_magnitude == right_magnitude)
		return 0;
	if (left.sign)
		return left_magnitude > right_magnitude ? -1 : 1;
	return left_magnitude < right_magnitude ? -1 : 1;
}

static uint64_t
zsf_convert_format(uint64_t bits, const struct zsf_format *source,
    const struct zsf_format *destination)
{
	struct zsf_number number = zsf_unpack(bits, source);
	uint64_t significand;
	unsigned int total_destination = destination->fraction_bits +
	    destination->exponent_bits;

	if (number.classification == ZSF_NAN)
		return zsf_default_nan(destination) |
		    ((uint64_t)number.sign << total_destination);
	if (number.classification == ZSF_INFINITY)
		return ((uint64_t)number.sign << total_destination) |
		    (zsf_mask(destination->exponent_bits) <<
		    destination->fraction_bits);
	if (number.classification == ZSF_ZERO)
		return (uint64_t)number.sign << total_destination;
	zsf_normalize(&number, source);
	if (source->fraction_bits <= destination->fraction_bits + 3U)
		significand = number.fraction <<
		    (destination->fraction_bits + 3U - source->fraction_bits);
	else
		significand = zsf_shift_right_jam(number.fraction,
		    source->fraction_bits - destination->fraction_bits - 3U);
	return zsf_round_pack(number.sign, number.exponent, significand,
	    destination);
}

static uint64_t
zsf_unsigned_to_format(uint64_t value, unsigned int sign,
    const struct zsf_format *format)
{
	unsigned int highest = 0U;
	uint64_t scan = value;
	uint64_t significand;

	if (value == 0U)
		return (uint64_t)sign <<
		    (format->fraction_bits + format->exponent_bits);
	while (scan >>= 1)
		highest++;
	if (highest <= format->fraction_bits + 3U)
		significand = value << (format->fraction_bits + 3U - highest);
	else
		significand = zsf_shift_right_jam(value,
		    highest - format->fraction_bits - 3U);
	return zsf_round_pack(sign, (int)highest, significand, format);
}

static int64_t
zsf_to_integer(uint64_t bits, const struct zsf_format *format,
    unsigned int width, int unsigned_result)
{
	struct zsf_number number = zsf_unpack(bits, format);
	uint64_t magnitude;
	uint64_t maximum = width == 64U ? UINT64_MAX :
	    ((UINT64_C(1) << width) - 1U);
	int shift;

	if (number.classification != ZSF_FINITE) {
		(void)feraiseexcept(FE_INVALID);
		return unsigned_result ? (int64_t)maximum :
		    (number.sign ? INT64_MIN : INT64_MAX);
	}
	zsf_normalize(&number, format);
	if (number.exponent < 0)
		return 0;
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
	if (unsigned_result) {
		if (number.sign && magnitude != 0U) {
			(void)feraiseexcept(FE_INVALID);
			return 0;
		}
		return (int64_t)magnitude;
	}
	maximum = width == 64U ? UINT64_C(0x7fffffffffffffff) :
	    ((UINT64_C(1) << (width - 1U)) - 1U);
	if ((!number.sign && magnitude > maximum) ||
	    (number.sign && magnitude > maximum + 1U)) {
		(void)feraiseexcept(FE_INVALID);
		return number.sign ? INT64_MIN : INT64_MAX;
	}
	if (number.sign)
		return magnitude == (UINT64_C(1) << 63) ? INT64_MIN :
		    -(int64_t)magnitude;
	return (int64_t)magnitude;
}

uint32_t zsf32_add(uint32_t a, uint32_t b)
{ return (uint32_t)zsf_add(a, b, &zsf_binary32); }
uint32_t zsf32_sub(uint32_t a, uint32_t b)
{ return (uint32_t)zsf_add(a, b ^ UINT32_C(0x80000000), &zsf_binary32); }
uint32_t zsf32_mul(uint32_t a, uint32_t b)
{ return (uint32_t)zsf_multiply(a, b, &zsf_binary32); }
uint32_t zsf32_div(uint32_t a, uint32_t b)
{ return (uint32_t)zsf_divide(a, b, &zsf_binary32); }
int zsf32_compare(uint32_t a, uint32_t b, int *u)
{ return zsf_compare_bits(a, b, &zsf_binary32, u); }

uint64_t zsf64_add(uint64_t a, uint64_t b)
{ return zsf_add(a, b, &zsf_binary64); }
uint64_t zsf64_sub(uint64_t a, uint64_t b)
{ return zsf_add(a, b ^ UINT64_C(0x8000000000000000), &zsf_binary64); }
uint64_t zsf64_mul(uint64_t a, uint64_t b)
{ return zsf_multiply(a, b, &zsf_binary64); }
uint64_t zsf64_div(uint64_t a, uint64_t b)
{ return zsf_divide(a, b, &zsf_binary64); }
int zsf64_compare(uint64_t a, uint64_t b, int *u)
{ return zsf_compare_bits(a, b, &zsf_binary64, u); }

uint64_t zsf32_to_64(uint32_t value)
{ return zsf_convert_format(value, &zsf_binary32, &zsf_binary64); }
uint32_t zsf64_to_32(uint64_t value)
{ return (uint32_t)zsf_convert_format(value, &zsf_binary64, &zsf_binary32); }
uint32_t zsf_i64_to_32(int64_t value)
{ return (uint32_t)zsf_unsigned_to_format(value < 0 ? (uint64_t)(-(value + 1)) + 1U : (uint64_t)value, value < 0, &zsf_binary32); }
uint32_t zsf_u64_to_32(uint64_t value)
{ return (uint32_t)zsf_unsigned_to_format(value, 0U, &zsf_binary32); }
uint64_t zsf_i64_to_64(int64_t value)
{ return zsf_unsigned_to_format(value < 0 ? (uint64_t)(-(value + 1)) + 1U : (uint64_t)value, value < 0, &zsf_binary64); }
uint64_t zsf_u64_to_64(uint64_t value)
{ return zsf_unsigned_to_format(value, 0U, &zsf_binary64); }
int64_t zsf32_to_i64(uint32_t value, unsigned int width, int u)
{ return zsf_to_integer(value, &zsf_binary32, width, u); }
int64_t zsf64_to_i64(uint64_t value, unsigned int width, int u)
{ return zsf_to_integer(value, &zsf_binary64, width, u); }
uint32_t zsf32_round_pack(unsigned int sign, int exponent, uint64_t sig)
{ return (uint32_t)zsf_round_pack(sign, exponent, sig, &zsf_binary32); }
uint64_t zsf64_round_pack(unsigned int sign, int exponent, uint64_t sig)
{ return zsf_round_pack(sign, exponent, sig, &zsf_binary64); }
