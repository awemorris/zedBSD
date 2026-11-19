/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The second stage's text on the screen the ROM set up.
 *
 * The loader draws the 8x16 font straight into the frame buffer at any
 * of the depths the ROM uses (1, 2, 4, 8, 16 and 32 bits).  Once the
 * loader has loaded the RGB332 colours into an 8-bit screen's colour
 * table, the text is light grey on black, as the kernel's console draws
 * it.  Before that, an indexed screen still has the ROM's table, where 0
 * is white and all ones is black, and a direct-colour screen shows 0 as
 * black and all ones as white: the text is drawn in all ones on 0.
 */

#include "bootloader/mac68k/stage2.h"
#include "bootloader/mac68k/screen.h"

/* The RGB332 colours of the text and the background (light grey, black). */
#define RGB332_TEXT		0xb6U
#define RGB332_BACKGROUND	0x00U

/*
 * The screen's memory and shape and where the next character goes; zero
 * until the screen is taken.
 */
static uint8_t *screen_base;
static uint32_t screen_row_bytes;
static uint32_t screen_depth;
static unsigned screen_columns;
static unsigned screen_rows;
static unsigned screen_column;
static unsigned screen_row;

/* The pixel values of the text and of the background at the screen's depth. */
static uint32_t screen_foreground;
static uint32_t screen_background;

static void put_character(char character);
static void new_line(void);
static void draw_glyph(unsigned column, unsigned row, char character);
static void put_pixel(uint8_t *line, uint32_t x, uint32_t value);
static void fill_row(unsigned row);

/*
 * Takes the screen the ROM set up, its address, row length, size and
 * depth, and whether its colour table holds the RGB332 colours.
 */
void
mac68k_screen_init(
	uint8_t *base,
	uint32_t row_bytes,
	uint32_t width,
	uint32_t height,
	uint32_t depth,
	int rgb332)
{
	uint32_t all_ones;

	/* The screen and how many cells it holds. */
	screen_base = base;
	screen_row_bytes = row_bytes;
	screen_depth = depth;
	screen_columns = width / MAC68K_FONT_WIDTH;
	screen_rows = height / MAC68K_FONT_HEIGHT;
	screen_column = 0;
	screen_row = 0;

	/* The value with every bit of a pixel set. */
	all_ones = 0xffffffffU;
	if (depth < 32U)
		all_ones = (1U << depth) - 1U;

	/* The colours: RGB332 grey on black, or all ones on zero. */
	if (rgb332) {
		screen_foreground = RGB332_TEXT;
		screen_background = RGB332_BACKGROUND;
	} else {
		screen_foreground = all_ones;
		screen_background = 0;
	}
}

/*
 * Fills the screen with the background and starts at its top.
 */
void
mac68k_screen_clear(
	void)
{
	unsigned row;

	/* Nothing to clear before the screen is taken. */
	if (screen_base == NULL)
		return;

	/* Fills every text row. */
	for (row = 0; row < screen_rows; row++)
		fill_row(row);

	screen_column = 0;
	screen_row = 0;
}

/*
 * Writes text, a line feed starting a new line.
 */
void
mac68k_screen_puts(
	const char *text)
{
	uint32_t index;

	/* Nothing to draw before the screen is taken. */
	if (screen_base == NULL)
		return;

	/* Draws each character in turn. */
	for (index = 0; text[index] != '\0'; index++)
		put_character(text[index]);
}

/*
 * Writes a 32-bit value as eight hexadecimal digits.
 */
void
mac68k_screen_put_hex(
	uint32_t value)
{
	static const char digits[] = "0123456789abcdef";
	char text[11];
	unsigned index;

	/* The prefix, then the digits from the most significant one. */
	text[0] = '0';
	text[1] = 'x';
	for (index = 0; index < 8U; index++)
		text[2U + index] = digits[(value >> (28U - 4U * index)) & 0xfU];
	text[10] = '\0';

	mac68k_screen_puts(text);
}

/*
 * Writes a value in decimal.
 */
void
mac68k_screen_put_decimal(
	uint32_t value)
{
	char text[11];
	unsigned at;

	/* Writes the digits backwards from the end of the buffer. */
	at = sizeof(text) - 1U;
	text[at] = '\0';
	do {
		at--;
		text[at] = (char)('0' + value % 10U);
		value /= 10U;
	} while (value != 0U);

	mac68k_screen_puts(text + at);
}

/* Draws one character at the cursor, or starts a new line. */
static void
put_character(
	char character)
{
	/* A line feed ends the line. */
	if (character == '\n') {
		new_line();
		return;
	}

	/* A full line wraps to the next one. */
	if (screen_column >= screen_columns)
		new_line();

	/* Draws the glyph and moves on. */
	draw_glyph(screen_column, screen_row, character);
	screen_column++;
}

/* Moves to the start of the next line, scrolling the text up at the bottom. */
static void
new_line(
	void)
{
	uint32_t row_size;
	uint32_t moved;

	/* Moves down a row while there is one. */
	screen_column = 0;
	if (screen_row + 1U < screen_rows) {
		screen_row++;
		return;
	}

	/* Scrolls every text row up by one and clears the last. */
	row_size = screen_row_bytes * MAC68K_FONT_HEIGHT;
	moved = row_size * (screen_rows - 1U);
	mac68k_copy_bytes(screen_base, screen_base + row_size, moved);
	fill_row(screen_rows - 1U);
}

/* Draws one glyph in its cell, an unknown character as a space. */
static void
draw_glyph(
	unsigned column,
	unsigned row,
	char character)
{
	const uint8_t *glyph;
	uint8_t *line;
	uint32_t code;
	uint32_t x;
	unsigned y;
	unsigned bit;

	/* The glyph, or the space for a character the font does not hold. */
	code = (uint8_t)character;
	if (code < MAC68K_FONT_FIRST || code >= MAC68K_FONT_FIRST + MAC68K_FONT_GLYPHS)
		code = MAC68K_FONT_FIRST;
	glyph = &mac68k_loader_font[(code - MAC68K_FONT_FIRST) * MAC68K_FONT_HEIGHT];

	/* Draws each of the glyph's rows, pixel by pixel. */
	for (y = 0; y < MAC68K_FONT_HEIGHT; y++) {
		line = screen_base + (row * MAC68K_FONT_HEIGHT + y) * screen_row_bytes;
		for (bit = 0; bit < MAC68K_FONT_WIDTH; bit++) {
			x = column * MAC68K_FONT_WIDTH + bit;

			/* A set bit is the text's colour, a clear one the background's. */
			if ((glyph[y] & (0x80U >> bit)) != 0U) {
				put_pixel(line, x, screen_foreground);
			} else {
				put_pixel(line, x, screen_background);
			}
		}
	}
}

/* Stores one pixel's value at its depth: bits of a byte, a byte, or bytes. */
static void
put_pixel(
	uint8_t *line,
	uint32_t x,
	uint32_t value)
{
	uint32_t bit_offset;
	uint32_t shift;
	uint32_t mask;
	uint8_t *at;

	/* Pixels of fewer bits than a byte share it, the leftmost in the high bits. */
	if (screen_depth < 8U) {
		bit_offset = x * screen_depth;
		at = line + bit_offset / 8U;
		shift = 8U - screen_depth - bit_offset % 8U;
		mask = ((1U << screen_depth) - 1U) << shift;
		*at = (uint8_t)((*at & ~mask) | ((value << shift) & mask));
		return;
	}

	/* A pixel of whole bytes, the most significant first. */
	switch (screen_depth) {
	case 8:
		line[x] = (uint8_t)value;
		break;
	case 16:
		mac68k_write_be16(line + x * 2U, (uint16_t)value);
		break;
	default:
		mac68k_write_be32(line + x * 4U, value);
		break;
	}
}

/* Fills one text row with the background. */
static void
fill_row(
	unsigned row)
{
	uint8_t *line;
	uint32_t x;
	uint32_t width;
	unsigned y;

	/* Every pixel of every line of the row. */
	width = screen_columns * MAC68K_FONT_WIDTH;
	for (y = 0; y < MAC68K_FONT_HEIGHT; y++) {
		line = screen_base + (row * MAC68K_FONT_HEIGHT + y) * screen_row_bytes;
		for (x = 0; x < width; x++)
			put_pixel(line, x, screen_background);
	}
}
