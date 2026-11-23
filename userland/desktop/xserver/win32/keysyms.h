/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The keyboard map of the X server's Windows backend: which keysyms each
 * X keycode carries and which keycodes are the modifiers, made from what
 * the Windows keyboard layout types.
 *
 * The X core protocol gives each keycode a row of keysyms (the answer of
 * GetKeyboardMapping) and each of the eight modifiers a few keycodes (the
 * answer of GetModifierMapping); an X client decides the character of a
 * key from them.  The first two columns are the key alone and with Shift,
 * the third and fourth the same in the second group, which the core
 * protocol's clients choose while the Mode_switch keysym's modifier is
 * held.  The backend asks the layout what each key types alone, with
 * Shift, with AltGr and with Shift and AltGr (layout.c) and builds the map
 * here: the keycode is the evdev code plus 8, AltGr is Mode_switch when
 * the layout has AltGr, and the keys that type no character get the
 * keysyms of their function.
 *
 * Pure: no Windows header, no system call, no state; the tests run on the
 * host.
 */

#ifndef KEILAND_X11_WIN32_KEYSYMS_H
#define KEILAND_X11_WIN32_KEYSYMS_H

#include <stdint.h>

/* The keycodes of the map: evdev 0 to 247 plus 8. */
#define X11_WIN32_KEYCODE_FIRST	8U
#define X11_WIN32_KEYCODE_COUNT	256U

/* The keysyms of one keycode: alone, Shift, AltGr, Shift and AltGr. */
#define X11_WIN32_COLUMNS	4U

/* The modifiers (Shift, Lock, Control, Mod1 to Mod5) and the keycodes each may have. */
#define X11_WIN32_MODIFIERS		8U
#define X11_WIN32_MODIFIER_KEYCODES	4U

/* The column of each state of a key. */
#define X11_WIN32_COLUMN_PLAIN		0U
#define X11_WIN32_COLUMN_SHIFT		1U
#define X11_WIN32_COLUMN_ALTGR		2U
#define X11_WIN32_COLUMN_SHIFT_ALTGR	3U

/* The virtual-key codes that make a character key a Japanese input key (<winuser.h> values). */
#define X11_WIN32_VK_CONVERT		0x1CU
#define X11_WIN32_VK_NONCONVERT		0x1DU
#define X11_WIN32_VK_DBE_SBCSCHAR	0xF3U
#define X11_WIN32_VK_DBE_DBCSCHAR	0xF4U

/*
 * What the layout types with one key, as layout.c read it.
 *
 * virtual_key is the layout's virtual-key code of the key (0: the layout
 * has none there).  chars are the characters (Unicode code points, 0 for
 * none) of the key alone, with Shift, with AltGr and with Shift and AltGr;
 * dead marks a column whose character is a dead key's (the accent it puts
 * on the next key).
 */
struct x11_win32_key_chars {
	uint16_t virtual_key;
	uint32_t chars[X11_WIN32_COLUMNS];
	uint8_t dead[X11_WIN32_COLUMNS];
};

/*
 * The keyboard map the X server answers its clients with.
 *
 * keysyms is indexed by the keycode (0 to 7 are never used), 0 being
 * NoSymbol.  modifiers lists for Shift, Lock, Control and Mod1 to Mod5 the
 * keycodes of each, 0 filling the rest.  altgr says whether the right Alt
 * key is AltGr (Mode_switch in Mod5) rather than a second Alt (Alt_R in
 * Mod1).
 */
struct x11_win32_keymap {
	uint32_t keysyms[X11_WIN32_KEYCODE_COUNT][X11_WIN32_COLUMNS];
	uint8_t modifiers[X11_WIN32_MODIFIERS][X11_WIN32_MODIFIER_KEYCODES];
	int altgr;
};

int x11_win32_key_is_typed(uint32_t code);
int x11_win32_keymap_altgr(const struct x11_win32_key_chars *keys, unsigned count);
void x11_win32_keymap_build(const struct x11_win32_key_chars *keys, unsigned count, int altgr, struct x11_win32_keymap *map);
uint32_t x11_win32_keysym_of_char(uint32_t character);
uint32_t x11_win32_keysym_of_dead(uint32_t character);
uint32_t x11_win32_keysym_of_key(uint32_t code, unsigned column, int altgr);

#endif
