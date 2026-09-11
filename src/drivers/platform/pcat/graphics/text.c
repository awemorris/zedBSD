/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PC/AT text console layer.
 *
 * /dev/graphics owns the display, so character output lives here beside the
 * graphics backend rather than in the HAL. This layer owns the cell array,
 * the cursor, scrolling and glyph drawing; /dev/console owns the terminal
 * discipline above it.
 */

#include <hal/hal.h>
#include <kern/lock.h>
#include <stddef.h>
#include <stdint.h>

#include "backend.h"
#include "font.h"
#include "text.h"

#define TEXT_GLYPH_WIDTH	8U
#define TEXT_GLYPH_HEIGHT	16U

/* Bounds the static cell array; 1920x1080 needs 240x67. */
#define TEXT_MAX_COLUMNS	240U
#define TEXT_MAX_ROWS		68U

/* One VGA text cell: character in the low byte, attribute in the high byte. */
static uint16_t text_cells[TEXT_MAX_ROWS * TEXT_MAX_COLUMNS];

static struct spinlock text_lock;
static volatile uint32_t *text_pixels;
static unsigned text_stride;
static unsigned text_origin_x;
static unsigned text_origin_y;
static unsigned text_columns;
static unsigned text_rows;
static unsigned text_cursor_row;
static unsigned text_cursor_column;
static int text_cursor_visible = 1;
static int text_rgbx;
static int text_ready;

/* The standard VGA palette the attribute byte indexes. */
static const uint32_t text_palette[16] = {
	0x000000U, 0x0000aaU, 0x00aa00U, 0x00aaaaU,
	0xaa0000U, 0xaa00aaU, 0xaa5500U, 0xaaaaaaU,
	0x555555U, 0x5555ffU, 0x55ff55U, 0x55ffffU,
	0xff5555U, 0xff55ffU, 0xffff55U, 0xffffffU
};

static uint32_t text_color(unsigned index);
static void draw_cell_locked(unsigned row, unsigned column, int cursor);
static void redraw_locked(void);
static void scroll_locked(void);
static void putc_locked(int character);

/*
 * Converts one palette index to the framebuffer pixel format.
 */
static uint32_t
text_color(
	unsigned index)
{
	uint32_t rgb;

	/* Selects the palette entry for this attribute nibble. */
	rgb = text_palette[index & 15U];

	/* Swaps the red and blue channels for an RGBX framebuffer. */
	if (text_rgbx) {
		return ((rgb & 0x00ff0000U) >> 16) | (rgb & 0x0000ff00U) |
		    ((rgb & 0x000000ffU) << 16);
	}

	/* Reports the native BGRX value. */
	return rgb;
}

/*
 * Draws one cell, optionally with the cursor inversion applied.
 */
static void
draw_cell_locked(
	unsigned row,
	unsigned column,
	int cursor)
{
	uint8_t glyph[32];
	unsigned glyph_width;
	unsigned glyph_height;
	unsigned first_x;
	unsigned first_y;
	unsigned line;
	unsigned dot;
	uint16_t cell;
	uint8_t attribute;
	uint32_t foreground;
	uint32_t background;
	uint32_t pixel;

	/* Rejects a cell outside the live geometry. */
	if (!text_ready || row >= text_rows || column >= text_columns)
		return;

	/* Decodes the stored character and attribute. */
	cell = text_cells[row * text_columns + column];
	attribute = (uint8_t)(cell >> 8);

	/* Inverts the cell colours while drawing the cursor. */
	if (cursor)
		attribute = (uint8_t)((attribute << 4) | (attribute >> 4));
	foreground = text_color(attribute & 15U);
	background = text_color((attribute >> 4) & 15U);

	/* Falls back to a blank cell when the font has no such glyph. */
	if (drv_pcat_font_get_glyph((uint8_t)cell, glyph, &glyph_width,
				    &glyph_height) != 0) {
		glyph_width = TEXT_GLYPH_WIDTH;
		glyph_height = TEXT_GLYPH_HEIGHT;
		for (line = 0; line < TEXT_GLYPH_HEIGHT; line++)
			glyph[line] = 0;
	}

	/* Clamps a font whose cell exceeds this layer's fixed geometry. */
	if (glyph_width > TEXT_GLYPH_WIDTH)
		glyph_width = TEXT_GLYPH_WIDTH;
	if (glyph_height > TEXT_GLYPH_HEIGHT)
		glyph_height = TEXT_GLYPH_HEIGHT;

	/* Computes the pixel origin of this cell. */
	first_x = text_origin_x + column * TEXT_GLYPH_WIDTH;
	first_y = text_origin_y + row * TEXT_GLYPH_HEIGHT;

	/* Paints every dot of the cell. */
	for (line = 0; line < TEXT_GLYPH_HEIGHT; line++) {
		for (dot = 0; dot < TEXT_GLYPH_WIDTH; dot++) {
			pixel = background;

			/* Selects the foreground for a set glyph dot. */
			if (line < glyph_height && dot < glyph_width &&
			    (glyph[line] & (0x80U >> dot)) != 0)
				pixel = foreground;
			text_pixels[(size_t)(first_y + line) * text_stride +
			    first_x + dot] = pixel;
		}
	}
}

/*
 * Redraws every cell of the screen.
 */
static void
redraw_locked(
	void)
{
	unsigned row;
	unsigned column;

	/* Paints the complete grid. */
	for (row = 0; row < text_rows; row++) {
		for (column = 0; column < text_columns; column++)
			draw_cell_locked(row, column, 0);
	}
}

/*
 * Moves every row up by one and clears the last row.
 */
static void
scroll_locked(
	void)
{
	unsigned row;
	unsigned column;
	uint16_t blank;

	/* Keeps the attribute of the current cursor cell for the new row. */
	blank = (uint16_t)((text_cells[text_cursor_row * text_columns] &
	    0xff00U) | (uint16_t)' ');

	/* Shifts the stored rows up by one. */
	for (row = 1; row < text_rows; row++) {
		for (column = 0; column < text_columns; column++) {
			text_cells[(row - 1U) * text_columns + column] =
			    text_cells[row * text_columns + column];
		}
	}

	/* Empties the last row. */
	for (column = 0; column < text_columns; column++)
		text_cells[(text_rows - 1U) * text_columns + column] = blank;

	/* Repaints the whole grid after the shift. */
	redraw_locked();
}

/*
 * Writes one character at the cursor, advancing and scrolling as needed.
 */
static void
putc_locked(
	int character)
{
	unsigned index;

	/* Ignores output before the framebuffer is published. */
	if (!text_ready)
		return;

	/* Handles the line and carriage controls. */
	if (character == '\n') {
		text_cursor_column = 0;
		text_cursor_row++;
	} else if (character == '\r') {
		text_cursor_column = 0;
	} else if (character == '\b') {
		/* Steps back one cell without erasing. */
		if (text_cursor_column > 0)
			text_cursor_column--;
	} else if (character == '\t') {
		/* Advances to the next eight-column stop. */
		text_cursor_column = (text_cursor_column + 8U) & ~7U;
	} else {
		/* Stores and paints one ordinary character. */
		index = text_cursor_row * text_columns + text_cursor_column;
		text_cells[index] = (uint16_t)((text_cells[index] & 0xff00U) |
		    (uint16_t)(character & 0xff));
		draw_cell_locked(text_cursor_row, text_cursor_column, 0);
		text_cursor_column++;
	}

	/* Wraps at the end of the row. */
	if (text_cursor_column >= text_columns) {
		text_cursor_column = 0;
		text_cursor_row++;
	}

	/* Scrolls when the cursor leaves the last row. */
	if (text_cursor_row >= text_rows) {
		text_cursor_row = text_rows - 1U;
		scroll_locked();
	}
}

/*
 * Establishes the text grid over the linear framebuffer.
 */
void
drv_pcat_text_init(
	void)
{
	unsigned width;
	unsigned height;
	unsigned index;
	unsigned long irq;

	/* Publishes the lock before any writer can reach this layer. */
	spin_init(&text_lock, LOCK_RANK_CONSOLE_TEXT, "pcat text console");

	/* Requires the linear framebuffer this layer draws into. */
	if (!drv_pcat_graphics_backend_get_framebuffer(&text_pixels, &width,
						       &height, &text_stride,
						       &text_rgbx))
		return;

	irq = spin_lock_irqsave(&text_lock);

	/* Derives the grid from the framebuffer and the fixed cell size. */
	text_columns = width / TEXT_GLYPH_WIDTH;
	text_rows = height / TEXT_GLYPH_HEIGHT;
	if (text_columns > TEXT_MAX_COLUMNS)
		text_columns = TEXT_MAX_COLUMNS;
	if (text_rows > TEXT_MAX_ROWS)
		text_rows = TEXT_MAX_ROWS;

	/* Centres the grid when the framebuffer is not an exact multiple. */
	text_origin_x = (width - text_columns * TEXT_GLYPH_WIDTH) / 2U;
	text_origin_y = (height - text_rows * TEXT_GLYPH_HEIGHT) / 2U;

	/* Starts from an empty screen with the default attribute. */
	for (index = 0; index < text_rows * text_columns; index++)
		text_cells[index] = (uint16_t)(0x0700U | (uint16_t)' ');
	text_cursor_row = 0;
	text_cursor_column = 0;
	text_cursor_visible = 1;
	text_ready = 1;
	redraw_locked();
	spin_unlock_irqrestore(&text_lock, irq);
}

/*
 * Reports whether the text layer can draw.
 */
int
drv_pcat_text_ready(
	void)
{
	/* Reports the published state. */
	return text_ready;
}

/*
 * Reports the text grid size in character cells.
 */
void
drv_pcat_text_get_size(
	unsigned *columns,
	unsigned *rows)
{
	/* Publishes the live geometry. */
	if (columns != NULL)
		*columns = text_columns;
	if (rows != NULL)
		*rows = text_rows;
}

/*
 * Writes one character at the cursor and advances it.
 */
void
drv_pcat_text_putc(
	int character)
{
	unsigned long irq;

	/* Serializes the write with every other text operation. */
	irq = spin_lock_irqsave(&text_lock);
	putc_locked(character);
	spin_unlock_irqrestore(&text_lock, irq);
}

/*
 * Writes a terminated string at a fixed cell with a fixed attribute.
 *
 * The string does not wrap and is clipped at the end of the row.
 */
void
drv_pcat_text_write(
	unsigned row,
	unsigned column,
	uint8_t attribute,
	const char *utf8)
{
	unsigned index;
	unsigned long irq;

	/* Ignores an absent string or a cell outside the grid. */
	if (utf8 == NULL)
		return;
	irq = spin_lock_irqsave(&text_lock);
	if (text_ready && row < text_rows) {
		/* Stores and paints each byte until the row ends. */
		for (index = column;
		     index < text_columns && *utf8 != '\0';
		     index++, utf8++) {
			text_cells[row * text_columns + index] =
			    (uint16_t)(((uint16_t)attribute << 8) |
			    (uint16_t)(*utf8 & 0xff));
			draw_cell_locked(row, index, 0);
		}
	}
	spin_unlock_irqrestore(&text_lock, irq);
}

/*
 * Clears the screen and homes the cursor.
 */
void
drv_pcat_text_clear(
	void)
{
	unsigned index;
	unsigned long irq;

	irq = spin_lock_irqsave(&text_lock);

	/* Empties every cell with the default attribute. */
	if (text_ready) {
		for (index = 0; index < text_rows * text_columns; index++) {
			text_cells[index] =
			    (uint16_t)(0x0700U | (uint16_t)' ');
		}
		text_cursor_row = 0;
		text_cursor_column = 0;
		redraw_locked();
	}
	spin_unlock_irqrestore(&text_lock, irq);
}

/*
 * Sets the cursor position.
 */
int
drv_pcat_text_set_cursor(
	unsigned row,
	unsigned column)
{
	unsigned long irq;
	int error;

	irq = spin_lock_irqsave(&text_lock);

	/* Rejects a position outside the live grid. */
	if (!text_ready || row >= text_rows || column >= text_columns) {
		error = -1;
	} else {
		/* Repaints the cell the cursor is leaving. */
		draw_cell_locked(text_cursor_row, text_cursor_column, 0);
		text_cursor_row = row;
		text_cursor_column = column;
		draw_cell_locked(row, column, text_cursor_visible);
		error = 0;
	}
	spin_unlock_irqrestore(&text_lock, irq);

	/* Reports the placement result. */
	return error;
}

/*
 * Reports the cursor position and whether it is visible.
 */
void
drv_pcat_text_get_cursor(
	unsigned *row,
	unsigned *column,
	int *visible)
{
	unsigned long irq;

	/* Captures a consistent cursor snapshot. */
	irq = spin_lock_irqsave(&text_lock);
	if (row != NULL)
		*row = text_cursor_row;
	if (column != NULL)
		*column = text_cursor_column;
	if (visible != NULL)
		*visible = text_cursor_visible;
	spin_unlock_irqrestore(&text_lock, irq);
}

/*
 * Shows or hides the cursor.
 */
void
drv_pcat_text_show_cursor(
	int visible)
{
	unsigned long irq;

	/* Repaints the cursor cell in its new state. */
	irq = spin_lock_irqsave(&text_lock);
	text_cursor_visible = visible != 0;
	draw_cell_locked(text_cursor_row, text_cursor_column,
			 text_cursor_visible);
	spin_unlock_irqrestore(&text_lock, irq);
}

/*
 * Repaints the cursor cell after a stream of writes.
 */
void
drv_pcat_text_update_cursor(
	void)
{
	unsigned long irq;

	/* Paints the cursor at its current position. */
	irq = spin_lock_irqsave(&text_lock);
	draw_cell_locked(text_cursor_row, text_cursor_column,
			 text_cursor_visible);
	spin_unlock_irqrestore(&text_lock, irq);
}

/*
 * Stops text rendering while a graphics mode owns the framebuffer.
 */
void
drv_pcat_text_suspend(
	void)
{
	unsigned long irq;

	/* Holds the cell array but stops painting. */
	irq = spin_lock_irqsave(&text_lock);
	text_ready = 0;
	spin_unlock_irqrestore(&text_lock, irq);
}

/*
 * Restores text rendering and repaints the retained screen.
 */
void
drv_pcat_text_resume(
	void)
{
	unsigned long irq;

	/* Resumes only when a framebuffer is still published. */
	irq = spin_lock_irqsave(&text_lock);
	if (text_pixels != NULL && text_columns != 0 && text_rows != 0) {
		text_ready = 1;
		redraw_locked();
		draw_cell_locked(text_cursor_row, text_cursor_column,
				 text_cursor_visible);
	}
	spin_unlock_irqrestore(&text_lock, irq);
}
