/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Finding text in Text Editor's document (plan/ws092/design.md section 11):
 * the bytes of the text searched for, ignoring the case of ASCII letters,
 * forward or backward from a position and round the end of the document.
 */

#include "textedit.h"

static unsigned char find_fold(unsigned char byte);

/*
 * Finds the next place a text occurs: forward, the first at or after
 * from; backward, the last starting before from.  Past the end (or the
 * start) the search goes round, and *wrapped says it did.
 *
 * Returns 1 with the place's start, or 0 when the text does not occur.
 */
int
te_find(
	const struct te_buffer *buffer,
	const char *needle,
	size_t length,
	size_t from,
	int forward,
	size_t *start,
	int *wrapped)
{
	size_t total;
	size_t last;
	size_t position;
	size_t steps;
	int match;

	/* Nothing to find, or a text longer than the document. */
	*wrapped = 0;
	total = te_buffer_length(buffer);
	if (length == 0U || length > total)
		return 0;

	/* The last place a match may start, and the place the search starts. */
	last = total - length;
	if (from > last + 1U)
		from = last + 1U;

	/* Every place once, going round. */
	position = from;
	for (steps = 0; steps <= last + 1U; steps++) {
		/* Forward the place moves on after it is tried; backward before. */
		if (!forward) {
			if (position == 0U) {
				position = last + 1U;
				*wrapped = 1;
			}

			/* The place before. */
			position--;
		} else if (position > last) {
			position = 0;
			*wrapped = 1;
		}

		/* The text at the place. */
		match = te_find_at(buffer, needle, length, position);
		if (match) {
			*start = position;
			return 1;
		}

		/* The next place forward. */
		if (forward)
			position++;
	}

	/* The text is nowhere. */
	return 0;
}

/*
 * Reports whether a text occurs at a position (ASCII letters in either case).
 */
int
te_find_at(
	const struct te_buffer *buffer,
	const char *needle,
	size_t length,
	size_t position)
{
	unsigned char have;
	unsigned char want;
	size_t index;
	size_t total;

	/* A text running past the end does not occur there. */
	total = te_buffer_length(buffer);
	if (position + length > total)
		return 0;

	/* Each byte of the text against the document's. */
	for (index = 0; index < length; index++) {
		have = find_fold(te_buffer_byte(buffer, position + index));
		want = find_fold((unsigned char)needle[index]);
		if (have != want)
			return 0;
	}

	/* Every byte matched: it occurs there. */
	return 1;
}

/* Folds an ASCII capital to its small letter. */
static unsigned char
find_fold(
	unsigned char byte)
{
	/* A capital becomes small. */
	if (byte >= 'A' && byte <= 'Z')
		return (unsigned char)(byte - 'A' + 'a');

	/* Anything else stays. */
	return byte;
}
