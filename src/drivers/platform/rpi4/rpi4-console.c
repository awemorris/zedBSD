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
 * each character goes to kern_console_early_putc(), which writes it to the
 * serial port and to the firmware framebuffer.  A positioned write has no
 * teletype meaning and is dropped; the grid only answers the size and
 * cursor queries that programs make.
 *
 * Input is read from the PL011 receive FIFO by a kernel thread that looks
 * at it every few milliseconds, so the console needs no interrupt routing.
 * Without the serial console in the build (CONFIG_RPI4_SERIAL_CONSOLE, off
 * by default) the port is left alone: no reader, and output goes only to
 * the framebuffer until a display driver takes it.
 *
 * Once a GPU has flipped its first image the console leaves the
 * framebuffer for good: the hardware layer's framebuffer output is
 * suspended and never resumed, and the serial line carries on.
 */

#include <stdint.h>

#include "drivers/platform/rpi4/rpi4-console.h"

#include "hal/arm64/bsp-rpi-common/cons.h"
#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/platform.h>
#include <kern/sched.h>
#include <kern/text-display.h>
#include <kern/thread.h>
#include <kern/tty.h>
#include <uapi/errno.h>

/* The kernel's direct map of physical memory and devices. */
#define RPI4_DIRECT_BASE	0xffff000000000000ULL

#if defined(HAL_BOARD_ODROID_C5)
/*
 * The ODROID-C5's debug UART (the S905X5M's uart_B), an Amlogic UART: its
 * receive FIFO, its status register and the status's receive-empty bit.
 *
 * Values referenced: Hardkernel Linux odroids7d-6.12.y common_drivers
 * arch/arm64/boot/dts/amlogic/meson-s7d.dtsi (uart_B under apb4's
 * ranges); Linux v7.3 drivers/tty/serial/meson_uart.c (the receive FIFO
 * and status offsets, and the receive-empty bit).
 */
#define RPI4_UART_BASE		0xfe07a000ULL
#define RPI4_UART_DR		0x04U
#define RPI4_UART_FR		0x0cU
#define RPI4_UART_FR_RXFE	0x00100000U
#elif defined(HAL_BOARD_RPI5)
/*
 * The Pi 5's PL011 of the three-pin debug connector, which shares nothing
 * with the Bluetooth controller.
 *
 * Value referenced: Raspberry Pi Linux rpi-6.12.y
 * arch/arm64/boot/dts/broadcom/bcm2712.dtsi (uart10 under the soc node's
 * ranges).
 */
#define RPI4_UART_BASE		0x107d001000ULL
#else
/* The PL011 the firmware routes to GPIO 14 and 15 (config.txt disables BT). */
#define RPI4_UART_BASE		0xfe201000ULL
#endif

/* The name prefix under which the hardware layer describes the firmware's screen. */
#if defined(HAL_BOARD_ODROID_C5)
#define RPI4_FRAMEBUFFER_NAME	"odroidc5.framebuffer"
#elif defined(HAL_BOARD_RPI5)
#define RPI4_FRAMEBUFFER_NAME	"rpi5.framebuffer"
#else
#define RPI4_FRAMEBUFFER_NAME	"rpi4.framebuffer"
#endif

/* A PL011's data and flag registers, and the flag's receive-empty bit. */
#if !defined(HAL_BOARD_ODROID_C5)
#define RPI4_UART_DR		0x00U
#define RPI4_UART_FR		0x18U
#define RPI4_UART_FR_RXFE	0x10U
#endif

/* The grid a serial terminal is assumed to have. */
#define RPI4_CONSOLE_COLUMNS	80U
#define RPI4_CONSOLE_ROWS	24U

/* Whether the serial line is the console (CONFIG_RPI4_SERIAL_CONSOLE). */
#ifdef RPI4_SERIAL_CONSOLE
#define RPI4_CONSOLE_SERIAL	1
#else
#define RPI4_CONSOLE_SERIAL	0
#endif

/* How often the receive FIFO is read, and how much of it at a time. */
#define RPI4_CONSOLE_POLL_MS	10U
#define RPI4_CONSOLE_DRAIN	64U

static void console_get_size(unsigned *columns, unsigned *rows);
static void console_putc(int character);
static void console_write(unsigned row, unsigned column, uint8_t attribute, const char *utf8);
static void console_clear(void);
static int console_set_cursor(unsigned row, unsigned column);
static void console_get_cursor(unsigned *row, unsigned *column, int *visible);
static void console_show_cursor(int visible);
static void console_nothing(void);
static void console_suspend(void);
static void console_resume(void);
static int console_end(const struct kern_text_range *ranges, unsigned count);
static uint32_t uart_read(unsigned offset);
static void console_input_worker(void *argument);

/*
 * The console's text operations, registered once at start-up and never
 * changed.  Everything but a character goes nowhere, because the console is
 * a teletype.
 */
static const struct kern_text_ops rpi4_console_ops = {
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
	.end = console_end
};

/*
 * The kernel thread that reads the serial port, or NULL before input was
 * started.  It is created once and runs for the life of the system; the
 * pointer only keeps a second start from creating another.
 */
static struct thread *input_worker;

/*
 * Whether the console has left the framebuffer for good: a resume no
 * longer gives the screen back to the hardware layer's console.  Set once
 * by console_end() before it suspends the hardware layer's output; read by
 * console_resume() before and after it resumes that output, all
 * sequentially consistent, so that no resume outlives an end.
 */
static int console_ended;

/*
 * Publishes the console output.
 */
void
drv_rpi4_console_init(
	void)
{
	/* Makes the teletype the console's text output. */
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

	/* No reader without the serial console, nor a second one. */
	if (!RPI4_CONSOLE_SERIAL || input_worker != NULL)
		return 0;

	/* Creates the reader; without it the console stays output only. */
	error = kthread_create(console_input_worker, NULL,
	    SCHED_PRIORITY_DEFAULT, &input_worker);
	if (error != 0)
		return error;

	/* Lets the reader run. */
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
	/* Reports the width when the caller asked for it. */
	if (columns != NULL)
		*columns = RPI4_CONSOLE_COLUMNS;

	/* Reports the height when the caller asked for it. */
	if (rows != NULL)
		*rows = RPI4_CONSOLE_ROWS;
}

/* Sends one character to the serial port and the framebuffer. */
static void
console_putc(
	int character)
{
	/* Writes the character wherever the hardware layer shows the console. */
	kern_console_early_putc(character);
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
	/* Refuses a row below the grid. */
	if (row >= RPI4_CONSOLE_ROWS)
		return 0;

	/* Refuses a column right of the grid. */
	if (column >= RPI4_CONSOLE_COLUMNS)
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

/* Reads one register of the debug UART. */
static uint32_t
uart_read(
	unsigned offset)
{
	uint32_t value;

	/* Reads the register through the kernel's map of the device. */
	value = kern_mmio_read32((const volatile void *)(uintptr_t)
	    (RPI4_DIRECT_BASE + RPI4_UART_BASE + offset));

	/* Succeeded: the register value. */
	return value;
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
	uint32_t flags;
	uint8_t byte;
	uint64_t wake;

	/* The worker is given no argument. */
	(void)argument;

	/* Reads the port for the life of the system. */
	for (;;) {
		/* Moves at most one batch of received bytes into the console. */
		for (drained = 0; drained < RPI4_CONSOLE_DRAIN; drained++) {
			/* Stops when the receive FIFO is empty. */
			flags = uart_read(RPI4_UART_FR);
			if ((flags & RPI4_UART_FR_RXFE) != 0)
				break;

			/* Takes the byte and translates Enter and backspace. */
			byte = (uint8_t)uart_read(RPI4_UART_DR);
			if (byte == (uint8_t)'\r') {
				byte = (uint8_t)'\n';
			} else if (byte == 0x7fU) {
				byte = 8U;
			}

			/* Hands the byte to the console's line discipline. */
			tty_console_input_byte(byte);
		}

		/* Waits until the next look at the port. */
		wake = sched_ticks() + kern_ms_to_ticks(RPI4_CONSOLE_POLL_MS);
		sched_sleep(wake);
	}
}

/* Keeps UART output and retained text active while graphics owns the framebuffer. */
static void
console_suspend(
	void)
{
	/* Stops only the early console's pixel writes under its own output lock. */
	rpi_cons_suspend_output();

	/* Succeeded: subsequent text cannot paint into graphics-owned scanout. */
	return;
}

/* Restores framebuffer text after the display driver confirms console adoption. */
static void
console_resume(
	void)
{
	int ended;

	/* A console that left the framebuffer stays off it; the serial line goes on. */
	ended = __atomic_load_n(&console_ended, __ATOMIC_SEQ_CST);
	if (ended)
		return;

	/* Repaints the retained early console without discarding newer log messages. */
	rpi_cons_resume_output();

	/* An end that came meanwhile may have suspended the output first: it stays suspended. */
	ended = __atomic_load_n(&console_ended, __ATOMIC_SEQ_CST);
	if (ended)
		rpi_cons_suspend_output();

	/* Succeeded: the framebuffer shows the console again, unless it has left it. */
	return;
}

/*
 * Ends the console's use of the framebuffer for good: the hardware layer's
 * framebuffer output is suspended and no later resume gives it back, while
 * the serial line goes on.  The hardware layer's stall watch still draws a
 * stop's report on the firmware's framebuffer when it has no compositor to
 * draw on, so that screen counts as left: EBUSY when it lies in the
 * ranges, 0 otherwise.
 */
static int
console_end(
	const struct kern_text_range *ranges,
	unsigned count)
{
	struct kern_boot_framebuffer screen;
	int overlaps;
	int error;

	/*
	 * No resume gives the screen back from now on: one that read the mark
	 * clear before this store suspends the output again after its own
	 * repaint, since it reads the mark once more.
	 */
	__atomic_store_n(&console_ended, 1, __ATOMIC_SEQ_CST);

	/* Stops the hardware layer's framebuffer writes under its output lock. */
	rpi_cons_suspend_output();

	/* A boot whose firmware allocated no screen leaves nothing behind. */
	error = kern_boot_framebuffer(RPI4_FRAMEBUFFER_NAME, &screen);
	if (error != 0 || screen.size == 0U)
		return 0;

	/* The firmware's screen, which a stop's report may still draw on. */
	overlaps = kern_text_range_overlaps(ranges, count, screen.physical_base, screen.size);
	if (overlaps)
		return EBUSY;

	/* Succeeded: nothing the console leaves behind lies in the ranges. */
	return 0;
}
