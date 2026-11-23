/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The text of the X server's Windows backend: UTF-8 and UTF-16, line
 * ends, and the titles.
 */

#include "userland/desktop/xserver/win32/text.h"

#include <stddef.h>
#include <stdint.h>

/* The character that stands for a broken sequence. */
#define TEXT_REPLACEMENT	0xFFFDU

/* The surrogates of UTF-16 and the last Unicode code point. */
#define TEXT_HIGH_FIRST		0xD800U
#define TEXT_HIGH_LAST		0xDBFFU
#define TEXT_LOW_FIRST		0xDC00U
#define TEXT_LOW_LAST		0xDFFFU
#define TEXT_LAST		0x10FFFFU

/* The line-end characters. */
#define TEXT_CR			0x0DU
#define TEXT_LF			0x0AU

/*
 * A text being written into a buffer that may be too small: what has been
 * written, the capacity, and how much the whole text needs.
 */
struct text_out16 {
	uint16_t *units;
	size_t capacity;
	size_t written;
	size_t needed;
	int full;
};

/*
 * The UTF-8 counterpart of struct text_out16.
 */
struct text_out8 {
	uint8_t *bytes;
	size_t capacity;
	size_t written;
	size_t needed;
	int full;
};

static uint32_t text_decode8(const uint8_t *text, size_t size, size_t *length);
static void text_put16(struct text_out16 *out, uint32_t character);
static void text_put8(struct text_out8 *out, uint32_t character);
static void text_put_byte(struct text_out8 *out, uint8_t byte);
static void text_put_unit(struct text_out16 *out, uint16_t unit);

/*
 * Converts UTF-8 to UTF-16.
 *
 * The text ends at size bytes or at a NUL.  A broken sequence becomes
 * U+FFFD.  crlf writes each LF that has no CR before it as CR LF (the line
 * end of the Windows clipboard).  At most capacity - 1 units are written
 * and a NUL after them (nothing with a capacity of 0).  Returns the units
 * the whole text needs without its NUL, so a call with a capacity of 0
 * measures it.
 */
size_t
x11_win32_utf8_to_utf16(
	const uint8_t *text,
	size_t size,
	int crlf,
	uint16_t *result,
	size_t capacity)
{
	struct text_out16 out;
	size_t offset;
	size_t length;
	uint32_t character;
	uint32_t previous;

	/* Starts an empty output. */
	out.units = result;
	out.capacity = capacity;
	out.written = 0;
	out.needed = 0;
	out.full = 0;

	/* Converts each character up to the end or a NUL. */
	offset = 0;
	previous = 0;
	while (offset < size && text[offset] != 0) {
		character = text_decode8(text + offset, size - offset, &length);
		offset += length;

		/* A bare LF becomes CR LF when asked. */
		if (crlf && character == TEXT_LF && previous != TEXT_CR)
			text_put16(&out, TEXT_CR);

		text_put16(&out, character);
		previous = character;
	}

	/* Ends the written text with a NUL. */
	if (capacity != 0)
		result[out.written] = 0;

	/* Succeeded: the units the whole text needs. */
	return out.needed;
}

/*
 * Converts UTF-16 to UTF-8.
 *
 * The text ends at count units or at a NUL.  A lone surrogate becomes
 * U+FFFD.  lf writes each CR LF as LF (X's line end).  At most capacity - 1
 * bytes are written, never a part of a character, and a NUL after them
 * (nothing with a capacity of 0).  Returns the bytes the whole text needs
 * without its NUL.
 */
size_t
x11_win32_utf16_to_utf8(
	const uint16_t *text,
	size_t count,
	int lf,
	uint8_t *result,
	size_t capacity)
{
	struct text_out8 out;
	size_t offset;
	uint32_t character;
	uint32_t low;

	/* Starts an empty output. */
	out.bytes = result;
	out.capacity = capacity;
	out.written = 0;
	out.needed = 0;
	out.full = 0;

	/* Converts each character up to the end or a NUL. */
	offset = 0;
	while (offset < count && text[offset] != 0) {
		character = text[offset];
		offset++;

		/* A high surrogate takes the low one after it; a lone surrogate is broken. */
		if (character >= TEXT_HIGH_FIRST && character <= TEXT_HIGH_LAST) {
			low = 0;
			if (offset < count)
				low = text[offset];
			if (low >= TEXT_LOW_FIRST && low <= TEXT_LOW_LAST) {
				character = 0x10000U + ((character - TEXT_HIGH_FIRST) << 10) + (low - TEXT_LOW_FIRST);
				offset++;
			} else {
				character = TEXT_REPLACEMENT;
			}
		} else if (character >= TEXT_LOW_FIRST && character <= TEXT_LOW_LAST) {
			character = TEXT_REPLACEMENT;
		}

		/* A CR before an LF is dropped when asked. */
		if (lf && character == TEXT_CR && offset < count && text[offset] == TEXT_LF)
			continue;

		text_put8(&out, character);
	}

	/* Ends the written text with a NUL. */
	if (capacity != 0)
		result[out.written] = 0;

	/* Succeeded: the bytes the whole text needs. */
	return out.needed;
}

/*
 * Makes a window's title: "[host] " in front of the X title when a host is
 * named, so a remote window cannot pass for a window of this computer.
 *
 * host and title are UTF-8 (either may be NULL).  The result is cut to
 * capacity - 1 units, never inside a surrogate pair, and ends with a NUL;
 * capacity is at least 1.
 */
void
x11_win32_title(
	const char *host,
	const char *title,
	uint16_t *result,
	size_t capacity)
{
	struct text_out16 out;
	size_t offset;
	size_t length;
	size_t size;
	uint32_t character;

	/* Starts an empty output. */
	out.units = result;
	out.capacity = capacity;
	out.written = 0;
	out.needed = 0;
	out.full = 0;

	/* Writes the host's name in brackets. */
	if (host != NULL && host[0] != '\0') {
		text_put16(&out, '[');
		size = 0;
		while (host[size] != '\0')
			size++;
		offset = 0;
		while (offset < size) {
			character = text_decode8((const uint8_t *)host + offset, size - offset, &length);
			offset += length;
			text_put16(&out, character);
		}
		text_put16(&out, ']');
		text_put16(&out, ' ');
	}

	/* Writes the X title. */
	if (title != NULL) {
		size = 0;
		while (title[size] != '\0')
			size++;
		offset = 0;
		while (offset < size) {
			character = text_decode8((const uint8_t *)title + offset, size - offset, &length);
			offset += length;
			text_put16(&out, character);
		}
	}

	/* Ends the title with a NUL. */
	result[out.written] = 0;
}

/*
 * Decodes one UTF-8 character of the text (size bytes, at least one),
 * setting *length to the bytes it took: U+FFFD and one byte for a broken
 * sequence, an overlong form, a surrogate or a value past Unicode.
 */
static uint32_t
text_decode8(
	const uint8_t *text,
	size_t size,
	size_t *length)
{
	uint32_t character;
	uint32_t smallest;
	size_t count;
	size_t i;

	/* One byte of ASCII. */
	*length = 1;
	if (text[0] < 0x80U)
		return text[0];

	/* The lead byte gives the length and the first bits. */
	if ((text[0] & 0xE0U) == 0xC0U) {
		count = 2;
		character = text[0] & 0x1FU;
		smallest = 0x80U;
	} else if ((text[0] & 0xF0U) == 0xE0U) {
		count = 3;
		character = text[0] & 0x0FU;
		smallest = 0x800U;
	} else if ((text[0] & 0xF8U) == 0xF0U) {
		count = 4;
		character = text[0] & 0x07U;
		smallest = 0x10000U;
	} else {
		return TEXT_REPLACEMENT;
	}

	/* A sequence cut by the end is broken. */
	if (count > size)
		return TEXT_REPLACEMENT;

	/* Each continuation byte adds six bits. */
	for (i = 1; i < count; i++) {
		if ((text[i] & 0xC0U) != 0x80U)
			return TEXT_REPLACEMENT;

		character = (character << 6) | (text[i] & 0x3FU);
	}

	/* Refuses overlong forms, surrogates and values past Unicode. */
	if (character < smallest)
		return TEXT_REPLACEMENT;
	if (character >= TEXT_HIGH_FIRST && character <= TEXT_LOW_LAST)
		return TEXT_REPLACEMENT;
	if (character > TEXT_LAST)
		return TEXT_REPLACEMENT;

	/* Succeeded: the character and its length. */
	*length = count;
	return character;
}

/* Writes one character as UTF-16, as one unit or a surrogate pair kept whole. */
static void
text_put16(
	struct text_out16 *out,
	uint32_t character)
{
	uint32_t offset;

	/* A character of the basic plane is one unit. */
	if (character < 0x10000U) {
		text_put_unit(out, (uint16_t)character);
		return;
	}

	/* A pair that would not fit whole is not started, nor anything after it. */
	if (out->written + 2 >= out->capacity)
		out->full = 1;

	/* Any other character is a surrogate pair. */
	offset = character - 0x10000U;
	text_put_unit(out, (uint16_t)(TEXT_HIGH_FIRST + (offset >> 10)));
	text_put_unit(out, (uint16_t)(TEXT_LOW_FIRST + (offset & 0x3FFU)));
}

/* Writes one UTF-16 unit where it fits and counts it. */
static void
text_put_unit(
	struct text_out16 *out,
	uint16_t unit)
{
	/* Writes the unit while a place before the NUL's is left, and nothing after a unit that did not fit. */
	if (!out->full && out->written + 1 < out->capacity) {
		out->units[out->written] = unit;
		out->written++;
	} else {
		out->full = 1;
	}

	out->needed++;
}

/* Writes one character as UTF-8, kept whole. */
static void
text_put8(
	struct text_out8 *out,
	uint32_t character)
{
	size_t count;

	/* The number of bytes of the character. */
	count = 4;
	if (character < 0x80U) {
		count = 1;
	} else if (character < 0x800U) {
		count = 2;
	} else if (character < 0x10000U) {
		count = 3;
	}

	/* A character that would not fit whole is not started, nor anything after it. */
	if (out->written + count >= out->capacity)
		out->full = 1;

	/* Writes the lead byte and the continuation bytes. */
	if (count == 1) {
		text_put_byte(out, (uint8_t)character);
	} else if (count == 2) {
		text_put_byte(out, (uint8_t)(0xC0U | (character >> 6)));
		text_put_byte(out, (uint8_t)(0x80U | (character & 0x3FU)));
	} else if (count == 3) {
		text_put_byte(out, (uint8_t)(0xE0U | (character >> 12)));
		text_put_byte(out, (uint8_t)(0x80U | ((character >> 6) & 0x3FU)));
		text_put_byte(out, (uint8_t)(0x80U | (character & 0x3FU)));
	} else {
		text_put_byte(out, (uint8_t)(0xF0U | (character >> 18)));
		text_put_byte(out, (uint8_t)(0x80U | ((character >> 12) & 0x3FU)));
		text_put_byte(out, (uint8_t)(0x80U | ((character >> 6) & 0x3FU)));
		text_put_byte(out, (uint8_t)(0x80U | (character & 0x3FU)));
	}
}

/* Writes one byte where it fits and counts it. */
static void
text_put_byte(
	struct text_out8 *out,
	uint8_t byte)
{
	/* Writes the byte while a place before the NUL's is left, and nothing after a byte that did not fit. */
	if (!out->full && out->written + 1 < out->capacity) {
		out->bytes[out->written] = byte;
		out->written++;
	} else {
		out->full = 1;
	}

	out->needed++;
}
