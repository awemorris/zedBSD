/* X68000 JIS keyboard scan-code translation. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <hal/hal.h>
#include "keyboard-map.h"

#define X68K_SCAN_CAPS  0x5dU
#define X68K_SCAN_SHIFT 0x70U
#define X68K_SCAN_CTRL  0x71U
#define X68K_SCAN_OPT1  0x72U

static const uint16_t normal_map[128] = {
	[0x01] = X68K_KEY_ESCAPE,
	[0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4',
	[0x06] = '5', [0x07] = '6', [0x08] = '7', [0x09] = '8',
	[0x0a] = '9', [0x0b] = '0', [0x0c] = '-', [0x0d] = '^',
	[0x0e] = '\\', [0x0f] = X68K_KEY_BACKSPACE, [0x10] = X68K_KEY_TAB,
	[0x11] = 'q', [0x12] = 'w', [0x13] = 'e', [0x14] = 'r',
	[0x15] = 't', [0x16] = 'y', [0x17] = 'u', [0x18] = 'i',
	[0x19] = 'o', [0x1a] = 'p', [0x1b] = '@', [0x1c] = '[',
	[0x1d] = X68K_KEY_ENTER, [0x1e] = 'a', [0x1f] = 's',
	[0x20] = 'd', [0x21] = 'f', [0x22] = 'g', [0x23] = 'h',
	[0x24] = 'j', [0x25] = 'k', [0x26] = 'l', [0x27] = ';',
	[0x28] = ':', [0x29] = ']', [0x2a] = 'z', [0x2b] = 'x',
	[0x2c] = 'c', [0x2d] = 'v', [0x2e] = 'b', [0x2f] = 'n',
	[0x30] = 'm', [0x31] = ',', [0x32] = '.', [0x33] = '/',
	[0x34] = '_', [0x35] = ' ',
	[0x36] = X68K_KEY_HOME, [0x37] = X68K_KEY_DELETE,
	[0x38] = X68K_KEY_PAGE_UP, [0x39] = X68K_KEY_PAGE_DOWN,
	[0x3a] = X68K_KEY_END, [0x3b] = X68K_KEY_LEFT,
	[0x3c] = X68K_KEY_UP, [0x3d] = X68K_KEY_RIGHT,
	[0x3e] = X68K_KEY_DOWN,
	[0x40] = '/', [0x41] = '*', [0x42] = '-', [0x43] = '7',
	[0x44] = '8', [0x45] = '9', [0x46] = '+', [0x47] = '4',
	[0x48] = '5', [0x49] = '6', [0x4a] = '=', [0x4b] = '1',
	[0x4c] = '2', [0x4d] = '3', [0x4e] = X68K_KEY_ENTER,
	[0x4f] = '0', [0x50] = ',', [0x51] = '.',
	[0x63] = X68K_KEY_F1, [0x64] = X68K_KEY_F2,
	[0x65] = X68K_KEY_F3, [0x66] = X68K_KEY_F4,
	[0x67] = X68K_KEY_F5, [0x68] = X68K_KEY_F6,
	[0x69] = X68K_KEY_F7, [0x6a] = X68K_KEY_F8,
	[0x6b] = X68K_KEY_F9, [0x6c] = X68K_KEY_F10,
};

static const uint16_t shift_map[128] = {
	[0x02] = '!', [0x03] = '"', [0x04] = '#', [0x05] = '$',
	[0x06] = '%', [0x07] = '&', [0x08] = '\'', [0x09] = '(',
	[0x0a] = ')', [0x0c] = '=', [0x0d] = '~', [0x0e] = '|',
	[0x1b] = '`', [0x1c] = '{', [0x27] = '+', [0x28] = '*',
	[0x29] = '}', [0x31] = '<', [0x32] = '>', [0x33] = '?',
	[0x34] = '_',
};

static int
scan_down(const struct x68k_keyboard_state *state, unsigned scan)
{
	return (state->down[scan >> 3] >> (scan & 7U)) & 1U;
}

static int
key_to_scan(int key)
{
	unsigned scan;
	if (key == X68K_KEY_SHIFT) return X68K_SCAN_SHIFT;
	for (scan = 0; scan < 128U; scan++)
		if (normal_map[scan] == (uint16_t)key)
			return (int)scan;
	return -1;
}

void
x68k_keyboard_state_reset(struct x68k_keyboard_state *state)
{
	unsigned index;
	for (index = 0; index < sizeof(state->down); index++)
		state->down[index] = 0;
	state->caps_lock = 0;
}

unsigned
x68k_keyboard_modifiers(const struct x68k_keyboard_state *state)
{
	return (scan_down(state, X68K_SCAN_SHIFT) ? X68K_KEY_EVENT_SHIFT : 0U) |
	    (scan_down(state, X68K_SCAN_CTRL) ? X68K_KEY_EVENT_CTRL : 0U) |
	    (scan_down(state, X68K_SCAN_OPT1) ? X68K_KEY_EVENT_GRAPH : 0U);
}

int
x68k_keyboard_feed(struct x68k_keyboard_state *state, uint8_t raw)
{
	unsigned scan = raw & 0x7fU;
	int released = (raw & 0x80U) != 0;
	uint16_t key;

	if (released)
		state->down[scan >> 3] &= (uint8_t)~(1U << (scan & 7U));
	else
		state->down[scan >> 3] |= (uint8_t)(1U << (scan & 7U));
	if (scan == X68K_SCAN_CAPS && !released)
		state->caps_lock ^= 1U;
	if (released || scan == X68K_SCAN_CAPS || scan == X68K_SCAN_SHIFT ||
	    scan == X68K_SCAN_CTRL || scan == X68K_SCAN_OPT1)
		return -1;
	key = scan_down(state, X68K_SCAN_SHIFT) && shift_map[scan] != 0 ?
		shift_map[scan] : normal_map[scan];
	if (key >= 'a' && key <= 'z') {
		if (scan_down(state, X68K_SCAN_SHIFT) ^ state->caps_lock)
			key = (uint16_t)(key - 'a' + 'A');
		if (scan_down(state, X68K_SCAN_CTRL))
			key = (uint16_t)((key | 0x20U) - 'a' + 1U);
	}
	if (key == 0)
		return -1;
	return (int)((unsigned)key | x68k_keyboard_modifiers(state));
}

int
x68k_keyboard_key_state(const struct x68k_keyboard_state *state, int key)
{
	int scan = key_to_scan(key);
	return scan < 0 ? -1 : scan_down(state, (unsigned)scan);
}
