/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Where a line may break: a simplified form of UAX #14 for the first pass.
 *
 * A line may break after a space, and between two characters when either
 * is a CJK ideograph, kana or hangul, except before closing punctuation
 * and small kana and after opening punctuation (the basic Japanese
 * kinsoku rules).  The full Line_Break classes come with the Unicode
 * tables (p010's follow-up).
 */

#include "text/text.h"

static int linebreak_is_cjk(uint32_t c);
static int linebreak_no_break_before(uint32_t c);
static int linebreak_no_break_after(uint32_t c);

/*
 * Tells whether a code point is collapsible whitespace (space, tab or line
 * feed).
 */
int
text_is_space(
	uint32_t code_point)
{
	/* The three whitespace characters of HTML text. */
	if (code_point == 0x20U || code_point == 0x09U || code_point == 0x0aU)
		return 1;

	/* Anything else is not. */
	return 0;
}

/*
 * Tells whether a line may break between two code points.
 */
int
text_break_between(
	uint32_t before,
	uint32_t after)
{
	int space;
	int cjk_before;
	int cjk_after;
	int forbidden;

	/* A break after a space. */
	space = text_is_space(before);
	if (space)
		return 1;

	/* Never before closing punctuation, never after opening punctuation. */
	forbidden = linebreak_no_break_before(after);
	if (forbidden)
		return 0;
	forbidden = linebreak_no_break_after(before);
	if (forbidden)
		return 0;

	/* Between CJK characters, or between one and other text. */
	cjk_before = linebreak_is_cjk(before);
	cjk_after = linebreak_is_cjk(after);
	if (cjk_before || cjk_after) {
		space = text_is_space(after);
		if (!space)
			return 1;
	}

	/* Elsewhere inside a word there is no break. */
	return 0;
}

/* Tells whether a code point is a CJK ideograph, kana, hangul or CJK punctuation. */
static int
linebreak_is_cjk(
	uint32_t c)
{
	/* CJK punctuation, hiragana, katakana and the halfwidth forms. */
	if (c >= 0x3000U && c <= 0x30ffU)
		return 1;
	if (c >= 0xff00U && c <= 0xffefU)
		return 1;

	/* The ideographs and their extensions, and the compatibility ideographs. */
	if (c >= 0x3400U && c <= 0x4dbfU)
		return 1;
	if (c >= 0x4e00U && c <= 0x9fffU)
		return 1;
	if (c >= 0xf900U && c <= 0xfaffU)
		return 1;
	if (c >= 0x20000U && c <= 0x3ffffU)
		return 1;

	/* Hangul syllables. */
	if (c >= 0xac00U && c <= 0xd7afU)
		return 1;

	/* Anything else is not CJK. */
	return 0;
}

/* Tells whether a line may not break before a code point (closing punctuation, small kana). */
static int
linebreak_no_break_before(
	uint32_t c)
{
	/* The characters a Japanese line must not start with. */
	switch (c) {
	case 0x3001U:	/* 、 */
	case 0x3002U:	/* 。 */
	case 0xff0cU:	/* ， */
	case 0xff0eU:	/* ． */
	case 0x300dU:	/* 」 */
	case 0x300fU:	/* 』 */
	case 0xff09U:	/* ） */
	case 0x3015U:	/* 〕 */
	case 0x3011U:	/* 】 */
	case 0x30fcU:	/* ー */
	case 0x3063U:	/* っ */
	case 0x30c3U:	/* ッ */
	case 0x3083U:	/* ゃ */
	case 0x3085U:	/* ゅ */
	case 0x3087U:	/* ょ */
	case 0x30e3U:	/* ャ */
	case 0x30e5U:	/* ュ */
	case 0x30e7U:	/* ョ */
	case 0xff01U:	/* ！ */
	case 0xff1fU:	/* ？ */
	case '!':
	case '?':
	case ',':
	case '.':
	case ')':
	case ']':
	case '}':
		return 1;
	default:
		return 0;
	}
}

/* Tells whether a line may not break after a code point (opening punctuation). */
static int
linebreak_no_break_after(
	uint32_t c)
{
	/* The characters a Japanese line must not end with. */
	switch (c) {
	case 0x300cU:	/* 「 */
	case 0x300eU:	/* 『 */
	case 0xff08U:	/* （ */
	case 0x3014U:	/* 〔 */
	case 0x3010U:	/* 【 */
	case '(':
	case '[':
	case '{':
		return 1;
	default:
		return 0;
	}
}
