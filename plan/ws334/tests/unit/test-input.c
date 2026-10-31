/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The unit test of the pointer and keyboard details of the Windows backend
 * (win32/input.c): the wheel distance, the modifier keys, the mouse
 * buttons, the position and the key flags of a message's lParam.
 *
 * The expected values are worked out by hand from the Win32 definitions
 * (WHEEL_DELTA is 120, a notch scrolls 60 pixels, GET_X_LPARAM is the
 * signed low word, the key message bits are 16 to 23, 24, 30 and 31) and
 * from the evdev and Keiland constants, which are written out as numbers
 * here rather than taken from the headers under test.
 */

#include "check.h"

#include "userland/desktop/libkeiland/win32/input.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The Keiland modifier bits, as numbers: shift, control, alt, super. */
#define MOD_SHIFT		1U
#define MOD_CTRL		2U
#define MOD_ALT			4U
#define MOD_SUPER		8U

/* The held bits of the header, as numbers. */
#define HELD_SHIFT		0x01U
#define HELD_CONTROL		0x02U
#define HELD_ALT		0x04U
#define HELD_WIN		0x08U
#define HELD_ALTGR		0x10U

static void test_wheel(void);
static void test_modifiers(void);
static void test_button(void);
static void test_point(void);
static void test_key_flags(void);

int
main(
	int argc,
	char **argv)
{
	/* The harness passes the system's name. */
	CHECK(argc == 2);
	(void)argv;

	test_wheel();
	test_modifiers();
	test_button();
	test_point();
	test_key_flags();

	return check_finish("input");
}

/* Checks that a double is exactly the expected one and not a negative zero. */
#define CHECK_PIXELS(actual, expected)							\
	do {										\
		double value = (actual);						\
		CHECK(value == (expected));						\
		CHECK((expected) != 0.0 || 1.0 / value > 0.0);				\
	} while (0)

/*
 * One notch is 120 units and 60 pixels; the vertical wheel is negated.
 */
static void
test_wheel(void)
{
	int delta;

	/* The constants the header promises. */
	CHECK_EQ(KEIUI_WIN32_WHEEL_DELTA, 120);
	CHECK(KEIUI_WIN32_NOTCH_PIXELS == 60.0);

	/* Vertical: a positive delta scrolls up, a negative distance. */
	CHECK_PIXELS(keiui_win32_wheel_pixels(120, 0), -60.0);
	CHECK_PIXELS(keiui_win32_wheel_pixels(-120, 0), 60.0);
	CHECK_PIXELS(keiui_win32_wheel_pixels(240, 0), -120.0);
	CHECK_PIXELS(keiui_win32_wheel_pixels(-360, 0), 180.0);
	CHECK_PIXELS(keiui_win32_wheel_pixels(30, 0), -15.0);
	CHECK_PIXELS(keiui_win32_wheel_pixels(-30, 0), 15.0);
	CHECK_PIXELS(keiui_win32_wheel_pixels(1, 0), -0.5);
	CHECK_PIXELS(keiui_win32_wheel_pixels(60, 0), -30.0);

	/* Horizontal: the same sign. */
	CHECK_PIXELS(keiui_win32_wheel_pixels(120, 1), 60.0);
	CHECK_PIXELS(keiui_win32_wheel_pixels(-120, 1), -60.0);
	CHECK_PIXELS(keiui_win32_wheel_pixels(30, 1), 15.0);
	CHECK_PIXELS(keiui_win32_wheel_pixels(-30, 1), -15.0);
	CHECK_PIXELS(keiui_win32_wheel_pixels(1, 1), 0.5);

	/* Any nonzero value of horizontal means horizontal. */
	CHECK_PIXELS(keiui_win32_wheel_pixels(120, 2), 60.0);
	CHECK_PIXELS(keiui_win32_wheel_pixels(120, -1), 60.0);

	/* Zero is zero for both, without a sign. */
	CHECK_PIXELS(keiui_win32_wheel_pixels(0, 0), 0.0);
	CHECK_PIXELS(keiui_win32_wheel_pixels(0, 1), 0.0);

	/* The extremes of a signed 16-bit delta: 32767 and -32768 (=273.058... notches). */
	CHECK(keiui_win32_wheel_pixels(32767, 0) == -(32767.0 * 60.0 / 120.0));
	CHECK(keiui_win32_wheel_pixels(-32768, 0) == 16384.0);
	CHECK(keiui_win32_wheel_pixels(-32768, 1) == -16384.0);

	/* The distance is linear in the delta and vertical is the horizontal one negated. */
	for (delta = -1000; delta <= 1000; delta += 7) {
		double vertical = keiui_win32_wheel_pixels(delta, 0);
		double horizontal = keiui_win32_wheel_pixels(delta, 1);

		CHECK(horizontal == (double)delta / 2.0);
		CHECK(vertical == -horizontal);
	}
}

/*
 * Each held bit maps to its Keiland bit; AltGr takes Control and Alt away.
 */
static void
test_modifiers(void)
{
	unsigned held;
	unsigned expected;

	/* Nothing held. */
	CHECK_EQ(keiui_win32_modifiers(0), 0);

	/* One at a time. */
	CHECK_EQ(keiui_win32_modifiers(HELD_SHIFT), MOD_SHIFT);
	CHECK_EQ(keiui_win32_modifiers(HELD_CONTROL), MOD_CTRL);
	CHECK_EQ(keiui_win32_modifiers(HELD_ALT), MOD_ALT);
	CHECK_EQ(keiui_win32_modifiers(HELD_WIN), MOD_SUPER);

	/* AltGr alone gives none: the character it types is not a shortcut. */
	CHECK_EQ(keiui_win32_modifiers(HELD_ALTGR), 0);

	/* Combinations. */
	CHECK_EQ(keiui_win32_modifiers(HELD_SHIFT | HELD_CONTROL), MOD_SHIFT | MOD_CTRL);
	CHECK_EQ(keiui_win32_modifiers(HELD_CONTROL | HELD_ALT), MOD_CTRL | MOD_ALT);
	CHECK_EQ(keiui_win32_modifiers(HELD_SHIFT | HELD_CONTROL | HELD_ALT | HELD_WIN),
	    MOD_SHIFT | MOD_CTRL | MOD_ALT | MOD_SUPER);

	/* Windows reports AltGr as Control and Alt as well: both go, Shift and Win stay. */
	CHECK_EQ(keiui_win32_modifiers(HELD_ALTGR | HELD_CONTROL | HELD_ALT), 0);
	CHECK_EQ(keiui_win32_modifiers(HELD_ALTGR | HELD_CONTROL), 0);
	CHECK_EQ(keiui_win32_modifiers(HELD_ALTGR | HELD_ALT), 0);
	CHECK_EQ(keiui_win32_modifiers(HELD_ALTGR | HELD_CONTROL | HELD_ALT | HELD_SHIFT), MOD_SHIFT);
	CHECK_EQ(keiui_win32_modifiers(HELD_ALTGR | HELD_CONTROL | HELD_ALT | HELD_WIN), MOD_SUPER);
	CHECK_EQ(keiui_win32_modifiers(HELD_ALTGR | HELD_CONTROL | HELD_ALT | HELD_SHIFT | HELD_WIN), MOD_SHIFT | MOD_SUPER);

	/* Unknown bits are ignored, alone and with known ones. */
	CHECK_EQ(keiui_win32_modifiers(0x20U), 0);
	CHECK_EQ(keiui_win32_modifiers(0x80000000U), 0);
	CHECK_EQ(keiui_win32_modifiers(0xFFFFFFE0U), 0);
	CHECK_EQ(keiui_win32_modifiers(0xFFFFFFE0U | HELD_CONTROL), MOD_CTRL);

	/* Every value of the low byte, against a table made bit by bit. */
	for (held = 0; held < 256; held++) {
		expected = 0;
		if ((held & HELD_SHIFT) != 0)
			expected |= MOD_SHIFT;
		if ((held & HELD_WIN) != 0)
			expected |= MOD_SUPER;
		if ((held & HELD_ALTGR) == 0) {
			if ((held & HELD_CONTROL) != 0)
				expected |= MOD_CTRL;
			if ((held & HELD_ALT) != 0)
				expected |= MOD_ALT;
		}
		CHECK_EQ(keiui_win32_modifiers(held), expected);
	}
}

/*
 * The five buttons give their evdev codes; the rest give 0.
 */
static void
test_button(void)
{
	/* The header's numbers for the buttons. */
	CHECK_EQ(KEIUI_WIN32_BUTTON_LEFT, 0);
	CHECK_EQ(KEIUI_WIN32_BUTTON_RIGHT, 1);
	CHECK_EQ(KEIUI_WIN32_BUTTON_MIDDLE, 2);
	CHECK_EQ(KEIUI_WIN32_BUTTON_X1, 3);
	CHECK_EQ(KEIUI_WIN32_BUTTON_X2, 4);

	/* BTN_LEFT 0x110, BTN_RIGHT 0x111, BTN_MIDDLE 0x112, BTN_SIDE 0x113, BTN_EXTRA 0x114. */
	CHECK_EQ(keiui_win32_button_code(0), 0x110);
	CHECK_EQ(keiui_win32_button_code(1), 0x111);
	CHECK_EQ(keiui_win32_button_code(2), 0x112);
	CHECK_EQ(keiui_win32_button_code(3), 0x113);
	CHECK_EQ(keiui_win32_button_code(4), 0x114);

	/* Anything else is unknown. */
	CHECK_EQ(keiui_win32_button_code(5), 0);
	CHECK_EQ(keiui_win32_button_code(6), 0);
	CHECK_EQ(keiui_win32_button_code(0x110), 0);
	CHECK_EQ(keiui_win32_button_code(0xFFFFFFFFU), 0);
}

/*
 * x is the signed low word and y the signed high word.
 */
static void
test_point(void)
{
	int32_t x;
	int32_t y;
	int64_t lparam;

	/* The example: 0x0010FFF0 is x = 0xFFF0 = -16 and y = 0x0010 = 16. */
	x = 7777;
	y = 7777;
	keiui_win32_point(0x0010FFF0, &x, &y);
	CHECK_EQ(x, -16);
	CHECK_EQ(y, 16);

	/* Plain positive values. */
	keiui_win32_point(0x00C8012C, &x, &y);
	CHECK_EQ(x, 300);
	CHECK_EQ(y, 200);

	/* Zero. */
	keiui_win32_point(0, &x, &y);
	CHECK_EQ(x, 0);
	CHECK_EQ(y, 0);

	/* The edges of a signed word: 0x7FFF is 32767, 0x8000 is -32768, 0xFFFF is -1. */
	keiui_win32_point(0x7FFF7FFF, &x, &y);
	CHECK_EQ(x, 32767);
	CHECK_EQ(y, 32767);
	keiui_win32_point(0x80008000, &x, &y);
	CHECK_EQ(x, -32768);
	CHECK_EQ(y, -32768);
	keiui_win32_point(0xFFFFFFFF, &x, &y);
	CHECK_EQ(x, -1);
	CHECK_EQ(y, -1);

	/* A negative y with a positive x: y = 0xFF38 = -200, x = 100. */
	keiui_win32_point(0xFF380064LL, &x, &y);
	CHECK_EQ(x, 100);
	CHECK_EQ(y, -200);

	/* LPARAM on a 64-bit Windows is sign-extended: the bits above 31 do not matter. */
	lparam = (int64_t)(int32_t)0xFF38FF9C;
	keiui_win32_point(lparam, &x, &y);
	CHECK_EQ(x, -100);
	CHECK_EQ(y, -200);
	lparam = (int64_t)0x7FFFFFFF00100020LL;
	keiui_win32_point(lparam, &x, &y);
	CHECK_EQ(x, 32);
	CHECK_EQ(y, 16);
	lparam = (int64_t)-1;
	keiui_win32_point(lparam, &x, &y);
	CHECK_EQ(x, -1);
	CHECK_EQ(y, -1);

	/* NULL outputs are not written, and the other one still is. */
	x = 7777;
	y = 7777;
	keiui_win32_point(0x0010FFF0, NULL, &y);
	CHECK_EQ(x, 7777);
	CHECK_EQ(y, 16);
	y = 7777;
	keiui_win32_point(0x0010FFF0, &x, NULL);
	CHECK_EQ(x, -16);
	CHECK_EQ(y, 7777);
	keiui_win32_point(0x0010FFF0, NULL, NULL);
}

/* Checks the four fields of the flags of an lParam. */
#define CHECK_FLAGS(lparam_value, scan_value, extended_value, repeated_value, released_value)	\
	do {											\
		struct keiui_win32_key_flags flags;						\
		memset(&flags, 0x5A, sizeof(flags));						\
		keiui_win32_key_flags((lparam_value), &flags);					\
		CHECK_EQ(flags.scan, (scan_value));						\
		CHECK_EQ(flags.extended, (extended_value));					\
		CHECK_EQ(flags.repeated, (repeated_value));					\
		CHECK_EQ(flags.released, (released_value));					\
	} while (0)

/*
 * Scan code in bits 16 to 23, extended in 24, repeated in 30, released in 31.
 */
static void
test_key_flags(void)
{
	struct keiui_win32_key_flags flags;
	int64_t all;

	/* Nothing set. */
	CHECK_FLAGS(0, 0, 0, 0, 0);

	/* The scan code alone: 0x1E is A, 0xFF the largest. */
	CHECK_FLAGS(0x001E0001LL, 0x1E, 0, 0, 0);
	CHECK_FLAGS(0x00FF0000LL, 0xFF, 0, 0, 0);
	CHECK_FLAGS(0x00010000LL, 0x01, 0, 0, 0);

	/* The low 16 bits (the repeat count) do not matter. */
	CHECK_FLAGS(0x0000FFFFLL, 0, 0, 0, 0);

	/* Each flag alone. */
	CHECK_FLAGS(0x01000000LL, 0, 1, 0, 0);
	CHECK_FLAGS(0x40000000LL, 0, 0, 1, 0);
	CHECK_FLAGS(0x80000000LL, 0, 0, 0, 1);

	/* Bits next to the flags are not the flags: 23 is scan, 25 to 29 are others. */
	CHECK_FLAGS(0x00800000LL, 0x80, 0, 0, 0);
	CHECK_FLAGS(0x3E000000LL, 0, 0, 0, 0);
	CHECK_FLAGS(0x20000000LL, 0, 0, 0, 0);

	/* A key up, as Windows sends it: bit 30 and 31 set together with the scan code. */
	CHECK_FLAGS(0xC0390001LL, 0x39, 0, 1, 1);

	/* An extended key (right Alt: scan 0x38, extended) going down for the first time. */
	CHECK_FLAGS(0x01380001LL, 0x38, 1, 0, 0);

	/* Auto-repeat of an extended key: previous state set. */
	CHECK_FLAGS(0x41380005LL, 0x38, 1, 1, 0);

	/* A key up of an extended key. */
	CHECK_FLAGS(0xC1380001LL, 0x38, 1, 1, 1);

	/* All 32 bits set. */
	CHECK_FLAGS(0xFFFFFFFFLL, 0xFF, 1, 1, 1);

	/* All 64 bits set (sign-extended LPARAM of a key up). */
	all = -1;
	CHECK_FLAGS(all, 0xFF, 1, 1, 1);
	CHECK_FLAGS((int64_t)(int32_t)0xC0390001, 0x39, 0, 1, 1);

	/* Bits above 31 do not matter. */
	CHECK_FLAGS(0x7FFFFFFF00000000LL, 0, 0, 0, 0);
	CHECK_FLAGS(0x7FFFFFFF001E0000LL, 0x1E, 0, 0, 0);

	/* NULL flags does nothing. */
	keiui_win32_key_flags(0x01380001LL, NULL);

	/* The fields are exactly 0 or 1 for the flags, not the masked bit. */
	memset(&flags, 0x5A, sizeof(flags));
	keiui_win32_key_flags(0xFFFFFFFFLL, &flags);
	CHECK(flags.extended == 1);
	CHECK(flags.repeated == 1);
	CHECK(flags.released == 1);
}
