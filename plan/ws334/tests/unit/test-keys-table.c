/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The unit test of the Windows key table (libkeiland/win32/keys-table.h).
 *
 * The expectations are made in two steps that the implementation does not
 * take.  First, the rows of Microsoft's scan code table (HID usage page,
 * usage, scan code set 1 make code) are copied here as they stand.  Then
 * each usage is turned into an evdev name by its meaning in the USB HID
 * Usage Tables (usage_keys below, written by reading the usage names), and
 * the numbers behind the names come from the evdev header.  Every one of
 * the 512 pairs of a scan code and an extended bit is then compared with
 * the table built from those rows, so a key the rows do not mention must
 * give 0 and a key they do mention must give its code.
 *
 * Values referenced: Microsoft Learn, Keyboard Input Overview, Scan codes
 * table (retrieved 2026-11-01); Microsoft Keyboard Scan Code Specification
 * 1.3a; USB HID Usage Tables 1.5, sections 4, 10 and 15; the numbers of the
 * KEY_ names of Linux v6.12 include/uapi/linux/input-event-codes.h.
 */

#include "check.h"

#include "userland/desktop/libkeiland/win32/keys-table.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The evdev codes the test uses, by the names of the evdev header.  The
 * values are the numbers of that header, listed here so that the test does
 * not depend on any other header.
 */
enum evdev_key {
	KEY_ESC = 1,
	KEY_1 = 2,
	KEY_2 = 3,
	KEY_3 = 4,
	KEY_4 = 5,
	KEY_5 = 6,
	KEY_6 = 7,
	KEY_7 = 8,
	KEY_8 = 9,
	KEY_9 = 10,
	KEY_0 = 11,
	KEY_MINUS = 12,
	KEY_EQUAL = 13,
	KEY_BACKSPACE = 14,
	KEY_TAB = 15,
	KEY_Q = 16,
	KEY_W = 17,
	KEY_E = 18,
	KEY_R = 19,
	KEY_T = 20,
	KEY_Y = 21,
	KEY_U = 22,
	KEY_I = 23,
	KEY_O = 24,
	KEY_P = 25,
	KEY_LEFTBRACE = 26,
	KEY_RIGHTBRACE = 27,
	KEY_ENTER = 28,
	KEY_LEFTCTRL = 29,
	KEY_A = 30,
	KEY_S = 31,
	KEY_D = 32,
	KEY_F = 33,
	KEY_G = 34,
	KEY_H = 35,
	KEY_J = 36,
	KEY_K = 37,
	KEY_L = 38,
	KEY_SEMICOLON = 39,
	KEY_APOSTROPHE = 40,
	KEY_GRAVE = 41,
	KEY_LEFTSHIFT = 42,
	KEY_BACKSLASH = 43,
	KEY_Z = 44,
	KEY_X = 45,
	KEY_C = 46,
	KEY_V = 47,
	KEY_B = 48,
	KEY_N = 49,
	KEY_M = 50,
	KEY_COMMA = 51,
	KEY_DOT = 52,
	KEY_SLASH = 53,
	KEY_RIGHTSHIFT = 54,
	KEY_KPASTERISK = 55,
	KEY_LEFTALT = 56,
	KEY_SPACE = 57,
	KEY_CAPSLOCK = 58,
	KEY_F1 = 59,
	KEY_F2 = 60,
	KEY_F3 = 61,
	KEY_F4 = 62,
	KEY_F5 = 63,
	KEY_F6 = 64,
	KEY_F7 = 65,
	KEY_F8 = 66,
	KEY_F9 = 67,
	KEY_F10 = 68,
	KEY_NUMLOCK = 69,
	KEY_SCROLLLOCK = 70,
	KEY_KP7 = 71,
	KEY_KP8 = 72,
	KEY_KP9 = 73,
	KEY_KPMINUS = 74,
	KEY_KP4 = 75,
	KEY_KP5 = 76,
	KEY_KP6 = 77,
	KEY_KPPLUS = 78,
	KEY_KP1 = 79,
	KEY_KP2 = 80,
	KEY_KP3 = 81,
	KEY_KP0 = 82,
	KEY_KPDOT = 83,
	KEY_102ND = 86,
	KEY_F11 = 87,
	KEY_F12 = 88,
	KEY_RO = 89,
	KEY_KATAKANA = 90,
	KEY_HIRAGANA = 91,
	KEY_HENKAN = 92,
	KEY_KATAKANAHIRAGANA = 93,
	KEY_MUHENKAN = 94,
	KEY_KPJPCOMMA = 95,
	KEY_KPENTER = 96,
	KEY_RIGHTCTRL = 97,
	KEY_KPSLASH = 98,
	KEY_SYSRQ = 99,
	KEY_RIGHTALT = 100,
	KEY_HOME = 102,
	KEY_UP = 103,
	KEY_PAGEUP = 104,
	KEY_LEFT = 105,
	KEY_RIGHT = 106,
	KEY_END = 107,
	KEY_DOWN = 108,
	KEY_PAGEDOWN = 109,
	KEY_INSERT = 110,
	KEY_DELETE = 111,
	KEY_MUTE = 113,
	KEY_VOLUMEDOWN = 114,
	KEY_VOLUMEUP = 115,
	KEY_POWER = 116,
	KEY_KPEQUAL = 117,
	KEY_PAUSE = 119,
	KEY_KPCOMMA = 121,
	KEY_HANGEUL = 122,
	KEY_HANJA = 123,
	KEY_YEN = 124,
	KEY_LEFTMETA = 125,
	KEY_RIGHTMETA = 126,
	KEY_COMPOSE = 127,
	KEY_STOP = 128,
	KEY_CALC = 140,
	KEY_SLEEP = 142,
	KEY_WAKEUP = 143,
	KEY_FILE = 144,
	KEY_MAIL = 155,
	KEY_BOOKMARKS = 156,
	KEY_BACK = 158,
	KEY_FORWARD = 159,
	KEY_NEXTSONG = 163,
	KEY_PLAYPAUSE = 164,
	KEY_PREVIOUSSONG = 165,
	KEY_STOPCD = 166,
	KEY_CONFIG = 171,
	KEY_HOMEPAGE = 172,
	KEY_REFRESH = 173,
	KEY_F13 = 183,
	KEY_F14 = 184,
	KEY_F15 = 185,
	KEY_F16 = 186,
	KEY_F17 = 187,
	KEY_F18 = 188,
	KEY_F19 = 189,
	KEY_F20 = 190,
	KEY_F21 = 191,
	KEY_F22 = 192,
	KEY_F23 = 193,
	KEY_F24 = 194,
	KEY_SEARCH = 217,
};

/* The HID usage pages the table takes keys from. */
#define PAGE_DESKTOP	0x01U
#define PAGE_KEYBOARD	0x07U
#define PAGE_CONSUMER	0x0CU

/* The largest evdev code a Keiland event carries. */
#define EVDEV_MAX	0x2ffU

/* A virtual key that is neither Pause nor Num Lock (the letter A). */
#define VK_OTHER	0x41U

/*
 * One row of Microsoft's scan code table: a key's HID usage and its make
 * code in scan code set 1.  extended is 1 when the row is "0xE0 xx".
 */
struct spec_row {
	unsigned page;
	unsigned usage;
	unsigned scan;
	int extended;
};

/*
 * The usage to key meaning the test reads from the HID usage names: a
 * usage and the evdev name of the key it stands for.
 */
struct usage_key {
	unsigned page;
	unsigned usage;
	unsigned key;
};

/*
 * The rows of Microsoft's scan code table of the Keyboard Input Overview,
 * in the order the table has them, without the ErrorRollOver row (0x00FF)
 * and the E1-prefixed Pause sequence, which no key message carries.  The
 * scan code 0x45 rows (Pause and Num Lock) are here too: the checks skip
 * them, because the virtual key decides that scan code.
 */
static const struct spec_row spec_rows[] = {
	{ PAGE_DESKTOP, 0x81U, 0x5EU, 1 },	/* System Power Down */
	{ PAGE_DESKTOP, 0x82U, 0x5FU, 1 },	/* System Sleep */
	{ PAGE_DESKTOP, 0x83U, 0x63U, 1 },	/* System Wake Up */
	{ PAGE_KEYBOARD, 0x04U, 0x1EU, 0 },	/* A */
	{ PAGE_KEYBOARD, 0x05U, 0x30U, 0 },	/* B */
	{ PAGE_KEYBOARD, 0x06U, 0x2EU, 0 },	/* C */
	{ PAGE_KEYBOARD, 0x07U, 0x20U, 0 },	/* D */
	{ PAGE_KEYBOARD, 0x08U, 0x12U, 0 },	/* E */
	{ PAGE_KEYBOARD, 0x09U, 0x21U, 0 },	/* F */
	{ PAGE_KEYBOARD, 0x0AU, 0x22U, 0 },	/* G */
	{ PAGE_KEYBOARD, 0x0BU, 0x23U, 0 },	/* H */
	{ PAGE_KEYBOARD, 0x0CU, 0x17U, 0 },	/* I */
	{ PAGE_KEYBOARD, 0x0DU, 0x24U, 0 },	/* J */
	{ PAGE_KEYBOARD, 0x0EU, 0x25U, 0 },	/* K */
	{ PAGE_KEYBOARD, 0x0FU, 0x26U, 0 },	/* L */
	{ PAGE_KEYBOARD, 0x10U, 0x32U, 0 },	/* M */
	{ PAGE_KEYBOARD, 0x11U, 0x31U, 0 },	/* N */
	{ PAGE_KEYBOARD, 0x12U, 0x18U, 0 },	/* O */
	{ PAGE_KEYBOARD, 0x13U, 0x19U, 0 },	/* P */
	{ PAGE_KEYBOARD, 0x14U, 0x10U, 0 },	/* Q */
	{ PAGE_KEYBOARD, 0x15U, 0x13U, 0 },	/* R */
	{ PAGE_KEYBOARD, 0x16U, 0x1FU, 0 },	/* S */
	{ PAGE_KEYBOARD, 0x17U, 0x14U, 0 },	/* T */
	{ PAGE_KEYBOARD, 0x18U, 0x16U, 0 },	/* U */
	{ PAGE_KEYBOARD, 0x19U, 0x2FU, 0 },	/* V */
	{ PAGE_KEYBOARD, 0x1AU, 0x11U, 0 },	/* W */
	{ PAGE_KEYBOARD, 0x1BU, 0x2DU, 0 },	/* X */
	{ PAGE_KEYBOARD, 0x1CU, 0x15U, 0 },	/* Y */
	{ PAGE_KEYBOARD, 0x1DU, 0x2CU, 0 },	/* Z */
	{ PAGE_KEYBOARD, 0x1EU, 0x02U, 0 },	/* 1 and Bang */
	{ PAGE_KEYBOARD, 0x1FU, 0x03U, 0 },	/* 2 and At */
	{ PAGE_KEYBOARD, 0x20U, 0x04U, 0 },	/* 3 And Hash */
	{ PAGE_KEYBOARD, 0x21U, 0x05U, 0 },	/* 4 and Dollar */
	{ PAGE_KEYBOARD, 0x22U, 0x06U, 0 },	/* 5 and Percent */
	{ PAGE_KEYBOARD, 0x23U, 0x07U, 0 },	/* 6 and Caret */
	{ PAGE_KEYBOARD, 0x24U, 0x08U, 0 },	/* 7 and Ampersand */
	{ PAGE_KEYBOARD, 0x25U, 0x09U, 0 },	/* 8 and Star */
	{ PAGE_KEYBOARD, 0x26U, 0x0AU, 0 },	/* 9 and Left Bracket */
	{ PAGE_KEYBOARD, 0x27U, 0x0BU, 0 },	/* 0 and Right Bracket */
	{ PAGE_KEYBOARD, 0x28U, 0x1CU, 0 },	/* Return Enter */
	{ PAGE_KEYBOARD, 0x29U, 0x01U, 0 },	/* Escape */
	{ PAGE_KEYBOARD, 0x2AU, 0x0EU, 0 },	/* Delete */
	{ PAGE_KEYBOARD, 0x2BU, 0x0FU, 0 },	/* Tab */
	{ PAGE_KEYBOARD, 0x2CU, 0x39U, 0 },	/* Spacebar */
	{ PAGE_KEYBOARD, 0x2DU, 0x0CU, 0 },	/* Dash and Underscore */
	{ PAGE_KEYBOARD, 0x2EU, 0x0DU, 0 },	/* Equals and Plus */
	{ PAGE_KEYBOARD, 0x2FU, 0x1AU, 0 },	/* Left Brace */
	{ PAGE_KEYBOARD, 0x30U, 0x1BU, 0 },	/* Right Brace */
	{ PAGE_KEYBOARD, 0x31U, 0x2BU, 0 },	/* Backslash and Pipe */
	{ PAGE_KEYBOARD, 0x32U, 0x2BU, 0 },	/* Non-US Hash and Tilde */
	{ PAGE_KEYBOARD, 0x33U, 0x27U, 0 },	/* SemiColon and Colon */
	{ PAGE_KEYBOARD, 0x34U, 0x28U, 0 },	/* Apostrophe and Double Quotation Mark */
	{ PAGE_KEYBOARD, 0x35U, 0x29U, 0 },	/* Grave Accent and Tilde */
	{ PAGE_KEYBOARD, 0x36U, 0x33U, 0 },	/* Comma and LessThan */
	{ PAGE_KEYBOARD, 0x37U, 0x34U, 0 },	/* Period and GreaterThan */
	{ PAGE_KEYBOARD, 0x38U, 0x35U, 0 },	/* ForwardSlash and QuestionMark */
	{ PAGE_KEYBOARD, 0x39U, 0x3AU, 0 },	/* Caps Lock */
	{ PAGE_KEYBOARD, 0x3AU, 0x3BU, 0 },	/* F1 */
	{ PAGE_KEYBOARD, 0x3BU, 0x3CU, 0 },	/* F2 */
	{ PAGE_KEYBOARD, 0x3CU, 0x3DU, 0 },	/* F3 */
	{ PAGE_KEYBOARD, 0x3DU, 0x3EU, 0 },	/* F4 */
	{ PAGE_KEYBOARD, 0x3EU, 0x3FU, 0 },	/* F5 */
	{ PAGE_KEYBOARD, 0x3FU, 0x40U, 0 },	/* F6 */
	{ PAGE_KEYBOARD, 0x40U, 0x41U, 0 },	/* F7 */
	{ PAGE_KEYBOARD, 0x41U, 0x42U, 0 },	/* F8 */
	{ PAGE_KEYBOARD, 0x42U, 0x43U, 0 },	/* F9 */
	{ PAGE_KEYBOARD, 0x43U, 0x44U, 0 },	/* F10 */
	{ PAGE_KEYBOARD, 0x44U, 0x57U, 0 },	/* F11 */
	{ PAGE_KEYBOARD, 0x45U, 0x58U, 0 },	/* F12 */
	{ PAGE_KEYBOARD, 0x46U, 0x37U, 1 },	/* PrintScreen */
	{ PAGE_KEYBOARD, 0x46U, 0x54U, 0 },	/* PrintScreen */
	{ PAGE_KEYBOARD, 0x47U, 0x46U, 0 },	/* Scroll Lock */
	{ PAGE_KEYBOARD, 0x48U, 0x46U, 1 },	/* Pause */
	{ PAGE_KEYBOARD, 0x48U, 0x45U, 0 },	/* Pause */
	{ PAGE_KEYBOARD, 0x49U, 0x52U, 1 },	/* Insert */
	{ PAGE_KEYBOARD, 0x4AU, 0x47U, 1 },	/* Home */
	{ PAGE_KEYBOARD, 0x4BU, 0x49U, 1 },	/* PageUp */
	{ PAGE_KEYBOARD, 0x4CU, 0x53U, 1 },	/* Delete Forward */
	{ PAGE_KEYBOARD, 0x4DU, 0x4FU, 1 },	/* End */
	{ PAGE_KEYBOARD, 0x4EU, 0x51U, 1 },	/* PageDown */
	{ PAGE_KEYBOARD, 0x4FU, 0x4DU, 1 },	/* RightArrow */
	{ PAGE_KEYBOARD, 0x50U, 0x4BU, 1 },	/* LeftArrow */
	{ PAGE_KEYBOARD, 0x51U, 0x50U, 1 },	/* DownArrow */
	{ PAGE_KEYBOARD, 0x52U, 0x48U, 1 },	/* UpArrow */
	{ PAGE_KEYBOARD, 0x53U, 0x45U, 0 },	/* Keypad Num Lock and Clear */
	{ PAGE_KEYBOARD, 0x53U, 0x45U, 1 },	/* Keypad Num Lock and Clear */
	{ PAGE_KEYBOARD, 0x54U, 0x35U, 1 },	/* Keypad Forward Slash */
	{ PAGE_KEYBOARD, 0x55U, 0x37U, 0 },	/* Keypad Star */
	{ PAGE_KEYBOARD, 0x56U, 0x4AU, 0 },	/* Keypad Dash */
	{ PAGE_KEYBOARD, 0x57U, 0x4EU, 0 },	/* Keypad Plus */
	{ PAGE_KEYBOARD, 0x58U, 0x1CU, 1 },	/* Keypad ENTER */
	{ PAGE_KEYBOARD, 0x59U, 0x4FU, 0 },	/* Keypad 1 and End */
	{ PAGE_KEYBOARD, 0x5AU, 0x50U, 0 },	/* Keypad 2 and Down Arrow */
	{ PAGE_KEYBOARD, 0x5BU, 0x51U, 0 },	/* Keypad 3 and PageDn */
	{ PAGE_KEYBOARD, 0x5CU, 0x4BU, 0 },	/* Keypad 4 and Left Arrow */
	{ PAGE_KEYBOARD, 0x5DU, 0x4CU, 0 },	/* Keypad 5 */
	{ PAGE_KEYBOARD, 0x5EU, 0x4DU, 0 },	/* Keypad 6 and Right Arrow */
	{ PAGE_KEYBOARD, 0x5FU, 0x47U, 0 },	/* Keypad 7 and Home */
	{ PAGE_KEYBOARD, 0x60U, 0x48U, 0 },	/* Keypad 8 and Up Arrow */
	{ PAGE_KEYBOARD, 0x61U, 0x49U, 0 },	/* Keypad 9 and PageUp */
	{ PAGE_KEYBOARD, 0x62U, 0x52U, 0 },	/* Keypad 0 and Insert */
	{ PAGE_KEYBOARD, 0x63U, 0x53U, 0 },	/* Keypad Period and Delete */
	{ PAGE_KEYBOARD, 0x64U, 0x56U, 0 },	/* Non-US Backslash and Pipe */
	{ PAGE_KEYBOARD, 0x65U, 0x5DU, 1 },	/* Application */
	{ PAGE_KEYBOARD, 0x66U, 0x5EU, 1 },	/* Power */
	{ PAGE_KEYBOARD, 0x67U, 0x59U, 0 },	/* Keypad Equals */
	{ PAGE_KEYBOARD, 0x68U, 0x64U, 0 },	/* F13 */
	{ PAGE_KEYBOARD, 0x69U, 0x65U, 0 },	/* F14 */
	{ PAGE_KEYBOARD, 0x6AU, 0x66U, 0 },	/* F15 */
	{ PAGE_KEYBOARD, 0x6BU, 0x67U, 0 },	/* F16 */
	{ PAGE_KEYBOARD, 0x6CU, 0x68U, 0 },	/* F17 */
	{ PAGE_KEYBOARD, 0x6DU, 0x69U, 0 },	/* F18 */
	{ PAGE_KEYBOARD, 0x6EU, 0x6AU, 0 },	/* F19 */
	{ PAGE_KEYBOARD, 0x6FU, 0x6BU, 0 },	/* F20 */
	{ PAGE_KEYBOARD, 0x70U, 0x6CU, 0 },	/* F21 */
	{ PAGE_KEYBOARD, 0x71U, 0x6DU, 0 },	/* F22 */
	{ PAGE_KEYBOARD, 0x72U, 0x6EU, 0 },	/* F23 */
	{ PAGE_KEYBOARD, 0x73U, 0x76U, 0 },	/* F24 */
	{ PAGE_KEYBOARD, 0x85U, 0x7EU, 0 },	/* Keypad Comma */
	{ PAGE_KEYBOARD, 0x87U, 0x73U, 0 },	/* International1 */
	{ PAGE_KEYBOARD, 0x88U, 0x70U, 0 },	/* International2 */
	{ PAGE_KEYBOARD, 0x89U, 0x7DU, 0 },	/* International3 */
	{ PAGE_KEYBOARD, 0x8AU, 0x79U, 0 },	/* International4 */
	{ PAGE_KEYBOARD, 0x8BU, 0x7BU, 0 },	/* International5 */
	{ PAGE_KEYBOARD, 0x8CU, 0x5CU, 0 },	/* International6 */
	{ PAGE_KEYBOARD, 0x90U, 0x72U, 0 },	/* LANG1 */
	{ PAGE_KEYBOARD, 0x90U, 0xF2U, 0 },	/* LANG1 */
	{ PAGE_KEYBOARD, 0x91U, 0x71U, 0 },	/* LANG2 */
	{ PAGE_KEYBOARD, 0x91U, 0xF1U, 0 },	/* LANG2 */
	{ PAGE_KEYBOARD, 0x92U, 0x78U, 0 },	/* LANG3 */
	{ PAGE_KEYBOARD, 0x93U, 0x77U, 0 },	/* LANG4 */
	{ PAGE_KEYBOARD, 0x94U, 0x76U, 0 },	/* LANG5 */
	{ PAGE_KEYBOARD, 0xE0U, 0x1DU, 0 },	/* LeftControl */
	{ PAGE_KEYBOARD, 0xE1U, 0x2AU, 0 },	/* LeftShift */
	{ PAGE_KEYBOARD, 0xE2U, 0x38U, 0 },	/* LeftAlt */
	{ PAGE_KEYBOARD, 0xE3U, 0x5BU, 1 },	/* Left GUI */
	{ PAGE_KEYBOARD, 0xE4U, 0x1DU, 1 },	/* RightControl */
	{ PAGE_KEYBOARD, 0xE5U, 0x36U, 0 },	/* RightShift */
	{ PAGE_KEYBOARD, 0xE6U, 0x38U, 1 },	/* RightAlt */
	{ PAGE_KEYBOARD, 0xE7U, 0x5CU, 1 },	/* Right GUI */
	{ PAGE_CONSUMER, 0xB5U, 0x19U, 1 },	/* Scan Next Track */
	{ PAGE_CONSUMER, 0xB6U, 0x10U, 1 },	/* Scan Previous Track */
	{ PAGE_CONSUMER, 0xB7U, 0x24U, 1 },	/* Stop */
	{ PAGE_CONSUMER, 0xCDU, 0x22U, 1 },	/* Play/Pause */
	{ PAGE_CONSUMER, 0xE2U, 0x20U, 1 },	/* Mute */
	{ PAGE_CONSUMER, 0xE9U, 0x30U, 1 },	/* Volume Increment */
	{ PAGE_CONSUMER, 0xEAU, 0x2EU, 1 },	/* Volume Decrement */
	{ PAGE_CONSUMER, 0x183U, 0x6DU, 1 },	/* AL Consumer Control Configuration */
	{ PAGE_CONSUMER, 0x18AU, 0x6CU, 1 },	/* AL Email Reader */
	{ PAGE_CONSUMER, 0x192U, 0x21U, 1 },	/* AL Calculator */
	{ PAGE_CONSUMER, 0x194U, 0x6BU, 1 },	/* AL Local Machine Browser */
	{ PAGE_CONSUMER, 0x221U, 0x65U, 1 },	/* AC Search */
	{ PAGE_CONSUMER, 0x223U, 0x32U, 1 },	/* AC Home */
	{ PAGE_CONSUMER, 0x224U, 0x6AU, 1 },	/* AC Back */
	{ PAGE_CONSUMER, 0x225U, 0x69U, 1 },	/* AC Forward */
	{ PAGE_CONSUMER, 0x226U, 0x68U, 1 },	/* AC Stop */
	{ PAGE_CONSUMER, 0x227U, 0x67U, 1 },	/* AC Refresh */
	{ PAGE_CONSUMER, 0x22AU, 0x66U, 1 },	/* AC Bookmarks */
};

/*
 * What each usage in the rows above stands for, read by hand from the HID
 * usage names and written with the evdev names.  The usage "Keyboard Non-US
 * # and ~" is the same key as the backslash for evdev, as are the two
 * system power usages.
 */
static const struct usage_key usage_keys[] = {
	{ PAGE_DESKTOP, 0x81U, KEY_POWER },
	{ PAGE_DESKTOP, 0x82U, KEY_SLEEP },
	{ PAGE_DESKTOP, 0x83U, KEY_WAKEUP },
	{ PAGE_KEYBOARD, 0x04U, KEY_A },
	{ PAGE_KEYBOARD, 0x05U, KEY_B },
	{ PAGE_KEYBOARD, 0x06U, KEY_C },
	{ PAGE_KEYBOARD, 0x07U, KEY_D },
	{ PAGE_KEYBOARD, 0x08U, KEY_E },
	{ PAGE_KEYBOARD, 0x09U, KEY_F },
	{ PAGE_KEYBOARD, 0x0AU, KEY_G },
	{ PAGE_KEYBOARD, 0x0BU, KEY_H },
	{ PAGE_KEYBOARD, 0x0CU, KEY_I },
	{ PAGE_KEYBOARD, 0x0DU, KEY_J },
	{ PAGE_KEYBOARD, 0x0EU, KEY_K },
	{ PAGE_KEYBOARD, 0x0FU, KEY_L },
	{ PAGE_KEYBOARD, 0x10U, KEY_M },
	{ PAGE_KEYBOARD, 0x11U, KEY_N },
	{ PAGE_KEYBOARD, 0x12U, KEY_O },
	{ PAGE_KEYBOARD, 0x13U, KEY_P },
	{ PAGE_KEYBOARD, 0x14U, KEY_Q },
	{ PAGE_KEYBOARD, 0x15U, KEY_R },
	{ PAGE_KEYBOARD, 0x16U, KEY_S },
	{ PAGE_KEYBOARD, 0x17U, KEY_T },
	{ PAGE_KEYBOARD, 0x18U, KEY_U },
	{ PAGE_KEYBOARD, 0x19U, KEY_V },
	{ PAGE_KEYBOARD, 0x1AU, KEY_W },
	{ PAGE_KEYBOARD, 0x1BU, KEY_X },
	{ PAGE_KEYBOARD, 0x1CU, KEY_Y },
	{ PAGE_KEYBOARD, 0x1DU, KEY_Z },
	{ PAGE_KEYBOARD, 0x1EU, KEY_1 },
	{ PAGE_KEYBOARD, 0x1FU, KEY_2 },
	{ PAGE_KEYBOARD, 0x20U, KEY_3 },
	{ PAGE_KEYBOARD, 0x21U, KEY_4 },
	{ PAGE_KEYBOARD, 0x22U, KEY_5 },
	{ PAGE_KEYBOARD, 0x23U, KEY_6 },
	{ PAGE_KEYBOARD, 0x24U, KEY_7 },
	{ PAGE_KEYBOARD, 0x25U, KEY_8 },
	{ PAGE_KEYBOARD, 0x26U, KEY_9 },
	{ PAGE_KEYBOARD, 0x27U, KEY_0 },
	{ PAGE_KEYBOARD, 0x28U, KEY_ENTER },
	{ PAGE_KEYBOARD, 0x29U, KEY_ESC },
	{ PAGE_KEYBOARD, 0x2AU, KEY_BACKSPACE },
	{ PAGE_KEYBOARD, 0x2BU, KEY_TAB },
	{ PAGE_KEYBOARD, 0x2CU, KEY_SPACE },
	{ PAGE_KEYBOARD, 0x2DU, KEY_MINUS },
	{ PAGE_KEYBOARD, 0x2EU, KEY_EQUAL },
	{ PAGE_KEYBOARD, 0x2FU, KEY_LEFTBRACE },
	{ PAGE_KEYBOARD, 0x30U, KEY_RIGHTBRACE },
	{ PAGE_KEYBOARD, 0x31U, KEY_BACKSLASH },
	{ PAGE_KEYBOARD, 0x32U, KEY_BACKSLASH },
	{ PAGE_KEYBOARD, 0x33U, KEY_SEMICOLON },
	{ PAGE_KEYBOARD, 0x34U, KEY_APOSTROPHE },
	{ PAGE_KEYBOARD, 0x35U, KEY_GRAVE },
	{ PAGE_KEYBOARD, 0x36U, KEY_COMMA },
	{ PAGE_KEYBOARD, 0x37U, KEY_DOT },
	{ PAGE_KEYBOARD, 0x38U, KEY_SLASH },
	{ PAGE_KEYBOARD, 0x39U, KEY_CAPSLOCK },
	{ PAGE_KEYBOARD, 0x3AU, KEY_F1 },
	{ PAGE_KEYBOARD, 0x3BU, KEY_F2 },
	{ PAGE_KEYBOARD, 0x3CU, KEY_F3 },
	{ PAGE_KEYBOARD, 0x3DU, KEY_F4 },
	{ PAGE_KEYBOARD, 0x3EU, KEY_F5 },
	{ PAGE_KEYBOARD, 0x3FU, KEY_F6 },
	{ PAGE_KEYBOARD, 0x40U, KEY_F7 },
	{ PAGE_KEYBOARD, 0x41U, KEY_F8 },
	{ PAGE_KEYBOARD, 0x42U, KEY_F9 },
	{ PAGE_KEYBOARD, 0x43U, KEY_F10 },
	{ PAGE_KEYBOARD, 0x44U, KEY_F11 },
	{ PAGE_KEYBOARD, 0x45U, KEY_F12 },
	{ PAGE_KEYBOARD, 0x46U, KEY_SYSRQ },
	{ PAGE_KEYBOARD, 0x47U, KEY_SCROLLLOCK },
	{ PAGE_KEYBOARD, 0x48U, KEY_PAUSE },
	{ PAGE_KEYBOARD, 0x49U, KEY_INSERT },
	{ PAGE_KEYBOARD, 0x4AU, KEY_HOME },
	{ PAGE_KEYBOARD, 0x4BU, KEY_PAGEUP },
	{ PAGE_KEYBOARD, 0x4CU, KEY_DELETE },
	{ PAGE_KEYBOARD, 0x4DU, KEY_END },
	{ PAGE_KEYBOARD, 0x4EU, KEY_PAGEDOWN },
	{ PAGE_KEYBOARD, 0x4FU, KEY_RIGHT },
	{ PAGE_KEYBOARD, 0x50U, KEY_LEFT },
	{ PAGE_KEYBOARD, 0x51U, KEY_DOWN },
	{ PAGE_KEYBOARD, 0x52U, KEY_UP },
	{ PAGE_KEYBOARD, 0x53U, KEY_NUMLOCK },
	{ PAGE_KEYBOARD, 0x54U, KEY_KPSLASH },
	{ PAGE_KEYBOARD, 0x55U, KEY_KPASTERISK },
	{ PAGE_KEYBOARD, 0x56U, KEY_KPMINUS },
	{ PAGE_KEYBOARD, 0x57U, KEY_KPPLUS },
	{ PAGE_KEYBOARD, 0x58U, KEY_KPENTER },
	{ PAGE_KEYBOARD, 0x59U, KEY_KP1 },
	{ PAGE_KEYBOARD, 0x5AU, KEY_KP2 },
	{ PAGE_KEYBOARD, 0x5BU, KEY_KP3 },
	{ PAGE_KEYBOARD, 0x5CU, KEY_KP4 },
	{ PAGE_KEYBOARD, 0x5DU, KEY_KP5 },
	{ PAGE_KEYBOARD, 0x5EU, KEY_KP6 },
	{ PAGE_KEYBOARD, 0x5FU, KEY_KP7 },
	{ PAGE_KEYBOARD, 0x60U, KEY_KP8 },
	{ PAGE_KEYBOARD, 0x61U, KEY_KP9 },
	{ PAGE_KEYBOARD, 0x62U, KEY_KP0 },
	{ PAGE_KEYBOARD, 0x63U, KEY_KPDOT },
	{ PAGE_KEYBOARD, 0x64U, KEY_102ND },
	{ PAGE_KEYBOARD, 0x65U, KEY_COMPOSE },
	{ PAGE_KEYBOARD, 0x66U, KEY_POWER },
	{ PAGE_KEYBOARD, 0x67U, KEY_KPEQUAL },
	{ PAGE_KEYBOARD, 0x68U, KEY_F13 },
	{ PAGE_KEYBOARD, 0x69U, KEY_F14 },
	{ PAGE_KEYBOARD, 0x6AU, KEY_F15 },
	{ PAGE_KEYBOARD, 0x6BU, KEY_F16 },
	{ PAGE_KEYBOARD, 0x6CU, KEY_F17 },
	{ PAGE_KEYBOARD, 0x6DU, KEY_F18 },
	{ PAGE_KEYBOARD, 0x6EU, KEY_F19 },
	{ PAGE_KEYBOARD, 0x6FU, KEY_F20 },
	{ PAGE_KEYBOARD, 0x70U, KEY_F21 },
	{ PAGE_KEYBOARD, 0x71U, KEY_F22 },
	{ PAGE_KEYBOARD, 0x72U, KEY_F23 },
	{ PAGE_KEYBOARD, 0x73U, KEY_F24 },
	{ PAGE_KEYBOARD, 0x85U, KEY_KPCOMMA },
	{ PAGE_KEYBOARD, 0x87U, KEY_RO },
	{ PAGE_KEYBOARD, 0x88U, KEY_KATAKANAHIRAGANA },
	{ PAGE_KEYBOARD, 0x89U, KEY_YEN },
	{ PAGE_KEYBOARD, 0x8AU, KEY_HENKAN },
	{ PAGE_KEYBOARD, 0x8BU, KEY_MUHENKAN },
	{ PAGE_KEYBOARD, 0x8CU, KEY_KPJPCOMMA },
	{ PAGE_KEYBOARD, 0x90U, KEY_HANGEUL },
	{ PAGE_KEYBOARD, 0x91U, KEY_HANJA },
	{ PAGE_KEYBOARD, 0x92U, KEY_KATAKANA },
	{ PAGE_KEYBOARD, 0x93U, KEY_HIRAGANA },
	{ PAGE_KEYBOARD, 0xE0U, KEY_LEFTCTRL },
	{ PAGE_KEYBOARD, 0xE1U, KEY_LEFTSHIFT },
	{ PAGE_KEYBOARD, 0xE2U, KEY_LEFTALT },
	{ PAGE_KEYBOARD, 0xE3U, KEY_LEFTMETA },
	{ PAGE_KEYBOARD, 0xE4U, KEY_RIGHTCTRL },
	{ PAGE_KEYBOARD, 0xE5U, KEY_RIGHTSHIFT },
	{ PAGE_KEYBOARD, 0xE6U, KEY_RIGHTALT },
	{ PAGE_KEYBOARD, 0xE7U, KEY_RIGHTMETA },
	{ PAGE_CONSUMER, 0xB5U, KEY_NEXTSONG },
	{ PAGE_CONSUMER, 0xB6U, KEY_PREVIOUSSONG },
	{ PAGE_CONSUMER, 0xB7U, KEY_STOPCD },
	{ PAGE_CONSUMER, 0xCDU, KEY_PLAYPAUSE },
	{ PAGE_CONSUMER, 0xE2U, KEY_MUTE },
	{ PAGE_CONSUMER, 0xE9U, KEY_VOLUMEUP },
	{ PAGE_CONSUMER, 0xEAU, KEY_VOLUMEDOWN },
	{ PAGE_CONSUMER, 0x183U, KEY_CONFIG },
	{ PAGE_CONSUMER, 0x18AU, KEY_MAIL },
	{ PAGE_CONSUMER, 0x192U, KEY_CALC },
	{ PAGE_CONSUMER, 0x194U, KEY_FILE },
	{ PAGE_CONSUMER, 0x221U, KEY_SEARCH },
	{ PAGE_CONSUMER, 0x223U, KEY_HOMEPAGE },
	{ PAGE_CONSUMER, 0x224U, KEY_BACK },
	{ PAGE_CONSUMER, 0x225U, KEY_FORWARD },
	{ PAGE_CONSUMER, 0x226U, KEY_STOP },
	{ PAGE_CONSUMER, 0x227U, KEY_REFRESH },
	{ PAGE_CONSUMER, 0x22AU, KEY_BOOKMARKS },
};

/*
 * The table the checks compare with, indexed by the extended bit and the
 * scan code: the evdev code the rows give, 0 where they give none.  It is
 * filled once by build_expected and never changes afterwards.
 */
static unsigned expected_codes[2][256];

/* The scan codes whose rows hold whether the key is sent with the prefix or without it. */
static const unsigned either_scans[] = { 0x54U, 0x72U, 0xF2U, 0x71U, 0xF1U };

/* Forward declarations. */
static unsigned usage_to_key(unsigned page, unsigned usage);
static int scan_is_either(unsigned scan);
static void build_expected(void);
static unsigned expected_code(unsigned scan, int extended, unsigned virtual_key);
static void test_rows(void);
static void test_all_pairs(void);
static void test_special_cases(void);
static void test_prefix_pairs(void);
static void test_bad_input(void);

/*
 * Runs the checks of the key table and prints the summary line.
 */
int
main(
	int argc,
	char **argv)
{
	int status;

	/* The system name argument is accepted for the common test runner; the table does not depend on it. */
	(void)argc;
	(void)argv;

	/* Builds the expected table from the specification rows. */
	build_expected();

	/* Runs each group of checks. */
	test_rows();
	test_all_pairs();
	test_special_cases();
	test_prefix_pairs();
	test_bad_input();

	/* Prints the summary and takes its status. */
	status = check_finish("keys-table");

	/* Reports whether every check passed. */
	return status;
}

/*
 * Turns a HID usage into the evdev name of its key; 0 when the test does not know the usage.
 */
static unsigned
usage_to_key(
	unsigned page,
	unsigned usage)
{
	size_t index;

	/* Looks for the usage in the hand-read list. */
	for (index = 0U;
	     index < sizeof(usage_keys) / sizeof(usage_keys[0]);
	     index++) {
		if (usage_keys[index].page != page)
			continue;
		if (usage_keys[index].usage == usage)
			return usage_keys[index].key;
	}

	/* Succeeded: the usage is not in the list, so it has no key. */
	return 0U;
}

/*
 * Tells whether a scan code is one the header gives the same key with or without the prefix.
 */
static int
scan_is_either(
	unsigned scan)
{
	size_t index;

	/* Looks for the scan code in the list. */
	for (index = 0U;
	     index < sizeof(either_scans) / sizeof(either_scans[0]);
	     index++) {
		if (either_scans[index] == scan)
			return 1;
	}

	/* Succeeded: the scan code is not one of them. */
	return 0;
}

/*
 * Builds the table of expected codes from the specification rows.
 */
static void
build_expected(void)
{
	size_t index;
	const struct spec_row *row;
	unsigned key;
	int either;

	/* Walks every row of Microsoft's table. */
	for (index = 0U;
	     index < sizeof(spec_rows) / sizeof(spec_rows[0]);
	     index++) {
		row = &spec_rows[index];

		/* Scan code 0x45 is Pause and Num Lock, told apart by the virtual key, so no row decides it. */
		if (row->scan == 0x45U)
			continue;

		/* LANG5 has the make code of F24 and F24 takes it (header), so its row is not used. */
		if (row->page == PAGE_KEYBOARD && row->usage == 0x94U)
			continue;

		/* Reads the usage's key; every other row must have one. */
		key = usage_to_key(row->page, row->usage);
		CHECK(key != 0U);

		/* A scan code two rows share must be given one key by both. */
		if (expected_codes[row->extended][row->scan] != 0U)
			CHECK_EQ(expected_codes[row->extended][row->scan], key);

		/* Enters the key under the prefix the row has. */
		expected_codes[row->extended][row->scan] = key;

		/* Enters the key under the other prefix too when keyboards send it either way. */
		either = scan_is_either(row->scan);
		if (either != 0)
			expected_codes[1 - row->extended][row->scan] = key;
	}
}

/*
 * Gives the code the header promises for one query, from the expected table.
 */
static unsigned
expected_code(
	unsigned scan,
	int extended,
	unsigned virtual_key)
{
	int prefix;

	/* Scan code 0x45 is Pause for the Pause virtual key and Num Lock for any other, with either prefix. */
	if (scan == 0x45U) {
		if (virtual_key == KEIUI_WIN32_VK_PAUSE)
			return KEY_PAUSE;
		return KEY_NUMLOCK;
	}

	/* Reduces the extended bit to 0 or 1 as the header says. */
	prefix = 0;
	if (extended != 0)
		prefix = 1;

	/* Succeeded: the table's entry, 0 for a scan code that has no key. */
	return expected_codes[prefix][scan];
}

/*
 * Checks every row of the specification: its scan code and prefix give the code of its usage.
 */
static void
test_rows(void)
{
	size_t index;
	const struct spec_row *row;
	unsigned key;
	uint32_t code;

	/* Looks up each row's make code and compares it with its usage's key. */
	for (index = 0U;
	     index < sizeof(spec_rows) / sizeof(spec_rows[0]);
	     index++) {
		row = &spec_rows[index];

		/* The rows of scan code 0x45 and the LANG5 row are checked on their own, in the special cases. */
		if (row->scan == 0x45U)
			continue;
		if (row->page == PAGE_KEYBOARD && row->usage == 0x94U)
			continue;

		/* The virtual key does not matter for these scan codes; passes one that is not Pause. */
		key = usage_to_key(row->page, row->usage);
		code = keiui_win32_key_code(row->scan, row->extended, VK_OTHER);
		CHECK_EQ(code, key);
	}
}

/*
 * Checks the 512 pairs of scan code and prefix against the expected table, with four virtual keys each.
 */
static void
test_all_pairs(void)
{
	static const unsigned virtual_keys[] = { 0U, KEIUI_WIN32_VK_PAUSE, KEIUI_WIN32_VK_NUMLOCK, VK_OTHER };
	unsigned scan;
	int extended;
	size_t index;
	uint32_t code;
	unsigned wanted;
	unsigned known;

	/* Counts the keys the table gives, to see that it is not empty. */
	known = 0U;

	/* Compares every scan code, both prefixes and each virtual key with the expectation. */
	for (scan = 0U;
	     scan < 256U;
	     scan++) {
		for (extended = 0;
		     extended < 2;
		     extended++) {
			for (index = 0U;
			     index < sizeof(virtual_keys) / sizeof(virtual_keys[0]);
			     index++) {
				code = keiui_win32_key_code(scan, extended, virtual_keys[index]);
				wanted = expected_code(scan, extended, virtual_keys[index]);
				CHECK_EQ(code, wanted);

				/* Every answer is no key or a code a Keiland event can carry. */
				CHECK(code == 0U || (code >= 1U && code <= EVDEV_MAX));
			}

			/* Counts the pair when the specification gives it a key. */
			wanted = expected_code(scan, extended, VK_OTHER);
			if (wanted != 0U)
				known++;
		}
	}

	/* Microsoft's table names well over a hundred keys; a table that lost half of them is caught here too. */
	CHECK(known > 150U);
}

/*
 * Checks the cases the header singles out.
 */
static void
test_special_cases(void)
{
	/* Pause: scan 0x45 with the Pause virtual key is Pause with either prefix. */
	CHECK_EQ(keiui_win32_key_code(0x45U, 0, KEIUI_WIN32_VK_PAUSE), KEY_PAUSE);
	CHECK_EQ(keiui_win32_key_code(0x45U, 1, KEIUI_WIN32_VK_PAUSE), KEY_PAUSE);

	/* Num Lock: scan 0x45 with the Num Lock virtual key or any other is Num Lock with either prefix. */
	CHECK_EQ(keiui_win32_key_code(0x45U, 1, KEIUI_WIN32_VK_NUMLOCK), KEY_NUMLOCK);
	CHECK_EQ(keiui_win32_key_code(0x45U, 0, KEIUI_WIN32_VK_NUMLOCK), KEY_NUMLOCK);
	CHECK_EQ(keiui_win32_key_code(0x45U, 1, 0U), KEY_NUMLOCK);
	CHECK_EQ(keiui_win32_key_code(0x45U, 0, VK_OTHER), KEY_NUMLOCK);

	/* Break (Ctrl+Pause) is 0x46 with the prefix; Scroll Lock is 0x46 without. */
	CHECK_EQ(keiui_win32_key_code(0x46U, 1, 0x03U), KEY_PAUSE);
	CHECK_EQ(keiui_win32_key_code(0x46U, 0, 0x91U), KEY_SCROLLLOCK);

	/* Print Screen is 0x37 with the prefix; the keypad star is 0x37 without. */
	CHECK_EQ(keiui_win32_key_code(0x37U, 1, 0x2CU), KEY_SYSRQ);
	CHECK_EQ(keiui_win32_key_code(0x37U, 0, 0x6AU), KEY_KPASTERISK);

	/* Alt+Print Screen is 0x54 without the prefix. */
	CHECK_EQ(keiui_win32_key_code(0x54U, 0, 0x2CU), KEY_SYSRQ);

	/* The Japanese keys, sent without the prefix. */
	CHECK_EQ(keiui_win32_key_code(0x29U, 0, 0xF3U), KEY_GRAVE);
	CHECK_EQ(keiui_win32_key_code(0x70U, 0, 0xF2U), KEY_KATAKANAHIRAGANA);
	CHECK_EQ(keiui_win32_key_code(0x73U, 0, 0xC1U), KEY_RO);
	CHECK_EQ(keiui_win32_key_code(0x79U, 0, 0x1CU), KEY_HENKAN);
	CHECK_EQ(keiui_win32_key_code(0x7BU, 0, 0x1DU), KEY_MUHENKAN);
	CHECK_EQ(keiui_win32_key_code(0x7DU, 0, 0xDCU), KEY_YEN);
	CHECK_EQ(keiui_win32_key_code(0x78U, 0, 0U), KEY_KATAKANA);
	CHECK_EQ(keiui_win32_key_code(0x77U, 0, 0U), KEY_HIRAGANA);

	/* Hangul and Hanja, with and without the prefix. */
	CHECK_EQ(keiui_win32_key_code(0x72U, 0, 0x15U), KEY_HANGEUL);
	CHECK_EQ(keiui_win32_key_code(0x72U, 1, 0x15U), KEY_HANGEUL);
	CHECK_EQ(keiui_win32_key_code(0xF2U, 0, 0x15U), KEY_HANGEUL);
	CHECK_EQ(keiui_win32_key_code(0xF2U, 1, 0x15U), KEY_HANGEUL);
	CHECK_EQ(keiui_win32_key_code(0x71U, 0, 0x19U), KEY_HANJA);
	CHECK_EQ(keiui_win32_key_code(0x71U, 1, 0x19U), KEY_HANJA);
	CHECK_EQ(keiui_win32_key_code(0xF1U, 0, 0x19U), KEY_HANJA);
	CHECK_EQ(keiui_win32_key_code(0xF1U, 1, 0x19U), KEY_HANJA);

	/* 0x76 is F24 (it is LANG5 as well in the table; the header gives F24). */
	CHECK_EQ(keiui_win32_key_code(0x76U, 0, 0x87U), KEY_F24);

	/* The media keys. */
	CHECK_EQ(keiui_win32_key_code(0x22U, 1, 0xB3U), KEY_PLAYPAUSE);
	CHECK_EQ(keiui_win32_key_code(0x20U, 1, 0xADU), KEY_MUTE);
	CHECK_EQ(keiui_win32_key_code(0x30U, 1, 0xAFU), KEY_VOLUMEUP);
	CHECK_EQ(keiui_win32_key_code(0x2EU, 1, 0xAEU), KEY_VOLUMEDOWN);
	CHECK_EQ(keiui_win32_key_code(0x19U, 1, 0xB0U), KEY_NEXTSONG);
	CHECK_EQ(keiui_win32_key_code(0x10U, 1, 0xB1U), KEY_PREVIOUSSONG);
}

/*
 * Checks the pairs of keys that differ only in the extended bit.
 */
static void
test_prefix_pairs(void)
{
	/* Enter and the keypad Enter. */
	CHECK_EQ(keiui_win32_key_code(0x1CU, 0, 0x0DU), KEY_ENTER);
	CHECK_EQ(keiui_win32_key_code(0x1CU, 1, 0x0DU), KEY_KPENTER);

	/* The left and the right Control. */
	CHECK_EQ(keiui_win32_key_code(0x1DU, 0, 0xA2U), KEY_LEFTCTRL);
	CHECK_EQ(keiui_win32_key_code(0x1DU, 1, 0xA3U), KEY_RIGHTCTRL);

	/* The left and the right Alt. */
	CHECK_EQ(keiui_win32_key_code(0x38U, 0, 0xA4U), KEY_LEFTALT);
	CHECK_EQ(keiui_win32_key_code(0x38U, 1, 0xA5U), KEY_RIGHTALT);

	/* The Up arrow and the keypad 8. */
	CHECK_EQ(keiui_win32_key_code(0x48U, 1, 0x26U), KEY_UP);
	CHECK_EQ(keiui_win32_key_code(0x48U, 0, 0x26U), KEY_KP8);

	/* The cursor block and the keypad digits under them. */
	CHECK_EQ(keiui_win32_key_code(0x4BU, 1, 0x25U), KEY_LEFT);
	CHECK_EQ(keiui_win32_key_code(0x4BU, 0, 0x25U), KEY_KP4);
	CHECK_EQ(keiui_win32_key_code(0x4DU, 1, 0x27U), KEY_RIGHT);
	CHECK_EQ(keiui_win32_key_code(0x4DU, 0, 0x27U), KEY_KP6);
	CHECK_EQ(keiui_win32_key_code(0x50U, 1, 0x28U), KEY_DOWN);
	CHECK_EQ(keiui_win32_key_code(0x50U, 0, 0x28U), KEY_KP2);

	/* Insert, Delete, Home, End, Page Up and Page Down against the keypad 0, period, 7, 1, 9 and 3. */
	CHECK_EQ(keiui_win32_key_code(0x52U, 1, 0x2DU), KEY_INSERT);
	CHECK_EQ(keiui_win32_key_code(0x52U, 0, 0x2DU), KEY_KP0);
	CHECK_EQ(keiui_win32_key_code(0x53U, 1, 0x2EU), KEY_DELETE);
	CHECK_EQ(keiui_win32_key_code(0x53U, 0, 0x2EU), KEY_KPDOT);
	CHECK_EQ(keiui_win32_key_code(0x47U, 1, 0x24U), KEY_HOME);
	CHECK_EQ(keiui_win32_key_code(0x47U, 0, 0x24U), KEY_KP7);
	CHECK_EQ(keiui_win32_key_code(0x4FU, 1, 0x23U), KEY_END);
	CHECK_EQ(keiui_win32_key_code(0x4FU, 0, 0x23U), KEY_KP1);
	CHECK_EQ(keiui_win32_key_code(0x49U, 1, 0x21U), KEY_PAGEUP);
	CHECK_EQ(keiui_win32_key_code(0x49U, 0, 0x21U), KEY_KP9);
	CHECK_EQ(keiui_win32_key_code(0x51U, 1, 0x22U), KEY_PAGEDOWN);
	CHECK_EQ(keiui_win32_key_code(0x51U, 0, 0x22U), KEY_KP3);

	/* The slash: the keypad / is 0x35 with the prefix, the main slash without. */
	CHECK_EQ(keiui_win32_key_code(0x35U, 0, 0xBFU), KEY_SLASH);
	CHECK_EQ(keiui_win32_key_code(0x35U, 1, 0x6FU), KEY_KPSLASH);

	/* 0x5B and 0x5C: the Windows keys have the prefix; 0x5C without it is International6; 0x5B without it is nothing. */
	CHECK_EQ(keiui_win32_key_code(0x5BU, 1, 0x5BU), KEY_LEFTMETA);
	CHECK_EQ(keiui_win32_key_code(0x5CU, 1, 0x5CU), KEY_RIGHTMETA);
	CHECK_EQ(keiui_win32_key_code(0x5CU, 0, 0xC2U), KEY_KPJPCOMMA);
	CHECK_EQ(keiui_win32_key_code(0x5BU, 0, 0x5BU), 0);
	CHECK_EQ(keiui_win32_key_code(0x5DU, 1, 0x5DU), KEY_COMPOSE);
	CHECK_EQ(keiui_win32_key_code(0x5DU, 0, 0x5DU), 0);

	/* The fake shifts Windows sends around the cursor keys name no key. */
	CHECK_EQ(keiui_win32_key_code(0x2AU, 1, 0x10U), 0);
	CHECK_EQ(keiui_win32_key_code(0x36U, 1, 0x10U), 0);
}

/*
 * Checks the answers to input a caller must not send.
 */
static void
test_bad_input(void)
{
	unsigned scan;
	int extended;

	/* No scan code gives 0, with either prefix and any virtual key. */
	CHECK_EQ(keiui_win32_key_code(0U, 0, 0U), 0);
	CHECK_EQ(keiui_win32_key_code(0U, 1, KEIUI_WIN32_VK_PAUSE), 0);
	CHECK_EQ(keiui_win32_key_code(0U, 1, KEIUI_WIN32_VK_NUMLOCK), 0);

	/* The unused scan codes between 0x5E and 0x63 and above 0x7E that the table does not hold. */
	CHECK_EQ(keiui_win32_key_code(0x5AU, 0, 0U), 0);
	CHECK_EQ(keiui_win32_key_code(0x5AU, 1, 0U), 0);
	CHECK_EQ(keiui_win32_key_code(0x60U, 0, 0U), 0);
	CHECK_EQ(keiui_win32_key_code(0x61U, 1, 0U), 0);
	CHECK_EQ(keiui_win32_key_code(0x7FU, 0, 0U), 0);
	CHECK_EQ(keiui_win32_key_code(0xFFU, 0, 0U), 0);
	CHECK_EQ(keiui_win32_key_code(0xFFU, 1, 0U), 0);

	/* A scan code that does not fit in 8 bits is the caller's mistake and gives 0, not the low byte's key. */
	CHECK_EQ(keiui_win32_key_code(256U, 0, 0U), 0);
	CHECK_EQ(keiui_win32_key_code(256U + 0x1EU, 0, 0U), 0);
	CHECK_EQ(keiui_win32_key_code(0x145U, 0, KEIUI_WIN32_VK_PAUSE), 0);
	CHECK_EQ(keiui_win32_key_code(0x145U, 1, KEIUI_WIN32_VK_NUMLOCK), 0);
	CHECK_EQ(keiui_win32_key_code(0x1FFU, 1, 0U), 0);
	CHECK_EQ(keiui_win32_key_code(0xFFFFFFFFU, 1, 0U), 0);
	CHECK_EQ(keiui_win32_key_code(0x80000000U, 0, 0U), 0);

	/* Any non-zero extended value is treated as 1. */
	CHECK_EQ(keiui_win32_key_code(0x1CU, 2, 0U), KEY_KPENTER);
	CHECK_EQ(keiui_win32_key_code(0x1CU, -1, 0U), KEY_KPENTER);
	CHECK_EQ(keiui_win32_key_code(0x1CU, INT_MAX, 0U), KEY_KPENTER);
	CHECK_EQ(keiui_win32_key_code(0x1CU, INT_MIN, 0U), KEY_KPENTER);

	/* An extended of 2 gives what 1 gives for every scan code, and a huge virtual key changes nothing. */
	for (scan = 0U;
	     scan < 256U;
	     scan++) {
		extended = 2;
		CHECK_EQ(keiui_win32_key_code(scan, extended, 0U), keiui_win32_key_code(scan, 1, 0U));
		CHECK_EQ(keiui_win32_key_code(scan, 0, 0xFFFFFFFFU), expected_code(scan, 0, 0xFFFFFFFFU));
	}
}
