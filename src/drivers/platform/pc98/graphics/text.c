/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PC-98 text console layer owned by /dev/graphics.
 *
 * The hardware keeps characters and attributes in two planes, and a
 * character is a 16-bit code rather than a byte, so this layer holds its
 * own cell arrays and publishes both planes together. A JIS X 0208 glyph
 * occupies two cells; the second carries the same code with the high bit
 * set, which is how the hardware knows it is the right half.
 *
 * The cell arrays are the source of truth, so a graphics mode can take
 * the screen and give it back without losing what was on it.
 */

#include <kern/device-io.h>
#include <kern/lock.h>
#include <kern/text-display.h>
#include <stddef.h>
#include <stdint.h>

#include "text.h"

/* Characters are 16-bit words; their attributes live in a second plane. */
#define TEXT_CODE_VRAM		((volatile uint16_t *)0x800a0000)
#define TEXT_ATTRIBUTE_VRAM	((volatile uint8_t *)0x800a2000)

#define TEXT_COLUMNS		80U
#define TEXT_ROWS		25U
#define TEXT_CELLS		(TEXT_COLUMNS * TEXT_ROWS)

/* The right half of a two-cell glyph carries the code with bit 15 set. */
#define TEXT_WIDE_SECOND	0x8000U

/* The uPD7220 status and command ports, and its cursor-address command. */
#define GDC_STATUS		0x0060U
#define GDC_COMMAND		0x0062U
#define GDC_PARAMETER		0x0060U
#define GDC_CSRW		0x49U
#define GDC_CSRFORM		0x4bU

/* Cursor form: bit 7 shows it, the low five bits are the row
 * height minus one, which the sixteen-line font fixes at 15. */
#define GDC_CSRFORM_SHOW	0x8fU
#define GDC_CSRFORM_HIDE	0x0fU
#define GDC_CSRFORM_TOP		0x20U
#define GDC_CSRFORM_BOTTOM	0x7bU
#define GDC_FIFO_FULL		0x02U

/* The JIS X 0208 table the HAL owns, indexed in row-major order. */
extern const uint16_t hal_pc98_jisx0208_to_ucs[7896];

static uint16_t text_codes[TEXT_CELLS];
static uint8_t text_attributes[TEXT_CELLS];

static struct spinlock text_lock;
static unsigned text_cursor_row;
static unsigned text_cursor_column;
static int text_cursor_visible = 1;
static int text_ready;

static uint8_t pc98_attribute(uint8_t attribute);
static int gdc_write(uint16_t port, uint8_t value);
static void publish_cell_locked(unsigned row, unsigned column);
static void redraw_locked(void);
static void cursor_locked(void);
static void scroll_locked(void);
static void newline_locked(void);
static void putc_locked(int character);
static void put_code_locked(uint16_t code, unsigned width, uint8_t attribute);
static uint32_t utf8_codepoint(const char **input, const char *end);
static uint16_t unicode_to_pc98(uint32_t codepoint, unsigned *width);

/* The character output this board publishes to /dev/console. */
static const struct kern_text_ops pc98_text_ops = {
	.get_size = drv_pc98_text_get_size,
	.putc = drv_pc98_text_putc,
	.write = drv_pc98_text_write,
	.clear = drv_pc98_text_clear,
	.set_cursor = drv_pc98_text_set_cursor,
	.get_cursor = drv_pc98_text_get_cursor,
	.show_cursor = drv_pc98_text_show_cursor,
	.update_cursor = drv_pc98_text_update_cursor,
	.suspend = drv_pc98_text_suspend,
	.resume = drv_pc98_text_resume
};

/*
 * Establishes the text grid over PC-98 text memory.
 */
void
drv_pc98_text_init(
	void)
{
	unsigned long irq;
	unsigned index;

	/* Publishes the lock before any writer can reach this layer. */
	spin_init(&text_lock, LOCK_RANK_CONSOLE_TEXT, "pc98 text console");

	irq = spin_lock_irqsave(&text_lock);

	/* Starts from an empty screen with the default attribute. */
	for (index = 0; index < TEXT_CELLS; index++) {
		text_codes[index] = (uint16_t)' ';
		text_attributes[index] = KERN_TEXT_ATTRIB_NORMAL;
	}
	text_cursor_row = 0;
	text_cursor_column = 0;
	text_cursor_visible = 1;
	text_ready = 1;
	redraw_locked();
	cursor_locked();
	spin_unlock_irqrestore(&text_lock, irq);

	/* Publishes this board's character output to the kernel. */
	kern_text_register(&pc98_text_ops);
}

/*
 * Reports whether the text layer can draw.
 */
int
drv_pc98_text_ready(
	void)
{
	/* Reports the published state. */
	return text_ready;
}

/*
 * Reports the text grid size in character cells.
 */
void
drv_pc98_text_get_size(
	unsigned *columns,
	unsigned *rows)
{
	/* Publishes the fixed geometry. */
	if (columns != NULL)
		*columns = TEXT_COLUMNS;
	if (rows != NULL)
		*rows = TEXT_ROWS;
}

/*
 * Writes one character at the cursor.
 */
void
drv_pc98_text_putc(
	int character)
{
	unsigned long irq;

	/* Renders one byte under the layer lock. */
	irq = spin_lock_irqsave(&text_lock);
	putc_locked(character);
	spin_unlock_irqrestore(&text_lock, irq);
}

/*
 * Writes a terminated string at one cell with one attribute.
 */
void
drv_pc98_text_write(
	unsigned row,
	unsigned column,
	uint8_t attribute,
	const char *utf8)
{
	unsigned long irq;
	const char *end;
	uint32_t codepoint;
	unsigned width;
	uint16_t code;

	/* Ignores a missing string or a start outside the grid. */
	if (utf8 == NULL || row >= TEXT_ROWS || column >= TEXT_COLUMNS)
		return;

	/* Measures the terminated input string. */
	end = utf8;
	while (*end != '\0')
		end++;

	irq = spin_lock_irqsave(&text_lock);

	/* Places the run at the requested cell. */
	text_cursor_row = row;
	text_cursor_column = column;

	/* Emits every complete or substituted code point. */
	while (utf8 < end) {
		codepoint = utf8_codepoint(&utf8, end);

		/* Stops at the end of the row rather than wrapping. */
		code = unicode_to_pc98(codepoint, &width);
		if (text_cursor_column + width > TEXT_COLUMNS)
			break;
		put_code_locked(code, width, attribute);
	}
	spin_unlock_irqrestore(&text_lock, irq);
}

/*
 * Blanks the grid and homes the cursor.
 */
void
drv_pc98_text_clear(
	void)
{
	unsigned long irq;
	unsigned index;

	/* Clears both planes and returns the cursor home. */
	irq = spin_lock_irqsave(&text_lock);
	for (index = 0; index < TEXT_CELLS; index++) {
		text_codes[index] = (uint16_t)' ';
		text_attributes[index] = KERN_TEXT_ATTRIB_NORMAL;
	}
	text_cursor_row = 0;
	text_cursor_column = 0;
	redraw_locked();
	cursor_locked();
	spin_unlock_irqrestore(&text_lock, irq);
}

/*
 * Moves the cursor.
 */
int
drv_pc98_text_set_cursor(
	unsigned row,
	unsigned column)
{
	unsigned long irq;

	/* Rejects a position outside the grid. */
	if (row >= TEXT_ROWS || column >= TEXT_COLUMNS)
		return 0;

	/* Publishes the new position to the hardware. */
	irq = spin_lock_irqsave(&text_lock);
	text_cursor_row = row;
	text_cursor_column = column;
	cursor_locked();
	spin_unlock_irqrestore(&text_lock, irq);

	/* Reports the accepted position. */
	return 1;
}

/*
 * Reports the cursor position and whether it is shown.
 */
void
drv_pc98_text_get_cursor(
	unsigned *row,
	unsigned *column,
	int *visible)
{
	unsigned long irq;

	/* Samples the tracked cursor state. */
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
drv_pc98_text_show_cursor(
	int visible)
{
	unsigned long irq;

	/* Publishes the new cursor form. */
	irq = spin_lock_irqsave(&text_lock);
	text_cursor_visible = visible != 0;
	cursor_locked();
	spin_unlock_irqrestore(&text_lock, irq);
}

/*
 * Repaints the cursor after a stream of writes.
 */
void
drv_pc98_text_update_cursor(
	void)
{
	unsigned long irq;

	/* Moves the hardware cursor to the tracked position. */
	irq = spin_lock_irqsave(&text_lock);
	cursor_locked();
	spin_unlock_irqrestore(&text_lock, irq);
}

/*
 * Stops text rendering while a graphics mode owns the screen.
 */
void
drv_pc98_text_suspend(
	void)
{
	unsigned long irq;

	/* Hides the hardware cursor before the screen changes hands. */
	irq = spin_lock_irqsave(&text_lock);
	if (text_ready) {
		text_cursor_visible = 0;
		cursor_locked();
		text_cursor_visible = 1;
	}

	/* Holds the cell arrays but stops painting. */
	text_ready = 0;
	spin_unlock_irqrestore(&text_lock, irq);
}

/*
 * Restores text rendering and repaints the retained screen.
 */
void
drv_pc98_text_resume(
	void)
{
	unsigned long irq;

	/* Repaints both planes from the retained cells. */
	irq = spin_lock_irqsave(&text_lock);
	text_ready = 1;
	redraw_locked();
	cursor_locked();
	spin_unlock_irqrestore(&text_lock, irq);
}

/*
 * Translates one kernel attribute byte into the PC-98 layout.
 *
 * The kernel's byte is the VGA one: the low nibble selects a foreground
 * from blue, green and red bits plus an intensity bit, and the high
 * nibble does the same for the background. The PC-98 has one set of
 * colour bits and a reverse bit instead of a background, so a non-black
 * background becomes reverse video.
 */
static uint8_t
pc98_attribute(
	uint8_t attribute)
{
	uint8_t value;

	/* Maps the foreground colour bits onto the three guns. */
	value = 0;
	if ((attribute & 0x01U) != 0)
		value |= 0x20U;
	if ((attribute & 0x02U) != 0)
		value |= 0x80U;
	if ((attribute & 0x04U) != 0)
		value |= 0x40U;

	/* Keeps a black foreground legible rather than invisible. */
	if ((attribute & 0x07U) == 0)
		value |= 0xe0U;

	/* Turns any non-black background into reverse video. */
	if ((attribute & 0x70U) != 0)
		value |= 0x04U;

	/* Marks the cell displayed; without this bit it is not drawn. */
	return (uint8_t)(value | 0x01U);
}

/* Issues one byte to the uPD7220 once its command FIFO has room. */
static int
gdc_write(
	uint16_t port,
	uint8_t value)
{
	uint8_t status;
	unsigned timeout;

	/* Polls the FIFO through a bounded legacy timeout. */
	for (timeout = 100000U; timeout != 0U; timeout--) {
		/* Stops as soon as the FIFO accepts another byte. */
		status = kern_io_in8(GDC_STATUS);
		if ((status & GDC_FIFO_FULL) == 0U)
			break;
	}

	/* Reports failure without issuing a byte after timeout. */
	if (timeout == 0U)
		return 0;
	kern_io_out8(port, value);

	/* Reports one completed port write. */
	return 1;
}

/* Publishes one stored cell to both hardware planes. */
static void
publish_cell_locked(
	unsigned row,
	unsigned column)
{
	unsigned offset;

	/* Suppresses video-memory writes during graphics ownership. */
	if (!text_ready || row >= TEXT_ROWS || column >= TEXT_COLUMNS)
		return;

	/* Publishes the character before its corresponding attribute. */
	offset = row * TEXT_COLUMNS + column;
	TEXT_CODE_VRAM[offset] = text_codes[offset];
	TEXT_ATTRIBUTE_VRAM[offset * 2U] =
	    pc98_attribute(text_attributes[offset]);
}

/* Repaints every cell of the screen. */
static void
redraw_locked(
	void)
{
	unsigned row;
	unsigned column;

	/* Paints the complete grid. */
	for (row = 0; row < TEXT_ROWS; row++) {
		for (column = 0; column < TEXT_COLUMNS; column++)
			publish_cell_locked(row, column);
	}
}

/* Moves the hardware cursor, or hides it. */
static void
cursor_locked(
	void)
{
	unsigned address;

	/* Leaves the hardware alone while another driver owns the screen. */
	if (!text_ready)
		return;

	/*
	 * Shows or hides the cursor. The low five bits of the first
	 * parameter are the scan lines per character row minus one, which
	 * is fixed by the font at sixteen; changing it would resize every
	 * row of text on the screen.
	 */
	if (!gdc_write(GDC_COMMAND, GDC_CSRFORM))
		return;
	if (!gdc_write(GDC_PARAMETER,
		       text_cursor_visible ? GDC_CSRFORM_SHOW
					   : GDC_CSRFORM_HIDE))
		return;
	if (!gdc_write(GDC_PARAMETER, GDC_CSRFORM_TOP))
		return;
	if (!gdc_write(GDC_PARAMETER, GDC_CSRFORM_BOTTOM))
		return;

	/* A hidden cursor needs no address. */
	if (!text_cursor_visible)
		return;

	/* Writes the cell address as the two-parameter cursor command. */
	address = text_cursor_row * TEXT_COLUMNS + text_cursor_column;
	if (!gdc_write(GDC_COMMAND, GDC_CSRW))
		return;
	if (!gdc_write(GDC_PARAMETER, (uint8_t)(address & 0xffU)))
		return;
	(void)gdc_write(GDC_PARAMETER, (uint8_t)((address >> 8) & 0xffU));
}

/* Moves every row up by one and clears the last row. */
static void
scroll_locked(
	void)
{
	unsigned index;

	/* Shifts both stored planes up by one row. */
	for (index = 0; index < TEXT_CELLS - TEXT_COLUMNS; index++) {
		text_codes[index] = text_codes[index + TEXT_COLUMNS];
		text_attributes[index] = text_attributes[index + TEXT_COLUMNS];
	}

	/* Empties the last row. */
	for (index = TEXT_CELLS - TEXT_COLUMNS; index < TEXT_CELLS; index++) {
		text_codes[index] = (uint16_t)' ';
		text_attributes[index] = KERN_TEXT_ATTRIB_NORMAL;
	}

	/* Repaints the whole grid after the shift. */
	redraw_locked();
}

/* Advances the cursor to the start of the next line. */
static void
newline_locked(
	void)
{
	/* Returns to the first column of the following row. */
	text_cursor_column = 0;
	text_cursor_row++;

	/* Scrolls instead of leaving the last row. */
	if (text_cursor_row >= TEXT_ROWS) {
		scroll_locked();
		text_cursor_row = TEXT_ROWS - 1U;
	}
}

/* Stores one converted glyph and advances the cursor across its cells. */
static void
put_code_locked(
	uint16_t code,
	unsigned width,
	uint8_t attribute)
{
	unsigned offset;

	/* Wraps before a glyph that would not fit on this row. */
	if (text_cursor_column + width > TEXT_COLUMNS)
		newline_locked();

	/* Stores and publishes the first cell. */
	offset = text_cursor_row * TEXT_COLUMNS + text_cursor_column;
	text_codes[offset] = code;
	text_attributes[offset] = attribute;
	publish_cell_locked(text_cursor_row, text_cursor_column);
	text_cursor_column++;

	/* Stores the marked right half of a double-cell glyph. */
	if (width == 2U) {
		offset++;
		text_codes[offset] = (uint16_t)(code | TEXT_WIDE_SECOND);
		text_attributes[offset] = attribute;
		publish_cell_locked(text_cursor_row, text_cursor_column);
		text_cursor_column++;
	}

	/* Wraps to the next line at the end of the row. */
	if (text_cursor_column >= TEXT_COLUMNS)
		newline_locked();
}

/* Renders one byte at the cursor, handling the control characters. */
static void
putc_locked(
	int character)
{
	unsigned offset;

	/* Handles the line and column control characters. */
	if (character == '\n') {
		newline_locked();
		cursor_locked();
		return;
	}
	if (character == '\r') {
		text_cursor_column = 0;
		cursor_locked();
		return;
	}
	if (character == '\b') {
		/* Steps back and erases within the current row only. */
		if (text_cursor_column > 0) {
			text_cursor_column--;
			offset = text_cursor_row * TEXT_COLUMNS +
			    text_cursor_column;
			text_codes[offset] = (uint16_t)' ';
			text_attributes[offset] = KERN_TEXT_ATTRIB_NORMAL;
			publish_cell_locked(text_cursor_row,
					    text_cursor_column);
		}
		cursor_locked();
		return;
	}
	if (character == '\t') {
		/* Advances to the next eight-column stop. */
		text_cursor_column = (text_cursor_column + 8U) & ~7U;

		/* Wraps when the stop leaves the row. */
		if (text_cursor_column >= TEXT_COLUMNS)
			newline_locked();
		cursor_locked();
		return;
	}

	/* Stores one ordinary cell. */
	put_code_locked((uint16_t)(uint8_t)character, 1U,
			KERN_TEXT_ATTRIB_NORMAL);
	cursor_locked();
}

/*
 * Decodes one UTF-8 sequence, substituting a question mark for anything
 * malformed, overlong, surrogate or out of range.
 */
static uint32_t
utf8_codepoint(
	const char **input,
	const char *end)
{
	const uint8_t *p;
	uint32_t codepoint;
	unsigned count;
	unsigned index;

	/* Loads the current input byte position. */
	p = (const uint8_t *)*input;

	/* Reports the sentinel code point at the range end. */
	if ((const char *)p >= end)
		return 0;

	/* Consumes and returns one ASCII byte directly. */
	if (p[0] < 0x80U) {
		*input = (const char *)(p + 1);
		return p[0];
	}

	/* Determines the encoded width and initial payload bits. */
	if ((p[0] & 0xe0U) == 0xc0U) {
		codepoint = p[0] & 0x1fU;
		count = 2;

		/* Rejects the overlong two-byte lead-byte range. */
		if (codepoint < 2U)
			count = 0;
	} else if ((p[0] & 0xf0U) == 0xe0U) {
		codepoint = p[0] & 0x0fU;
		count = 3;
	} else if ((p[0] & 0xf8U) == 0xf0U) {
		codepoint = p[0] & 0x07U;
		count = 4;

		/* Rejects lead bytes above the Unicode scalar range. */
		if (codepoint > 4U)
			count = 0;
	} else {
		codepoint = 0;
		count = 0;
	}

	/* Consumes one invalid lead byte for a short or impossible run. */
	if (count == 0U || end - (const char *)p < (int)count) {
		*input = (const char *)(p + 1);
		return '?';
	}

	/* Accumulates every validated continuation-byte payload. */
	for (index = 1U; index < count; index++) {
		/* Substitutes the lead byte on a malformed continuation. */
		if ((p[index] & 0xc0U) != 0x80U) {
			*input = (const char *)(p + 1);
			return '?';
		}
		codepoint = (codepoint << 6) | (p[index] & 0x3fU);
	}

	/* Publishes the position after the complete byte sequence. */
	*input = (const char *)(p + count);

	/* Substitutes overlong, surrogate, and out-of-range scalars. */
	if ((count == 2U && codepoint < 0x80U) ||
	    (count == 3U && codepoint < 0x800U) ||
	    (count == 4U && codepoint < 0x10000U) ||
	    codepoint > 0x10ffffU ||
	    (codepoint >= 0xd800U && codepoint <= 0xdfffU)) {
		return '?';
	}

	/* Returns the validated Unicode scalar. */
	return codepoint;
}

/* Maps one Unicode scalar to a PC-98 text code and its cell width. */
static uint16_t
unicode_to_pc98(
	uint32_t codepoint,
	unsigned *width)
{
	unsigned index;
	uint16_t ku;
	uint16_t ten;

	/* Maps ASCII directly to one text cell. */
	if (codepoint < 0x80U) {
		*width = 1;
		return (uint16_t)codepoint;
	}

	/* Maps half-width katakana directly to one text cell. */
	if (codepoint >= 0xff61U && codepoint <= 0xff9fU) {
		*width = 1;
		return (uint16_t)(0xa1U + codepoint - 0xff61U);
	}

	/* Searches BMP scalars in the HAL-owned JIS X 0208 table. */
	if (codepoint <= 0xffffU) {
		/* Visits each retained table entry in row-major order. */
		for (index = 0; index < 7896U; index++) {
			/* Skips entries for a different scalar. */
			if (hal_pc98_jisx0208_to_ucs[index] != codepoint)
				continue;

			/* Encodes the JIS row and cell for PC-98 memory. */
			ku = (uint16_t)(0x21U + index / 94U);
			ten = (uint16_t)(0x21U + index % 94U);
			*width = 2;

			/* Returns the PC-98 byte ordering for this glyph. */
			return (uint16_t)((ten << 8) | (ku - 0x20U));
		}
	}

	/* Substitutes an unmapped scalar with one question-mark cell. */
	*width = 1;
	return '?';
}
