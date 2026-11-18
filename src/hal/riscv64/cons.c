/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The riscv64 early text console.
 *
 * The HAL and the early kernel print through hal_putc(), which writes each
 * character to the firmware's console and here.  The console keeps an
 * 80 by 25 grid of cells from the first character on, so that the lines
 * printed before a board's screen is attached appear on it once it is;
 * from then on each change is drawn as it is made and written through to
 * the display.  Once the kernel publishes its own console, hal_putc() no
 * longer comes here.
 */

#include <hal/hal.h>
#include "spin.h"
#include "cons.h"
#include "fbcons.h"

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
 * The grid as it should look.  Kept under cons_lock; a zero character is a
 * cell never written, drawn as a blank.
 */
static struct cons_cell cons_grid[RISCV64_CONSOLE_ROWS][RISCV64_CONSOLE_COLUMNS];

/* The cursor's row in cons_grid, kept under cons_lock. */
static unsigned cons_row;

/* The cursor's column in cons_grid, kept under cons_lock. */
static unsigned cons_column;

/* Guards the grid, the cursor and the drawing; zero when free. */
static volatile unsigned cons_lock;

static void paint_cell_locked(unsigned row, unsigned column);
static void draw_cell_locked(unsigned row, unsigned column);
static void draw_all_locked(void);
static void newline_locked(void);
static void scroll_locked(void);
static void putc_locked(int character);

/*
 * Writes one character to the console.
 */
void
riscv64_cons_putc(
	int character)
{
	bool enabled;

	/* Changes the grid and draws it as one step against other harts and handlers. */
	enabled = riscv64_spin_lock(&cons_lock);

	putc_locked(character);

	riscv64_spin_unlock(&cons_lock, enabled);
}

/*
 * Draws the whole grid once a screen is attached, so the lines printed
 * before it appear.
 */
void
riscv64_cons_redraw(
	void)
{
	bool enabled;

	/* Repaints every cell and the cursor. */
	enabled = riscv64_spin_lock(&cons_lock);

	draw_all_locked();

	riscv64_spin_unlock(&cons_lock, enabled);
}

/*
 * Gives the screen up to the kernel's console, which took the early
 * console's window of it over and removes it.  The kernel prints through
 * its own console by then, so nothing here draws again; the screen goes
 * under the console's lock all the same, so that a character that began
 * before the hand-over finishes before the window goes.
 */
void
riscv64_cons_detach_screen(
	void)
{
	bool enabled;

	/* Waits for a character being drawn, then keeps the next ones off the screen. */
	enabled = riscv64_spin_lock(&cons_lock);

	riscv64_fbcons_detach();

	riscv64_spin_unlock(&cons_lock, enabled);
}

/* Paints one cell of the grid, a blank for one never written, without writing it through. */
static void
paint_cell_locked(
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
	riscv64_fbcons_cell(row, column, character, attribute);
	if (row == cons_row && column == cons_column)
		riscv64_fbcons_cursor(row, column);
}

/* Draws one cell of the grid and writes it through to the display. */
static void
draw_cell_locked(
	unsigned row,
	unsigned column)
{
	/* Paints the cell, then writes its pixel rows through. */
	paint_cell_locked(row, column);
	riscv64_fbcons_flush_cell(row, column);
}

/* Draws every cell of the grid and writes the grid through once. */
static void
draw_all_locked(
	void)
{
	unsigned row;
	unsigned column;

	/* Paints each row, left to right. */
	for (row = 0; row < RISCV64_CONSOLE_ROWS; row++) {
		for (column = 0; column < RISCV64_CONSOLE_COLUMNS; column++)
			paint_cell_locked(row, column);
	}

	/* Writes every pixel row of the grid through, each once. */
	riscv64_fbcons_flush_grid();
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
	if (old_column < RISCV64_CONSOLE_COLUMNS)
		draw_cell_locked(old_row, old_column);

	/* Scrolls instead of leaving the last row. */
	if (cons_row >= RISCV64_CONSOLE_ROWS) {
		cons_row = RISCV64_CONSOLE_ROWS - 1U;
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
	for (row = 1; row < RISCV64_CONSOLE_ROWS; row++) {
		for (column = 0; column < RISCV64_CONSOLE_COLUMNS; column++)
			cons_grid[row - 1U][column] = cons_grid[row][column];
	}

	/* Blanks the row exposed at the bottom. */
	for (column = 0; column < RISCV64_CONSOLE_COLUMNS; column++) {
		cons_grid[RISCV64_CONSOLE_ROWS - 1U][column].character = 0;
		cons_grid[RISCV64_CONSOLE_ROWS - 1U][column].attribute = 0;
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
		if (column < RISCV64_CONSOLE_COLUMNS)
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
		if (column < RISCV64_CONSOLE_COLUMNS)
			draw_cell_locked(cons_row, column);
		draw_cell_locked(cons_row, cons_column);
		return;
	}

	/* A tab moves to the next eight-column stop, wrapping past the row's end. */
	if (character == '\t') {
		/* Takes the cursor off its cell and moves it to the stop. */
		column = cons_column;
		cons_column = (cons_column + 8U) & ~7U;
		if (column < RISCV64_CONSOLE_COLUMNS)
			draw_cell_locked(cons_row, column);

		/* A stop past the row's end starts the next line. */
		if (cons_column >= RISCV64_CONSOLE_COLUMNS) {
			newline_locked();
			return;
		}

		/* Shows the cursor at the stop. */
		draw_cell_locked(cons_row, cons_column);
		return;
	}

	/* Wraps before writing past the end of the row. */
	if (cons_column >= RISCV64_CONSOLE_COLUMNS)
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
	if (cons_column < RISCV64_CONSOLE_COLUMNS)
		draw_cell_locked(cons_row, cons_column);
}
