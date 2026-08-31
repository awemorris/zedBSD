/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_KERN_INPUT_KEYMAP_H
#define ZEDBSD_KERN_INPUT_KEYMAP_H

#include <stdint.h>
#include <hal/hal.h>

#define INPUT_KEY_MASK       0x000001ffU
#define INPUT_KEY_SHIFT      0x00010000U
#define INPUT_KEY_CTRL       0x00020000U
#define INPUT_KEY_GRAPH      0x00040000U
#define INPUT_KEY_RELEASE    0x00080000U

#define INPUT_KEY_ESCAPE     0x1bU
#define INPUT_KEY_BACKSPACE  0x08U
#define INPUT_KEY_TAB        0x09U
#define INPUT_KEY_ENTER      0x0dU
#define INPUT_KEY_PAGE_UP    0x136U
#define INPUT_KEY_PAGE_DOWN  0x137U
#define INPUT_KEY_INSERT     0x138U
#define INPUT_KEY_DELETE     0x139U
#define INPUT_KEY_UP         0x13aU
#define INPUT_KEY_LEFT       0x13bU
#define INPUT_KEY_RIGHT      0x13cU
#define INPUT_KEY_DOWN       0x13dU
#define INPUT_KEY_HOME       0x13eU
#define INPUT_KEY_END        0x13fU
#define INPUT_KEY_F1         0x162U
#define INPUT_KEY_F4         0x165U
#define INPUT_KEY_F10        0x16bU

#define INPUT_KEY_SHIFT_SYMBOL 0x170U
#define INPUT_KEY_CAPS_LOCK    0x171U
#define INPUT_KEY_KANA         0x172U
#define INPUT_KEY_GRAPH_SYMBOL 0x173U
#define INPUT_KEY_CTRL_SYMBOL  0x174U

struct input_keymap_state {
	uint8_t left_shift;
	uint8_t right_shift;
	uint8_t left_control;
	uint8_t right_control;
	uint8_t left_graph;
	uint8_t right_graph;
	uint8_t caps_lock;
	uint8_t kana_lock;
};

void input_keymap_init(struct input_keymap_state *);
uint16_t input_key_from_symbol(const char *);
int input_key_symbol_supported(const char *);
int input_keymap_event_from_code(uint16_t, int32_t,
	struct hal_key_event *);
int input_keymap_translate(struct input_keymap_state *,
	const struct hal_key_event *, uint32_t *);

#endif
