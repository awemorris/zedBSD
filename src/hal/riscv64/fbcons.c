/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The riscv64 early console's drawing.
 *
 * A board hands over the screen it has, the virt board's RAM framebuffer
 * or the one the firmware left on, and the console's grid of 80 by 25
 * cells of the 8x16 font is drawn in the middle of it.  The grid is drawn
 * from the console's copy of the text, never read back from the screen,
 * so a screen the processor reaches uncached costs nothing to read.  A
 * board whose display does not see the processor's caches passes a flush,
 * which is called on each range drawn.
 */

#include <hal/hal.h>
#include <kern/boot.h>
#include "cons.h"
#include "fbcons.h"
#include "font.h"

/* The bytes of one pixel: the only depth the console draws. */
#define FBCONS_PIXEL_BYTES	4U

/* The pixel width of one cell; a glyph is one byte per row. */
#define FBCONS_CELL_WIDTH	8U

/* The size of the grid in pixels. */
#define FBCONS_GRID_WIDTH	(RISCV64_CONSOLE_COLUMNS * FBCONS_CELL_WIDTH)
#define FBCONS_GRID_HEIGHT	(RISCV64_CONSOLE_ROWS * RISCV64_FONT_HEIGHT)

/*
 * The screen the console draws on.  It and the three variables below are
 * written once by riscv64_fbcons_attach() on the boot hart before any
 * other hart runs and read only afterwards, under the console's lock;
 * riscv64_fbcons_move() changes the address once, on the boot hart too.
 */
static struct riscv64_fbcons_screen fbcons_screen;

/* The column of the grid's top-left pixel, which centres the grid on the screen. */
static uint32_t fbcons_origin_x;

/* The row of the grid's top-left pixel. */
static uint32_t fbcons_origin_y;

/* The sixteen colours as pixel values in the screen's format. */
static uint32_t fbcons_colours[16];

/*
 * Nonzero once a screen was attached; until then nothing is drawn and the
 * console only keeps its text.  Set with fbcons_screen, and cleared for
 * good under the console's lock once the kernel's console takes the
 * screen over (riscv64_fbcons_detach()).
 */
static int fbcons_on;

/*
 * The colours of the sixteen text attributes, red in bits 16 to 23,
 * green in 8 to 15 and blue in 0 to 7.
 *
 * Values referenced: the IBM VGA text-mode palette, the sixteen default
 * attribute colours.
 */
static const uint32_t fbcons_palette[16] = {
	0x000000U, 0x0000aaU, 0x00aa00U, 0x00aaaaU,
	0xaa0000U, 0xaa00aaU, 0xaa5500U, 0xaaaaaaU,
	0x555555U, 0x5555ffU, 0x55ff55U, 0x55ffffU,
	0xff5555U, 0xff55ffU, 0xffff55U, 0xffffffU
};

static int check_screen(const struct riscv64_fbcons_screen *screen);
static uint32_t colour_value(uint32_t colour, uint32_t format);
static uint32_t *row_pixels(uint32_t y);
static void flush_span(uint32_t x, uint32_t y, uint32_t width);
static void clear_screen(void);

/*
 * Attaches the early console to a board's screen and draws what it holds.
 *
 * Returns HAL_OK, or HAL_ERR_INVALID for a description the console cannot
 * draw on: no address, a format it does not know, a screen smaller than
 * the grid or rows shorter than the width.  A refused screen leaves the
 * console on the firmware's output alone.
 */
int
riscv64_fbcons_attach(
	const struct riscv64_fbcons_screen *screen)
{
	unsigned index;
	int error;

	/* Refuses a screen the console cannot draw on. */
	error = check_screen(screen);
	if (error != HAL_OK)
		return error;

	/* Keeps the screen and centres the grid on it. */
	fbcons_screen = *screen;
	fbcons_origin_x = (screen->width - FBCONS_GRID_WIDTH) / 2U;
	fbcons_origin_y = (screen->height - FBCONS_GRID_HEIGHT) / 2U;

	/* Spells the colours in the screen's byte order. */
	for (index = 0; index < 16U; index++)
		fbcons_colours[index] = colour_value(fbcons_palette[index], screen->format);

	/* Starts from a black screen, the firmware's last picture gone. */
	clear_screen();

	/*
	 * Lets every later drawing reach the screen, then draws the text printed
	 * before the screen was attached.
	 */
	fbcons_on = 1;
	riscv64_cons_redraw();

	/* Succeeded: drawing reaches the screen. */
	return HAL_OK;
}

/*
 * Reports whether a screen is attached.
 */
int
riscv64_fbcons_ready(
	void)
{
	/* Succeeded: whether riscv64_fbcons_attach() succeeded. */
	return fbcons_on;
}

/*
 * Draws the attached screen through a new address from now on: the early
 * console's window of it.  Called on the boot hart, before any other hart
 * runs, once the kernel's tables hold the window.
 */
void
riscv64_fbcons_move(
	uintptr_t address)
{
	/* Nothing to move without a screen. */
	if (!fbcons_on)
		return;

	/* The same pixels at the window's address. */
	fbcons_screen.address = address;
}

/*
 * Forgets the screen for good: nothing is drawn on it again.  The caller
 * holds the console's lock.
 */
void
riscv64_fbcons_detach(
	void)
{
	/* No drawing reaches the screen from now on. */
	fbcons_on = 0;
}

/*
 * Draws one text cell: a glyph in the attribute's colours.
 *
 * The caller flushes what it drew.
 */
void
riscv64_fbcons_cell(
	unsigned row,
	unsigned column,
	int character,
	uint8_t attribute)
{
	const uint8_t *glyph;
	uint32_t *pixels;
	uint32_t foreground;
	uint32_t background;
	uint32_t value;
	uint32_t x;
	uint32_t y;
	unsigned line;
	unsigned bit;
	uint8_t bits;

	/* Nothing to draw before a screen is attached, or off the grid. */
	if (!fbcons_on)
		return;
	if (row >= RISCV64_CONSOLE_ROWS || column >= RISCV64_CONSOLE_COLUMNS)
		return;

	/* The glyph of the character (the font covers every byte value), its colours and its corner. */
	glyph = &riscv64_font8x16[((unsigned)character & 0xffU) * RISCV64_FONT_HEIGHT];
	foreground = fbcons_colours[attribute & 15U];
	background = fbcons_colours[(attribute >> 4) & 15U];
	x = fbcons_origin_x + (uint32_t)column * FBCONS_CELL_WIDTH;
	y = fbcons_origin_y + (uint32_t)row * RISCV64_FONT_HEIGHT;

	/* Each row of the glyph, from its leftmost pixel in the high bit. */
	for (line = 0; line < RISCV64_FONT_HEIGHT; line++) {
		/* Takes the screen row and the glyph's bits for it. */
		pixels = row_pixels(y + line);
		bits = glyph[line];

		/* Paints the row's pixels of the cell. */
		for (bit = 0; bit < FBCONS_CELL_WIDTH; bit++) {
			/* A set bit is the glyph's ink, a clear one its background. */
			value = background;
			if ((bits & (0x80U >> bit)) != 0)
				value = foreground;
			pixels[x + bit] = value;
		}
	}
}

/*
 * Draws the cursor: the two bottom pixel rows of a cell in white.
 *
 * The caller flushes what it drew.
 */
void
riscv64_fbcons_cursor(
	unsigned row,
	unsigned column)
{
	uint32_t *pixels;
	uint32_t x;
	uint32_t y;
	unsigned line;
	unsigned bit;

	/* Nothing to draw before a screen is attached, or off the grid. */
	if (!fbcons_on)
		return;
	if (row >= RISCV64_CONSOLE_ROWS || column >= RISCV64_CONSOLE_COLUMNS)
		return;

	/* Whitens rows 14 and 15 of the cell. */
	x = fbcons_origin_x + (uint32_t)column * FBCONS_CELL_WIDTH;
	y = fbcons_origin_y + (uint32_t)row * RISCV64_FONT_HEIGHT;
	for (line = RISCV64_FONT_HEIGHT - 2U; line < RISCV64_FONT_HEIGHT; line++) {
		/* Whitens the row's pixels of the cell. */
		pixels = row_pixels(y + line);
		for (bit = 0; bit < FBCONS_CELL_WIDTH; bit++)
			pixels[x + bit] = fbcons_colours[15];
	}
}

/*
 * Writes one drawn cell through to the display: each of its pixel rows.
 */
void
riscv64_fbcons_flush_cell(
	unsigned row,
	unsigned column)
{
	uint32_t x;
	uint32_t y;
	unsigned line;

	/* A screen the display sees directly, or none, needs nothing. */
	if (!fbcons_on || fbcons_screen.flush == NULL)
		return;
	if (row >= RISCV64_CONSOLE_ROWS || column >= RISCV64_CONSOLE_COLUMNS)
		return;

	/* Each pixel row of the cell is one range of the screen. */
	x = fbcons_origin_x + (uint32_t)column * FBCONS_CELL_WIDTH;
	y = fbcons_origin_y + (uint32_t)row * RISCV64_FONT_HEIGHT;
	for (line = 0; line < RISCV64_FONT_HEIGHT; line++)
		flush_span(x, y + line, FBCONS_CELL_WIDTH);
}

/*
 * Writes the whole drawn grid through to the display: each of its pixel
 * rows, once.
 */
void
riscv64_fbcons_flush_grid(
	void)
{
	uint32_t line;

	/* A screen the display sees directly, or none, needs nothing. */
	if (!fbcons_on || fbcons_screen.flush == NULL)
		return;

	/* Each pixel row of the grid is one range of the screen. */
	for (line = 0; line < FBCONS_GRID_HEIGHT; line++)
		flush_span(fbcons_origin_x, fbcons_origin_y + line, FBCONS_GRID_WIDTH);
}

/* Reports whether the console can draw on a described screen. */
static int
check_screen(
	const struct riscv64_fbcons_screen *screen)
{
	/* Refuses a missing description or one without memory. */
	if (screen == NULL || screen->address == 0)
		return HAL_ERR_INVALID;

	/* Refuses a pixel layout the console does not spell. */
	if (screen->format != KERN_BOOT_FRAMEBUFFER_RGBX8888 &&
	    screen->format != KERN_BOOT_FRAMEBUFFER_BGRX8888)
		return HAL_ERR_INVALID;

	/* Refuses a screen the grid does not fit on. */
	if (screen->width < FBCONS_GRID_WIDTH || screen->height < FBCONS_GRID_HEIGHT)
		return HAL_ERR_INVALID;

	/* Refuses rows shorter than the width or not whole pixels. */
	if ((uint64_t)screen->pitch < (uint64_t)screen->width * FBCONS_PIXEL_BYTES)
		return HAL_ERR_INVALID;
	if ((screen->pitch % FBCONS_PIXEL_BYTES) != 0)
		return HAL_ERR_INVALID;

	/* Succeeded: the screen holds the grid. */
	return HAL_OK;
}

/*
 * Spells a palette colour as a pixel of the screen's format, read as a
 * little-endian word: blue first is the palette's own order, red first has
 * red and blue exchanged.
 */
static uint32_t
colour_value(
	uint32_t colour,
	uint32_t format)
{
	uint32_t red;
	uint32_t green;
	uint32_t blue;

	/* A blue-first screen stores the palette's value as it is. */
	if (format == KERN_BOOT_FRAMEBUFFER_BGRX8888)
		return colour;

	/* A red-first screen holds red in the lowest byte. */
	red = (colour >> 16) & 0xffU;
	green = (colour >> 8) & 0xffU;
	blue = colour & 0xffU;

	/* Succeeded: the red-first pixel. */
	return (blue << 16) | (green << 8) | red;
}

/* Reports the first pixel of a row of the screen. */
static uint32_t *
row_pixels(
	uint32_t y)
{
	uintptr_t address;

	/* Each row starts one pitch after the one above. */
	address = fbcons_screen.address + (uintptr_t)y * fbcons_screen.pitch;

	/* Succeeded: the row's pixels. */
	return (uint32_t *)address;
}

/* Writes a run of pixels of one row through to the display. */
static void
flush_span(
	uint32_t x,
	uint32_t y,
	uint32_t width)
{
	uintptr_t start;

	/* The run starts at its first pixel and is width pixels long. */
	start = (uintptr_t)row_pixels(y) + (uintptr_t)x * FBCONS_PIXEL_BYTES;
	fbcons_screen.flush(start, (size_t)width * FBCONS_PIXEL_BYTES);
}

/* Paints the whole screen black and writes it through. */
static void
clear_screen(
	void)
{
	uint32_t *pixels;
	uint32_t y;
	uint32_t x;

	/* Clears each row's visible pixels, then writes the row through. */
	for (y = 0; y < fbcons_screen.height; y++) {
		/* Blackens the row's visible pixels. */
		pixels = row_pixels(y);
		for (x = 0; x < fbcons_screen.width; x++)
			pixels[x] = 0;

		/* A display that does not see the caches gets the row written through. */
		if (fbcons_screen.flush != NULL)
			fbcons_screen.flush((uintptr_t)pixels, (size_t)fbcons_screen.width * FBCONS_PIXEL_BYTES);
	}
}
