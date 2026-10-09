/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PIN's keypad of the login screen and the lock screen (lock-keypad.c,
 * ws199-p001 section 3.8): drawn just under the field while it takes a
 * PIN, so a machine without a keyboard (a tablet) can type one.  The
 * digits 3 by 4, and for a security key's PIN "ABC" turns it into the
 * letters (with Shift); the Software Security Key's six digits have the
 * digits alone.  A physical keyboard types as well.
 *
 * It knows nothing of the server or the fonts: the caller hands it the
 * place and a press, and draws and types what it says (greeter.c).  So
 * the host tests run it alone.  Every size is in logical pixels.
 */

#ifndef KWL_LOCK_KEYPAD_H
#define KWL_LOCK_KEYPAD_H

#include <stddef.h>
#include <stdint.h>

/* A key's height, the gap between keys, the rows, and the most keys a layout has. */
#define KWL_KEYPAD_KEY		40
#define KWL_KEYPAD_GAP		6
#define KWL_KEYPAD_ROWS		4
#define KWL_KEYPAD_KEYS		32U

/* What a key does. */
enum kwl_keypad_action {
	KWL_KEYPAD_NONE,
	KWL_KEYPAD_CHARACTER,
	KWL_KEYPAD_BACKSPACE,
	KWL_KEYPAD_ENTER,
	KWL_KEYPAD_LETTERS,
	KWL_KEYPAD_DIGITS,
	KWL_KEYPAD_SHIFT
};

/* The keypad's state: the digits alone (a six-digit PIN), the letters shown, and the capitals. */
struct kwl_keypad {
	unsigned digits_only;
	unsigned letters;
	unsigned upper;
};

/* One key laid out: where it is (x, y, width, height), what it does, its character and its label. */
struct kwl_keypad_key {
	int32_t rect[4];
	enum kwl_keypad_action action;
	char character;
	char label[8];
};

void kwl_keypad_reset(struct kwl_keypad *pad, unsigned digits_only);
int32_t kwl_keypad_height(void);
size_t kwl_keypad_layout(const struct kwl_keypad *pad, int32_t x, int32_t y, int32_t width, struct kwl_keypad_key *keys, size_t capacity);
int kwl_keypad_hit(const struct kwl_keypad_key *keys, size_t count, int32_t x, int32_t y);
enum kwl_keypad_action kwl_keypad_press(struct kwl_keypad *pad, const struct kwl_keypad_key *key, char *character);

#endif
