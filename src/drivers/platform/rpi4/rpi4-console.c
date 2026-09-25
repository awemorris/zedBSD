/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Raspberry Pi 4 console: /dev/console over the PL011 serial port.
 *
 * The board has no text display of its own, so the console is a teletype:
 * each character goes to hal_putc(), which writes it to the serial port and
 * to the firmware framebuffer.  A positioned write has no teletype meaning
 * and is dropped; the grid only answers the size and cursor queries that
 * programs make.
 *
 * Input is read from the PL011 receive FIFO by a kernel thread that looks
 * at it every few milliseconds, so the console needs no interrupt routing.
 */

#include <stdint.h>

#include "drivers/platform/rpi4/rpi4-console.h"

#include <hal/hal.h>
#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/sched.h>
#include <kern/text-display.h>
#include <kern/thread.h>
#include <kern/tty.h>

/* The kernel's direct map of physical memory and devices. */
#define RPI4_DIRECT_BASE	0xffff000000000000ULL

/* The PL011 the firmware routes to GPIO 14 and 15 (config.txt disables BT). */
#define RPI4_UART_BASE		0xfe201000ULL
#define RPI4_UART_DR		0x00U
#define RPI4_UART_FR		0x18U
#define RPI4_UART_FR_RXFE	0x10U

/* The grid a serial terminal is assumed to have. */
#define RPI4_CONSOLE_COLUMNS	80U
#define RPI4_CONSOLE_ROWS	24U

/* How often the receive FIFO is read, and how much of it at a time. */
#define RPI4_CONSOLE_POLL_MS	10U
#define RPI4_CONSOLE_DRAIN	64U

static void console_get_size(unsigned *columns, unsigned *rows);
static void console_putc(int character);
static void console_write(unsigned row, unsigned column, uint8_t attribute,
			  const char *utf8);
static void console_clear(void);
static int console_set_cursor(unsigned row, unsigned column);
static void console_get_cursor(unsigned *row, unsigned *column, int *visible);
static void console_show_cursor(int visible);
static void console_nothing(void);
static uint32_t uart_read(unsigned offset);
static void console_input_worker(void *argument);

static const struct kern_text_ops rpi4_console_ops = {
	.get_size = console_get_size,
	.putc = console_putc,
	.write = console_write,
	.clear = console_clear,
	.set_cursor = console_set_cursor,
	.get_cursor = console_get_cursor,
	.show_cursor = console_show_cursor,
	.update_cursor = console_nothing,
	.suspend = console_nothing,
	.resume = console_nothing
};

static struct thread *input_worker;

/*
 * Publishes the console output.
 */
void
drv_rpi4_console_init(
	void)
{
	kern_text_register(&rpi4_console_ops);
}

/*
 * Starts reading the serial port into the console.
 */
int
drv_rpi4_console_start_input(
	void)
{
	int error;

	/* Handles a second start. */
	if (input_worker != NULL)
		return 0;

	error = kthread_create(console_input_worker, NULL,
	    SCHED_PRIORITY_DEFAULT, &input_worker);
	if (error != 0) {
		/* Failed: the console stays output only. */
		return error;
	}
	thread_start(input_worker);

	/* Succeeded. */
	return 0;
}

/* Reports the assumed terminal grid. */
static void
console_get_size(
	unsigned *columns,
	unsigned *rows)
{
	if (columns != NULL)
		*columns = RPI4_CONSOLE_COLUMNS;
	if (rows != NULL)
		*rows = RPI4_CONSOLE_ROWS;
}

/* Sends one character to the serial port and the framebuffer. */
static void
console_putc(
	int character)
{
	hal_putc(character);
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
	/* Succeeded when the position is inside the grid. */
	return row < RPI4_CONSOLE_ROWS && column < RPI4_CONSOLE_COLUMNS;
}

/* Reports the home position and a shown cursor. */
static void
console_get_cursor(
	unsigned *row,
	unsigned *column,
	int *visible)
{
	if (row != NULL)
		*row = 0;
	if (column != NULL)
		*column = 0;
	if (visible != NULL)
		*visible = 1;
}

/* Ignores the request: the terminal draws its own cursor. */
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

/* Reads one PL011 register. */
static uint32_t
uart_read(
	unsigned offset)
{
	/* Succeeded: the register value. */
	return kern_mmio_read32((const volatile void *)(uintptr_t)
	    (RPI4_DIRECT_BASE + RPI4_UART_BASE + offset));
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
	uint8_t byte;

	(void)argument;
	for (;;) {
		for (drained = 0; drained < RPI4_CONSOLE_DRAIN; drained++) {
			if ((uart_read(RPI4_UART_FR) & RPI4_UART_FR_RXFE) != 0)
				break;
			byte = (uint8_t)uart_read(RPI4_UART_DR);
			if (byte == (uint8_t)'\r')
				byte = (uint8_t)'\n';
			else if (byte == 0x7fU)
				byte = 8U;
			tty_console_input_byte(byte);
		}
		sched_sleep(sched_ticks() + kern_ms_to_ticks(RPI4_CONSOLE_POLL_MS));
	}
}
