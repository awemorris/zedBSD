/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The board's framebuffer: the display the firmware set up, whose address,
 * size, row length and depth the firmware reports.  The HAL draws on it
 * directly, 80 by 25 cells of the 8x16 font at the screen's top left, in
 * 32-bit, 16-bit (5-6-5), 15-bit (5-5-5) or 8-bit pixels, until the
 * kernel publishes its own console; then nothing here draws again.  The
 * screen is handed over as "mac99.framebuffer" (32-bit words: the
 * physical address, the width, the height, the bytes a row and the bits
 * a pixel) and, at 8 bits, the colours the HAL takes the table to show as
 * "mac99.framebuffer.palette" (256 words of 0xRRGGBB, index 0 first), so
 * the kernel's console draws with the same entries.
 *
 * An 8-bit pixel is an index into the display's colour table, which the
 * firmware loaded and the HAL leaves as it is.  Apple's firmware leaves
 * the Macintosh 8-bit system table, whose index 0 is white and index 7 a
 * light pink, so drawing the colour numbers as they are shows pink text
 * on white; each text colour is drawn instead with the system table's
 * entry nearest to it (black 255, light grey 248).
 *
 * Values referenced: Inside Macintosh: Imaging With QuickDraw (1994), the
 * 8-bit system colour table: indices 0 to 214 the 6x6x6 cube of 0x33
 * steps from white down (red the slowest, blue the fastest), 215 to 254
 * ten-step ramps of red, green, blue and grey (0xee down to 0x11), 255
 * black.
 */

#include <hal/hal.h>
#include "font.h"
#include "framebuffer.h"
#include "mac-arch.h"

/* The 8-bit system colour table: its entries, the cube's entries and step, the ramps' start and their ten levels. */
#define MAC_TABLE_ENTRIES	256U
#define MAC_CUBE_ENTRIES	215U
#define MAC_CUBE_STEP		0x33U
#define MAC_RAMP_FIRST		215U
#define MAC_RAMP_LEVELS		10U

/* The screen's memory and shape; zero until the firmware's display is taken. */
static uint8_t *framebuffer_base;
static uint32_t framebuffer_width;
static uint32_t framebuffer_height;
static uint32_t framebuffer_linebytes;
static uint32_t framebuffer_depth;

/*
 * The colours of the sixteen text attributes, as 0xRRGGBB values.
 *
 * Values referenced: the IBM VGA text-mode palette, the sixteen default
 * attribute colours.
 */
static const uint32_t framebuffer_palette[16] = {
	0x000000U, 0x0000aaU, 0x00aa00U, 0x00aaaaU,
	0xaa0000U, 0xaa00aaU, 0xaa5500U, 0xaaaaaaU,
	0x555555U, 0x5555ffU, 0x55ff55U, 0x55ffffU,
	0xff5555U, 0xff55ffU, 0xffff55U, 0xffffffU
};

/*
 * The colour table index each text colour is drawn with at 8 bits: the
 * system table's entry nearest the colour.  Set when the screen is taken.
 */
static uint8_t framebuffer_index[16];

/*
 * The screen as the kernel takes it over: the physical address, the
 * width, the height, the bytes a row and the bits a pixel.  Filled once
 * when the screen is taken; all zero before, and when it is not.
 */
static uint32_t framebuffer_handoff[5];

/*
 * The colours the 8-bit table is taken to show, as 0xRRGGBB, index 0
 * first: the system table.  Filled once when an 8-bit screen is taken.
 */
static uint32_t framebuffer_table[MAC_TABLE_ENTRIES];

static void put_pixel(uint32_t x, uint32_t y, unsigned colour);
static unsigned nearest_entry(uint32_t colour);
static uint32_t mac_system_colour(unsigned index);
static uint32_t colour_distance(uint32_t first, uint32_t second);

/*
 * Takes the firmware's display.  Returns HAL_OK, or HAL_ERR_UNSUPPORTED
 * for a display too small for the grid or of a depth the HAL cannot draw.
 */
int
virt_framebuffer_init(
	uint32_t address,
	uint32_t width,
	uint32_t height,
	uint32_t linebytes,
	uint32_t depth)
{
	uint32_t row;
	uint32_t column;
	unsigned colour;
	unsigned index;

	/* The grid must fit, at a depth the drawing knows. */
	if (address == 0)
		return HAL_ERR_UNSUPPORTED;
	if (width < VIRT_CONSOLE_COLUMNS * 8U || height < VIRT_CONSOLE_ROWS * VIRT_FONT_HEIGHT)
		return HAL_ERR_UNSUPPORTED;
	if (depth != 32U &&
	    depth != 16U &&
	    depth != 15U &&
	    depth != 8U)
		return HAL_ERR_UNSUPPORTED;
	framebuffer_base = (uint8_t *)(uintptr_t)address;
	framebuffer_width = width;
	framebuffer_height = height;
	framebuffer_linebytes = linebytes;
	framebuffer_depth = depth;

	/* The colours of the table the firmware is taken to have left. */
	for (index = 0; index < MAC_TABLE_ENTRIES; index++)
		framebuffer_table[index] = mac_system_colour(index);

	/* At 8 bits, each text colour's nearest entry of that table. */
	for (colour = 0; colour < 16U; colour++)
		framebuffer_index[colour] = (uint8_t)nearest_entry(framebuffer_palette[colour]);

	/* The screen as the kernel's console takes it over. */
	framebuffer_handoff[0] = address;
	framebuffer_handoff[1] = width;
	framebuffer_handoff[2] = height;
	framebuffer_handoff[3] = linebytes;
	framebuffer_handoff[4] = depth;

	/* Starts from a black screen. */
	for (row = 0; row < height; row++) {
		for (column = 0; column < width; column++)
			put_pixel(column, row, 0);
	}

	/* Succeeded: drawing reaches the screen. */
	return HAL_OK;
}

/*
 * Reports whether the screen is shown.
 */
int
virt_framebuffer_ready(
	void)
{
	/* Succeeded: whether virt_framebuffer_init() succeeded. */
	return framebuffer_base != NULL;
}

/*
 * Reports the screen as the kernel takes it over, or NULL when the HAL
 * did not take it.
 */
const uint32_t *
virt_framebuffer_handoff(
	void)
{
	/* No screen, nothing to hand over. */
	if (framebuffer_base == NULL)
		return NULL;

	/* Succeeded: the five words. */
	return framebuffer_handoff;
}

/*
 * Reports the colours the 8-bit table is taken to show, or NULL for a
 * screen of another depth or none.
 */
const uint32_t *
virt_framebuffer_palette(
	void)
{
	/* Only an 8-bit screen has a table. */
	if (framebuffer_base == NULL || framebuffer_depth != 8U)
		return NULL;

	/* Succeeded: the 256 colours. */
	return framebuffer_table;
}

/*
 * Draws one text cell: a glyph in the attribute's colours.
 */
void
virt_framebuffer_cell(
	unsigned row,
	unsigned column,
	int character,
	uint8_t attribute)
{
	const uint8_t *glyph;
	unsigned line;
	unsigned bit;
	uint8_t bits;

	/* Nothing to draw before the screen is shown, or off the grid. */
	if (framebuffer_base == NULL)
		return;
	if (row >= VIRT_CONSOLE_ROWS || column >= VIRT_CONSOLE_COLUMNS)
		return;

	/* Each row of the glyph, from its leftmost pixel in the high bit. */
	glyph = &virt_font8x16[((unsigned)character & 0xffU) * VIRT_FONT_HEIGHT];
	for (line = 0; line < VIRT_FONT_HEIGHT; line++) {
		bits = glyph[line];
		for (bit = 0; bit < 8U; bit++) {
			if ((bits & (0x80U >> bit)) != 0) {
				put_pixel(column * 8U + bit, row * VIRT_FONT_HEIGHT + line, attribute & 15U);
			} else {
				put_pixel(column * 8U + bit, row * VIRT_FONT_HEIGHT + line, (attribute >> 4) & 15U);
			}
		}
	}
}

/*
 * Draws the cursor: the two bottom pixel rows of a cell in white.
 */
void
virt_framebuffer_cursor(
	unsigned row,
	unsigned column)
{
	unsigned line;
	unsigned bit;

	/* Nothing to draw before the screen is shown, or off the grid. */
	if (framebuffer_base == NULL)
		return;
	if (row >= VIRT_CONSOLE_ROWS || column >= VIRT_CONSOLE_COLUMNS)
		return;

	/* Whitens rows 14 and 15 of the cell. */
	for (line = VIRT_FONT_HEIGHT - 2U; line < VIRT_FONT_HEIGHT; line++) {
		for (bit = 0; bit < 8U; bit++)
			put_pixel(column * 8U + bit, row * VIRT_FONT_HEIGHT + line, 15U);
	}
}

/*
 * The screen shows what is drawn at once; there is nothing to send.
 */
void
virt_framebuffer_flush(
	void)
{
}

/*
 * Writes the screen's address and shape to the boot log.
 */
void
virt_framebuffer_describe(
	void)
{
	/* Describes a shown screen. */
	if (framebuffer_base == NULL)
		return;
	hal_printf(MAC_ARCH_TAG " FRAMEBUFFER %ux%u depth %u phys=%x\n",
		   framebuffer_width, framebuffer_height, framebuffer_depth, (uint32_t)(uintptr_t)framebuffer_base);
	if (framebuffer_depth == 8U)
		hal_printf(MAC_ARCH_TAG " PALETTE MAC SYSTEM TABLE, BLACK %u GREY %u WHITE %u\n",
			   framebuffer_index[0], framebuffer_index[7], framebuffer_index[15]);
}

/* Writes one pixel in one of the sixteen colours, at the screen's depth. */
static void
put_pixel(
	uint32_t x,
	uint32_t y,
	unsigned colour)
{
	uint8_t *at;
	uint32_t rgb;

	/* The pixel's place and its colour. */
	at = framebuffer_base + y * framebuffer_linebytes;
	rgb = framebuffer_palette[colour];

	/* The colour packed for the depth; an 8-bit pixel is the colour's table index. */
	switch (framebuffer_depth) {
	case 32U:
		((volatile uint32_t *)at)[x] = rgb;
		break;
	case 16U:
		((volatile uint16_t *)at)[x] = (uint16_t)(((rgb >> 8) & 0xf800U) | ((rgb >> 5) & 0x07e0U) | ((rgb >> 3) & 0x001fU));
		break;
	case 15U:
		((volatile uint16_t *)at)[x] = (uint16_t)(((rgb >> 9) & 0x7c00U) | ((rgb >> 6) & 0x03e0U) | ((rgb >> 3) & 0x001fU));
		break;
	default:
		((volatile uint8_t *)at)[x] = framebuffer_index[colour & 15U];
		break;
	}
}

/*
 * Finds the system table's entry nearest a colour; the first of the
 * nearest wins.
 */
static unsigned
nearest_entry(
	uint32_t colour)
{
	uint32_t distance;
	uint32_t best_distance;
	unsigned best;
	unsigned index;

	/* Each entry's distance from the colour. */
	best = 0;
	best_distance = 0xffffffffU;
	for (index = 0; index < MAC_TABLE_ENTRIES; index++) {
		distance = colour_distance(colour, framebuffer_table[index]);
		if (distance < best_distance) {
			best_distance = distance;
			best = index;
		}
	}

	/* Succeeded: the nearest entry. */
	return best;
}

/* Reports an entry of the Macintosh 8-bit system colour table as 0xRRGGBB. */
static uint32_t
mac_system_colour(
	unsigned index)
{
	static const uint8_t levels[MAC_RAMP_LEVELS] = {
		0xee, 0xdd, 0xbb, 0xaa, 0x88, 0x77, 0x55, 0x44, 0x22, 0x11
	};
	uint32_t red;
	uint32_t green;
	uint32_t blue;
	uint32_t level;
	unsigned ramp;

	/* The cube, red the slowest, each from 0xff down in 0x33 steps. */
	if (index < MAC_CUBE_ENTRIES) {
		red = (5U - index / 36U) * MAC_CUBE_STEP;
		green = (5U - (index / 6U) % 6U) * MAC_CUBE_STEP;
		blue = (5U - index % 6U) * MAC_CUBE_STEP;
		return (red << 16) | (green << 8) | blue;
	}

	/* Black at the end. */
	if (index >= MAC_RAMP_FIRST + 4U * MAC_RAMP_LEVELS)
		return 0;

	/* The ramps: red, green, blue, then grey. */
	ramp = (index - MAC_RAMP_FIRST) / MAC_RAMP_LEVELS;
	level = levels[(index - MAC_RAMP_FIRST) % MAC_RAMP_LEVELS];
	if (ramp == 0U)
		return level << 16;
	if (ramp == 1U)
		return level << 8;
	if (ramp == 2U)
		return level;
	return (level << 16) | (level << 8) | level;
}

/* Reports the squared distance between two 0xRRGGBB colours. */
static uint32_t
colour_distance(
	uint32_t first,
	uint32_t second)
{
	int32_t red;
	int32_t green;
	int32_t blue;

	/* Each component's difference, squared and summed. */
	red = (int32_t)((first >> 16) & 0xffU) - (int32_t)((second >> 16) & 0xffU);
	green = (int32_t)((first >> 8) & 0xffU) - (int32_t)((second >> 8) & 0xffU);
	blue = (int32_t)(first & 0xffU) - (int32_t)(second & 0xffU);
	return (uint32_t)(red * red + green * green + blue * blue);
}
