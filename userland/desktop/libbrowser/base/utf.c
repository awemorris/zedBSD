/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * UTF-8 and UTF-16, decoded the way the WHATWG Encoding Standard decodes
 * them: a malformed sequence becomes one U+FFFD for its longest valid
 * prefix, and decoding resumes at the byte that broke it.
 */

#include "base/base.h"

#include <errno.h>

static int utf_is_high_surrogate(uint32_t unit);
static int utf_is_low_surrogate(uint32_t unit);

/*
 * Decodes the code point at the start of bytes.
 *
 * Returns how many bytes it took (at least one when length is not zero);
 * a malformed or truncated sequence yields U+FFFD.
 */
size_t
wb_utf8_decode(
	const unsigned char *bytes,
	size_t length,
	uint32_t *code_point)
{
	uint32_t value;
	size_t needed;
	size_t index;
	unsigned char lower;
	unsigned char upper;
	unsigned char lead;

	/* Nothing to decode yields nothing. */
	if (length == 0) {
		*code_point = WB_REPLACEMENT;
		return 0;
	}

	/* The lead byte says how many continuation bytes follow and what the first may be. */
	lead = bytes[0];
	lower = 0x80;
	upper = 0xbf;
	if (lead < 0x80) {
		/* ASCII stands for itself. */
		*code_point = lead;
		return 1;
	} else if (lead >= 0xc2 && lead <= 0xdf) {
		needed = 1;
		value = lead & 0x1fU;
	} else if (lead >= 0xe0 && lead <= 0xef) {
		/* E0 would allow overlong forms and ED surrogates; their second byte is narrowed. */
		needed = 2;
		value = lead & 0x0fU;
		if (lead == 0xe0)
			lower = 0xa0;
		if (lead == 0xed)
			upper = 0x9f;
	} else if (lead >= 0xf0 && lead <= 0xf4) {
		/* F0 would allow overlong forms and F4 values past U+10FFFF. */
		needed = 3;
		value = lead & 0x07U;
		if (lead == 0xf0)
			lower = 0x90;
		if (lead == 0xf4)
			upper = 0x8f;
	} else {
		/* A continuation byte or an impossible lead is one malformed byte. */
		*code_point = WB_REPLACEMENT;
		return 1;
	}

	/* Takes the continuation bytes, stopping at the first that does not fit. */
	for (index = 1; index <= needed; index++) {
		/* A sequence cut short by the end of the input is malformed. */
		if (index >= length) {
			*code_point = WB_REPLACEMENT;
			return index;
		}

		/* A byte outside the allowed range ends the malformed prefix before it. */
		if (bytes[index] < lower || bytes[index] > upper) {
			*code_point = WB_REPLACEMENT;
			return index;
		}

		/* Adds the byte's six bits; only the first continuation byte has a narrowed range. */
		value = (value << 6) | (bytes[index] & 0x3fU);
		lower = 0x80;
		upper = 0xbf;
	}

	/* Succeeded: the whole sequence decoded. */
	*code_point = value;
	return needed + 1U;
}

/*
 * Encodes one code point as UTF-8 into out (room for four bytes).
 *
 * A surrogate or a value past U+10FFFF is encoded as U+FFFD.  Returns the
 * number of bytes written.
 */
size_t
wb_utf8_encode(
	uint32_t code_point,
	unsigned char *out)
{
	/* Values UTF-8 cannot carry become the replacement character. */
	if (code_point > WB_CODE_POINT_MAX)
		code_point = WB_REPLACEMENT;
	if (code_point >= 0xd800U && code_point <= 0xdfffU)
		code_point = WB_REPLACEMENT;

	/* Writes one to four bytes by the size of the value. */
	if (code_point < 0x80U) {
		out[0] = (unsigned char)code_point;
		return 1;
	} else if (code_point < 0x800U) {
		out[0] = (unsigned char)(0xc0U | (code_point >> 6));
		out[1] = (unsigned char)(0x80U | (code_point & 0x3fU));
		return 2;
	} else if (code_point < 0x10000U) {
		out[0] = (unsigned char)(0xe0U | (code_point >> 12));
		out[1] = (unsigned char)(0x80U | ((code_point >> 6) & 0x3fU));
		out[2] = (unsigned char)(0x80U | (code_point & 0x3fU));
		return 3;
	}

	/* A supplementary code point takes four bytes. */
	out[0] = (unsigned char)(0xf0U | (code_point >> 18));
	out[1] = (unsigned char)(0x80U | ((code_point >> 12) & 0x3fU));
	out[2] = (unsigned char)(0x80U | ((code_point >> 6) & 0x3fU));
	out[3] = (unsigned char)(0x80U | (code_point & 0x3fU));
	return 4;
}

/*
 * Decodes the code point at the start of UTF-16 units.
 *
 * A surrogate pair yields one supplementary code point; a lone surrogate
 * is returned as itself (the DOM and JavaScript keep lone surrogates).
 * Returns how many units it took.
 */
size_t
wb_utf16_decode(
	const uint16_t *units,
	size_t length,
	uint32_t *code_point)
{
	uint32_t high;
	uint32_t low;
	int paired;

	/* Nothing to decode yields nothing. */
	if (length == 0) {
		*code_point = WB_REPLACEMENT;
		return 0;
	}

	/* Anything but a high surrogate with a low one after it stands alone. */
	high = units[0];
	paired = utf_is_high_surrogate(high);
	if (!paired || length < 2) {
		*code_point = high;
		return 1;
	}

	/* The unit after it must be a low surrogate. */
	low = units[1];
	paired = utf_is_low_surrogate(low);
	if (!paired) {
		*code_point = high;
		return 1;
	}

	/* Succeeded: the pair combines into one code point. */
	*code_point = 0x10000U + ((high - 0xd800U) << 10) + (low - 0xdc00U);
	return 2;
}

/*
 * Encodes one code point as UTF-16 into out (room for two units).
 *
 * Returns the number of units written; a value past U+10FFFF is written as
 * U+FFFD.
 */
size_t
wb_utf16_encode(
	uint32_t code_point,
	uint16_t *out)
{
	uint32_t offset;

	/* Values past the last code point become the replacement character. */
	if (code_point > WB_CODE_POINT_MAX)
		code_point = WB_REPLACEMENT;

	/* A code point of the basic plane is one unit. */
	if (code_point < 0x10000U) {
		out[0] = (uint16_t)code_point;
		return 1;
	}

	/* A supplementary code point is a surrogate pair. */
	offset = code_point - 0x10000U;
	out[0] = (uint16_t)(0xd800U + (offset >> 10));
	out[1] = (uint16_t)(0xdc00U + (offset & 0x3ffU));
	return 2;
}

/*
 * Decodes UTF-8 bytes and appends them to a UTF-16 buffer.
 */
int
wb_utf8_to_units(
	const unsigned char *bytes,
	size_t length,
	struct wb_units *units)
{
	uint32_t code_point;
	size_t offset;
	size_t used;
	int error;

	/* Room for one unit per byte is always enough. */
	error = wb_units_reserve(units, length);
	if (error != 0)
		return error;

	/* Decodes each code point and appends its units. */
	offset = 0;
	while (offset < length) {
		used = wb_utf8_decode(bytes + offset, length - offset, &code_point);
		offset += used;

		/* Appends it as UTF-16. */
		error = wb_units_append_code_point(units, code_point);
		if (error != 0)
			return error;
	}

	/* Succeeded: the text is at the end of the buffer. */
	return 0;
}

/*
 * Encodes UTF-16 units as UTF-8 and appends them to a byte buffer.
 *
 * Lone surrogates, which UTF-8 cannot carry, become U+FFFD.
 */
int
wb_units_to_utf8(
	const uint16_t *units,
	size_t length,
	struct wb_buffer *buffer)
{
	uint32_t code_point;
	size_t offset;
	size_t used;
	int error;

	/* Encodes each code point in turn. */
	offset = 0;
	while (offset < length) {
		used = wb_utf16_decode(units + offset, length - offset, &code_point);
		offset += used;

		/* Appends it as UTF-8. */
		error = wb_buffer_append_utf8(buffer, code_point);
		if (error != 0)
			return error;
	}

	/* Succeeded: the text is at the end of the buffer. */
	return 0;
}

/* Tells whether a UTF-16 unit is the first half of a surrogate pair. */
static int
utf_is_high_surrogate(
	uint32_t unit)
{
	/* The high surrogates are D800 to DBFF. */
	if (unit >= 0xd800U && unit <= 0xdbffU)
		return 1;

	/* Anything else is not. */
	return 0;
}

/* Tells whether a UTF-16 unit is the second half of a surrogate pair. */
static int
utf_is_low_surrogate(
	uint32_t unit)
{
	/* The low surrogates are DC00 to DFFF. */
	if (unit >= 0xdc00U && unit <= 0xdfffU)
		return 1;

	/* Anything else is not. */
	return 0;
}
