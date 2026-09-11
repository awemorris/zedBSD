/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The kernel's character-output face of the display device.
 *
 * This holds one table and forwards to it. The table is published once
 * during the display driver's bring-up and read on every console write,
 * so it is stored and loaded atomically rather than under a lock: taking
 * a lock here would put one on the path of every character.
 */

#include <stddef.h>

#include "kern/text-display.h"

static const struct kern_text_ops *text_ops;

/*
 * Publishes one board's character output.
 */
void
kern_text_register(
	const struct kern_text_ops *ops)
{
	/* Releases the completed table to every reader. */
	__atomic_store_n(&text_ops, ops, __ATOMIC_RELEASE);
}

/*
 * Reports whether a board has published character output.
 */
int
kern_text_ready(
	void)
{
	/* Reports the published table's presence. */
	return __atomic_load_n(&text_ops, __ATOMIC_ACQUIRE) != NULL;
}

/* Reads the published table, or NULL before any board registers. */
static const struct kern_text_ops *
ops(void)
{
	/* Pairs with the release in the register path. */
	return __atomic_load_n(&text_ops, __ATOMIC_ACQUIRE);
}

/*
 * Reports the grid size in character cells.
 */
void
kern_text_get_size(
	unsigned *columns,
	unsigned *rows)
{
	const struct kern_text_ops *table;

	/* Reports an empty grid when no display is published. */
	table = ops();
	if (table == NULL) {
		if (columns != NULL)
			*columns = 0;
		if (rows != NULL)
			*rows = 0;
		return;
	}
	table->get_size(columns, rows);
}

/*
 * Writes one character at the cursor.
 */
void
kern_text_putc(
	int character)
{
	const struct kern_text_ops *table;

	/* Discards output until a display is published. */
	table = ops();
	if (table != NULL)
		table->putc(character);
}

/*
 * Writes a terminated string at one cell with one attribute.
 */
void
kern_text_write(
	unsigned row,
	unsigned column,
	uint8_t attribute,
	const char *utf8)
{
	const struct kern_text_ops *table;

	/* Discards output until a display is published. */
	table = ops();
	if (table != NULL)
		table->write(row, column, attribute, utf8);
}

/*
 * Blanks the grid and homes the cursor.
 */
void
kern_text_clear(
	void)
{
	const struct kern_text_ops *table;

	/* Does nothing without a display. */
	table = ops();
	if (table != NULL)
		table->clear();
}

/*
 * Moves the cursor.
 */
int
kern_text_set_cursor(
	unsigned row,
	unsigned column)
{
	const struct kern_text_ops *table;

	/* Reports failure without a display. */
	table = ops();
	if (table == NULL)
		return 0;
	return table->set_cursor(row, column);
}

/*
 * Reports the cursor position and whether it is shown.
 */
void
kern_text_get_cursor(
	unsigned *row,
	unsigned *column,
	int *visible)
{
	const struct kern_text_ops *table;

	/* Reports a hidden home cursor without a display. */
	table = ops();
	if (table == NULL) {
		if (row != NULL)
			*row = 0;
		if (column != NULL)
			*column = 0;
		if (visible != NULL)
			*visible = 0;
		return;
	}
	table->get_cursor(row, column, visible);
}

/*
 * Shows or hides the cursor.
 */
void
kern_text_show_cursor(
	int visible)
{
	const struct kern_text_ops *table;

	/* Does nothing without a display. */
	table = ops();
	if (table != NULL)
		table->show_cursor(visible);
}

/*
 * Repaints the cursor after a stream of writes.
 */
void
kern_text_update_cursor(
	void)
{
	const struct kern_text_ops *table;

	/* Does nothing without a display. */
	table = ops();
	if (table != NULL)
		table->update_cursor();
}

/*
 * Stops output while a graphics mode owns the screen.
 */
void
kern_text_suspend(
	void)
{
	const struct kern_text_ops *table;

	/* Does nothing without a display. */
	table = ops();
	if (table != NULL)
		table->suspend();
}

/*
 * Restarts output and repaints the retained screen.
 */
void
kern_text_resume(
	void)
{
	const struct kern_text_ops *table;

	/* Does nothing without a display. */
	table = ops();
	if (table != NULL)
		table->resume();
}
