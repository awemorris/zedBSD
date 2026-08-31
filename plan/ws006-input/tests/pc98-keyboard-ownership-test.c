/* WS006 IN-T33: PC-98 physical make/break/repeat preservation. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "hal/i386/bsp-pc98/keyboard-map.h"
#include "kern/input-keymap.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <zedbsd/input.h>

static unsigned
key(unsigned event)
{
	return event & HAL_KEY_EVENT_KEY_MASK;
}

int
main(void)
{
	struct pc98_keyboard first, second;
	struct input_keymap_state keymap;
	struct hal_key_event translated_event;
	uint32_t translated;
	unsigned event;

	pc98_keyboard_reset(&first);
	pc98_keyboard_reset(&second);
	assert(pc98_keyboard_feed(&first, 0x70U, &event));
	assert(key(event) == HAL_KEY_SHIFT);
	assert((event & HAL_KEY_EVENT_SHIFT_PRIVATE) != 0);
	assert(pc98_keyboard_feed(&second, 0x02U, &event));
	assert(key(event) == HAL_KEY_JIS_2);
	assert((event & HAL_KEY_EVENT_SHIFT_PRIVATE) == 0);
	assert(pc98_keyboard_is_down(&second, '2') == 1);
	assert(pc98_keyboard_feed(&first, 0x02U, &event));
	assert(key(event) == HAL_KEY_JIS_2);
	assert((event & HAL_KEY_EVENT_REPEAT_PRIVATE) == 0);
	assert(input_key_from_symbol("jis-2") == KEY_2);
	input_keymap_init(&keymap);
	memset(&translated_event, 0, sizeof(translated_event));
	strcpy(translated_event.symbol, "leftshift");
	translated_event.flags = HAL_KEY_EVENT_PRESS;
	assert(input_keymap_translate(&keymap, &translated_event, &translated));
	strcpy(translated_event.symbol, "jis-2");
	assert(input_keymap_translate(&keymap, &translated_event, &translated));
	assert((translated & INPUT_KEY_MASK) == '"');
	assert(pc98_keyboard_feed(&first, 0x02U, &event));
	assert(key(event) == HAL_KEY_JIS_2);
	assert((event & HAL_KEY_EVENT_REPEAT_PRIVATE) != 0);
	assert(pc98_keyboard_feed(&first, 0x82U, &event));
	assert(key(event) == HAL_KEY_JIS_2);
	assert((event & HAL_KEY_EVENT_RELEASE_PRIVATE) != 0);
	assert(pc98_keyboard_is_down(&first, '2') == 0);
	assert(pc98_keyboard_feed(&first, 0xf0U, &event));
	assert(key(event) == HAL_KEY_SHIFT);
	assert((event & HAL_KEY_EVENT_RELEASE_PRIVATE) != 0);

	/* Locking keys toggle only on the first make, not typematic repeats. */
	assert(pc98_keyboard_feed(&first, 0x71U, &event));
	assert(key(event) == HAL_KEY_CAPS_LOCK);
	assert(pc98_keyboard_feed(&first, 0x71U, &event));
	assert((event & HAL_KEY_EVENT_REPEAT_PRIVATE) != 0);
	assert(first.caps == 1);
	assert(pc98_keyboard_feed(&first, 0x1dU, &event));
	assert(key(event) == 'a');
	assert(pc98_keyboard_feed(&first, 0x9dU, &event));
	assert(key(event) == 'a');
	assert((event & HAL_KEY_EVENT_RELEASE_PRIVATE) != 0);
	assert(pc98_keyboard_feed(&first, 0xf1U, &event));
	assert(key(event) == HAL_KEY_CAPS_LOCK);
	assert((event & HAL_KEY_EVENT_RELEASE_PRIVATE) != 0);
	assert(pc98_keyboard_is_down(&first, HAL_KEY_CAPS_LOCK) == 0);
	assert(input_key_from_symbol("jis-rbrace") == KEY_BACKSLASH);
	assert(input_key_from_symbol("jis-yen") == KEY_RESERVED);
	assert(input_key_symbol_supported("kana"));
	puts("WS006 PC-98 physical keyboard ownership: PASS");
	return 0;
}
