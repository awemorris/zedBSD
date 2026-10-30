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

#include <uapi/errno.h>
#include <kern/device-io.h>
#include <kern/text-display.h>
#include <kern/klog.h>
#include <kern/lock.h>
#include <kern/pmem.h>
#include <kern/clock.h>
#include <kern/sched.h>
#include <kern/thread.h>
#include <stddef.h>
#include <stdint.h>

#include "backend.h"
#include "font.h"
#include "splash.h"
#include "text.h"
#include "../serial-mirror.h"

#ifdef KERN_TEST_TEXT_END_SECONDS
#include <kern/platform.h>
#endif

#define TEXT_GLYPH_WIDTH	8U
#define TEXT_GLYPH_HEIGHT	16U

/*
 * How often the splash's spinner is looked at while no line comes, and how
 * many times at most (two minutes; milliseconds).  The spinner keeps its
 * own time and draws only when a step has passed, so a look much shorter
 * than its step only keeps the turns even.
 */
#define TEXT_SPLASH_TICK_MS	25U
#define TEXT_SPLASH_TICKS_MAX	4800U

/* Bounds the static cell array; 1920x1080 needs 240x67. */
#define TEXT_MAX_COLUMNS	240U
#define TEXT_MAX_ROWS		68U

/* How many times a fatal stop tries text_lock before it draws without it. */
#define TEXT_FATAL_LOCK_SPINS	50000000U

/* The legacy aperture, and text memory's offset and geometry inside it. */
#define TEXT_VGA_APERTURE	0x000a0000U
#define TEXT_VGA_APERTURE_SIZE	0x00020000U
#define TEXT_VGA_OFFSET		0x00018000U
#define TEXT_VGA_COLUMNS	80U
#define TEXT_VGA_ROWS		25U
#define TEXT_VGA_CRTC_INDEX	0x03d4U
#define TEXT_VGA_CRTC_DATA	0x03d5U
#define TEXT_VGA_CURSOR_HIGH	0x0eU
#define TEXT_VGA_CURSOR_LOW	0x0fU
#define TEXT_VGA_CURSOR_START	0x0aU
#define TEXT_VGA_CURSOR_END	0x0bU
#define TEXT_VGA_CURSOR_OFF	0x20U

/* Where the characters actually land. */
enum text_surface {
	TEXT_SURFACE_NONE,
	TEXT_SURFACE_FRAMEBUFFER,
	TEXT_SURFACE_VGA_TEXT
};

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
static enum text_surface text_surface;
static volatile uint16_t *text_vram;

/*
 * A quiet console (the boot parameter kmsg=quiet): the cells
 * are kept but not drawn, so the boot logo stays on the screen, until the
 * console is revealed (a reader on it, a diagnostic that bypasses the log).
 * The framebuffer's height is kept to clear the logo then.  Protected by
 * text_lock.
 */
static int text_hidden;
static unsigned text_framebuffer_height;

/*
 * Whether the splash's spinner has ended for good (a display was taken
 * for graphics): it is not drawn again, by a log record or by its ticker.
 * Protected by text_lock.
 */
static int text_splash_ended;

/*
 * Whether the console has left the display for good (a GPU flipped its
 * first image there): nothing is drawn on the surface again, by a write,
 * a resume, a reveal or a fatal stop, while the cells keep following the
 * characters.  Set once under text_lock and read atomically, also by a
 * fatal stop that draws without the lock.
 */
static int text_ended;

#ifdef KERN_TEST_TEXT_END_SECONDS
/*
 * The console's own end, KERN_TEST_TEXT_END_SECONDS after the layer is up:
 * what kern_text_end() returned (text_end_test_result), and 1 once it has
 * returned (text_end_test_done).  Written once, by the thread that asks.
 * Nothing in the kernel reads them; they are volatile so that both stores
 * and both symbols stay in the image for a debugger.
 */
static volatile int text_end_test_result;
static volatile int text_end_test_done;

/* Whether the thread that ends the console was started; written by the layer's set-up only. */
static int text_end_test_started;
#endif

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
static int vga_text_attach_locked(void);
static void vga_cursor_locked(void);
static void text_splash_ticker(void *argument);
static void text_splash_ticker_start(void);
static int cells_live_locked(void);
#ifdef KERN_TEST_TEXT_END_SECONDS
static void text_end_test_start(void);
static void text_end_test(void *argument);
#endif

/*
 * Renders retained text cells into an independent RAM image without reading display memory.
 *
 * The existing cell and glyph renderer is serialized while only its destination changes.
 * Cursor, cell contents, live surface and output readiness are restored before unlocking.
 */
int
drv_pcat_text_snapshot(
	struct kern_text_snapshot *snapshot)
{
	volatile uint32_t *previous_pixels;
	enum text_surface previous_surface;
	unsigned previous_stride;
	unsigned previous_origin_x;
	unsigned previous_origin_y;
	unsigned long irq;
	size_t required;
	size_t index;
	int previous_rgbx;
	int previous_ready;
	int previous_hidden;

	/* Rejects an absent request before acquiring the renderer's state lock. */
	if (snapshot == NULL)
		return EINVAL;

	/* Geometry and retained cells belong to the same locked text-grid snapshot. */
	irq = spin_lock_irqsave(&text_lock);

	/* A machine with no attached grid cannot provide useful snapshot geometry. */
	if (text_surface == TEXT_SURFACE_NONE ||
	    text_columns == 0U ||
	    text_rows == 0U) {
		spin_unlock_irqrestore(&text_lock, irq);
		return ENODEV;
	}

	/* The fixed cell-array limits bound these products below eight megabytes. */
	snapshot->width = text_columns * TEXT_GLYPH_WIDTH;
	snapshot->height = text_rows * TEXT_GLYPH_HEIGHT;
	snapshot->stride = snapshot->width * sizeof(uint32_t);
	required = (size_t)snapshot->stride * snapshot->height;

	/* Geometry queries never touch a pixel destination. */
	if (snapshot->pixels == NULL) {
		spin_unlock_irqrestore(&text_lock, irq);
		if (snapshot->bytes != 0U)
			return EINVAL;
		return 0;
	}

	/* Refuses partial destinations before painting any cell or cursor pixel. */
	if (snapshot->bytes < required) {
		spin_unlock_irqrestore(&text_lock, irq);
		return ENOSPC;
	}

	/*
	 * A quiet console shows none of its text: its image is
	 * black, so a display given back to the console between two graphical
	 * owners (the greeter and the session) stays dark until it is revealed.
	 */
	if (text_hidden) {
		for (index = 0; index < required / sizeof(uint32_t); index++)
			snapshot->pixels[index] = 0U;
		spin_unlock_irqrestore(&text_lock, irq);
		return 0;
	}

	/* Saves the live destination while preserving all authoritative character and cursor state. */
	previous_pixels = text_pixels;
	previous_surface = text_surface;
	previous_stride = text_stride;
	previous_origin_x = text_origin_x;
	previous_origin_y = text_origin_y;
	previous_rgbx = text_rgbx;
	previous_ready = text_ready;
	previous_hidden = text_hidden;

	/* Redirects the already initialized glyph renderer to tightly packed BGRA8888 RAM. */
	text_pixels = snapshot->pixels;
	text_surface = TEXT_SURFACE_FRAMEBUFFER;
	text_stride = snapshot->width;
	text_origin_x = 0U;
	text_origin_y = 0U;
	text_rgbx = 0;
	text_ready = 1;
	text_hidden = 0;
	redraw_locked();
	draw_cell_locked(text_cursor_row, text_cursor_column, text_cursor_visible);

	/* Restores every temporary target field before other console writers can enter. */
	text_pixels = previous_pixels;
	text_surface = previous_surface;
	text_stride = previous_stride;
	text_origin_x = previous_origin_x;
	text_origin_y = previous_origin_y;
	text_rgbx = previous_rgbx;
	text_ready = previous_ready;
	text_hidden = previous_hidden;

	spin_unlock_irqrestore(&text_lock, irq);

	/* Succeeded: the independent snapshot contains all retained cells and the current cursor. */
	return 0;
}

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

	/* A quiet console keeps its cells off the screen. */
	if (text_hidden)
		return;

	/* Decodes the stored character and attribute. */
	cell = text_cells[row * text_columns + column];
	attribute = (uint8_t)(cell >> 8);

	/*
	 * Text memory takes the cell word as it stands. The cursor is the
	 * CRTC's own, so this surface never inverts a cell to show it.
	 */
	if (text_surface == TEXT_SURFACE_VGA_TEXT) {
		text_vram[row * text_columns + column] = cell;
		return;
	}

	/* Inverts the cell colours while drawing the cursor. */
	if (cursor)
		attribute = (uint8_t)((attribute << 4) | (attribute >> 4));
	foreground = text_color(attribute & 15U);
	background = text_color((attribute >> 4) & 15U);

	/*
	 * Falls back to a blank cell when the font has no such glyph. The
	 * font reports success as 1.
	 */
	if (!drv_pcat_font_get_glyph((uint8_t)cell, glyph, &glyph_width,
				     &glyph_height)) {
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

	/*
	 * The mirror runs before the readiness test: a host reading the serial
	 * line should see the same stream whether or not a framebuffer was
	 * published, and it does not touch the cell array.
	 */
	drv_pcat_serial_mirror(character);

	/* Ignores output before the framebuffer is published (an ended console keeps its cells). */
	if (!cells_live_locked())
		return;

	/* A line nobody sees on a quiet boot turns the splash's spinner. */
	if (text_hidden && character == '\n')
		drv_pcat_splash_step();

	/*
	 * A control character moves the cursor without redrawing its cell,
	 * so the inverted cursor image is erased here first.
	 */
	if (character == '\n' || character == '\r' ||
	    character == '\b' || character == '\t')
		draw_cell_locked(text_cursor_row, text_cursor_column, 0);

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

/* The character output this board publishes to /dev/console. */
static const struct kern_text_ops pcat_text_ops = {
	.get_size = drv_pcat_text_get_size,
	.putc = drv_pcat_text_putc,
	.write = drv_pcat_text_write,
	.clear = drv_pcat_text_clear,
	.set_cursor = drv_pcat_text_set_cursor,
	.get_cursor = drv_pcat_text_get_cursor,
	.show_cursor = drv_pcat_text_show_cursor,
	.update_cursor = drv_pcat_text_update_cursor,
	.suspend = drv_pcat_text_suspend,
	.resume = drv_pcat_text_resume,
	.snapshot = drv_pcat_text_snapshot,
	.reveal = drv_pcat_text_reveal,
	.reveal_fatal = drv_pcat_text_reveal_fatal,
	.progress = drv_pcat_text_progress,
	.end = drv_pcat_text_end
};

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

	/*
	 * Prefers the linear framebuffer, where characters are drawn as
	 * pixels and can share the screen with graphics. Without one the
	 * machine is old enough that text memory is the only surface.
	 */
	if (drv_pcat_graphics_backend_get_framebuffer(&text_pixels, &width,
						      &height, &text_stride,
						      &text_rgbx)) {
		irq = spin_lock_irqsave(&text_lock);
		text_surface = TEXT_SURFACE_FRAMEBUFFER;

		/* Derives the grid from the framebuffer and cell size. */
		text_framebuffer_height = height;
		text_columns = width / TEXT_GLYPH_WIDTH;
		text_rows = height / TEXT_GLYPH_HEIGHT;
		if (text_columns > TEXT_MAX_COLUMNS)
			text_columns = TEXT_MAX_COLUMNS;
		if (text_rows > TEXT_MAX_ROWS)
			text_rows = TEXT_MAX_ROWS;

		/* Centres a grid that does not fill the framebuffer. */
		text_origin_x =
		    (width - text_columns * TEXT_GLYPH_WIDTH) / 2U;
		text_origin_y =
		    (height - text_rows * TEXT_GLYPH_HEIGHT) / 2U;
	} else {
		irq = spin_lock_irqsave(&text_lock);

		/* Gives up when text memory cannot be reached either. */
		if (!vga_text_attach_locked()) {
			spin_unlock_irqrestore(&text_lock, irq);
			return;
		}
	}

	/* Starts from an empty screen with the default attribute. */
	for (index = 0; index < text_rows * text_columns; index++)
		text_cells[index] = (uint16_t)(0x0700U | (uint16_t)' ');
	text_cursor_row = 0;
	text_cursor_column = 0;
	text_cursor_visible = 1;
	text_ready = 1;

	/*
	 * A quiet boot leaves the screen (the boot logo) as it is, and the
	 * splash's spinner is drawn through this layer's mapping from now on;
	 * otherwise the empty grid is drawn.
	 */
	text_hidden = kern_log_quiet();
	if (text_hidden && text_surface == TEXT_SURFACE_FRAMEBUFFER)
		drv_pcat_splash_retarget(text_pixels);
	redraw_locked();
	vga_cursor_locked();
	spin_unlock_irqrestore(&text_lock, irq);

	/* Publishes this board's character output to the kernel. */
	kern_text_register(&pcat_text_ops);

	/* On a quiet boot the splash's spinner also turns by itself, so a long wait does not look stopped. */
	if (text_hidden)
		text_splash_ticker_start();

#ifdef KERN_TEST_TEXT_END_SECONDS
	/* The console ends itself after the configured wait. */
	text_end_test_start();
#endif
}

/*
 * Attaches the VGA text-memory surface.
 *
 * Returns 0 when the aperture cannot be mapped, which leaves the layer
 * unavailable and the early console in charge of the screen.
 */
static int
vga_text_attach_locked(
	void)
{
	void *aperture;

	/* Maps the uncached legacy aperture that holds text memory. */
	if (kern_device_map(TEXT_VGA_APERTURE, TEXT_VGA_APERTURE_SIZE,
			    KERN_DEVICE_UNCACHED, &aperture) != 0)
		return 0;

	/* Publishes the text-memory window and its fixed geometry. */
	text_vram = (volatile uint16_t *)((volatile uint8_t *)aperture +
	    TEXT_VGA_OFFSET);
	text_surface = TEXT_SURFACE_VGA_TEXT;
	text_columns = TEXT_VGA_COLUMNS;
	text_rows = TEXT_VGA_ROWS;
	text_origin_x = 0;
	text_origin_y = 0;

	/* Reports an attached surface. */
	return 1;
}

/*
 * Moves the CRTC cursor to the tracked cell, or hides it.
 *
 * This does nothing on the framebuffer surface, where the cursor is a
 * drawn inversion of the cell rather than a hardware feature.
 */
static void
vga_cursor_locked(
	void)
{
	unsigned offset;

	/* Leaves the hardware alone unless text memory is the surface (and shown). */
	if (text_surface != TEXT_SURFACE_VGA_TEXT || !text_ready || text_hidden)
		return;

	/* Disables the cursor scan lines while it is hidden. */
	if (!text_cursor_visible) {
		kern_io_out8(TEXT_VGA_CRTC_INDEX, TEXT_VGA_CURSOR_START);
		kern_io_out8(TEXT_VGA_CRTC_DATA, TEXT_VGA_CURSOR_OFF);
		return;
	}

	/* Restores an underline cursor on the last two scan lines. */
	kern_io_out8(TEXT_VGA_CRTC_INDEX, TEXT_VGA_CURSOR_START);
	kern_io_out8(TEXT_VGA_CRTC_DATA, 14U);
	kern_io_out8(TEXT_VGA_CRTC_INDEX, TEXT_VGA_CURSOR_END);
	kern_io_out8(TEXT_VGA_CRTC_DATA, 15U);

	/* Writes the linear cell offset to the cursor registers. */
	offset = text_cursor_row * text_columns + text_cursor_column;
	kern_io_out8(TEXT_VGA_CRTC_INDEX, TEXT_VGA_CURSOR_HIGH);
	kern_io_out8(TEXT_VGA_CRTC_DATA, (uint8_t)(offset >> 8));
	kern_io_out8(TEXT_VGA_CRTC_INDEX, TEXT_VGA_CURSOR_LOW);
	kern_io_out8(TEXT_VGA_CRTC_DATA, (uint8_t)(offset & 0xffU));
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
	if (cells_live_locked() && row < text_rows) {
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
	if (cells_live_locked()) {
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
	if (!cells_live_locked() || row >= text_rows || column >= text_columns) {
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
	vga_cursor_locked();
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

	/* Hides the hardware cursor before the screen changes hands. */
	irq = spin_lock_irqsave(&text_lock);
	if (text_surface == TEXT_SURFACE_VGA_TEXT && text_ready) {
		kern_io_out8(TEXT_VGA_CRTC_INDEX, TEXT_VGA_CURSOR_START);
		kern_io_out8(TEXT_VGA_CRTC_DATA, TEXT_VGA_CURSOR_OFF);
	}

	/* Holds the cell array but stops painting. */
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

	/* Resumes only when a surface is still attached and the console has not left it. */
	irq = spin_lock_irqsave(&text_lock);
	if (!text_ended && text_surface != TEXT_SURFACE_NONE && text_columns != 0 &&
	    text_rows != 0) {
		text_ready = 1;
		redraw_locked();
		draw_cell_locked(text_cursor_row, text_cursor_column,
				 text_cursor_visible);
		vga_cursor_locked();
	}
	spin_unlock_irqrestore(&text_lock, irq);
}

/*
 * Turns the splash's spinner while the console is kept off the screen: a
 * log record went nowhere else; or stops it for good, a
 * display having been taken for graphics.
 */
void
drv_pcat_text_progress(
	int end)
{
	unsigned long irq;

	/* Serializes the spinner with the other console writers. */
	irq = spin_lock_irqsave(&text_lock);

	/* The end: nothing more is drawn over what was the splash. */
	if (end) {
		text_splash_ended = 1;
		drv_pcat_splash_stop();
	}

	/* The next frame, only over the splash (a console that is shown has none). */
	if (text_hidden && !text_splash_ended)
		drv_pcat_splash_step();

	/* Lets the other console writers draw again. */
	spin_unlock_irqrestore(&text_lock, irq);

	/* Succeeded: the spinner shows the step. */
	return;
}

/*
 * Shows a quiet console: the screen is cleared (the boot logo goes) and the
 * retained cells and the cursor are drawn.
 */
void
drv_pcat_text_reveal(
	void)
{
	volatile uint32_t *pixels;
	unsigned long irq;
	size_t count;
	size_t index;

	/* Clears the logo and draws the retained text, once. */
	irq = spin_lock_irqsave(&text_lock);

	/* A console already shown is left as it is. */
	if (text_hidden) {
		text_hidden = 0;

		/* The splash goes with the clear, and its spinner stops. */
		drv_pcat_splash_stop();

		/* The whole framebuffer black, then the text, while the surface is drawn to. */
		if (text_ready && text_surface == TEXT_SURFACE_FRAMEBUFFER && text_pixels != NULL) {
			pixels = text_pixels;
			count = (size_t)text_stride * text_framebuffer_height;
			for (index = 0; index < count; index++)
				pixels[index] = 0U;
		}

		/* The cells, the cursor, and the hardware cursor of text memory. */
		redraw_locked();
		draw_cell_locked(text_cursor_row, text_cursor_column, text_cursor_visible);
		vga_cursor_locked();
	}

	/* Lets the other console writers draw again. */
	spin_unlock_irqrestore(&text_lock, irq);
}

/*
 * Shows the console for a fatal stop.
 *
 * A graphics mode suspended the text (text_ready 0) and draws over the same
 * framebuffer, so the ordinary reveal draws nothing there and a panic looked
 * like a freeze.  Here the retained cells are drawn back whatever owned the
 * screen.  The stop may begin while this CPU holds text_lock, and
 * spin_trylock() traps on that recursion, so the owner is checked first and
 * the screen is then drawn without the lock: nothing else runs on this CPU
 * again.  Interrupts are already disabled.
 */
void
drv_pcat_text_reveal_fatal(
	void)
{
	volatile uint32_t *pixels;
	unsigned spins;
	unsigned cpu;
	unsigned held;
	size_t count;
	size_t index;
	int locked;

	/* Takes text_lock unless this CPU holds it already or another holds it too long. */
	cpu = kern_cpu_current();
	locked = 0;
	for (spins = 0; spins < TEXT_FATAL_LOCK_SPINS; spins++) {
		/* This CPU stopped inside a console write: drawing without the lock is all that is left. */
		held = atomic_load_acquire(&text_lock.held);
		if (held != 0 && text_lock.owner_valid && text_lock.owner_cpu == cpu)
			break;

		/* Takes the lock when it is free. */
		locked = spin_trylock(&text_lock);
		if (locked)
			break;
	}

	/* The splash goes, and the console is shown from now on. */
	text_hidden = 0;
	drv_pcat_splash_stop();

	/* A console that left the display draws nothing there; the stop goes to the serial line and the log. */
	if (__atomic_load_n(&text_ended, __ATOMIC_ACQUIRE) != 0) {
		/* Gives the lock back when it was taken. */
		if (locked)
			spin_unlock(&text_lock);
		return;
	}

	/* A surface that is still attached is drawn to again, whoever suspended it. */
	if (text_surface != TEXT_SURFACE_NONE && text_columns != 0 &&
	    text_rows != 0)
		text_ready = 1;

	/* The whole framebuffer black, so the graphics under the text go. */
	if (text_ready && text_surface == TEXT_SURFACE_FRAMEBUFFER &&
	    text_pixels != NULL) {
		pixels = text_pixels;
		count = (size_t)text_stride * text_framebuffer_height;
		for (index = 0; index < count; index++)
			pixels[index] = 0U;
	}

	/* The cells, the cursor, and the hardware cursor of text memory. */
	redraw_locked();
	draw_cell_locked(text_cursor_row, text_cursor_column, text_cursor_visible);
	vga_cursor_locked();

	/* Gives the lock back when it was taken. */
	if (locked)
		spin_unlock(&text_lock);

	/* Succeeded: the retained text is on the screen. */
	return;
}

/*
 * Ends the console's use of the display for good: it stops drawing on its
 * surface (and the splash with it), keeps its cells following the
 * characters (which the serial mirror still carries), takes the kernel's
 * character path over from the HAL's early console if it had not yet, and
 * has /dev/graphics and its backend give up the display and their
 * mappings.  Returns what /dev/graphics returns, or EBUSY when a writer
 * this layer cannot stop, or the legacy text memory it keeps, lies in the
 * ranges.  Thread context, no lock held.
 */
int
drv_pcat_text_end(
	const struct kern_text_range *ranges,
	unsigned count)
{
	void (*published)(int);
	unsigned long irq;
	int foreign;
	int error;

	/* Serializes the end with every other text operation. */
	irq = spin_lock_irqsave(&text_lock);

	/* The first end stops the surface; a later one finds it stopped. */
	if (!text_ended) {
		/* Hides the hardware cursor of text memory, as a suspend does. */
		if (text_surface == TEXT_SURFACE_VGA_TEXT && text_ready) {
			kern_io_out8(TEXT_VGA_CRTC_INDEX, TEXT_VGA_CURSOR_START);
			kern_io_out8(TEXT_VGA_CRTC_DATA, TEXT_VGA_CURSOR_OFF);
		}

		/* Nothing draws on the surface from now on; the cells go on (a fatal stop reads the mark unlocked). */
		__atomic_store_n(&text_ended, 1, __ATOMIC_RELEASE);
		text_ready = 0;

		/* The splash and its spinner stop for good. */
		text_splash_ended = 1;
		drv_pcat_splash_stop();
	}

	spin_unlock_irqrestore(&text_lock, irq);

	/* The kernel's characters come here, not to the HAL's early console, from now on. */
	foreign = 0;
	published = __atomic_load_n(&kernel_putc, __ATOMIC_ACQUIRE);
	if (published == NULL) {
		__atomic_store_n(&kernel_putc, kern_text_kernel_putc, __ATOMIC_RELEASE);
	} else if (published != kern_text_kernel_putc) {
		foreign = 1;
	}

	/* /dev/graphics and the backend give the display and their mappings up. */
	error = drv_pcat_graphics_end(ranges, count);
	if (error != 0)
		return error;

	/* Another character path may still draw somewhere on the display: any memory the caller gives may be under it. */
	if (foreign && count != 0U)
		return EBUSY;

	/* Text memory's legacy aperture lies in a window that stays mapped. */
	if (text_surface == TEXT_SURFACE_VGA_TEXT &&
	    kern_text_range_overlaps(ranges, count, TEXT_VGA_APERTURE, TEXT_VGA_APERTURE_SIZE))
		return EBUSY;

	/* Succeeded: nothing of the text layer draws on the display or keeps memory in the ranges. */
	return 0;
}

/* Starts the thread that turns the splash's spinner by itself; without it the spinner turns with the log only. */
static void
text_splash_ticker_start(
	void)
{
	struct thread *thread;
	int error;

	/* The thread, at the default priority. */
	error = kthread_create(text_splash_ticker, NULL, SCHED_PRIORITY_DEFAULT, &thread);
	if (error != 0)
		return;

	/* Lets it run. */
	thread_start(thread);
}

/*
 * Looks at the splash's spinner every TEXT_SPLASH_TICK_MS while the console
 * is kept off the screen, so that it turns while no line comes, until the
 * console is shown, a display is taken for graphics, or
 * TEXT_SPLASH_TICKS_MAX looks have passed (two minutes).
 */
static void
text_splash_ticker(
	void *argument)
{
	unsigned long irq;
	unsigned turns;
	int going;

	/* Each turn after a short sleep. */
	(void)argument;
	for (turns = 0; turns < TEXT_SPLASH_TICKS_MAX; turns++) {
		sched_sleep(sched_ticks() + kern_ms_to_ticks(TEXT_SPLASH_TICK_MS));

		/* Serializes the spinner with the other console writers. */
		irq = spin_lock_irqsave(&text_lock);

		/* A spinner still over the splash turns. */
		going = 0;
		if (text_hidden && !text_splash_ended) {
			going = 1;
			drv_pcat_splash_step();
		}

		/* Lets the other console writers draw again. */
		spin_unlock_irqrestore(&text_lock, irq);

		/* The splash is gone: the thread ends. */
		if (!going)
			return;
	}
}

#ifdef KERN_TEST_TEXT_END_SECONDS
/* Starts, once a boot, the thread that ends the console after the configured wait. */
static void
text_end_test_start(
	void)
{
	struct thread *thread;
	int error;

	/* Only the first set-up of the layer starts it. */
	if (text_end_test_started)
		return;
	text_end_test_started = 1;

	/* The thread, at the default priority. */
	error = kthread_create(text_end_test, NULL, SCHED_PRIORITY_DEFAULT, &thread);
	if (error != 0) {
		kern_logf("text: the timed end did not start (%d)\n", error);
		return;
	}

	/* Lets it run. */
	thread_start(thread);
}

/*
 * Waits KERN_TEST_TEXT_END_SECONDS, then ends the console's use of the
 * display once, with the boot framebuffer's range (no range without one),
 * and keeps the result.
 */
static void
text_end_test(
	void *argument)
{
	struct kern_boot_framebuffer framebuffer;
	struct kern_text_range range;
	uint64_t deadline;
	int error;

	(void)argument;

	/* The wait, whole even if the sleep comes back early. */
	deadline = sched_ticks() + kern_ms_to_ticks((uint64_t)KERN_TEST_TEXT_END_SECONDS * 1000U);
	while (sched_ticks() < deadline)
		sched_sleep(deadline);

	/* The framebuffer the boot handed over, if there is one. */
	error = kern_boot_framebuffer("pcat.framebuffer", &framebuffer);
	if (error == 0 && framebuffer.size != 0U) {
		/* Ends over the framebuffer's range. */
		range.physical = framebuffer.physical_base;
		range.bytes = framebuffer.size;
		error = kern_text_end(&range, 1U);
	} else {
		/* Ends with no range to judge. */
		error = kern_text_end(NULL, 0U);
	}

	/* The result, then the mark that it is there. */
	text_end_test_result = error;
	__atomic_store_n(&text_end_test_done, 1, __ATOMIC_RELEASE);
	kern_logf("text: the timed end returned %d\n", error);
}
#endif

/*
 * Has the console draw through another mapping of the framebuffer (to for
 * from), and the spinner too, under the console's lock.
 */
void
drv_pcat_text_retarget(
	volatile uint32_t *from,
	volatile uint32_t *to)
{
	unsigned long irq;

	/* Only the mapping the console draws through now changes. */
	irq = spin_lock_irqsave(&text_lock);
	if (text_pixels == from) {
		text_pixels = to;
		drv_pcat_splash_retarget(to);
	}

	/* Other writers see the new mapping. */
	spin_unlock_irqrestore(&text_lock, irq);
}

/*
 * Reports whether a write changes the cells: the surface draws them, or
 * the console left the display and its cells keep following the
 * characters.  The caller holds text_lock.
 */
static int
cells_live_locked(
	void)
{
	/* A drawing surface takes every write. */
	if (text_ready)
		return 1;

	/* An ended console with a grid keeps its cells. */
	if (text_ended && text_surface != TEXT_SURFACE_NONE && text_columns != 0U && text_rows != 0U)
		return 1;

	/* A suspended console, or one without a grid, ignores writes. */
	return 0;
}
