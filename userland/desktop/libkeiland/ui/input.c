/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The characters of the keys (ws090-p003): evdev key codes to the
 * characters they type.  The compositor forwards evdev codes with no keymap, so
 * the library carries the US layout, the one Files, Text Editor,
 * Terminal, PDF Viewer and the file chooser each carried a copy of.
 */

#include <keiland/keiland.h>

/* How many codes the character tables cover (up to the space bar). */
#define INPUT_KEYS		58U

/*
 * The character each key types without shift, by evdev code; 0 for a key
 * that types none.  The US layout.
 */
static const char input_plain[INPUT_KEYS] = {
	0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 0, 0,
	'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', 0, 0,
	'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\',
	'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' '
};

/* The character each key types with shift, by evdev code. */
static const char input_shifted[INPUT_KEYS] = {
	0, 0, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 0, 0,
	'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', 0, 0,
	'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|',
	'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' '
};

/*
 * Reports the character a key types with the modifiers held (KL_MOD_*),
 * or 0 for a key that types none (a key with Control, Alt or Super types
 * none either).
 */
uint32_t
kl_key_character(
	uint32_t key,
	unsigned modifiers)
{
	char character;

	/* Control, Alt and Super make a key a command, not a character. */
	if ((modifiers & (KL_MOD_CTRL | KL_MOD_ALT | KL_MOD_SUPER)) != 0U)
		return 0U;

	/* Only the keys of the tables type characters. */
	if (key >= INPUT_KEYS)
		return 0U;

	/* The character, shifted or not. */
	character = input_plain[key];
	if ((modifiers & KL_MOD_SHIFT) != 0U)
		character = input_shifted[key];

	/* Succeeded: the character, or 0. */
	return (uint32_t)(unsigned char)character;
}
