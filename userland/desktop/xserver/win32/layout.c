/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The X server's Windows backend: what the thread's keyboard layout types
 * with each key, read into the X keyboard map (keysyms.c).
 *
 * For each key that types characters, the layout gives the key's
 * virtual-key code (MapVirtualKeyExW) and the character of the key alone,
 * with Shift, with AltGr (Ctrl and Alt) and with Shift and AltGr
 * (ToUnicodeEx, asked not to change the keyboard's state; a dead key is
 * asked a second time, which ends its state where that request is not
 * honoured).
 */

#include "userland/desktop/xserver/win32/internal.h"

#include <string.h>

/* ToUnicodeEx's flag that leaves the keyboard's state (a pending dead key) as it was (Windows 10, 1607). */
#define LAYOUT_KEEP_STATE	0x4U

/* The key-state byte of a key held. */
#define LAYOUT_DOWN		0x80U

/* The most UTF-16 units one key types. */
#define LAYOUT_UNITS		8

static void layout_key(HKL layout, uint32_t code, struct x11_win32_key_chars *key);
static void layout_column(HKL layout, UINT virtual_key, UINT scan, unsigned column, struct x11_win32_key_chars *key);

/*
 * Reads the thread's keyboard layout into the backend's map: the
 * characters of each key, whether the right Alt is AltGr, and the keysyms
 * and modifiers made of them.
 */
void
win32_layout_read(
	struct x11_win32 *backend)
{
	static struct x11_win32_key_chars keys[X11_WIN32_EVDEV_MAX + 1];
	HKL layout;
	uint32_t code;
	int altgr;
	int typed;

	/* The layout of the thread the windows belong to. */
	layout = GetKeyboardLayout(0);

	/* Asks the layout about each key that types characters. */
	memset(keys, 0, sizeof(keys));
	for (code = 0; code <= X11_WIN32_EVDEV_MAX; code++) {
		typed = x11_win32_key_is_typed(code);
		if (typed)
			layout_key(layout, code, &keys[code]);
	}

	/* Makes the map. */
	altgr = x11_win32_keymap_altgr(keys, X11_WIN32_EVDEV_MAX + 1);
	x11_win32_keymap_build(keys, X11_WIN32_EVDEV_MAX + 1, altgr, &backend->keymap);
	win32_log(backend, "X11 WIN32 layout %p altgr=%d", (void *)layout, altgr);
}

/* Reads what the layout types with one key (an evdev code) in the four states. */
static void
layout_key(
	HKL layout,
	uint32_t code,
	struct x11_win32_key_chars *key)
{
	unsigned scan;
	int extended;
	int error;
	UINT scan_ex;
	UINT virtual_key;
	unsigned column;

	/* The key's scan code, which the layout knows it by. */
	error = x11_win32_evdev_to_scan(code, &scan, &extended);
	if (error != 0)
		return;
	scan_ex = scan;
	if (extended)
		scan_ex |= 0xE000U;

	/* The layout's virtual-key code of the key (none: the layout has no key there). */
	virtual_key = MapVirtualKeyExW(scan_ex, MAPVK_VSC_TO_VK_EX, layout);
	key->virtual_key = (uint16_t)virtual_key;
	if (virtual_key == 0)
		return;

	/* Its character in each state. */
	for (column = 0; column < X11_WIN32_COLUMNS; column++)
		layout_column(layout, virtual_key, scan, column, key);
}

/*
 * Reads the character one key types in one state into its column: none
 * for nothing or a control character, the character with its dead mark
 * for a dead key.
 */
static void
layout_column(
	HKL layout,
	UINT virtual_key,
	UINT scan,
	unsigned column,
	struct x11_win32_key_chars *key)
{
	BYTE state[256];
	WCHAR units[LAYOUT_UNITS];
	int count;
	uint32_t character;

	/* The keys held: Shift for the Shift columns, Ctrl and both Alts for AltGr's. */
	memset(state, 0, sizeof(state));
	if (column == X11_WIN32_COLUMN_SHIFT || column == X11_WIN32_COLUMN_SHIFT_ALTGR) {
		state[VK_SHIFT] = LAYOUT_DOWN;
		state[VK_LSHIFT] = LAYOUT_DOWN;
	}
	if (column == X11_WIN32_COLUMN_ALTGR || column == X11_WIN32_COLUMN_SHIFT_ALTGR) {
		state[VK_CONTROL] = LAYOUT_DOWN;
		state[VK_LCONTROL] = LAYOUT_DOWN;
		state[VK_MENU] = LAYOUT_DOWN;
		state[VK_RMENU] = LAYOUT_DOWN;
	}

	/* Asks the layout what the key types. */
	count = ToUnicodeEx(virtual_key, scan, state, units, LAYOUT_UNITS, LAYOUT_KEEP_STATE, layout);

	/* A dead key: asked again, which ends its state where the flag is not honoured. */
	if (count < 0) {
		(void)ToUnicodeEx(virtual_key, scan, state, units + 1, LAYOUT_UNITS - 1, LAYOUT_KEEP_STATE, layout);
		character = units[0];
		if (character < 0x20U || character == 0x7FU)
			return;

		key->chars[column] = character;
		key->dead[column] = 1;
		return;
	}

	/* Nothing typed. */
	if (count == 0)
		return;

	/* One character: one unit, or a surrogate pair. */
	character = units[0];
	if (count == 2 && units[0] >= 0xD800U && units[0] <= 0xDBFFU && units[1] >= 0xDC00U && units[1] <= 0xDFFFU) {
		character = 0x10000U + (((uint32_t)units[0] - 0xD800U) << 10) + ((uint32_t)units[1] - 0xDC00U);
	} else if (count != 1) {
		return;
	}

	/* A control character (Ctrl and a letter in AltGr's state) is no character of the key. */
	if (character < 0x20U || character == 0x7FU)
		return;

	/* Succeeded: the column's character. */
	key->chars[column] = character;
}
