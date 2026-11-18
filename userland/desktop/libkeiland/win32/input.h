/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The pointer and keyboard details of the Windows backend (input.c): what
 * the window procedure reads out of a message's wParam and lParam, turned
 * into the values of struct kl_window_event.  Pure: no Windows header (the
 * few constants are given here with their <winuser.h> values), no state.
 */

#ifndef KEIUI_WIN32_INPUT_H
#define KEIUI_WIN32_INPUT_H

#include <stdint.h>

/* One wheel notch on Windows (WHEEL_DELTA). */
#define KEIUI_WIN32_WHEEL_DELTA		120

/*
 * The pixels one notch scrolls on Keiland: the compositor sends 15 units
 * a notch (wayland/seat.c WHEEL_STEP) and libkeiland's Wayland backend
 * scales units by 4 (ui/window.c WINDOW_SCROLL_SCALE), so 60.
 */
#define KEIUI_WIN32_NOTCH_PIXELS	60.0

/* The buttons of the mouse messages (which message, or XBUTTON1 and 2 of WM_XBUTTON*). */
#define KEIUI_WIN32_BUTTON_LEFT		0U	/* WM_LBUTTON* */
#define KEIUI_WIN32_BUTTON_RIGHT	1U	/* WM_RBUTTON* */
#define KEIUI_WIN32_BUTTON_MIDDLE	2U	/* WM_MBUTTON* */
#define KEIUI_WIN32_BUTTON_X1		3U	/* WM_XBUTTON* with XBUTTON1 */
#define KEIUI_WIN32_BUTTON_X2		4U	/* WM_XBUTTON* with XBUTTON2 */

/* The modifier keys held, as the caller reads them with GetKeyState (bit set: the key is down). */
#define KEIUI_WIN32_HELD_SHIFT		0x01U	/* VK_SHIFT */
#define KEIUI_WIN32_HELD_CONTROL	0x02U	/* VK_CONTROL */
#define KEIUI_WIN32_HELD_ALT		0x04U	/* VK_MENU */
#define KEIUI_WIN32_HELD_WIN		0x08U	/* VK_LWIN or VK_RWIN */
#define KEIUI_WIN32_HELD_ALTGR		0x10U	/* the right Alt of a layout with AltGr (VK_RMENU with VK_LCONTROL synthesized) */

/* The kinds of pointer of WM_POINTER* (POINTER_INPUT_TYPE values). */
#define KEIUI_WIN32_POINTER_GENERIC	1U	/* PT_POINTER */
#define KEIUI_WIN32_POINTER_TOUCH	2U	/* PT_TOUCH */
#define KEIUI_WIN32_POINTER_PEN		3U	/* PT_PEN */
#define KEIUI_WIN32_POINTER_MOUSE	4U	/* PT_MOUSE */
#define KEIUI_WIN32_POINTER_TOUCHPAD	5U	/* PT_TOUCHPAD */

/* What a key message's lParam says (keiui_win32_key_flags). */
struct keiui_win32_key_flags {
	unsigned scan;		/* bits 16 to 23 */
	int extended;		/* bit 24: 1 or 0 */
	int repeated;		/* bit 30 as it is (1 or 0): on a key down, the key was already down; Windows sets it on every key up */
	int released;		/* bit 31: 1 for a key up */
};

/*
 * Turns one WM_MOUSEWHEEL or WM_MOUSEHWHEEL delta into Keiland's distance
 * in pixels.
 *
 * delta:      the signed 16-bit high word of wParam (a notch is 120; a
 *             precision touch pad sends smaller values).
 * horizontal: 1 for WM_MOUSEHWHEEL, 0 for WM_MOUSEWHEEL.
 * Returns the distance for the event's dy (vertical) or dx (horizontal):
 * KEIUI_WIN32_NOTCH_PIXELS * delta / 120, negated for the vertical wheel
 * (Windows' positive vertical delta turns the wheel away from the user,
 * which scrolls up; Keiland's positive dy scrolls down) and not negated for
 * the horizontal one (both positive to the right).
 */
double keiui_win32_wheel_pixels(int delta, int horizontal);

/*
 * Turns the modifier keys held into KL_MOD_* (<keiland/keiland_key.h>).
 *
 * held: KEIUI_WIN32_HELD_* bits; unknown bits are ignored.
 * AltGr is not Ctrl and Alt to Keiland: when KEIUI_WIN32_HELD_ALTGR is set,
 * the CONTROL and ALT bits that Windows synthesizes with it are dropped (an
 * application then sees the character AltGr types, not a shortcut).
 * Returns the KL_MOD_SHIFT, KL_MOD_CTRL, KL_MOD_ALT and KL_MOD_SUPER bits.
 */
unsigned keiui_win32_modifiers(unsigned held);

/*
 * Gives the evdev code of a mouse button (KL_BUTTON_LEFT 0x110,
 * KL_BUTTON_RIGHT 0x111, KL_BUTTON_MIDDLE 0x112, BTN_SIDE 0x113 for X1,
 * BTN_EXTRA 0x114 for X2), or 0 for an unknown button.
 */
uint32_t keiui_win32_button_code(unsigned button);

/*
 * Reads a point from a mouse message's lParam: x is the signed low word, y
 * the signed high word (GET_X_LPARAM and GET_Y_LPARAM; negative on a
 * second monitor to the left or above).  lparam is the message's LPARAM
 * converted to int64_t.  NULL outputs are not written.
 */
void keiui_win32_point(int64_t lparam, int32_t *x, int32_t *y);

/*
 * Reads a key message's lParam.  flags NULL does nothing.
 */
void keiui_win32_key_flags(int64_t lparam, struct keiui_win32_key_flags *flags);

#endif
