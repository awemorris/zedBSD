/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PC-98 early console.
 *
 * This writes to text memory and exists so the HAL and the early part of
 * kernel start-up have somewhere to print. It is output only, and ASCII
 * only: the messages that reach it are diagnostics, and the Japanese
 * text path belongs to the display driver that takes over later. Once
 * the kernel publishes a console through kernel_putc, every character
 * goes there instead and this code stops touching the screen.
 *
 * Keyboard input is not handled here. The 8251 keyboard and the bus
 * mouse are separate devices on separate interrupts, and each has its
 * own driver.
 */

#include <hal/hal.h>

#include <string.h>

#include "../defs.h"
#include "../irq.h"

/* Characters are 16-bit words; their attributes live in a second plane. */
#define CONS_TEXT_VRAM		((volatile uint16_t *)0x800a0000)
#define CONS_ATTRIBUTE_VRAM	((volatile uint8_t *)0x800a2000)

#define CONS_COLUMNS		80U
#define CONS_ROWS		25U

/* Green, red and blue lit, and the cell marked visible. */
#define CONS_ATTRIBUTE		0xe1U

/* The uPD7220 status and command ports, and its cursor-address command. */
#define GDC_STATUS		0x0060U
#define GDC_COMMAND		0x0062U
#define GDC_PARAMETER		0x0060U
#define GDC_CSRW		0x49U
#define GDC_CSRFORM		0x4bU

/*
 * Cursor form. Bit 7 of the first parameter shows the cursor; its low
 * five bits are the scan lines per character row minus one, which the
 * sixteen-line font fixes at 15. The display logic reads that field
 * too, so a wrong value repeats the screen instead of only losing the
 * cursor.
 */
#define GDC_CSRFORM_SHOW	0x8fU
#define GDC_CSRFORM_TOP		0x20U
#define GDC_CSRFORM_BOTTOM	0x7bU
#define GDC_FIFO_FULL		0x02U

static unsigned cursor_row;
static unsigned cursor_column;
static volatile unsigned output_lock;
static int output_suspended;

static bool output_enter(void);
static void output_leave(bool interrupts_enabled);
static int gdc_write(uint16_t port, uint8_t value);
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
pc98_cons_output_begin(
	void)
{
	/* Reports the saved interrupt state as the release token. */
	return output_enter() ? 1U : 0U;
}

/*
 * Releases console-output ownership.
 */
void
pc98_cons_output_end(
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
pc98_cons_suspend_output(
	void)
{
	bool interrupts_enabled;

	/* Publishes the suspension under the output lock. */
	interrupts_enabled = output_enter();
	output_suspended = 1;
	output_leave(interrupts_enabled);
}

void
pc98_cons_resume_output(
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
 * Prepares the PC-98 early console.
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
 * until the 8251 driver registers, so a key pressed during start-up
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
		status = hal_io_inp8(GDC_STATUS);
		if ((status & GDC_FIFO_FULL) == 0U)
			break;
	}

	/* Reports failure without issuing a byte after timeout. */
	if (timeout == 0U)
		return 0;
	hal_io_outp8(port, value);

	/* Reports one completed port write. */
	return 1;
}

/* Moves the hardware cursor to the tracked position. */
static void
update_cursor_locked(
	void)
{
	unsigned address;

	/* Leaves the hardware alone while another driver owns the screen. */
	if (output_suspended)
		return;

	/* States the row height and shows the cursor. */
	if (!gdc_write(GDC_COMMAND, GDC_CSRFORM))
		return;
	if (!gdc_write(GDC_PARAMETER, GDC_CSRFORM_SHOW))
		return;
	if (!gdc_write(GDC_PARAMETER, GDC_CSRFORM_TOP))
		return;
	if (!gdc_write(GDC_PARAMETER, GDC_CSRFORM_BOTTOM))
		return;

	/* Writes the cell address as the two-parameter cursor command. */
	address = cursor_row * CONS_COLUMNS + cursor_column;
	if (!gdc_write(GDC_COMMAND, GDC_CSRW))
		return;
	if (!gdc_write(GDC_PARAMETER, (uint8_t)(address & 0xffU)))
		return;
	(void)gdc_write(GDC_PARAMETER, (uint8_t)((address >> 8) & 0xffU));
}

/* Fills one row with blanks. */
static void
clear_row_locked(
	unsigned row)
{
	unsigned column;
	unsigned offset;

	/* Suppresses text-memory writes during graphics ownership. */
	if (output_suspended)
		return;

	/* Publishes a blank cell and its attribute across the row. */
	for (column = 0; column < CONS_COLUMNS; column++) {
		offset = row * CONS_COLUMNS + column;
		CONS_TEXT_VRAM[offset] = (uint16_t)' ';
		CONS_ATTRIBUTE_VRAM[offset * 2U] = CONS_ATTRIBUTE;
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
	unsigned destination;
	unsigned source;

	/* Suppresses text-memory writes during graphics ownership. */
	if (output_suspended)
		return;

	/* Copies every row up one position, characters and attributes. */
	for (row = 1; row < CONS_ROWS; row++) {
		for (column = 0; column < CONS_COLUMNS; column++) {
			destination = (row - 1U) * CONS_COLUMNS + column;
			source = row * CONS_COLUMNS + column;
			CONS_TEXT_VRAM[destination] = CONS_TEXT_VRAM[source];
			CONS_ATTRIBUTE_VRAM[destination * 2U] =
			    CONS_ATTRIBUTE_VRAM[source * 2U];
		}
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
	unsigned offset;

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

	/* Publishes the character cell and its attribute. */
	if (!output_suspended) {
		offset = cursor_row * CONS_COLUMNS + cursor_column;
		CONS_TEXT_VRAM[offset] = (uint16_t)(uint8_t)character;
		CONS_ATTRIBUTE_VRAM[offset * 2U] = CONS_ATTRIBUTE;
	}
	cursor_column++;

	/* Wraps to the next line at the end of the row. */
	if (cursor_column >= CONS_COLUMNS)
		newline_locked();
	update_cursor_locked();
}
