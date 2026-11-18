/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The keys of the Windows backend: scan code set 1 to evdev codes.
 */

#include "userland/desktop/libkeiland/win32/keys-table.h"

#include <stddef.h>
#include <stdint.h>

/*
 * How a table row treats the extended bit of the key message: it matches
 * only a key sent without the 0xE0 prefix, only a key sent with it, or a
 * key either way (the keys whose reports differ between keyboards).
 */
#define KEYS_PLAIN	0U
#define KEYS_EXT	1U
#define KEYS_EITHER	2U

/*
 * The evdev codes of the two keys that share scan code 0x45, which the
 * function decides by the virtual-key code (see keys-table.h).
 */
#define KEYS_EVDEV_NUMLOCK	69U
#define KEYS_EVDEV_PAUSE	119U

/*
 * One scan code of scan code set 1 and the evdev code of its key.
 *
 * scan is the make code without the 0xE0 prefix, extended says how the
 * prefix has to match, and code is the evdev code (1 to 0x2ff).  Rows are
 * constant for the life of the program.
 */
struct keys_row {
	uint8_t scan;
	uint8_t extended;
	uint16_t code;
};

/*
 * The scan codes the table knows, by the HID usage each belongs to.
 *
 * Scan code 0x45 is not here: it is Num Lock and Pause, told apart by the
 * virtual-key code.  LANG5 (usage 0x94) has the make code of F24 (0x76) and
 * F24 takes it.  Where two usages share a make code (the two backslash
 * usages, 0x2B; Keyboard Power and System Power Down, 0xE0 0x5E) they map
 * to one evdev code and appear once.  The table is only read, so the
 * function is pure.
 *
 * The first block is the Keyboard/Keypad page, the second the Generic
 * Desktop system controls, the third the Consumer page.  The comment of a
 * row gives the HID usage (hexadecimal, of its page), its name and the
 * evdev name of the code.
 */
static const struct keys_row key_rows[] = {
	/*
	 * The Keyboard/Keypad page (0x07).
	 *
	 * Values referenced: Microsoft Learn, Keyboard Input Overview, Scan
	 * codes table (retrieved 2026-11-01), column "Scan 1 Make" for each
	 * usage and Note 1 to Note 6; Microsoft Keyboard Scan Code
	 * Specification 1.3a, Scan Code Table (keys by location); USB HID
	 * Usage Tables 1.5, section 10 (usage names); Linux v6.12
	 * drivers/hid/hid-input.c, the usage to evdev code of this page
	 * (values only); Linux v6.12 include/uapi/linux/input-event-codes.h
	 * (the numbers behind the KEY_ names).
	 *
	 * Rows of KEYS_EITHER: 0x54 (Print Screen with Alt, sent without the
	 * prefix) and Hangul 0x72 and 0xF2, Hanja 0x71 and 0xF1, which
	 * keyboards send with or without the prefix.
	 */
	{ 0x1EU, KEYS_PLAIN, 30U },	/* 0x04 A: KEY_A */
	{ 0x30U, KEYS_PLAIN, 48U },	/* 0x05 B: KEY_B */
	{ 0x2EU, KEYS_PLAIN, 46U },	/* 0x06 C: KEY_C */
	{ 0x20U, KEYS_PLAIN, 32U },	/* 0x07 D: KEY_D */
	{ 0x12U, KEYS_PLAIN, 18U },	/* 0x08 E: KEY_E */
	{ 0x21U, KEYS_PLAIN, 33U },	/* 0x09 F: KEY_F */
	{ 0x22U, KEYS_PLAIN, 34U },	/* 0x0A G: KEY_G */
	{ 0x23U, KEYS_PLAIN, 35U },	/* 0x0B H: KEY_H */
	{ 0x17U, KEYS_PLAIN, 23U },	/* 0x0C I: KEY_I */
	{ 0x24U, KEYS_PLAIN, 36U },	/* 0x0D J: KEY_J */
	{ 0x25U, KEYS_PLAIN, 37U },	/* 0x0E K: KEY_K */
	{ 0x26U, KEYS_PLAIN, 38U },	/* 0x0F L: KEY_L */
	{ 0x32U, KEYS_PLAIN, 50U },	/* 0x10 M: KEY_M */
	{ 0x31U, KEYS_PLAIN, 49U },	/* 0x11 N: KEY_N */
	{ 0x18U, KEYS_PLAIN, 24U },	/* 0x12 O: KEY_O */
	{ 0x19U, KEYS_PLAIN, 25U },	/* 0x13 P: KEY_P */
	{ 0x10U, KEYS_PLAIN, 16U },	/* 0x14 Q: KEY_Q */
	{ 0x13U, KEYS_PLAIN, 19U },	/* 0x15 R: KEY_R */
	{ 0x1FU, KEYS_PLAIN, 31U },	/* 0x16 S: KEY_S */
	{ 0x14U, KEYS_PLAIN, 20U },	/* 0x17 T: KEY_T */
	{ 0x16U, KEYS_PLAIN, 22U },	/* 0x18 U: KEY_U */
	{ 0x2FU, KEYS_PLAIN, 47U },	/* 0x19 V: KEY_V */
	{ 0x11U, KEYS_PLAIN, 17U },	/* 0x1A W: KEY_W */
	{ 0x2DU, KEYS_PLAIN, 45U },	/* 0x1B X: KEY_X */
	{ 0x15U, KEYS_PLAIN, 21U },	/* 0x1C Y: KEY_Y */
	{ 0x2CU, KEYS_PLAIN, 44U },	/* 0x1D Z: KEY_Z */
	{ 0x02U, KEYS_PLAIN, 2U },	/* 0x1E 1 and Bang: KEY_1 */
	{ 0x03U, KEYS_PLAIN, 3U },	/* 0x1F 2 and At: KEY_2 */
	{ 0x04U, KEYS_PLAIN, 4U },	/* 0x20 3 And Hash: KEY_3 */
	{ 0x05U, KEYS_PLAIN, 5U },	/* 0x21 4 and Dollar: KEY_4 */
	{ 0x06U, KEYS_PLAIN, 6U },	/* 0x22 5 and Percent: KEY_5 */
	{ 0x07U, KEYS_PLAIN, 7U },	/* 0x23 6 and Caret: KEY_6 */
	{ 0x08U, KEYS_PLAIN, 8U },	/* 0x24 7 and Ampersand: KEY_7 */
	{ 0x09U, KEYS_PLAIN, 9U },	/* 0x25 8 and Star: KEY_8 */
	{ 0x0AU, KEYS_PLAIN, 10U },	/* 0x26 9 and Left Bracket: KEY_9 */
	{ 0x0BU, KEYS_PLAIN, 11U },	/* 0x27 0 and Right Bracket: KEY_0 */
	{ 0x1CU, KEYS_PLAIN, 28U },	/* 0x28 Return Enter: KEY_ENTER */
	{ 0x01U, KEYS_PLAIN, 1U },	/* 0x29 Escape: KEY_ESC */
	{ 0x0EU, KEYS_PLAIN, 14U },	/* 0x2A Delete: KEY_BACKSPACE */
	{ 0x0FU, KEYS_PLAIN, 15U },	/* 0x2B Tab: KEY_TAB */
	{ 0x39U, KEYS_PLAIN, 57U },	/* 0x2C Spacebar: KEY_SPACE */
	{ 0x0CU, KEYS_PLAIN, 12U },	/* 0x2D Dash and Underscore: KEY_MINUS */
	{ 0x0DU, KEYS_PLAIN, 13U },	/* 0x2E Equals and Plus: KEY_EQUAL */
	{ 0x1AU, KEYS_PLAIN, 26U },	/* 0x2F Left Brace: KEY_LEFTBRACE */
	{ 0x1BU, KEYS_PLAIN, 27U },	/* 0x30 Right Brace: KEY_RIGHTBRACE */
	{ 0x2BU, KEYS_PLAIN, 43U },	/* 0x31 Backslash and Pipe: KEY_BACKSLASH */
	{ 0x27U, KEYS_PLAIN, 39U },	/* 0x33 SemiColon and Colon: KEY_SEMICOLON */
	{ 0x28U, KEYS_PLAIN, 40U },	/* 0x34 Apostrophe and Double Quotation Mark: KEY_APOSTROPHE */
	{ 0x29U, KEYS_PLAIN, 41U },	/* 0x35 Grave Accent and Tilde: KEY_GRAVE */
	{ 0x33U, KEYS_PLAIN, 51U },	/* 0x36 Comma and LessThan: KEY_COMMA */
	{ 0x34U, KEYS_PLAIN, 52U },	/* 0x37 Period and GreaterThan: KEY_DOT */
	{ 0x35U, KEYS_PLAIN, 53U },	/* 0x38 ForwardSlash and QuestionMark: KEY_SLASH */
	{ 0x3AU, KEYS_PLAIN, 58U },	/* 0x39 Caps Lock: KEY_CAPSLOCK */
	{ 0x3BU, KEYS_PLAIN, 59U },	/* 0x3A F1: KEY_F1 */
	{ 0x3CU, KEYS_PLAIN, 60U },	/* 0x3B F2: KEY_F2 */
	{ 0x3DU, KEYS_PLAIN, 61U },	/* 0x3C F3: KEY_F3 */
	{ 0x3EU, KEYS_PLAIN, 62U },	/* 0x3D F4: KEY_F4 */
	{ 0x3FU, KEYS_PLAIN, 63U },	/* 0x3E F5: KEY_F5 */
	{ 0x40U, KEYS_PLAIN, 64U },	/* 0x3F F6: KEY_F6 */
	{ 0x41U, KEYS_PLAIN, 65U },	/* 0x40 F7: KEY_F7 */
	{ 0x42U, KEYS_PLAIN, 66U },	/* 0x41 F8: KEY_F8 */
	{ 0x43U, KEYS_PLAIN, 67U },	/* 0x42 F9: KEY_F9 */
	{ 0x44U, KEYS_PLAIN, 68U },	/* 0x43 F10: KEY_F10 */
	{ 0x57U, KEYS_PLAIN, 87U },	/* 0x44 F11: KEY_F11 */
	{ 0x58U, KEYS_PLAIN, 88U },	/* 0x45 F12: KEY_F12 */
	{ 0x37U, KEYS_EXT, 99U },	/* 0x46 PrintScreen: KEY_SYSRQ */
	{ 0x54U, KEYS_EITHER, 99U },	/* 0x46 PrintScreen: KEY_SYSRQ */
	{ 0x46U, KEYS_PLAIN, 70U },	/* 0x47 Scroll Lock: KEY_SCROLLLOCK */
	{ 0x46U, KEYS_EXT, 119U },	/* 0x48 Pause: KEY_PAUSE */
	{ 0x52U, KEYS_EXT, 110U },	/* 0x49 Insert: KEY_INSERT */
	{ 0x47U, KEYS_EXT, 102U },	/* 0x4A Home: KEY_HOME */
	{ 0x49U, KEYS_EXT, 104U },	/* 0x4B PageUp: KEY_PAGEUP */
	{ 0x53U, KEYS_EXT, 111U },	/* 0x4C Delete Forward: KEY_DELETE */
	{ 0x4FU, KEYS_EXT, 107U },	/* 0x4D End: KEY_END */
	{ 0x51U, KEYS_EXT, 109U },	/* 0x4E PageDown: KEY_PAGEDOWN */
	{ 0x4DU, KEYS_EXT, 106U },	/* 0x4F RightArrow: KEY_RIGHT */
	{ 0x4BU, KEYS_EXT, 105U },	/* 0x50 LeftArrow: KEY_LEFT */
	{ 0x50U, KEYS_EXT, 108U },	/* 0x51 DownArrow: KEY_DOWN */
	{ 0x48U, KEYS_EXT, 103U },	/* 0x52 UpArrow: KEY_UP */
	{ 0x35U, KEYS_EXT, 98U },	/* 0x54 Keypad Forward Slash: KEY_KPSLASH */
	{ 0x37U, KEYS_PLAIN, 55U },	/* 0x55 Keypad Star: KEY_KPASTERISK */
	{ 0x4AU, KEYS_PLAIN, 74U },	/* 0x56 Keypad Dash: KEY_KPMINUS */
	{ 0x4EU, KEYS_PLAIN, 78U },	/* 0x57 Keypad Plus: KEY_KPPLUS */
	{ 0x1CU, KEYS_EXT, 96U },	/* 0x58 Keypad ENTER: KEY_KPENTER */
	{ 0x4FU, KEYS_PLAIN, 79U },	/* 0x59 Keypad 1 and End: KEY_KP1 */
	{ 0x50U, KEYS_PLAIN, 80U },	/* 0x5A Keypad 2 and Down Arrow: KEY_KP2 */
	{ 0x51U, KEYS_PLAIN, 81U },	/* 0x5B Keypad 3 and PageDn: KEY_KP3 */
	{ 0x4BU, KEYS_PLAIN, 75U },	/* 0x5C Keypad 4 and Left Arrow: KEY_KP4 */
	{ 0x4CU, KEYS_PLAIN, 76U },	/* 0x5D Keypad 5: KEY_KP5 */
	{ 0x4DU, KEYS_PLAIN, 77U },	/* 0x5E Keypad 6 and Right Arrow: KEY_KP6 */
	{ 0x47U, KEYS_PLAIN, 71U },	/* 0x5F Keypad 7 and Home: KEY_KP7 */
	{ 0x48U, KEYS_PLAIN, 72U },	/* 0x60 Keypad 8 and Up Arrow: KEY_KP8 */
	{ 0x49U, KEYS_PLAIN, 73U },	/* 0x61 Keypad 9 and PageUp: KEY_KP9 */
	{ 0x52U, KEYS_PLAIN, 82U },	/* 0x62 Keypad 0 and Insert: KEY_KP0 */
	{ 0x53U, KEYS_PLAIN, 83U },	/* 0x63 Keypad Period and Delete: KEY_KPDOT */
	{ 0x56U, KEYS_PLAIN, 86U },	/* 0x64 Non-US Backslash and Pipe: KEY_102ND */
	{ 0x5DU, KEYS_EXT, 127U },	/* 0x65 Application: KEY_COMPOSE */
	{ 0x59U, KEYS_PLAIN, 117U },	/* 0x67 Keypad Equals: KEY_KPEQUAL */
	{ 0x64U, KEYS_PLAIN, 183U },	/* 0x68 F13: KEY_F13 */
	{ 0x65U, KEYS_PLAIN, 184U },	/* 0x69 F14: KEY_F14 */
	{ 0x66U, KEYS_PLAIN, 185U },	/* 0x6A F15: KEY_F15 */
	{ 0x67U, KEYS_PLAIN, 186U },	/* 0x6B F16: KEY_F16 */
	{ 0x68U, KEYS_PLAIN, 187U },	/* 0x6C F17: KEY_F17 */
	{ 0x69U, KEYS_PLAIN, 188U },	/* 0x6D F18: KEY_F18 */
	{ 0x6AU, KEYS_PLAIN, 189U },	/* 0x6E F19: KEY_F19 */
	{ 0x6BU, KEYS_PLAIN, 190U },	/* 0x6F F20: KEY_F20 */
	{ 0x6CU, KEYS_PLAIN, 191U },	/* 0x70 F21: KEY_F21 */
	{ 0x6DU, KEYS_PLAIN, 192U },	/* 0x71 F22: KEY_F22 */
	{ 0x6EU, KEYS_PLAIN, 193U },	/* 0x72 F23: KEY_F23 */
	{ 0x76U, KEYS_PLAIN, 194U },	/* 0x73 F24: KEY_F24 */
	{ 0x7EU, KEYS_PLAIN, 121U },	/* 0x85 Keypad Comma: KEY_KPCOMMA */
	{ 0x73U, KEYS_PLAIN, 89U },	/* 0x87 International1: KEY_RO */
	{ 0x70U, KEYS_PLAIN, 93U },	/* 0x88 International2: KEY_KATAKANAHIRAGANA */
	{ 0x7DU, KEYS_PLAIN, 124U },	/* 0x89 International3: KEY_YEN */
	{ 0x79U, KEYS_PLAIN, 92U },	/* 0x8A International4: KEY_HENKAN */
	{ 0x7BU, KEYS_PLAIN, 94U },	/* 0x8B International5: KEY_MUHENKAN */
	{ 0x5CU, KEYS_PLAIN, 95U },	/* 0x8C International6: KEY_KPJPCOMMA */
	{ 0x72U, KEYS_EITHER, 122U },	/* 0x90 LANG1: KEY_HANGEUL */
	{ 0xF2U, KEYS_EITHER, 122U },	/* 0x90 LANG1: KEY_HANGEUL */
	{ 0x71U, KEYS_EITHER, 123U },	/* 0x91 LANG2: KEY_HANJA */
	{ 0xF1U, KEYS_EITHER, 123U },	/* 0x91 LANG2: KEY_HANJA */
	{ 0x78U, KEYS_PLAIN, 90U },	/* 0x92 LANG3: KEY_KATAKANA */
	{ 0x77U, KEYS_PLAIN, 91U },	/* 0x93 LANG4: KEY_HIRAGANA */
	{ 0x1DU, KEYS_PLAIN, 29U },	/* 0xE0 LeftControl: KEY_LEFTCTRL */
	{ 0x2AU, KEYS_PLAIN, 42U },	/* 0xE1 LeftShift: KEY_LEFTSHIFT */
	{ 0x38U, KEYS_PLAIN, 56U },	/* 0xE2 LeftAlt: KEY_LEFTALT */
	{ 0x5BU, KEYS_EXT, 125U },	/* 0xE3 Left GUI: KEY_LEFTMETA */
	{ 0x1DU, KEYS_EXT, 97U },	/* 0xE4 RightControl: KEY_RIGHTCTRL */
	{ 0x36U, KEYS_PLAIN, 54U },	/* 0xE5 RightShift: KEY_RIGHTSHIFT */
	{ 0x38U, KEYS_EXT, 100U },	/* 0xE6 RightAlt: KEY_RIGHTALT */
	{ 0x5CU, KEYS_EXT, 126U },	/* 0xE7 Right GUI: KEY_RIGHTMETA */

	/*
	 * The Generic Desktop page (0x01), system controls.
	 *
	 * Values referenced: Microsoft Learn, Keyboard Input Overview, Scan
	 * codes table (retrieved 2026-11-01); Microsoft Keyboard Scan Code
	 * Specification 1.3a, ACPI Power Management Keys (set 1); USB HID
	 * Usage Tables 1.5, section 4 (usage names); Linux v6.12
	 * drivers/hid/hid-input.c, the system control usages (values only).
	 */
	{ 0x5EU, KEYS_EXT, 116U },	/* 0x81 System Power Down: KEY_POWER */
	{ 0x5FU, KEYS_EXT, 142U },	/* 0x82 System Sleep: KEY_SLEEP */
	{ 0x63U, KEYS_EXT, 143U },	/* 0x83 System Wake Up: KEY_WAKEUP */

	/*
	 * The Consumer page (0x0C), media and application keys.
	 *
	 * Values referenced: Microsoft Learn, Keyboard Input Overview, Scan
	 * codes table (retrieved 2026-11-01); USB HID Usage Tables 1.5,
	 * section 15 (usage names); Linux v6.12 drivers/hid/hid-input.c, the
	 * consumer usages (values only).
	 */
	{ 0x19U, KEYS_EXT, 163U },	/* 0xB5 Scan Next Track: KEY_NEXTSONG */
	{ 0x10U, KEYS_EXT, 165U },	/* 0xB6 Scan Previous Track: KEY_PREVIOUSSONG */
	{ 0x24U, KEYS_EXT, 166U },	/* 0xB7 Stop: KEY_STOPCD */
	{ 0x22U, KEYS_EXT, 164U },	/* 0xCD Play/Pause: KEY_PLAYPAUSE */
	{ 0x20U, KEYS_EXT, 113U },	/* 0xE2 Mute: KEY_MUTE */
	{ 0x30U, KEYS_EXT, 115U },	/* 0xE9 Volume Increment: KEY_VOLUMEUP */
	{ 0x2EU, KEYS_EXT, 114U },	/* 0xEA Volume Decrement: KEY_VOLUMEDOWN */
	{ 0x6DU, KEYS_EXT, 171U },	/* 0x183 AL Consumer Control Configuration: KEY_CONFIG */
	{ 0x6CU, KEYS_EXT, 155U },	/* 0x18A AL Email Reader: KEY_MAIL */
	{ 0x21U, KEYS_EXT, 140U },	/* 0x192 AL Calculator: KEY_CALC */
	{ 0x6BU, KEYS_EXT, 144U },	/* 0x194 AL Local Machine Browser: KEY_FILE */
	{ 0x65U, KEYS_EXT, 217U },	/* 0x221 AC Search: KEY_SEARCH */
	{ 0x32U, KEYS_EXT, 172U },	/* 0x223 AC Home: KEY_HOMEPAGE */
	{ 0x6AU, KEYS_EXT, 158U },	/* 0x224 AC Back: KEY_BACK */
	{ 0x69U, KEYS_EXT, 159U },	/* 0x225 AC Forward: KEY_FORWARD */
	{ 0x68U, KEYS_EXT, 128U },	/* 0x226 AC Stop: KEY_STOP */
	{ 0x67U, KEYS_EXT, 173U },	/* 0x227 AC Refresh: KEY_REFRESH */
	{ 0x66U, KEYS_EXT, 156U },	/* 0x22A AC Bookmarks: KEY_BOOKMARKS */
};

/* The number of rows of the table. */
#define KEYS_ROW_COUNT	(sizeof(key_rows) / sizeof(key_rows[0]))

/*
 * Gives the evdev code of the key a key message names.
 *
 * A scan code of 0 or above 255 names no key and gives 0, as does a scan
 * code the table does not hold.  Scan code 0x45 is Pause when the virtual
 * key says so and Num Lock otherwise; every other key is found in the
 * table by its scan code and the extended bit.
 */
uint32_t
keiui_win32_key_code(
	unsigned scan,
	int extended,
	unsigned virtual_key)
{
	size_t index;
	unsigned prefix;
	const struct keys_row *row;

	/* Refuses a scan code that no key message carries: 0 is none, above 255 is a caller's mistake. */
	if (scan == 0U)
		return 0U;
	if (scan > 0xFFU)
		return 0U;

	/* Reduces the extended bit to the two values the table uses. */
	prefix = KEYS_PLAIN;
	if (extended != 0)
		prefix = KEYS_EXT;

	/*
	 * Windows reports Pause as scan code 0x45 without the extended bit and
	 * Num Lock as 0x45 with it, so the bit alone does not say which key it
	 * is; the virtual-key code decides, whatever the bit.
	 */
	if (scan == 0x45U) {
		if (virtual_key == KEIUI_WIN32_VK_PAUSE)
			return KEYS_EVDEV_PAUSE;

		/* Any other virtual key on 0x45 is Num Lock. */
		return KEYS_EVDEV_NUMLOCK;
	}

	/* Looks for the row of this scan code whose extended bit matches. */
	for (index = 0U;
	     index < KEYS_ROW_COUNT;
	     index++) {
		row = &key_rows[index];

		/* Skips the rows of other scan codes. */
		if (row->scan != scan)
			continue;

		/* A row for either prefix matches whatever the message says. */
		if (row->extended == KEYS_EITHER)
			return row->code;

		/* A row for one prefix matches only a message with that prefix. */
		if (row->extended == prefix)
			return row->code;
	}

	/* Succeeded: the table has no key for this scan code, so the answer is 0. */
	return 0U;
}
