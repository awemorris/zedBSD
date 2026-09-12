/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Raspberry Pi 4 early console.
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
 */

#include <hal/hal.h>

#include "../bsp.h"
#include "framebuffer.h"
#include "uart.h"

#define CONS_COLUMNS	80U
#define CONS_ROWS	25U

/* Light grey on black, the default text attribute. */
#define CONS_ATTRIBUTE	0x07U

struct cell {
	uint8_t character;
	uint8_t attribute;
};

static struct cell shadow[CONS_ROWS][CONS_COLUMNS];
static unsigned cursor_row;
static unsigned cursor_column;
static volatile unsigned output_lock;
static int output_suspended;

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
 * Acquires console-output ownership.
 *
 * The return value is the caller's interrupt state, which the matching
 * release needs; it is opaque to the caller.
 */
uint32_t
rpi4_cons_output_begin(
	void)
{
	/* Reports the saved interrupt state as the release token. */
	return output_enter() ? 1U : 0U;
}

/*
 * Releases console-output ownership.
 */
void
rpi4_cons_output_end(
	uint32_t token)
{
	/* Restores the interrupt state the matching acquire saved. */
	output_leave(token != 0);
}

/*
 * Stops and resumes writes to the framebuffer.
 *
 * A display driver that takes the hardware calls this so late HAL
 * diagnostics do not corrupt the screen. The UART keeps working, since
 * nothing else owns it.
 */
void
rpi4_cons_suspend_output(
	void)
{
	bool interrupts_enabled;

	/* Publishes the suspension under the output lock. */
	interrupts_enabled = output_enter();
	output_suspended = 1;
	output_leave(interrupts_enabled);
}

void
rpi4_cons_resume_output(
	void)
{
	bool interrupts_enabled;

	/* Restores framebuffer output and repaints a known state. */
	interrupts_enabled = output_enter();
	output_suspended = 0;
	clear_locked();
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
	/* Brings up the serial line the HAL prints to. */
	rpi4_uart_init();

	/* Starts from a known screen and cursor position. */
	output_lock = 0;
	output_suspended = 0;
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
	const struct rpi4_fdt_info *info;

	/* Keeps the line quiet until its driver claims it. */
	info = rpi4_boot_info();
	if (info != NULL && info->uart_irq != 0)
		hal_irq_mask((int)info->uart_irq);
}

/* Acquires the output lock with interrupts disabled. */
static bool
output_enter(
	void)
{
	bool interrupts_enabled;

	/* Keeps rendering atomic against interrupt-context output. */
	interrupts_enabled = hal_irq_disable();

	/* Spins until this CPU owns the output lock. */
	while (__atomic_exchange_n(&output_lock, 1U, __ATOMIC_ACQUIRE) != 0U)
		__asm__ volatile("yield" : : : "memory");

	/* Reports the interrupt state the release has to restore. */
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

	/* Leaves the screen alone while another driver owns it. */
	if (output_suspended || row >= CONS_ROWS || column >= CONS_COLUMNS)
		return;
	c = &shadow[row][column];
	rpi4_framebuffer_cell(row, column, c->character, c->attribute);
}

/* Draws the cursor at the tracked position. */
static void
update_cursor_locked(
	void)
{
	/* Leaves the screen alone while another driver owns it. */
	if (output_suspended)
		return;
	rpi4_framebuffer_cursor(cursor_row, cursor_column, 1);
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
	/* The serial line carries every byte, control characters too. */
	rpi4_uart_putc(character);

	/* Removes the cursor before the cell under it changes. */
	draw_locked(cursor_row, cursor_column);

	/* Handles the line and column control characters. */
	if (character == '\n') {
		newline_locked();
		update_cursor_locked();
		return;
	}
	if (character == '\r') {
		cursor_column = 0;
		update_cursor_locked();
		return;
	}
	if (character == '\b') {
		/* Steps back and erases within the current row only. */
		if (cursor_column > 0)
			cursor_column--;
		shadow[cursor_row][cursor_column].character = ' ';
		draw_locked(cursor_row, cursor_column);
		update_cursor_locked();
		return;
	}
	if (character == '\t') {
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

	/* Stores one printable cell, substituting anything else. */
	shadow[cursor_row][cursor_column].character =
	    (uint8_t)(character >= 0x20 && character < 0x7f ? character : '?');
	shadow[cursor_row][cursor_column].attribute = CONS_ATTRIBUTE;
	draw_locked(cursor_row, cursor_column);
	cursor_column++;

	/* Wraps to the next line at the end of the row. */
	if (cursor_column >= CONS_COLUMNS)
		newline_locked();
	update_cursor_locked();
}
