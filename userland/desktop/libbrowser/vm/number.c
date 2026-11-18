/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Numbers and their decimal text, exactly (plan/ws074/design.md §12.3: the
 * shortest round-trip form is the engine's own, not the C library's).
 *
 * A double is m * 2^e exactly, so every conversion is done on big integers
 * with no rounding until the one the language asks for:
 *
 *  - Number::toString's digits are the shortest that read back as the same
 *    double (the free-format algorithm of Steele and White, as Burger and
 *    Dybvig give it), ties going to the even digit;
 *  - toFixed, toExponential and toPrecision round the exact value to the
 *    digits asked for, a tie going up;
 *  - a decimal numeral is read to the nearest double, a tie going to the
 *    even one, whatever its length (digits past the 768th only count as
 *    "more than zero").
 *
 * Other radixes use the floating-point method V8 uses (the digits stop once
 * the next one no longer tells the neighbours apart).
 */

#include "vm/internal.h"

#include <errno.h>
#include <math.h>
#include <string.h>

/* The words of a big integer: enough for 10^1200 times 2^4000. */
#define NUMBER_BIG_WORDS	320U

/* The most significant digits of a numeral read exactly (a nonzero digit past them only rounds). */
#define NUMBER_DIGITS_MAX	768U

/* The bits of a double's fraction, and the exponent bias. */
#define NUMBER_FRACTION_BITS	52U
#define NUMBER_EXPONENT_BIAS	1023

/* The lowest binary exponent of a subnormal's unit (2^-1074). */
#define NUMBER_EXPONENT_MIN	(-1074)

/* The biggest radix-10 exponent that toString writes without an exponent. */
#define NUMBER_FIXED_LIMIT	21

/*
 * A big unsigned integer: words, least significant first, count of them
 * in use (a zero has none).  Big integers live on the C stack of the
 * conversion using them.
 */
struct number_big {
	uint32_t count;
	uint32_t words[NUMBER_BIG_WORDS];
};

/*
 * The digits of a number and where its decimal point goes: the value is
 * 0.DIGITS times 10^point.
 */
struct number_digits {
	char digits[800];
	int count;
	int point;
};

static void big_set(struct number_big *big, uint64_t value);
static int big_is_zero(const struct number_big *big);
static uint32_t big_bits(const struct number_big *big);
static int big_compare(const struct number_big *left, const struct number_big *right);
static void big_add(struct number_big *big, const struct number_big *addend);
static void big_subtract(struct number_big *big, const struct number_big *subtrahend);
static void big_shift_left(struct number_big *big, uint32_t bits);
static void big_multiply_small(struct number_big *big, uint32_t factor, uint32_t addend);
static void big_multiply_power10(struct number_big *big, uint32_t exponent);
static uint32_t big_divide_small(struct number_big *big, uint32_t divisor);
static void big_divide(const struct number_big *numerator, const struct number_big *denominator, struct number_big *quotient, struct number_big *remainder);
static int big_bit(const struct number_big *big, uint32_t index);
static uint64_t big_top64(const struct number_big *big, uint32_t *shift, int *sticky);
static void big_decimal(const struct number_big *big, struct number_digits *digits);
static void number_split(double number, uint64_t *fraction, int *exponent);
static void number_shortest(double number, struct number_digits *digits);
static int number_estimate_power(uint64_t fraction, int exponent);
static void number_round_scaled(double number, int power, struct number_big *result);
static double number_from_parts(uint64_t top, int top_exponent, int sticky);
static void number_digits_exact(double number, int count, struct number_digits *digits);
static int number_append_zeros(struct wb_buffer *out, int count);
static int number_write_exponential(const struct number_digits *digits, int negative, struct wb_buffer *out);
static char number_digit_char(int digit);
static int number_digit_value(char character);
static int number_to_radix(double number, int radix, struct wb_buffer *out);
static double number_whole_modulo(double whole, uint32_t divisor);

/*
 * Writes a number as Number::toString does in a radix from 2 to 36.
 */
int
vm_number_to_text(
	double number,
	int radix,
	struct wb_buffer *out)
{
	struct number_digits digits;
	int negative;
	int point;
	int infinite;
	int error;

	/* The special values. */
	if (number != number) {
		error = wb_buffer_append_string(out, "NaN");
		return error;
	}

	/* Zero (either sign). */
	if (number == 0.0) {
		error = wb_buffer_append_string(out, "0");
		return error;
	}

	/* The sign, then the magnitude. */
	negative = 0;
	if (number < 0.0) {
		negative = 1;
		number = -number;
	}

	/* The sign's minus. */
	if (negative) {
		error = wb_buffer_append_byte(out, '-');
		if (error != 0)
			return error;
	}

	/* An infinity. */
	infinite = isinf(number);
	if (infinite) {
		error = wb_buffer_append_string(out, "Infinity");
		return error;
	}

	/* Radix 10: the shortest digits, placed by the language's rules. */
	if (radix == 10) {
		number_shortest(number, &digits);
		point = digits.point;

		/* An integer of up to 21 digits: the digits, then zeros. */
		if (digits.count <= point && point <= NUMBER_FIXED_LIMIT) {
			error = wb_buffer_append(out, digits.digits, (size_t)digits.count);
			if (error == 0)
				error = number_append_zeros(out, point - digits.count);
			return error;
		}

		/* A point inside the digits. */
		if (0 < point && point <= NUMBER_FIXED_LIMIT) {
			error = wb_buffer_append(out, digits.digits, (size_t)point);
			if (error == 0)
				error = wb_buffer_append_byte(out, '.');
			if (error == 0)
				error = wb_buffer_append(out, digits.digits + point, (size_t)(digits.count - point));
			return error;
		}

		/* A small number: 0.000digits. */
		if (-6 < point && point <= 0) {
			error = wb_buffer_append_string(out, "0.");
			if (error == 0)
				error = number_append_zeros(out, -point);
			if (error == 0)
				error = wb_buffer_append(out, digits.digits, (size_t)digits.count);
			return error;
		}

		/* Anything else with an exponent. */
		error = number_write_exponential(&digits, 0, out);
		return error;
	}

	/* Another radix. */
	error = number_to_radix(number, radix, out);
	if (error != 0)
		return error;

	/* Succeeded: the text is written. */
	return 0;
}

/*
 * Writes a number with a fixed count of digits after the point, as
 * toFixed does for a finite number below 10^21 in magnitude.
 */
int
vm_number_to_fixed(
	double number,
	int fraction_digits,
	struct wb_buffer *out)
{
	struct number_big scaled;
	struct number_digits digits;
	int negative;
	int integer_digits;
	int error;

	/* The sign (toFixed writes -0 as 0). */
	negative = 0;
	if (number < 0.0) {
		negative = 1;
		number = -number;
	}

	/* The exact value times 10^f, rounded (a tie up), as decimal digits. */
	number_round_scaled(number, fraction_digits, &scaled);
	big_decimal(&scaled, &digits);

	/* A zero keeps its sign only when it is not zero before rounding. */
	if (negative && (digits.count > 1 || digits.digits[0] != '0' || number != 0.0)) {
		error = wb_buffer_append_byte(out, '-');
		if (error != 0)
			return error;
	}

	/* The integer part (at least 0), then the point and the fraction's digits. */
	integer_digits = digits.count - fraction_digits;
	if (integer_digits <= 0) {
		error = wb_buffer_append_byte(out, '0');
		if (error == 0 && fraction_digits > 0)
			error = wb_buffer_append_byte(out, '.');
		if (error == 0)
			error = number_append_zeros(out, -integer_digits);
		if (error == 0)
			error = wb_buffer_append(out, digits.digits, (size_t)digits.count);
		return error;
	}

	/* The integer part, then the point and the fraction's digits. */
	error = wb_buffer_append(out, digits.digits, (size_t)integer_digits);
	if (error == 0 && fraction_digits > 0) {
		error = wb_buffer_append_byte(out, '.');
		if (error == 0)
			error = wb_buffer_append(out, digits.digits + integer_digits, (size_t)fraction_digits);
	}

	/* Reports a buffer that could not grow. */
	if (error != 0)
		return error;

	/* Succeeded: the text is written. */
	return 0;
}

/*
 * Writes a finite number in exponential form as toExponential does: with
 * fraction_digits digits after the point, or (with -1) as many as the
 * shortest round trip needs.
 */
int
vm_number_to_exponential(
	double number,
	int fraction_digits,
	struct wb_buffer *out)
{
	struct number_digits digits;
	int negative;
	int error;

	/* The sign. */
	negative = 0;
	if (number < 0.0) {
		negative = 1;
		number = -number;
	}

	/* Zero: zeros after the point. */
	if (number == 0.0) {
		digits.digits[0] = '0';
		digits.count = 1;
		if (fraction_digits > 0) {
			memset(digits.digits + 1, '0', (size_t)fraction_digits);
			digits.count += fraction_digits;
		}

		/* The point after the first digit. */
		digits.point = 1;
		error = number_write_exponential(&digits, negative, out);
		return error;
	}

	/* The digits: the shortest, or exactly as many as asked for. */
	if (fraction_digits < 0) {
		number_shortest(number, &digits);
	} else {
		number_digits_exact(number, fraction_digits + 1, &digits);
	}

	/* The form d.ddde+x. */
	error = number_write_exponential(&digits, negative, out);
	if (error != 0)
		return error;

	/* Succeeded: the text is written. */
	return 0;
}

/*
 * Writes a finite number with a count of significant digits as
 * toPrecision does: fixed when its exponent is from -6 to below the
 * precision, exponential otherwise.
 */
int
vm_number_to_precision(
	double number,
	int precision,
	struct wb_buffer *out)
{
	struct number_digits digits;
	int negative;
	int exponent;
	int error;

	/* The sign. */
	negative = 0;
	if (number < 0.0) {
		negative = 1;
		number = -number;
	}

	/* The sign's minus. */
	if (negative) {
		error = wb_buffer_append_byte(out, '-');
		if (error != 0)
			return error;
	}

	/* The digits, exactly as many as the precision (zero is all zeros). */
	if (number == 0.0) {
		memset(digits.digits, '0', (size_t)precision);
		digits.count = precision;
		digits.point = 1;
	} else {
		number_digits_exact(number, precision, &digits);
	}

	/* An exponent outside -6 to the precision writes the exponential form. */
	exponent = digits.point - 1;
	if (exponent < -6 || exponent >= precision) {
		error = number_write_exponential(&digits, 0, out);
		return error;
	}

	/* The fixed form: the point after exponent + 1 digits, or 0.000 before them. */
	if (exponent >= 0) {
		error = wb_buffer_append(out, digits.digits, (size_t)(exponent + 1));
		if (error == 0 && precision > exponent + 1) {
			error = wb_buffer_append_byte(out, '.');
			if (error == 0)
				error = wb_buffer_append(out, digits.digits + exponent + 1, (size_t)(precision - exponent - 1));
		}

		/* Reports a buffer that could not grow, or the text written. */
		return error;
	}

	/* A small number: 0.000digits. */
	error = wb_buffer_append_string(out, "0.");
	if (error == 0)
		error = number_append_zeros(out, -(exponent + 1));
	if (error == 0)
		error = wb_buffer_append(out, digits.digits, (size_t)precision);
	if (error != 0)
		return error;

	/* Succeeded: the text is written. */
	return 0;
}

/*
 * Reads a decimal numeral (digits with an optional point, and an optional
 * exponent: "12.5e-3", ".5", "5."; no sign) to the nearest double.  The
 * caller has checked its form.
 */
double
vm_number_parse(
	const char *text,
	size_t length)
{
	struct number_big value;
	struct number_big scale;
	struct number_big quotient;
	struct number_big remainder;
	uint64_t top;
	uint32_t shift;
	uint32_t value_bits;
	uint32_t scale_bits;
	size_t index;
	long exponent;
	long written_exponent;
	int digits;
	int after_point;
	int dropped;
	int sticky;
	int exponent_sign;
	int binary;
	int power;
	int zero;
	char character;

	/* The significant digits into a big integer, counting those after the point. */
	big_set(&value, 0);
	digits = 0;
	after_point = 0;
	dropped = 0;
	exponent = 0;
	for (index = 0; index < length; index++) {
		character = text[index];
		if (character == '.') {
			after_point = 1;
			continue;
		}

		/* Anything but a digit ends the digits. */
		if (character < '0' || character > '9')
			break;

		/* Leading zeros count for nothing but their place. */
		if (digits == 0 && character == '0') {
			if (after_point)
				exponent--;
			continue;
		}

		/* A digit past the most kept only says the value is above what is kept. */
		if (digits >= (int)NUMBER_DIGITS_MAX) {
			if (character != '0')
				dropped = 1;
			if (!after_point)
				exponent++;
			continue;
		}

		/* The digit joins the value. */
		big_multiply_small(&value, 10U, (uint32_t)(character - '0'));
		digits++;
		if (after_point)
			exponent--;
	}

	/* The written exponent. */
	if (index < length && (text[index] == 'e' || text[index] == 'E')) {
		index++;
		exponent_sign = 1;
		if (index < length && (text[index] == '+' || text[index] == '-')) {
			if (text[index] == '-')
				exponent_sign = -1;
			index++;
		}

		/* The exponent's digits. */
		written_exponent = 0;
		while (index < length && text[index] >= '0' && text[index] <= '9') {
			if (written_exponent < 100000)
				written_exponent = written_exponent * 10 + (text[index] - '0');
			index++;
		}

		/* The written exponent moves the point. */
		exponent += exponent_sign * written_exponent;
	}

	/* No significant digit: zero. */
	if (digits == 0)
		return 0.0;

	/* Far outside the doubles: infinity or zero. */
	if (exponent + digits > 400)
		return INFINITY;
	if (exponent + digits < -400)
		return 0.0;

	/* A digit dropped past the kept ones makes the value a little above them: one more digit, 1. */
	if (dropped) {
		big_multiply_small(&value, 10U, 1U);
		exponent--;
	}

	/* A whole number: the value times 10^exponent, rounded from its top bits. */
	if (exponent >= 0) {
		big_multiply_power10(&value, (uint32_t)exponent);
		top = big_top64(&value, &shift, &sticky);
		return number_from_parts(top, (int)big_bits(&value) - 1, sticky);
	}

	/* A fraction: the value over 10^-exponent, scaled so the quotient has 64 bits. */
	big_set(&scale, 1);
	big_multiply_power10(&scale, (uint32_t)-exponent);
	value_bits = big_bits(&value);
	scale_bits = big_bits(&scale);
	power = 64 - ((int)value_bits - (int)scale_bits);
	if (power > 0)
		big_shift_left(&value, (uint32_t)power);
	if (power < 0)
		big_shift_left(&scale, (uint32_t)-power);
	big_divide(&value, &scale, &quotient, &remainder);

	/* The quotient's top 64 bits and whether anything is left below them. */
	top = big_top64(&quotient, &shift, &sticky);
	zero = big_is_zero(&remainder);
	if (!zero)
		sticky = 1;
	binary = (int)big_bits(&quotient) - 1 - power;

	/* Succeeded: the nearest double. */
	return number_from_parts(top, binary, sticky);
}

/*
 * Reads the digits of an integer in a radix from 2 to 36 (no prefix, no
 * sign) to the nearest double; the caller has checked the digits.
 */
double
vm_number_parse_radix(
	const char *text,
	size_t length,
	int radix)
{
	struct number_big value;
	uint64_t top;
	uint32_t shift;
	uint32_t bits;
	size_t index;
	int sticky;
	int below;
	int digit;
	int zero;
	char character;

	/* The digits into a big integer, exactly (past the capacity a digit only counts as more). */
	big_set(&value, 0);
	sticky = 0;
	for (index = 0; index < length; index++) {
		character = text[index];
		digit = number_digit_value((char)(character | 0x20));
		if (character <= '9')
			digit = character - '0';
		bits = big_bits(&value);
		if (bits > (NUMBER_BIG_WORDS - 2U) * 32U) {
			if (digit != 0)
				sticky = 1;
			continue;
		}

		/* The digit joins the value. */
		big_multiply_small(&value, (uint32_t)radix, (uint32_t)digit);
	}

	/* Zero, or the nearest double from the top bits (and anything below them). */
	zero = big_is_zero(&value);
	if (zero)
		return 0.0;
	top = big_top64(&value, &shift, &below);
	if (below)
		sticky = 1;

	/* Succeeded: the nearest double. */
	return number_from_parts(top, (int)big_bits(&value) - 1, sticky);
}

/* Sets a big integer to a 64-bit value. */
static void
big_set(
	struct number_big *big,
	uint64_t value)
{
	/* The low word, then the high one, dropping a zero high word. */
	big->words[0] = (uint32_t)value;
	big->words[1] = (uint32_t)(value >> 32);
	big->count = 2;
	while (big->count > 0 && big->words[big->count - 1] == 0)
		big->count--;
}

/* Tells whether a big integer is zero. */
static int
big_is_zero(
	const struct number_big *big)
{
	/* A zero has no words. */
	if (big->count == 0)
		return 1;

	/* Anything else. */
	return 0;
}

/* Reports how many bits a big integer takes (0 for zero). */
static uint32_t
big_bits(
	const struct number_big *big)
{
	uint32_t top;
	uint32_t bits;

	/* Zero takes none. */
	if (big->count == 0)
		return 0;

	/* The words below the top one, and the top one's bits. */
	top = big->words[big->count - 1];
	bits = (big->count - 1U) * 32U;
	while (top != 0) {
		bits++;
		top >>= 1;
	}

	/* Reports the length. */
	return bits;
}

/* Compares two big integers: below zero, zero or above zero as the left is below, equal to or above the right. */
static int
big_compare(
	const struct number_big *left,
	const struct number_big *right)
{
	uint32_t index;

	/* More words is bigger. */
	if (left->count < right->count)
		return -1;
	if (left->count > right->count)
		return 1;

	/* Otherwise the first word that differs, from the top. */
	for (index = left->count; index > 0; index--) {
		if (left->words[index - 1] < right->words[index - 1])
			return -1;
		if (left->words[index - 1] > right->words[index - 1])
			return 1;
	}

	/* Equal. */
	return 0;
}

/* Adds a big integer to another. */
static void
big_add(
	struct number_big *big,
	const struct number_big *addend)
{
	uint64_t carry;
	uint64_t sum;
	uint32_t count;
	uint32_t index;

	/* Word by word with the carry, over the longer of the two. */
	count = big->count;
	if (addend->count > count)
		count = addend->count;
	carry = 0;
	for (index = 0; index < count; index++) {
		sum = carry;
		if (index < big->count)
			sum += big->words[index];
		if (index < addend->count)
			sum += addend->words[index];
		big->words[index] = (uint32_t)sum;
		carry = sum >> 32;
	}

	/* A carry out of the top is one more word. */
	big->count = count;
	if (carry != 0 && count < NUMBER_BIG_WORDS) {
		big->words[count] = (uint32_t)carry;
		big->count = count + 1U;
	}
}

/* Subtracts a big integer from another that is not smaller. */
static void
big_subtract(
	struct number_big *big,
	const struct number_big *subtrahend)
{
	uint64_t borrow;
	uint64_t difference;
	uint32_t index;

	/* Word by word with the borrow. */
	borrow = 0;
	for (index = 0; index < big->count; index++) {
		difference = (uint64_t)big->words[index] - borrow;
		if (index < subtrahend->count)
			difference -= subtrahend->words[index];
		big->words[index] = (uint32_t)difference;
		borrow = (difference >> 63) & 1U;
	}

	/* The top words that became zero. */
	while (big->count > 0 && big->words[big->count - 1] == 0)
		big->count--;
}

/* Shifts a big integer left by some bits (multiplies it by 2^bits). */
static void
big_shift_left(
	struct number_big *big,
	uint32_t bits)
{
	uint32_t words;
	uint32_t shift;
	uint32_t index;
	uint32_t high;

	/* Nothing to shift. */
	if (big->count == 0 || bits == 0)
		return;

	/* Whole words first, then the bits within a word, from the top down. */
	words = bits / 32U;
	shift = bits % 32U;
	if (big->count + words + 1U > NUMBER_BIG_WORDS)
		return;
	big->words[big->count + words] = 0;
	for (index = big->count; index > 0; index--) {
		high = 0;
		if (shift != 0)
			high = big->words[index - 1] >> (32U - shift);
		big->words[index + words] |= high;
		big->words[index - 1 + words] = big->words[index - 1] << shift;
	}

	/* The words below are zero. */
	for (index = 0; index < words; index++)
		big->words[index] = 0;

	/* The new length. */
	big->count += words + 1U;
	while (big->count > 0 && big->words[big->count - 1] == 0)
		big->count--;
}

/* Multiplies a big integer by a small factor and adds a small addend. */
static void
big_multiply_small(
	struct number_big *big,
	uint32_t factor,
	uint32_t addend)
{
	uint64_t carry;
	uint64_t product;
	uint32_t index;

	/* Word by word with the carry, the addend as the first carry. */
	carry = addend;
	for (index = 0; index < big->count; index++) {
		product = (uint64_t)big->words[index] * factor + carry;
		big->words[index] = (uint32_t)product;
		carry = product >> 32;
	}

	/* A carry out of the top is one more word. */
	if (carry != 0 && big->count < NUMBER_BIG_WORDS) {
		big->words[big->count] = (uint32_t)carry;
		big->count++;
	}
}

/* Multiplies a big integer by 10^exponent. */
static void
big_multiply_power10(
	struct number_big *big,
	uint32_t exponent)
{
	/* Nine digits at a time, then the rest. */
	while (exponent >= 9U) {
		big_multiply_small(big, 1000000000U, 0);
		exponent -= 9U;
	}
	while (exponent > 0) {
		big_multiply_small(big, 10U, 0);
		exponent--;
	}
}

/* Divides a big integer by a small divisor in place; reports the remainder. */
static uint32_t
big_divide_small(
	struct number_big *big,
	uint32_t divisor)
{
	uint64_t remainder;
	uint64_t current;
	uint32_t index;

	/* From the top word down, the remainder carried into the next. */
	remainder = 0;
	for (index = big->count; index > 0; index--) {
		current = (remainder << 32) | big->words[index - 1];
		big->words[index - 1] = (uint32_t)(current / divisor);
		remainder = current % divisor;
	}

	/* The top words that became zero. */
	while (big->count > 0 && big->words[big->count - 1] == 0)
		big->count--;

	/* Reports the remainder. */
	return (uint32_t)remainder;
}

/* Divides one big integer by another (long division bit by bit). */
static void
big_divide(
	const struct number_big *numerator,
	const struct number_big *denominator,
	struct number_big *quotient,
	struct number_big *remainder)
{
	uint32_t bits;
	uint32_t index;
	int bit;
	int comparison;

	/* Each bit of the numerator from the top: the remainder takes it, and the denominator is taken out when it fits. */
	big_set(quotient, 0);
	big_set(remainder, 0);
	bits = big_bits(numerator);
	for (index = bits; index > 0; index--) {
		big_shift_left(remainder, 1);
		bit = big_bit(numerator, index - 1U);
		if (bit) {
			if (remainder->count == 0)
				remainder->count = 1;
			remainder->words[0] |= 1U;
		}

		/* The quotient takes the bit that says whether the denominator was taken out. */
		big_shift_left(quotient, 1);
		comparison = big_compare(remainder, denominator);
		if (comparison >= 0) {
			big_subtract(remainder, denominator);
			if (quotient->count == 0) {
				quotient->words[0] = 0;
				quotient->count = 1;
			}

			/* The bit is one. */
			quotient->words[0] |= 1U;
		}
	}
}

/* Reports one bit of a big integer. */
static int
big_bit(
	const struct number_big *big,
	uint32_t index)
{
	/* Past the words is zero. */
	if (index / 32U >= big->count)
		return 0;

	/* The bit. */
	return (int)((big->words[index / 32U] >> (index % 32U)) & 1U);
}

/* Reports a big integer's top 64 bits (its highest bit at bit 63), how far they were shifted down, and whether any lower bit is set. */
static uint64_t
big_top64(
	const struct number_big *big,
	uint32_t *shift,
	int *sticky)
{
	uint64_t top;
	uint32_t bits;
	uint32_t index;
	int bit;

	/* The bits from the highest down, 64 of them (or all there are, shifted up). */
	bits = big_bits(big);
	top = 0;
	*sticky = 0;
	*shift = 0;
	for (index = 0; index < 64U; index++) {
		top <<= 1;
		bit = 0;
		if (index < bits)
			bit = big_bit(big, bits - 1U - index);
		if (bit)
			top |= 1U;
	}

	/* Whether a bit below them is set. */
	if (bits > 64U) {
		*shift = bits - 64U;
		for (index = 0; index < bits - 64U; index++) {
			bit = big_bit(big, index);
			if (bit) {
				*sticky = 1;
				break;
			}
		}
	}

	/* Reports the bits. */
	return top;
}

/* Writes a big integer's decimal digits (the point after them). */
static void
big_decimal(
	const struct number_big *big,
	struct number_digits *digits)
{
	struct number_big work;
	char reversed[800];
	uint32_t chunk;
	int count;
	int index;
	int piece;
	int zero;

	/* Nine digits at a time from the bottom, into a reversed list. */
	work = *big;
	count = 0;
	for (;;) {
		zero = big_is_zero(&work);
		if (zero || count >= (int)sizeof(reversed) - 9)
			break;
		chunk = big_divide_small(&work, 1000000000U);
		for (piece = 0; piece < 9; piece++) {
			reversed[count] = (char)('0' + chunk % 10U);
			chunk /= 10U;
			count++;
		}
	}

	/* The zeros at the top of the last chunk go; zero itself is one digit. */
	while (count > 1 && reversed[count - 1] == '0')
		count--;
	if (count == 0) {
		reversed[0] = '0';
		count = 1;
	}

	/* The digits in order. */
	for (index = 0; index < count; index++)
		digits->digits[index] = reversed[count - 1 - index];
	digits->count = count;
	digits->point = count;
}

/* Splits a positive finite double into its integer fraction and binary exponent (the value is fraction * 2^exponent). */
static void
number_split(
	double number,
	uint64_t *fraction,
	int *exponent)
{
	uint64_t bits;
	int biased;

	/* The bits: a biased exponent and 52 fraction bits (the leading 1 is implied unless subnormal). */
	memcpy(&bits, &number, sizeof(bits));
	biased = (int)((bits >> NUMBER_FRACTION_BITS) & 0x7FFU);
	*fraction = bits & ((1ULL << NUMBER_FRACTION_BITS) - 1U);
	if (biased == 0) {
		*exponent = NUMBER_EXPONENT_MIN;
		return;
	}

	/* A normal number. */
	*fraction |= 1ULL << NUMBER_FRACTION_BITS;
	*exponent = biased - NUMBER_EXPONENT_BIAS - (int)NUMBER_FRACTION_BITS;
}

/*
 * Finds the shortest digits that read back as a positive finite double
 * (free-format, Steele and White / Burger and Dybvig), a tie going to the
 * even digit.
 */
static void
number_shortest(
	double number,
	struct number_digits *digits)
{
	struct number_big r;
	struct number_big s;
	struct number_big m_plus;
	struct number_big m_minus;
	struct number_big sum;
	struct number_big twice;
	uint64_t fraction;
	int exponent;
	int power;
	int even;
	int low;
	int high;
	int digit;
	int comparison;

	/* r / s is the value, m+ and m- the distances to the midpoints with its neighbours (all doubled). */
	number_split(number, &fraction, &exponent);
	even = (fraction & 1U) == 0;
	if (exponent >= 0) {
		if (fraction != (1ULL << NUMBER_FRACTION_BITS)) {
			big_set(&r, fraction);
			big_shift_left(&r, (uint32_t)exponent + 1U);
			big_set(&s, 2);
			big_set(&m_plus, 1);
			big_shift_left(&m_plus, (uint32_t)exponent);
			m_minus = m_plus;
		} else {
			big_set(&r, fraction);
			big_shift_left(&r, (uint32_t)exponent + 2U);
			big_set(&s, 4);
			big_set(&m_plus, 1);
			big_shift_left(&m_plus, (uint32_t)exponent + 1U);
			big_set(&m_minus, 1);
			big_shift_left(&m_minus, (uint32_t)exponent);
		}
	} else if (exponent == NUMBER_EXPONENT_MIN || fraction != (1ULL << NUMBER_FRACTION_BITS)) {
		big_set(&r, fraction);
		big_shift_left(&r, 1);
		big_set(&s, 1);
		big_shift_left(&s, (uint32_t)(1 - exponent));
		big_set(&m_plus, 1);
		big_set(&m_minus, 1);
	} else {
		big_set(&r, fraction);
		big_shift_left(&r, 2);
		big_set(&s, 1);
		big_shift_left(&s, (uint32_t)(2 - exponent));
		big_set(&m_plus, 2);
		big_set(&m_minus, 1);
	}

	/* The decimal exponent: scale so the value is below 1 and at least 0.1 (the estimate is at most one low). */
	power = number_estimate_power(fraction, exponent);
	if (power >= 0) {
		big_multiply_power10(&s, (uint32_t)power);
	} else {
		big_multiply_power10(&r, (uint32_t)-power);
		big_multiply_power10(&m_plus, (uint32_t)-power);
		big_multiply_power10(&m_minus, (uint32_t)-power);
	}

	/* One too low an estimate shows as the value reaching 1; it is corrected. */
	sum = r;
	big_add(&sum, &m_plus);
	comparison = big_compare(&sum, &s);
	if (comparison > 0 || (even && comparison == 0)) {
		big_multiply_small(&s, 10U, 0);
		power++;
	}

	/* The digits, until one of them is close enough to tell this double from its neighbours. */
	digits->count = 0;
	digits->point = power;
	for (;;) {
		big_multiply_small(&r, 10U, 0);
		big_multiply_small(&m_plus, 10U, 0);
		big_multiply_small(&m_minus, 10U, 0);
		digit = 0;
		for (;;) {
			comparison = big_compare(&r, &s);
			if (comparison < 0)
				break;
			big_subtract(&r, &s);
			digit++;
		}

		/* Low: the rest is within the lower margin; high: the digit plus one is within the upper margin. */
		comparison = big_compare(&r, &m_minus);
		low = comparison < 0 || (even && comparison == 0);
		sum = r;
		big_add(&sum, &m_plus);
		comparison = big_compare(&sum, &s);
		high = comparison > 0 || (even && comparison == 0);
		if (!low && !high) {
			digits->digits[digits->count] = (char)('0' + digit);
			digits->count++;
			continue;
		}

		/* The last digit: the nearer of digit and digit + 1 (a tie takes the even one). */
		if (low && high) {
			twice = r;
			big_shift_left(&twice, 1);
			comparison = big_compare(&twice, &s);
			if (comparison > 0 || (comparison == 0 && (digit & 1) != 0))
				digit++;
		} else if (high) {
			digit++;
		}

		/* A digit that does not end them. */
		digits->digits[digits->count] = (char)('0' + digit);
		digits->count++;
		break;
	}
}

/* Estimates the decimal exponent k with 10^(k-1) <= value < 10^k (one too low at most). */
static int
number_estimate_power(
	uint64_t fraction,
	int exponent)
{
	double log_value;
	uint32_t bits;
	uint64_t rest;

	/* The value's binary length, times log10(2), a little under. */
	bits = 0;
	rest = fraction;
	while (rest != 0) {
		bits++;
		rest >>= 1;
	}

	/* The value's binary length, times log10(2), a little under. */
	log_value = ((double)exponent + (double)bits - 1.0) * 0.30102999566398114;

	/* Rounded up, then one less to be safe from above. */
	return (int)ceil(log_value - 1e-10);
}

/* Rounds a positive double times 10^power to the nearest integer (a tie up), exactly. */
static void
number_round_scaled(
	double number,
	int power,
	struct number_big *result)
{
	struct number_big numerator;
	struct number_big denominator;
	struct number_big remainder;
	uint64_t fraction;
	int exponent;
	int comparison;

	/* numerator / denominator is the value times 10^power exactly. */
	if (number == 0.0) {
		big_set(result, 0);
		return;
	}

	/* The value's parts. */
	number_split(number, &fraction, &exponent);
	big_set(&numerator, fraction);
	big_set(&denominator, 1);
	if (exponent >= 0) {
		big_shift_left(&numerator, (uint32_t)exponent);
	} else {
		big_shift_left(&denominator, (uint32_t)-exponent);
	}

	/* The power of ten, on the side it belongs. */
	if (power >= 0) {
		big_multiply_power10(&numerator, (uint32_t)power);
	} else {
		big_multiply_power10(&denominator, (uint32_t)-power);
	}

	/* The quotient, one more when the remainder is at least half the denominator. */
	big_divide(&numerator, &denominator, result, &remainder);
	big_shift_left(&remainder, 1);
	comparison = big_compare(&remainder, &denominator);
	if (comparison >= 0)
		big_multiply_small(result, 1U, 1U);
}

/* Makes the double nearest to top * 2^(top_exponent - 63) (top's highest bit at 63), the sticky bit saying there is more below. */
static double
number_from_parts(
	uint64_t top,
	int top_exponent,
	int sticky)
{
	uint64_t kept;
	uint64_t rest;
	uint64_t half;
	uint64_t bits;
	double number;
	int shift;
	int round_up;

	/* Too big: infinity. */
	if (top_exponent > 1023)
		return INFINITY;

	/* The bits kept: 53 for a normal number, fewer for a subnormal one. */
	shift = 11;
	if (top_exponent < -1022)
		shift = 11 + (-1022 - top_exponent);
	if (shift >= 64) {
		kept = 0;
		rest = top;
		half = 0;
		if (shift == 64)
			half = 1ULL << 63;
	} else {
		kept = top >> shift;
		rest = top & ((1ULL << shift) - 1U);
		half = 1ULL << (shift - 1);
	}

	/* Rounding to nearest, a tie to even (the sticky bit breaks a seeming tie upwards). */
	round_up = 0;
	if (half != 0 && rest > half)
		round_up = 1;
	if (half != 0 && rest == half && (sticky || (kept & 1U) != 0))
		round_up = 1;
	if (round_up)
		kept++;

	/* A normal number: the carry past 53 bits moves the exponent. */
	if (top_exponent >= -1022) {
		if (kept == (1ULL << 53)) {
			kept >>= 1;
			top_exponent++;
			if (top_exponent > 1023)
				return INFINITY;
		}

		/* The biased exponent and the fraction's bits. */
		bits = ((uint64_t)(top_exponent + NUMBER_EXPONENT_BIAS) << NUMBER_FRACTION_BITS) | (kept & ((1ULL << NUMBER_FRACTION_BITS) - 1U));
	} else {
		/* A subnormal: the kept bits are the fraction (a carry into bit 52 makes the smallest normal number). */
		bits = kept;
	}

	/* The double. */
	memcpy(&number, &bits, sizeof(number));
	return number;
}

/* Finds exactly count significant digits of a positive finite double, rounded (a tie up), and where the point goes. */
static void
number_digits_exact(
	double number,
	int count,
	struct number_digits *digits)
{
	struct number_big scaled;
	uint64_t fraction;
	int exponent;
	int power;

	/* The decimal exponent k with 10^(k-1) <= value < 10^k, from the estimate. */
	number_split(number, &fraction, &exponent);
	power = number_estimate_power(fraction, exponent);
	for (;;) {
		/* value * 10^(count - k) rounded must have count digits. */
		number_round_scaled(number, count - power, &scaled);
		big_decimal(&scaled, digits);
		if (digits->count > count) {
			power++;
			continue;
		}

		/* Too few: the power was too high. */
		if (digits->count < count) {
			power--;
			continue;
		}

		/* The right count. */
		break;
	}

	/* The point: 0.DIGITS times 10^power. */
	digits->point = power;
}

/* Appends a count of zeros. */
static int
number_append_zeros(
	struct wb_buffer *out,
	int count)
{
	int error;

	/* One at a time (the counts are small). */
	error = 0;
	while (error == 0 && count > 0) {
		error = wb_buffer_append_byte(out, '0');
		count--;
	}

	/* Reports a buffer that could not grow. */
	if (error != 0)
		return error;

	/* Succeeded: the zeros are written. */
	return 0;
}

/* Writes digits in exponential form: d.ddd e+x (a sign first when negative). */
static int
number_write_exponential(
	const struct number_digits *digits,
	int negative,
	struct wb_buffer *out)
{
	int exponent;
	int error;
	char sign;

	/* The sign, the first digit, and the others after a point. */
	error = 0;
	if (negative)
		error = wb_buffer_append_byte(out, '-');
	if (error == 0)
		error = wb_buffer_append_byte(out, (unsigned char)digits->digits[0]);
	if (error == 0 && digits->count > 1) {
		error = wb_buffer_append_byte(out, '.');
		if (error == 0)
			error = wb_buffer_append(out, digits->digits + 1, (size_t)(digits->count - 1));
	}

	/* The exponent with its sign. */
	exponent = digits->point - 1;
	sign = '+';
	if (exponent < 0) {
		sign = '-';
		exponent = -exponent;
	}

	/* The exponent itself. */
	if (error == 0)
		error = wb_buffer_printf(out, "e%c%d", sign, exponent);
	if (error != 0)
		return error;

	/* Succeeded: the text is written. */
	return 0;
}

/* Reports the character of a digit in a radix up to 36. */
static char
number_digit_char(
	int digit)
{
	/* 0 to 9, then a to z. */
	if (digit < 10)
		return (char)('0' + digit);

	/* A letter. */
	return (char)('a' + digit - 10);
}

/* Reports the value of a digit character in a radix up to 36. */
static int
number_digit_value(
	char character)
{
	/* 0 to 9. */
	if (character <= '9')
		return character - '0';

	/* a to z. */
	return character - 'a' + 10;
}

/*
 * Writes a positive finite number in a radix other than 10 (V8's
 * DoubleToRadixCString): the fraction's digits while they still tell the
 * double from its neighbours, rounded to even with a carry that may reach
 * the integer part, then the integer part's digits.
 */
static int
number_to_radix(
	double number,
	int radix,
	struct wb_buffer *out)
{
	char integer_text[1100];
	char fraction_text[1100];
	double integer;
	double fraction;
	double delta;
	double smallest;
	double remainder;
	int integer_count;
	int fraction_count;
	int digit;
	int index;
	int error;

	/* The parts, and half the distance to the next double (at least the smallest one). */
	integer = floor(number);
	fraction = number - integer;
	delta = 0.5 * (nextafter(number, INFINITY) - number);
	smallest = nextafter(0.0, 1.0);
	if (smallest > delta)
		delta = smallest;

	/* The fraction's digits. */
	fraction_count = 0;
	if (fraction >= delta) {
		do {
			fraction *= (double)radix;
			delta *= (double)radix;
			digit = (int)fraction;
			fraction_text[fraction_count] = number_digit_char(digit);
			fraction_count++;
			fraction -= (double)digit;

			/* Past the half (or at it on an odd digit), and near enough the next value: round up with the carry. */
			if (fraction > 0.5 || (fraction == 0.5 && (digit & 1) != 0)) {
				if (fraction + delta > 1.0) {
					for (;;) {
						fraction_count--;
						if (fraction_count < 0) {
							fraction_count = 0;
							integer += 1.0;
							break;
						}

						/* The digit before, one up unless it carries further. */
						digit = number_digit_value(fraction_text[fraction_count]);
						if (digit + 1 < radix) {
							fraction_text[fraction_count] = number_digit_char(digit + 1);
							fraction_count++;
							break;
						}
					}

					/* The rounding ends the digits. */
					break;
				}
			}
		} while (fraction >= delta && fraction_count < (int)sizeof(fraction_text) - 1);
	}

	/* The integer's digits from the end; where a double cannot hold them all, the lowest are zeros. */
	integer_count = 0;
	while (integer / (double)radix >= 9007199254740992.0 && integer_count < (int)sizeof(integer_text) - 1) {
		integer /= (double)radix;
		integer_text[integer_count] = '0';
		integer_count++;
	}

	/* The rest, digit by digit. */
	do {
		remainder = number_whole_modulo(integer, (uint32_t)radix);
		integer_text[integer_count] = number_digit_char((int)remainder);
		integer_count++;
		integer = (integer - remainder) / (double)radix;
	} while (integer > 0.0 && integer_count < (int)sizeof(integer_text));

	/* The integer's digits in order, then the fraction's. */
	for (index = integer_count - 1; index >= 0; index--) {
		error = wb_buffer_append_byte(out, (unsigned char)integer_text[index]);
		if (error != 0)
			return error;
	}

	/* The point and the fraction's digits. */
	if (fraction_count > 0) {
		error = wb_buffer_append_byte(out, '.');
		if (error == 0)
			error = wb_buffer_append(out, fraction_text, (size_t)fraction_count);
		if (error != 0)
			return error;
	}

	/* Succeeded: the text is written. */
	return 0;
}

/* Reports a non-negative double modulo a small divisor, exactly as IEEE fmod (the C library's need not be). */
static double
number_whole_modulo(
	double number,
	uint32_t divisor)
{
	struct number_big value;
	struct number_big modulus;
	struct number_big quotient;
	struct number_big remainder;
	uint64_t fraction;
	uint64_t top;
	uint32_t shift;
	double result;
	int exponent;
	int sticky;
	int zero;

	/* Zero. */
	if (number == 0.0)
		return 0.0;

	/* number = fraction * 2^exponent; the modulus is the divisor on the same scale. */
	number_split(number, &fraction, &exponent);
	big_set(&value, fraction);
	big_set(&modulus, divisor);
	if (exponent >= 0) {
		big_shift_left(&value, (uint32_t)exponent);
	} else {
		big_shift_left(&modulus, (uint32_t)-exponent);
	}

	/* The remainder on that scale. */
	big_divide(&value, &modulus, &quotient, &remainder);
	zero = big_is_zero(&remainder);
	if (zero)
		return 0.0;

	/* Back to a double (exact: the remainder has no more bits than the fraction had). */
	top = big_top64(&remainder, &shift, &sticky);
	if (exponent >= 0)
		exponent = 0;
	result = number_from_parts(top, (int)big_bits(&remainder) - 1 + exponent, sticky);

	/* The remainder. */
	return result;
}
