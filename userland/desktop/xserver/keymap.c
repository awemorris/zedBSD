/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The keyboard's map of the US layout, by the keys' evdev codes.
 *
 * The core's keycodes name the keys by their place on the keyboard: an
 * evdev key code plus 8.  This map gives each key the keysyms it carries
 * alone and with Shift, and the modifier keys; it is the core's map until
 * a backend gives its own (the Windows backend reads the user's layout),
 * and the map of zedBSD's desktop, whose keyboard is US.
 */

#include "userland/desktop/xserver/internal.h"

#include "userland/desktop/xserver/keycodes.h"

#include <string.h>

/* The core protocol's modifiers, by their index in the map. */
#define KEYMAP_SHIFT		0U
#define KEYMAP_LOCK		1U
#define KEYMAP_CONTROL		2U
#define KEYMAP_MOD1		3U
#define KEYMAP_MOD2		4U
#define KEYMAP_MOD4		6U

/*
 * One key of the layout: its evdev code and its keysyms alone and with
 * Shift (0: none, the first is used).
 */
struct keymap_key {
	uint16_t code;
	uint32_t plain;
	uint32_t shifted;
};

/*
 * One modifier key of the layout: the modifier it holds (its index in the
 * map) and the key's evdev code.
 */
struct keymap_modifier {
	unsigned modifier;
	uint16_t code;
};

/*
 * The keys of the US layout and their keysyms (X's keysym numbers: a
 * Latin-1 character is its own number, the other keys' from 0xff00).
 * The keypad keeps its keys' functions alone and its digits second, which
 * a client chooses while Num_Lock's modifier is held.
 */
static const struct keymap_key keymap_keys[] = {
	{ KEY_ESC, 0xff1bU, 0U },	/* Escape */
	{ KEY_1, 0x31U, 0x21U },	/* 1 ! */
	{ KEY_2, 0x32U, 0x40U },	/* 2 @ */
	{ KEY_3, 0x33U, 0x23U },	/* 3 # */
	{ KEY_4, 0x34U, 0x24U },	/* 4 $ */
	{ KEY_5, 0x35U, 0x25U },	/* 5 % */
	{ KEY_6, 0x36U, 0x5eU },	/* 6 ^ */
	{ KEY_7, 0x37U, 0x26U },	/* 7 & */
	{ KEY_8, 0x38U, 0x2aU },	/* 8 * */
	{ KEY_9, 0x39U, 0x28U },	/* 9 ( */
	{ KEY_0, 0x30U, 0x29U },	/* 0 ) */
	{ KEY_MINUS, 0x2dU, 0x5fU },	/* - _ */
	{ KEY_EQUAL, 0x3dU, 0x2bU },	/* = + */
	{ KEY_BACKSPACE, 0xff08U, 0U },	/* BackSpace */
	{ KEY_TAB, 0xff09U, 0xfe20U },	/* Tab, ISO_Left_Tab */
	{ KEY_Q, 0x71U, 0x51U },	/* q Q */
	{ KEY_W, 0x77U, 0x57U },	/* w W */
	{ KEY_E, 0x65U, 0x45U },	/* e E */
	{ KEY_R, 0x72U, 0x52U },	/* r R */
	{ KEY_T, 0x74U, 0x54U },	/* t T */
	{ KEY_Y, 0x79U, 0x59U },	/* y Y */
	{ KEY_U, 0x75U, 0x55U },	/* u U */
	{ KEY_I, 0x69U, 0x49U },	/* i I */
	{ KEY_O, 0x6fU, 0x4fU },	/* o O */
	{ KEY_P, 0x70U, 0x50U },	/* p P */
	{ KEY_LEFTBRACE, 0x5bU, 0x7bU },	/* [ { */
	{ KEY_RIGHTBRACE, 0x5dU, 0x7dU },	/* ] } */
	{ KEY_ENTER, 0xff0dU, 0U },	/* Return */
	{ KEY_LEFTCTRL, 0xffe3U, 0U },	/* Control_L */
	{ KEY_A, 0x61U, 0x41U },	/* a A */
	{ KEY_S, 0x73U, 0x53U },	/* s S */
	{ KEY_D, 0x64U, 0x44U },	/* d D */
	{ KEY_F, 0x66U, 0x46U },	/* f F */
	{ KEY_G, 0x67U, 0x47U },	/* g G */
	{ KEY_H, 0x68U, 0x48U },	/* h H */
	{ KEY_J, 0x6aU, 0x4aU },	/* j J */
	{ KEY_K, 0x6bU, 0x4bU },	/* k K */
	{ KEY_L, 0x6cU, 0x4cU },	/* l L */
	{ KEY_SEMICOLON, 0x3bU, 0x3aU },	/* ; : */
	{ KEY_APOSTROPHE, 0x27U, 0x22U },	/* ' " */
	{ KEY_GRAVE, 0x60U, 0x7eU },	/* ` ~ */
	{ KEY_LEFTSHIFT, 0xffe1U, 0U },	/* Shift_L */
	{ KEY_BACKSLASH, 0x5cU, 0x7cU },	/* \\ | */
	{ KEY_Z, 0x7aU, 0x5aU },	/* z Z */
	{ KEY_X, 0x78U, 0x58U },	/* x X */
	{ KEY_C, 0x63U, 0x43U },	/* c C */
	{ KEY_V, 0x76U, 0x56U },	/* v V */
	{ KEY_B, 0x62U, 0x42U },	/* b B */
	{ KEY_N, 0x6eU, 0x4eU },	/* n N */
	{ KEY_M, 0x6dU, 0x4dU },	/* m M */
	{ KEY_COMMA, 0x2cU, 0x3cU },	/* , < */
	{ KEY_DOT, 0x2eU, 0x3eU },	/* . > */
	{ KEY_SLASH, 0x2fU, 0x3fU },	/* / ? */
	{ KEY_RIGHTSHIFT, 0xffe2U, 0U },	/* Shift_R */
	{ KEY_KPASTERISK, 0xffaaU, 0U },	/* KP_Multiply */
	{ KEY_LEFTALT, 0xffe9U, 0xffe7U },	/* Alt_L, Meta_L */
	{ KEY_SPACE, 0x20U, 0U },	/* space */
	{ KEY_CAPSLOCK, 0xffe5U, 0U },	/* Caps_Lock */
	{ KEY_F1, 0xffbeU, 0U },	/* F1 */
	{ KEY_F2, 0xffbfU, 0U },	/* F2 */
	{ KEY_F3, 0xffc0U, 0U },	/* F3 */
	{ KEY_F4, 0xffc1U, 0U },	/* F4 */
	{ KEY_F5, 0xffc2U, 0U },	/* F5 */
	{ KEY_F6, 0xffc3U, 0U },	/* F6 */
	{ KEY_F7, 0xffc4U, 0U },	/* F7 */
	{ KEY_F8, 0xffc5U, 0U },	/* F8 */
	{ KEY_F9, 0xffc6U, 0U },	/* F9 */
	{ KEY_F10, 0xffc7U, 0U },	/* F10 */
	{ KEY_NUMLOCK, 0xff7fU, 0U },	/* Num_Lock */
	{ KEY_SCROLLLOCK, 0xff14U, 0U },	/* Scroll_Lock */
	{ KEY_KP7, 0xff95U, 0xffb7U },	/* KP_Home, KP_7 */
	{ KEY_KP8, 0xff97U, 0xffb8U },	/* KP_Up, KP_8 */
	{ KEY_KP9, 0xff9aU, 0xffb9U },	/* KP_Prior, KP_9 */
	{ KEY_KPMINUS, 0xffadU, 0U },	/* KP_Subtract */
	{ KEY_KP4, 0xff96U, 0xffb4U },	/* KP_Left, KP_4 */
	{ KEY_KP5, 0xff9dU, 0xffb5U },	/* KP_Begin, KP_5 */
	{ KEY_KP6, 0xff98U, 0xffb6U },	/* KP_Right, KP_6 */
	{ KEY_KPPLUS, 0xffabU, 0U },	/* KP_Add */
	{ KEY_KP1, 0xff9cU, 0xffb1U },	/* KP_End, KP_1 */
	{ KEY_KP2, 0xff99U, 0xffb2U },	/* KP_Down, KP_2 */
	{ KEY_KP3, 0xff9bU, 0xffb3U },	/* KP_Next, KP_3 */
	{ KEY_KP0, 0xff9eU, 0xffb0U },	/* KP_Insert, KP_0 */
	{ KEY_KPDOT, 0xff9fU, 0xffaeU },	/* KP_Delete, KP_Decimal */
	{ KEY_102ND, 0x3cU, 0x3eU },	/* < > */
	{ KEY_F11, 0xffc8U, 0U },	/* F11 */
	{ KEY_F12, 0xffc9U, 0U },	/* F12 */
	{ KEY_KPENTER, 0xff8dU, 0U },	/* KP_Enter */
	{ KEY_RIGHTCTRL, 0xffe4U, 0U },	/* Control_R */
	{ KEY_KPSLASH, 0xffafU, 0U },	/* KP_Divide */
	{ KEY_SYSRQ, 0xff61U, 0U },	/* Print */
	{ KEY_RIGHTALT, 0xffeaU, 0xffe8U },	/* Alt_R, Meta_R */
	{ KEY_HOME, 0xff50U, 0U },	/* Home */
	{ KEY_UP, 0xff52U, 0U },	/* Up */
	{ KEY_PAGEUP, 0xff55U, 0U },	/* Prior */
	{ KEY_LEFT, 0xff51U, 0U },	/* Left */
	{ KEY_RIGHT, 0xff53U, 0U },	/* Right */
	{ KEY_END, 0xff57U, 0U },	/* End */
	{ KEY_DOWN, 0xff54U, 0U },	/* Down */
	{ KEY_PAGEDOWN, 0xff56U, 0U },	/* Next */
	{ KEY_INSERT, 0xff63U, 0U },	/* Insert */
	{ KEY_DELETE, 0xffffU, 0U },	/* Delete */
	{ KEY_PAUSE, 0xff13U, 0U },	/* Pause */
	{ KEY_LEFTMETA, 0xffebU, 0U },	/* Super_L */
	{ KEY_RIGHTMETA, 0xffecU, 0U },	/* Super_R */
	{ KEY_COMPOSE, 0xff67U, 0U }	/* Menu */
};

/*
 * The modifier keys: the modifier, and the key.
 */
static const struct keymap_modifier keymap_modifiers[] = {
	{ KEYMAP_SHIFT, KEY_LEFTSHIFT },
	{ KEYMAP_SHIFT, KEY_RIGHTSHIFT },
	{ KEYMAP_LOCK, KEY_CAPSLOCK },
	{ KEYMAP_CONTROL, KEY_LEFTCTRL },
	{ KEYMAP_CONTROL, KEY_RIGHTCTRL },
	{ KEYMAP_MOD1, KEY_LEFTALT },
	{ KEYMAP_MOD1, KEY_RIGHTALT },
	{ KEYMAP_MOD2, KEY_NUMLOCK },
	{ KEYMAP_MOD4, KEY_LEFTMETA },
	{ KEYMAP_MOD4, KEY_RIGHTMETA }
};

/*
 * Fills a keyboard map with the US layout: each key's keysyms alone and
 * with Shift at its keycode (its evdev code plus 8), and the modifier keys
 * (Shift, Caps Lock, Control, Alt as Mod1, Num Lock as Mod2, the logo keys
 * as Mod4).
 */
void
x11_keymap_us(
	struct x11_keymap *keymap)
{
	unsigned keycode;
	unsigned modifier;
	unsigned slot;
	size_t index;

	/* No key carries anything yet. */
	memset(keymap, 0, sizeof(*keymap));

	/* Each key's keysyms at its keycode. */
	for (index = 0U; index < sizeof(keymap_keys) / sizeof(keymap_keys[0]); index++) {
		keycode = (unsigned)keymap_keys[index].code + X11_KEYMAP_EVDEV_OFFSET;
		keymap->keysyms[keycode][0] = keymap_keys[index].plain;
		keymap->keysyms[keycode][1] = keymap_keys[index].shifted;
	}

	/* Each modifier key in the next free slot of its modifier. */
	for (index = 0U; index < sizeof(keymap_modifiers) / sizeof(keymap_modifiers[0]); index++) {
		/* The modifier's first free slot. */
		modifier = keymap_modifiers[index].modifier;
		keycode = (unsigned)keymap_modifiers[index].code + X11_KEYMAP_EVDEV_OFFSET;
		for (slot = 0U; slot < X11_KEYMAP_MODIFIER_KEYS; slot++) {
			if (keymap->modifiers[modifier][slot] == 0U)
				break;
		}

		/* The key in it (the table never fills a modifier). */
		if (slot < X11_KEYMAP_MODIFIER_KEYS)
			keymap->modifiers[modifier][slot] = (uint8_t)keycode;
	}
}
