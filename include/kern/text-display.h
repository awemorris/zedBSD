/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The kernel's character-output face of the display device.
 *
 * /dev/graphics owns the display on every board, including the ability to
 * put characters on it. What that means in hardware differs: a linear
 * framebuffer draws glyphs as pixels, VGA text memory takes a cell word,
 * and the PC-98 keeps characters and attributes in two separate planes.
 * /dev/console needs none of that. It calls through this table, so it
 * stays one platform-independent multiplexer over the display and evdev.
 *
 * The board's display driver registers one table during its own bring-up.
 * Until it does, kern_text_ready() reports zero and every call here is a
 * no-op, which is the correct behaviour on a board with no display.
 */

#ifndef KERN_TEXT_DISPLAY_H
#define KERN_TEXT_DISPLAY_H

#include <stdint.h>

/* Light grey on black, the default text attribute on every board. */
#define KERN_TEXT_ATTRIB_NORMAL	0x07U

/*
 * One board's character output.
 *
 * Every entry is required. The display driver holds the cell state; this
 * table is only the way in.
 */
struct kern_text_ops {
	/* Reports the grid size in character cells. */
	void (*get_size)(unsigned *columns, unsigned *rows);

	/* Writes one character at the cursor, scrolling as needed. */
	void (*putc)(int character);

	/* Writes a terminated string at one cell with one attribute. */
	void (*write)(unsigned row, unsigned column, uint8_t attribute,
		      const char *utf8);

	/* Blanks the grid and homes the cursor. */
	void (*clear)(void);

	/* Moves the cursor; returns zero when the position is outside. */
	int (*set_cursor)(unsigned row, unsigned column);

	/* Reports the cursor position and whether it is shown. */
	void (*get_cursor)(unsigned *row, unsigned *column, int *visible);

	/* Shows or hides the cursor. */
	void (*show_cursor)(int visible);

	/* Repaints the cursor after a stream of writes. */
	void (*update_cursor)(void);

	/* Stops and restarts output while a graphics mode owns the screen. */
	void (*suspend)(void);
	void (*resume)(void);
};

/*
 * Publish one board's character output.
 *
 * Called once, from the display driver's bring-up. A second call replaces
 * the first, which is how a board that gains a better display later can
 * hand over.
 */
void kern_text_register(const struct kern_text_ops *ops);

/* Reports whether a board has published character output. */
int kern_text_ready(void);

/* The calls /dev/console makes. Each is a no-op with no display. */
void kern_text_get_size(unsigned *columns, unsigned *rows);
void kern_text_putc(int character);
void kern_text_write(unsigned row, unsigned column, uint8_t attribute,
		     const char *utf8);
void kern_text_clear(void);
int kern_text_set_cursor(unsigned row, unsigned column);
void kern_text_get_cursor(unsigned *row, unsigned *column, int *visible);
void kern_text_show_cursor(int visible);
void kern_text_update_cursor(void);
void kern_text_suspend(void);
void kern_text_resume(void);

#endif
