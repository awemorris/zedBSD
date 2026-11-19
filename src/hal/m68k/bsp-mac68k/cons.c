/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Macintosh's early text console.
 *
 * The HAL and the early kernel print through hal_putc(), which comes here
 * until the kernel publishes its own console.  The console keeps a grid of
 * cells from the first character on, 80 by 25 until the screen is taken
 * and then as many as the screen holds, so that the lines printed before
 * the screen is shown appear on it once it is; from then on each change
 * is drawn as it is made.
 */

#include <hal/hal.h>
#include "../m68040/spin.h"
#include "bsp.h"

/* The grid before the screen is taken, and the most cells it ever has. */
#define CONS_DEFAULT_COLUMNS	80U
#define CONS_DEFAULT_ROWS	25U
#define CONS_MAX_COLUMNS	128U
#define CONS_MAX_ROWS		64U

/*
 * The grid as it should look: the character of each cell, zero for a cell
 * never written, which is drawn as a blank.  Kept under cons_lock.
 */
static uint8_t cons_grid[CONS_MAX_ROWS][CONS_MAX_COLUMNS];

/* The grid's size in use and the cursor.  Kept under cons_lock. */
static unsigned cons_columns = CONS_DEFAULT_COLUMNS;
static unsigned cons_rows = CONS_DEFAULT_ROWS;
static unsigned cons_row;
static unsigned cons_column;

/* Nonzero once the screen is taken and the grid drawn on it.  Kept under cons_lock. */
static int cons_attached;

/* Guards the grid, the cursor and the drawing; zero when free. */
static volatile unsigned cons_lock;

static void draw_cell_locked(unsigned row, unsigned column);
static void draw_all_locked(void);
static void newline_locked(void);
static void scroll_locked(void);
static void putc_locked(int character);

/*
 * Writes one character to the console.
 */
void
mac68k_cons_putc(
	int character)
{
	bool enabled;

	/* Changes the grid and draws it as one step against handlers. */
	enabled = m68040_spin_lock(&cons_lock);

	putc_locked(character);

	m68040_spin_unlock(&cons_lock, enabled);
}

/*
 * Takes the screen's size and draws the whole grid on it, so that the
 * lines printed before it appear.
 */
void
mac68k_cons_attach(
	void)
{
	unsigned columns;
	unsigned rows;
	bool enabled;

	/* The screen's cells, within the grid's room; a screen too small is not used. */
	columns = mac68k_framebuffer_columns();
	rows = mac68k_framebuffer_rows();
	if (columns == 0 || rows == 0)
		return;
	if (columns > CONS_MAX_COLUMNS)
		columns = CONS_MAX_COLUMNS;
	if (rows > CONS_MAX_ROWS)
		rows = CONS_MAX_ROWS;

	/* Resizes the grid, keeping the cursor on it, and repaints every cell. */
	enabled = m68040_spin_lock(&cons_lock);

	cons_columns = columns;
	cons_rows = rows;
	if (cons_column > cons_columns)
		cons_column = cons_columns;
	while (cons_row >= cons_rows) {
		cons_row--;
		scroll_locked();
	}
	cons_attached = 1;
	draw_all_locked();

	m68040_spin_unlock(&cons_lock, enabled);
}

/*
 * Writes one character to the early console: the screen.
 */
void
m68040_board_putc(
	int character)
{
	/* The screen is the only early output. */
	mac68k_cons_putc(character);
}

/* Draws one cell of the grid, a blank for one never written, with the cursor when it is there. */
static void
draw_cell_locked(
	unsigned row,
	unsigned column)
{
	int character;
	bool cursor;

	/* Nothing is drawn before the screen is taken, or outside the grid. */
	if (!cons_attached)
		return;
	if (row >= cons_rows || column >= cons_columns)
		return;

	/* A cell never written is a blank. */
	character = cons_grid[row][column];
	if (character == 0)
		character = ' ';

	/* Draws the cell, with the cursor when it is the cursor's. */
	cursor = false;
	if (row == cons_row && column == cons_column)
		cursor = true;
	mac68k_framebuffer_cell(row, column, character, cursor);
}

/* Draws every cell of the grid. */
static void
draw_all_locked(
	void)
{
	unsigned row;
	unsigned column;

	/* Each row, left to right. */
	for (row = 0; row < cons_rows; row++) {
		for (column = 0; column < cons_columns; column++)
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
	draw_cell_locked(old_row, old_column);

	/* Scrolls instead of leaving the last row. */
	if (cons_row >= cons_rows) {
		cons_row = cons_rows - 1U;
		scroll_locked();
	}

	/* Shows the cursor at its new place. */
	draw_cell_locked(cons_row, cons_column);
}

/*
 * Moves every row of the grid up by one and blanks the bottom row; the
 * screen moves its pixels the same way.
 */
static void
scroll_locked(
	void)
{
	unsigned row;
	unsigned column;

	/* Shifts the rows up. */
	for (row = 1; row < cons_rows; row++) {
		for (column = 0; column < cons_columns; column++)
			cons_grid[row - 1U][column] = cons_grid[row][column];
	}

	/* Blanks the row exposed at the bottom. */
	for (column = 0; column < cons_columns; column++)
		cons_grid[cons_rows - 1U][column] = 0;

	/* The screen's pixels move with the grid. */
	if (cons_attached)
		mac68k_framebuffer_scroll();
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
		draw_cell_locked(cons_row, column);
		draw_cell_locked(cons_row, cons_column);
		return;
	}

	/* A backspace steps back and erases within the line. */
	if (character == '\b') {
		column = cons_column;
		if (cons_column > 0)
			cons_column--;
		cons_grid[cons_row][cons_column] = ' ';
		draw_cell_locked(cons_row, column);
		draw_cell_locked(cons_row, cons_column);
		return;
	}

	/* A tab moves to the next eight-column stop, wrapping past the row's end. */
	if (character == '\t') {
		column = cons_column;
		cons_column = (cons_column + 8U) & ~7U;
		draw_cell_locked(cons_row, column);
		if (cons_column >= cons_columns) {
			newline_locked();
			return;
		}

		/* Draws the cursor at the stop. */
		draw_cell_locked(cons_row, cons_column);
		return;
	}

	/* Wraps before writing past the end of the row. */
	if (cons_column >= cons_columns)
		newline_locked();

	/* Stores a printable character, or a question mark for anything else. */
	cons_grid[cons_row][cons_column] = '?';
	if (character >= 0x20 && character < 0x7f)
		cons_grid[cons_row][cons_column] = (uint8_t)character;

	/* Moves the cursor on and draws the written cell and the cursor's. */
	column = cons_column;
	cons_column++;
	draw_cell_locked(cons_row, column);
	draw_cell_locked(cons_row, cons_column);
}
