/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The keys of the X server's Windows backend: scan code set 1 to evdev
 * codes and back.
 */

#include "userland/desktop/xserver/win32/scancode.h"

#include <stddef.h>
#include <stdint.h>

/*
 * How a row treats the extended bit of a key message: the key comes only
 * without the 0xE0 prefix, only with it, or either way (keyboards differ).
 */
#define SCAN_PLAIN	0U
#define SCAN_EXTENDED	1U
#define SCAN_EITHER	2U

/*
 * The block of make codes whose evdev code is the make code itself: Linux
 * numbered its first 83 keys (Escape to the keypad's period) after the
 * PC/XT scan codes, which scan code set 1 keeps.
 */
#define SCAN_IDENTITY_FIRST	0x01U
#define SCAN_IDENTITY_LAST	0x53U

/* The make code Num Lock and Pause share, told apart by the virtual-key code. */
#define SCAN_NUMLOCK_PAUSE	0x45U

/* The evdev codes of the two keys of that make code. */
#define SCAN_EVDEV_NUMLOCK	69U
#define SCAN_EVDEV_PAUSE	119U

/*
 * One make code of scan code set 1 outside the identity block, and the
 * evdev code of its key.
 *
 * scan is the make code without the 0xE0 prefix, extended says how the
 * prefix has to match, and code is the evdev code.  Rows are constant for
 * the life of the program.
 */
struct scan_row {
	uint8_t scan;
	uint8_t extended;
	uint16_t code;
};

/*
 * The make codes outside the identity block.
 *
 * Values referenced: Microsoft Learn, Keyboard Input Overview, the scan
 * codes table (its "Scan 1 Make" column and its HID usages; the extended
 * codes are written "E0 xx" there); Microsoft Keyboard Scan Code
 * Specification 1.3a, the scan code table by key location; Linux v6.12
 * drivers/hid/hid-input.c (the evdev code of each HID usage, values only)
 * and include/uapi/linux/input-event-codes.h (the evdev numbers).
 * The comment of a row names the key and its evdev name.
 *
 * Hangul (0x72 and 0xF2) and Hanja (0x71 and 0xF1) come with or without
 * the prefix depending on the keyboard.  0x76 is both F24 and LANG5 in the
 * table; it is F24, which the keyboards that have the key send.  0x54 is
 * Print Screen pressed with Alt, which Windows reports without the prefix
 * (and some keyboards with it).
 * Scan code 0x45 is not here: it is Num Lock and Pause, decided by the
 * virtual-key code.
 */
static const struct scan_row scan_rows[] = {
	/* The keys after the identity block, without the prefix. */
	{ 0x54U, SCAN_EITHER, 99U },		/* Alt+Print Screen: KEY_SYSRQ */
	{ 0x56U, SCAN_PLAIN, 86U },		/* the key left of Z on ISO keyboards: KEY_102ND */
	{ 0x57U, SCAN_PLAIN, 87U },		/* F11: KEY_F11 */
	{ 0x58U, SCAN_PLAIN, 88U },		/* F12: KEY_F12 */
	{ 0x59U, SCAN_PLAIN, 117U },		/* keypad equals: KEY_KPEQUAL */
	{ 0x5CU, SCAN_PLAIN, 95U },		/* International 6 (keypad comma of Japan): KEY_KPJPCOMMA */
	{ 0x64U, SCAN_PLAIN, 183U },		/* F13: KEY_F13 */
	{ 0x65U, SCAN_PLAIN, 184U },		/* F14: KEY_F14 */
	{ 0x66U, SCAN_PLAIN, 185U },		/* F15: KEY_F15 */
	{ 0x67U, SCAN_PLAIN, 186U },		/* F16: KEY_F16 */
	{ 0x68U, SCAN_PLAIN, 187U },		/* F17: KEY_F17 */
	{ 0x69U, SCAN_PLAIN, 188U },		/* F18: KEY_F18 */
	{ 0x6AU, SCAN_PLAIN, 189U },		/* F19: KEY_F19 */
	{ 0x6BU, SCAN_PLAIN, 190U },		/* F20: KEY_F20 */
	{ 0x6CU, SCAN_PLAIN, 191U },		/* F21: KEY_F21 */
	{ 0x6DU, SCAN_PLAIN, 192U },		/* F22: KEY_F22 */
	{ 0x6EU, SCAN_PLAIN, 193U },		/* F23: KEY_F23 */
	{ 0x70U, SCAN_PLAIN, 93U },		/* Katakana/Hiragana: KEY_KATAKANAHIRAGANA */
	{ 0x71U, SCAN_EITHER, 123U },		/* Hanja: KEY_HANJA */
	{ 0x72U, SCAN_EITHER, 122U },		/* Hangul: KEY_HANGEUL */
	{ 0x73U, SCAN_PLAIN, 89U },		/* Ro (backslash and underscore of Japan): KEY_RO */
	{ 0x76U, SCAN_PLAIN, 194U },		/* F24: KEY_F24 */
	{ 0x77U, SCAN_PLAIN, 91U },		/* LANG4 (Hiragana): KEY_HIRAGANA */
	{ 0x78U, SCAN_PLAIN, 90U },		/* LANG3 (Katakana): KEY_KATAKANA */
	{ 0x79U, SCAN_PLAIN, 92U },		/* Henkan: KEY_HENKAN */
	{ 0x7BU, SCAN_PLAIN, 94U },		/* Muhenkan: KEY_MUHENKAN */
	{ 0x7DU, SCAN_PLAIN, 124U },		/* Yen: KEY_YEN */
	{ 0x7EU, SCAN_PLAIN, 121U },		/* keypad comma (Brazil): KEY_KPCOMMA */
	{ 0xF1U, SCAN_EITHER, 123U },		/* Hanja, the other report: KEY_HANJA */
	{ 0xF2U, SCAN_EITHER, 122U },		/* Hangul, the other report: KEY_HANGEUL */

	/* The keys sent with the prefix. */
	{ 0x10U, SCAN_EXTENDED, 165U },		/* previous track: KEY_PREVIOUSSONG */
	{ 0x19U, SCAN_EXTENDED, 163U },		/* next track: KEY_NEXTSONG */
	{ 0x1CU, SCAN_EXTENDED, 96U },		/* keypad Enter: KEY_KPENTER */
	{ 0x1DU, SCAN_EXTENDED, 97U },		/* right Ctrl: KEY_RIGHTCTRL */
	{ 0x20U, SCAN_EXTENDED, 113U },		/* mute: KEY_MUTE */
	{ 0x21U, SCAN_EXTENDED, 140U },		/* calculator: KEY_CALC */
	{ 0x22U, SCAN_EXTENDED, 164U },		/* play and pause: KEY_PLAYPAUSE */
	{ 0x24U, SCAN_EXTENDED, 166U },		/* stop: KEY_STOPCD */
	{ 0x2EU, SCAN_EXTENDED, 114U },		/* volume down: KEY_VOLUMEDOWN */
	{ 0x30U, SCAN_EXTENDED, 115U },		/* volume up: KEY_VOLUMEUP */
	{ 0x32U, SCAN_EXTENDED, 172U },		/* browser home: KEY_HOMEPAGE */
	{ 0x35U, SCAN_EXTENDED, 98U },		/* keypad slash: KEY_KPSLASH */
	{ 0x37U, SCAN_EXTENDED, 99U },		/* Print Screen: KEY_SYSRQ */
	{ 0x38U, SCAN_EXTENDED, 100U },		/* right Alt (AltGr): KEY_RIGHTALT */
	{ 0x46U, SCAN_EXTENDED, 119U },		/* Ctrl+Pause (Break): KEY_PAUSE */
	{ 0x47U, SCAN_EXTENDED, 102U },		/* Home: KEY_HOME */
	{ 0x48U, SCAN_EXTENDED, 103U },		/* up arrow: KEY_UP */
	{ 0x49U, SCAN_EXTENDED, 104U },		/* Page Up: KEY_PAGEUP */
	{ 0x4BU, SCAN_EXTENDED, 105U },		/* left arrow: KEY_LEFT */
	{ 0x4DU, SCAN_EXTENDED, 106U },		/* right arrow: KEY_RIGHT */
	{ 0x4FU, SCAN_EXTENDED, 107U },		/* End: KEY_END */
	{ 0x50U, SCAN_EXTENDED, 108U },		/* down arrow: KEY_DOWN */
	{ 0x51U, SCAN_EXTENDED, 109U },		/* Page Down: KEY_PAGEDOWN */
	{ 0x52U, SCAN_EXTENDED, 110U },		/* Insert: KEY_INSERT */
	{ 0x53U, SCAN_EXTENDED, 111U },		/* Delete: KEY_DELETE */
	{ 0x5BU, SCAN_EXTENDED, 125U },		/* left Windows key: KEY_LEFTMETA */
	{ 0x5CU, SCAN_EXTENDED, 126U },		/* right Windows key: KEY_RIGHTMETA */
	{ 0x5DU, SCAN_EXTENDED, 127U },		/* application (menu) key: KEY_COMPOSE */
	{ 0x5EU, SCAN_EXTENDED, 116U },		/* power: KEY_POWER */
	{ 0x5FU, SCAN_EXTENDED, 142U },		/* sleep: KEY_SLEEP */
	{ 0x63U, SCAN_EXTENDED, 143U },		/* wake: KEY_WAKEUP */
	{ 0x65U, SCAN_EXTENDED, 217U },		/* browser search: KEY_SEARCH */
	{ 0x66U, SCAN_EXTENDED, 156U },		/* browser favorites: KEY_BOOKMARKS */
	{ 0x67U, SCAN_EXTENDED, 173U },		/* browser refresh: KEY_REFRESH */
	{ 0x68U, SCAN_EXTENDED, 128U },		/* browser stop: KEY_STOP */
	{ 0x69U, SCAN_EXTENDED, 159U },		/* browser forward: KEY_FORWARD */
	{ 0x6AU, SCAN_EXTENDED, 158U },		/* browser back: KEY_BACK */
	{ 0x6BU, SCAN_EXTENDED, 144U },		/* my computer (the local machine browser): KEY_FILE */
	{ 0x6CU, SCAN_EXTENDED, 155U },		/* mail: KEY_MAIL */
	{ 0x6DU, SCAN_EXTENDED, 171U },		/* media select (the consumer control configuration): KEY_CONFIG */
};

/* The number of rows of scan_rows. */
#define SCAN_ROW_COUNT	(sizeof(scan_rows) / sizeof(scan_rows[0]))

static int scan_row_matches(const struct scan_row *row, unsigned scan, int extended);

/*
 * Gives the evdev code of the key a Windows key message names.
 *
 * scan is lParam bits 16 to 23, extended lParam bit 24 (any nonzero value
 * counts as set), virtual_key the wParam, read only for scan code 0x45:
 * Windows reports Pause as 0x45 with the virtual key VK_PAUSE, and Num Lock
 * as 0x45 too.  Returns the evdev code, or 0 for a key the table does not
 * know (and for scan code 0, which synthesized messages carry).
 */
uint32_t
x11_win32_scan_to_evdev(
	unsigned scan,
	int extended,
	unsigned virtual_key)
{
	size_t i;
	int matches;

	/* Tells Pause from Num Lock, which share their make code. */
	if (scan == SCAN_NUMLOCK_PAUSE) {
		if (virtual_key == X11_WIN32_VK_PAUSE)
			return SCAN_EVDEV_PAUSE;

		return SCAN_EVDEV_NUMLOCK;
	}

	/* Gives a key of the identity block its own make code. */
	if (extended == 0 && scan >= SCAN_IDENTITY_FIRST && scan <= SCAN_IDENTITY_LAST)
		return scan;

	/* Looks the make code up among the other keys. */
	for (i = 0; i < SCAN_ROW_COUNT; i++) {
		matches = scan_row_matches(&scan_rows[i], scan, extended);
		if (matches)
			return scan_rows[i].code;
	}

	/* No key of the table: the message is dropped. */
	return 0;
}

/*
 * Gives the scan code and extended bit of a key named by its evdev code.
 *
 * The backend asks the keyboard layout what a key types by its scan code,
 * so it needs the way back.  Where a key has two reports, the first row's
 * is given (the one without the prefix).  Returns 0 with *scan and
 * *extended filled, or -1 for an evdev code no scan code gives.
 */
int
x11_win32_evdev_to_scan(
	uint32_t code,
	unsigned *scan,
	int *extended)
{
	size_t i;

	/* Gives Num Lock and Pause the make code they share. */
	if (code == SCAN_EVDEV_NUMLOCK || code == SCAN_EVDEV_PAUSE) {
		*scan = SCAN_NUMLOCK_PAUSE;
		*extended = 0;
		return 0;
	}

	/* Gives a key of the identity block its own number. */
	if (code >= SCAN_IDENTITY_FIRST && code <= SCAN_IDENTITY_LAST) {
		*scan = code;
		*extended = 0;
		return 0;
	}

	/* Looks for the first row of the key. */
	for (i = 0; i < SCAN_ROW_COUNT; i++) {
		if (scan_rows[i].code != code)
			continue;

		*scan = scan_rows[i].scan;
		*extended = 0;
		if (scan_rows[i].extended == SCAN_EXTENDED)
			*extended = 1;
		return 0;
	}

	/* No scan code gives the key. */
	return -1;
}

/* Tells whether a row names the key of a make code and extended bit. */
static int
scan_row_matches(
	const struct scan_row *row,
	unsigned scan,
	int extended)
{
	/* A row of another make code does not match. */
	if (row->scan != scan)
		return 0;

	/* A row that takes either report matches. */
	if (row->extended == SCAN_EITHER)
		return 1;

	/* A row of the prefixed key matches a message with the extended bit. */
	if (row->extended == SCAN_EXTENDED) {
		if (extended != 0)
			return 1;

		return 0;
	}

	/* A row of the plain key matches a message without it. */
	if (extended == 0)
		return 1;

	return 0;
}
