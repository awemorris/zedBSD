/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Macintosh's screen: the frame buffer the ROM (or the emulator) set
 * up, whose address, depth, row length and size the boot information
 * gives.  The HAL draws text cells of the 8x16 font on it, light grey on
 * black, until the kernel publishes its own console.
 *
 * Two depths are drawn.  At 32 bits (which the boot information calls 24)
 * a pixel is four bytes, unused, red, green, blue.  At 8 bits a pixel is
 * an index into the colour table, which holds the RGB332 colours: the
 * loader sets the table so before it leaves the ROM, and on the emulator's
 * Quadra 800, which the kernel is started on directly, the HAL loads the
 * table itself through the DAFB video controller.
 *
 * Values referenced: QEMU v10.0.0 hw/display/macfb.c (the DAFB registers
 * at 0x800000 past the video base: the colour table index at 0x200 and
 * its data at 0x210, red, green and blue in turn; the 24-bit mode's pixel
 * layout) and hw/m68k/q800.c (the video base 0xf9000000).
 * include/kern/boot.h (the RGB332 colours).
 */

#include <hal/hal.h>
#include "../m68040/asm.h"
#include "bsp.h"
#include "font.h"

/* The most text cells the console keeps in a row and in a column. */
#define FRAMEBUFFER_MAX_COLUMNS	128U
#define FRAMEBUFFER_MAX_ROWS	64U

/* The cell size, which is the font's. */
#define CELL_WIDTH		8U
#define CELL_HEIGHT		MAC68K_FONT_HEIGHT

/* The DAFB colour table registers of the Quadra 800. */
#define DAFB_LUT_INDEX		0xf9800200U
#define DAFB_LUT_DATA		0xf9800210U

/* The text and background colours at 32 bits (light grey, black). */
#define COLOUR32_TEXT		0x00aaaaaaU
#define COLOUR32_BACKGROUND	0x00000000U

/*
 * The text and background colours at 8 bits, as RGB332 indices: red 5,
 * green 5 and blue 2 of 7, 7 and 3, which shows 0xb6b6aa; and black.
 */
#define COLOUR8_TEXT		0xb6U
#define COLOUR8_BACKGROUND	0x00U

/*
 * The screen's memory and shape, and how many cells it holds; zero until
 * the screen is taken, and fixed afterwards.
 */
static uint8_t *framebuffer_base;
static uint32_t framebuffer_width;
static uint32_t framebuffer_height;
static uint32_t framebuffer_row_bytes;
static uint32_t framebuffer_bytes_per_pixel;
static unsigned framebuffer_columns;
static unsigned framebuffer_rows;

static void load_rgb332_table(void);
static void fill_pixels(uint32_t x, uint32_t y, uint32_t width, uint32_t height, int text);
static void put_pixel(uint8_t *row, uint32_t x, int text);

/*
 * Takes the screen the boot information describes and clears it.
 * Reports HAL_OK, or why the screen cannot be drawn on.
 */
int
mac68k_framebuffer_init(
	void)
{
	const struct mac68k_board_info *board;
	unsigned columns;
	unsigned rows;

	/* Requires a screen of a depth the HAL draws. */
	board = mac68k_board();
	if (board->video_base == 0 || board->video_width == 0 || board->video_height == 0)
		return HAL_ERR_UNSUPPORTED;
	if (board->video_depth == 8U) {
		framebuffer_bytes_per_pixel = 1U;
	} else if (board->video_depth == 24U || board->video_depth == 32U) {
		framebuffer_bytes_per_pixel = 4U;
	} else {
		return HAL_ERR_UNSUPPORTED;
	}

	/* Requires rows long enough for the pixels. */
	if (board->video_row_bytes < board->video_width * framebuffer_bytes_per_pixel)
		return HAL_ERR_INVALID;

	/* Takes the shape and the cells it holds. */
	framebuffer_base = (uint8_t *)(uintptr_t)board->video_base;
	framebuffer_width = board->video_width;
	framebuffer_height = board->video_height;
	framebuffer_row_bytes = board->video_row_bytes;
	columns = framebuffer_width / CELL_WIDTH;
	rows = framebuffer_height / CELL_HEIGHT;
	if (columns > FRAMEBUFFER_MAX_COLUMNS)
		columns = FRAMEBUFFER_MAX_COLUMNS;
	if (rows > FRAMEBUFFER_MAX_ROWS)
		rows = FRAMEBUFFER_MAX_ROWS;
	framebuffer_columns = columns;
	framebuffer_rows = rows;

	/* On the emulator's Quadra 800 no loader set the colour table: the HAL does. */
	if (framebuffer_bytes_per_pixel == 1U && board->model == MAC68K_MODEL_QUADRA_800)
		load_rgb332_table();

	/* Succeeded: a black screen. */
	fill_pixels(0, 0, framebuffer_width, framebuffer_height, 0);
	return HAL_OK;
}

/*
 * Reports how many cells a row of the screen holds; zero without a screen.
 */
unsigned
mac68k_framebuffer_columns(
	void)
{
	/* Fixed when the screen was taken. */
	return framebuffer_columns;
}

/*
 * Reports how many rows of cells the screen holds; zero without a screen.
 */
unsigned
mac68k_framebuffer_rows(
	void)
{
	/* Fixed when the screen was taken. */
	return framebuffer_rows;
}

/*
 * Draws one cell: a character, with the cursor's underline when asked.
 */
void
mac68k_framebuffer_cell(
	unsigned row,
	unsigned column,
	int character,
	bool cursor)
{
	const uint8_t *glyph;
	uint8_t *line;
	uint32_t x;
	uint32_t y;
	unsigned glyph_row;
	unsigned bit;
	uint8_t bits;
	int lit;

	/* Nothing is drawn without a screen or outside it. */
	if (framebuffer_base == NULL)
		return;
	if (row >= framebuffer_rows || column >= framebuffer_columns)
		return;

	/* The glyph, and the cell's top left pixel. */
	glyph = &mac68k_font8x16[((unsigned)character & 0xffU) * MAC68K_FONT_HEIGHT];
	x = column * CELL_WIDTH;
	y = row * CELL_HEIGHT;

	/* Each row of the glyph, its last two filled under the cursor. */
	for (glyph_row = 0; glyph_row < CELL_HEIGHT; glyph_row++) {
		bits = glyph[glyph_row];
		if (cursor && glyph_row >= CELL_HEIGHT - 2U)
			bits = 0xff;
		line = framebuffer_base + (y + glyph_row) * framebuffer_row_bytes;
		for (bit = 0; bit < CELL_WIDTH; bit++) {
			/* A set bit of the glyph is a pixel of the character. */
			lit = 0;
			if ((bits & (0x80U >> bit)) != 0)
				lit = 1;
			put_pixel(line, x + bit, lit);
		}
	}
}

/*
 * Moves every row of cells up by one and clears the bottom row.
 */
void
mac68k_framebuffer_scroll(
	void)
{
	uint32_t line;
	uint32_t text_lines;
	uint32_t row_length;

	/* Nothing is moved without a screen. */
	if (framebuffer_base == NULL || framebuffer_rows == 0)
		return;

	/* Each pixel line of the text area takes the one a cell below it. */
	text_lines = framebuffer_rows * CELL_HEIGHT;
	row_length = framebuffer_columns * CELL_WIDTH * framebuffer_bytes_per_pixel;
	for (line = 0; line + CELL_HEIGHT < text_lines; line++) {
		hal_memcpy(framebuffer_base + line * framebuffer_row_bytes,
			   framebuffer_base + (line + CELL_HEIGHT) * framebuffer_row_bytes,
			   row_length);
	}

	/* The bottom row is blank. */
	fill_pixels(0, text_lines - CELL_HEIGHT, framebuffer_columns * CELL_WIDTH, CELL_HEIGHT, 0);
}

/*
 * Writes one line that describes the screen.
 */
void
mac68k_framebuffer_describe(
	void)
{
	const struct mac68k_board_info *board;

	/* The address, the size, the depth and the cells. */
	board = mac68k_board();
	hal_printf("MAC68K FRAMEBUFFER %x %ux%u depth %u row %u cells %ux%u\n",
		   board->video_base,
		   framebuffer_width,
		   framebuffer_height,
		   board->video_depth,
		   framebuffer_row_bytes,
		   framebuffer_columns,
		   framebuffer_rows);
}

/*
 * Loads the DAFB colour table with the RGB332 colours: index i shows red
 * ((i >> 5) & 7) * 255 / 7, green ((i >> 2) & 7) * 255 / 7 and blue
 * (i & 3) * 255 / 3.
 */
static void
load_rgb332_table(
	void)
{
	volatile uint32_t *index_register;
	volatile uint32_t *data_register;
	unsigned entry;

	/* The table is written from entry 0, three components an entry. */
	index_register = (volatile uint32_t *)(uintptr_t)DAFB_LUT_INDEX;
	data_register = (volatile uint32_t *)(uintptr_t)DAFB_LUT_DATA;
	*index_register = 0;
	for (entry = 0; entry < 256U; entry++) {
		*data_register = ((entry >> 5) & 7U) * 255U / 7U;
		*data_register = ((entry >> 2) & 7U) * 255U / 7U;
		*data_register = (entry & 3U) * 255U / 3U;
	}
}

/* Fills a rectangle of pixels with the text or the background colour. */
static void
fill_pixels(
	uint32_t x,
	uint32_t y,
	uint32_t width,
	uint32_t height,
	int text)
{
	uint8_t *line;
	uint32_t row;
	uint32_t column;

	/* Each pixel line of the rectangle, left to right. */
	for (row = 0; row < height; row++) {
		line = framebuffer_base + (y + row) * framebuffer_row_bytes;
		for (column = 0; column < width; column++)
			put_pixel(line, x + column, text);
	}
}

/* Writes one pixel of a pixel line in the text or the background colour. */
static void
put_pixel(
	uint8_t *row,
	uint32_t x,
	int text)
{
	/* One byte, an index into the RGB332 table. */
	if (framebuffer_bytes_per_pixel == 1U) {
		row[x] = COLOUR8_BACKGROUND;
		if (text)
			row[x] = COLOUR8_TEXT;
		return;
	}

	/* Four bytes: unused, red, green, blue. */
	((uint32_t *)(void *)row)[x] = COLOUR32_BACKGROUND;
	if (text)
		((uint32_t *)(void *)row)[x] = COLOUR32_TEXT;
}
