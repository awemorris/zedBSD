/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PC/AT early console.
 *
 * This writes to VGA text memory and exists so the HAL and the early
 * part of kernel start-up have somewhere to print. It is output only.
 * Once the kernel publishes a console through kernel_putc, every
 * character goes there instead and this code stops touching the screen.
 * Keyboard input is not handled here; the 8042 driver owns that.
 */

#include <hal/hal.h>

#include <string.h>

#include "../asm.h"
#include "../defs.h"
#include "../i386.h"
#include "../irq.h"

#define VGA_MEMORY ((volatile uint16_t *)(SYS_START + 0x000b8000U))
#define VGA_INDEX 0x3d4U
#define VGA_DATA 0x3d5U

#define CONS_COLUMNS 80U
#define CONS_ROWS 25U
#define CONS_ATTRIBUTE 0x07U

static unsigned cursor_row;
static unsigned cursor_column;
static volatile unsigned output_lock;
static int output_suspended;

static bool output_enter(void);
static void output_leave(bool interrupts_enabled);
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
pcat_cons_output_begin(
	void)
{
	/* Reports the saved interrupt state as the release token. */
	return output_enter() ? 1U : 0U;
}

/*
 * Releases console-output ownership.
 */
void
pcat_cons_output_end(
	uint32_t token)
{
	/* Restores the interrupt state the matching acquire saved. */
	output_leave(token != 0);
}

/*
 * Stops and resumes writes to text memory.
 *
 * A display driver that takes the hardware into a graphics mode calls
 * this so late HAL diagnostics do not corrupt the screen.
 */
void
pcat_cons_suspend_output(
	void)
{
	bool interrupts_enabled;

	/* Publishes the suspension under the output lock. */
	interrupts_enabled = output_enter();
	output_suspended = 1;
	output_leave(interrupts_enabled);
}

void
pcat_cons_resume_output(
	void)
{
	bool interrupts_enabled;

	/* Restores text output and repaints a known state. */
	interrupts_enabled = output_enter();
	output_suspended = 0;
	clear_locked();
	output_leave(interrupts_enabled);
}

/*
 * Prepares the PC/AT early console.
 *
 * This runs before kernel_entry(), so it may not allocate or block.
 */
void
prekern_bsp_cons_init(
	void)
{
	/* Starts from a known screen and cursor position. */
	output_lock = 0;
	output_suspended = 0;
	cursor_row = 0;
	cursor_column = 0;
	clear_locked();
}

/*
 * Leaves the keyboard interrupt masked for its driver.
 *
 * The early console does not read the keyboard. The line stays masked
 * until the 8042 driver registers, so a key pressed during start-up
 * cannot raise an interrupt nobody handles.
 */
void
prekern_bsp_cons_irq_init(
	void)
{
	/* Keeps the keyboard line quiet until its driver claims it. */
	hal_irq_mask(IRQ_KEYBOARD);
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
		__asm__ volatile("pause" : : : "memory");

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

/* Moves the hardware cursor to the tracked position. */
static void
update_cursor_locked(
	void)
{
	unsigned offset;

	/* Leaves the hardware alone while another driver owns the screen. */
	if (output_suspended)
		return;

	/* Writes the linear cell offset to the CRTC cursor registers. */
	offset = cursor_row * CONS_COLUMNS + cursor_column;
	asm_outb(VGA_INDEX, 0x0eU);
	asm_outb(VGA_DATA, (uint8_t)(offset >> 8));
	asm_outb(VGA_INDEX, 0x0fU);
	asm_outb(VGA_DATA, (uint8_t)(offset & 0xffU));
}

/* Fills one row with blanks. */
static void
clear_row_locked(
	unsigned row)
{
	unsigned column;

	/* Suppresses text-memory writes during graphics ownership. */
	if (output_suspended)
		return;

	/* Publishes a blank cell across the whole row. */
	for (column = 0; column < CONS_COLUMNS; column++) {
		VGA_MEMORY[row * CONS_COLUMNS + column] =
		    (uint16_t)(' ' | ((uint16_t)CONS_ATTRIBUTE << 8));
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

	/* Suppresses text-memory writes during graphics ownership. */
	if (output_suspended)
		return;

	/* Copies every row up one position. */
	for (row = 1; row < CONS_ROWS; row++) {
		for (column = 0; column < CONS_COLUMNS; column++) {
			VGA_MEMORY[(row - 1) * CONS_COLUMNS + column] =
			    VGA_MEMORY[row * CONS_COLUMNS + column];
		}
	}

	/* Blanks the row exposed at the bottom. */
	clear_row_locked(CONS_ROWS - 1);
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
		cursor_row = CONS_ROWS - 1;
	}
}

/* Renders one character at the cursor. */
static void
putc_locked(
	int character)
{
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
		/* Steps back within the current row only. */
		if (cursor_column > 0)
			cursor_column--;
		update_cursor_locked();
		return;
	}
	if (character == '\t') {
		/* Advances to the next eight-column stop. */
		cursor_column = (cursor_column + 8U) & ~7U;

		/* Wraps to the next line when the stop leaves the row. */
		if (cursor_column >= CONS_COLUMNS)
			newline_locked();
		update_cursor_locked();
		return;
	}

	/* Publishes the character cell. */
	if (!output_suspended) {
		VGA_MEMORY[cursor_row * CONS_COLUMNS + cursor_column] =
		    (uint16_t)((uint8_t)character |
		    ((uint16_t)CONS_ATTRIBUTE << 8));
	}
	cursor_column++;

	/* Wraps to the next line at the end of the row. */
	if (cursor_column >= CONS_COLUMNS)
		newline_locked();
	update_cursor_locked();
}
