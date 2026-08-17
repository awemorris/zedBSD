/* Host tests for X68000 JIS keyboard translation. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <hal/hal.h>
#include <stdio.h>
#include "src/hal/m68k/bsp-x68k/keyboard-map.h"

#define CHECK(x) do { if (!(x)) { \
	fprintf(stderr, "FAIL:%d: %s\n", __LINE__, #x); return 1; \
} } while (0)

int
main(void)
{
	struct x68k_keyboard_state state;
	int event;

	x68k_keyboard_state_reset(&state);
	CHECK(x68k_keyboard_feed(&state, 0x11U) == 'q');
	CHECK(x68k_keyboard_feed(&state, 0x91U) < 0);
	CHECK(x68k_keyboard_feed(&state, 0x70U) < 0);
	event = x68k_keyboard_feed(&state, 0x11U);
	CHECK((event & HAL_KEY_EVENT_KEY_MASK) == 'Q');
	CHECK((event & HAL_KEY_EVENT_SHIFT) != 0);
	CHECK(x68k_keyboard_key_state(&state, HAL_KEY_SHIFT) == 1);
	CHECK(x68k_keyboard_feed(&state, 0xf0U) < 0);
	CHECK(x68k_keyboard_feed(&state, 0x71U) < 0);
	event = x68k_keyboard_feed(&state, 0x2cU);
	CHECK((event & HAL_KEY_EVENT_KEY_MASK) == 3); /* Ctrl-C. */
	CHECK((event & HAL_KEY_EVENT_CTRL) != 0);
	CHECK(x68k_keyboard_feed(&state, 0xf1U) < 0);
	CHECK(x68k_keyboard_feed(&state, 0x5dU) < 0);
	CHECK(x68k_keyboard_feed(&state, 0x1eU) == 'A');
	CHECK(x68k_keyboard_feed(&state, 0x9eU) < 0);
	CHECK(x68k_keyboard_feed(&state, 0x5dU) < 0);
	CHECK(x68k_keyboard_feed(&state, 0x1eU) == 'a');
	CHECK(x68k_keyboard_feed(&state, 0x70U) < 0);
	CHECK((x68k_keyboard_feed(&state, 0x33U) &
	    HAL_KEY_EVENT_KEY_MASK) == '?');
	CHECK(x68k_keyboard_feed(&state, 0xf0U) < 0);
	CHECK(x68k_keyboard_feed(&state, 0x3bU) == HAL_KEY_LEFT);
	CHECK(x68k_keyboard_feed(&state, 0x63U) == HAL_KEY_F1);
	puts("X68k keyboard translation host tests passed");
	return 0;
}
