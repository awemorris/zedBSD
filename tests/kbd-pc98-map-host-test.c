/*
 * NEC PC-98 keyboard translation host test
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include "drivers/kbd-pc98-map.h"

#include <noct/beui.h>

#include <stdio.h>

static int failures;

#define CHECK(expression)						\
	do {								\
		if (!(expression)) {					\
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__,	\
			       #expression);				\
			failures++;					\
		}							\
	} while (0)

static struct boots_kbd_pc98 kb;

/* Feed a make then a break, returning the make's key. */
static int
tap(uint8_t scan)
{
	int key = boots_kbd_pc98_feed(&kb, scan);

	boots_kbd_pc98_feed(&kb, scan | 0x80U);
	return key;
}

int
main(void)
{
	boots_kbd_pc98_reset(&kb);

	/* Plain letters and digits. */
	CHECK(tap(0x1d) == 'a');
	CHECK(tap(0x29) == 'z');
	CHECK(tap(0x01) == '1');
	CHECK(tap(0x0a) == '0');
	CHECK(tap(0x34) == ' ');

	/* Special keys map to the shared BeUI namespace. */
	CHECK(tap(0x00) == NOCT_BEUI_KEY_ESCAPE);
	CHECK(tap(0x1c) == NOCT_BEUI_KEY_ENTER);
	CHECK(tap(0x0f) == NOCT_BEUI_KEY_TAB);
	CHECK(tap(0x3a) == NOCT_BEUI_KEY_UP);
	CHECK(tap(0x62) == NOCT_BEUI_KEY_F1);
	CHECK(tap(0x6b) == NOCT_BEUI_KEY_F10);

	/* Releases and modifiers alone yield nothing. */
	CHECK(boots_kbd_pc98_feed(&kb, 0x1d | 0x80U) == 0);
	CHECK(boots_kbd_pc98_feed(&kb, 0x70) == 0);       /* shift make */

	/* Shift is held: letters upper-case, digits punctuate. */
	CHECK(tap(0x1d) == 'A');
	CHECK(tap(0x01) == '!');
	CHECK(tap(0x02) == '"');
	CHECK(boots_kbd_pc98_feed(&kb, 0x70 | 0x80U) == 0); /* shift break */
	CHECK(tap(0x1d) == 'a');

	/* Control collapses letters to control codes, held across taps. */
	CHECK(boots_kbd_pc98_feed(&kb, 0x74) == 0);       /* ctrl make */
	CHECK(tap(0x1d) == 1);                            /* ^A */
	CHECK(tap(0x11) == 23);                           /* ^W */
	CHECK(boots_kbd_pc98_feed(&kb, 0x74 | 0x80U) == 0);

	/* Caps lock toggles on make and flips letter case only. */
	CHECK(boots_kbd_pc98_feed(&kb, 0x71) == 0);       /* caps on */
	boots_kbd_pc98_feed(&kb, 0x71 | 0x80U);
	CHECK(tap(0x1d) == 'A');
	CHECK(tap(0x01) == '1');                          /* digit unaffected */
	CHECK(boots_kbd_pc98_feed(&kb, 0x71) == 0);       /* caps off */
	boots_kbd_pc98_feed(&kb, 0x71 | 0x80U);
	CHECK(tap(0x1d) == 'a');

	/* Real-time press state, as BeUI.isKeyDown reads it. */
	boots_kbd_pc98_reset(&kb);
	CHECK(boots_kbd_pc98_is_down(&kb, 'a') == 0);
	boots_kbd_pc98_feed(&kb, 0x1d);                   /* press a */
	CHECK(boots_kbd_pc98_is_down(&kb, 'a') == 1);
	CHECK(boots_kbd_pc98_is_down(&kb, 'A') == 1);     /* case-folded */
	boots_kbd_pc98_feed(&kb, 0x3a);                   /* press Up */
	CHECK(boots_kbd_pc98_is_down(&kb, NOCT_BEUI_KEY_UP) == 1);
	boots_kbd_pc98_feed(&kb, 0x1d | 0x80U);           /* release a */
	CHECK(boots_kbd_pc98_is_down(&kb, 'a') == 0);
	CHECK(boots_kbd_pc98_is_down(&kb, NOCT_BEUI_KEY_UP) == 1);
	/* A key with no PC-98 scancode reports -1. */
	CHECK(boots_kbd_pc98_is_down(&kb, 0x1ffff) == -1);

	if (failures != 0) {
		printf("kbd-pc98 tests: %d failure(s)\n", failures);
		return 1;
	}
	printf("Boots PC-98 keyboard translation host tests: OK\n");
	return 0;
}
