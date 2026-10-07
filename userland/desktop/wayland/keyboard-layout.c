/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The flick panel's layouts (ws102-p003, plan/ws102/design.md §2.4).
 *
 * Each face is a grid of four columns and four rows.  The three left
 * columns are the twelve keys of a telephone keypad; the last column is the
 * same on every face: delete, space, enter and the face key.  A key types
 * its centre character when it is tapped and the character of a direction
 * when it is flicked that way; a flick is a movement from the press of at
 * least max(16, a third of the key) pixels, its direction the nearer axis.
 *
 * The kana face is the Japanese twelve-key layout (あ in the middle,
 * い う え お to the left, up, right and down); its voice key turns the
 * last kana into its voiced, half-voiced or small form and back.  The alpha
 * face is the telephone's letters, with a case key; the number face has
 * the digits, with the ASCII symbols the alpha face does not have around
 * them.  Between the alpha and the number face every lower-case letter,
 * digit and printable ASCII symbol can be typed (the host's tests check
 * it).
 */

#include "keyboard.h"

#include <string.h>

/* The smallest movement that is a flick, and the share of a key's side it grows to (in tenths). */
#define LAYOUT_FLICK_MIN	16
#define LAYOUT_FLICK_TENTHS	3

/*
 * One key of the QWERTY panel that types a letter (its lower and upper
 * case) or a symbol (the same with Shift), a key wide; and one row of the
 * panel's table from its array of keys.
 */
#define LAYOUT_LETTER(lower, upper)	{ lower, upper, lower, upper, KWL_FLICK_TYPE, 0U, 4U }
#define LAYOUT_SYMBOL(symbol)		{ symbol, symbol, symbol, symbol, KWL_FLICK_TYPE, 0U, 4U }
#define LAYOUT_ROW(keys)		{ keys, sizeof(keys) / sizeof(keys[0]) }

/*
 * One row of the QWERTY panel: its keys and how many.  The faces' rows
 * are [face][row].
 */
struct layout_qwerty_row {
	const struct kwl_qwerty_key *keys;
	unsigned count;
};

/*
 * The keys of every face, row by row, column by column: [face][row][column].
 * The texts are centre, left, up, right, down.
 */
static const struct kwl_flick_key layout_keys[KWL_FLICK_FACES][KWL_FLICK_ROWS][KWL_FLICK_COLUMNS] = {
	{
		{
			{ "あ", KWL_FLICK_TYPE, { "あ", "い", "う", "え", "お" } },
			{ "か", KWL_FLICK_TYPE, { "か", "き", "く", "け", "こ" } },
			{ "さ", KWL_FLICK_TYPE, { "さ", "し", "す", "せ", "そ" } },
			{ "Del", KWL_FLICK_BACKSPACE, { NULL, NULL, NULL, NULL, NULL } }
		},
		{
			{ "た", KWL_FLICK_TYPE, { "た", "ち", "つ", "て", "と" } },
			{ "な", KWL_FLICK_TYPE, { "な", "に", "ぬ", "ね", "の" } },
			{ "は", KWL_FLICK_TYPE, { "は", "ひ", "ふ", "へ", "ほ" } },
			{ "空白", KWL_FLICK_SPACE, { " ", NULL, NULL, NULL, NULL } }
		},
		{
			{ "ま", KWL_FLICK_TYPE, { "ま", "み", "む", "め", "も" } },
			{ "や", KWL_FLICK_TYPE, { "や", "（", "ゆ", "）", "よ" } },
			{ "ら", KWL_FLICK_TYPE, { "ら", "り", "る", "れ", "ろ" } },
			{ "改行", KWL_FLICK_ENTER, { "\n", NULL, NULL, NULL, NULL } }
		},
		{
			{ "゛゜小", KWL_FLICK_VOICE, { NULL, NULL, NULL, NULL, NULL } },
			{ "わ", KWL_FLICK_TYPE, { "わ", "を", "ん", "ー", "〜" } },
			{ "、", KWL_FLICK_TYPE, { "、", "。", "？", "！", "…" } },
			{ "A", KWL_FLICK_FACE, { NULL, NULL, NULL, NULL, NULL } }
		}
	},
	{
		{
			{ "@", KWL_FLICK_TYPE, { "@", "#", "/", "&", "_" } },
			{ "abc", KWL_FLICK_TYPE, { "a", "b", "c", NULL, NULL } },
			{ "def", KWL_FLICK_TYPE, { "d", "e", "f", NULL, NULL } },
			{ "Del", KWL_FLICK_BACKSPACE, { NULL, NULL, NULL, NULL, NULL } }
		},
		{
			{ "ghi", KWL_FLICK_TYPE, { "g", "h", "i", NULL, NULL } },
			{ "jkl", KWL_FLICK_TYPE, { "j", "k", "l", NULL, NULL } },
			{ "mno", KWL_FLICK_TYPE, { "m", "n", "o", NULL, NULL } },
			{ "space", KWL_FLICK_SPACE, { " ", NULL, NULL, NULL, NULL } }
		},
		{
			{ "pqrs", KWL_FLICK_TYPE, { "p", "q", "r", "s", NULL } },
			{ "tuv", KWL_FLICK_TYPE, { "t", "u", "v", NULL, NULL } },
			{ "wxyz", KWL_FLICK_TYPE, { "w", "x", "y", "z", NULL } },
			{ "Enter", KWL_FLICK_ENTER, { "\n", NULL, NULL, NULL, NULL } }
		},
		{
			{ "a/A", KWL_FLICK_CASE, { NULL, NULL, NULL, NULL, NULL } },
			{ "'\"()", KWL_FLICK_TYPE, { "'", "\"", "(", ")", ":" } },
			{ ".,?!", KWL_FLICK_TYPE, { ".", ",", "?", "!", "-" } },
			{ "1", KWL_FLICK_FACE, { NULL, NULL, NULL, NULL, NULL } }
		}
	},
	{
		{
			{ "1", KWL_FLICK_TYPE, { "1", "+", "-", "*", "/" } },
			{ "2", KWL_FLICK_TYPE, { "2", "=", "%", "<", ">" } },
			{ "3", KWL_FLICK_TYPE, { "3", "[", "]", "{", "}" } },
			{ "Del", KWL_FLICK_BACKSPACE, { NULL, NULL, NULL, NULL, NULL } }
		},
		{
			{ "4", KWL_FLICK_TYPE, { "4", "$", "^", "~", "\\" } },
			{ "5", KWL_FLICK_TYPE, { "5", "|", ";", "`", ":" } },
			{ "6", KWL_FLICK_TYPE, { "6", "(", ")", "'", "\"" } },
			{ "space", KWL_FLICK_SPACE, { " ", NULL, NULL, NULL, NULL } }
		},
		{
			{ "7", KWL_FLICK_TYPE, { "7", "!", "?", "@", "#" } },
			{ "8", KWL_FLICK_TYPE, { "8", "&", "_", ".", "," } },
			{ "9", KWL_FLICK_TYPE, { "9", NULL, NULL, NULL, NULL } },
			{ "Enter", KWL_FLICK_ENTER, { "\n", NULL, NULL, NULL, NULL } }
		},
		{
			{ "「」", KWL_FLICK_TYPE, { "「", "」", "・", "。", "、" } },
			{ "0", KWL_FLICK_TYPE, { "0", NULL, NULL, NULL, NULL } },
			{ "¥€°", KWL_FLICK_TYPE, { "¥", "€", "°", "±", "×" } },
			{ "あ", KWL_FLICK_FACE, { NULL, NULL, NULL, NULL, NULL } }
		}
	}
};

/*
 * The QWERTY panel's keys, face by face, row by row (ws102-p006; the extra
 * keys' row on top of both faces, p020): label,
 * label with Shift, text, text with Shift, action, arrow code, width in
 * quarter keys.  The rows are LAYOUT_QWERTY_* long; a row narrower than
 * KWL_QWERTY_ROW_UNITS is centred.
 */

static const struct kwl_qwerty_key layout_extra[] = {
	{ "Esc", "Esc", NULL, NULL, KWL_FLICK_ARROW, KWL_KEY_ESC, 4U },
	{ "Tab", "Tab", NULL, NULL, KWL_FLICK_ARROW, KWL_KEY_TAB, 4U },
	{ "Ctrl", "Ctrl", NULL, NULL, KWL_FLICK_CTRL, 0U, 4U },
	{ "Alt", "Alt", NULL, NULL, KWL_FLICK_ALT, 0U, 4U },
	{ "|", "|", "|", "|", KWL_FLICK_TYPE, 0U, 3U },
	{ "~", "~", "~", "~", KWL_FLICK_TYPE, 0U, 3U },
	{ "/", "/", "/", "/", KWL_FLICK_TYPE, 0U, 3U },
	{ "-", "-", "-", "-", KWL_FLICK_TYPE, 0U, 3U },
	{ "Home", "Home", NULL, NULL, KWL_FLICK_ARROW, KWL_KEY_HOME, 3U },
	{ "End", "End", NULL, NULL, KWL_FLICK_ARROW, KWL_KEY_END, 3U },
	{ "PgUp", "PgUp", NULL, NULL, KWL_FLICK_ARROW, KWL_KEY_PAGE_UP, 3U },
	{ "PgDn", "PgDn", NULL, NULL, KWL_FLICK_ARROW, KWL_KEY_PAGE_DOWN, 3U }
};

static const struct kwl_qwerty_key layout_digits[] = {
	{ "1", "!", "1", "!", KWL_FLICK_TYPE, 0U, 4U },
	{ "2", "@", "2", "@", KWL_FLICK_TYPE, 0U, 4U },
	{ "3", "#", "3", "#", KWL_FLICK_TYPE, 0U, 4U },
	{ "4", "$", "4", "$", KWL_FLICK_TYPE, 0U, 4U },
	{ "5", "%", "5", "%", KWL_FLICK_TYPE, 0U, 4U },
	{ "6", "^", "6", "^", KWL_FLICK_TYPE, 0U, 4U },
	{ "7", "&", "7", "&", KWL_FLICK_TYPE, 0U, 4U },
	{ "8", "*", "8", "*", KWL_FLICK_TYPE, 0U, 4U },
	{ "9", "(", "9", "(", KWL_FLICK_TYPE, 0U, 4U },
	{ "0", ")", "0", ")", KWL_FLICK_TYPE, 0U, 4U }
};

static const struct kwl_qwerty_key layout_letters_top[] = {
	LAYOUT_LETTER("q", "Q"), LAYOUT_LETTER("w", "W"), LAYOUT_LETTER("e", "E"), LAYOUT_LETTER("r", "R"), LAYOUT_LETTER("t", "T"),
	LAYOUT_LETTER("y", "Y"), LAYOUT_LETTER("u", "U"), LAYOUT_LETTER("i", "I"), LAYOUT_LETTER("o", "O"), LAYOUT_LETTER("p", "P")
};

static const struct kwl_qwerty_key layout_letters_middle[] = {
	LAYOUT_LETTER("a", "A"), LAYOUT_LETTER("s", "S"), LAYOUT_LETTER("d", "D"), LAYOUT_LETTER("f", "F"), LAYOUT_LETTER("g", "G"),
	LAYOUT_LETTER("h", "H"), LAYOUT_LETTER("j", "J"), LAYOUT_LETTER("k", "K"), LAYOUT_LETTER("l", "L")
};

static const struct kwl_qwerty_key layout_letters_bottom[] = {
	{ "Shift", "Shift", NULL, NULL, KWL_FLICK_SHIFT, 0U, 6U },
	LAYOUT_LETTER("z", "Z"), LAYOUT_LETTER("x", "X"), LAYOUT_LETTER("c", "C"), LAYOUT_LETTER("v", "V"),
	LAYOUT_LETTER("b", "B"), LAYOUT_LETTER("n", "N"), LAYOUT_LETTER("m", "M"),
	{ "Del", "Del", NULL, NULL, KWL_FLICK_BACKSPACE, 0U, 6U }
};

static const struct kwl_qwerty_key layout_letters_space[] = {
	{ "?123", "?123", NULL, NULL, KWL_FLICK_FACE, 0U, 5U },
	{ ",", ",", ",", ",", KWL_FLICK_TYPE, 0U, 3U },
	{ "space", "space", " ", " ", KWL_FLICK_SPACE, 0U, 12U },
	{ ".", ".", ".", ".", KWL_FLICK_TYPE, 0U, 3U },
	{ "Enter", "Enter", "\n", "\n", KWL_FLICK_ENTER, 0U, 5U },
	{ "←", "←", NULL, NULL, KWL_FLICK_ARROW, KWL_KEY_LEFT, 3U },
	{ "↑", "↑", NULL, NULL, KWL_FLICK_ARROW, KWL_KEY_UP, 3U },
	{ "↓", "↓", NULL, NULL, KWL_FLICK_ARROW, KWL_KEY_DOWN, 3U },
	{ "→", "→", NULL, NULL, KWL_FLICK_ARROW, KWL_KEY_RIGHT, 3U }
};

static const struct kwl_qwerty_key layout_symbols_top[] = {
	LAYOUT_SYMBOL("-"), LAYOUT_SYMBOL("/"), LAYOUT_SYMBOL(":"), LAYOUT_SYMBOL(";"), LAYOUT_SYMBOL("<"),
	LAYOUT_SYMBOL(">"), LAYOUT_SYMBOL("["), LAYOUT_SYMBOL("]"), LAYOUT_SYMBOL("{"), LAYOUT_SYMBOL("}")
};

static const struct kwl_qwerty_key layout_symbols_middle[] = {
	LAYOUT_SYMBOL("."), LAYOUT_SYMBOL(","), LAYOUT_SYMBOL("?"), LAYOUT_SYMBOL("!"), LAYOUT_SYMBOL("'"),
	LAYOUT_SYMBOL("\""), LAYOUT_SYMBOL("`"), LAYOUT_SYMBOL("_"), LAYOUT_SYMBOL("\\"), LAYOUT_SYMBOL("|")
};

static const struct kwl_qwerty_key layout_symbols_bottom[] = {
	LAYOUT_SYMBOL("~"), LAYOUT_SYMBOL("+"), LAYOUT_SYMBOL("="), LAYOUT_SYMBOL("*"),
	LAYOUT_SYMBOL("#"), LAYOUT_SYMBOL("%"), LAYOUT_SYMBOL("^"), LAYOUT_SYMBOL("&"),
	{ "Del", "Del", NULL, NULL, KWL_FLICK_BACKSPACE, 0U, 8U }
};

static const struct kwl_qwerty_key layout_symbols_space[] = {
	{ "ABC", "ABC", NULL, NULL, KWL_FLICK_FACE, 0U, 5U },
	{ ",", ",", ",", ",", KWL_FLICK_TYPE, 0U, 3U },
	{ "space", "space", " ", " ", KWL_FLICK_SPACE, 0U, 12U },
	{ ".", ".", ".", ".", KWL_FLICK_TYPE, 0U, 3U },
	{ "Enter", "Enter", "\n", "\n", KWL_FLICK_ENTER, 0U, 5U },
	{ "←", "←", NULL, NULL, KWL_FLICK_ARROW, KWL_KEY_LEFT, 3U },
	{ "↑", "↑", NULL, NULL, KWL_FLICK_ARROW, KWL_KEY_UP, 3U },
	{ "↓", "↓", NULL, NULL, KWL_FLICK_ARROW, KWL_KEY_DOWN, 3U },
	{ "→", "→", NULL, NULL, KWL_FLICK_ARROW, KWL_KEY_RIGHT, 3U }
};

static const struct layout_qwerty_row layout_qwerty[KWL_QWERTY_FACES][KWL_QWERTY_ROWS] = {
	{
		LAYOUT_ROW(layout_extra),
		LAYOUT_ROW(layout_digits),
		LAYOUT_ROW(layout_letters_top),
		LAYOUT_ROW(layout_letters_middle),
		LAYOUT_ROW(layout_letters_bottom),
		LAYOUT_ROW(layout_letters_space)
	},
	{
		LAYOUT_ROW(layout_extra),
		LAYOUT_ROW(layout_digits),
		LAYOUT_ROW(layout_symbols_top),
		LAYOUT_ROW(layout_symbols_middle),
		LAYOUT_ROW(layout_symbols_bottom),
		LAYOUT_ROW(layout_symbols_space)
	}
};

/* The QWERTY panel's faces' names, for the log. */
static const char *const layout_qwerty_names[KWL_QWERTY_FACES] = {
	"letters",
	"symbols"
};

/* The faces' names, for the log and the title band. */
static const char *const layout_face_names[KWL_FLICK_FACES] = {
	"kana",
	"alpha",
	"number"
};

/* The directions' names, for the log. */
static const char *const layout_direction_names[KWL_FLICK_DIRECTIONS] = {
	"center",
	"left",
	"up",
	"right",
	"down"
};

/*
 * The kana that have voiced, half-voiced or small forms, each cycle a
 * string of its forms in the order the voice key goes through them (the
 * last goes back to the first).
 */
static const char *const layout_voice_cycles[] = {
	"あぁ", "いぃ", "うぅ", "えぇ", "おぉ",
	"かが", "きぎ", "くぐ", "けげ", "こご",
	"さざ", "しじ", "すず", "せぜ", "そぞ",
	"ただ", "ちぢ", "つっづ", "てで", "とど",
	"はばぱ", "ひびぴ", "ふぶぷ", "へべぺ", "ほぼぽ",
	"やゃ", "ゆゅ", "よょ", "わゎ"
};

/*
 * The keys of the US layout (the compositor's keymap, keymap.c) by evdev code,
 * as the characters they type without and with Shift: the code of a
 * character is its place in one of the two strings, 0 where a code types
 * none.  The codes run from 0 to 57 (the space bar).
 */
static const char layout_us_plain[] =
	"\0\0" "1234567890-=" "\0\0" "qwertyuiop[]" "\0\0" "asdfghjkl;'`" "\0\\" "zxcvbnm,./" "\0\0\0 ";
static const char layout_us_shifted[] =
	"\0\0" "!@#$%^&*()_+" "\0\0" "QWERTYUIOP{}" "\0\0" "ASDFGHJKL:\"~" "\0|" "ZXCVBNM<>?" "\0\0\0 ";

/* The bytes of one kana in UTF-8 (all of them are three). */
#define LAYOUT_KANA_BYTES	3U

/*
 * The emoji face's emoji (ws102-p022), category by category: each a single
 * code point that is shown as an emoji by default (no variation selector,
 * no joined sequence), and each has a colour glyph in Noto Color Emoji
 * 2.047 (the host's test checks both).
 */
static const char *const layout_emoji[KWL_EMOJI_CATEGORIES][KWL_EMOJI_PER_CATEGORY] = {
	{
		"\xf0\x9f\x98\x80", "\xf0\x9f\x98\x83", "\xf0\x9f\x98\x84", "\xf0\x9f\x98\x81", "\xf0\x9f\x98\x86",
		"\xf0\x9f\x98\x85", "\xf0\x9f\x98\x82", "\xf0\x9f\x99\x82", "\xf0\x9f\x98\x89", "\xf0\x9f\x98\x8a",
		"\xf0\x9f\x98\x87", "\xf0\x9f\x98\x8d", "\xf0\x9f\x98\x98", "\xf0\x9f\x98\x8b", "\xf0\x9f\x98\x8e",
		"\xf0\x9f\xa4\x94", "\xf0\x9f\x98\x90", "\xf0\x9f\x98\xb4", "\xf0\x9f\x98\xa2", "\xf0\x9f\x98\xad"
	},
	{
		"\xf0\x9f\x91\x8d", "\xf0\x9f\x91\x8e", "\xf0\x9f\x91\x8f", "\xf0\x9f\x99\x8c", "\xf0\x9f\x91\x8b",
		"\xf0\x9f\x99\x8f", "\xf0\x9f\x92\xaa", "\xf0\x9f\x91\x8c", "\xf0\x9f\xa4\x9d", "\xe2\x9c\x8b",
		"\xf0\x9f\x91\x89", "\xf0\x9f\x91\x86", "\xf0\x9f\x91\x80", "\xf0\x9f\x91\xb6", "\xf0\x9f\x91\xa6",
		"\xf0\x9f\x91\xa7", "\xf0\x9f\x91\xa8", "\xf0\x9f\x91\xa9", "\xf0\x9f\x91\xb4", "\xf0\x9f\x91\xb5"
	},
	{
		"\xf0\x9f\x93\xb1", "\xf0\x9f\x92\xbb", "\xf0\x9f\x93\xb7", "\xf0\x9f\x93\x9a", "\xf0\x9f\x93\x9d",
		"\xf0\x9f\x93\x8e", "\xf0\x9f\x94\x91", "\xf0\x9f\x8e\x81", "\xe2\x98\x95", "\xf0\x9f\x8d\xa3",
		"\xf0\x9f\x8d\x99", "\xf0\x9f\x8d\x9c", "\xf0\x9f\x8d\xb0", "\xf0\x9f\x8d\xba", "\xf0\x9f\x9a\x97",
		"\xf0\x9f\x9a\x83", "\xf0\x9f\x8f\xa0", "\xf0\x9f\x8c\xb8", "\xf0\x9f\x8d\x81", "\xe2\x9a\xbd"
	},
	{
		"\xf0\x9f\x92\xaf", "\xe2\x9c\xa8", "\xe2\xad\x90", "\xf0\x9f\x94\xa5", "\xf0\x9f\x92\xa1",
		"\xe2\x9a\xa1", "\xe2\x9c\x85", "\xe2\x9d\x8c", "\xe2\x9d\x93", "\xe2\x9d\x97",
		"\xe2\xad\x95", "\xf0\x9f\x92\xa4", "\xf0\x9f\x92\xa2", "\xf0\x9f\x92\xac", "\xf0\x9f\x8e\x89",
		"\xf0\x9f\x94\x94", "\xf0\x9f\x86\x97", "\xf0\x9f\x86\x95", "\xe2\x8c\x9b", "\xf0\x9f\x8c\x88"
	}
};

/* The emoji face's categories' names, shown on their tabs. */
static const char *const layout_emoji_names[KWL_EMOJI_CATEGORIES] = {
	"顔",
	"手と人",
	"物",
	"記号"
};

/*
 * Returns a key of a face by its row and column; NULL outside the grid.
 */
const struct kwl_flick_key *
kwl_flick_key(
	unsigned face,
	unsigned row,
	unsigned column)
{
	/* Only the faces and the grid. */
	if (face >= KWL_FLICK_FACES)
		return NULL;
	if (row >= KWL_FLICK_ROWS || column >= KWL_FLICK_COLUMNS)
		return NULL;

	/* The key. */
	return &layout_keys[face][row][column];
}

/*
 * Returns a face's name (kana, alpha, number); "?" for none.
 */
const char *
kwl_flick_face_name(
	unsigned face)
{
	/* Only the faces. */
	if (face >= KWL_FLICK_FACES)
		return "?";

	/* The name. */
	return layout_face_names[face];
}

/*
 * Returns the face that follows one (kana, alpha, number, then kana again).
 */
unsigned
kwl_flick_face_next(
	unsigned face)
{
	/* The next, wrapping. */
	return (face + 1U) % KWL_FLICK_FACES;
}

/*
 * Works out the direction of a movement from a key's press (dx right,
 * dy down, in pixels) on a key of a side: the centre when it is shorter
 * than the flick's distance, otherwise the nearer axis's direction.
 */
unsigned
kwl_flick_direction(
	int dx,
	int dy,
	int key_size)
{
	long distance;
	long threshold;
	int across;
	int down;

	/* The flick's distance: a share of the key, at least the minimum. */
	threshold = (long)key_size * LAYOUT_FLICK_TENTHS / 10L;
	if (threshold < LAYOUT_FLICK_MIN)
		threshold = LAYOUT_FLICK_MIN;

	/* Shorter than that: a tap. */
	distance = (long)dx * dx + (long)dy * dy;
	if (distance < threshold * threshold)
		return KWL_FLICK_CENTER;

	/* The nearer axis (a tie goes across). */
	across = dx;
	if (across < 0)
		across = -across;
	down = dy;
	if (down < 0)
		down = -down;

	/* Across: left or right. */
	if (across >= down) {
		if (dx < 0)
			return KWL_FLICK_LEFT;
		return KWL_FLICK_RIGHT;
	}

	/* Up or down (the screen's y grows down). */
	if (dy < 0)
		return KWL_FLICK_UP;
	return KWL_FLICK_DOWN;
}

/*
 * Returns what a key types in a direction; NULL where it types nothing
 * (a direction without a character, or a key that only acts).
 */
const char *
kwl_flick_text(
	const struct kwl_flick_key *key,
	unsigned direction)
{
	/* Only a key and a direction. */
	if (key == NULL || direction >= KWL_FLICK_DIRECTIONS)
		return NULL;

	/* The character, or none. */
	return key->text[direction];
}

/*
 * Works out the next form of a kana for the voice key: か to が and back,
 * は to ば to ぱ and back, つ to っ to づ and back.  Returns 1 with the
 * next form (UTF-8) in next, or 0 when the text is not such a kana.
 */
int
kwl_flick_voice(
	const char *previous,
	char *next,
	size_t size)
{
	const char *cycle;
	const char *found;
	size_t cycle_length;
	size_t length;
	size_t index;
	size_t offset;

	/* Only one kana (three bytes), and room for one. */
	length = strlen(previous);
	if (length != LAYOUT_KANA_BYTES || size <= LAYOUT_KANA_BYTES)
		return 0;

	/* The cycle that has it, at a kana's boundary. */
	for (index = 0; index < sizeof(layout_voice_cycles) / sizeof(layout_voice_cycles[0]); index++) {
		/* The kana in this cycle, if it is there. */
		cycle = layout_voice_cycles[index];
		found = strstr(cycle, previous);
		if (found == NULL)
			continue;

		/* Only at a kana's boundary (not across two). */
		offset = (size_t)(found - cycle);
		if (offset % LAYOUT_KANA_BYTES != 0U)
			continue;

		/* The form after it, or the first after the last. */
		offset += LAYOUT_KANA_BYTES;
		cycle_length = strlen(cycle);
		if (offset >= cycle_length)
			offset = 0;
		memcpy(next, cycle + offset, LAYOUT_KANA_BYTES);
		next[LAYOUT_KANA_BYTES] = '\0';
		return 1;
	}

	/* Not a kana with other forms. */
	return 0;
}

/*
 * Works out the other case of a letter for the case key.  Returns 1 with
 * it in next, or 0 when the text is not one ASCII letter.
 */
int
kwl_flick_case(
	const char *previous,
	char *next,
	size_t size)
{
	char letter;

	/* Only one character, and room for it. */
	if (previous[0] == '\0' || previous[1] != '\0' || size < 2U)
		return 0;
	letter = previous[0];

	/* A lower-case letter becomes upper-case. */
	if (letter >= 'a' && letter <= 'z') {
		next[0] = (char)(letter - 'a' + 'A');
		next[1] = '\0';
		return 1;
	}

	/* An upper-case letter becomes lower-case. */
	if (letter >= 'A' && letter <= 'Z') {
		next[0] = (char)(letter - 'A' + 'a');
		next[1] = '\0';
		return 1;
	}

	/* Not a letter. */
	return 0;
}

/*
 * Returns a direction's name (center, left, up, right, down); "?" for none.
 */
const char *
kwl_flick_direction_name(
	unsigned direction)
{
	/* Only the directions. */
	if (direction >= KWL_FLICK_DIRECTIONS)
		return "?";

	/* The name. */
	return layout_direction_names[direction];
}

/*
 * Finds the key of the US layout that types a text of one ASCII character
 * (a newline is the enter key).  Returns 1 with its evdev code and whether
 * Shift must be held, or 0 when no key types it (any other text).
 */
int
kwl_flick_us_key(
	const char *text,
	unsigned *code,
	int *shift)
{
	unsigned index;
	char character;

	/* Only one character. */
	character = text[0];
	if (character == '\0' || text[1] != '\0')
		return 0;

	/* A newline is the enter key. */
	if (character == '\n') {
		*code = KWL_FLICK_KEY_ENTER;
		*shift = 0;
		return 1;
	}

	/* The code whose key types it, without Shift or with it. */
	for (index = 0; index < sizeof(layout_us_plain) - 1U; index++) {
		/* Without Shift. */
		if (layout_us_plain[index] == character) {
			*code = index;
			*shift = 0;
			return 1;
		}

		/* With Shift (the space bar types a space either way, found above). */
		if (layout_us_shifted[index] == character) {
			*code = index;
			*shift = 1;
			return 1;
		}
	}

	/* No key types it. */
	return 0;
}

/*
 * Returns a row of a QWERTY face and how many keys it has; NULL outside
 * the faces and rows.
 */
const struct kwl_qwerty_key *
kwl_qwerty_row(
	unsigned face,
	unsigned row,
	unsigned *count)
{
	/* Only the faces and their rows. */
	*count = 0;
	if (face >= KWL_QWERTY_FACES || row >= KWL_QWERTY_ROWS)
		return NULL;

	/* The row's keys. */
	*count = layout_qwerty[face][row].count;
	return layout_qwerty[face][row].keys;
}

/*
 * Returns a QWERTY face's name (letters, symbols); "?" for none.
 */
const char *
kwl_qwerty_face_name(
	unsigned face)
{
	/* Only the faces. */
	if (face >= KWL_QWERTY_FACES)
		return "?";

	/* The name. */
	return layout_qwerty_names[face];
}

/*
 * Returns how many emoji a category of the emoji face holds (0 for no
 * such category).
 */
unsigned
kwl_emoji_count(
	unsigned category)
{
	/* Only the categories. */
	if (category >= KWL_EMOJI_CATEGORIES)
		return 0U;

	/* Every category is full. */
	return KWL_EMOJI_PER_CATEGORY;
}

/*
 * Returns an emoji of a category (UTF-8), or NULL for a place that holds
 * none.
 */
const char *
kwl_emoji(
	unsigned category,
	unsigned index)
{
	/* Only the categories and their places. */
	if (category >= KWL_EMOJI_CATEGORIES || index >= KWL_EMOJI_PER_CATEGORY)
		return NULL;

	/* The emoji. */
	return layout_emoji[category][index];
}

/*
 * Returns a category's name for its tab; "?" for none.
 */
const char *
kwl_emoji_category_name(
	unsigned category)
{
	/* Only the categories. */
	if (category >= KWL_EMOJI_CATEGORIES)
		return "?";

	/* The name. */
	return layout_emoji_names[category];
}
