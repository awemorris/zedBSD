/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The sun4u console: /dev/console over the screen and the serial port.
 *
 * The console is a teletype.  When the firmware framebuffer can be mapped
 * (WS275) the console owns the screen: it keeps an 80-column grid of the
 * framebuffer's height, draws it with the 8x16 font, writes each character
 * to the serial port itself, and publishes the same output as the
 * kernel's console (kernel_putc), so the HAL stops touching the display
 * and the kernel's own records and the terminal share one cursor.  The
 * grid stops being drawn while /dev/graphics owns the screen and is drawn
 * again, over a cleared screen, when it is given back.  Once a GPU shows
 * its own image the console leaves the screen for good: the grid is never
 * drawn again and the framebuffer's mapping is given back, while the
 * characters still reach the serial port.  Without a framebuffer each
 * character goes to kern_console_early_putc() as before.  A positioned
 * write has no teletype meaning and is dropped.
 *
 * Input is read from the serial port, a 16550, by a kernel thread that
 * looks at it every few milliseconds, so the console needs no interrupt
 * routing.  The HAL names the port by "sun4u.serial.port".
 */

#include <stdint.h>

#include "drivers/platform/sun4u/sun4u-console.h"
#include "drivers/platform/sun4u/sun4u-firmware-fb.h"
#include "drivers/platform/pcat/graphics/vgafont.h"

#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/graphics-backend.h>
#include <kern/irq.h>
#include <kern/platform.h>
#include <kern/sched.h>
#include <kern/text-display.h>
#include <kern/thread.h>
#include <kern/tty.h>
#include <uapi/errno.h>

/*
 * The 16550's receive buffer and line status registers, and the bit that
 * says a byte was received.
 *
 * Values referenced: National Semiconductor PC16550D data sheet (the
 * register offsets and the data-ready bit of the line status register).
 */
#define SUN4U_UART_RBR		0U
#define SUN4U_UART_THR		0U
#define SUN4U_UART_LSR		5U
#define SUN4U_UART_LSR_DR	0x01U
#define SUN4U_UART_LSR_THRE	0x20U

/* How long a write waits for the transmitter before dropping the byte. */
#define SUN4U_UART_SPIN_LIMIT	100000U

/*
 * The grid the console reports: the HAL's screen's without a framebuffer,
 * 80 columns by as many 16-pixel rows as the framebuffer holds with one
 * (at most SUN4U_CONSOLE_MAX_ROWS).
 */
#define SUN4U_CONSOLE_COLUMNS	80U
#define SUN4U_CONSOLE_ROWS	25U
#define SUN4U_CONSOLE_MAX_ROWS	128U
#define SUN4U_CONSOLE_CELL_WIDTH	8U
#define SUN4U_CONSOLE_CELL_HEIGHT	16U

/* The grid's colors: light gray on black, and the cursor's white. */
#define SUN4U_CONSOLE_FOREGROUND	0xaaaaaaU
#define SUN4U_CONSOLE_BACKGROUND	0x000000U
#define SUN4U_CONSOLE_CURSOR		0xffffffU

/* A drawn cell's mark for the cursor, beside its character. */
#define SUN4U_CONSOLE_DRAWN_CURSOR	0x100U

/* How often the serial port is read, and how many bytes at a time. */
#define SUN4U_CONSOLE_POLL_MS	10U
#define SUN4U_CONSOLE_DRAIN	64U

/* The terminal's Enter and Delete, and what the line discipline expects. */
#define SUN4U_CONSOLE_RETURN	0x0dU
#define SUN4U_CONSOLE_DELETE	0x7fU
#define SUN4U_CONSOLE_BACKSPACE	0x08U

static void console_get_size(unsigned *columns, unsigned *rows);
static void console_putc(int character);
static void console_write(unsigned row, unsigned column, uint8_t attribute, const char *utf8);
static void console_clear(void);
static int console_set_cursor(unsigned row, unsigned column);
static void console_get_cursor(unsigned *row, unsigned *column, int *visible);
static void console_show_cursor(int visible);
static void console_nothing(void);
static void console_input_worker(void *argument);
static void console_reveal_fatal(void);
static void console_suspend(void);
static void console_resume(void);
static int console_end(const struct kern_text_range *ranges, unsigned count);
static void console_kernel_putc(int character);
static void serial_write(int character);
static unsigned long screen_enter(int *owned);
static void screen_leave(unsigned long enabled, int owned);
static void screen_putc(int character);
static void screen_newline(void);
static void screen_draw_cell(unsigned row, unsigned column);
static void screen_draw_all(void);
static void screen_repaint(void);

/*
 * The console's text operations, registered once at start-up and never
 * changed.  Everything but a character goes nowhere, because the console
 * is a teletype; suspend, resume, reveal_fatal and end matter only when
 * the console or the hardware layer draws the screen.
 */
static const struct kern_text_ops sun4u_console_ops = {
	.get_size = console_get_size,
	.putc = console_putc,
	.write = console_write,
	.clear = console_clear,
	.set_cursor = console_set_cursor,
	.get_cursor = console_get_cursor,
	.show_cursor = console_show_cursor,
	.update_cursor = console_nothing,
	.suspend = console_suspend,
	.resume = console_resume,
	.reveal_fatal = console_reveal_fatal,
	.end = console_end
};

/*
 * The serial port's I/O port number, from the HAL; set once when input is
 * started.
 */
static uint16_t serial_port;

/*
 * Whether the console draws the framebuffer, set once by
 * drv_sun4u_console_init() before kernel_putc is published.
 */
static int screen_on;

/*
 * The grid as it should look, its size, and the cursor.  Changed only
 * inside screen_enter() and screen_leave(): interrupts off, which is
 * enough on the port's one processor, and screen_busy set.  A zero
 * character is a cell never written, drawn as a blank.
 */
static uint8_t screen_grid[SUN4U_CONSOLE_MAX_ROWS][SUN4U_CONSOLE_COLUMNS];
static unsigned screen_rows;
static unsigned screen_columns;
static unsigned screen_row;
static unsigned screen_column;

/*
 * What each cell of the screen shows now: its character, with
 * SUN4U_CONSOLE_DRAWN_CURSOR when the cursor is drawn on it.  A cell is drawn only when it differs from
 * the grid, so a scroll redraws only the cells that changed.  Guarded as
 * the grid is.
 */
static uint16_t screen_drawn[SUN4U_CONSOLE_MAX_ROWS][SUN4U_CONSOLE_COLUMNS];

/*
 * Whether /dev/graphics owns the screen: the grid is still kept, but not
 * drawn.  Set and cleared inside screen_enter() (and by a fatal stop
 * without it).
 */
static int screen_suspended;

/*
 * Whether the console has left the screen for good (a GPU shows its own
 * image there): screen_suspended stays set, so neither a write, a resume
 * nor a fatal stop's reveal draws again, while the grid keeps following
 * the characters.  Set once inside screen_enter() with a release, and read
 * inside it, or by a fatal stop's reveal with an acquire.
 */
static int screen_ended;

/*
 * Set while a writer changes the grid.  A writer that finds it set is the
 * same processor coming back in (an interrupt handler's record, a fatal
 * stop): it writes anyway rather than waiting for itself.
 */
static unsigned screen_busy;

/*
 * The kernel thread that reads the serial port, or NULL before input was
 * started.  It is created once and runs for the life of the system; the
 * pointer only keeps a second start from creating another.
 */
static struct thread *input_worker;

/*
 * Publishes the console output.
 */
void
drv_sun4u_console_init(
	void)
{
	const uint64_t *port;
	unsigned width;
	unsigned height;
	unsigned row;
	unsigned column;
	int error;

	/* Makes the teletype the console's text output. */
	kern_text_register(&sun4u_console_ops);

	/* Without the framebuffer the HAL keeps drawing the screen. */
	error = drv_sun4u_fb_init();
	if (error != 0)
		return;

	/* Finds the serial port the console writes itself, when there is one. */
	port = kern_boot_handoff("sun4u.serial.port");
	if (port != NULL && *port <= UINT16_MAX)
		serial_port = (uint16_t)*port;

	/* Sizes the grid to the framebuffer. */
	drv_sun4u_fb_get_size(&width, &height);
	screen_columns = width / SUN4U_CONSOLE_CELL_WIDTH;
	if (screen_columns > SUN4U_CONSOLE_COLUMNS)
		screen_columns = SUN4U_CONSOLE_COLUMNS;
	screen_rows = height / SUN4U_CONSOLE_CELL_HEIGHT;
	if (screen_rows > SUN4U_CONSOLE_MAX_ROWS)
		screen_rows = SUN4U_CONSOLE_MAX_ROWS;
	if (screen_columns == 0 || screen_rows == 0)
		return;

	/* Starts from a blank grid at the top of a cleared screen. */
	for (row = 0; row < screen_rows; row++) {
		for (column = 0; column < screen_columns; column++)
			screen_grid[row][column] = 0;
	}
	screen_row = 0;
	screen_column = 0;
	screen_repaint();
	screen_on = 1;

	/* Takes the kernel's output over from the HAL. */
	__atomic_store_n(&kernel_putc, console_kernel_putc, __ATOMIC_RELEASE);
}

/*
 * Starts reading the serial port into the console.
 */
int
drv_sun4u_console_start_input(
	void)
{
	const uint64_t *port;
	int error;

	/* A second start does nothing. */
	if (input_worker != NULL)
		return 0;

	/* Without the port's name from the HAL, the console reads no input. */
	port = kern_boot_handoff("sun4u.serial.port");
	if (port == NULL || *port > UINT16_MAX)
		return 0;
	serial_port = (uint16_t)*port;

	/* Creates the reader; without it the console stays output only. */
	error = kthread_create(console_input_worker, NULL, SCHED_PRIORITY_DEFAULT, &input_worker);
	if (error != 0)
		return error;

	/* Lets the reader run. */
	thread_start(input_worker);

	/* Succeeded. */
	return 0;
}

/* Reports the grid. */
static void
console_get_size(
	unsigned *columns,
	unsigned *rows)
{
	/* Reports the width when the caller asked for it. */
	if (columns != NULL) {
		*columns = SUN4U_CONSOLE_COLUMNS;
		if (screen_on)
			*columns = screen_columns;
	}

	/* Reports the height when the caller asked for it. */
	if (rows != NULL) {
		*rows = SUN4U_CONSOLE_ROWS;
		if (screen_on)
			*rows = screen_rows;
	}
}

/* Sends one character to the serial port and the screen. */
static void
console_putc(
	int character)
{
	/* Without the framebuffer, the hardware layer shows the console. */
	if (!screen_on) {
		kern_console_early_putc(character);
		return;
	}

	/* Writes the grid and the serial port directly. */
	console_kernel_putc(character);
}

/* Drops a positioned write, which a teletype cannot place. */
static void
console_write(
	unsigned row,
	unsigned column,
	uint8_t attribute,
	const char *utf8)
{
	(void)row;
	(void)column;
	(void)attribute;
	(void)utf8;
}

/* Leaves the terminal as it is: the teletype has nothing to blank. */
static void
console_clear(
	void)
{
}

/* Accepts a position inside the grid without moving anything. */
static int
console_set_cursor(
	unsigned row,
	unsigned column)
{
	unsigned rows;
	unsigned columns;

	/* Reads the grid's size. */
	console_get_size(&columns, &rows);

	/* Refuses a row below the grid. */
	if (row >= rows)
		return 0;

	/* Refuses a column right of the grid. */
	if (column >= columns)
		return 0;

	/* Succeeded: the position is inside the grid. */
	return 1;
}

/* Reports the home position and a shown cursor. */
static void
console_get_cursor(
	unsigned *row,
	unsigned *column,
	int *visible)
{
	/* Reports the home row when the caller asked for it. */
	if (row != NULL)
		*row = 0;

	/* Reports the home column when the caller asked for it. */
	if (column != NULL)
		*column = 0;

	/* Reports a shown cursor when the caller asked for it. */
	if (visible != NULL)
		*visible = 1;
}

/* Ignores the request: the console draws its own cursor. */
static void
console_show_cursor(
	int visible)
{
	(void)visible;
}

/* Does nothing for an operation a teletype does not have. */
static void
console_nothing(
	void)
{
}

/*
 * Moves what the serial port received into the console.
 *
 * A terminal sends a carriage return for Enter and a delete for backspace;
 * the line discipline expects a newline and a backspace.
 */
static void
console_input_worker(
	void *argument)
{
	unsigned drained;
	uint8_t status;
	uint8_t byte;
	uint64_t wake;

	/* The worker is given no argument. */
	(void)argument;

	/* Reads the port for the life of the system. */
	for (;;) {
		/* Moves at most one batch of received bytes into the console. */
		for (drained = 0; drained < SUN4U_CONSOLE_DRAIN; drained++) {
			/* Stops when nothing was received. */
			status = kern_io_in8(serial_port + SUN4U_UART_LSR);
			if ((status & SUN4U_UART_LSR_DR) == 0)
				break;

			/* Takes the byte and translates Enter and backspace. */
			byte = kern_io_in8(serial_port + SUN4U_UART_RBR);
			if (byte == SUN4U_CONSOLE_RETURN) {
				byte = (uint8_t)'\n';
			} else if (byte == SUN4U_CONSOLE_DELETE) {
				byte = SUN4U_CONSOLE_BACKSPACE;
			}

			/* Hands the byte to the console's line discipline. */
			tty_console_input_byte(byte);
		}

		/* Waits until the next look at the port. */
		wake = sched_ticks() + kern_ms_to_ticks(SUN4U_CONSOLE_POLL_MS);
		sched_sleep(wake);
	}
}

/*
 * Shows the console for a fatal stop, even over a graphics owner.
 *
 * Interrupts are off and the stopping code may hold any lock, so the grid
 * is drawn without waiting for a writer.
 */
static void
console_reveal_fatal(
	void)
{
	int ended;

	/* Nothing to show without the framebuffer: the serial port has it all. */
	if (!screen_on)
		return;

	/* Nothing is shown once the console has left the screen: it is the GPU's. */
	ended = __atomic_load_n(&screen_ended, __ATOMIC_ACQUIRE);
	if (ended)
		return;

	/* Takes the screen back and draws the grid over a cleared screen. */
	screen_suspended = 0;
	screen_repaint();
}

/* Stops drawing the grid while /dev/graphics owns the screen. */
static void
console_suspend(
	void)
{
	unsigned long enabled;
	int owned;

	/* Nothing is drawn without the framebuffer. */
	if (!screen_on)
		return;

	/* Marks the screen taken, apart from a writer. */
	enabled = screen_enter(&owned);

	screen_suspended = 1;

	screen_leave(enabled, owned);
}

/* Draws the grid again, over a cleared screen, once graphics are left. */
static void
console_resume(
	void)
{
	unsigned long enabled;
	int owned;

	/* Nothing is drawn without the framebuffer. */
	if (!screen_on)
		return;

	/* Gives the screen back to the grid and repaints all of it, unless the console has left it. */
	enabled = screen_enter(&owned);

	if (!screen_ended) {
		screen_suspended = 0;
		screen_repaint();
	}

	screen_leave(enabled, owned);
}

/*
 * Ends the console's use of the screen for good: the grid is never drawn
 * again (it still follows the characters, which still reach the serial
 * port), the kernel's characters are taken over from the hardware layer's
 * early console if they were not yet, and /dev/graphics and the firmware
 * framebuffer give the screen and their mappings up.  Returns the first
 * error they return, or EBUSY when another character path, which may draw
 * anywhere on the screen, was published and the caller gives memory back.
 * Thread context, no lock held.
 */
static int
console_end(
	const struct kern_text_range *ranges,
	unsigned count)
{
	const uint64_t *port;
	void (*published)(int);
	unsigned long enabled;
	int owned;
	int foreign;
	int graphics_error;
	int framebuffer_error;

	/* Finds the serial port, which a console that never drew the screen has not looked up. */
	port = kern_boot_handoff("sun4u.serial.port");

	/*
	 * Stops drawing the grid for good, apart from any writer: once this
	 * section is left no writer is drawing, and none draws again.
	 */
	enabled = screen_enter(&owned);

	/* A suspend that lasts: a resume or a fatal stop's reveal sees the end. */
	if (!screen_ended) {
		screen_suspended = 1;
		__atomic_store_n(&screen_ended, 1, __ATOMIC_RELEASE);
	}

	/* The characters keep reaching the serial port the hardware layer wrote. */
	if (serial_port == 0 &&
	    port != NULL &&
	    *port <= UINT16_MAX)
		serial_port = (uint16_t)*port;

	screen_leave(enabled, owned);

	/* The kernel's characters come here, not to the hardware layer's early console, from now on. */
	foreign = 0;
	published = __atomic_load_n(&kernel_putc, __ATOMIC_ACQUIRE);
	if (published == NULL) {
		__atomic_store_n(&kernel_putc, console_kernel_putc, __ATOMIC_RELEASE);
	} else if (published != console_kernel_putc) {
		foreign = 1;
	}

	/* /dev/graphics drops its mode, refuses every request, and has its backend give its mappings up. */
	graphics_error = 0;
#if CONFIG_DRIVER_GRAPHICS_DEVICE
	graphics_error = drv_graphics_device_end(ranges, count);
#endif

	/* The framebuffer's own mapping goes, whichever backend /dev/graphics chose. */
	framebuffer_error = drv_sun4u_fb_end(ranges, count);

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
 * Writes one character of the console or of the kernel to the serial
 * port and the grid.
 *
 * This is the published kernel_putc: it calls neither the log nor the HAL,
 * so nothing comes back into it.
 */
static void
console_kernel_putc(
	int character)
{
	unsigned long enabled;
	int owned;

	/* Writes the serial port and the grid as one step; a console without a framebuffer has no grid. */
	enabled = screen_enter(&owned);

	serial_write(character);
	if (screen_on)
		screen_putc(character);

	screen_leave(enabled, owned);
}

/*
 * Writes one byte to the serial port, a newline after a carriage return;
 * a byte the transmitter never takes is dropped.
 */
static void
serial_write(
	int character)
{
	unsigned spins;
	uint8_t status;

	/* Nothing to write without the port. */
	if (serial_port == 0)
		return;

	/* A newline is preceded by a carriage return. */
	if (character == '\n')
		serial_write('\r');

	/* Waits for room in the transmitter. */
	for (spins = 0; spins < SUN4U_UART_SPIN_LIMIT; spins++) {
		status = kern_io_in8(serial_port + SUN4U_UART_LSR);
		if ((status & SUN4U_UART_LSR_THRE) != 0)
			break;
	}

	/* Drops the byte on a port that never drains. */
	if (spins == SUN4U_UART_SPIN_LIMIT)
		return;

	/* Hands the byte to the transmitter. */
	kern_io_out8(serial_port + SUN4U_UART_THR, (uint8_t)character);
}

/*
 * Starts a change of the grid: interrupts off, and the writer's mark set.
 * Reports whether interrupts were on, and in owned whether this call set
 * the mark (a nested writer did not).
 */
static unsigned long
screen_enter(
	int *owned)
{
	unsigned long enabled;
	unsigned busy;

	/* Keeps interrupt handlers out; the port has one processor. */
	enabled = kern_irq_disable();

	/* Sets the mark; one already set is this processor coming back in. */
	busy = __atomic_exchange_n(&screen_busy, 1U, __ATOMIC_ACQUIRE);
	*owned = 0;
	if (busy == 0)
		*owned = 1;

	/* Succeeded: the caller may change the grid. */
	return enabled;
}

/* Ends a change of the grid started by screen_enter(). */
static void
screen_leave(
	unsigned long enabled,
	int owned)
{
	/* Clears the mark only when this writer set it. */
	if (owned)
		__atomic_store_n(&screen_busy, 0U, __ATOMIC_RELEASE);

	/* Lets interrupts in again when they were on. */
	if (enabled)
		kern_irq_enable();
}

/* Puts one character at the cursor. */
static void
screen_putc(
	int character)
{
	unsigned column;

	/* A newline moves to the next line. */
	if (character == '\n') {
		screen_newline();
		return;
	}

	/* A carriage return moves to the line's start. */
	if (character == '\r') {
		column = screen_column;
		screen_column = 0;
		if (column < screen_columns)
			screen_draw_cell(screen_row, column);
		screen_draw_cell(screen_row, screen_column);
		return;
	}

	/* A backspace steps back and erases within the line. */
	if (character == '\b') {
		column = screen_column;
		if (screen_column > 0)
			screen_column--;
		screen_grid[screen_row][screen_column] = ' ';
		if (column < screen_columns)
			screen_draw_cell(screen_row, column);
		screen_draw_cell(screen_row, screen_column);
		return;
	}

	/* A tab moves to the next eight-column stop, wrapping past the row's end. */
	if (character == '\t') {
		column = screen_column;
		screen_column = (screen_column + 8U) & ~7U;
		if (column < screen_columns)
			screen_draw_cell(screen_row, column);
		if (screen_column >= screen_columns) {
			screen_newline();
			return;
		}
		screen_draw_cell(screen_row, screen_column);
		return;
	}

	/* Wraps before writing past the end of the row. */
	if (screen_column >= screen_columns)
		screen_newline();

	/* Stores a printable character, or a question mark for anything else. */
	screen_grid[screen_row][screen_column] = '?';
	if (character >= 0x20 && character < 0x7f)
		screen_grid[screen_row][screen_column] = (uint8_t)character;

	/* Moves the cursor on and draws the written cell and the cursor's. */
	column = screen_column;
	screen_column++;
	screen_draw_cell(screen_row, column);
	if (screen_column < screen_columns)
		screen_draw_cell(screen_row, screen_column);
}

/* Moves the cursor to the start of the next line, scrolling at the bottom. */
static void
screen_newline(
	void)
{
	unsigned old_row;
	unsigned old_column;
	unsigned row;
	unsigned column;

	/* Moves down when there is a row below. */
	old_row = screen_row;
	old_column = screen_column;
	screen_column = 0;
	if (screen_row + 1U < screen_rows) {
		/* The cell the cursor left loses its cursor, then the new one gains it. */
		screen_row++;
		if (old_column < screen_columns)
			screen_draw_cell(old_row, old_column);
		screen_draw_cell(screen_row, screen_column);
		return;
	}

	/* Shifts the rows up and blanks the bottom one. */
	for (row = 1; row < screen_rows; row++) {
		for (column = 0; column < screen_columns; column++)
			screen_grid[row - 1U][column] = screen_grid[row][column];
	}
	for (column = 0; column < screen_columns; column++)
		screen_grid[screen_rows - 1U][column] = 0;

	/* Draws the cells the scroll changed. */
	screen_draw_all();
}

/* Draws one cell when it differs from what the screen shows. */
static void
screen_draw_cell(
	unsigned row,
	unsigned column)
{
	const uint8_t *glyph;
	uint16_t wanted;
	uint8_t character;
	unsigned x;
	unsigned y;

	/* A cell never written is a blank; the cursor's cell carries its mark. */
	character = screen_grid[row][column];
	if (character == 0)
		character = ' ';
	wanted = character;
	if (row == screen_row && column == screen_column)
		wanted |= SUN4U_CONSOLE_DRAWN_CURSOR;

	/* Draws nothing while graphics own the screen, or for an unchanged cell. */
	if (screen_suspended || screen_drawn[row][column] == wanted)
		return;

	/* Draws the glyph, and the cursor's two bottom lines over it. */
	x = column * SUN4U_CONSOLE_CELL_WIDTH;
	y = row * SUN4U_CONSOLE_CELL_HEIGHT;
	glyph = &drv_pcat_vgafont16[(unsigned)character * PCAT_VGAFONT_HEIGHT];
	drv_sun4u_fb_glyph(x, y, glyph, SUN4U_CONSOLE_FOREGROUND, SUN4U_CONSOLE_BACKGROUND);
	if ((wanted & SUN4U_CONSOLE_DRAWN_CURSOR) != 0) {
		drv_sun4u_fb_fill(x, y + SUN4U_CONSOLE_CELL_HEIGHT - 2U,
				  SUN4U_CONSOLE_CELL_WIDTH, 2U, SUN4U_CONSOLE_CURSOR);
	}

	/* Records what the cell shows now. */
	screen_drawn[row][column] = wanted;
}

/* Draws every cell that differs from what the screen shows. */
static void
screen_draw_all(
	void)
{
	unsigned row;
	unsigned column;

	/* Each row, left to right. */
	for (row = 0; row < screen_rows; row++) {
		for (column = 0; column < screen_columns; column++)
			screen_draw_cell(row, column);
	}
}

/*
 * Clears the whole screen and draws the grid on it, unless graphics own
 * the screen.
 */
static void
screen_repaint(
	void)
{
	unsigned width;
	unsigned height;
	unsigned row;
	unsigned column;
	uint16_t blank;

	/* Leaves a screen graphics own alone. */
	if (screen_suspended)
		return;

	/* Paints the whole framebuffer the background color. */
	drv_sun4u_fb_get_size(&width, &height);
	drv_sun4u_fb_fill(0, 0, width, height, SUN4U_CONSOLE_BACKGROUND);

	/* Every cell now shows a blank without the cursor. */
	blank = ' ';
	for (row = 0; row < screen_rows; row++) {
		for (column = 0; column < screen_columns; column++)
			screen_drawn[row][column] = blank;
	}

	/* Draws the cells that are not blank, and the cursor. */
	screen_draw_all();
}
