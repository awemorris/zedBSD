/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The sun4u board's text console.
 *
 * The HAL and the kernel print through hal_putc(), which writes each
 * character to the serial port and here.  The console keeps an 80 by 25
 * grid of cells from the first character on, so that the lines printed
 * before the framebuffer is shown appear on it once it is; from then on
 * each change is drawn as it is made.  Once the kernel publishes its own
 * console, hal_putc() goes there instead.
 */

#include <hal/hal.h>
#include "console.h"
#include "framebuffer.h"
#include "uart.h"

/* Light grey on black, the default text attribute. */
#define CONS_ATTRIBUTE	0x07U

/*
 * One text cell as the console keeps it.
 */
struct cons_cell {
	uint8_t character;
	uint8_t attribute;
};

/*
 * The grid as it should look, and the cursor.  Changed with interrupts
 * off, which is enough on the port's one processor; a zero character is
 * a cell never written, drawn as a blank.
 */
static struct cons_cell cons_grid[SUN4U_CONSOLE_ROWS][SUN4U_CONSOLE_COLUMNS];
static unsigned cons_row;
static unsigned cons_column;

static void draw_cell_locked(unsigned row, unsigned column);
static void draw_all_locked(void);
static void newline_locked(void);
static void scroll_locked(void);
static void putc_locked(int character);

/*
 * Writes one character to the console, or to the kernel's console once the
 * kernel has published one.
 */
void
hal_putc(
	int c)
{
	void (*kernel_output)(int character);
	bool enabled;

	/* Delegates to the kernel console after the handover. */
	kernel_output = __atomic_load_n(&kernel_putc, __ATOMIC_ACQUIRE);
	if (kernel_output != NULL) {
		kernel_output(c);
		return;
	}

	/* Writes the serial port, which ends its lines itself. */
	sun4u_uart_putc(c);

	/* Changes the grid and draws it as one step against interrupt handlers. */
	enabled = hal_irq_disable();

	putc_locked(c);

	if (enabled)
		hal_irq_enable();
}

/*
 * Draws the whole grid once the framebuffer is shown, so the lines printed
 * before it appear.
 */
void
sun4u_console_attach(
	void)
{
	bool enabled;

	/* Repaints every cell and the cursor. */
	enabled = hal_irq_disable();

	draw_all_locked();

	if (enabled)
		hal_irq_enable();
}

/* Draws one cell of the grid, a blank for one never written. */
static void
draw_cell_locked(
	unsigned row,
	unsigned column)
{
	const struct cons_cell *cell;
	int character;
	uint8_t attribute;

	/* A cell never written is a blank in the default colours. */
	cell = &cons_grid[row][column];
	character = cell->character;
	attribute = cell->attribute;
	if (character == 0) {
		character = ' ';
		attribute = CONS_ATTRIBUTE;
	}

	/* Draws the cell, with the cursor on it when it is the cursor's. */
	sun4u_framebuffer_cell(row, column, character, attribute);
	if (row == cons_row && column == cons_column)
		sun4u_framebuffer_cursor(row, column);
}

/* Draws every cell of the grid. */
static void
draw_all_locked(
	void)
{
	unsigned row;
	unsigned column;

	/* Each row, left to right. */
	for (row = 0; row < SUN4U_CONSOLE_ROWS; row++) {
		for (column = 0; column < SUN4U_CONSOLE_COLUMNS; column++)
			draw_cell_locked(row, column);
	}
}

/* Moves the cursor to the start of the next line, scrolling at the bottom. */
static void
newline_locked(
	void)
{
	unsigned old_row;
	unsigned old_column;

	/* The cell the cursor leaves loses its cursor. */
	old_row = cons_row;
	old_column = cons_column;
	cons_column = 0;
	cons_row++;
	if (old_column < SUN4U_CONSOLE_COLUMNS)
		draw_cell_locked(old_row, old_column);

	/* Scrolls instead of leaving the last row. */
	if (cons_row >= SUN4U_CONSOLE_ROWS) {
		cons_row = SUN4U_CONSOLE_ROWS - 1U;
		scroll_locked();
		return;
	}

	/* Shows the cursor at its new place. */
	draw_cell_locked(cons_row, cons_column);
}

/* Moves every row up by one and blanks the bottom row. */
static void
scroll_locked(
	void)
{
	unsigned row;
	unsigned column;

	/* Shifts the rows up. */
	for (row = 1; row < SUN4U_CONSOLE_ROWS; row++) {
		for (column = 0; column < SUN4U_CONSOLE_COLUMNS; column++)
			cons_grid[row - 1U][column] = cons_grid[row][column];
	}

	/* Blanks the row exposed at the bottom. */
	for (column = 0; column < SUN4U_CONSOLE_COLUMNS; column++) {
		cons_grid[SUN4U_CONSOLE_ROWS - 1U][column].character = 0;
		cons_grid[SUN4U_CONSOLE_ROWS - 1U][column].attribute = 0;
	}

	/* Repaints the moved screen. */
	draw_all_locked();
}

/* Puts one character at the cursor. */
static void
putc_locked(
	int character)
{
	unsigned column;

	/* A newline moves to the next line. */
	if (character == '\n') {
		newline_locked();
		return;
	}

	/* A carriage return moves to the line's start. */
	if (character == '\r') {
		column = cons_column;
		cons_column = 0;
		if (column < SUN4U_CONSOLE_COLUMNS)
			draw_cell_locked(cons_row, column);
		draw_cell_locked(cons_row, cons_column);
		return;
	}

	/* A backspace steps back and erases within the line. */
	if (character == '\b') {
		column = cons_column;
		if (cons_column > 0)
			cons_column--;
		cons_grid[cons_row][cons_column].character = ' ';
		cons_grid[cons_row][cons_column].attribute = CONS_ATTRIBUTE;
		if (column < SUN4U_CONSOLE_COLUMNS)
			draw_cell_locked(cons_row, column);
		draw_cell_locked(cons_row, cons_column);
		return;
	}

	/* A tab moves to the next eight-column stop, wrapping past the row's end. */
	if (character == '\t') {
		column = cons_column;
		cons_column = (cons_column + 8U) & ~7U;
		if (column < SUN4U_CONSOLE_COLUMNS)
			draw_cell_locked(cons_row, column);
		if (cons_column >= SUN4U_CONSOLE_COLUMNS) {
			newline_locked();
			return;
		}
		draw_cell_locked(cons_row, cons_column);
		return;
	}

	/* Wraps before writing past the end of the row. */
	if (cons_column >= SUN4U_CONSOLE_COLUMNS)
		newline_locked();

	/* Stores a printable character, or a question mark for anything else. */
	cons_grid[cons_row][cons_column].character = '?';
	if (character >= 0x20 && character < 0x7f)
		cons_grid[cons_row][cons_column].character = (uint8_t)character;
	cons_grid[cons_row][cons_column].attribute = CONS_ATTRIBUTE;

	/* Moves the cursor on and draws the written cell and the cursor's. */
	column = cons_column;
	cons_column++;
	draw_cell_locked(cons_row, column);
	if (cons_column < SUN4U_CONSOLE_COLUMNS)
		draw_cell_locked(cons_row, cons_column);
}
