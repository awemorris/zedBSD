/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The keys the input method hears: evdev codes to the characters of the US
 * layout, and the seat's modifier bits to the engines' (plan/ws095/design.md
 * section 5).  The table is Terminal's (terminal/keys.c), as the compositor's
 * keymap is the US one.
 */

#include "program.h"

/* How many codes the character tables cover (up to the space bar). */
#define KEYS_TABLE_SIZE		58U

/* The seat's modifier bits (the compositor's input.c: shift, control, alt, meta). */
#define KEYS_SEAT_SHIFT		0x01U
#define KEYS_SEAT_CONTROL	0x04U
#define KEYS_SEAT_ALT		0x08U
#define KEYS_SEAT_META		0x40U

/*
 * The character each key types without shift, by evdev code; 0 for a key
 * that types none.
 */
static const char keys_plain[KEYS_TABLE_SIZE] = {
	0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 0, 0,
	'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', 0, 0,
	'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\',
	'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' '
};

/* The character each key types with shift, by evdev code. */
static const char keys_shifted[KEYS_TABLE_SIZE] = {
	0, 0, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 0, 0,
	'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', 0, 0,
	'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|',
	'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' '
};

/*
 * Gives the character a key types with the seat's modifiers held, or 0.
 */
uint32_t
program_key_character(
	uint32_t code,
	uint32_t modifiers)
{
	/* Keys past the tables type no character. */
	if (code >= KEYS_TABLE_SIZE)
		return 0;

	/* Shift chooses the table. */
	if ((modifiers & KEYS_SEAT_SHIFT) != 0U)
		return (unsigned char)keys_shifted[code];

	/* The plain character. */
	return (unsigned char)keys_plain[code];
}

/*
 * Gives the engines' modifier bits (engine.h) for the seat's.
 */
uint32_t
program_key_modifiers(
	uint32_t modifiers)
{
	uint32_t engine_modifiers;

	/* Each held modifier in turn. */
	engine_modifiers = 0;
	if ((modifiers & KEYS_SEAT_SHIFT) != 0U)
		engine_modifiers |= IME_MOD_SHIFT;
	if ((modifiers & KEYS_SEAT_CONTROL) != 0U)
		engine_modifiers |= IME_MOD_CTRL;
	if ((modifiers & KEYS_SEAT_ALT) != 0U)
		engine_modifiers |= IME_MOD_ALT;
	if ((modifiers & KEYS_SEAT_META) != 0U)
		engine_modifiers |= IME_MOD_SUPER;

	/* The engines' bits. */
	return engine_modifiers;
}
