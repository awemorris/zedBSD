/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The text console over the boot framebuffer and a board's serial line.
 *
 * The console keeps a grid of character cells as large as the framebuffer
 * holds and draws it with the 8x16 font through the boot framebuffer
 * driver, which also offers the screen to /dev/graphics.  Each character
 * the console or the kernel writes goes to the grid, and to the board's
 * serial line when that is the console; the kernel's own characters reach
 * it through kernel_putc, which the console publishes once it can show a
 * character, so the hardware layer stops touching the display and the
 * serial line.  While /dev/graphics or a display driver owns the screen
 * nothing is drawn and the characters reach only the serial line; when
 * the screen is given back the grid is drawn again over a cleared screen.
 * Once a GPU shows its own image the console leaves the screen for good.
 */

#include <stddef.h>
#include <stdint.h>

#include <drivers/generic/boot-fb.h>
#include <drivers/generic/fb-text.h>
#include "drivers/platform/pcat/graphics/vgafont.h"

#include <kern/atomic.h>
#include <kern/clock.h>
#include <kern/graphics-backend.h>
#include <kern/lock.h>
#include <kern/platform.h>
#include <kern/sched.h>
#include <kern/text-display.h>
#include <kern/thread.h>
#include <uapi/errno.h>

#ifdef KERN_TEST_TEXT_SUSPEND_SECONDS
#include <kern/klog.h>
#endif

/* The grid a serial terminal is assumed to have when there is no screen. */
#define TEXT_SERIAL_COLUMNS	80U
#define TEXT_SERIAL_ROWS	24U

/* One cell of the screen in pixels. */
#define TEXT_CELL_WIDTH		8U
#define TEXT_CELL_HEIGHT	16U

/* Bounds the grid; a 1920x1080 screen needs 240x67. */
#define TEXT_MAX_COLUMNS	240U
#define TEXT_MAX_ROWS		68U

/* A blank cell in the default attribute: the attribute in the high byte, the character in the low. */
#define TEXT_CELL_BLANK		((uint16_t)((KERN_TEXT_ATTRIB_NORMAL << 8) | ' '))

/* A drawn cell's mark for the cursor, beside its cell word. */
#define TEXT_DRAWN_CURSOR	0x10000U

/* How many times a fatal stop tries text_lock before it draws without it. */
#define TEXT_FATAL_LOCK_SPINS	50000000U

static void text_get_size(unsigned *columns, unsigned *rows);
static void text_putc(int character);
static void text_write(unsigned row, unsigned column, uint8_t attribute, const char *utf8);
static void text_clear(void);
static int text_set_cursor(unsigned row, unsigned column);
static void text_get_cursor(unsigned *row, unsigned *column, int *visible);
static void text_show_cursor(int visible);
static void text_update_cursor(void);
static void text_suspend(void);
static void text_resume(void);
static void text_reveal_fatal(void);
static int text_end(const struct kern_text_range *ranges, unsigned count);

/*
 * The console's text operations, registered once at start-up and never
 * changed.
 */
static const struct kern_text_ops fb_text_ops = {
	.get_size = text_get_size,
	.putc = text_putc,
	.write = text_write,
	.clear = text_clear,
	.set_cursor = text_set_cursor,
	.get_cursor = text_get_cursor,
	.show_cursor = text_show_cursor,
	.update_cursor = text_update_cursor,
	.suspend = text_suspend,
	.resume = text_resume,
	.reveal_fatal = text_reveal_fatal,
	.end = text_end
};

/*
 * Serializes every change of the grid, every drawing of it, and every
 * character sent to the serial line, against interrupt handlers and other
 * processors.  Initialized once by drv_fb_text_init() before the
 * operations are registered.
 */
static struct spinlock text_lock;

/*
 * What the board adds: its serial line and its fatal reveal.  Set once by
 * drv_fb_text_init() before the operations are registered; NULL before.
 */
static const struct drv_fb_text_board *text_board;

/*
 * Whether the console publishes kernel_putc, so the hardware layer's early
 * console no longer writes: set once with a release by the start-up or the
 * end, read with an acquire.  Before it is set a character goes to the
 * early console.
 */
static int text_published;

/*
 * The grid as it should look: one word per cell, the attribute in the high
 * byte and the character in the low, row after row of text_columns.
 * Changed only under text_lock.
 */
static uint16_t text_cells[TEXT_MAX_ROWS * TEXT_MAX_COLUMNS];

/*
 * What each cell of the screen shows now: its cell word, with
 * TEXT_DRAWN_CURSOR when the cursor is drawn on it.  A cell is drawn only
 * when it differs from what it should show, so a scroll draws only the
 * cells that changed.  Changed only under text_lock.
 */
static uint32_t text_drawn[TEXT_MAX_ROWS * TEXT_MAX_COLUMNS];

/*
 * The grid's size and its top-left pixel, centered on the screen.  Set
 * once by drv_fb_text_init() before the operations are registered; zero
 * without a screen.
 */
static unsigned text_columns;
static unsigned text_rows;
static unsigned text_origin_x;
static unsigned text_origin_y;

/* The cursor, and whether it is shown.  Changed only under text_lock. */
static unsigned text_cursor_row;
static unsigned text_cursor_column;
static int text_cursor_visible;

/*
 * Whether the console has a grid on the screen.  Set once by
 * drv_fb_text_init() before the operations are registered.
 */
static int text_surface;

/*
 * Whether the grid is drawn: cleared while /dev/graphics or a display
 * driver owns the screen, and for good once the console has left it.
 * Changed only under text_lock (and by a fatal stop that cannot take it).
 */
static int text_drawing;

/*
 * Whether the console has left the screen for good (a GPU shows its own
 * image there): nothing is drawn again, by a write, a resume or a fatal
 * stop, while the cells keep following the characters.  Set once under
 * text_lock with a release, and read with an acquire, also by a fatal
 * stop that draws without the lock.
 */
static int text_ended;

#ifdef KERN_TEST_TEXT_SUSPEND_SECONDS
/*
 * The test build's stand-in for a graphics owner: how long it keeps the
 * screen, the color it fills it with, and the mark it writes on it so a
 * reading of the screen tells the owner's image from the console's.
 */
#define TEXT_TEST_OWNER_SECONDS	30U
#define TEXT_TEST_OWNER_COLOR	0x0000aaU
#define TEXT_TEST_OWNER_MARK	"GRAPHICS OWNER"

/*
 * Whether the test owner's thread was started; written by the console's
 * start-up only.
 */
static int text_test_started;
#endif

/* The sixteen text colors as 0xRRGGBB, in the attribute's order. */
static const uint32_t text_palette[16] = {
	0x000000U, 0x0000aaU, 0x00aa00U, 0x00aaaaU,
	0xaa0000U, 0xaa00aaU, 0xaa5500U, 0xaaaaaaU,
	0x555555U, 0x5555ffU, 0x55ff55U, 0x55ffffU,
	0xff5555U, 0xff55ffU, 0xffff55U, 0xffffffU
};

static int cells_live_locked(void);
static void putc_locked(int character);
static void newline_locked(void);
static void scroll_locked(void);
static void draw_cell_locked(unsigned row, unsigned column);
static void draw_all_locked(void);
static void repaint_locked(void);
static void cursor_move_locked(unsigned row, unsigned column);
#ifdef KERN_TEST_TEXT_SUSPEND_SECONDS
static void text_test_start(void);
static void text_test_owner(void *argument);
#endif

/*
 * Publishes the console.
 */
void
drv_fb_text_init(
	const struct drv_fb_text_board *board)
{
	void (*published)(int);
	unsigned long irq;
	unsigned width;
	unsigned height;
	unsigned index;
	int ready;

	/* Readies the lock before any writer can reach the console. */
	spin_init(&text_lock, LOCK_RANK_CONSOLE_TEXT, "framebuffer text console");
	text_board = board;

	/* Sizes the grid to the screen and centers it; without one the console is the serial line alone. */
	ready = drv_boot_fb_ready();
	if (ready) {
		drv_boot_fb_get_size(&width, &height);
		text_columns = width / TEXT_CELL_WIDTH;
		if (text_columns > TEXT_MAX_COLUMNS)
			text_columns = TEXT_MAX_COLUMNS;
		text_rows = height / TEXT_CELL_HEIGHT;
		if (text_rows > TEXT_MAX_ROWS)
			text_rows = TEXT_MAX_ROWS;
		text_origin_x = (width - text_columns * TEXT_CELL_WIDTH) / 2U;
		text_origin_y = (height - text_rows * TEXT_CELL_HEIGHT) / 2U;
		if (text_columns != 0 && text_rows != 0)
			text_surface = 1;
	}

	/* Starts from a blank grid at the top of a cleared screen. */
	irq = spin_lock_irqsave(&text_lock);

	for (index = 0; index < TEXT_MAX_ROWS * TEXT_MAX_COLUMNS; index++)
		text_cells[index] = TEXT_CELL_BLANK;
	text_cursor_row = 0;
	text_cursor_column = 0;
	text_cursor_visible = 1;
	if (text_surface) {
		text_drawing = 1;
		repaint_locked();
	}

	spin_unlock_irqrestore(&text_lock, irq);

	/* Makes the grid the console's text output. */
	kern_text_register(&fb_text_ops);

#ifdef KERN_TEST_TEXT_SUSPEND_SECONDS
	/* The test build's graphics owner takes the screen and gives it back after the configured wait. */
	text_test_start();
#endif

	/* Without a screen or a serial line the hardware layer keeps the kernel's characters. */
	if (!text_surface && (board == NULL || board->serial_write == NULL))
		return;

	/* Takes the kernel's characters over from the hardware layer, unless another path has them. */
	published = __atomic_load_n(&kernel_putc, __ATOMIC_ACQUIRE);
	if (published == NULL) {
		__atomic_store_n(&text_published, 1, __ATOMIC_RELEASE);
		__atomic_store_n(&kernel_putc, kern_text_kernel_putc, __ATOMIC_RELEASE);
	}
}

/* Reports the grid, or the assumed terminal's without a screen. */
static void
text_get_size(
	unsigned *columns,
	unsigned *rows)
{
	/* Reports the width when the caller asked for it. */
	if (columns != NULL) {
		*columns = TEXT_SERIAL_COLUMNS;
		if (text_surface)
			*columns = text_columns;
	}

	/* Reports the height when the caller asked for it. */
	if (rows != NULL) {
		*rows = TEXT_SERIAL_ROWS;
		if (text_surface)
			*rows = text_rows;
	}
}

/* Writes one character to the serial line and the grid. */
static void
text_putc(
	int character)
{
	unsigned long irq;
	int published;

	/* Before the console takes the kernel's characters over, the hardware layer shows them. */
	published = __atomic_load_n(&text_published, __ATOMIC_ACQUIRE);
	if (!published) {
		kern_console_early_putc(character);
		return;
	}

	/* Writes the serial line and the grid as one step. */
	irq = spin_lock_irqsave(&text_lock);

	putc_locked(character);

	spin_unlock_irqrestore(&text_lock, irq);
}

/*
 * Writes a terminated string at a fixed cell with a fixed attribute.
 *
 * The string does not wrap and is clipped at the end of the row; the
 * serial line, a teletype, does not get it.
 */
static void
text_write(
	unsigned row,
	unsigned column,
	uint8_t attribute,
	const char *utf8)
{
	unsigned long irq;
	unsigned index;
	int live;

	/* Ignores an absent string. */
	if (utf8 == NULL)
		return;

	/* Stores and draws each byte until the row or the string ends. */
	irq = spin_lock_irqsave(&text_lock);

	live = cells_live_locked();
	if (live && row < text_rows) {
		for (index = column; index < text_columns && *utf8 != '\0'; index++) {
			text_cells[row * text_columns + index] = (uint16_t)(((uint16_t)attribute << 8) | (uint8_t)*utf8);
			draw_cell_locked(row, index);
			utf8++;
		}
	}

	spin_unlock_irqrestore(&text_lock, irq);
}

/* Blanks the grid and homes the cursor. */
static void
text_clear(
	void)
{
	unsigned long irq;
	unsigned index;
	int live;

	/* Empties every cell with the default attribute and draws what changed. */
	irq = spin_lock_irqsave(&text_lock);

	live = cells_live_locked();
	if (live) {
		for (index = 0; index < text_rows * text_columns; index++)
			text_cells[index] = TEXT_CELL_BLANK;
		text_cursor_row = 0;
		text_cursor_column = 0;
		draw_all_locked();
	}

	spin_unlock_irqrestore(&text_lock, irq);
}

/* Moves the cursor; refuses a position outside the grid. */
static int
text_set_cursor(
	unsigned row,
	unsigned column)
{
	unsigned long irq;
	unsigned rows;
	unsigned columns;
	int live;

	/* Reads the grid's size. */
	text_get_size(&columns, &rows);

	/* Refuses a row below the grid. */
	if (row >= rows)
		return 0;

	/* Refuses a column right of the grid. */
	if (column >= columns)
		return 0;

	/* Moves the cursor of a grid that follows the characters; a teletype has none to move. */
	irq = spin_lock_irqsave(&text_lock);

	live = cells_live_locked();
	if (live)
		cursor_move_locked(row, column);

	spin_unlock_irqrestore(&text_lock, irq);

	/* Succeeded: the position is inside the grid. */
	return 1;
}

/* Reports the cursor, the home position for a teletype. */
static void
text_get_cursor(
	unsigned *row,
	unsigned *column,
	int *visible)
{
	unsigned long irq;

	/* Reads a consistent cursor. */
	irq = spin_lock_irqsave(&text_lock);

	if (row != NULL)
		*row = text_cursor_row;
	if (column != NULL)
		*column = text_cursor_column;
	if (visible != NULL)
		*visible = text_cursor_visible;

	spin_unlock_irqrestore(&text_lock, irq);
}

/* Shows or hides the cursor. */
static void
text_show_cursor(
	int visible)
{
	unsigned long irq;

	/* Draws the cursor's cell in its new state. */
	irq = spin_lock_irqsave(&text_lock);

	text_cursor_visible = 0;
	if (visible)
		text_cursor_visible = 1;
	draw_cell_locked(text_cursor_row, text_cursor_column);

	spin_unlock_irqrestore(&text_lock, irq);
}

/* Draws the cursor's cell after a stream of writes. */
static void
text_update_cursor(
	void)
{
	unsigned long irq;

	/* Draws the cell when it does not show the cursor yet. */
	irq = spin_lock_irqsave(&text_lock);

	draw_cell_locked(text_cursor_row, text_cursor_column);

	spin_unlock_irqrestore(&text_lock, irq);
}

/* Stops drawing the grid while /dev/graphics or a display driver owns the screen. */
static void
text_suspend(
	void)
{
	unsigned long irq;

	/* Nothing is drawn from now on; the serial line goes on. */
	irq = spin_lock_irqsave(&text_lock);

	text_drawing = 0;

	spin_unlock_irqrestore(&text_lock, irq);
}

/* Draws the grid again, over a cleared screen, once the screen is given back. */
static void
text_resume(
	void)
{
	unsigned long irq;
	int ended;

	/* Gives the screen back to the grid and repaints all of it, unless the console has left it. */
	irq = spin_lock_irqsave(&text_lock);

	ended = __atomic_load_n(&text_ended, __ATOMIC_ACQUIRE);
	if (!ended && text_surface) {
		text_drawing = 1;
		repaint_locked();
	}

	spin_unlock_irqrestore(&text_lock, irq);
}

/*
 * Shows the console for a fatal stop, even over a graphics owner.
 *
 * The stop may begin while this processor holds text_lock, and
 * spin_trylock() traps on that recursion, so the owner is checked first
 * and the screen is then drawn without the lock: nothing else runs on
 * this processor again.  Interrupts are already disabled.
 */
static void
text_reveal_fatal(
	void)
{
	unsigned spins;
	unsigned cpu;
	unsigned held;
	int locked;
	int ended;

	/* Nothing is shown once the console has left the screen: it is the GPU's, and the serial line has it all. */
	ended = __atomic_load_n(&text_ended, __ATOMIC_ACQUIRE);
	if (ended)
		return;

	/* The board puts the boot screen back on the display first. */
	if (text_board != NULL && text_board->reveal_fatal != NULL)
		text_board->reveal_fatal();

	/* Nothing to draw without a screen. */
	if (!text_surface)
		return;

	/* Takes text_lock unless this processor holds it already or another holds it too long. */
	cpu = kern_cpu_current();
	locked = 0;
	for (spins = 0; spins < TEXT_FATAL_LOCK_SPINS; spins++) {
		/* This processor stopped inside a console write: drawing without the lock is all that is left. */
		held = atomic_load_acquire(&text_lock.held);
		if (held != 0 && text_lock.owner_valid && text_lock.owner_cpu == cpu)
			break;

		/* Takes the lock when it is free. */
		locked = spin_trylock(&text_lock);
		if (locked)
			break;
	}

	/* Takes the screen back and draws the grid over a cleared screen. */
	text_drawing = 1;
	repaint_locked();

	/* Gives the lock back when it was taken. */
	if (locked)
		spin_unlock(&text_lock);
}

/*
 * Ends the console's use of the screen for good: the grid is never drawn
 * again (it still follows the characters, which still reach the serial
 * line), the kernel's characters are taken over from the hardware layer's
 * early console if they were not yet, and /dev/graphics and the boot
 * framebuffer give the screen and their mappings up.  Returns the first
 * error they return, or EBUSY when another character path, which may draw
 * anywhere on the screen, was published and the caller gives memory back;
 * ENOTSUP, changing nothing, without a screen or a serial line.  Thread
 * context, no lock held.
 */
static int
text_end(
	const struct kern_text_range *ranges,
	unsigned count)
{
	void (*published)(int);
	unsigned long irq;
	int foreign;
	int graphics_error;
	int framebuffer_error;

	/*
	 * Without a screen or a serial line the hardware layer's early console
	 * is the only path the characters have: nothing changes.
	 */
	if (!text_surface && (text_board == NULL || text_board->serial_write == NULL))
		return ENOTSUP;

	/* Stops drawing for good, apart from any writer: once this section is left none draws again. */
	irq = spin_lock_irqsave(&text_lock);

	__atomic_store_n(&text_ended, 1, __ATOMIC_RELEASE);
	text_drawing = 0;

	spin_unlock_irqrestore(&text_lock, irq);

	/* The kernel's characters come here, not to the hardware layer's early console, from now on. */
	foreign = 0;
	published = __atomic_load_n(&kernel_putc, __ATOMIC_ACQUIRE);
	if (published == NULL) {
		__atomic_store_n(&text_published, 1, __ATOMIC_RELEASE);
		__atomic_store_n(&kernel_putc, kern_text_kernel_putc, __ATOMIC_RELEASE);
	} else if (published != kern_text_kernel_putc) {
		foreign = 1;
	}

	/* /dev/graphics drops its mode, refuses every request, and has its backend give its mapping up. */
	graphics_error = 0;
#if CONFIG_DRIVER_GRAPHICS_DEVICE
	graphics_error = drv_graphics_device_end(ranges, count);
#endif

	/* The framebuffer's own mapping goes, whichever backend /dev/graphics chose. */
	framebuffer_error = drv_boot_fb_end(ranges, count);

	/* Reports what /dev/graphics left in the ranges first. */
	if (graphics_error != 0)
		return graphics_error;

	/* Then what the framebuffer left in them. */
	if (framebuffer_error != 0)
		return framebuffer_error;

	/* Another character path may still draw somewhere on the screen: any memory the caller gives may be under it. */
	if (foreign && count != 0U)
		return EBUSY;

	/* Succeeded: nothing of the console draws the screen or keeps memory in the ranges. */
	return 0;
}

/*
 * Reports whether a write changes the cells: the grid is drawn, or the
 * console left the screen and its cells keep following the characters.
 * While /dev/graphics or a display driver owns the screen the characters
 * are dropped, not kept for later.  The caller holds text_lock.
 */
static int
cells_live_locked(
	void)
{
	int ended;

	/* A console without a screen has no cells. */
	if (!text_surface)
		return 0;

	/* A drawn grid takes every write. */
	if (text_drawing)
		return 1;

	/* An ended console keeps its cells. */
	ended = __atomic_load_n(&text_ended, __ATOMIC_ACQUIRE);
	if (ended)
		return 1;

	/* A suspended console ignores writes. */
	return 0;
}

/* Writes one character to the serial line and at the cursor. */
static void
putc_locked(
	int character)
{
	unsigned index;
	uint8_t stored;
	int live;

	/* The serial line, a teletype, carries every byte, control characters too. */
	if (text_board != NULL && text_board->serial_write != NULL)
		text_board->serial_write(character);

	/* Nothing more without cells that follow the characters. */
	live = cells_live_locked();
	if (!live)
		return;

	/* Handles the line and column control characters. */
	if (character == '\n') {
		/* A new line moves to the start of the next row. */
		newline_locked();
		return;
	} else if (character == '\r') {
		/* A carriage return moves to the start of the row. */
		cursor_move_locked(text_cursor_row, 0);
		return;
	} else if (character == '\b') {
		/* Steps back one cell without erasing. */
		if (text_cursor_column > 0)
			cursor_move_locked(text_cursor_row, text_cursor_column - 1U);
		return;
	} else if (character == '\t') {
		/* Advances to the next eight-column stop, wrapping past the row's end. */
		index = (text_cursor_column + 8U) & ~7U;
		if (index >= text_columns) {
			newline_locked();
			return;
		}
		cursor_move_locked(text_cursor_row, index);
		return;
	}

	/* Keeps a printable character, and substitutes anything else. */
	stored = '?';
	if (character >= 0x20 && character < 0x7f)
		stored = (uint8_t)character;

	/* Stores the character in the cell's attribute and draws it. */
	index = text_cursor_row * text_columns + text_cursor_column;
	text_cells[index] = (uint16_t)((text_cells[index] & 0xff00U) | stored);
	draw_cell_locked(text_cursor_row, text_cursor_column);

	/* Moves past the cell, to the next row at the end of this one. */
	if (text_cursor_column + 1U >= text_columns) {
		newline_locked();
		return;
	}
	cursor_move_locked(text_cursor_row, text_cursor_column + 1U);
}

/* Moves the cursor to the start of the next line, scrolling at the bottom. */
static void
newline_locked(
	void)
{
	/* Moves down when there is a row below. */
	if (text_cursor_row + 1U < text_rows) {
		cursor_move_locked(text_cursor_row + 1U, 0);
		return;
	}

	/* Scrolls the grid, then draws the cursor at the start of the bottom row. */
	scroll_locked();
	cursor_move_locked(text_cursor_row, 0);
}

/* Moves every row up by one and blanks the bottom one. */
static void
scroll_locked(
	void)
{
	unsigned row;
	unsigned column;

	/* Shifts the stored rows up by one. */
	for (row = 1; row < text_rows; row++) {
		for (column = 0; column < text_columns; column++)
			text_cells[(row - 1U) * text_columns + column] = text_cells[row * text_columns + column];
	}

	/* Empties the bottom row. */
	for (column = 0; column < text_columns; column++)
		text_cells[(text_rows - 1U) * text_columns + column] = TEXT_CELL_BLANK;

	/* Draws the cells the shift changed. */
	draw_all_locked();
}

/*
 * Draws one cell when it differs from what the screen shows: the glyph in
 * the attribute's colors, exchanged under a shown cursor.
 */
static void
draw_cell_locked(
	unsigned row,
	unsigned column)
{
	const uint8_t *glyph;
	unsigned index;
	uint32_t wanted;
	uint16_t cell;
	uint8_t attribute;
	uint8_t character;

	/* Nothing is drawn while the grid is not on the screen, nor off the grid. */
	if (!text_drawing || row >= text_rows || column >= text_columns)
		return;

	/* What the cell should show: its word, and the cursor's mark on the cursor's cell. */
	index = row * text_columns + column;
	cell = text_cells[index];
	wanted = cell;
	if (text_cursor_visible && row == text_cursor_row && column == text_cursor_column)
		wanted |= TEXT_DRAWN_CURSOR;

	/* An unchanged cell is left as it is. */
	if (text_drawn[index] == wanted)
		return;

	/* The cursor exchanges the cell's colors. */
	attribute = (uint8_t)(cell >> 8);
	if ((wanted & TEXT_DRAWN_CURSOR) != 0)
		attribute = (uint8_t)((attribute << 4) | (attribute >> 4));

	/* Draws the glyph in the attribute's foreground on its background. */
	character = (uint8_t)cell;
	glyph = &drv_pcat_vgafont16[(unsigned)character * PCAT_VGAFONT_HEIGHT];
	drv_boot_fb_glyph(text_origin_x + column * TEXT_CELL_WIDTH,
			  text_origin_y + row * TEXT_CELL_HEIGHT,
			  glyph,
			  text_palette[attribute & 15U],
			  text_palette[(attribute >> 4) & 15U]);

	/* Records what the cell shows now. */
	text_drawn[index] = wanted;
}

/* Draws every cell that differs from what the screen shows. */
static void
draw_all_locked(
	void)
{
	unsigned row;
	unsigned column;

	/* Each row, left to right. */
	for (row = 0; row < text_rows; row++) {
		for (column = 0; column < text_columns; column++)
			draw_cell_locked(row, column);
	}
}

/*
 * Clears the whole screen and draws the grid on it, unless the grid is not
 * on the screen.
 */
static void
repaint_locked(
	void)
{
	unsigned width;
	unsigned height;
	unsigned index;

	/* Leaves a screen someone else owns alone. */
	if (!text_drawing)
		return;

	/* Paints the whole framebuffer black. */
	drv_boot_fb_get_size(&width, &height);
	drv_boot_fb_fill(0, 0, width, height, text_palette[0]);

	/* Every cell now shows a blank without the cursor. */
	for (index = 0; index < text_rows * text_columns; index++)
		text_drawn[index] = TEXT_CELL_BLANK;

	/* Draws the cells that are not blank, and the cursor. */
	draw_all_locked();
}

/* Moves the cursor, drawing the cell it leaves and the cell it reaches. */
static void
cursor_move_locked(
	unsigned row,
	unsigned column)
{
	unsigned old_row;
	unsigned old_column;

	/* The cell the cursor leaves loses its mark, then the new one gains it. */
	old_row = text_cursor_row;
	old_column = text_cursor_column;
	text_cursor_row = row;
	text_cursor_column = column;
	draw_cell_locked(old_row, old_column);
	draw_cell_locked(row, column);
}

#ifdef KERN_TEST_TEXT_SUSPEND_SECONDS
/* Starts, once a boot, the thread that stands in for a graphics owner. */
static void
text_test_start(
	void)
{
	struct thread *thread;
	int error;

	/* Only the first start-up of the console starts it. */
	if (text_test_started || !text_surface)
		return;
	text_test_started = 1;

	/* The thread, at the default priority. */
	error = kthread_create(text_test_owner, NULL, SCHED_PRIORITY_DEFAULT, &thread);
	if (error != 0)
		return;

	/* Lets it run. */
	thread_start(thread);
}

/*
 * Waits KERN_TEST_TEXT_SUSPEND_SECONDS, then takes the screen as a
 * graphics owner does (the console suspended, the screen filled and
 * marked), keeps it TEXT_TEST_OWNER_SECONDS, and gives it back.
 */
static void
text_test_owner(
	void *argument)
{
	const char *mark;
	unsigned width;
	unsigned height;
	unsigned column;
	uint64_t deadline;

	(void)argument;

	/* The wait, whole even if the sleep comes back early. */
	deadline = sched_ticks() + kern_ms_to_ticks((uint64_t)KERN_TEST_TEXT_SUSPEND_SECONDS * 1000U);
	while (sched_ticks() < deadline)
		sched_sleep(deadline);

	/* Takes the screen: the console stops drawing, then the owner's image goes on. */
	kern_logf("text: the test owner takes the screen\n");
	kern_text_suspend();
	drv_boot_fb_get_size(&width, &height);
	drv_boot_fb_fill(0, 0, width, height, TEXT_TEST_OWNER_COLOR);

	/* Marks the image on the grid's tenth row, as text a reading of the screen finds. */
	mark = TEXT_TEST_OWNER_MARK;
	for (column = 0; mark[column] != '\0'; column++) {
		drv_boot_fb_glyph(text_origin_x + (column + 8U) * TEXT_CELL_WIDTH,
				  text_origin_y + 10U * TEXT_CELL_HEIGHT,
				  &drv_pcat_vgafont16[(unsigned)(uint8_t)mark[column] * PCAT_VGAFONT_HEIGHT],
				  text_palette[15],
				  TEXT_TEST_OWNER_COLOR);
	}

	/* Keeps the screen for the owner's time, a line going to the log meanwhile. */
	deadline = sched_ticks() + kern_ms_to_ticks((uint64_t)TEXT_TEST_OWNER_SECONDS * 1000U);
	while (sched_ticks() < deadline)
		sched_sleep(deadline);
	kern_logf("text: the test owner gives the screen back\n");

	/* Gives the screen back: the console draws its grid again. */
	kern_text_resume();
}
#endif
