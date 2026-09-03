/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PC-98 keyboard scan-code translation contract.
 *
 * Focused host tests share this private state and event representation.
 */

#ifndef ZEDBSD_HAL_I386_PC98_KEYBOARD_MAP_H
#define ZEDBSD_HAL_I386_PC98_KEYBOARD_MAP_H

#include <stdint.h>

#define HAL_KEY_ESCAPE 0x1b
#define HAL_KEY_BACKSPACE 0x08
#define HAL_KEY_TAB 0x09
#define HAL_KEY_ENTER 0x0d
#define HAL_KEY_PAGE_UP 0x136
#define HAL_KEY_PAGE_DOWN 0x137
#define HAL_KEY_INSERT 0x138
#define HAL_KEY_DELETE 0x139
#define HAL_KEY_UP 0x13a
#define HAL_KEY_LEFT 0x13b
#define HAL_KEY_RIGHT 0x13c
#define HAL_KEY_DOWN 0x13d
#define HAL_KEY_HOME 0x13e
#define HAL_KEY_END 0x13f
#define HAL_KEY_F1 0x162
#define HAL_KEY_F2 0x163
#define HAL_KEY_F3 0x164
#define HAL_KEY_F4 0x165
#define HAL_KEY_F5 0x166
#define HAL_KEY_F6 0x167
#define HAL_KEY_F7 0x168
#define HAL_KEY_F8 0x169
#define HAL_KEY_F9 0x16a
#define HAL_KEY_F10 0x16b
#define HAL_KEY_SHIFT_L 0x170
#define HAL_KEY_CAPS_LOCK 0x171
#define HAL_KEY_KANA 0x172
#define HAL_KEY_GRAPH 0x173
#define HAL_KEY_CTRL 0x174
#define HAL_KEY_SHIFT_R 0x175
#define HAL_KEY_SHIFT HAL_KEY_SHIFT_L
#define HAL_KEY_JIS_1 0x180
#define HAL_KEY_JIS_2 0x181
#define HAL_KEY_JIS_3 0x182
#define HAL_KEY_JIS_4 0x183
#define HAL_KEY_JIS_5 0x184
#define HAL_KEY_JIS_6 0x185
#define HAL_KEY_JIS_7 0x186
#define HAL_KEY_JIS_8 0x187
#define HAL_KEY_JIS_9 0x188
#define HAL_KEY_JIS_0 0x189
#define HAL_KEY_JIS_MINUS 0x18a
#define HAL_KEY_JIS_CARET 0x18b
#define HAL_KEY_JIS_YEN 0x18c
#define HAL_KEY_JIS_AT 0x18d
#define HAL_KEY_JIS_LBRACE 0x18e
#define HAL_KEY_JIS_SEMI 0x18f
#define HAL_KEY_JIS_COLON 0x190
#define HAL_KEY_JIS_RBRACE 0x191
#define HAL_KEY_JIS_COMMA 0x192
#define HAL_KEY_JIS_DOT 0x193
#define HAL_KEY_JIS_SLASH 0x194
#define HAL_KEY_JIS_RO 0x195
#define HAL_KEY_EVENT_KEY_MASK 0x000001ffU
#define HAL_KEY_EVENT_SHIFT_PRIVATE 0x00010000U
#define HAL_KEY_EVENT_CTRL_PRIVATE 0x00020000U
#define HAL_KEY_EVENT_GRAPH_PRIVATE 0x00040000U
#define HAL_KEY_EVENT_RELEASE_PRIVATE 0x00080000U
#define HAL_KEY_EVENT_REPEAT_PRIVATE 0x00100000U

struct pc98_keyboard {
	uint8_t shift;
	uint8_t ctrl;
	uint8_t graph;
	uint8_t caps;
	uint8_t kana;
	uint8_t down[16];
	uint16_t last_key[128];
};

void pc98_keyboard_reset(struct pc98_keyboard *);
int pc98_keyboard_feed(struct pc98_keyboard *, uint8_t, unsigned *);
int pc98_keyboard_is_down(const struct pc98_keyboard *, int);

#endif
