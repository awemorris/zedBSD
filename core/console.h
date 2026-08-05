/*
 * Boots
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef BOOTS_CONSOLE_H
#define BOOTS_CONSOLE_H

#include <stdint.h>

enum boots_console_mode {
	BOOTS_CONSOLE_FIXED_MENU,
	BOOTS_CONSOLE_TERMINAL,
};

#define BOOTS_CONSOLE_COLUMNS 80U
#define BOOTS_CONSOLE_ROWS 25U
#define BOOTS_CONSOLE_NORMAL_ATTRIBUTE 0xe1U

struct boots_console_state {
	enum boots_console_mode mode;
	unsigned row;
	unsigned column;
	int cursor_visible;
};

void boots_console_reset(void);
void boots_console_clear(void);
void boots_console_set_mode(enum boots_console_mode mode);
void boots_console_putc(uint8_t byte);
void boots_console_puts_sjis(const uint8_t *string);
void boots_console_write_at(unsigned row, unsigned column,
			     const uint8_t *string);
void boots_console_clear_row(unsigned row);
void boots_console_clear_to_eol(void);
int boots_console_put_sjis_at(unsigned row, unsigned column,
			       const uint8_t *string, uint8_t attribute);
int boots_console_put_utf8_at(unsigned row, unsigned column,
			       const char *string, unsigned length,
			       uint8_t attribute);
int boots_console_clear_to_eol_at(unsigned row, unsigned column);
int boots_console_set_cursor(unsigned row, unsigned column);
void boots_console_show_cursor(int visible);
void boots_console_save_state(struct boots_console_state *state);
void boots_console_restore_terminal(const struct boots_console_state *state);
void boots_console_update_cursor(void);

#endif
