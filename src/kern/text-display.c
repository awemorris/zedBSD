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

#include <errno.h>
#include <stddef.h>

#include "kern/text-display.h"

/* The published backend is immutable and outlives every atomic reader. */
static const struct kern_text_ops *text_ops;

/*
 * This unsigned counter changes after completed text mutations and backend replacement.
 * Equality is a redraw hint between observations, not a globally unique lifetime token.
 */
static uint32_t text_generation;

static const struct kern_text_ops *ops(void);

/*
 * Publishes one board's character output.
 */
void
kern_text_register(
	const struct kern_text_ops *ops)
{
	/* Releases the completed table to every reader. */
	__atomic_store_n(&text_ops, ops, __ATOMIC_RELEASE);

	/* Snapshot consumers must repaint even when the replacement backend kept the same text. */
	__atomic_add_fetch(&text_generation, 1U, __ATOMIC_RELEASE);

	/* Succeeded: readers can observe the new immutable backend and its redraw generation. */
	return;
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
	if (table != NULL) {
		table->putc(character);

		/* Publishes the appended character and updated cursor to snapshot consumers. */
		__atomic_add_fetch(&text_generation, 1U, __ATOMIC_RELEASE);
	}

	/* Succeeded: the available backend completed its text operation. */
	return;
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
	if (table != NULL) {
		table->write(row, column, attribute, utf8);

		/* Publishes the replaced text span to snapshot consumers. */
		__atomic_add_fetch(&text_generation, 1U, __ATOMIC_RELEASE);
	}

	/* Succeeded: the available backend completed its text operation. */
	return;
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
	if (table != NULL) {
		table->clear();

		/* Publishes the cleared retained grid to snapshot consumers. */
		__atomic_add_fetch(&text_generation, 1U, __ATOMIC_RELEASE);
	}

	/* Succeeded: the available backend completed its text operation. */
	return;
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
	int placement;

	/* Reports failure without a display. */
	table = ops();
	if (table == NULL)
		return 0;

	/* Retains the backend's cursor convention while notifying snapshot consumers. */
	placement = table->set_cursor(row, column);
	__atomic_add_fetch(&text_generation, 1U, __ATOMIC_RELEASE);

	/* Succeeded: reports the backend's placement answer. */
	return placement;
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
	if (table != NULL) {
		table->show_cursor(visible);

		/* Publishes the changed cursor visibility to snapshot consumers. */
		__atomic_add_fetch(&text_generation, 1U, __ATOMIC_RELEASE);
	}

	/* Succeeded: the available backend completed its text operation. */
	return;
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
	if (table != NULL) {
		table->update_cursor();

		/* Publishes the completed cursor repaint to snapshot consumers. */
		__atomic_add_fetch(&text_generation, 1U, __ATOMIC_RELEASE);
	}

	/* Succeeded: the available backend completed its text operation. */
	return;
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
	if (table != NULL) {
		table->suspend();

		/* Publishes the backend's suspended rendering state to snapshot consumers. */
		__atomic_add_fetch(&text_generation, 1U, __ATOMIC_RELEASE);
	}

	/* Succeeded: the available backend completed its text operation. */
	return;
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
	if (table != NULL) {
		table->resume();

		/* Publishes the resumed retained text to snapshot consumers. */
		__atomic_add_fetch(&text_generation, 1U, __ATOMIC_RELEASE);
	}

	/* Succeeded: the available backend completed its text operation. */
	return;
}

/*
 * Renders the published backend's retained cells into caller-owned ordinary RAM.
 */
int
kern_text_snapshot(
	struct kern_text_snapshot *snapshot)
{
	const struct kern_text_ops *table;
	int error;

	/* An absent request cannot receive geometry or pixels. */
	if (snapshot == NULL)
		return EINVAL;

	/* Backends without retained-cell rendering keep their existing text behavior. */
	table = ops();
	if (table == NULL)
		return ENOTSUP;

	/* The optional tail callback allows legacy platform tables to remain unchanged. */
	if (table->snapshot == NULL)
		return ENOTSUP;

	/* Only the renderer reads its retained character state and owns its text lock. */
	error = table->snapshot(snapshot);
	if (error != 0)
		return error;

	/* Succeeded: the caller owns either exact geometry or a complete independent image. */
	return 0;
}

/*
 * Observes completed text mutations without entering any display or console lock.
 */
uint32_t
kern_text_generation(
	void)
{
	uint32_t generation;

	/* Acquire ordering observes retained text writes which preceded this redraw hint. */
	generation = __atomic_load_n(&text_generation, __ATOMIC_ACQUIRE);

	/* Succeeded: the caller may compare this generation with its last displayed snapshot. */
	return generation;
}

/* Reads the published table, or NULL before any board registers. */
static const struct kern_text_ops *
ops(void)
{
	/* Pairs with the release in the register path. */
	return __atomic_load_n(&text_ops, __ATOMIC_ACQUIRE);
}
