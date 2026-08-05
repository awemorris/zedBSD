/*
 * NEC PC-98 keyboard scancode translation
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 *
 * Scancode assignments follow the PC-98 keyboard (linux-pc98
 * drivers/input/keyboard/pc98kbd.c) and agree with the inverse map the
 * loader already carried.  The base/shift tables are the JIS layout; a
 * boot environment needs ASCII and romaji (Remacs SKK), so the kana
 * plane is intentionally absent.
 */

#include "drivers/kbd-pc98-map.h"

#include <noct/beui.h>

#define SCAN_MAX 0x80U

#define SCAN_SHIFT_L 0x70U
#define SCAN_CAPS    0x71U
#define SCAN_KANA    0x72U
#define SCAN_GRAPH   0x73U
#define SCAN_CTRL    0x74U
#define SCAN_SHIFT_R 0x7dU

/* Unshifted characters.  0 means "not a printable key" (handled by the
 * special-key table or ignored). */
static const uint8_t base_table[SCAN_MAX] = {
	[0x01] = '1', [0x02] = '2', [0x03] = '3', [0x04] = '4', [0x05] = '5',
	[0x06] = '6', [0x07] = '7', [0x08] = '8', [0x09] = '9', [0x0a] = '0',
	[0x0b] = '-', [0x0c] = '^', [0x0d] = '\\',
	[0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r', [0x14] = 't',
	[0x15] = 'y', [0x16] = 'u', [0x17] = 'i', [0x18] = 'o', [0x19] = 'p',
	[0x1a] = '@', [0x1b] = '[',
	[0x1d] = 'a', [0x1e] = 's', [0x1f] = 'd', [0x20] = 'f', [0x21] = 'g',
	[0x22] = 'h', [0x23] = 'j', [0x24] = 'k', [0x25] = 'l', [0x26] = ';',
	[0x27] = ':', [0x28] = ']',
	[0x29] = 'z', [0x2a] = 'x', [0x2b] = 'c', [0x2c] = 'v', [0x2d] = 'b',
	[0x2e] = 'n', [0x2f] = 'm', [0x30] = ',', [0x31] = '.', [0x32] = '/',
	[0x33] = '\\', [0x34] = ' ',
};

/* Shifted characters for the same keys. */
static const uint8_t shift_table[SCAN_MAX] = {
	[0x01] = '!', [0x02] = '"', [0x03] = '#', [0x04] = '$', [0x05] = '%',
	[0x06] = '&', [0x07] = '\'', [0x08] = '(', [0x09] = ')', [0x0a] = '0',
	[0x0b] = '=', [0x0c] = '~', [0x0d] = '|',
	[0x10] = 'Q', [0x11] = 'W', [0x12] = 'E', [0x13] = 'R', [0x14] = 'T',
	[0x15] = 'Y', [0x16] = 'U', [0x17] = 'I', [0x18] = 'O', [0x19] = 'P',
	[0x1a] = '`', [0x1b] = '{',
	[0x1d] = 'A', [0x1e] = 'S', [0x1f] = 'D', [0x20] = 'F', [0x21] = 'G',
	[0x22] = 'H', [0x23] = 'J', [0x24] = 'K', [0x25] = 'L', [0x26] = '+',
	[0x27] = '*', [0x28] = '}',
	[0x29] = 'Z', [0x2a] = 'X', [0x2b] = 'C', [0x2c] = 'V', [0x2d] = 'B',
	[0x2e] = 'N', [0x2f] = 'M', [0x30] = '<', [0x31] = '>', [0x32] = '?',
	[0x33] = '_', [0x34] = ' ',
};

static int
special_key(uint8_t scan)
{
	switch (scan) {
	case 0x00: return NOCT_BEUI_KEY_ESCAPE;
	case 0x0e: return NOCT_BEUI_KEY_BACKSPACE;
	case 0x0f: return NOCT_BEUI_KEY_TAB;
	case 0x1c: return NOCT_BEUI_KEY_ENTER;
	case 0x36: return NOCT_BEUI_KEY_PAGE_UP;
	case 0x37: return NOCT_BEUI_KEY_PAGE_DOWN;
	case 0x38: return NOCT_BEUI_KEY_INSERT;
	case 0x39: return NOCT_BEUI_KEY_DELETE;
	case 0x3a: return NOCT_BEUI_KEY_UP;
	case 0x3b: return NOCT_BEUI_KEY_LEFT;
	case 0x3c: return NOCT_BEUI_KEY_RIGHT;
	case 0x3d: return NOCT_BEUI_KEY_DOWN;
	case 0x3e: return NOCT_BEUI_KEY_HOME;
	case 0x3f: return NOCT_BEUI_KEY_END;
	case 0x62: return NOCT_BEUI_KEY_F1;
	case 0x63: return NOCT_BEUI_KEY_F2;
	case 0x64: return NOCT_BEUI_KEY_F3;
	case 0x65: return NOCT_BEUI_KEY_F4;
	case 0x66: return NOCT_BEUI_KEY_F5;
	case 0x67: return NOCT_BEUI_KEY_F6;
	case 0x68: return NOCT_BEUI_KEY_F7;
	case 0x69: return NOCT_BEUI_KEY_F8;
	case 0x6a: return NOCT_BEUI_KEY_F9;
	case 0x6b: return NOCT_BEUI_KEY_F10;
	default: return 0;
	}
}

/* Normalized key -> PC-98 scancode, for the real-time press query.
 * The inverse of the tables above; letters and digits map back through
 * their base entries. */
static int
key_to_scan(int key)
{
	unsigned scan;

	if (key <= 0)
		return -1;
	if (key >= 'A' && key <= 'Z')
		key = key - 'A' + 'a';
	switch (key) {
	case NOCT_BEUI_KEY_ESCAPE: return 0x00;
	case NOCT_BEUI_KEY_BACKSPACE: return 0x0e;
	case NOCT_BEUI_KEY_TAB: return 0x0f;
	case NOCT_BEUI_KEY_ENTER: return 0x1c;
	case NOCT_BEUI_KEY_INSERT: return 0x38;
	case NOCT_BEUI_KEY_DELETE: return 0x39;
	case NOCT_BEUI_KEY_UP: return 0x3a;
	case NOCT_BEUI_KEY_LEFT: return 0x3b;
	case NOCT_BEUI_KEY_RIGHT: return 0x3c;
	case NOCT_BEUI_KEY_DOWN: return 0x3d;
	case NOCT_BEUI_KEY_HOME: return 0x3e;
	case NOCT_BEUI_KEY_END: return 0x3f;
	case NOCT_BEUI_KEY_PAGE_UP: return 0x36;
	case NOCT_BEUI_KEY_PAGE_DOWN: return 0x37;
	case NOCT_BEUI_KEY_SHIFT: return SCAN_SHIFT_L;
	default: break;
	}
	if (key >= NOCT_BEUI_KEY_F1 && key <= NOCT_BEUI_KEY_F10)
		return 0x62 + (key - NOCT_BEUI_KEY_F1);
	for (scan = 0; scan < SCAN_MAX; scan++)
		if (base_table[scan] == (uint8_t)key)
			return (int)scan;
	return -1;
}

void
boots_kbd_pc98_reset(struct boots_kbd_pc98 *kb)
{
	unsigned i;

	kb->shift = kb->ctrl = kb->graph = kb->caps = kb->kana = 0;
	for (i = 0; i < sizeof(kb->down); i++)
		kb->down[i] = 0;
}

static void
set_down(struct boots_kbd_pc98 *kb, uint8_t scan, int pressed)
{
	uint8_t mask = (uint8_t)(1U << (scan & 7U));

	if (pressed)
		kb->down[scan >> 3] |= mask;
	else
		kb->down[scan >> 3] &= (uint8_t)~mask;
}

int
boots_kbd_pc98_feed(struct boots_kbd_pc98 *kb, uint8_t raw)
{
	uint8_t scan = raw & 0x7fU;
	int pressed = (raw & 0x80U) == 0;
	uint8_t ch;

	set_down(kb, scan, pressed);

	switch (scan) {
	case SCAN_SHIFT_L:
	case SCAN_SHIFT_R:
		kb->shift = (uint8_t)pressed;
		return 0;
	case SCAN_CTRL:
		kb->ctrl = (uint8_t)pressed;
		return 0;
	case SCAN_GRAPH:
		kb->graph = (uint8_t)pressed;
		return 0;
	case SCAN_CAPS:
		/* Caps and kana are locking keys: toggle on the make. */
		if (pressed)
			kb->caps ^= 1U;
		return 0;
	case SCAN_KANA:
		if (pressed)
			kb->kana ^= 1U;
		return 0;
	default:
		break;
	}

	/* Releases and modifiers produce no key. */
	if (!pressed)
		return 0;

	{
		int special = special_key(scan);

		if (special != 0)
			return special;
	}

	ch = kb->shift ? shift_table[scan] : base_table[scan];
	if (ch == 0)
		return 0;

	/* Caps lock affects letters only. */
	if (kb->caps && ch >= 'a' && ch <= 'z')
		ch = (uint8_t)(ch - 'a' + 'A');
	else if (kb->caps && ch >= 'A' && ch <= 'Z')
		ch = (uint8_t)(ch - 'A' + 'a');

	/* Control collapses a letter to its control code. */
	if (kb->ctrl) {
		if (ch >= 'a' && ch <= 'z')
			return ch - 'a' + 1;
		if (ch >= 'A' && ch <= 'Z')
			return ch - 'A' + 1;
	}
	return ch;
}

int
boots_kbd_pc98_is_down(const struct boots_kbd_pc98 *kb, int key)
{
	int scan = key_to_scan(key);

	if (scan < 0)
		return -1;
	return (kb->down[scan >> 3] >> (scan & 7)) & 1;
}
