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
 * 32-bit, 16-bit (5-6-5), 15-bit (5-5-5) or 8-bit pixels.
 */

#include <hal/hal.h>
#include "font.h"
#include "framebuffer.h"

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

static void put_pixel(uint32_t x, uint32_t y, unsigned colour);

/*
 * Takes the firmware's display.  Returns HAL_OK, or HAL_ERR_UNSUPPORTED
 * for a display too small for the grid or of a depth the HAL cannot draw.
 */
int
virt_framebuffer_init(
	uintptr_t address,
	uint32_t width,
	uint32_t height,
	uint32_t linebytes,
	uint32_t depth)
{
	uint32_t row;
	uint32_t column;

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
	hal_printf("PPC64 FRAMEBUFFER %ux%u depth %u phys=%lx\n",
		   framebuffer_width, framebuffer_height, framebuffer_depth, (unsigned long)(uintptr_t)framebuffer_base);
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

	/* The colour packed for the depth; 8-bit pixels are the colour's index. */
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
		((volatile uint8_t *)at)[x] = (uint8_t)colour;
		break;
	}
}
