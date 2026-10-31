/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The pointer and keyboard details of the Windows backend.
 *
 * The bit layouts are those of the Win32 messages: the wheel delta is a
 * signed 16-bit value in WM_MOUSEWHEEL and WM_MOUSEHWHEEL, a mouse
 * position is two signed 16-bit words of lParam, and a key message's
 * lParam carries the scan code in bits 16 to 23, the extended-key flag in
 * bit 24, the previous key state in bit 30 and the transition state in bit
 * 31.
 */

#include "userland/desktop/libkeiland/win32/input.h"

#include <keiland/keiland_key.h>
#include <keiland/keiland_window.h>

#include <stddef.h>
#include <stdint.h>

/* The evdev code of the first side button (BTN_SIDE); the next one (BTN_EXTRA) follows it. */
#define EVDEV_BUTTON_SIDE	0x113U
#define EVDEV_BUTTON_EXTRA	0x114U

/* The sign bit of a 16-bit word, and the size of the 16-bit range. */
#define WORD_SIGN_BIT		0x8000U
#define WORD_RANGE		0x10000

/* Forward declarations. */
static int32_t signed_word(uint64_t word);

/*
 * Turns a wheel delta into Keiland's distance in pixels.
 */
double
keiui_win32_wheel_pixels(
	int delta,
	int horizontal)
{
	double pixels;

	/* No movement is plain zero, never a negative zero. */
	if (delta == 0)
		return 0.0;

	/* A notch is KEIUI_WIN32_WHEEL_DELTA units and moves a fixed distance. */
	pixels = KEIUI_WIN32_NOTCH_PIXELS * (double)delta / (double)KEIUI_WIN32_WHEEL_DELTA;

	/* The horizontal wheel has the same direction on both systems. */
	if (horizontal != 0)
		return pixels;

	/* A positive vertical delta scrolls up, which is a negative distance on Keiland. */
	return -pixels;
}

/*
 * Turns the modifier keys held into KL_MOD_* bits.
 */
unsigned
keiui_win32_modifiers(
	unsigned held)
{
	unsigned modifiers;

	modifiers = 0;

	/* Shift and the Windows key map one to one. */
	if ((held & KEIUI_WIN32_HELD_SHIFT) != 0)
		modifiers |= KL_MOD_SHIFT;
	if ((held & KEIUI_WIN32_HELD_WIN) != 0)
		modifiers |= KL_MOD_SUPER;

	/* Control and Alt map one to one unless AltGr made them. */
	if ((held & KEIUI_WIN32_HELD_CONTROL) != 0)
		modifiers |= KL_MOD_CTRL;
	if ((held & KEIUI_WIN32_HELD_ALT) != 0)
		modifiers |= KL_MOD_ALT;

	/*
	 * AltGr is a key of its own that types characters.  Windows reports it
	 * as Control and Alt together, which would make every character it types
	 * a shortcut, so both are dropped.
	 */
	if ((held & KEIUI_WIN32_HELD_ALTGR) != 0)
		modifiers &= ~(unsigned)(KL_MOD_CTRL | KL_MOD_ALT);

	/* The modifiers Keiland sees. */
	return modifiers;
}

/*
 * Gives the evdev code of a mouse button.
 */
uint32_t
keiui_win32_button_code(
	unsigned button)
{
	/* Chooses the code of the button; an unknown one has none. */
	switch (button) {
	case KEIUI_WIN32_BUTTON_LEFT:
		return KL_BUTTON_LEFT;
	case KEIUI_WIN32_BUTTON_RIGHT:
		return KL_BUTTON_RIGHT;
	case KEIUI_WIN32_BUTTON_MIDDLE:
		return KL_BUTTON_MIDDLE;
	case KEIUI_WIN32_BUTTON_X1:
		return EVDEV_BUTTON_SIDE;
	case KEIUI_WIN32_BUTTON_X2:
		return EVDEV_BUTTON_EXTRA;
	default:
		break;
	}

	/* No such button. */
	return 0;
}

/*
 * Reads a point from a mouse message's lParam.
 */
void
keiui_win32_point(
	int64_t lparam,
	int32_t *x,
	int32_t *y)
{
	uint64_t bits;

	/* Works on the bit pattern, so a negative lParam shifts predictably. */
	bits = (uint64_t)lparam;

	/* The low word is x. */
	if (x != NULL)
		*x = signed_word(bits & 0xFFFFU);

	/* The next word is y. */
	if (y != NULL)
		*y = signed_word((bits >> 16) & 0xFFFFU);
}

/*
 * Reads a key message's lParam.
 */
void
keiui_win32_key_flags(
	int64_t lparam,
	struct keiui_win32_key_flags *flags)
{
	uint64_t bits;

	/* Does nothing when there is nowhere to put the flags. */
	if (flags == NULL)
		return;

	bits = (uint64_t)lparam;

	/* Bits 16 to 23 are the scan code. */
	flags->scan = (unsigned)((bits >> 16) & 0xFFU);

	/* Bit 24 says the key is an extended one (right Alt, the arrows). */
	flags->extended = (int)((bits >> 24) & 1U);

	/* Bit 30 is the previous state of the key, reported as it is. */
	flags->repeated = (int)((bits >> 30) & 1U);

	/* Bit 31 is set when the message is for a key going up. */
	flags->released = (int)((bits >> 31) & 1U);
}

/*
 * Reads a 16-bit word as a signed number.
 */
static int32_t
signed_word(
	uint64_t word)
{
	/* A word with its top bit set is negative in two's complement. */
	if ((word & WORD_SIGN_BIT) != 0)
		return (int32_t)word - WORD_RANGE;

	/* A word without it is the number itself. */
	return (int32_t)word;
}
