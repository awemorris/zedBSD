/* WS006 IN-T31: X68000 physical make/break/repeat preservation. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "hal/m68k/bsp-x68k/keyboard-map.h"
#include "kern/input-keymap.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <zedbsd/input.h>

static unsigned
key(unsigned event)
{
	return event & X68K_KEY_EVENT_KEY_MASK;
}

int
main(void)
{
	struct x68k_keyboard_state first, second;
	struct input_keymap_state keymap;
	struct hal_key_event translated_event;
	uint32_t translated;
	int event;

	x68k_keyboard_state_reset(&first);
	x68k_keyboard_state_reset(&second);
	event = x68k_keyboard_feed(&first, 0x70U);
	assert(key((unsigned)event) == X68K_KEY_SHIFT);
	assert(((unsigned)event & X68K_KEY_EVENT_SHIFT) != 0);
	event = x68k_keyboard_feed(&second, 0x1eU);
	assert(key((unsigned)event) == 'a');
	assert(((unsigned)event & X68K_KEY_EVENT_SHIFT) == 0);
	event = x68k_keyboard_feed(&first, 0x03U);
	assert(key((unsigned)event) == X68K_KEY_JIS_2);
	assert(((unsigned)event & X68K_KEY_EVENT_REPEAT) == 0);
	assert(x68k_keyboard_key_state(&first, '2') == 1);
	input_keymap_init(&keymap);
	memset(&translated_event, 0, sizeof(translated_event));
	strcpy(translated_event.symbol, "leftshift");
	translated_event.flags = HAL_KEY_EVENT_PRESS;
	assert(input_keymap_translate(&keymap, &translated_event, &translated));
	strcpy(translated_event.symbol, "jis-2");
	assert(input_keymap_translate(&keymap, &translated_event, &translated));
	assert((translated & INPUT_KEY_MASK) == '"');
	event = x68k_keyboard_feed(&first, 0x03U);
	assert(key((unsigned)event) == X68K_KEY_JIS_2);
	assert(((unsigned)event & X68K_KEY_EVENT_REPEAT) != 0);
	event = x68k_keyboard_feed(&first, 0x83U);
	assert(key((unsigned)event) == X68K_KEY_JIS_2);
	assert(((unsigned)event & X68K_KEY_EVENT_RELEASE) != 0);
	assert(x68k_keyboard_key_state(&first, '2') == 0);
	event = x68k_keyboard_feed(&first, 0x1eU);
	assert(key((unsigned)event) == 'a');
	assert(((unsigned)event & X68K_KEY_EVENT_REPEAT) == 0);
	event = x68k_keyboard_feed(&first, 0x1eU);
	assert(key((unsigned)event) == 'a');
	assert(((unsigned)event & X68K_KEY_EVENT_REPEAT) != 0);
	event = x68k_keyboard_feed(&first, 0x9eU);
	assert(key((unsigned)event) == 'a');
	assert(((unsigned)event & X68K_KEY_EVENT_RELEASE) != 0);
	event = x68k_keyboard_feed(&first, 0xf0U);
	assert(key((unsigned)event) == X68K_KEY_SHIFT);
	assert(((unsigned)event & X68K_KEY_EVENT_RELEASE) != 0);
	assert(x68k_keyboard_key_state(&first, X68K_KEY_SHIFT) == 0);
	assert(x68k_keyboard_key_state(&second, 'a') == 1);
	assert(input_key_from_symbol("jis-rbrace") == KEY_BACKSLASH);
	assert(input_key_from_symbol("jis-yen") == KEY_RESERVED);
	assert(input_key_from_symbol("jis-kp-2") == KEY_RESERVED);
	assert(input_key_symbol_supported("jis-kp-2"));
	puts("WS006 X68000 physical keyboard ownership: PASS");
	return 0;
}
