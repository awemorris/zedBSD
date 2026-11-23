/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The keyboard map of the X server's Windows backend: keysyms and
 * modifiers from what the layout types.
 */

#include "userland/desktop/xserver/win32/keysyms.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The keysyms the map names itself (values below, with their source). */
#define KEYSYM_NONE			0x0000U
#define KEYSYM_ALT_R			0xFFEAU
#define KEYSYM_META_R			0xFFE8U
#define KEYSYM_MODE_SWITCH		0xFF7EU
#define KEYSYM_ZENKAKU_HANKAKU		0xFF2AU
#define KEYSYM_HENKAN_MODE		0xFF23U
#define KEYSYM_MUHENKAN			0xFF22U

/* The base of the keysyms that carry a Unicode code point (keysym = base + code point). */
#define KEYSYM_UNICODE_BASE		0x01000000U

/* The largest Unicode code point and the surrogates' range, which name no character. */
#define UNICODE_LAST			0x10FFFFU
#define UNICODE_SURROGATE_FIRST		0xD800U
#define UNICODE_SURROGATE_LAST		0xDFFFU

/* The evdev codes the modifier map names. */
#define CODE_LEFTCTRL		29U
#define CODE_LEFTSHIFT		42U
#define CODE_RIGHTSHIFT		54U
#define CODE_LEFTALT		56U
#define CODE_CAPSLOCK		58U
#define CODE_NUMLOCK		69U
#define CODE_GRAVE		41U
#define CODE_RIGHTCTRL		97U
#define CODE_RIGHTALT		100U
#define CODE_LEFTMETA		125U
#define CODE_RIGHTMETA		126U

/* The modifiers by their index in the map (X's order). */
#define MODIFIER_SHIFT		0U
#define MODIFIER_LOCK		1U
#define MODIFIER_CONTROL	2U
#define MODIFIER_MOD1		3U
#define MODIFIER_MOD2		4U
#define MODIFIER_MOD4		6U
#define MODIFIER_MOD5		7U

/*
 * A key that types no character and has the same keysyms on every
 * layout: its evdev code, its keysym alone and its keysym with Shift (0:
 * the same as alone, which X reads from an empty second column).
 */
struct keysym_key {
	uint16_t code;
	uint32_t plain;
	uint32_t shift;
};

/*
 * A run of evdev codes whose keys type characters, which the layout is
 * asked about.
 */
struct keysym_range {
	uint16_t first;
	uint16_t last;
};

/*
 * A dead key's accent, as the character the layout reports for it, and
 * the dead keysym that names the accent.
 */
struct keysym_dead {
	uint32_t character;
	uint32_t keysym;
};

/*
 * The keys of fixed function.
 *
 * Values referenced: xorgproto 2024.1 include/X11/keysymdef.h (the keysyms
 * of the function keys, the keypad, the modifiers and the Japanese and
 * Korean input keys) and include/X11/XF86keysym.h (the media and browser
 * keys); xkeyboard-config 2.42 symbols/inet with keycodes/evdev (which
 * keysym each media and browser key's evdev code carries); Linux v6.12
 * include/uapi/linux/input-event-codes.h (the evdev codes).  The keypad keys carry their function first and their digit with
 * Shift (and with Num Lock, which X applies to the keypad's second
 * column), as the X servers on Linux give them.  The right Alt key is not
 * here: it depends on whether the layout has AltGr.
 */
static const struct keysym_key keysym_keys[] = {
	{ 1U, 0xFF1BU, 0U },			/* KEY_ESC: Escape */
	{ 14U, 0xFF08U, 0U },			/* KEY_BACKSPACE: BackSpace */
	{ 15U, 0xFF09U, 0xFE20U },		/* KEY_TAB: Tab, ISO_Left_Tab */
	{ 28U, 0xFF0DU, 0U },			/* KEY_ENTER: Return */
	{ 29U, 0xFFE3U, 0U },			/* KEY_LEFTCTRL: Control_L */
	{ 42U, 0xFFE1U, 0U },			/* KEY_LEFTSHIFT: Shift_L */
	{ 54U, 0xFFE2U, 0U },			/* KEY_RIGHTSHIFT: Shift_R */
	{ 55U, 0xFFAAU, 0U },			/* KEY_KPASTERISK: KP_Multiply */
	{ 56U, 0xFFE9U, 0xFFE7U },		/* KEY_LEFTALT: Alt_L, Meta_L */
	{ 58U, 0xFFE5U, 0U },			/* KEY_CAPSLOCK: Caps_Lock */
	{ 59U, 0xFFBEU, 0U },			/* KEY_F1: F1 */
	{ 60U, 0xFFBFU, 0U },			/* KEY_F2: F2 */
	{ 61U, 0xFFC0U, 0U },			/* KEY_F3: F3 */
	{ 62U, 0xFFC1U, 0U },			/* KEY_F4: F4 */
	{ 63U, 0xFFC2U, 0U },			/* KEY_F5: F5 */
	{ 64U, 0xFFC3U, 0U },			/* KEY_F6: F6 */
	{ 65U, 0xFFC4U, 0U },			/* KEY_F7: F7 */
	{ 66U, 0xFFC5U, 0U },			/* KEY_F8: F8 */
	{ 67U, 0xFFC6U, 0U },			/* KEY_F9: F9 */
	{ 68U, 0xFFC7U, 0U },			/* KEY_F10: F10 */
	{ 69U, 0xFF7FU, 0U },			/* KEY_NUMLOCK: Num_Lock */
	{ 70U, 0xFF14U, 0U },			/* KEY_SCROLLLOCK: Scroll_Lock */
	{ 71U, 0xFF95U, 0xFFB7U },		/* KEY_KP7: KP_Home, KP_7 */
	{ 72U, 0xFF97U, 0xFFB8U },		/* KEY_KP8: KP_Up, KP_8 */
	{ 73U, 0xFF9AU, 0xFFB9U },		/* KEY_KP9: KP_Prior, KP_9 */
	{ 74U, 0xFFADU, 0U },			/* KEY_KPMINUS: KP_Subtract */
	{ 75U, 0xFF96U, 0xFFB4U },		/* KEY_KP4: KP_Left, KP_4 */
	{ 76U, 0xFF9DU, 0xFFB5U },		/* KEY_KP5: KP_Begin, KP_5 */
	{ 77U, 0xFF98U, 0xFFB6U },		/* KEY_KP6: KP_Right, KP_6 */
	{ 78U, 0xFFABU, 0U },			/* KEY_KPPLUS: KP_Add */
	{ 79U, 0xFF9CU, 0xFFB1U },		/* KEY_KP1: KP_End, KP_1 */
	{ 80U, 0xFF99U, 0xFFB2U },		/* KEY_KP2: KP_Down, KP_2 */
	{ 81U, 0xFF9BU, 0xFFB3U },		/* KEY_KP3: KP_Next, KP_3 */
	{ 82U, 0xFF9EU, 0xFFB0U },		/* KEY_KP0: KP_Insert, KP_0 */
	{ 83U, 0xFF9FU, 0xFFAEU },		/* KEY_KPDOT: KP_Delete, KP_Decimal */
	{ 87U, 0xFFC8U, 0U },			/* KEY_F11: F11 */
	{ 88U, 0xFFC9U, 0U },			/* KEY_F12: F12 */
	{ 90U, 0xFF26U, 0U },			/* KEY_KATAKANA: Katakana */
	{ 91U, 0xFF25U, 0U },			/* KEY_HIRAGANA: Hiragana */
	{ 92U, 0xFF23U, 0U },			/* KEY_HENKAN: Henkan_Mode */
	{ 93U, 0xFF27U, 0U },			/* KEY_KATAKANAHIRAGANA: Hiragana_Katakana */
	{ 94U, 0xFF22U, 0U },			/* KEY_MUHENKAN: Muhenkan */
	{ 95U, 0xFFACU, 0U },			/* KEY_KPJPCOMMA: KP_Separator */
	{ 96U, 0xFF8DU, 0U },			/* KEY_KPENTER: KP_Enter */
	{ 97U, 0xFFE4U, 0U },			/* KEY_RIGHTCTRL: Control_R */
	{ 98U, 0xFFAFU, 0U },			/* KEY_KPSLASH: KP_Divide */
	{ 99U, 0xFF61U, 0xFF15U },		/* KEY_SYSRQ: Print, Sys_Req */
	{ 102U, 0xFF50U, 0U },			/* KEY_HOME: Home */
	{ 103U, 0xFF52U, 0U },			/* KEY_UP: Up */
	{ 104U, 0xFF55U, 0U },			/* KEY_PAGEUP: Prior */
	{ 105U, 0xFF51U, 0U },			/* KEY_LEFT: Left */
	{ 106U, 0xFF53U, 0U },			/* KEY_RIGHT: Right */
	{ 107U, 0xFF57U, 0U },			/* KEY_END: End */
	{ 108U, 0xFF54U, 0U },			/* KEY_DOWN: Down */
	{ 109U, 0xFF56U, 0U },			/* KEY_PAGEDOWN: Next */
	{ 110U, 0xFF63U, 0U },			/* KEY_INSERT: Insert */
	{ 111U, 0xFFFFU, 0U },			/* KEY_DELETE: Delete */
	{ 113U, 0x1008FF12U, 0U },		/* KEY_MUTE: XF86AudioMute */
	{ 114U, 0x1008FF11U, 0U },		/* KEY_VOLUMEDOWN: XF86AudioLowerVolume */
	{ 115U, 0x1008FF13U, 0U },		/* KEY_VOLUMEUP: XF86AudioRaiseVolume */
	{ 116U, 0x1008FF2AU, 0U },		/* KEY_POWER: XF86PowerOff */
	{ 117U, 0xFFBDU, 0U },			/* KEY_KPEQUAL: KP_Equal */
	{ 119U, 0xFF13U, 0xFF6BU },		/* KEY_PAUSE: Pause, Break */
	{ 121U, 0xFFACU, 0U },			/* KEY_KPCOMMA: KP_Separator */
	{ 122U, 0xFF31U, 0U },			/* KEY_HANGEUL: Hangul */
	{ 123U, 0xFF34U, 0U },			/* KEY_HANJA: Hangul_Hanja */
	{ 125U, 0xFFEBU, 0U },			/* KEY_LEFTMETA: Super_L */
	{ 126U, 0xFFECU, 0U },			/* KEY_RIGHTMETA: Super_R */
	{ 127U, 0xFF67U, 0U },			/* KEY_COMPOSE: Menu */
	{ 128U, 0xFF69U, 0U },			/* KEY_STOP: Cancel */
	{ 140U, 0x1008FF1DU, 0U },		/* KEY_CALC: XF86Calculator */
	{ 142U, 0x1008FF2FU, 0U },		/* KEY_SLEEP: XF86Sleep */
	{ 143U, 0x1008FF2BU, 0U },		/* KEY_WAKEUP: XF86WakeUp */
	{ 144U, 0x1008FF5DU, 0U },		/* KEY_FILE: XF86Explorer */
	{ 155U, 0x1008FF19U, 0U },		/* KEY_MAIL: XF86Mail */
	{ 156U, 0x1008FF30U, 0U },		/* KEY_BOOKMARKS: XF86Favorites */
	{ 158U, 0x1008FF26U, 0U },		/* KEY_BACK: XF86Back */
	{ 159U, 0x1008FF27U, 0U },		/* KEY_FORWARD: XF86Forward */
	{ 163U, 0x1008FF17U, 0U },		/* KEY_NEXTSONG: XF86AudioNext */
	{ 164U, 0x1008FF14U, 0U },		/* KEY_PLAYPAUSE: XF86AudioPlay */
	{ 165U, 0x1008FF16U, 0U },		/* KEY_PREVIOUSSONG: XF86AudioPrev */
	{ 166U, 0x1008FF15U, 0U },		/* KEY_STOPCD: XF86AudioStop */
	{ 171U, 0x1008FF81U, 0U },		/* KEY_CONFIG: XF86Tools */
	{ 172U, 0x1008FF18U, 0U },		/* KEY_HOMEPAGE: XF86HomePage */
	{ 173U, 0x1008FF73U, 0U },		/* KEY_REFRESH: XF86Reload */
	{ 183U, 0xFFCAU, 0U },			/* KEY_F13: F13 */
	{ 184U, 0xFFCBU, 0U },			/* KEY_F14: F14 */
	{ 185U, 0xFFCCU, 0U },			/* KEY_F15: F15 */
	{ 186U, 0xFFCDU, 0U },			/* KEY_F16: F16 */
	{ 187U, 0xFFCEU, 0U },			/* KEY_F17: F17 */
	{ 188U, 0xFFCFU, 0U },			/* KEY_F18: F18 */
	{ 189U, 0xFFD0U, 0U },			/* KEY_F19: F19 */
	{ 190U, 0xFFD1U, 0U },			/* KEY_F20: F20 */
	{ 191U, 0xFFD2U, 0U },			/* KEY_F21: F21 */
	{ 192U, 0xFFD3U, 0U },			/* KEY_F22: F22 */
	{ 193U, 0xFFD4U, 0U },			/* KEY_F23: F23 */
	{ 194U, 0xFFD5U, 0U },			/* KEY_F24: F24 */
	{ 217U, 0x1008FF1BU, 0U },		/* KEY_SEARCH: XF86Search */
};

/* The number of rows of keysym_keys. */
#define KEYSYM_KEY_COUNT	(sizeof(keysym_keys) / sizeof(keysym_keys[0]))

/*
 * The keys that type characters, by evdev code: the rows of the main
 * block (1 to =, Q to ], A to ` , \ to /), the space bar, the key left of
 * Z on ISO keyboards and the two extra keys of Japanese keyboards (Ro and
 * Yen).  Values referenced: Linux v6.12
 * include/uapi/linux/input-event-codes.h.
 */
static const struct keysym_range keysym_typed[] = {
	{ 2U, 13U },		/* KEY_1 to KEY_EQUAL */
	{ 16U, 27U },		/* KEY_Q to KEY_RIGHTBRACE */
	{ 30U, 41U },		/* KEY_A to KEY_GRAVE */
	{ 43U, 53U },		/* KEY_BACKSLASH to KEY_SLASH */
	{ 57U, 57U },		/* KEY_SPACE */
	{ 86U, 86U },		/* KEY_102ND */
	{ 89U, 89U },		/* KEY_RO */
	{ 124U, 124U },		/* KEY_YEN */
};

/* The number of rows of keysym_typed. */
#define KEYSYM_TYPED_COUNT	(sizeof(keysym_typed) / sizeof(keysym_typed[0]))

/*
 * The dead keys' accents.
 *
 * Values referenced: xorgproto 2024.1 include/X11/keysymdef.h (the dead_*
 * keysyms).  A layout reports a dead key by the spacing form of its accent
 * (the US International layout by the ASCII apostrophe and quotation mark,
 * and some layouts by the combining form), so each accent is here in the
 * forms layouts use.
 */
static const struct keysym_dead keysym_deads[] = {
	{ 0x0060U, 0xFE50U },		/* grave accent: dead_grave */
	{ 0x0300U, 0xFE50U },		/* combining grave accent: dead_grave */
	{ 0x00B4U, 0xFE51U },		/* acute accent: dead_acute */
	{ 0x0027U, 0xFE51U },		/* apostrophe (US International): dead_acute */
	{ 0x0301U, 0xFE51U },		/* combining acute accent: dead_acute */
	{ 0x005EU, 0xFE52U },		/* circumflex accent: dead_circumflex */
	{ 0x02C6U, 0xFE52U },		/* modifier letter circumflex: dead_circumflex */
	{ 0x0302U, 0xFE52U },		/* combining circumflex accent: dead_circumflex */
	{ 0x007EU, 0xFE53U },		/* tilde: dead_tilde */
	{ 0x02DCU, 0xFE53U },		/* small tilde: dead_tilde */
	{ 0x0303U, 0xFE53U },		/* combining tilde: dead_tilde */
	{ 0x00AFU, 0xFE54U },		/* macron: dead_macron */
	{ 0x0304U, 0xFE54U },		/* combining macron: dead_macron */
	{ 0x02D8U, 0xFE55U },		/* breve: dead_breve */
	{ 0x0306U, 0xFE55U },		/* combining breve: dead_breve */
	{ 0x02D9U, 0xFE56U },		/* dot above: dead_abovedot */
	{ 0x0307U, 0xFE56U },		/* combining dot above: dead_abovedot */
	{ 0x00A8U, 0xFE57U },		/* diaeresis: dead_diaeresis */
	{ 0x0022U, 0xFE57U },		/* quotation mark (US International): dead_diaeresis */
	{ 0x0308U, 0xFE57U },		/* combining diaeresis: dead_diaeresis */
	{ 0x02DAU, 0xFE58U },		/* ring above: dead_abovering */
	{ 0x00B0U, 0xFE58U },		/* degree sign (some layouts' ring): dead_abovering */
	{ 0x030AU, 0xFE58U },		/* combining ring above: dead_abovering */
	{ 0x02DDU, 0xFE59U },		/* double acute accent: dead_doubleacute */
	{ 0x030BU, 0xFE59U },		/* combining double acute accent: dead_doubleacute */
	{ 0x02C7U, 0xFE5AU },		/* caron: dead_caron */
	{ 0x030CU, 0xFE5AU },		/* combining caron: dead_caron */
	{ 0x00B8U, 0xFE5BU },		/* cedilla: dead_cedilla */
	{ 0x0327U, 0xFE5BU },		/* combining cedilla: dead_cedilla */
	{ 0x02DBU, 0xFE5CU },		/* ogonek: dead_ogonek */
	{ 0x0328U, 0xFE5CU },		/* combining ogonek: dead_ogonek */
};

/* The number of rows of keysym_deads. */
#define KEYSYM_DEAD_COUNT	(sizeof(keysym_deads) / sizeof(keysym_deads[0]))

static void keymap_add_modifier(struct x11_win32_keymap *map, unsigned modifier, uint32_t code);
static uint32_t keymap_typed_keysym(const struct x11_win32_key_chars *key, unsigned column);

/*
 * Tells whether a key (an evdev code) types characters on some layout, so
 * that the backend asks the layout about it.
 */
int
x11_win32_key_is_typed(
	uint32_t code)
{
	size_t i;

	/* Looks for the run the key is in. */
	for (i = 0; i < KEYSYM_TYPED_COUNT; i++) {
		if (code >= keysym_typed[i].first && code <= keysym_typed[i].last)
			return 1;
	}

	/* A key of fixed function, or none. */
	return 0;
}

/*
 * Tells whether a layout has AltGr: whether any of its keys types a
 * character with AltGr.
 *
 * keys holds count entries indexed by the evdev code.
 */
int
x11_win32_keymap_altgr(
	const struct x11_win32_key_chars *keys,
	unsigned count)
{
	unsigned code;

	/* Looks for a key that types something in the second group. */
	for (code = 0; code < count; code++) {
		if (keys[code].chars[X11_WIN32_COLUMN_ALTGR] != 0)
			return 1;
		if (keys[code].chars[X11_WIN32_COLUMN_SHIFT_ALTGR] != 0)
			return 1;
	}

	/* No key does: the right Alt is a second Alt. */
	return 0;
}

/*
 * Builds the X keyboard map of a layout.
 *
 * keys holds count entries indexed by the evdev code: what the layout
 * types with each key (entries of the keys of fixed function are not
 * read, and an index past count is a key the layout does not type with).
 * altgr says whether the right Alt key is AltGr.  The map is filled
 * whole: the keycode is the evdev code plus 8 for evdev codes 0 to 247.
 */
void
x11_win32_keymap_build(
	const struct x11_win32_key_chars *keys,
	unsigned count,
	int altgr,
	struct x11_win32_keymap *map)
{
	uint32_t code;
	unsigned keycode;
	unsigned column;
	uint32_t keysym;
	int typed;

	/* Starts from an empty map. */
	memset(map, 0, sizeof(*map));
	map->altgr = altgr;

	/* Gives every keycode its keysyms. */
	for (keycode = X11_WIN32_KEYCODE_FIRST; keycode < X11_WIN32_KEYCODE_COUNT; keycode++) {
		code = keycode - X11_WIN32_KEYCODE_FIRST;
		typed = x11_win32_key_is_typed(code);

		/* A key of fixed function has the same keysyms on every layout. */
		if (!typed) {
			map->keysyms[keycode][X11_WIN32_COLUMN_PLAIN] = x11_win32_keysym_of_key(code, X11_WIN32_COLUMN_PLAIN, altgr);
			map->keysyms[keycode][X11_WIN32_COLUMN_SHIFT] = x11_win32_keysym_of_key(code, X11_WIN32_COLUMN_SHIFT, altgr);
			continue;
		}

		/* A key the layout has nothing for keeps NoSymbol. */
		if (code >= count)
			continue;

		/*
		 * The Japanese layout makes the key left of 1 Hankaku/Zenkaku,
		 * which evdev cannot tell from the grave key: the layout's
		 * virtual-key code tells them apart.
		 */
		if (code == CODE_GRAVE) {
			if (keys[code].virtual_key == X11_WIN32_VK_DBE_SBCSCHAR ||
			    keys[code].virtual_key == X11_WIN32_VK_DBE_DBCSCHAR) {
				map->keysyms[keycode][X11_WIN32_COLUMN_PLAIN] = KEYSYM_ZENKAKU_HANKAKU;
				continue;
			}
		}

		/* A layout may make a character key Henkan or Muhenkan. */
		if (keys[code].virtual_key == X11_WIN32_VK_CONVERT) {
			map->keysyms[keycode][X11_WIN32_COLUMN_PLAIN] = KEYSYM_HENKAN_MODE;
			continue;
		}
		if (keys[code].virtual_key == X11_WIN32_VK_NONCONVERT) {
			map->keysyms[keycode][X11_WIN32_COLUMN_PLAIN] = KEYSYM_MUHENKAN;
			continue;
		}

		/* Gives each column the keysym of the character the layout types. */
		for (column = 0; column < X11_WIN32_COLUMNS; column++) {
			keysym = keymap_typed_keysym(&keys[code], column);
			map->keysyms[keycode][column] = keysym;
		}
	}

	/* Names the keycodes of the modifiers. */
	keymap_add_modifier(map, MODIFIER_SHIFT, CODE_LEFTSHIFT);
	keymap_add_modifier(map, MODIFIER_SHIFT, CODE_RIGHTSHIFT);
	keymap_add_modifier(map, MODIFIER_LOCK, CODE_CAPSLOCK);
	keymap_add_modifier(map, MODIFIER_CONTROL, CODE_LEFTCTRL);
	keymap_add_modifier(map, MODIFIER_CONTROL, CODE_RIGHTCTRL);
	keymap_add_modifier(map, MODIFIER_MOD1, CODE_LEFTALT);
	keymap_add_modifier(map, MODIFIER_MOD2, CODE_NUMLOCK);
	keymap_add_modifier(map, MODIFIER_MOD4, CODE_LEFTMETA);
	keymap_add_modifier(map, MODIFIER_MOD4, CODE_RIGHTMETA);

	/* The right Alt is Mode_switch in Mod5 on a layout with AltGr, else a second Alt in Mod1. */
	if (altgr) {
		keymap_add_modifier(map, MODIFIER_MOD5, CODE_RIGHTALT);
	} else {
		keymap_add_modifier(map, MODIFIER_MOD1, CODE_RIGHTALT);
	}
}

/*
 * Gives the keysym of a character the layout types.
 *
 * The keysyms of Latin-1's printable characters are their code points;
 * any other character is the Unicode keysym (0x01000000 plus the code
 * point), which Xlib and the toolkits read as that character.  Control
 * characters, surrogates and values past Unicode give 0 (NoSymbol): the
 * key's function keysym, not the layout, names a control key.
 *
 * Values referenced: X Window System Protocol, X Consortium Standard, X
 * Version 11 Release 6.9/7.0, Appendix A (the Latin-1 keysyms); xorgproto
 * 2024.1 include/X11/keysymdef.h (the Unicode keysyms' base).
 */
uint32_t
x11_win32_keysym_of_char(
	uint32_t character)
{
	/* The C0 controls and DEL type no keysym. */
	if (character < 0x20U || character == 0x7FU)
		return KEYSYM_NONE;

	/* The printable ASCII characters are their own keysyms. */
	if (character < 0x7FU)
		return character;

	/* The C1 controls type no keysym. */
	if (character < 0xA0U)
		return KEYSYM_NONE;

	/* The rest of Latin-1 is its own keysyms. */
	if (character <= 0xFFU)
		return character;

	/* A surrogate or a value past Unicode is no character. */
	if (character >= UNICODE_SURROGATE_FIRST && character <= UNICODE_SURROGATE_LAST)
		return KEYSYM_NONE;
	if (character > UNICODE_LAST)
		return KEYSYM_NONE;

	/* Any other character is its Unicode keysym. */
	return KEYSYM_UNICODE_BASE + character;
}

/*
 * Gives the dead keysym of a dead key's accent, or the keysym of the
 * character itself for an accent X has no dead keysym for.
 */
uint32_t
x11_win32_keysym_of_dead(
	uint32_t character)
{
	size_t i;
	uint32_t keysym;

	/* Looks the accent up. */
	for (i = 0; i < KEYSYM_DEAD_COUNT; i++) {
		if (keysym_deads[i].character == character)
			return keysym_deads[i].keysym;
	}

	/* An accent X has no dead keysym for types its character. */
	keysym = x11_win32_keysym_of_char(character);
	return keysym;
}

/*
 * Gives the keysym of a key of fixed function in a column (alone or with
 * Shift), the right Alt key's depending on whether the layout has AltGr.
 * Returns 0 (NoSymbol) for another column, a key the table does not know
 * and a second column the key does not need.
 */
uint32_t
x11_win32_keysym_of_key(
	uint32_t code,
	unsigned column,
	int altgr)
{
	size_t i;

	/* Only the first group's columns hold a function. */
	if (column != X11_WIN32_COLUMN_PLAIN && column != X11_WIN32_COLUMN_SHIFT)
		return KEYSYM_NONE;

	/* The right Alt key: Mode_switch on a layout with AltGr, else Alt_R and Meta_R. */
	if (code == CODE_RIGHTALT) {
		if (altgr) {
			if (column == X11_WIN32_COLUMN_PLAIN)
				return KEYSYM_MODE_SWITCH;

			return KEYSYM_NONE;
		}
		if (column == X11_WIN32_COLUMN_PLAIN)
			return KEYSYM_ALT_R;

		return KEYSYM_META_R;
	}

	/* Looks the key up among the keys of fixed function. */
	for (i = 0; i < KEYSYM_KEY_COUNT; i++) {
		if (keysym_keys[i].code != code)
			continue;

		if (column == X11_WIN32_COLUMN_PLAIN)
			return keysym_keys[i].plain;

		return keysym_keys[i].shift;
	}

	/* A key the table does not know. */
	return KEYSYM_NONE;
}

/* Adds an evdev key's keycode to a modifier's first free place. */
static void
keymap_add_modifier(
	struct x11_win32_keymap *map,
	unsigned modifier,
	uint32_t code)
{
	unsigned slot;

	/* Finds the modifier's first free place. */
	for (slot = 0; slot < X11_WIN32_MODIFIER_KEYCODES; slot++) {
		if (map->modifiers[modifier][slot] == 0) {
			map->modifiers[modifier][slot] = (uint8_t)(code + X11_WIN32_KEYCODE_FIRST);
			return;
		}
	}
}

/* Gives the keysym of one column of a key the layout types (0: none). */
static uint32_t
keymap_typed_keysym(
	const struct x11_win32_key_chars *key,
	unsigned column)
{
	uint32_t keysym;

	/* A column with no character has no keysym. */
	if (key->chars[column] == 0)
		return KEYSYM_NONE;

	/* A dead key's column is its accent's dead keysym. */
	if (key->dead[column] != 0) {
		keysym = x11_win32_keysym_of_dead(key->chars[column]);
		return keysym;
	}

	/* Any other column is its character's keysym. */
	keysym = x11_win32_keysym_of_char(key->chars[column]);
	return keysym;
}
