/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A teletype console for a board whose console is a serial line.
 *
 * Each character the console writes goes to kern_console_early_putc(),
 * which the platform sends to the firmware's console (the same serial line)
 * and, on a board that has one, to a framebuffer it draws text on.  A
 * positioned write has no teletype meaning and is dropped; the grid only
 * answers the size and cursor queries programs make.
 *
 * Input arrives from a UART driver, one byte at a time in interrupt
 * context, and is typed into the console's line discipline.
 */

#include <stddef.h>
#include <stdint.h>

#include <drivers/generic/serial-console.h>

#include <kern/platform.h>
#include <kern/text-display.h>
#include <kern/tty.h>

/* The grid a serial terminal is assumed to have. */
#define SERIAL_CONSOLE_COLUMNS	80U
#define SERIAL_CONSOLE_ROWS	24U

/* What a terminal sends for Enter and for backspace. */
#define SERIAL_CONSOLE_CARRIAGE_RETURN	0x0dU
#define SERIAL_CONSOLE_DELETE		0x7fU

/* What the line discipline reads as the end of a line and as an erase. */
#define SERIAL_CONSOLE_NEWLINE		0x0aU
#define SERIAL_CONSOLE_BACKSPACE	0x08U

/* Marks a parameter the text operations' signatures carry but a teletype does not need. */
#define UNUSED_PARAMETER(parameter)	((void)(parameter))

static void console_get_size(unsigned *columns, unsigned *rows);
static void console_putc(int character);
static void console_write(unsigned row, unsigned column, uint8_t attribute, const char *utf8);
static void console_clear(void);
static int console_set_cursor(unsigned row, unsigned column);
static void console_get_cursor(unsigned *row, unsigned *column, int *visible);
static void console_show_cursor(int visible);
static void console_nothing(void);

/*
 * The console's text operations, registered once at start-up and never
 * changed.  Everything but a character goes nowhere, because the console is
 * a teletype; suspending and resuming have nothing to stop.
 */
static const struct kern_text_ops serial_console_ops = {
	.get_size = console_get_size,
	.putc = console_putc,
	.write = console_write,
	.clear = console_clear,
	.set_cursor = console_set_cursor,
	.get_cursor = console_get_cursor,
	.show_cursor = console_show_cursor,
	.update_cursor = console_nothing,
	.suspend = console_nothing,
	.resume = console_nothing
};

/*
 * Publishes the console output.
 */
void
drv_serial_console_init(
	void)
{
	/* Makes the teletype the console's text output. */
	kern_text_register(&serial_console_ops);
}

/*
 * Reports the console's text operations without publishing them.
 */
const struct kern_text_ops *
drv_serial_console_ops(
	void)
{
	/* The same table drv_serial_console_init() publishes. */
	return &serial_console_ops;
}

/*
 * Types one received byte into the console.
 *
 * A terminal sends a carriage return for Enter and a delete for backspace;
 * the line discipline expects a newline and a backspace.
 */
void
drv_serial_console_receive(
	uint8_t byte)
{
	/* Translates Enter and backspace to what the discipline reads. */
	if (byte == SERIAL_CONSOLE_CARRIAGE_RETURN) {
		byte = SERIAL_CONSOLE_NEWLINE;
	} else if (byte == SERIAL_CONSOLE_DELETE) {
		byte = SERIAL_CONSOLE_BACKSPACE;
	}

	/* Hands the byte to the console's line discipline. */
	tty_console_input_byte(byte);
}

/* Reports the assumed terminal grid. */
static void
console_get_size(
	unsigned *columns,
	unsigned *rows)
{
	/* Reports the width when the caller asked for it. */
	if (columns != NULL)
		*columns = SERIAL_CONSOLE_COLUMNS;

	/* Reports the height when the caller asked for it. */
	if (rows != NULL)
		*rows = SERIAL_CONSOLE_ROWS;
}

/* Sends one character wherever the hardware layer shows the console. */
static void
console_putc(
	int character)
{
	/* Writes the character through the hardware layer's console. */
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
	UNUSED_PARAMETER(row);
	UNUSED_PARAMETER(column);
	UNUSED_PARAMETER(attribute);
	UNUSED_PARAMETER(utf8);
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
	if (row >= SERIAL_CONSOLE_ROWS)
		return 0;

	/* Refuses a column right of the grid. */
	if (column >= SERIAL_CONSOLE_COLUMNS)
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

/* Ignores the request: the terminal draws its own cursor. */
static void
console_show_cursor(
	int visible)
{
	UNUSED_PARAMETER(visible);
}

/* Does nothing for an operation a teletype does not have. */
static void
console_nothing(
	void)
{
}
