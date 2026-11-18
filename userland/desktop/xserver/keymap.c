/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The keyboard's table: evdev key codes become X keycodes, and keycodes
 * become keysyms.
 *
 * A key that types a character has the keycode of that character plus 8
 * (so its keysym is the Latin-1 character itself); the keys that type
 * nothing (the arrows, Home, End and so on) have keycodes from 0xe0 up.
 * The layout is US.
 */

#include "userland/desktop/xserver/internal.h"

#include "userland/desktop/xserver/keycodes.h"

/* The keysyms of the keys that type nothing. */
#define KEYSYM_HOME		0xff50U
#define KEYSYM_LEFT		0xff51U
#define KEYSYM_UP		0xff52U
#define KEYSYM_RIGHT		0xff53U
#define KEYSYM_DOWN		0xff54U
#define KEYSYM_PAGE_UP		0xff55U
#define KEYSYM_PAGE_DOWN	0xff56U
#define KEYSYM_END		0xff57U
#define KEYSYM_INSERT		0xff63U
#define KEYSYM_DELETE		0xffffU

/* The characters of the control keys. */
#define KEYMAP_ESCAPE		0x1bU
#define KEYMAP_BACKSPACE	0x08U
#define KEYMAP_TAB		0x09U
#define KEYMAP_RETURN		0x0dU

/* The first keycode (the ASCII characters are offset by it) and the last a character can have. */
#define KEYMAP_OFFSET		8U
#define KEYMAP_CHARACTER_MAX	248U

/*
 * One key that types a character: its evdev code, the character without
 * and with shift, and whether caps lock shifts it (a letter).
 */
struct keymap_key {
	uint16_t code;
	uint8_t normal;
	uint8_t shifted;
	uint8_t letter;
};

/*
 * The keys that type characters, US layout.
 */
static const struct keymap_key keymap_keys[] = {
	{ KEY_A, 'a', 'A', 1 }, { KEY_B, 'b', 'B', 1 }, { KEY_C, 'c', 'C', 1 }, { KEY_D, 'd', 'D', 1 },
	{ KEY_E, 'e', 'E', 1 }, { KEY_F, 'f', 'F', 1 }, { KEY_G, 'g', 'G', 1 }, { KEY_H, 'h', 'H', 1 },
	{ KEY_I, 'i', 'I', 1 }, { KEY_J, 'j', 'J', 1 }, { KEY_K, 'k', 'K', 1 }, { KEY_L, 'l', 'L', 1 },
	{ KEY_M, 'm', 'M', 1 }, { KEY_N, 'n', 'N', 1 }, { KEY_O, 'o', 'O', 1 }, { KEY_P, 'p', 'P', 1 },
	{ KEY_Q, 'q', 'Q', 1 }, { KEY_R, 'r', 'R', 1 }, { KEY_S, 's', 'S', 1 }, { KEY_T, 't', 'T', 1 },
	{ KEY_U, 'u', 'U', 1 }, { KEY_V, 'v', 'V', 1 }, { KEY_W, 'w', 'W', 1 }, { KEY_X, 'x', 'X', 1 },
	{ KEY_Y, 'y', 'Y', 1 }, { KEY_Z, 'z', 'Z', 1 },
	{ KEY_0, '0', ')', 0 }, { KEY_1, '1', '!', 0 }, { KEY_2, '2', '@', 0 }, { KEY_3, '3', '#', 0 },
	{ KEY_4, '4', '$', 0 }, { KEY_5, '5', '%', 0 }, { KEY_6, '6', '^', 0 }, { KEY_7, '7', '&', 0 },
	{ KEY_8, '8', '*', 0 }, { KEY_9, '9', '(', 0 },
	{ KEY_SPACE, ' ', ' ', 0 }, { KEY_MINUS, '-', '_', 0 }, { KEY_EQUAL, '=', '+', 0 },
	{ KEY_LEFTBRACE, '[', '{', 0 }, { KEY_RIGHTBRACE, ']', '}', 0 }, { KEY_SEMICOLON, ';', ':', 0 },
	{ KEY_APOSTROPHE, '\'', '"', 0 }, { KEY_GRAVE, '`', '~', 0 }, { KEY_BACKSLASH, '\\', '|', 0 },
	{ KEY_COMMA, ',', '<', 0 }, { KEY_DOT, '.', '>', 0 }, { KEY_SLASH, '/', '?', 0 }
};

static uint8_t keymap_character(uint16_t code, int shifted, int caps_lock);

/*
 * Returns the X keycode of an evdev key in the shift and caps lock states
 * given, or 0 for a key the table does not have.
 */
uint8_t
x11_keymap_keycode(
	uint16_t code,
	int shifted,
	int caps_lock)
{
	uint8_t character;

	/* The keys that type nothing. */
	switch (code) {
	case KEY_UP:
		return (uint8_t)X11_KEYCODE_UP;
	case KEY_DOWN:
		return (uint8_t)X11_KEYCODE_DOWN;
	case KEY_LEFT:
		return (uint8_t)X11_KEYCODE_LEFT;
	case KEY_RIGHT:
		return (uint8_t)X11_KEYCODE_RIGHT;
	case KEY_HOME:
		return (uint8_t)X11_KEYCODE_HOME;
	case KEY_END:
		return (uint8_t)X11_KEYCODE_END;
	case KEY_PAGEUP:
		return (uint8_t)X11_KEYCODE_PAGE_UP;
	case KEY_PAGEDOWN:
		return (uint8_t)X11_KEYCODE_PAGE_DOWN;
	case KEY_INSERT:
		return (uint8_t)X11_KEYCODE_INSERT;
	case KEY_DELETE:
		return (uint8_t)X11_KEYCODE_DELETE;
	default:
		break;
	}

	/* A key that types a character, which it has not when the table lacks it. */
	character = keymap_character(code, shifted, caps_lock);
	if (character == 0U || character >= KEYMAP_CHARACTER_MAX)
		return 0U;

	/* Succeeded: the character's keycode. */
	return (uint8_t)(character + KEYMAP_OFFSET);
}

/*
 * Returns the keysym of an X keycode (0 for none).
 */
uint32_t
x11_keymap_keysym(
	uint32_t keycode)
{
	/* The keys that type nothing have their own keysyms. */
	switch (keycode) {
	case X11_KEYCODE_UP:
		return KEYSYM_UP;
	case X11_KEYCODE_DOWN:
		return KEYSYM_DOWN;
	case X11_KEYCODE_LEFT:
		return KEYSYM_LEFT;
	case X11_KEYCODE_RIGHT:
		return KEYSYM_RIGHT;
	case X11_KEYCODE_HOME:
		return KEYSYM_HOME;
	case X11_KEYCODE_END:
		return KEYSYM_END;
	case X11_KEYCODE_PAGE_UP:
		return KEYSYM_PAGE_UP;
	case X11_KEYCODE_PAGE_DOWN:
		return KEYSYM_PAGE_DOWN;
	case X11_KEYCODE_INSERT:
		return KEYSYM_INSERT;
	case X11_KEYCODE_DELETE:
		return KEYSYM_DELETE;
	default:
		break;
	}

	/* The keycodes below the first have none. */
	if (keycode < KEYMAP_OFFSET)
		return 0U;

	/* Succeeded: a character's keysym is the character. */
	return keycode - KEYMAP_OFFSET;
}

/* Returns the character an evdev key types in the shift and caps lock states given, or 0. */
static uint8_t
keymap_character(
	uint16_t code,
	int shifted,
	int caps_lock)
{
	size_t index;
	int use_shift;

	/* The control keys. */
	switch (code) {
	case KEY_ESC:
		return (uint8_t)KEYMAP_ESCAPE;
	case KEY_BACKSPACE:
		return (uint8_t)KEYMAP_BACKSPACE;
	case KEY_TAB:
		return (uint8_t)KEYMAP_TAB;
	case KEY_ENTER:
		return (uint8_t)KEYMAP_RETURN;
	default:
		break;
	}

	/* The key's row of the table. */
	for (index = 0U; index < sizeof(keymap_keys) / sizeof(keymap_keys[0]); index++) {
		if (keymap_keys[index].code == code)
			break;
	}

	/* A key the table does not have types nothing. */
	if (index == sizeof(keymap_keys) / sizeof(keymap_keys[0]))
		return 0U;

	/* Caps lock shifts a letter, and shift undoes it. */
	use_shift = shifted;
	if (keymap_keys[index].letter && caps_lock)
		use_shift = !shifted;
	if (use_shift)
		return keymap_keys[index].shifted;

	/* Succeeded: the unshifted character. */
	return keymap_keys[index].normal;
}
