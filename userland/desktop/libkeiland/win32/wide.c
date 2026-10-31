/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * UTF-16 and UTF-8 for the Windows backend.
 *
 * The forms and the treatment of ill-formed text follow the Unicode
 * Standard, section 3.9: the well-formed UTF-8 byte sequences of table 3-7,
 * the surrogate code points of UTF-16, and the replacement of each maximal
 * subpart of an ill-formed UTF-8 sequence by one U+FFFD.
 */

#include "userland/desktop/libkeiland/win32/wide.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

/* The character that stands for text that could not be decoded. */
#define REPLACEMENT_CHARACTER	0xFFFDU

/* The first and last high surrogates (the first unit of a pair). */
#define HIGH_SURROGATE_FIRST	0xD800U
#define HIGH_SURROGATE_LAST	0xDBFFU

/* The first and last low surrogates (the second unit of a pair). */
#define LOW_SURROGATE_FIRST	0xDC00U
#define LOW_SURROGATE_LAST	0xDFFFU

/* The first character that needs a pair of UTF-16 units. */
#define SUPPLEMENTARY_FIRST	0x10000UL

/* The most UTF-8 bytes one character takes. */
#define UTF8_MAX_BYTES		4U

/* Forward declarations. */
static int is_high_surrogate(uint32_t unit);
static int is_low_surrogate(uint32_t unit);
static uint32_t join_surrogates(uint32_t high, uint32_t low);
static size_t encode_utf8(uint32_t code_point, unsigned char *bytes);
static size_t decode_utf8(const unsigned char *bytes, size_t available, uint32_t *code_point);
static size_t encode_utf16(uint32_t code_point, uint16_t *units);
static size_t utf8_scan_units(const unsigned char *bytes, size_t length, uint16_t *units, size_t capacity);

/*
 * Converts UTF-16 to UTF-8, writing whole characters only.
 *
 * An unpaired surrogate becomes U+FFFD.  When the next character does not
 * fit together with the closing NUL, the text is cut before it and ERANGE
 * is reported.
 */
int
keiui_win32_utf8_from_utf16(
	const uint16_t *units,
	size_t count,
	char *buffer,
	size_t size,
	size_t *consumed)
{
	unsigned char bytes[UTF8_MAX_BYTES];
	size_t index;
	size_t written;
	size_t length;
	size_t step;
	size_t byte_index;
	uint32_t code_point;
	uint32_t unit;
	int error;

	/* Nothing has been converted yet. */
	if (consumed != NULL)
		*consumed = 0;

	/* Refuses arguments that leave no place to read from or write to. */
	if (buffer == NULL || size == 0)
		return EINVAL;
	if (units == NULL && count > 0)
		return EINVAL;

	/*
	 * Converts character by character.  A character is encoded into a small
	 * scratch array first so that it is copied only if all of it fits.
	 */
	index = 0;
	written = 0;
	error = 0;
	while (index < count) {
		unit = units[index];

		/* A NUL unit ends the text and is not part of it. */
		if (unit == 0)
			break;

		/* Reads one character: a pair, a lone surrogate or a plain unit. */
		step = 1;
		code_point = unit;
		if (is_high_surrogate(unit)) {
			if (index + 1 < count && is_low_surrogate(units[index + 1])) {
				code_point = join_surrogates(unit, units[index + 1]);
				step = 2;
			} else {
				code_point = REPLACEMENT_CHARACTER;
			}
		} else if (is_low_surrogate(unit)) {
			code_point = REPLACEMENT_CHARACTER;
		}

		/* Writes the character in UTF-8 form to the scratch array. */
		length = encode_utf8(code_point, bytes);

		/* Stops before a character that cannot fit with the closing NUL. */
		if (length >= size - written) {
			error = ERANGE;
			break;
		}

		/* Copies the whole character into the buffer. */
		for (byte_index = 0;
		     byte_index < length;
		     byte_index++) {
			buffer[written + byte_index] = (char)bytes[byte_index];
		}

		written += length;
		index += step;
	}

	/* Closes the text: the buffer always holds a NUL string. */
	buffer[written] = '\0';

	/* Tells the caller how many units were converted. */
	if (consumed != NULL)
		*consumed = index;

	/* Reports a text that was cut short. */
	if (error != 0)
		return error;

	/* Succeeded: the whole text is in the buffer. */
	return 0;
}

/*
 * Converts UTF-8 to UTF-16.
 *
 * The text is measured first and written only when it fits with its NUL
 * unit, so a text that does not fit leaves an empty string and the count
 * of the units it needs.
 */
int
keiui_win32_utf16_from_utf8(
	const char *text,
	size_t length,
	uint16_t *units,
	size_t capacity,
	size_t *count)
{
	size_t needed;

	/* Refuses arguments that leave nothing to read from or write to. */
	if (count == NULL)
		return EINVAL;
	*count = 0;
	if (units == NULL || capacity == 0)
		return EINVAL;
	if (text == NULL && length > 0)
		return EINVAL;

	/* Measures how many units the whole text needs. */
	needed = utf8_scan_units((const unsigned char *)text, length, NULL, 0);

	/* Leaves an empty string and the needed size when the text cannot fit. */
	if (needed >= capacity) {
		units[0] = 0;
		*count = needed;
		return ERANGE;
	}

	/* Writes the units and the closing NUL unit. */
	utf8_scan_units((const unsigned char *)text, length, units, capacity);
	units[needed] = 0;
	*count = needed;

	/* Succeeded: the whole text is converted. */
	return 0;
}

/*
 * Takes one WM_CHAR unit and reports the character it completes.
 *
 * A waiting high surrogate is replaced by a later high one (reported as
 * U+FFFD) or dropped when a unit outside the surrogates follows it, because
 * a call reports one character at most.
 */
int
keiui_win32_char_unit(
	struct keiui_win32_surrogate *state,
	uint16_t unit,
	uint32_t *code_point)
{
	/* Changes nothing without a state and a place for the character. */
	if (state == NULL || code_point == NULL)
		return 0;

	/* A high surrogate waits for its low one; a waiting one is lost. */
	if (is_high_surrogate(unit)) {
		if (state->high != 0) {
			state->high = unit;
			*code_point = REPLACEMENT_CHARACTER;
			return 1;
		}

		state->high = unit;
		return 0;
	}

	/* A low surrogate completes a pair, or stands alone and is replaced. */
	if (is_low_surrogate(unit)) {
		if (state->high != 0) {
			*code_point = join_surrogates(state->high, unit);
			state->high = 0;
			return 1;
		}

		*code_point = REPLACEMENT_CHARACTER;
		return 1;
	}

	/* Any other unit is a character by itself; a waiting high one is dropped. */
	state->high = 0;
	*code_point = unit;

	/* Reports a complete character. */
	return 1;
}

/*
 * Tells whether a UTF-16 unit is a high (leading) surrogate.
 */
static int
is_high_surrogate(
	uint32_t unit)
{
	/* The high surrogates are U+D800 to U+DBFF. */
	if (unit < HIGH_SURROGATE_FIRST)
		return 0;
	if (unit > HIGH_SURROGATE_LAST)
		return 0;

	/* The unit is in the high range. */
	return 1;
}

/*
 * Tells whether a UTF-16 unit is a low (trailing) surrogate.
 */
static int
is_low_surrogate(
	uint32_t unit)
{
	/* The low surrogates are U+DC00 to U+DFFF. */
	if (unit < LOW_SURROGATE_FIRST)
		return 0;
	if (unit > LOW_SURROGATE_LAST)
		return 0;

	/* The unit is in the low range. */
	return 1;
}

/*
 * Joins a surrogate pair into the character above U+FFFF it stands for.
 */
static uint32_t
join_surrogates(
	uint32_t high,
	uint32_t low)
{
	uint32_t high_bits;
	uint32_t low_bits;

	/* Takes the ten payload bits of each unit. */
	high_bits = high - HIGH_SURROGATE_FIRST;
	low_bits = low - LOW_SURROGATE_FIRST;

	/* The high bits come first, and the pairs start at U+10000. */
	return (uint32_t)(SUPPLEMENTARY_FIRST + ((unsigned long)high_bits << 10) + low_bits);
}

/*
 * Writes a character as UTF-8 and gives the number of bytes it took.
 *
 * The caller passes a Unicode scalar value (never a surrogate), so the
 * result is always well-formed.
 */
static size_t
encode_utf8(
	uint32_t code_point,
	unsigned char *bytes)
{
	/* One byte for U+0000 to U+007F. */
	if (code_point < 0x80U) {
		bytes[0] = (unsigned char)code_point;
		return 1;
	}

	/* Two bytes for U+0080 to U+07FF. */
	if (code_point < 0x800U) {
		bytes[0] = (unsigned char)(0xC0U | (code_point >> 6));
		bytes[1] = (unsigned char)(0x80U | (code_point & 0x3FU));
		return 2;
	}

	/* Three bytes for the rest of the BMP. */
	if (code_point < 0x10000U) {
		bytes[0] = (unsigned char)(0xE0U | (code_point >> 12));
		bytes[1] = (unsigned char)(0x80U | ((code_point >> 6) & 0x3FU));
		bytes[2] = (unsigned char)(0x80U | (code_point & 0x3FU));
		return 3;
	}

	/* Four bytes above the BMP. */
	bytes[0] = (unsigned char)(0xF0U | (code_point >> 18));
	bytes[1] = (unsigned char)(0x80U | ((code_point >> 12) & 0x3FU));
	bytes[2] = (unsigned char)(0x80U | ((code_point >> 6) & 0x3FU));
	bytes[3] = (unsigned char)(0x80U | (code_point & 0x3FU));
	return 4;
}

/*
 * Decodes the character at the start of a UTF-8 text.
 *
 * bytes:      the text; the first byte is not NUL.
 * available:  how many bytes may be read (above 0).
 * code_point: receives the character, or U+FFFD for an ill-formed sequence.
 * Returns how many bytes were used (1 to 4, at least 1).  For an ill-formed
 * sequence that is its maximal subpart: the longest prefix that could begin
 * a well-formed sequence.  A byte that cannot begin one is a subpart of its
 * own.  Which second byte may follow a lead byte is table 3-7 of the
 * Unicode Standard (it excludes overlong forms, surrogates and values above
 * U+10FFFF).
 */
static size_t
decode_utf8(
	const unsigned char *bytes,
	size_t available,
	uint32_t *code_point)
{
	uint32_t lead;
	uint32_t value;
	uint32_t next;
	uint32_t low_limit;
	uint32_t high_limit;
	uint32_t second_low;
	uint32_t second_high;
	size_t total;
	size_t index;

	lead = bytes[0];

	/* An ASCII byte is a character by itself. */
	if (lead < 0x80U) {
		*code_point = lead;
		return 1;
	}

	/*
	 * Works out how long the sequence is and which values its second byte
	 * may take.  The other continuation bytes are always 80 to BF.
	 */
	second_low = 0x80U;
	second_high = 0xBFU;
	if (lead >= 0xC2U && lead <= 0xDFU) {
		total = 2;
		value = lead & 0x1FU;
	} else if (lead >= 0xE0U && lead <= 0xEFU) {
		total = 3;
		value = lead & 0x0FU;
		if (lead == 0xE0U) {
			/* E0 80 to E0 9F would be an overlong form. */
			second_low = 0xA0U;
		} else if (lead == 0xEDU) {
			/* ED A0 to ED BF would encode a surrogate. */
			second_high = 0x9FU;
		}
	} else if (lead >= 0xF0U && lead <= 0xF4U) {
		total = 4;
		value = lead & 0x07U;
		if (lead == 0xF0U) {
			/* F0 80 to F0 8F would be an overlong form. */
			second_low = 0x90U;
		} else if (lead == 0xF4U) {
			/* F4 90 and above would exceed U+10FFFF. */
			second_high = 0x8FU;
		}
	} else {
		/* 80 to C1 and F5 to FF never begin a well-formed sequence. */
		*code_point = REPLACEMENT_CHARACTER;
		return 1;
	}

	/* Reads the continuation bytes until one is missing or wrong. */
	for (index = 1;
	     index < total;
	     index++) {
		/* The text ends inside the sequence: what was read is the subpart. */
		if (index >= available) {
			*code_point = REPLACEMENT_CHARACTER;
			return index;
		}

		/* The second byte has its own range, the others are 80 to BF. */
		next = bytes[index];
		low_limit = 0x80U;
		high_limit = 0xBFU;
		if (index == 1) {
			low_limit = second_low;
			high_limit = second_high;
		}

		/* A byte outside the range ends the subpart before it. */
		if (next < low_limit || next > high_limit) {
			*code_point = REPLACEMENT_CHARACTER;
			return index;
		}

		value = (value << 6) | (next & 0x3FU);
	}

	/* The whole sequence was well-formed. */
	*code_point = value;
	return total;
}

/*
 * Writes a character as UTF-16 and gives the number of units it took.
 *
 * The caller passes a Unicode scalar value.
 */
static size_t
encode_utf16(
	uint32_t code_point,
	uint16_t *units)
{
	uint32_t offset;

	/* The BMP is one unit. */
	if (code_point < SUPPLEMENTARY_FIRST) {
		units[0] = (uint16_t)code_point;
		return 1;
	}

	/* Above the BMP, the offset from U+10000 is split into two ten-bit halves. */
	offset = code_point - (uint32_t)SUPPLEMENTARY_FIRST;
	units[0] = (uint16_t)(HIGH_SURROGATE_FIRST + (offset >> 10));
	units[1] = (uint16_t)(LOW_SURROGATE_FIRST + (offset & 0x3FFU));
	return 2;
}

/*
 * Converts a UTF-8 text to UTF-16 and gives how many units the whole text
 * takes, not counting the NUL unit.
 *
 * A NUL byte ends the text.  When units is NULL nothing is written; when it
 * is not, units must hold the whole text (the caller measured it first), and
 * capacity is the room it has.
 */
static size_t
utf8_scan_units(
	const unsigned char *bytes,
	size_t length,
	uint16_t *units,
	size_t capacity)
{
	uint16_t pair[2];
	size_t position;
	size_t step;
	size_t produced;
	size_t unit_count;
	size_t unit_index;
	uint32_t code_point;

	position = 0;
	produced = 0;

	/* Walks the text one character (or one ill-formed subpart) at a time. */
	while (position < length) {
		/* A NUL byte ends the text. */
		if (bytes[position] == 0)
			break;

		step = decode_utf8(bytes + position, length - position, &code_point);
		unit_count = encode_utf16(code_point, pair);

		/* Stores the units when asked, never past the room given. */
		if (units != NULL) {
			for (unit_index = 0;
			     unit_index < unit_count;
			     unit_index++) {
				if (produced + unit_index < capacity)
					units[produced + unit_index] = pair[unit_index];
			}
		}

		produced += unit_count;
		position += step;
	}

	/* The number of units the text takes. */
	return produced;
}
