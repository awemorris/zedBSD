/* X68000 keyboard scan-code translation. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#ifndef ZEDBSD_HAL_M68K_X68K_KEYBOARD_MAP_H
#define ZEDBSD_HAL_M68K_X68K_KEYBOARD_MAP_H

#include <stdint.h>

#define X68K_KEY_ESCAPE 0x1b
#define X68K_KEY_BACKSPACE 0x08
#define X68K_KEY_TAB 0x09
#define X68K_KEY_ENTER 0x0d
#define X68K_KEY_PAGE_UP 0x136
#define X68K_KEY_PAGE_DOWN 0x137
#define X68K_KEY_DELETE 0x139
#define X68K_KEY_UP 0x13a
#define X68K_KEY_LEFT 0x13b
#define X68K_KEY_RIGHT 0x13c
#define X68K_KEY_DOWN 0x13d
#define X68K_KEY_HOME 0x13e
#define X68K_KEY_END 0x13f
#define X68K_KEY_F1 0x162
#define X68K_KEY_F2 0x163
#define X68K_KEY_F3 0x164
#define X68K_KEY_F4 0x165
#define X68K_KEY_F5 0x166
#define X68K_KEY_F6 0x167
#define X68K_KEY_F7 0x168
#define X68K_KEY_F8 0x169
#define X68K_KEY_F9 0x16a
#define X68K_KEY_F10 0x16b
#define X68K_KEY_SHIFT 0x170
#define X68K_KEY_EVENT_SHIFT 0x00010000U
#define X68K_KEY_EVENT_CTRL 0x00020000U
#define X68K_KEY_EVENT_GRAPH 0x00040000U
#define X68K_KEY_EVENT_KEY_MASK 0x000001ffU

struct x68k_keyboard_state {
	uint8_t down[16];
	uint8_t caps_lock;
};

void x68k_keyboard_state_reset(struct x68k_keyboard_state *);
int x68k_keyboard_feed(struct x68k_keyboard_state *, uint8_t);
int x68k_keyboard_key_state(const struct x68k_keyboard_state *, int);
unsigned x68k_keyboard_modifiers(const struct x68k_keyboard_state *);

#endif
