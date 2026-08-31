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
#define X68K_KEY_CAPS_LOCK 0x171
#define X68K_KEY_GRAPH 0x173
#define X68K_KEY_CTRL 0x174
#define X68K_KEY_JIS_1 0x180
#define X68K_KEY_JIS_2 0x181
#define X68K_KEY_JIS_3 0x182
#define X68K_KEY_JIS_4 0x183
#define X68K_KEY_JIS_5 0x184
#define X68K_KEY_JIS_6 0x185
#define X68K_KEY_JIS_7 0x186
#define X68K_KEY_JIS_8 0x187
#define X68K_KEY_JIS_9 0x188
#define X68K_KEY_JIS_0 0x189
#define X68K_KEY_JIS_MINUS 0x18a
#define X68K_KEY_JIS_CARET 0x18b
#define X68K_KEY_JIS_YEN 0x18c
#define X68K_KEY_JIS_AT 0x18d
#define X68K_KEY_JIS_LBRACE 0x18e
#define X68K_KEY_JIS_SEMI 0x18f
#define X68K_KEY_JIS_COLON 0x190
#define X68K_KEY_JIS_RBRACE 0x191
#define X68K_KEY_JIS_COMMA 0x192
#define X68K_KEY_JIS_DOT 0x193
#define X68K_KEY_JIS_SLASH 0x194
#define X68K_KEY_JIS_RO 0x195
#define X68K_KEY_KP_SLASH 0x196
#define X68K_KEY_KP_STAR 0x197
#define X68K_KEY_KP_MINUS 0x198
#define X68K_KEY_KP_7 0x199
#define X68K_KEY_KP_8 0x19a
#define X68K_KEY_KP_9 0x19b
#define X68K_KEY_KP_PLUS 0x19c
#define X68K_KEY_KP_4 0x19d
#define X68K_KEY_KP_5 0x19e
#define X68K_KEY_KP_6 0x19f
#define X68K_KEY_KP_EQUAL 0x1a0
#define X68K_KEY_KP_1 0x1a1
#define X68K_KEY_KP_2 0x1a2
#define X68K_KEY_KP_3 0x1a3
#define X68K_KEY_KP_ENTER 0x1a4
#define X68K_KEY_KP_0 0x1a5
#define X68K_KEY_KP_COMMA 0x1a6
#define X68K_KEY_KP_DOT 0x1a7
#define X68K_KEY_EVENT_SHIFT 0x00010000U
#define X68K_KEY_EVENT_CTRL 0x00020000U
#define X68K_KEY_EVENT_GRAPH 0x00040000U
#define X68K_KEY_EVENT_RELEASE 0x00080000U
#define X68K_KEY_EVENT_REPEAT 0x00100000U
#define X68K_KEY_EVENT_KEY_MASK 0x000001ffU

struct x68k_keyboard_state {
	uint8_t down[16];
	uint8_t caps_lock;
	uint16_t last_key[128];
};

void x68k_keyboard_state_reset(struct x68k_keyboard_state *);
int x68k_keyboard_feed(struct x68k_keyboard_state *, uint8_t);
int x68k_keyboard_key_state(const struct x68k_keyboard_state *, int);
unsigned x68k_keyboard_modifiers(const struct x68k_keyboard_state *);

#endif
