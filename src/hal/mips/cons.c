/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The MIPS boards' early text console.
 *
 * The HAL and the early kernel print through hal_putc(), which the board
 * writes here.  The console keeps a grid of cells from the first
 * character on, so that the lines printed before the board's display is
 * shown appear on it once it is attached; from then on each change is
 * drawn as it is made, through the functions the board's display gives.
 * The grid has the largest size until a display is attached and that
 * display's size after.  Once the kernel publishes its own console,
 * hal_putc() no longer comes here.
 */

#include <hal/hal.h>
#include "cons.h"
#include "spin.h"

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
 * The grid as it should look, its size, the cursor and the display it
 * is drawn on (none before mips_cons_attach()).  Kept under cons_lock; a
 * zero character is a cell never written, drawn as a blank.
 */
static struct cons_cell cons_grid[MIPS_CONS_ROWS_MAX][MIPS_CONS_COLUMNS_MAX];
static unsigned cons_columns = MIPS_CONS_COLUMNS_MAX;
static unsigned cons_rows = MIPS_CONS_ROWS_MAX;
static unsigned cons_row;
static unsigned cons_column;
static const struct mips_cons_display *cons_display;

/* Guards the grid, the cursor and the drawing; zero when free. */
static volatile unsigned cons_lock;

static void draw_cell_locked(unsigned row, unsigned column);
static void draw_all_locked(void);
static void newline_locked(void);
static void scroll_locked(void);
static void putc_locked(int character);
static void blank_locked(void);
static unsigned word_length(const char *word);

/*
 * Writes one character to the console.
 */
void
mips_cons_putc(
	int character)
{
	bool enabled;

	/* Changes the grid and draws it as one step against interrupt handlers. */
	enabled = mips_spin_lock(&cons_lock);

	putc_locked(character);

	mips_spin_unlock(&cons_lock, enabled);
}

/*
 * Draws the grid on the board's display once it is shown, so that the
 * lines printed before it appear.
 *
 * A display of another size than the grid's starts from a blank grid:
 * a board whose display is smaller than the largest grid attaches it
 * before it prints.
 */
void
mips_cons_attach(
	const struct mips_cons_display *display)
{
	bool enabled;
	unsigned columns;
	unsigned rows;

	/* Keeps the grid within its storage. */
	columns = display->columns;
	if (columns > MIPS_CONS_COLUMNS_MAX)
		columns = MIPS_CONS_COLUMNS_MAX;
	rows = display->rows;
	if (rows > MIPS_CONS_ROWS_MAX)
		rows = MIPS_CONS_ROWS_MAX;

	/* Takes the display's size and repaints every cell and the cursor. */
	enabled = mips_spin_lock(&cons_lock);

	if (columns != cons_columns || rows != cons_rows) {
		cons_columns = columns;
		cons_rows = rows;
		blank_locked();
	}
	cons_display = display;
	draw_all_locked();

	mips_spin_unlock(&cons_lock, enabled);
}

/*
 * Reports the columns of the grid: the display's once one is attached.
 */
unsigned
mips_cons_columns(
	void)
{
	/* Succeeded: the grid's width. */
	return cons_columns;
}

/*
 * Prints a line of words, starting a new line before a word that would
 * not fit in the grid's width, so that a long line breaks between words
 * on a narrow display.  Each piece holds whole words separated by
 * single spaces; the pieces follow one another with a space.
 */
void
mips_cons_print_words(
	const char *const pieces[],
	unsigned count)
{
	const char *word;
	unsigned columns;
	unsigned column;
	unsigned length;
	unsigned index;
	unsigned offset;
	unsigned letter;

	/* Each word of each piece, after a space or on a new line. */
	columns = mips_cons_columns();
	column = 0;
	for (index = 0; index < count; index++) {
		offset = 0;
		while (pieces[index][offset] != '\0') {
			/* Skips the spaces between words. */
			if (pieces[index][offset] == ' ') {
				offset++;
				continue;
			}

			/* Breaks the line before a word that does not fit after the last. */
			word = &pieces[index][offset];
			length = word_length(word);
			if (column != 0 && column + 1U + length > columns) {
				hal_putc('\n');
				column = 0;
			}
			if (column != 0) {
				hal_putc(' ');
				column++;
			}

			/* Prints the word and moves past it. */
			for (letter = 0; letter < length; letter++)
				hal_putc(word[letter]);
			column += length;
			offset += length;
		}
	}

	/* Ends the line. */
	hal_putc('\n');
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

	/* Nothing is drawn before a display is attached. */
	if (cons_display == NULL)
		return;

	/* A cell never written is a blank in the default colours. */
	cell = &cons_grid[row][column];
	character = cell->character;
	attribute = cell->attribute;
	if (character == 0) {
		character = ' ';
		attribute = CONS_ATTRIBUTE;
	}

	/* Draws the cell, with the cursor on it when it is the cursor's. */
	cons_display->cell(row, column, character, attribute);
	if (row == cons_row && column == cons_column)
		cons_display->cursor(row, column);
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
	if (old_column < cons_columns)
		draw_cell_locked(old_row, old_column);

	/* Scrolls instead of leaving the last row. */
	if (cons_row >= cons_rows) {
		cons_row = cons_rows - 1U;
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
	for (row = 1; row < cons_rows; row++) {
		for (column = 0; column < cons_columns; column++)
			cons_grid[row - 1U][column] = cons_grid[row][column];
	}

	/* Blanks the row exposed at the bottom. */
	for (column = 0; column < cons_columns; column++) {
		cons_grid[cons_rows - 1U][column].character = 0;
		cons_grid[cons_rows - 1U][column].attribute = 0;
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
		if (column < cons_columns)
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
		if (column < cons_columns)
			draw_cell_locked(cons_row, column);
		draw_cell_locked(cons_row, cons_column);
		return;
	}

	/* A tab moves to the next eight-column stop, wrapping past the row's end. */
	if (character == '\t') {
		column = cons_column;
		cons_column = (cons_column + 8U) & ~7U;
		if (column < cons_columns)
			draw_cell_locked(cons_row, column);
		if (cons_column >= cons_columns) {
			newline_locked();
			return;
		}

		/* Draws the cell the character went into. */
		draw_cell_locked(cons_row, cons_column);
		return;
	}

	/* Wraps before writing past the end of the row. */
	if (cons_column >= cons_columns)
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
	if (cons_column < cons_columns)
		draw_cell_locked(cons_row, cons_column);
}

/* Blanks every cell and moves the cursor home. */
static void
blank_locked(
	void)
{
	unsigned row;
	unsigned column;

	/* Each cell becomes one never written. */
	for (row = 0; row < MIPS_CONS_ROWS_MAX; row++) {
		for (column = 0; column < MIPS_CONS_COLUMNS_MAX; column++) {
			cons_grid[row][column].character = 0;
			cons_grid[row][column].attribute = 0;
		}
	}

	/* The cursor starts at the top left. */
	cons_row = 0;
	cons_column = 0;
}

/* Reports the characters of a word, up to the space or the end that follows it. */
static unsigned
word_length(
	const char *word)
{
	unsigned length;

	/* Counts up to the word's end. */
	length = 0;
	while (word[length] != '\0' && word[length] != ' ')
		length++;

	/* Succeeded: the word's length. */
	return length;
}
