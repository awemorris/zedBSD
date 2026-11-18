/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The MIPS boards' early text console, drawn by the board's display.
 */

#ifndef KERN_HAL_MIPS_CONS_H
#define KERN_HAL_MIPS_CONS_H

#include <hal/types.h>

/* The largest grid of text cells a board's display may hold. */
#define MIPS_CONS_COLUMNS_MAX	80U
#define MIPS_CONS_ROWS_MAX	30U

/*
 * A board's display as the console draws on it: its grid of cells, at
 * most MIPS_CONS_COLUMNS_MAX by MIPS_CONS_ROWS_MAX, a function that draws
 * one cell and one that draws the cursor over a cell.
 */
struct mips_cons_display {
	unsigned columns;
	unsigned rows;
	void (*cell)(unsigned row, unsigned column, int character, uint8_t attribute);
	void (*cursor)(unsigned row, unsigned column);
};

void
mips_cons_putc(
	int character);

void
mips_cons_attach(
	const struct mips_cons_display *display);

unsigned
mips_cons_columns(
	void);

void
mips_cons_print_words(
	const char *const pieces[],
	unsigned count);

#endif
