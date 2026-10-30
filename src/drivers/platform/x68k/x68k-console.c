/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The X68000 console: /dev/console over the platform's text screen.
 *
 * The console is a teletype: each character goes to
 * kern_console_early_putc(), which draws it on the text screen.  A
 * positioned write has no teletype meaning and is dropped; the grid only
 * answers the size and cursor queries that programs make.  The console has
 * no input yet.
 *
 * Once a GPU has flipped its first image the console leaves the text
 * screen for good: the kernel takes the character path over from the
 * hardware layer and drops every character, which then only the kernel's
 * log keeps, since the board has no other output.
 */

#include <stdint.h>

#include "drivers/platform/x68k/x68k-console.h"

#include <hal/hal.h>
#include <kern/platform.h>
#include <kern/text-display.h>
#include <uapi/errno.h>

/* The grid the console reports, the platform's text screen's. */
#define X68K_CONSOLE_COLUMNS	96U
#define X68K_CONSOLE_ROWS	32U

static void console_get_size(unsigned *columns, unsigned *rows);
static void console_putc(int character);
static void console_write(unsigned row, unsigned column, uint8_t attribute, const char *utf8);
static void console_clear(void);
static int console_set_cursor(unsigned row, unsigned column);
static void console_get_cursor(unsigned *row, unsigned *column, int *visible);
static void console_show_cursor(int visible);
static void console_nothing(void);
static int console_end(const struct kern_text_range *ranges, unsigned count);
static void console_discard(int character);

/*
 * The console's text operations, registered once at start-up and never
 * changed.  Everything but a character goes nowhere, because the console
 * is a teletype.
 */
static const struct kern_text_ops x68k_console_ops = {
	.get_size = console_get_size,
	.putc = console_putc,
	.write = console_write,
	.clear = console_clear,
	.set_cursor = console_set_cursor,
	.get_cursor = console_get_cursor,
	.show_cursor = console_show_cursor,
	.update_cursor = console_nothing,
	.suspend = console_nothing,
	.resume = console_nothing,
	.end = console_end
};

/*
 * Publishes the console output.
 */
void
drv_x68k_console_init(
	void)
{
	/* Makes the teletype the console's text output. */
	kern_text_register(&x68k_console_ops);
}

/* Reports the grid. */
static void
console_get_size(
	unsigned *columns,
	unsigned *rows)
{
	/* Reports the width when the caller asked for it. */
	if (columns != NULL)
		*columns = X68K_CONSOLE_COLUMNS;

	/* Reports the height when the caller asked for it. */
	if (rows != NULL)
		*rows = X68K_CONSOLE_ROWS;
}

/* Sends one character to the text screen. */
static void
console_putc(
	int character)
{
	/* Writes the character wherever the hardware layer shows the console. */
	kern_console_early_putc(character);
}

/* Drops a positioned write, which a teletype cannot place. */
static void
console_write(
	unsigned row,
	unsigned column,
	uint8_t attribute,
	const char *utf8)
{
	(void)row;
	(void)column;
	(void)attribute;
	(void)utf8;
}

/* Leaves the terminal as it is: the teletype has nothing to blank. */
static void
console_clear(
	void)
{
}

/* Accepts a position inside the grid without moving anything. */
static int
console_set_cursor(
	unsigned row,
	unsigned column)
{
	/* Refuses a row below the grid. */
	if (row >= X68K_CONSOLE_ROWS)
		return 0;

	/* Refuses a column right of the grid. */
	if (column >= X68K_CONSOLE_COLUMNS)
		return 0;

	/* Succeeded: the position is inside the grid. */
	return 1;
}

/* Reports the home position and a shown cursor. */
static void
console_get_cursor(
	unsigned *row,
	unsigned *column,
	int *visible)
{
	/* Reports the home row when the caller asked for it. */
	if (row != NULL)
		*row = 0;

	/* Reports the home column when the caller asked for it. */
	if (column != NULL)
		*column = 0;

	/* Reports a shown cursor when the caller asked for it. */
	if (visible != NULL)
		*visible = 1;
}

/* Ignores the request: the console draws its own cursor. */
static void
console_show_cursor(
	int visible)
{
	(void)visible;
}

/* Does nothing for an operation a teletype does not have. */
static void
console_nothing(
	void)
{
}

/*
 * Ends the console's use of the text screen for good: a character path
 * that drops every character is published as the kernel's, so the
 * hardware layer hands each character to it and never draws again.
 * Nothing is left behind (the hardware layer reaches the text screen by
 * its physical address), so this returns 0; EBUSY with ranges when
 * another character path was published already, since what it draws is
 * not known.
 */
static int
console_end(
	const struct kern_text_range *ranges,
	unsigned count)
{
	void (*published)(int);
	int foreign;

	(void)ranges;

	/* The hardware layer hands every later character to the dropping path. */
	foreign = 0;
	published = __atomic_load_n(&kernel_putc, __ATOMIC_ACQUIRE);
	if (published == NULL) {
		__atomic_store_n(&kernel_putc, console_discard, __ATOMIC_RELEASE);
	} else if (published != console_discard) {
		foreign = 1;
	}

	/* Another path than this one may still draw anywhere the caller gives. */
	if (foreign && count != 0U)
		return EBUSY;

	/* Succeeded: nothing of the console draws on the screen. */
	return 0;
}

/* Drops one character of the kernel's own path; the kernel's log keeps it. */
static void
console_discard(
	int character)
{
	(void)character;
}
