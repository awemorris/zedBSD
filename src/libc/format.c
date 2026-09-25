/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#include <limits.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

struct format_output {
	char *buffer;
	size_t size;
	size_t length;
};

#ifndef KERN_NO_PRINTF_FLOAT
/* The 32-bit words of the big integers that hold a double exactly. */
#define FLOAT_WORDS 36

/*
 * The most decimal digits a double's exact value has: 309 before the point
 * and 1074 after it.
 */
#define FLOAT_DECIMAL_MAX 1400

/* The bits of a double, to read its sign, exponent and fraction. */
union double_shape {
	double value;
	uint64_t bits;
};

/*
 * The exact decimal value of a finite double's magnitude.
 *
 * digits holds values 0 to 9, with the point after the first integer_count
 * of them.  A value below 1 has no integer digits, and its digits start
 * right after the point, leading zeros included.  Zero has no digits.
 */
struct float_decimal {
	unsigned char digits[FLOAT_DECIMAL_MAX + 1];
	int count;
	int integer_count;
};

/*
 * Where the characters of a conversion go.  They are always counted, and
 * written only when there is an output, so that a conversion is measured
 * once before it is padded and written.
 */
struct float_sink {
	struct format_output *output;
	size_t length;
};

static void sink_character(struct float_sink *sink, char character);
static char decimal_character(const struct float_decimal *decimal, int index);
static void sink_exponent(struct float_sink *sink, int exponent, int upper);
static void big_from_u64(uint32_t *words, uint64_t value, unsigned int shift);
static uint32_t big_divide_small(uint32_t *words, uint32_t divisor);
static int big_is_zero(const uint32_t *words);
static void exact_decimal(double magnitude, struct float_decimal *decimal);
static void integer_digits(uint32_t *words, struct float_decimal *decimal);
static void fraction_digits(uint32_t *words, unsigned int scale, struct float_decimal *decimal);
static void round_decimal(struct float_decimal *decimal, int keep);
static int first_nonzero(const struct float_decimal *decimal);
static void format_fixed(struct float_sink *sink, const struct float_decimal *exact, int precision, int alternate, int strip);
static void format_exponential(struct float_sink *sink, const struct float_decimal *exact, int precision, int upper, int alternate, int strip);
static void format_general(struct float_sink *sink, const struct float_decimal *exact, int precision, int upper, int alternate);
static void format_magnitude(struct float_sink *sink, const struct float_decimal *exact, const char *special, char conversion, int precision, int alternate);
static void emit_float(struct format_output *output, double value, char conversion, int alternate, int left, int plus, int space, int zero, int width, int precision);
#endif

static void
emit_character(struct format_output *output, char character)
{
	if (output->size != 0 && output->length + 1U < output->size)
		output->buffer[output->length] = character;
	output->length++;
}

static void
emit_repeat(struct format_output *output, char character, size_t count)
{
	while (count-- != 0)
		emit_character(output, character);
}

static void
emit_bytes(struct format_output *output, const char *text, size_t length)
{
	while (length-- != 0)
		emit_character(output, *text++);
}

static size_t
wide_text_length(const wchar_t *text, int precision)
{
	mbstate_t state = { 0 };
	char bytes[4];
	size_t length = 0;

	while (*text != 0) {
		size_t count = wcrtomb(bytes, *text++, &state);
		if (count == (size_t)-1)
			return (size_t)-1;
		if (precision >= 0 && count > (size_t)precision - length)
			break;
		length += count;
	}
	return length;
}

static int
emit_wide_text(struct format_output *output, const wchar_t *text,
	size_t length)
{
	mbstate_t state = { 0 };
	char bytes[4];
	size_t emitted = 0;

	while (*text != 0 && emitted < length) {
		size_t count = wcrtomb(bytes, *text++, &state);
		if (count == (size_t)-1 || count > length - emitted)
			return -1;
		emit_bytes(output, bytes, count);
		emitted += count;
	}
	return 0;
}

#ifndef KERN_NO_PRINTF_FLOAT
/* Counts a character, and writes it when there is an output. */
static void
sink_character(
	struct float_sink *sink,
	char character)
{
	/* Written on the second pass only. */
	if (sink->output != NULL)
		emit_character(sink->output, character);
	sink->length++;
}

/* Returns a digit of a decimal as a character; past its digits it is 0. */
static char
decimal_character(
	const struct float_decimal *decimal,
	int index)
{
	/* No digit there. */
	if (index < 0 || index >= decimal->count)
		return '0';

	/* Succeeded. */
	return (char)('0' + decimal->digits[index]);
}

/* Writes the exponent of %e: the letter, the sign, and two digits at least. */
static void
sink_exponent(
	struct float_sink *sink,
	int exponent,
	int upper)
{
	char reverse[12];
	unsigned int magnitude;
	int count;

	/* The letter. */
	if (upper)
		sink_character(sink, 'E');
	else
		sink_character(sink, 'e');

	/* The sign. */
	if (exponent < 0) {
		sink_character(sink, '-');
		magnitude = (unsigned int)(-exponent);
	} else {
		sink_character(sink, '+');
		magnitude = (unsigned int)exponent;
	}

	/* The digits, from the last, then written from the first. */
	count = 0;
	do {
		reverse[count] = (char)('0' + magnitude % 10U);
		count++;
		magnitude /= 10U;
	} while (magnitude != 0);
	while (count < 2) {
		reverse[count] = '0';
		count++;
	}
	while (count > 0) {
		count--;
		sink_character(sink, reverse[count]);
	}
}

/* Sets a big integer to a 64-bit value shifted left by some bits. */
static void
big_from_u64(
	uint32_t *words,
	uint64_t value,
	unsigned int shift)
{
	unsigned int bit;
	unsigned int position;

	/* Zero, then each set bit of the value at its place. */
	memset(words, 0, sizeof(*words) * FLOAT_WORDS);
	for (bit = 0; bit < 64U; bit++) {
		if (((value >> bit) & 1U) == 0)
			continue;
		position = shift + bit;
		words[position / 32U] |= (uint32_t)1U << (position % 32U);
	}
}

/* Divides a big integer by a small number; returns the remainder. */
static uint32_t
big_divide_small(
	uint32_t *words,
	uint32_t divisor)
{
	uint64_t part;
	uint32_t remainder;
	int index;

	/* From the highest word down, carrying the remainder. */
	remainder = 0;
	for (index = FLOAT_WORDS - 1; index >= 0; index--) {
		part = ((uint64_t)remainder << 32) | words[index];
		words[index] = (uint32_t)(part / divisor);
		remainder = (uint32_t)(part % divisor);
	}

	/* Succeeded. */
	return remainder;
}

/* Returns whether a big integer is zero. */
static int
big_is_zero(
	const uint32_t *words)
{
	int index;

	/* Any word that is not zero. */
	for (index = 0; index < FLOAT_WORDS; index++) {
		if (words[index] != 0)
			return 0;
	}

	/* Succeeded: all zero. */
	return 1;
}

/*
 * Makes the exact decimal value of a finite, non-negative double.  The
 * double is a 53-bit integer times a power of two: a positive power makes
 * an integer, and a negative one an integer part and a binary fraction,
 * whose decimal digits end after as many digits as the power.
 */
static void
exact_decimal(
	double magnitude,
	struct float_decimal *decimal)
{
	uint32_t words[FLOAT_WORDS];
	union double_shape shape;
	uint64_t mantissa;
	uint64_t integer;
	uint64_t rest;
	unsigned int field;
	unsigned int scale;
	int exponent;

	/* The integer and the power of two it is multiplied by. */
	shape.value = magnitude;
	field = (unsigned int)((shape.bits >> 52) & 0x7ffU);
	mantissa = shape.bits & 0x000fffffffffffffULL;
	exponent = -1074;
	if (field != 0) {
		mantissa |= (uint64_t)1U << 52;
		exponent = (int)field - 1075;
	}

	/* Zero has no digits. */
	decimal->count = 0;
	decimal->integer_count = 0;
	if (mantissa == 0)
		return;

	/* A whole number. */
	if (exponent >= 0) {
		big_from_u64(words, mantissa, (unsigned int)exponent);
		integer_digits(words, decimal);
		return;
	}

	/* The integer part, which fits in 64 bits. */
	scale = (unsigned int)(-exponent);
	integer = 0;
	rest = mantissa;
	if (scale < 64U) {
		integer = mantissa >> scale;
		rest = mantissa & (((uint64_t)1U << scale) - 1U);
	}

	/* Its digits, when it is not zero. */
	if (integer != 0) {
		big_from_u64(words, integer, 0);
		integer_digits(words, decimal);
	}

	/* The fraction: rest over two to the power of scale. */
	big_from_u64(words, rest, 0);
	fraction_digits(words, scale, decimal);
}

/* Appends the decimal digits of a big integer as the integer digits. */
static void
integer_digits(
	uint32_t *words,
	struct float_decimal *decimal)
{
	uint32_t chunks[FLOAT_WORDS * 2];
	uint32_t chunk;
	int chunk_count;
	int digit;
	int zero;
	int started;

	/* Groups of nine digits, the lowest first. */
	chunk_count = 0;
	for (;;) {
		zero = big_is_zero(words);
		if (zero)
			break;
		chunks[chunk_count] = big_divide_small(words, 1000000000U);
		chunk_count++;
	}

	/* The groups from the highest, without the leading zeros of the first. */
	started = 0;
	while (chunk_count > 0) {
		chunk_count--;
		chunk = chunks[chunk_count];
		for (digit = 8; digit >= 0; digit--) {
			decimal->digits[decimal->count] = (unsigned char)(chunk / 100000000U);
			chunk = (chunk % 100000000U) * 10U;
			if (!started && decimal->digits[decimal->count] == 0)
				continue;
			started = 1;
			decimal->count++;
		}
	}

	/* Succeeded: every digit so far is before the point. */
	decimal->integer_count = decimal->count;
}

/*
 * Appends the decimal digits of a binary fraction, a big integer over two
 * to the power of scale: ten times it has the next digit above that power.
 */
static void
fraction_digits(
	uint32_t *words,
	unsigned int scale,
	struct float_decimal *decimal)
{
	uint64_t product;
	uint32_t carry;
	unsigned int bit;
	unsigned int position;
	unsigned int digit;
	int index;
	int zero;

	/* Each digit, until the fraction is used up. */
	for (;;) {
		zero = big_is_zero(words);
		if (zero || decimal->count >= FLOAT_DECIMAL_MAX)
			break;

		/* Ten times the fraction. */
		carry = 0;
		for (index = 0; index < FLOAT_WORDS; index++) {
			product = (uint64_t)words[index] * 10U + carry;
			words[index] = (uint32_t)product;
			carry = (uint32_t)(product >> 32);
		}

		/* The part above the scale is the digit, which leaves it. */
		digit = 0;
		for (bit = 0; bit < 4U; bit++) {
			position = scale + bit;
			if (((words[position / 32U] >> (position % 32U)) & 1U) == 0)
				continue;
			digit |= 1U << bit;
			words[position / 32U] &= ~((uint32_t)1U << (position % 32U));
		}

		/* The digit, added. */
		decimal->digits[decimal->count] = (unsigned char)digit;
		decimal->count++;
	}
}

/*
 * Keeps the first digits of a decimal and rounds by the rest, to the
 * nearest and to even on a tie.  Rounding up past the first digit adds a
 * new first digit before the point.
 */
static void
round_decimal(
	struct float_decimal *decimal,
	int keep)
{
	int next;
	int sticky;
	int up;
	int index;

	/* Nothing to drop. */
	if (keep >= decimal->count)
		return;

	/* The first dropped digit, and whether any after it is not zero. */
	next = decimal->digits[keep];
	sticky = 0;
	for (index = keep + 1; index < decimal->count; index++) {
		if (decimal->digits[index] != 0) {
			sticky = 1;
			break;
		}
	}

	/* Up above a half, and on a half when the kept digit is odd. */
	up = 0;
	if (next > 5) {
		up = 1;
	} else if (next == 5 && sticky) {
		up = 1;
	} else if (next == 5 && keep > 0) {
		if ((decimal->digits[keep - 1] & 1U) != 0)
			up = 1;
	}

	/* The kept digits; rounding down is done. */
	decimal->count = keep;
	if (!up)
		return;

	/* The carry through the nines. */
	index = keep - 1;
	while (index >= 0 && decimal->digits[index] == 9) {
		decimal->digits[index] = 0;
		index--;
	}

	/* A digit below 9 takes the carry. */
	if (index >= 0) {
		decimal->digits[index]++;
		return;
	}

	/* Succeeded: a carry out of every digit is a new one before the point. */
	memmove(decimal->digits + 1, decimal->digits, (size_t)keep);
	decimal->digits[0] = 1;
	decimal->count = keep + 1;
	decimal->integer_count++;
}

/* Returns the index of the first digit that is not zero, or -1. */
static int
first_nonzero(
	const struct float_decimal *decimal)
{
	int index;

	/* The digits in order. */
	for (index = 0; index < decimal->count; index++) {
		if (decimal->digits[index] != 0)
			return index;
	}

	/* None: the value is zero. */
	return -1;
}

/*
 * Writes %f: every integer digit, and as many fraction digits as the
 * precision.  strip drops the trailing zeros of the fraction (for %g).
 */
static void
format_fixed(
	struct float_sink *sink,
	const struct float_decimal *exact,
	int precision,
	int alternate,
	int strip)
{
	struct float_decimal rounded;
	char digit;
	int shown;
	int index;

	/* The value rounded after the last fraction digit shown. */
	memcpy(&rounded, exact, sizeof(rounded));
	round_decimal(&rounded, rounded.integer_count + precision);

	/* The integer part; 0 when the value is below 1. */
	if (rounded.integer_count == 0)
		sink_character(sink, '0');
	for (index = 0; index < rounded.integer_count; index++) {
		digit = decimal_character(&rounded, index);
		sink_character(sink, digit);
	}

	/* How many fraction digits: trailing zeros go for %g. */
	shown = precision;
	while (strip && shown > 0) {
		digit = decimal_character(&rounded, rounded.integer_count + shown - 1);
		if (digit != '0')
			break;
		shown--;
	}

	/* The point, then the fraction digits. */
	if (shown > 0 || alternate)
		sink_character(sink, '.');
	for (index = 0; index < shown; index++) {
		digit = decimal_character(&rounded, rounded.integer_count + index);
		sink_character(sink, digit);
	}
}

/*
 * Writes %e: the first digit that is not zero, a point, as many digits as
 * the precision, and the exponent.  strip drops the trailing zeros (%g).
 */
static void
format_exponential(
	struct float_sink *sink,
	const struct float_decimal *exact,
	int precision,
	int upper,
	int alternate,
	int strip)
{
	struct float_decimal rounded;
	char digit;
	int first;
	int exponent;
	int shown;
	int index;

	/* The value rounded after the last digit shown. */
	memcpy(&rounded, exact, sizeof(rounded));
	first = first_nonzero(&rounded);
	exponent = 0;
	if (first >= 0) {
		round_decimal(&rounded, first + precision + 1);
		first = first_nonzero(&rounded);
		exponent = rounded.integer_count - 1 - first;
	}

	/* The first digit; zero has only zeros. */
	digit = decimal_character(&rounded, first);
	if (first < 0)
		digit = '0';
	sink_character(sink, digit);

	/* How many more digits: trailing zeros go for %g. */
	shown = precision;
	while (strip && shown > 0) {
		digit = decimal_character(&rounded, first + shown);
		if (first >= 0 && digit != '0')
			break;
		shown--;
	}

	/* The point, the digits, and the exponent. */
	if (shown > 0 || alternate)
		sink_character(sink, '.');
	for (index = 1; index <= shown; index++) {
		digit = '0';
		if (first >= 0)
			digit = decimal_character(&rounded, first + index);
		sink_character(sink, digit);
	}

	/* The exponent. */
	sink_exponent(sink, exponent, upper);
}

/*
 * Writes %g: with precision P (0 is 1) and the exponent X that %e would
 * write, %f with precision P - 1 - X when P > X >= -4, and %e with
 * precision P - 1 otherwise; trailing zeros go unless # is given.
 */
static void
format_general(
	struct float_sink *sink,
	const struct float_decimal *exact,
	int precision,
	int upper,
	int alternate)
{
	struct float_decimal probe;
	int first;
	int exponent;
	int strip;

	/* The precision. */
	if (precision < 0)
		precision = 6;
	if (precision == 0)
		precision = 1;

	/* The exponent %e would write with this precision. */
	memcpy(&probe, exact, sizeof(probe));
	exponent = 0;
	first = first_nonzero(&probe);
	if (first >= 0) {
		round_decimal(&probe, first + precision);
		first = first_nonzero(&probe);
		exponent = probe.integer_count - 1 - first;
	}

	/* Succeeded: the style the exponent chooses. */
	strip = 1;
	if (alternate)
		strip = 0;
	if (precision > exponent && exponent >= -4)
		format_fixed(sink, exact, precision - 1 - exponent, alternate, strip);
	else
		format_exponential(sink, exact, precision - 1, upper, alternate, strip);
}

/*
 * Writes a conversion of a double's magnitude, or the text of infinity or
 * not-a-number when special is given.
 */
static void
format_magnitude(
	struct float_sink *sink,
	const struct float_decimal *exact,
	const char *special,
	char conversion,
	int precision,
	int alternate)
{
	int upper;

	/* Infinity and not-a-number are words. */
	if (special != NULL) {
		while (*special != '\0') {
			sink_character(sink, *special);
			special++;
		}

		/* Nothing else to write. */
		return;
	}

	/* The upper-case conversions. */
	upper = 0;
	if (conversion == 'F' || conversion == 'E' || conversion == 'G')
		upper = 1;

	/* The conversion; %f and %e take 6 digits by default. */
	if (conversion == 'g' || conversion == 'G') {
		format_general(sink, exact, precision, upper, alternate);
		return;
	}

	/* %f and %e: the default precision, then the style. */
	if (precision < 0)
		precision = 6;
	if (conversion == 'e' || conversion == 'E')
		format_exponential(sink, exact, precision, upper, alternate, 0);
	else
		format_fixed(sink, exact, precision, alternate, 0);
}

/*
 * Writes a double by %f, %e or %g with its flags, width and precision.
 * The conversion is measured first, so that it can be padded to the width.
 */
static void
emit_float(
	struct format_output *output,
	double value,
	char conversion,
	int alternate,
	int left,
	int plus,
	int space,
	int zero,
	int width,
	int precision)
{
	struct float_decimal exact;
	struct float_sink sink;
	union double_shape shape;
	const char *special;
	unsigned int field;
	uint64_t fraction;
	size_t total;
	char prefix;
	int upper;

	/* The sign, and what goes before the digits. */
	shape.value = value;
	prefix = '\0';
	if ((shape.bits >> 63) != 0)
		prefix = '-';
	else if (plus)
		prefix = '+';
	else if (space)
		prefix = ' ';

	/* Infinity and not-a-number, which are never padded with zeros. */
	field = (unsigned int)((shape.bits >> 52) & 0x7ffU);
	fraction = shape.bits & 0x000fffffffffffffULL;
	upper = 0;
	if (conversion == 'F' || conversion == 'E' || conversion == 'G')
		upper = 1;
	special = NULL;
	if (field == 0x7ffU && fraction != 0) {
		special = "nan";
		if (upper)
			special = "NAN";
		zero = 0;
	} else if (field == 0x7ffU) {
		special = "inf";
		if (upper)
			special = "INF";
		zero = 0;
	}

	/* The exact digits of a finite magnitude. */
	exact.count = 0;
	exact.integer_count = 0;
	if (special == NULL) {
		shape.bits &= 0x7fffffffffffffffULL;
		exact_decimal(shape.value, &exact);
	}

	/* The length, measured without writing. */
	sink.output = NULL;
	sink.length = 0;
	format_magnitude(&sink, &exact, special, conversion, precision, alternate);
	total = sink.length;
	if (prefix != '\0')
		total++;

	/* The padding before, the sign, zeros, the conversion, the padding after. */
	if (!left && !zero && width > 0 && (size_t)width > total)
		emit_repeat(output, ' ', (size_t)width - total);
	if (prefix != '\0')
		emit_character(output, prefix);
	if (!left && zero && width > 0 && (size_t)width > total)
		emit_repeat(output, '0', (size_t)width - total);
	sink.output = output;
	format_magnitude(&sink, &exact, special, conversion, precision, alternate);
	if (left && width > 0 && (size_t)width > total)
		emit_repeat(output, ' ', (size_t)width - total);
}
#endif

static size_t
unsigned_digits(char *reverse, uint64_t value, unsigned int base, int upper)
{
	const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
	size_t length = 0;

	do {
		reverse[length++] = digits[(unsigned int)(value % base)];
		value /= base;
	} while (value != 0);
	return length;
}

static void
emit_integer(struct format_output *output, uint64_t value, int negative,
	unsigned int base, int upper, int alternate, int left, int plus,
	int space, int zero, int width, int precision)
{
	char reverse[32];
	char prefix[3];
	size_t digits = unsigned_digits(reverse, value, base, upper);
	size_t zeroes = 0;
	size_t prefix_length = 0;
	size_t total;
	size_t index;

	if (precision == 0 && value == 0)
		digits = 0;
	if (negative)
		prefix[prefix_length++] = '-';
	else if (plus)
		prefix[prefix_length++] = '+';
	else if (space)
		prefix[prefix_length++] = ' ';
	if (alternate && value != 0 && base == 16) {
		prefix[prefix_length++] = '0';
		prefix[prefix_length++] = upper ? 'X' : 'x';
	} else if (alternate && base == 8 &&
		   (digits == 0 || reverse[digits - 1U] != '0')) {
		prefix[prefix_length++] = '0';
	}
	if (precision > 0 && (size_t)precision > digits)
		zeroes = (size_t)precision - digits;
	if (zero && !left && precision < 0 && width > 0 &&
	    (size_t)width > prefix_length + digits)
		zeroes = (size_t)width - prefix_length - digits;
	total = prefix_length + zeroes + digits;
	if (!left && width > 0 && (size_t)width > total)
		emit_repeat(output, ' ', (size_t)width - total);
	emit_bytes(output, prefix, prefix_length);
	emit_repeat(output, '0', zeroes);
	for (index = digits; index != 0; index--)
		emit_character(output, reverse[index - 1U]);
	if (left && width > 0 && (size_t)width > total)
		emit_repeat(output, ' ', (size_t)width - total);
}

enum length_modifier {
	LENGTH_DEFAULT,
	LENGTH_CHAR,
	LENGTH_SHORT,
	LENGTH_LONG,
	LENGTH_LONG_LONG,
	LENGTH_SIZE
};

int
vsnprintf(char *buffer, size_t size, const char *format, va_list arguments)
{
	struct format_output output = { buffer, size, 0 };

	while (*format != '\0') {
		int alternate, left, plus, space, zero, width, precision;
		enum length_modifier length;
		char conversion;

		if (*format != '%') {
			emit_character(&output, *format++);
			continue;
		}
		format++;
		if (*format == '%') {
			emit_character(&output, *format++);
			continue;
		}
		alternate = left = plus = space = zero = 0;
		for (;;) {
			if (*format == '#') alternate = 1;
			else if (*format == '-') left = 1;
			else if (*format == '+') plus = 1;
			else if (*format == ' ') space = 1;
			else if (*format == '0') zero = 1;
			else break;
			format++;
		}
		width = 0;
		if (*format == '*') {
			width = va_arg(arguments, int);
			format++;
			if (width < 0) {
				left = 1;
				width = -width;
			}
		} else {
			while (*format >= '0' && *format <= '9')
				width = width * 10 + (*format++ - '0');
		}
		precision = -1;
		if (*format == '.') {
			precision = 0;
			format++;
			if (*format == '*') {
				precision = va_arg(arguments, int);
				format++;
				if (precision < 0)
					precision = -1;
			} else {
				while (*format >= '0' && *format <= '9')
					precision = precision * 10 + (*format++ - '0');
			}
		}
		length = LENGTH_DEFAULT;
		if (*format == 'h') {
			format++;
			length = *format == 'h' ? (format++, LENGTH_CHAR) : LENGTH_SHORT;
		} else if (*format == 'l') {
			format++;
			length = *format == 'l' ?
				(format++, LENGTH_LONG_LONG) : LENGTH_LONG;
		} else if (*format == 'z' || *format == 't' || *format == 'j') {
			length = *format == 'j' ? LENGTH_LONG_LONG : LENGTH_SIZE;
			format++;
		}
		conversion = *format == '\0' ? '\0' : *format++;
		if (conversion == 's') {
			if (length == LENGTH_LONG) {
				const wchar_t *text = va_arg(arguments, const wchar_t *);
				size_t text_length;
				if (text == NULL)
					text = L"(null)";
				text_length = wide_text_length(text, precision);
				if (text_length == (size_t)-1)
					return -1;
				if (!left && width > 0 && (size_t)width > text_length)
					emit_repeat(&output, ' ', (size_t)width - text_length);
				if (emit_wide_text(&output, text, text_length) != 0)
					return -1;
				if (left && width > 0 && (size_t)width > text_length)
					emit_repeat(&output, ' ', (size_t)width - text_length);
				continue;
			}
			const char *text = va_arg(arguments, const char *);
			size_t text_length;
			if (text == NULL)
				text = "(null)";
			text_length = precision >= 0 ?
				strnlen(text, (size_t)precision) : strlen(text);
			if (!left && width > 0 && (size_t)width > text_length)
				emit_repeat(&output, ' ', (size_t)width - text_length);
			emit_bytes(&output, text, text_length);
			if (left && width > 0 && (size_t)width > text_length)
				emit_repeat(&output, ' ', (size_t)width - text_length);
		} else if (conversion == 'c') {
			if (length == LENGTH_LONG) {
				wchar_t wide = (wchar_t)va_arg(arguments, wint_t);
				mbstate_t state = { 0 };
				char bytes[4];
				size_t count = wcrtomb(bytes, wide, &state);
				if (count == (size_t)-1)
					return -1;
				if (!left && width > 1)
					emit_repeat(&output, ' ', (size_t)width - 1U);
				emit_bytes(&output, bytes, count);
				if (left && width > 1)
					emit_repeat(&output, ' ', (size_t)width - 1U);
				continue;
			}
			if (!left && width > 1)
				emit_repeat(&output, ' ', (size_t)width - 1U);
			emit_character(&output, (char)va_arg(arguments, int));
			if (left && width > 1)
				emit_repeat(&output, ' ', (size_t)width - 1U);
		} else if (conversion == 'd' || conversion == 'i') {
			int64_t signed_value;
			if (length == LENGTH_LONG_LONG)
				signed_value = va_arg(arguments, long long);
			else if (length == LENGTH_LONG)
				signed_value = va_arg(arguments, long);
			else if (length == LENGTH_SIZE)
				signed_value = va_arg(arguments, ptrdiff_t);
			else
				signed_value = va_arg(arguments, int);
			emit_integer(&output,
				signed_value < 0 ? 0U - (uint64_t)signed_value :
				(uint64_t)signed_value,
				signed_value < 0, 10, 0, alternate, left, plus,
				space, zero, width, precision);
		} else if (conversion == 'u' || conversion == 'x' ||
			   conversion == 'X' || conversion == 'o') {
			uint64_t value;
			unsigned int base = conversion == 'o' ? 8U :
				(conversion == 'u' ? 10U : 16U);
			if (length == LENGTH_LONG_LONG)
				value = va_arg(arguments, unsigned long long);
			else if (length == LENGTH_LONG)
				value = va_arg(arguments, unsigned long);
			else if (length == LENGTH_SIZE)
				value = va_arg(arguments, size_t);
			else
				value = va_arg(arguments, unsigned int);
			emit_integer(&output, value, 0, base, conversion == 'X',
				alternate, left, 0, 0, zero, width, precision);
		} else if (conversion == 'p') {
			uintptr_t value = (uintptr_t)va_arg(arguments, void *);
			emit_integer(&output, value, 0, 16, 0, 1, left, 0, 0,
				zero, width, precision);
		} else if (conversion == 'f' || conversion == 'F' ||
			   conversion == 'e' || conversion == 'E' ||
			   conversion == 'g' || conversion == 'G') {
			#ifdef KERN_NO_PRINTF_FLOAT
			(void)va_arg(arguments, double);
			emit_bytes(&output, "[float]", 7U);
			#else
			double value = va_arg(arguments, double);

			emit_float(&output, value, conversion, alternate, left, plus,
				space, zero, width, precision);
			#endif
		} else if (conversion == '\0') {
			break;
		} else {
			emit_character(&output, '%');
			emit_character(&output, conversion);
		}
	}
	if (size != 0) {
		size_t terminator = output.length < size ? output.length : size - 1U;
		buffer[terminator] = '\0';
	}
	return output.length > (size_t)INT_MAX ? INT_MAX : (int)output.length;
}

int
snprintf(char *buffer, size_t size, const char *format, ...)
{
	va_list arguments;
	int result;
	va_start(arguments, format);
	result = vsnprintf(buffer, size, format, arguments);
	va_end(arguments);
	return result;
}
