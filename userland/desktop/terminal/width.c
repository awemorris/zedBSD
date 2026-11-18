/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The width of a character in terminal's grid: one cell or two.
 *
 * East Asian wide and fullwidth characters always take two cells.  The
 * characters whose East Asian Width is Ambiguous (Greek, Cyrillic, circles,
 * box drawing and the like, which CJK fonts and programs draw full width)
 * take two only while the View menu's "Treat Ambiguous-Width Characters as
 * Wide" is on (ws128-p009), and one otherwise, as before.  The Ambiguous
 * characters come from the Unicode data (ambiguous.h, made by
 * ambiguous-gen.py) and are found by a binary search over their ranges, so
 * the width costs a few comparisons per character.
 */

#include "width.h"

#include <stddef.h>

#include "ambiguous.h"

/* The first Ambiguous character (U+00A1); everything below it, ASCII included, is never Ambiguous. */
#define WIDTH_AMBIGUOUS_FIRST	0x00a1U

static int width_east_asian_wide(uint32_t codepoint);

/*
 * Tells whether a character takes two cells: an East Asian wide or
 * fullwidth character always, an Ambiguous one only when ambiguous_wide is
 * nonzero.
 */
int
terminal_width_wide(
	uint32_t codepoint,
	int ambiguous_wide)
{
	int wide;
	int ambiguous;

	/* Wide and fullwidth characters take two cells whatever the setting. */
	wide = width_east_asian_wide(codepoint);
	if (wide)
		return 1;

	/* With the setting off, every other character takes one cell. */
	if (!ambiguous_wide)
		return 0;

	/* With it on, an Ambiguous character takes two. */
	ambiguous = terminal_width_ambiguous(codepoint);
	if (ambiguous)
		return 1;

	/* Everything else is one cell. */
	return 0;
}

/*
 * Tells whether a character's East Asian Width is Ambiguous (combining
 * marks, format characters and controls excluded).
 */
int
terminal_width_ambiguous(
	uint32_t codepoint)
{
	size_t low;
	size_t high;
	size_t middle;

	/* ASCII and the rest of the low range are never Ambiguous. */
	if (codepoint < WIDTH_AMBIGUOUS_FIRST)
		return 0;

	/* Halves the ranges still in question until one holds the character or none is left. */
	low = 0U;
	high = AMBIGUOUS_RANGES;
	while (low < high) {
		/* Compares the character with the range in the middle of those left. */
		middle = low + (high - low) / 2U;
		if (codepoint < ambiguous_ranges[middle].first) {
			/* The character is below this range. */
			high = middle;
		} else if (codepoint > ambiguous_ranges[middle].last) {
			/* The character is above this range. */
			low = middle + 1U;
		} else {
			/* The character is in this range. */
			return 1;
		}
	}

	/* No range holds the character. */
	return 0;
}

/* Tells whether a character is East Asian wide or fullwidth. */
static int
width_east_asian_wide(
	uint32_t codepoint)
{
	/* Hangul Jamo, the leading consonants. */
	if (codepoint >= 0x1100U && codepoint <= 0x115fU)
		return 1;

	/* The angle brackets. */
	if (codepoint == 0x2329U || codepoint == 0x232aU)
		return 1;

	/* CJK radicals to Yi. */
	if (codepoint >= 0x2e80U && codepoint <= 0xa4cfU)
		return 1;

	/* Hangul syllables. */
	if (codepoint >= 0xac00U && codepoint <= 0xd7a3U)
		return 1;

	/* CJK compatibility ideographs. */
	if (codepoint >= 0xf900U && codepoint <= 0xfaffU)
		return 1;

	/* Vertical forms, CJK compatibility forms and small form variants. */
	if (codepoint >= 0xfe10U && codepoint <= 0xfe6fU)
		return 1;

	/* Fullwidth forms. */
	if (codepoint >= 0xff01U && codepoint <= 0xff60U)
		return 1;

	/* Fullwidth signs. */
	if (codepoint >= 0xffe0U && codepoint <= 0xffe6U)
		return 1;

	/* The ideographs of the supplementary planes. */
	if (codepoint >= 0x20000U && codepoint <= 0x3fffdU)
		return 1;

	/* Everything else is one cell. */
	return 0;
}
