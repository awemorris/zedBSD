/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The keys of the Android backend: Android's key codes to the evdev codes
 * libkeiland's programs know (keiland_key.h), and the meta state to the
 * modifiers held.
 *
 * A key of a real keyboard comes with its scan code, which is the evdev code
 * already; a key from the soft keyboard or the key bar comes with Android's
 * code alone, and the table gives it.  The Back key is the terminal's Escape.
 */

#include "apps/libkeiland/android/android.h"

#include <android/input.h>
#include <android/keycodes.h>

#include <stddef.h>
#include <stdint.h>

/* The evdev codes (linux/input-event-codes.h) the table uses. */
#define KEYS_ESC	1
#define KEYS_1	2
#define KEYS_2	3
#define KEYS_3	4
#define KEYS_4	5
#define KEYS_5	6
#define KEYS_6	7
#define KEYS_7	8
#define KEYS_8	9
#define KEYS_9	10
#define KEYS_0	11
#define KEYS_MINUS	12
#define KEYS_EQUAL	13
#define KEYS_BACKSPACE	14
#define KEYS_TAB	15
#define KEYS_Q	16
#define KEYS_W	17
#define KEYS_E	18
#define KEYS_R	19
#define KEYS_T	20
#define KEYS_Y	21
#define KEYS_U	22
#define KEYS_I	23
#define KEYS_O	24
#define KEYS_P	25
#define KEYS_LEFTBRACE	26
#define KEYS_RIGHTBRACE	27
#define KEYS_ENTER	28
#define KEYS_LEFTCTRL	29
#define KEYS_A	30
#define KEYS_S	31
#define KEYS_D	32
#define KEYS_F	33
#define KEYS_G	34
#define KEYS_H	35
#define KEYS_J	36
#define KEYS_K	37
#define KEYS_L	38
#define KEYS_SEMICOLON	39
#define KEYS_APOSTROPHE	40
#define KEYS_GRAVE	41
#define KEYS_LEFTSHIFT	42
#define KEYS_BACKSLASH	43
#define KEYS_Z	44
#define KEYS_X	45
#define KEYS_C	46
#define KEYS_V	47
#define KEYS_B	48
#define KEYS_N	49
#define KEYS_M	50
#define KEYS_COMMA	51
#define KEYS_DOT	52
#define KEYS_SLASH	53
#define KEYS_RIGHTSHIFT	54
#define KEYS_KPASTERISK	55
#define KEYS_LEFTALT	56
#define KEYS_SPACE	57
#define KEYS_CAPSLOCK	58
#define KEYS_F1	59
#define KEYS_F2	60
#define KEYS_F3	61
#define KEYS_F4	62
#define KEYS_F5	63
#define KEYS_F6	64
#define KEYS_F7	65
#define KEYS_F8	66
#define KEYS_F9	67
#define KEYS_F10	68
#define KEYS_NUMLOCK	69
#define KEYS_SCROLLLOCK	70
#define KEYS_KP7	71
#define KEYS_KP8	72
#define KEYS_KP9	73
#define KEYS_KPMINUS	74
#define KEYS_KP4	75
#define KEYS_KP5	76
#define KEYS_KP6	77
#define KEYS_KPPLUS	78
#define KEYS_KP1	79
#define KEYS_KP2	80
#define KEYS_KP3	81
#define KEYS_KP0	82
#define KEYS_KPDOT	83
#define KEYS_F11	87
#define KEYS_F12	88
#define KEYS_KPENTER	96
#define KEYS_RIGHTCTRL	97
#define KEYS_KPSLASH	98
#define KEYS_SYSRQ	99
#define KEYS_RIGHTALT	100
#define KEYS_HOME	102
#define KEYS_UP	103
#define KEYS_PAGEUP	104
#define KEYS_LEFT	105
#define KEYS_RIGHT	106
#define KEYS_END	107
#define KEYS_DOWN	108
#define KEYS_PAGEDOWN	109
#define KEYS_INSERT	110
#define KEYS_DELETE	111
#define KEYS_KPEQUAL	117
#define KEYS_PAUSE	119
#define KEYS_LEFTMETA	125
#define KEYS_RIGHTMETA	126
#define KEYS_COMPOSE	127

/* The evdev code of each Android key code; 0 for a key that has none. */
static const uint16_t keys_table[] = {
	[AKEYCODE_0] = KEYS_0,
	[AKEYCODE_1] = KEYS_1,
	[AKEYCODE_2] = KEYS_2,
	[AKEYCODE_3] = KEYS_3,
	[AKEYCODE_4] = KEYS_4,
	[AKEYCODE_5] = KEYS_5,
	[AKEYCODE_6] = KEYS_6,
	[AKEYCODE_7] = KEYS_7,
	[AKEYCODE_8] = KEYS_8,
	[AKEYCODE_9] = KEYS_9,
	[AKEYCODE_A] = KEYS_A,
	[AKEYCODE_B] = KEYS_B,
	[AKEYCODE_C] = KEYS_C,
	[AKEYCODE_D] = KEYS_D,
	[AKEYCODE_E] = KEYS_E,
	[AKEYCODE_F] = KEYS_F,
	[AKEYCODE_G] = KEYS_G,
	[AKEYCODE_H] = KEYS_H,
	[AKEYCODE_I] = KEYS_I,
	[AKEYCODE_J] = KEYS_J,
	[AKEYCODE_K] = KEYS_K,
	[AKEYCODE_L] = KEYS_L,
	[AKEYCODE_M] = KEYS_M,
	[AKEYCODE_N] = KEYS_N,
	[AKEYCODE_O] = KEYS_O,
	[AKEYCODE_P] = KEYS_P,
	[AKEYCODE_Q] = KEYS_Q,
	[AKEYCODE_R] = KEYS_R,
	[AKEYCODE_S] = KEYS_S,
	[AKEYCODE_T] = KEYS_T,
	[AKEYCODE_U] = KEYS_U,
	[AKEYCODE_V] = KEYS_V,
	[AKEYCODE_W] = KEYS_W,
	[AKEYCODE_X] = KEYS_X,
	[AKEYCODE_Y] = KEYS_Y,
	[AKEYCODE_Z] = KEYS_Z,
	[AKEYCODE_COMMA] = KEYS_COMMA,
	[AKEYCODE_PERIOD] = KEYS_DOT,
	[AKEYCODE_ALT_LEFT] = KEYS_LEFTALT,
	[AKEYCODE_ALT_RIGHT] = KEYS_RIGHTALT,
	[AKEYCODE_SHIFT_LEFT] = KEYS_LEFTSHIFT,
	[AKEYCODE_SHIFT_RIGHT] = KEYS_RIGHTSHIFT,
	[AKEYCODE_TAB] = KEYS_TAB,
	[AKEYCODE_SPACE] = KEYS_SPACE,
	[AKEYCODE_ENTER] = KEYS_ENTER,
	[AKEYCODE_DEL] = KEYS_BACKSPACE,
	[AKEYCODE_GRAVE] = KEYS_GRAVE,
	[AKEYCODE_MINUS] = KEYS_MINUS,
	[AKEYCODE_EQUALS] = KEYS_EQUAL,
	[AKEYCODE_LEFT_BRACKET] = KEYS_LEFTBRACE,
	[AKEYCODE_RIGHT_BRACKET] = KEYS_RIGHTBRACE,
	[AKEYCODE_BACKSLASH] = KEYS_BACKSLASH,
	[AKEYCODE_SEMICOLON] = KEYS_SEMICOLON,
	[AKEYCODE_APOSTROPHE] = KEYS_APOSTROPHE,
	[AKEYCODE_SLASH] = KEYS_SLASH,
	[AKEYCODE_DPAD_UP] = KEYS_UP,
	[AKEYCODE_DPAD_DOWN] = KEYS_DOWN,
	[AKEYCODE_DPAD_LEFT] = KEYS_LEFT,
	[AKEYCODE_DPAD_RIGHT] = KEYS_RIGHT,
	[AKEYCODE_ESCAPE] = KEYS_ESC,
	[AKEYCODE_BACK] = KEYS_ESC,
	[AKEYCODE_FORWARD_DEL] = KEYS_DELETE,
	[AKEYCODE_CTRL_LEFT] = KEYS_LEFTCTRL,
	[AKEYCODE_CTRL_RIGHT] = KEYS_RIGHTCTRL,
	[AKEYCODE_CAPS_LOCK] = KEYS_CAPSLOCK,
	[AKEYCODE_SCROLL_LOCK] = KEYS_SCROLLLOCK,
	[AKEYCODE_META_LEFT] = KEYS_LEFTMETA,
	[AKEYCODE_META_RIGHT] = KEYS_RIGHTMETA,
	[AKEYCODE_SYSRQ] = KEYS_SYSRQ,
	[AKEYCODE_BREAK] = KEYS_PAUSE,
	[AKEYCODE_MOVE_HOME] = KEYS_HOME,
	[AKEYCODE_MOVE_END] = KEYS_END,
	[AKEYCODE_INSERT] = KEYS_INSERT,
	[AKEYCODE_PAGE_UP] = KEYS_PAGEUP,
	[AKEYCODE_PAGE_DOWN] = KEYS_PAGEDOWN,
	[AKEYCODE_MENU] = KEYS_COMPOSE,
	[AKEYCODE_NUM_LOCK] = KEYS_NUMLOCK,
	[AKEYCODE_NUMPAD_DIVIDE] = KEYS_KPSLASH,
	[AKEYCODE_NUMPAD_MULTIPLY] = KEYS_KPASTERISK,
	[AKEYCODE_NUMPAD_SUBTRACT] = KEYS_KPMINUS,
	[AKEYCODE_NUMPAD_ADD] = KEYS_KPPLUS,
	[AKEYCODE_NUMPAD_DOT] = KEYS_KPDOT,
	[AKEYCODE_NUMPAD_ENTER] = KEYS_KPENTER,
	[AKEYCODE_NUMPAD_EQUALS] = KEYS_KPEQUAL,
	[AKEYCODE_NUMPAD_0] = KEYS_KP0,
	[AKEYCODE_NUMPAD_1] = KEYS_KP1,
	[AKEYCODE_NUMPAD_2] = KEYS_KP2,
	[AKEYCODE_NUMPAD_3] = KEYS_KP3,
	[AKEYCODE_NUMPAD_4] = KEYS_KP4,
	[AKEYCODE_NUMPAD_5] = KEYS_KP5,
	[AKEYCODE_NUMPAD_6] = KEYS_KP6,
	[AKEYCODE_NUMPAD_7] = KEYS_KP7,
	[AKEYCODE_NUMPAD_8] = KEYS_KP8,
	[AKEYCODE_NUMPAD_9] = KEYS_KP9,
	[AKEYCODE_F1] = KEYS_F1,
	[AKEYCODE_F2] = KEYS_F2,
	[AKEYCODE_F3] = KEYS_F3,
	[AKEYCODE_F4] = KEYS_F4,
	[AKEYCODE_F5] = KEYS_F5,
	[AKEYCODE_F6] = KEYS_F6,
	[AKEYCODE_F7] = KEYS_F7,
	[AKEYCODE_F8] = KEYS_F8,
	[AKEYCODE_F9] = KEYS_F9,
	[AKEYCODE_F10] = KEYS_F10,
	[AKEYCODE_F11] = KEYS_F11,
	[AKEYCODE_F12] = KEYS_F12,
};

/*
 * Returns the evdev code of a key: the scan code when the keyboard gave one
 * (1 to 0x2ff), else the table's code of the Android key, else 0.
 */
uint32_t
android_key_code(
	int32_t keycode,
	int32_t scancode)
{
	/* A real keyboard's scan code is the evdev code. */
	if (scancode >= 1 && scancode <= 0x2ff)
		return (uint32_t)scancode;

	/* The table. */
	if (keycode < 0 || (size_t)keycode >= sizeof(keys_table) / sizeof(keys_table[0]))
		return 0U;
	return keys_table[keycode];
}

/*
 * Returns the modifiers (KL_MOD_*) of an Android meta state.
 */
unsigned
android_key_modifiers(
	int32_t meta)
{
	unsigned modifiers;

	/* The ones libkeiland knows. */
	modifiers = 0U;
	if ((meta & AMETA_SHIFT_ON) != 0)
		modifiers |= KL_MOD_SHIFT;
	if ((meta & AMETA_CTRL_ON) != 0)
		modifiers |= KL_MOD_CTRL;
	if ((meta & AMETA_ALT_ON) != 0)
		modifiers |= KL_MOD_ALT;
	if ((meta & AMETA_META_ON) != 0)
		modifiers |= KL_MOD_SUPER;

	/* Succeeded. */
	return modifiers;
}
