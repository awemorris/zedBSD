/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The word of the fingers' selection (ws190-p002, plan/ws190/phase001/
 * phase.md section 2.4): what a double tap of a finger selects in a field,
 * a text area or a text view of a program's own.
 *
 * A word is a run of characters of one kind: ASCII letters, digits and
 * the underscore, or characters past ASCII that are not punctuation
 * (Japanese, accented letters).  Everything else -- spaces, ASCII
 * punctuation, the CJK symbols and punctuation (U+3000 to U+303F) and the
 * full-width ASCII punctuation -- parts words and is no word itself.  A
 * double tap selects the word right of its place, else the one left of it;
 * between two separators it selects nothing.
 */

#include "internal.h"

/* The kinds of character a word is made of. */
#define SELECT_SEPARATOR	0
#define SELECT_ASCII		1
#define SELECT_WIDE		2

static int select_kind(const char *text, size_t length, size_t at, size_t *size);
static size_t select_before(const char *text, size_t at);
static uint32_t select_code(const char *text, size_t length, size_t at, size_t *size);

/*
 * Gives the word around a position of a UTF-8 text (byte offsets on
 * character boundaries): start and end of the word right of the position,
 * else of the one left of it; both the position when neither side is a
 * word's.
 */
void
keiui_select_word(
	const char *text,
	size_t length,
	size_t position,
	size_t *start,
	size_t *end)
{
	size_t at;
	size_t size;
	size_t previous;
	size_t next_size;
	int kind;
	int other;

	/* Nothing selected until a word is found. */
	*start = position;
	*end = position;

	/* The character right of the position. */
	kind = SELECT_SEPARATOR;
	at = position;
	size = 0;
	if (position < length)
		kind = select_kind(text, length, position, &size);

	/* Not a word's: the character left of it. */
	if (kind == SELECT_SEPARATOR && position > 0U) {
		at = select_before(text, position);
		kind = select_kind(text, length, at, &size);
	}

	/* Neither side is a word's. */
	if (kind == SELECT_SEPARATOR)
		return;

	/* Back over the characters of the same kind. */
	*start = at;
	while (*start > 0U) {
		previous = select_before(text, *start);
		other = select_kind(text, length, previous, &next_size);
		if (other != kind)
			break;
		*start = previous;
	}

	/* On over them. */
	*end = at + size;
	while (*end < length) {
		other = select_kind(text, length, *end, &next_size);
		if (other != kind)
			break;
		*end += next_size;
	}
}

/* Tells the kind of the character at a byte offset (SELECT_*), with its size in bytes. */
static int
select_kind(
	const char *text,
	size_t length,
	size_t at,
	size_t *size)
{
	uint32_t code;

	/* The character. */
	code = select_code(text, length, at, size);

	/* ASCII: letters, digits and the underscore are a word's. */
	if (code < 0x80U) {
		if (code >= '0' && code <= '9')
			return SELECT_ASCII;
		if (code >= 'A' && code <= 'Z')
			return SELECT_ASCII;
		if (code >= 'a' && code <= 'z')
			return SELECT_ASCII;
		if (code == '_')
			return SELECT_ASCII;
		return SELECT_SEPARATOR;
	}

	/* The CJK symbols and punctuation, and the full-width ASCII punctuation, part words. */
	if (code >= 0x3000U && code <= 0x303fU)
		return SELECT_SEPARATOR;
	if (code >= 0xff01U && code <= 0xff0fU)
		return SELECT_SEPARATOR;
	if (code >= 0xff1aU && code <= 0xff20U)
		return SELECT_SEPARATOR;

	/* Any other character past ASCII is a word's. */
	return SELECT_WIDE;
}

/* Gives the offset of the character before a character boundary. */
static size_t
select_before(
	const char *text,
	size_t at)
{
	/* Back one byte, then over the continuation bytes. */
	at--;
	while (at > 0U && ((unsigned char)text[at] & 0xc0U) == 0x80U)
		at--;

	/* Reports the character's first byte. */
	return at;
}

/*
 * Decodes the character at a byte offset; a byte that does not start a
 * whole character stands for itself, one byte long.
 */
static uint32_t
select_code(
	const char *text,
	size_t length,
	size_t at,
	size_t *size)
{
	unsigned char lead;
	uint32_t code;
	size_t count;
	size_t index;

	/* The lead byte: ASCII is itself. */
	lead = (unsigned char)text[at];
	*size = 1;
	if (lead < 0x80U)
		return lead;

	/* How many bytes the lead byte says follow. */
	if ((lead & 0xe0U) == 0xc0U) {
		count = 1;
		code = lead & 0x1fU;
	} else if ((lead & 0xf0U) == 0xe0U) {
		count = 2;
		code = lead & 0x0fU;
	} else if ((lead & 0xf8U) == 0xf0U) {
		count = 3;
		code = lead & 0x07U;
	} else {
		return lead;
	}

	/* The continuation bytes; a short or broken one leaves the lead byte alone. */
	if (at + count >= length)
		return lead;
	for (index = 1; index <= count; index++) {
		if (((unsigned char)text[at + index] & 0xc0U) != 0x80U)
			return lead;
		code = (code << 6) | ((unsigned char)text[at + index] & 0x3fU);
	}

	/* Succeeded: the character and its size. */
	*size = count + 1U;
	return code;
}
