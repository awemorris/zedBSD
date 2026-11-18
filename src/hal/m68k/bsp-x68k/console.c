/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The X68000's text console, drawn on the text screen with the machine's
 * own 8x16 character ROM.
 *
 * The HAL and the kernel print through hal_putc(), which draws each
 * character here; once the kernel publishes its own console, hal_putc()
 * goes there instead.  The text screen is four bit planes of 1024 by 1024
 * pixels, of which 768 by 512 show; the first three planes carry the
 * glyph, so it shows in palette colour 7, and the fourth stays clear.
 */

#include <hal/hal.h>
#include "bsp.h"
#include "mmio.h"

/* The grid the visible text screen holds: 768 by 512 pixels in 8x16 cells. */
#define CONSOLE_COLUMNS		96U
#define CONSOLE_ROWS		32U

/* A glyph's rows, and the bytes of one row of a text plane. */
#define CONSOLE_FONT_HEIGHT	16U
#define CONSOLE_ROW_BYTES	128U

/* The distance from one text plane to the next, and the planes the glyph is drawn in. */
#define CONSOLE_PLANE_BYTES	0x00020000U
#define CONSOLE_PLANES		4U
#define CONSOLE_GLYPH_PLANES	3U

/*
 * The character ROM's 8x16 font and the text palette, in the physical
 * space.
 *
 * Values referenced: the X68000's memory map (the character generator
 * ROM's 8x16 font and the text palette registers).
 */
#define CONSOLE_CGROM_8X16	0x00f3a800U
#define CONSOLE_TEXT_PALETTE	0x00e82200U

/* The palette entries the console sets: 0 black, 7 the three-plane white. */
#define CONSOLE_PALETTE_BLACK	0U
#define CONSOLE_PALETTE_WHITE	14U

/*
 * The characters on the screen and the cursor.  Changed with interrupts
 * off, which is enough on the machine's one processor.
 */
static uint8_t cons_grid[CONSOLE_ROWS][CONSOLE_COLUMNS];
static unsigned cons_row;
static unsigned cons_column;

static void draw_cell(unsigned row, unsigned column);
static void clear_screen(void);
static void newline_locked(void);
static void scroll_locked(void);
static void putc_locked(int character);

/*
 * Sets the text screen's palette and clears the console.
 *
 * The firmware's display timings are kept; only the two colours the
 * console uses are set.
 */
void
x68k_cons_init(
	void)
{
	/* Makes palette entry 0 black and entry 7 white. */
	hal_mmio_write16((volatile void *)(X68K_DEVICE_ADDRESS(CONSOLE_TEXT_PALETTE) + CONSOLE_PALETTE_BLACK), 0x0000U);
	hal_mmio_write16((volatile void *)(X68K_DEVICE_ADDRESS(CONSOLE_TEXT_PALETTE) + CONSOLE_PALETTE_WHITE), 0xffffU);

	/* Starts with a blank screen and the cursor at the top left. */
	clear_screen();
}

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

	/* Changes the grid and draws it as one step against interrupt handlers. */
	enabled = hal_irq_disable();

	putc_locked(c);

	if (enabled)
		hal_irq_enable();
}

/* Draws one cell of the grid from the character ROM. */
static void
draw_cell(
	unsigned row,
	unsigned column)
{
	const volatile uint8_t *font;
	volatile uint8_t *plane_base;
	uintptr_t offset;
	unsigned character;
	unsigned scanline;
	unsigned plane;
	uint8_t pattern;

	/* The glyph of the character; a cell never written is a blank. */
	font = (const volatile uint8_t *)X68K_DEVICE_ADDRESS(CONSOLE_CGROM_8X16);
	character = cons_grid[row][column];
	if (character == 0)
		character = ' ';

	/* Each scanline of the glyph, in each plane: the glyph in the first three, nothing in the fourth. */
	for (scanline = 0; scanline < CONSOLE_FONT_HEIGHT; scanline++) {
		offset = (uintptr_t)row * CONSOLE_FONT_HEIGHT * CONSOLE_ROW_BYTES +
			 (uintptr_t)scanline * CONSOLE_ROW_BYTES + column;
		pattern = font[character * CONSOLE_FONT_HEIGHT + scanline];
		for (plane = 0; plane < CONSOLE_PLANES; plane++) {
			plane_base = (volatile uint8_t *)X68K_DEVICE_ADDRESS(X68K_TVRAM_PHYSICAL + plane * CONSOLE_PLANE_BYTES);
			plane_base[offset] = 0;
			if (plane < CONSOLE_GLYPH_PLANES)
				plane_base[offset] = pattern;
		}
	}
}

/* Blanks every cell and puts the cursor at the top left. */
static void
clear_screen(
	void)
{
	unsigned row;
	unsigned column;

	/* Blanks and draws each cell. */
	for (row = 0; row < CONSOLE_ROWS; row++) {
		for (column = 0; column < CONSOLE_COLUMNS; column++) {
			cons_grid[row][column] = ' ';
			draw_cell(row, column);
		}
	}

	/* Homes the cursor. */
	cons_row = 0;
	cons_column = 0;
}

/* Moves the cursor to the start of the next line, scrolling at the bottom. */
static void
newline_locked(
	void)
{
	/* Moves down a row, or scrolls instead of leaving the last one. */
	cons_column = 0;
	cons_row++;
	if (cons_row >= CONSOLE_ROWS) {
		cons_row = CONSOLE_ROWS - 1U;
		scroll_locked();
	}
}

/* Moves every row up by one and blanks the bottom row. */
static void
scroll_locked(
	void)
{
	unsigned row;
	unsigned column;

	/* Shifts the rows up and redraws them. */
	for (row = 1; row < CONSOLE_ROWS; row++) {
		for (column = 0; column < CONSOLE_COLUMNS; column++) {
			cons_grid[row - 1U][column] = cons_grid[row][column];
			draw_cell(row - 1U, column);
		}
	}

	/* Blanks the row exposed at the bottom. */
	for (column = 0; column < CONSOLE_COLUMNS; column++) {
		cons_grid[CONSOLE_ROWS - 1U][column] = ' ';
		draw_cell(CONSOLE_ROWS - 1U, column);
	}
}

/* Puts one character at the cursor. */
static void
putc_locked(
	int character)
{
	/* A newline moves to the next line. */
	if (character == '\n') {
		newline_locked();
		return;
	}

	/* A carriage return moves to the line's start. */
	if (character == '\r') {
		cons_column = 0;
		return;
	}

	/* A backspace steps back and erases within the line. */
	if (character == '\b') {
		if (cons_column > 0)
			cons_column--;
		cons_grid[cons_row][cons_column] = ' ';
		draw_cell(cons_row, cons_column);
		return;
	}

	/* A tab moves to the next eight-column stop. */
	if (character == '\t') {
		cons_column = (cons_column + 8U) & ~7U;
		if (cons_column >= CONSOLE_COLUMNS)
			newline_locked();
		return;
	}

	/* Wraps before writing past the end of the row. */
	if (cons_column >= CONSOLE_COLUMNS)
		newline_locked();

	/* Stores a printable character, or a question mark for anything else, and moves on. */
	cons_grid[cons_row][cons_column] = '?';
	if (character >= 0x20 && character < 0x7f)
		cons_grid[cons_row][cons_column] = (uint8_t)character;
	draw_cell(cons_row, cons_column);
	cons_column++;
}
