/* X68000 keyboard scan-code translation. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#ifndef ZEDBSD_HAL_M68K_X68K_KEYBOARD_MAP_H
#define ZEDBSD_HAL_M68K_X68K_KEYBOARD_MAP_H

#include <stdint.h>

struct x68k_keyboard_state {
	uint8_t down[16];
	uint8_t caps_lock;
};

void x68k_keyboard_state_reset(struct x68k_keyboard_state *);
int x68k_keyboard_feed(struct x68k_keyboard_state *, uint8_t);
int x68k_keyboard_key_state(const struct x68k_keyboard_state *, int);
unsigned x68k_keyboard_modifiers(const struct x68k_keyboard_state *);

#endif
