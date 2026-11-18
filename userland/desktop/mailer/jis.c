/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Mail's Japanese character sets (ws177-p016; mail.h): ISO-2022-JP (its
 * escapes to ASCII, JIS X 0201's Roman letters, JIS X 0208 and the
 * half-width katakana), Shift_JIS (and Windows-31J's JIS X 0208 part) and
 * EUC-JP, read a character at a time into code points with JIS X 0208's
 * table (jisx0208.h).  A sequence that is not a character of the set, or
 * a cell without one (JIS X 0212's, a vendor's), is U+FFFD.
 */

#include "mail.h"
#include "jisx0208.h"

#include <string.h>

/* The escape character of ISO-2022-JP. */
#define JIS_ESCAPE		0x1bU

/* The code point of the replacement character, and of the first half-width katakana. */
#define JIS_REPLACEMENT		0xfffdUL
#define JIS_KANA_FIRST		0xff61UL

/* What ISO-2022-JP's bytes stand for after the last escape (the state of ml_jis_next). */
#define JIS_STATE_ASCII		0
#define JIS_STATE_KANJI		1
#define JIS_STATE_KANA		2

/* One name of a Japanese character set (as MIME's charset gives it) and which set it is (ML_JIS_*). */
struct jis_name {
	const char *name;
	int charset;
};

/* The names of the character sets, matched in any case.  Constant for the program's life. */
static const struct jis_name jis_names[] = {
	{ "iso-2022-jp", ML_JIS_ISO2022 },
	{ "csiso2022jp", ML_JIS_ISO2022 },
	{ "shift_jis", ML_JIS_SHIFT },
	{ "shift-jis", ML_JIS_SHIFT },
	{ "sjis", ML_JIS_SHIFT },
	{ "x-sjis", ML_JIS_SHIFT },
	{ "ms_kanji", ML_JIS_SHIFT },
	{ "windows-31j", ML_JIS_SHIFT },
	{ "cp932", ML_JIS_SHIFT },
	{ "euc-jp", ML_JIS_EUC },
	{ "x-euc-jp", ML_JIS_EUC },
	{ "eucjp", ML_JIS_EUC }
};

static int jis_iso2022(const unsigned char *bytes, size_t length, size_t *at, int *state, unsigned long *code_point);
static int jis_escape(const unsigned char *bytes, size_t length, size_t *at, int *state);
static unsigned long jis_shift(const unsigned char *bytes, size_t length, size_t *at);
static unsigned long jis_euc(const unsigned char *bytes, size_t length, size_t *at);
static unsigned long jis_cell(unsigned first, unsigned second);
static int jis_same(const char *a, const char *b);

/*
 * Tells which Japanese character set a MIME charset name is (ML_JIS_*),
 * or ML_JIS_NONE for another.
 */
int
ml_jis_charset(
	const char *name)
{
	size_t index;
	int same;

	/* Each name. */
	for (index = 0; index < sizeof(jis_names) / sizeof(jis_names[0]); index++) {
		same = jis_same(name, jis_names[index].name);
		if (same)
			return jis_names[index].charset;
	}

	/* Not a Japanese one. */
	return ML_JIS_NONE;
}

/*
 * Reads the next character of bytes in a Japanese character set from
 * *at, which moves past it; *state is ISO-2022-JP's (0 before the first
 * byte).  Returns 1 with its code point, 0 at the end.
 */
int
ml_jis_next(
	int charset,
	const unsigned char *bytes,
	size_t length,
	size_t *at,
	int *state,
	unsigned long *code_point)
{
	int read;

	/* The end. */
	if (*at >= length)
		return 0;

	/* ISO-2022-JP: its escapes change what the bytes stand for. */
	if (charset == ML_JIS_ISO2022) {
		read = jis_iso2022(bytes, length, at, state, code_point);
		return read;
	}

	/* Shift_JIS. */
	if (charset == ML_JIS_SHIFT) {
		*code_point = jis_shift(bytes, length, at);
		return 1;
	}

	/* EUC-JP. */
	*code_point = jis_euc(bytes, length, at);
	return 1;
}

/* Reads ISO-2022-JP's next character, its escapes taken on the way; 0 at the end. */
static int
jis_iso2022(
	const unsigned char *bytes,
	size_t length,
	size_t *at,
	int *state,
	unsigned long *code_point)
{
	unsigned first;
	int escaped;

	/* Until a character: each escape changes the state. */
	for (;;) {
		/* The end. */
		if (*at >= length)
			return 0;
		first = bytes[*at];

		/* An escape: the state it sets (one not known is the escape character's). */
		if (first == JIS_ESCAPE) {
			escaped = jis_escape(bytes, length, at, state);
			if (escaped)
				continue;
			*at += 1U;
			*code_point = JIS_REPLACEMENT;
			return 1;
		}

		/* Shift In and Shift Out (another way to the katakana) are not characters. */
		if (first == 0x0eU || first == 0x0fU) {
			*at += 1U;
			continue;
		}

		/* A line end is itself whatever the state. */
		if (first == '\r' || first == '\n') {
			*at += 1U;
			*code_point = first;
			return 1;
		}

		/* The half-width katakana. */
		if (*state == JIS_STATE_KANA && first >= 0x21U && first <= 0x5fU) {
			*at += 1U;
			*code_point = JIS_KANA_FIRST + (first - 0x21U);
			return 1;
		}

		/* JIS X 0208: two bytes. */
		if (*state == JIS_STATE_KANJI && first >= 0x21U && first <= 0x7eU) {
			if (*at + 1U >= length) {
				*at += 1U;
				*code_point = JIS_REPLACEMENT;
				return 1;
			}

			/* The cell of the two. */
			*code_point = jis_cell(first, bytes[*at + 1U]);
			*at += 2U;
			return 1;
		}

		/* ASCII (and a byte the state does not take). */
		*at += 1U;
		*code_point = first;
		if (first >= 0x80U)
			*code_point = JIS_REPLACEMENT;
		return 1;
	}
}

/*
 * Takes an escape of ISO-2022-JP at *at: "$@" and "$B" to JIS X 0208,
 * "(B" and "(J" to ASCII, "(I" to the half-width katakana.  Returns 1
 * when it was one of them (*at past it), else 0.
 */
static int
jis_escape(
	const unsigned char *bytes,
	size_t length,
	size_t *at,
	int *state)
{
	unsigned first;
	unsigned second;

	/* Two bytes after the escape. */
	if (*at + 2U >= length)
		return 0;
	first = bytes[*at + 1U];
	second = bytes[*at + 2U];

	/* JIS X 0208 (of 1978 or of 1983). */
	if (first == '$' && (second == '@' || second == 'B')) {
		*state = JIS_STATE_KANJI;
		*at += 3U;
		return 1;
	}

	/* ASCII, or JIS X 0201's Roman letters, taken as ASCII. */
	if (first == '(' && (second == 'B' || second == 'J')) {
		*state = JIS_STATE_ASCII;
		*at += 3U;
		return 1;
	}

	/* The half-width katakana. */
	if (first == '(' && second == 'I') {
		*state = JIS_STATE_KANA;
		*at += 3U;
		return 1;
	}

	/* Not one of them. */
	return 0;
}

/* Reads Shift_JIS's next character. */
static unsigned long
jis_shift(
	const unsigned char *bytes,
	size_t length,
	size_t *at)
{
	unsigned first;
	unsigned second;
	unsigned row;
	unsigned cell;

	/* ASCII. */
	first = bytes[*at];
	if (first < 0x80U) {
		*at += 1U;
		return first;
	}

	/* The half-width katakana. */
	if (first >= 0xa1U && first <= 0xdfU) {
		*at += 1U;
		return JIS_KANA_FIRST + (first - 0xa1U);
	}

	/* Not a first byte of two, or the last byte. */
	if (first < 0x81U || first > 0xfcU || (first > 0x9fU && first < 0xe0U) || *at + 1U >= length) {
		*at += 1U;
		return JIS_REPLACEMENT;
	}

	/* Not a second byte. */
	second = bytes[*at + 1U];
	if (second < 0x40U || second > 0xfcU || second == 0x7fU) {
		*at += 1U;
		return JIS_REPLACEMENT;
	}

	/* The two bytes taken. */
	*at += 2U;

	/* Two rows to each first byte: the second byte says which, and the cell. */
	row = first - 0x81U;
	if (first >= 0xe0U)
		row = first - 0xc1U;
	row = row * 2U + 1U;
	if (second >= 0x80U)
		second--;
	if (second >= 0x9eU) {
		row++;
		cell = second - 0x9eU + 1U;
	} else {
		cell = second - 0x40U + 1U;
	}

	/* Its code point (rows past JIS X 0208's are the users' own). */
	return jis_cell(row + 0x20U, cell + 0x20U);
}

/* Reads EUC-JP's next character. */
static unsigned long
jis_euc(
	const unsigned char *bytes,
	size_t length,
	size_t *at)
{
	unsigned first;
	unsigned second;

	/* ASCII. */
	first = bytes[*at];
	if (first < 0x80U) {
		*at += 1U;
		return first;
	}

	/* The half-width katakana after SS2. */
	if (first == 0x8eU && *at + 1U < length && bytes[*at + 1U] >= 0xa1U && bytes[*at + 1U] <= 0xdfU) {
		second = bytes[*at + 1U];
		*at += 2U;
		return JIS_KANA_FIRST + (second - 0xa1U);
	}

	/* JIS X 0212 after SS3, which the table does not have. */
	if (first == 0x8fU && *at + 2U < length) {
		*at += 3U;
		return JIS_REPLACEMENT;
	}

	/* JIS X 0208: two bytes with their high bits. */
	if (first >= 0xa1U && first <= 0xfeU && *at + 1U < length && bytes[*at + 1U] >= 0xa1U && bytes[*at + 1U] <= 0xfeU) {
		second = bytes[*at + 1U];
		*at += 2U;
		return jis_cell(first - 0x80U, second - 0x80U);
	}

	/* A byte that starts nothing. */
	*at += 1U;
	return JIS_REPLACEMENT;
}

/* Gives the code point of JIS X 0208's cell by its two bytes (0x21 to 0x7e), or U+FFFD. */
static unsigned long
jis_cell(
	unsigned first,
	unsigned second)
{
	unsigned long code_point;
	size_t index;

	/* Out of the table. */
	if (first < 0x21U || second < 0x21U || second > 0x7eU)
		return JIS_REPLACEMENT;
	if (first - 0x21U >= ML_JIS_ROWS)
		return JIS_REPLACEMENT;

	/* The cell. */
	index = (size_t)(first - 0x21U) * ML_JIS_CELLS + (size_t)(second - 0x21U);
	code_point = ml_jisx0208_to_ucs[index];

	/* A cell without a character. */
	if (code_point == 0UL)
		return JIS_REPLACEMENT;

	/* Succeeded: the character. */
	return code_point;
}

/* Tells whether two names are the same in any ASCII case. */
static int
jis_same(
	const char *a,
	const char *b)
{
	size_t index;
	int left;
	int right;

	/* Each byte in lower case. */
	for (index = 0;; index++) {
		left = (unsigned char)a[index];
		right = (unsigned char)b[index];
		if (left >= 'A' && left <= 'Z')
			left = left - 'A' + 'a';
		if (right >= 'A' && right <= 'Z')
			right = right - 'A' + 'a';
		if (left != right)
			return 0;
		if (left == '\0')
			return 1;
	}
}
