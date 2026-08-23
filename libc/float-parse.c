/*
 * Floating-point string conversion for zedBSD.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
 *
 * Decimal conversion uses integer long division and passes guard, round and
 * sticky bits directly to the zedBSD IEEE packer.  It therefore performs one
 * rounding step rather than accumulating error through floating arithmetic.
 */

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

#include "src/softfloat/zed-softfloat.h"

#ifndef UINT64_C
#define UINT64_C(value) value##ULL
#endif

#define BIG_LIMBS 160U

struct big_unsigned {
	uint32_t limb[BIG_LIMBS];
	unsigned int used;
};

static void
big_trim(struct big_unsigned *value)
{
	while (value->used != 0U && value->limb[value->used - 1U] == 0U)
		value->used--;
}

static int
big_multiply_small(struct big_unsigned *value, uint32_t multiplier)
{
	uint64_t carry = 0U;
	unsigned int index;

	for (index = 0U; index < value->used; index++) {
		uint64_t product = (uint64_t)value->limb[index] * multiplier + carry;
		value->limb[index] = (uint32_t)product;
		carry = product >> 32;
	}
	if (carry != 0U) {
		if (value->used == BIG_LIMBS)
			return -1;
		value->limb[value->used++] = (uint32_t)carry;
	}
	return 0;
}

static int
big_add_small(struct big_unsigned *value, uint32_t addend)
{
	uint64_t sum = addend;
	unsigned int index = 0U;

	if (sum == 0U && value->used == 0U)
		return 0;
	while (sum != 0U && index < value->used) {
		sum += value->limb[index];
		value->limb[index++] = (uint32_t)sum;
		sum >>= 32;
	}
	if (sum != 0U || value->used == 0U) {
		if (value->used == BIG_LIMBS)
			return -1;
		value->limb[value->used++] = (uint32_t)sum;
	}
	return 0;
}

static unsigned int
big_bit_length(const struct big_unsigned *value)
{
	uint32_t top;
	unsigned int bits;

	if (value->used == 0U)
		return 0U;
	top = value->limb[value->used - 1U];
	bits = (value->used - 1U) * 32U;
	while (top != 0U) {
		bits++;
		top >>= 1;
	}
	return bits;
}

static int
big_compare(const struct big_unsigned *left,
    const struct big_unsigned *right)
{
	unsigned int index;

	if (left->used != right->used)
		return left->used < right->used ? -1 : 1;
	index = left->used;
	while (index-- != 0U) {
		if (left->limb[index] != right->limb[index])
			return left->limb[index] < right->limb[index] ? -1 : 1;
	}
	return 0;
}

static void
big_subtract(struct big_unsigned *left, const struct big_unsigned *right)
{
	uint64_t borrow = 0U;
	unsigned int index;

	for (index = 0U; index < left->used; index++) {
		uint64_t subtrahend = borrow;
		uint64_t original = left->limb[index];
		if (index < right->used)
			subtrahend += right->limb[index];
		left->limb[index] = (uint32_t)(original - subtrahend);
		borrow = original < subtrahend;
	}
	big_trim(left);
}

static int
big_shift_left(struct big_unsigned *value, unsigned int distance)
{
	unsigned int whole = distance / 32U;
	unsigned int part = distance % 32U;
	unsigned int index;
	uint32_t carry = 0U;

	if (value->used == 0U || distance == 0U)
		return 0;
	if (value->used + whole + (part != 0U) > BIG_LIMBS)
		return -1;
	for (index = value->used; index-- != 0U;)
		value->limb[index + whole] = value->limb[index];
	for (index = 0U; index < whole; index++)
		value->limb[index] = 0U;
	value->used += whole;
	if (part != 0U) {
		for (index = whole; index < value->used; index++) {
			uint32_t next = value->limb[index] >> (32U - part);
			value->limb[index] = (value->limb[index] << part) | carry;
			carry = next;
		}
		if (carry != 0U)
			value->limb[value->used++] = carry;
	}
	return 0;
}

static void
big_shift_right_one(struct big_unsigned *value)
{
	uint32_t carry = 0U;
	unsigned int index = value->used;

	while (index-- != 0U) {
		uint32_t next = value->limb[index] << 31;
		value->limb[index] = (value->limb[index] >> 1) | carry;
		carry = next;
	}
	big_trim(value);
}

static int
big_compare_shifted(const struct big_unsigned *left,
    const struct big_unsigned *right, int right_shift)
{
	struct big_unsigned shifted;

	if (right_shift >= 0) {
		shifted = *right;
		if (big_shift_left(&shifted, (unsigned int)right_shift) != 0)
			return -1;
		return big_compare(left, &shifted);
	}
	shifted = *left;
	if (big_shift_left(&shifted, (unsigned int)-right_shift) != 0)
		return 1;
	return big_compare(&shifted, right);
}

static uint64_t
big_divide_u64(struct big_unsigned numerator,
    struct big_unsigned denominator, int *remainder_nonzero)
{
	int shift = (int)big_bit_length(&numerator) -
	    (int)big_bit_length(&denominator);
	uint64_t quotient = 0U;

	if (shift < 0) {
		*remainder_nonzero = numerator.used != 0U;
		return 0U;
	}
	if (big_shift_left(&denominator, (unsigned int)shift) != 0) {
		*remainder_nonzero = 1;
		return 0U;
	}
	for (; shift >= 0; shift--) {
		if (big_compare(&numerator, &denominator) >= 0) {
			big_subtract(&numerator, &denominator);
			if (shift < 64)
				quotient |= UINT64_C(1) << (unsigned int)shift;
		}
		big_shift_right_one(&denominator);
	}
	*remainder_nonzero = numerator.used != 0U;
	return quotient;
}

static uint64_t
decimal_to_bits(struct big_unsigned numerator, int decimal_exponent,
    unsigned int sign, unsigned int fraction_bits, int *range_error)
{
	struct big_unsigned denominator = { { 1U }, 1U };
	unsigned int power;
	int binary_adjust;
	int ratio_exponent;
	int tentative;
	int scale;
	int remainder;
	uint64_t significand;

	if (numerator.used == 0U)
		return (uint64_t)sign << (fraction_bits == 52U ? 63U : 31U);
	if (decimal_exponent >= 0) {
		for (power = 0U; power < (unsigned int)decimal_exponent; power++) {
			if (big_multiply_small(&numerator, 5U) != 0) {
				*range_error = 1;
				return fraction_bits == 52U ?
				    (UINT64_C(0x7ff0000000000000) |
				    ((uint64_t)sign << 63)) :
				    (UINT64_C(0x7f800000) | ((uint64_t)sign << 31));
			}
		}
		binary_adjust = decimal_exponent;
	} else {
		for (power = 0U; power < (unsigned int)-decimal_exponent; power++) {
			if (big_multiply_small(&denominator, 5U) != 0) {
				*range_error = 1;
				return (uint64_t)sign <<
				    (fraction_bits == 52U ? 63U : 31U);
			}
		}
		binary_adjust = decimal_exponent;
	}
	tentative = (int)big_bit_length(&numerator) -
	    (int)big_bit_length(&denominator);
	ratio_exponent = big_compare_shifted(&numerator, &denominator,
	    tentative) >= 0 ? tentative : tentative - 1;
	scale = (int)fraction_bits + 3 - ratio_exponent;
	if (scale >= 0) {
		if (big_shift_left(&numerator, (unsigned int)scale) != 0) {
			*range_error = 1;
			return fraction_bits == 52U ?
			    UINT64_C(0x7ff0000000000000) : UINT64_C(0x7f800000);
		}
	} else if (big_shift_left(&denominator, (unsigned int)-scale) != 0) {
		*range_error = 1;
		return (uint64_t)sign << (fraction_bits == 52U ? 63U : 31U);
	}
	significand = big_divide_u64(numerator, denominator, &remainder);
	if (remainder)
		significand |= 1U;
	if (fraction_bits == 52U)
		return zsf64_round_pack(sign, ratio_exponent + binary_adjust,
		    significand);
	return zsf32_round_pack(sign, ratio_exponent + binary_adjust,
	    significand);
}

static uint64_t
binary_to_bits(struct big_unsigned numerator, int binary_exponent,
    unsigned int sign, unsigned int fraction_bits, int *range_error)
{
	struct big_unsigned denominator = { { 1U }, 1U };
	int value_exponent;
	int scale;
	int remainder;
	uint64_t significand;

	if (numerator.used == 0U)
		return (uint64_t)sign << (fraction_bits == 52U ? 63U : 31U);
	value_exponent = (int)big_bit_length(&numerator) - 1;
	scale = (int)fraction_bits + 3 - value_exponent;
	if (scale >= 0) {
		if (big_shift_left(&numerator, (unsigned int)scale) != 0) {
			*range_error = 1;
			return fraction_bits == 52U ?
			    UINT64_C(0x7ff0000000000000) : UINT64_C(0x7f800000);
		}
	} else if (big_shift_left(&denominator, (unsigned int)-scale) != 0) {
		*range_error = 1;
		return (uint64_t)sign << (fraction_bits == 52U ? 63U : 31U);
	}
	significand = big_divide_u64(numerator, denominator, &remainder);
	if (remainder)
		significand |= 1U;
	if (fraction_bits == 52U)
		return zsf64_round_pack(sign, value_exponent + binary_exponent,
		    significand);
	return zsf32_round_pack(sign, value_exponent + binary_exponent,
	    significand);
}

static int ascii_space(unsigned char c)
{ return c == ' ' || (c >= '\t' && c <= '\r'); }
static int ascii_digit(unsigned char c)
{ return c >= '0' && c <= '9'; }
static unsigned char ascii_lower(unsigned char c)
{ return c >= 'A' && c <= 'Z' ? (unsigned char)(c + ('a' - 'A')) : c; }

static int
ascii_hex_value(unsigned char character)
{
	character = ascii_lower(character);
	if (ascii_digit(character))
		return character - '0';
	if (character >= 'a' && character <= 'f')
		return character - 'a' + 10;
	return -1;
}

static int
word_matches(const char *text, const char *word)
{
	while (*word != '\0') {
		if (ascii_lower((unsigned char)*text++) != (unsigned char)*word++)
			return 0;
	}
	return 1;
}

static uint64_t
parse_float(const char *string, char **end, unsigned int fraction_bits)
{
	const char *original = string;
	const char *cursor;
	const char *exponent_start;
	struct big_unsigned significand = { { 0U }, 0U };
	unsigned int sign = 0U;
	unsigned int digits = 0U;
	unsigned int fractional_digits = 0U;
	unsigned int stored_digits = 0U;
	int saw_point = 0;
	int decimal_exponent = 0;
	int exponent_sign = 1;
	int range_error = 0;
	uint64_t result;

	while (ascii_space((unsigned char)*string))
		string++;
	if (*string == '+' || *string == '-') {
		sign = *string == '-';
		string++;
	}
	if (word_matches(string, "inf")) {
		cursor = string + 3;
		if (word_matches(cursor, "inity"))
			cursor += 5;
		if (end != 0)
			*end = (char *)cursor;
		return (fraction_bits == 52U ? UINT64_C(0x7ff0000000000000) :
		    UINT64_C(0x7f800000)) |
		    ((uint64_t)sign << (fraction_bits == 52U ? 63U : 31U));
	}
	if (word_matches(string, "nan")) {
		cursor = string + 3;
		if (*cursor == '(') {
			const char *payload = cursor + 1;
			while (ascii_digit((unsigned char)*payload) ||
			    (ascii_lower((unsigned char)*payload) >= 'a' &&
			    ascii_lower((unsigned char)*payload) <= 'z') ||
			    *payload == '_')
				payload++;
			if (*payload == ')')
				cursor = payload + 1;
		}
		if (end != 0)
			*end = (char *)cursor;
		return (fraction_bits == 52U ? UINT64_C(0x7ff8000000000000) :
		    UINT64_C(0x7fc00000)) |
		    ((uint64_t)sign << (fraction_bits == 52U ? 63U : 31U));
	}
	if (string[0] == '0' && ascii_lower((unsigned char)string[1]) == 'x') {
		unsigned int hexadecimal_digits = 0U;
		unsigned int fractional_nibbles = 0U;
		int hexadecimal_exponent;

		saw_point = 0;
		for (cursor = string + 2; ; cursor++) {
			int digit = ascii_hex_value((unsigned char)*cursor);
			if (*cursor == '.' && !saw_point) {
				saw_point = 1;
				continue;
			}
			if (digit < 0)
				break;
			hexadecimal_digits++;
			if (saw_point)
				fractional_nibbles++;
			if (big_multiply_small(&significand, 16U) != 0 ||
			    big_add_small(&significand, (uint32_t)digit) != 0)
				range_error = 1;
		}
		if (hexadecimal_digits != 0U) {
			hexadecimal_exponent = -(int)(fractional_nibbles * 4U);
			exponent_start = cursor;
			if (*cursor == 'p' || *cursor == 'P') {
				const char *scan = cursor + 1;
				int explicit_exponent = 0;
				exponent_sign = 1;
				if (*scan == '+' || *scan == '-') {
					exponent_sign = *scan == '-' ? -1 : 1;
					scan++;
				}
				if (ascii_digit((unsigned char)*scan)) {
					cursor = scan;
					while (ascii_digit((unsigned char)*cursor)) {
						if (explicit_exponent < 10000)
							explicit_exponent = explicit_exponent * 10 +
							    (*cursor - '0');
						cursor++;
					}
					hexadecimal_exponent += exponent_sign *
					    explicit_exponent;
				} else {
					cursor = exponent_start;
				}
			}
			if (end != 0)
				*end = (char *)cursor;
			if (range_error || hexadecimal_exponent > 20000) {
				errno = ERANGE;
				return (fraction_bits == 52U ?
				    UINT64_C(0x7ff0000000000000) : UINT64_C(0x7f800000)) |
				    ((uint64_t)sign <<
				    (fraction_bits == 52U ? 63U : 31U));
			}
			if (hexadecimal_exponent < -20000) {
				errno = ERANGE;
				return (uint64_t)sign <<
				    (fraction_bits == 52U ? 63U : 31U);
			}
			result = binary_to_bits(significand, hexadecimal_exponent,
			    sign, fraction_bits, &range_error);
			if (range_error)
				errno = ERANGE;
			return result;
		}
		significand = (struct big_unsigned){ { 0U }, 0U };
	}
	for (cursor = string; ; cursor++) {
		unsigned char character = (unsigned char)*cursor;
		if (character == '.' && !saw_point) {
			saw_point = 1;
			continue;
		}
		if (!ascii_digit(character))
			break;
		digits++;
		if (saw_point)
			fractional_digits++;
		if (stored_digits < 1000U) {
			if (big_multiply_small(&significand, 10U) != 0 ||
			    big_add_small(&significand, character - '0') != 0)
				range_error = 1;
			stored_digits++;
		} else {
			decimal_exponent++;
		}
	}
	if (digits == 0U) {
		if (end != 0)
			*end = (char *)original;
		return 0U;
	}
	decimal_exponent -= (int)fractional_digits;
	exponent_start = cursor;
	if (*cursor == 'e' || *cursor == 'E') {
		const char *scan = cursor + 1;
		int explicit_exponent = 0;
		if (*scan == '+' || *scan == '-') {
			exponent_sign = *scan == '-' ? -1 : 1;
			scan++;
		}
		if (ascii_digit((unsigned char)*scan)) {
			cursor = scan;
			while (ascii_digit((unsigned char)*cursor)) {
				if (explicit_exponent < 10000)
					explicit_exponent = explicit_exponent * 10 +
					    (*cursor - '0');
				cursor++;
			}
			decimal_exponent += exponent_sign * explicit_exponent;
		} else {
			cursor = exponent_start;
		}
	}
	if (end != 0)
		*end = (char *)cursor;
	if (range_error || decimal_exponent > 5000) {
		errno = ERANGE;
		return (fraction_bits == 52U ? UINT64_C(0x7ff0000000000000) :
		    UINT64_C(0x7f800000)) |
		    ((uint64_t)sign << (fraction_bits == 52U ? 63U : 31U));
	}
	if (decimal_exponent < -5000) {
		errno = ERANGE;
		return (uint64_t)sign << (fraction_bits == 52U ? 63U : 31U);
	}
	result = decimal_to_bits(significand, decimal_exponent, sign,
	    fraction_bits, &range_error);
	if (range_error)
		errno = ERANGE;
	return result;
}

double
strtod(const char *string, char **end)
{
	union { uint64_t bits; double value; } result = {
	    parse_float(string, end, 52U) };
	return result.value;
}

float
strtof(const char *string, char **end)
{
	union { uint32_t bits; float value; } result = {
	    (uint32_t)parse_float(string, end, 23U) };
	return result.value;
}

long double
strtold(const char *string, char **end)
{
	return (long double)strtod(string, end);
}
