/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/input-keymap.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zedbsd/input.h>

static struct hal_key_event
key_event(const char *symbol, uint32_t flags)
{
	struct hal_key_event event;

	memset(&event, 0, sizeof(event));
	if (strlen(symbol) >= sizeof(event.symbol)) {
		fprintf(stderr, "IN-T20: test symbol too long\n");
		exit(1);
	}
	strcpy(event.symbol, symbol);
	event.flags = flags;
	return event;
}

static void
expect_evdev(const char *symbol, uint16_t expected)
{
	uint16_t actual = input_key_from_symbol(symbol);

	if (actual != expected) {
		fprintf(stderr, "IN-T20: %s: got %u, expected %u\n", symbol,
		    actual, expected);
		exit(1);
	}
}

static void
expect_tty(struct input_keymap_state *state, const char *symbol,
	uint32_t flags, uint32_t expected)
{
	struct hal_key_event event = key_event(symbol, flags);
	uint32_t actual = 0;

	if (!input_keymap_translate(state, &event, &actual) ||
	    actual != expected) {
		fprintf(stderr, "IN-T20: tty %s: got %#x, expected %#x\n",
		    symbol, actual, expected);
		exit(1);
	}
}

int
main(void)
{
	struct input_keymap_state state;
	struct hal_key_event invalid;
	uint32_t ignored;

	if (sizeof(((struct hal_key_event *)0)->symbol) != 16U ||
	    offsetof(struct hal_key_event, flags) != 16U) {
		fprintf(stderr, "IN-T20: hal_key_event layout changed\n");
		return 1;
	}
	expect_evdev("a", KEY_A);
	expect_evdev("1", KEY_1);
	expect_evdev("minus", KEY_MINUS);
	expect_evdev("leftshift", KEY_LEFTSHIFT);
	expect_evdev("rightshift", KEY_RIGHTSHIFT);
	expect_evdev("enter", KEY_ENTER);
	expect_evdev("left", KEY_LEFT);
	expect_evdev("f10", KEY_F10);
	expect_evdev("unknown", KEY_RESERVED);

	input_keymap_init(&state);
	expect_tty(&state, "leftshift", HAL_KEY_EVENT_PRESS,
	    INPUT_KEY_SHIFT_SYMBOL | INPUT_KEY_SHIFT);
	expect_tty(&state, "a", HAL_KEY_EVENT_PRESS, 'A' | INPUT_KEY_SHIFT);
	expect_tty(&state, "1", HAL_KEY_EVENT_REPEAT, '!' | INPUT_KEY_SHIFT);
	expect_tty(&state, "a", HAL_KEY_EVENT_RELEASE,
	    'A' | INPUT_KEY_SHIFT | INPUT_KEY_RELEASE);
	expect_tty(&state, "leftshift", HAL_KEY_EVENT_RELEASE,
	    INPUT_KEY_SHIFT_SYMBOL | INPUT_KEY_RELEASE);
	expect_tty(&state, "leftshift", HAL_KEY_EVENT_PRESS,
	    INPUT_KEY_SHIFT_SYMBOL | INPUT_KEY_SHIFT);
	expect_tty(&state, "rightshift", HAL_KEY_EVENT_PRESS,
	    INPUT_KEY_SHIFT_SYMBOL | INPUT_KEY_SHIFT);
	expect_tty(&state, "leftshift", HAL_KEY_EVENT_RELEASE,
	    INPUT_KEY_SHIFT_SYMBOL | INPUT_KEY_SHIFT | INPUT_KEY_RELEASE);
	expect_tty(&state, "a", HAL_KEY_EVENT_PRESS, 'A' | INPUT_KEY_SHIFT);
	expect_tty(&state, "rightshift", HAL_KEY_EVENT_RELEASE,
	    INPUT_KEY_SHIFT_SYMBOL | INPUT_KEY_RELEASE);
	expect_tty(&state, "capslock", HAL_KEY_EVENT_PRESS,
	    INPUT_KEY_CAPS_LOCK);
	expect_tty(&state, "a", HAL_KEY_EVENT_PRESS, 'A');

	memset(&invalid, 'x', sizeof(invalid));
	invalid.flags = HAL_KEY_EVENT_PRESS;
	if (input_keymap_translate(&state, &invalid, &ignored)) {
		fprintf(stderr, "IN-T20: unterminated symbol accepted\n");
		return 1;
	}
	puts("WS006 input keymap: PASS");
	return 0;
}
