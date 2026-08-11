/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef BOOTS_HAL_CONSOLE_H
#define BOOTS_HAL_CONSOLE_H

#include <hal/types.h>

enum hal_cons_mode {
	HAL_CONS_FIXED_MENU,
	HAL_CONS_TERMINAL,
};

#define HAL_CONS_COLUMNS 80U
#define HAL_CONS_ROWS 25U
#define HAL_CONS_NORMAL_ATTRIBUTE 0xe1U

enum hal_key {
	HAL_KEY_ESCAPE = 0x1b,
	HAL_KEY_BACKSPACE = 0x08,
	HAL_KEY_TAB = 0x09,
	HAL_KEY_ENTER = 0x0d,
	HAL_KEY_PAGE_UP = 0x136,
	HAL_KEY_PAGE_DOWN = 0x137,
	HAL_KEY_INSERT = 0x138,
	HAL_KEY_DELETE = 0x139,
	HAL_KEY_UP = 0x13a,
	HAL_KEY_LEFT = 0x13b,
	HAL_KEY_RIGHT = 0x13c,
	HAL_KEY_DOWN = 0x13d,
	HAL_KEY_HOME = 0x13e,
	HAL_KEY_END = 0x13f,
	HAL_KEY_F1 = 0x162,
	HAL_KEY_F2 = 0x163,
	HAL_KEY_F3 = 0x164,
	HAL_KEY_F4 = 0x165,
	HAL_KEY_F5 = 0x166,
	HAL_KEY_F6 = 0x167,
	HAL_KEY_F7 = 0x168,
	HAL_KEY_F8 = 0x169,
	HAL_KEY_F9 = 0x16a,
	HAL_KEY_F10 = 0x16b,
	HAL_KEY_SHIFT = 0x170,
};

#define HAL_KEY_EVENT_KEY_MASK 0x000001ffU
#define HAL_KEY_EVENT_SHIFT    0x00010000U
#define HAL_KEY_EVENT_CTRL     0x00020000U
#define HAL_KEY_EVENT_GRAPH    0x00040000U

struct hal_cons_state {
	enum hal_cons_mode mode;
	unsigned row;
	unsigned column;
	int cursor_visible;
};

void bsp_cons_init(void);
void cons_cls(void);
void cons_putc(int c);
void cons_puts(const char *utf8);
int cons_getc(void);
void cons_set_attr(int fg, int bg);

void hal_cons_reset(void);
void hal_cons_clear(void);
void hal_cons_set_mode(enum hal_cons_mode mode);
void hal_cons_putc(int character);
void hal_cons_write(const char *utf8);
void hal_cons_write_n(const char *utf8, unsigned length);
void hal_cons_write_at(unsigned row, unsigned column, const char *utf8);
void hal_cons_clear_row(unsigned row);
void hal_cons_clear_to_eol(void);
int hal_cons_write_at_attr(unsigned row, unsigned column, const char *utf8,
			   uint8_t attribute);
int hal_cons_write_n_at(unsigned row, unsigned column, const char *utf8,
			unsigned length, uint8_t attribute);
int hal_cons_clear_to_eol_at(unsigned row, unsigned column);
int hal_cons_set_cursor(unsigned row, unsigned column);
void hal_cons_show_cursor(int visible);
void hal_cons_save_state(struct hal_cons_state *state);
void hal_cons_restore_terminal(const struct hal_cons_state *state);
void hal_cons_update_cursor(void);
int hal_cons_read_event(void);
int hal_cons_poll_event(void);
int hal_cons_key_state(int key);
void hal_cons_drain_input(void);
unsigned hal_cons_modifiers(void);

#endif
