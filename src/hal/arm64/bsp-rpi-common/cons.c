/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Raspberry Pi early console.
 *
 * This exists so the HAL and the early part of kernel start-up have
 * somewhere to print. It is output only, and it writes to both the UART
 * and the framebuffer: the serial line is what a developer watches, and
 * the framebuffer is what a display shows, so a message that matters
 * before the kernel has a console device goes to both. Once the kernel
 * publishes a console through kernel_putc, every character goes there
 * instead and this code stops touching either.
 *
 * Input is not handled here. The UART's receive side belongs to a
 * driver, which turns its bytes into input events like any other
 * keyboard.
 *
 * The serial line is used only when the build asks for it
 * (CONFIG_RPI4_SERIAL_CONSOLE or CONFIG_RPI5_SERIAL_CONSOLE, off by
 * default); otherwise the UART is never set up or written.
 */

#include <hal/hal.h>
#include "../bsp.h"
#include "../watch.h"
#include "framebuffer.h"
#include "uart.h"

/* The console's grid of text cells. */
#define CONS_COLUMNS	80U
#define CONS_ROWS	25U

/*
 * Whether the serial line carries the console.  The build defines
 * RPI4_SERIAL_CONSOLE for either board's serial-console option.
 */
#ifdef RPI4_SERIAL_CONSOLE
#define CONS_SERIAL	1
#else
#define CONS_SERIAL	0
#endif

/* Light grey on black, the default text attribute. */
#define CONS_ATTRIBUTE	0x07U

/*
 * One text cell of the console as it should look: a character and its
 * colour attribute.  The shadow grid keeps every cell, so the screen can
 * be repainted after scrolling.
 */
struct cell {
	uint8_t character;
	uint8_t attribute;
};

/*
 * The text of the whole screen.  Written only under the output lock;
 * blank cells from prekern_bsp_cons_init() on.
 */
static struct cell shadow[CONS_ROWS][CONS_COLUMNS];

/* Where the next character goes; changed only under the output lock. */
static unsigned cursor_row;
static unsigned cursor_column;

/*
 * The output lock: nonzero while one CPU renders.  Taken with interrupts
 * off, so output from an interrupt cannot interleave with a character
 * half drawn.
 */
static volatile unsigned output_lock;

static bool output_enter(void);
static void output_leave(bool interrupts_enabled);
static void draw_locked(unsigned row, unsigned column);
static void update_cursor_locked(void);
static void clear_row_locked(unsigned row);
static void clear_locked(void);
static void scroll_locked(void);
static void newline_locked(void);
static void putc_locked(int character);

/*
 * Writes one character to the early console, or to the kernel console
 * once the kernel has published one.
 */
void
hal_putc(
	int character)
{
	void (*kernel_output)(int c);
	bool interrupts_enabled;

	/* Keeps the character for a stall report, which shows the log's last lines. */
	arm64_watch_note_char(character);

	/* Delegates to the kernel console after the handover. */
	kernel_output = __atomic_load_n(&kernel_putc, __ATOMIC_ACQUIRE);
	if (kernel_output != NULL) {
		kernel_output(character);
		return;
	}

	/* Serializes character rendering with all console output. */
	interrupts_enabled = output_enter();

	putc_locked(character);

	output_leave(interrupts_enabled);
}

/*
 * Prepares the Raspberry Pi early console.
 *
 * This runs before kernel_entry(), so it may not allocate or block.
 */
void
prekern_bsp_cons_init(
	void)
{
	/* Brings up the serial line the HAL prints to, when the build uses it. */
	if (CONS_SERIAL)
		rpi_uart_init();

	/* Starts from a known screen and cursor position. */
	output_lock = 0;
	cursor_row = 0;
	cursor_column = 0;
	clear_locked();
}

/*
 * Leaves the UART receive interrupt masked for its driver.
 *
 * The early console does not read the serial line. It stays masked
 * until the input driver registers, so a byte that arrives during
 * start-up cannot raise an interrupt nobody handles.
 */
void
prekern_bsp_cons_irq_init(
	void)
{
	const struct rpi_fdt_info *info;

	/* Finds the UART's interrupt in the device tree's record. */
	info = rpi_boot_info();

	/* Keeps the line quiet until its driver claims it. */
	if (info != NULL && info->uart_irq != 0)
		hal_irq_mask((int)info->uart_irq);
}

/*
 * Gives the framebuffer up to the kernel's console, which took the early
 * console's window of it over and removes it.  The kernel prints through
 * its own console by then, so nothing here paints again; the view goes
 * under the output lock all the same, so that a character that began
 * before the hand-over finishes before the window goes.
 */
void
rpi_cons_framebuffer_release(
	void)
{
	bool interrupts_enabled;

	/* Waits for a character being painted, then keeps the next ones off the framebuffer. */
	interrupts_enabled = output_enter();

	rpi_framebuffer_forget();

	output_leave(interrupts_enabled);
}

/* Acquires the output lock with interrupts disabled. */
static bool
output_enter(
	void)
{
	bool interrupts_enabled;
	unsigned previous;
	unsigned tries;

	/* Keeps rendering atomic against interrupt-context output. */
	interrupts_enabled = hal_irq_disable();

	/* Spins until this CPU owns the output lock, now and then looking for its own stall. */
	tries = 0;
	for (;;) {
		previous = __atomic_exchange_n(&output_lock, 1U, __ATOMIC_ACQUIRE);
		if (previous == 0U)
			break;

		/* Lets the other hardware thread run while the owner renders. */
		__asm__ volatile("yield" : : : "memory");

		/* Now and then checks whether this CPU itself has stalled. */
		tries++;
		if ((tries % ARM64_WATCH_SPIN_INTERVAL) == 0)
			arm64_watch_self_check();
	}

	/* Succeeded: the interrupt state the release has to restore. */
	return interrupts_enabled;
}

/* Releases the output lock and restores the caller's interrupt state. */
static void
output_leave(
	bool interrupts_enabled)
{
	/* Publishes every pending cell write before releasing. */
	__atomic_store_n(&output_lock, 0U, __ATOMIC_RELEASE);

	/* Restores interrupts only when they were previously enabled. */
	if (interrupts_enabled)
		hal_irq_enable();
}

/* Paints one stored cell onto the framebuffer. */
static void
draw_locked(
	unsigned row,
	unsigned column)
{
	const struct cell *c;

	/* A cell outside the grid has nothing to paint. */
	if (row >= CONS_ROWS || column >= CONS_COLUMNS)
		return;

	/* Paints the cell's character in its attribute. */
	c = &shadow[row][column];
	rpi_framebuffer_cell(row, column, c->character, c->attribute);
}

/* Draws the cursor at the tracked position. */
static void
update_cursor_locked(
	void)
{
	/* Draws the cursor under the cell the next character goes to. */
	rpi_framebuffer_cursor(cursor_row, cursor_column, 1);
}

/* Fills one row with blanks. */
static void
clear_row_locked(
	unsigned row)
{
	unsigned column;

	/* Rejects a row outside the grid. */
	if (row >= CONS_ROWS)
		return;

	/* Blanks and repaints every cell of the row. */
	for (column = 0; column < CONS_COLUMNS; column++) {
		shadow[row][column].character = ' ';
		shadow[row][column].attribute = CONS_ATTRIBUTE;
		draw_locked(row, column);
	}
}

/* Clears the screen and homes the cursor. */
static void
clear_locked(
	void)
{
	unsigned row;

	/* Blanks every row in order. */
	for (row = 0; row < CONS_ROWS; row++)
		clear_row_locked(row);

	/* Returns the cursor to the top-left cell. */
	cursor_row = 0;
	cursor_column = 0;
	update_cursor_locked();
}

/* Scrolls the screen up by one row. */
static void
scroll_locked(
	void)
{
	unsigned row;
	unsigned column;

	/* Shifts the stored rows up by one. */
	for (row = 1; row < CONS_ROWS; row++) {
		for (column = 0; column < CONS_COLUMNS; column++)
			shadow[row - 1U][column] = shadow[row][column];
	}

	/* Repaints every row the shift moved. */
	for (row = 0; row < CONS_ROWS - 1U; row++) {
		for (column = 0; column < CONS_COLUMNS; column++)
			draw_locked(row, column);
	}

	/* Blanks the row exposed at the bottom. */
	clear_row_locked(CONS_ROWS - 1U);
}

/* Advances the cursor to the start of the next line. */
static void
newline_locked(
	void)
{
	/* Returns to the first column of the following row. */
	cursor_column = 0;
	cursor_row++;

	/* Scrolls instead of leaving the last row. */
	if (cursor_row >= CONS_ROWS) {
		scroll_locked();
		cursor_row = CONS_ROWS - 1U;
	}
}

/* Renders one character at the cursor. */
static void
putc_locked(
	int character)
{
	uint8_t stored;

	/* The serial line, when the build uses it, carries every byte, control characters too. */
	if (CONS_SERIAL)
		rpi_uart_putc(character);

	/* Removes the cursor before the cell under it changes. */
	draw_locked(cursor_row, cursor_column);

	/* Handles the line and column control characters. */
	if (character == '\n') {
		/* A new line moves to the start of the next row. */
		newline_locked();
		update_cursor_locked();
		return;
	} else if (character == '\r') {
		/* A carriage return moves to the start of the row. */
		cursor_column = 0;
		update_cursor_locked();
		return;
	} else if (character == '\b') {
		/* Steps back and erases within the current row only. */
		if (cursor_column > 0)
			cursor_column--;
		shadow[cursor_row][cursor_column].character = ' ';
		draw_locked(cursor_row, cursor_column);
		update_cursor_locked();
		return;
	} else if (character == '\t') {
		/* Advances to the next eight-column stop. */
		cursor_column = (cursor_column + 8U) & ~7U;

		/* Wraps when the stop leaves the row. */
		if (cursor_column >= CONS_COLUMNS)
			newline_locked();
		update_cursor_locked();
		return;
	}

	/* Wraps before writing past the end of the row. */
	if (cursor_column >= CONS_COLUMNS)
		newline_locked();

	/* Keeps a printable character, and substitutes anything else. */
	stored = '?';
	if (character >= 0x20 && character < 0x7f)
		stored = (uint8_t)character;

	/* Stores and paints the cell, then moves past it. */
	shadow[cursor_row][cursor_column].character = stored;
	shadow[cursor_row][cursor_column].attribute = CONS_ATTRIBUTE;
	draw_locked(cursor_row, cursor_column);
	cursor_column++;

	/* Wraps to the next line at the end of the row. */
	if (cursor_column >= CONS_COLUMNS)
		newline_locked();
	update_cursor_locked();
}
