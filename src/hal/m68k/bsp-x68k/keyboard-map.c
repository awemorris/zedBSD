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
	[0x02] = X68K_KEY_JIS_1, [0x03] = X68K_KEY_JIS_2,
	[0x04] = X68K_KEY_JIS_3, [0x05] = X68K_KEY_JIS_4,
	[0x06] = X68K_KEY_JIS_5, [0x07] = X68K_KEY_JIS_6,
	[0x08] = X68K_KEY_JIS_7, [0x09] = X68K_KEY_JIS_8,
	[0x0a] = X68K_KEY_JIS_9, [0x0b] = X68K_KEY_JIS_0,
	[0x0c] = X68K_KEY_JIS_MINUS, [0x0d] = X68K_KEY_JIS_CARET,
	[0x0e] = X68K_KEY_JIS_YEN,
	[0x0f] = X68K_KEY_BACKSPACE, [0x10] = X68K_KEY_TAB,
	[0x11] = 'q', [0x12] = 'w', [0x13] = 'e', [0x14] = 'r',
	[0x15] = 't', [0x16] = 'y', [0x17] = 'u', [0x18] = 'i',
	[0x19] = 'o', [0x1a] = 'p', [0x1b] = X68K_KEY_JIS_AT,
	[0x1c] = X68K_KEY_JIS_LBRACE,
	[0x1d] = X68K_KEY_ENTER, [0x1e] = 'a', [0x1f] = 's',
	[0x20] = 'd', [0x21] = 'f', [0x22] = 'g', [0x23] = 'h',
	[0x24] = 'j', [0x25] = 'k', [0x26] = 'l',
	[0x27] = X68K_KEY_JIS_SEMI, [0x28] = X68K_KEY_JIS_COLON,
	[0x29] = X68K_KEY_JIS_RBRACE, [0x2a] = 'z', [0x2b] = 'x',
	[0x2c] = 'c', [0x2d] = 'v', [0x2e] = 'b', [0x2f] = 'n',
	[0x30] = 'm', [0x31] = X68K_KEY_JIS_COMMA,
	[0x32] = X68K_KEY_JIS_DOT, [0x33] = X68K_KEY_JIS_SLASH,
	[0x34] = X68K_KEY_JIS_RO, [0x35] = ' ',
	[0x36] = X68K_KEY_HOME, [0x37] = X68K_KEY_DELETE,
	[0x38] = X68K_KEY_PAGE_UP, [0x39] = X68K_KEY_PAGE_DOWN,
	[0x3a] = X68K_KEY_END, [0x3b] = X68K_KEY_LEFT,
	[0x3c] = X68K_KEY_UP, [0x3d] = X68K_KEY_RIGHT,
	[0x3e] = X68K_KEY_DOWN,
	[0x40] = X68K_KEY_KP_SLASH, [0x41] = X68K_KEY_KP_STAR,
	[0x42] = X68K_KEY_KP_MINUS, [0x43] = X68K_KEY_KP_7,
	[0x44] = X68K_KEY_KP_8, [0x45] = X68K_KEY_KP_9,
	[0x46] = X68K_KEY_KP_PLUS, [0x47] = X68K_KEY_KP_4,
	[0x48] = X68K_KEY_KP_5, [0x49] = X68K_KEY_KP_6,
	[0x4a] = X68K_KEY_KP_EQUAL, [0x4b] = X68K_KEY_KP_1,
	[0x4c] = X68K_KEY_KP_2, [0x4d] = X68K_KEY_KP_3,
	[0x4e] = X68K_KEY_KP_ENTER, [0x4f] = X68K_KEY_KP_0,
	[0x50] = X68K_KEY_KP_COMMA, [0x51] = X68K_KEY_KP_DOT,
	[0x63] = X68K_KEY_F1, [0x64] = X68K_KEY_F2,
	[0x65] = X68K_KEY_F3, [0x66] = X68K_KEY_F4,
	[0x67] = X68K_KEY_F5, [0x68] = X68K_KEY_F6,
	[0x69] = X68K_KEY_F7, [0x6a] = X68K_KEY_F8,
	[0x6b] = X68K_KEY_F9, [0x6c] = X68K_KEY_F10,
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

	/* Stable JIS identities retain the legacy scalar key-state aliases. */
	switch (key) {
	case '1': return 0x02;
	case '2': return 0x03;
	case '3': return 0x04;
	case '4': return 0x05;
	case '5': return 0x06;
	case '6': return 0x07;
	case '7': return 0x08;
	case '8': return 0x09;
	case '9': return 0x0a;
	case '0': return 0x0b;
	case '-': return 0x0c;
	case '^': return 0x0d;
	case '\\': return 0x0e;
	case '@': return 0x1b;
	case '[': return 0x1c;
	case ';': return 0x27;
	case ':': return 0x28;
	case ']': return 0x29;
	case ',': return 0x31;
	case '.': return 0x32;
	case '/': return 0x33;
	case '_': return 0x34;
	default: break;
	}
	if (key == X68K_KEY_SHIFT) return X68K_SCAN_SHIFT;
	if (key == X68K_KEY_CTRL) return X68K_SCAN_CTRL;
	if (key == X68K_KEY_GRAPH) return X68K_SCAN_OPT1;
	if (key == X68K_KEY_CAPS_LOCK) return X68K_SCAN_CAPS;
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
	for (index = 0; index < 128U; index++)
		state->last_key[index] = 0;
	state->caps_lock = 0;
}

static uint16_t
modifier_key(unsigned scan)
{
	switch (scan) {
	case X68K_SCAN_SHIFT: return X68K_KEY_SHIFT;
	case X68K_SCAN_CTRL: return X68K_KEY_CTRL;
	case X68K_SCAN_OPT1: return X68K_KEY_GRAPH;
	case X68K_SCAN_CAPS: return X68K_KEY_CAPS_LOCK;
	default: return 0;
	}
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
	int was_down = scan_down(state, scan);
	uint16_t key = modifier_key(scan);

	if (released) {
		if (key == 0)
			key = state->last_key[scan];
		state->down[scan >> 3] &= (uint8_t)~(1U << (scan & 7U));
		state->last_key[scan] = 0;
		return key == 0 ? -1 : (int)((unsigned)key |
		    x68k_keyboard_modifiers(state) | X68K_KEY_EVENT_RELEASE);
	} else {
		state->down[scan >> 3] |= (uint8_t)(1U << (scan & 7U));
	}
	if (scan == X68K_SCAN_CAPS && !was_down)
		state->caps_lock ^= 1U;
	if (key == 0) {
		key = normal_map[scan];
	}
	if (key == 0)
		return -1;
	state->last_key[scan] = key;
	return (int)((unsigned)key | x68k_keyboard_modifiers(state) |
	    (was_down ? X68K_KEY_EVENT_REPEAT : 0U));
}

int
x68k_keyboard_key_state(const struct x68k_keyboard_state *state, int key)
{
	int scan = key_to_scan(key);
	return scan < 0 ? -1 : scan_down(state, (unsigned)scan);
}
