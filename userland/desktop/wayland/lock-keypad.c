/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PIN's keypad (lock-keypad.h, ws199-p001 section 3.8).
 *
 * The digits, three keys a row:
 *
 *   1 2 3 / 4 5 6 / 7 8 9 / ABC 0 <-      (a security key's PIN)
 *   1 2 3 / 4 5 6 / 7 8 9 / OK  0 <-      (the six digits of the PIN)
 *
 * The letters, ten key widths a row:
 *
 *   q w e r t y u i o p / a s d f g h j k l / Shift z x c v b n m <- / 123 OK
 *
 * Shift makes the next letter a capital (pressed twice it stays until
 * pressed again); "123" goes back to the digits.
 */

#include "lock-keypad.h"

#include <stdio.h>
#include <string.h>

/* The letters' rows. */
static const char *const keypad_letter_rows[3] = {
	"qwertyuiop",
	"asdfghjkl",
	"zxcvbnm"
};

/* The labels of the keys that are not characters (the arrows from the glyph cache, as Log In's). */
#define KEYPAD_LABEL_BACKSPACE	"\xe2\x86\x90"
#define KEYPAD_LABEL_SHIFT	"\xe2\x86\x91"

static void keypad_key(struct kwl_keypad_key *keys, size_t capacity, size_t *count, int32_t x, int32_t y, int32_t width, enum kwl_keypad_action action, char character, const char *label);
static void keypad_digits(const struct kwl_keypad *pad, int32_t x, int32_t y, int32_t width, struct kwl_keypad_key *keys, size_t capacity, size_t *count);
static void keypad_letters(const struct kwl_keypad *pad, int32_t x, int32_t y, int32_t width, struct kwl_keypad_key *keys, size_t capacity, size_t *count);

/* Starts a keypad on its digits, lower case; digits_only for a six-digit PIN. */
void
kwl_keypad_reset(
	struct kwl_keypad *pad,
	unsigned digits_only)
{
	/* The digits. */
	memset(pad, 0, sizeof(*pad));
	pad->digits_only = digits_only;
}

/* Gives the keypad's height: its rows and the gaps between them. */
int32_t
kwl_keypad_height(void)
{
	/* Four rows. */
	return KWL_KEYPAD_ROWS * KWL_KEYPAD_KEY + (KWL_KEYPAD_ROWS - 1) * KWL_KEYPAD_GAP;
}

/*
 * Lays the keypad out from (x, y), width wide, into keys (at most
 * capacity).  Returns how many keys there are.
 */
size_t
kwl_keypad_layout(
	const struct kwl_keypad *pad,
	int32_t x,
	int32_t y,
	int32_t width,
	struct kwl_keypad_key *keys,
	size_t capacity)
{
	size_t count;

	/* The letters, or the digits. */
	count = 0U;
	if (pad->letters && !pad->digits_only) {
		keypad_letters(pad, x, y, width, keys, capacity, &count);
	} else {
		keypad_digits(pad, x, y, width, keys, capacity, &count);
	}

	/* Succeeded: the keys. */
	return count;
}

/* Gives the index of the key at (x, y), or -1. */
int
kwl_keypad_hit(
	const struct kwl_keypad_key *keys,
	size_t count,
	int32_t x,
	int32_t y)
{
	const int32_t *rect;
	size_t index;

	/* Each key's rectangle. */
	for (index = 0U; index < count; index++) {
		rect = keys[index].rect;
		if (x < rect[0] || y < rect[1])
			continue;
		if (x >= rect[0] + rect[2] || y >= rect[1] + rect[3])
			continue;
		return (int)index;
	}

	/* None. */
	return -1;
}

/*
 * Presses a key: the letters, the digits and Shift change the keypad; a
 * character is given in *character (a capital once, after Shift).
 * Returns what the caller is to do (KWL_KEYPAD_CHARACTER, _BACKSPACE,
 * _ENTER) or what the keypad did.
 */
enum kwl_keypad_action
kwl_keypad_press(
	struct kwl_keypad *pad,
	const struct kwl_keypad_key *key,
	char *character)
{
	/* What the key does. */
	*character = '\0';
	switch (key->action) {
	case KWL_KEYPAD_CHARACTER:
		/* Its character, a capital after Shift (Shift once lasts one letter). */
		*character = key->character;
		if (pad->upper != 0U && key->character >= 'a' && key->character <= 'z')
			*character = (char)(key->character - 'a' + 'A');
		if (pad->upper == 1U)
			pad->upper = 0U;
		break;
	case KWL_KEYPAD_LETTERS:
		/* The letters, lower case. */
		pad->letters = 1U;
		pad->upper = 0U;
		break;
	case KWL_KEYPAD_DIGITS:
		/* The digits. */
		pad->letters = 0U;
		pad->upper = 0U;
		break;
	case KWL_KEYPAD_SHIFT:
		/* Off, once, then locked, then off again. */
		pad->upper = (pad->upper + 1U) % 3U;
		break;
	default:
		break;
	}

	/* Succeeded: what the key does. */
	return key->action;
}

/* Adds one key, while there is room. */
static void
keypad_key(
	struct kwl_keypad_key *keys,
	size_t capacity,
	size_t *count,
	int32_t x,
	int32_t y,
	int32_t width,
	enum kwl_keypad_action action,
	char character,
	const char *label)
{
	struct kwl_keypad_key *key;

	/* No room. */
	if (*count >= capacity)
		return;

	/* The key. */
	key = &keys[*count];
	memset(key, 0, sizeof(*key));
	key->rect[0] = x;
	key->rect[1] = y;
	key->rect[2] = width;
	key->rect[3] = KWL_KEYPAD_KEY;
	key->action = action;
	key->character = character;
	(void)snprintf(key->label, sizeof(key->label), "%s", label);
	(*count)++;
}

/* Lays the digits out: three a row, and the row of ABC (or OK), 0 and Backspace. */
static void
keypad_digits(
	const struct kwl_keypad *pad,
	int32_t x,
	int32_t y,
	int32_t width,
	struct kwl_keypad_key *keys,
	size_t capacity,
	size_t *count)
{
	char label[2];
	int32_t key_width;
	int32_t row_y;
	int32_t column;
	int32_t row;

	/* Three keys a row, the gaps between them. */
	key_width = (width - 2 * KWL_KEYPAD_GAP) / 3;

	/* 1 to 9. */
	for (row = 0; row < 3; row++) {
		row_y = y + row * (KWL_KEYPAD_KEY + KWL_KEYPAD_GAP);
		for (column = 0; column < 3; column++) {
			label[0] = (char)('1' + row * 3 + column);
			label[1] = '\0';
			keypad_key(keys, capacity, count, x + column * (key_width + KWL_KEYPAD_GAP), row_y, key_width, KWL_KEYPAD_CHARACTER,
			    label[0], label);
		}
	}

	/* The last row: the letters (or OK for six digits), 0, Backspace. */
	row_y = y + 3 * (KWL_KEYPAD_KEY + KWL_KEYPAD_GAP);
	if (pad->digits_only) {
		keypad_key(keys, capacity, count, x, row_y, key_width, KWL_KEYPAD_ENTER, '\0', "OK");
	} else {
		keypad_key(keys, capacity, count, x, row_y, key_width, KWL_KEYPAD_LETTERS, '\0', "ABC");
	}

	/* 0 and Backspace. */
	keypad_key(keys, capacity, count, x + key_width + KWL_KEYPAD_GAP, row_y, key_width, KWL_KEYPAD_CHARACTER, '0', "0");
	keypad_key(keys, capacity, count, x + 2 * (key_width + KWL_KEYPAD_GAP), row_y, key_width, KWL_KEYPAD_BACKSPACE, '\0',
	    KEYPAD_LABEL_BACKSPACE);
}

/* Lays the letters out: three rows of letters (Shift and Backspace in the third), then 123 and OK. */
static void
keypad_letters(
	const struct kwl_keypad *pad,
	int32_t x,
	int32_t y,
	int32_t width,
	struct kwl_keypad_key *keys,
	size_t capacity,
	size_t *count)
{
	const char *letters;
	char label[2];
	int32_t unit;
	int32_t key_width;
	int32_t start;
	int32_t row_y;
	int32_t row;
	size_t length;
	size_t index;

	/* Ten key widths a row. */
	unit = (width + KWL_KEYPAD_GAP) / 10;
	key_width = unit - KWL_KEYPAD_GAP;

	/* Each row of letters, centred; the third between Shift and Backspace. */
	for (row = 0; row < 3; row++) {
		letters = keypad_letter_rows[row];
		length = strlen(letters);
		row_y = y + row * (KWL_KEYPAD_KEY + KWL_KEYPAD_GAP);
		start = x + (int32_t)(10U - length) * unit / 2;
		if (row == 2) {
			keypad_key(keys, capacity, count, x, row_y, unit + unit / 2 - KWL_KEYPAD_GAP, KWL_KEYPAD_SHIFT, '\0', KEYPAD_LABEL_SHIFT);
			start = x + unit + unit / 2;
		}

		/* Its letters, a capital's label after Shift. */
		for (index = 0U; index < length; index++) {
			label[0] = letters[index];
			if (pad->upper != 0U)
				label[0] = (char)(letters[index] - 'a' + 'A');
			label[1] = '\0';
			keypad_key(keys, capacity, count, start + (int32_t)index * unit, row_y, key_width, KWL_KEYPAD_CHARACTER, letters[index],
			    label);
		}

		/* Backspace at the third row's end. */
		if (row == 2) {
			keypad_key(keys, capacity, count, start + (int32_t)length * unit, row_y, x + width - (start + (int32_t)length * unit),
			    KWL_KEYPAD_BACKSPACE, '\0', KEYPAD_LABEL_BACKSPACE);
		}
	}

	/* The last row: back to the digits, and OK. */
	row_y = y + 3 * (KWL_KEYPAD_KEY + KWL_KEYPAD_GAP);
	keypad_key(keys, capacity, count, x, row_y, (width - KWL_KEYPAD_GAP) / 2, KWL_KEYPAD_DIGITS, '\0', "123");
	keypad_key(keys, capacity, count, x + (width + KWL_KEYPAD_GAP) / 2, row_y, (width - KWL_KEYPAD_GAP) / 2, KWL_KEYPAD_ENTER, '\0',
	    "OK");
}
