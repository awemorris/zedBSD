/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The console of an S905X5M board: /dev/console over the screen the boot
 * loader left on, and over the board's debug UART (an Amlogic UART) when
 * the build makes the serial line the console.
 *
 * The screen and the text on it are the generic boot framebuffer's and
 * framebuffer text console's; this file gives them the board's screen and
 * the UART, and reads the UART's receive FIFO into the console from a
 * kernel thread that looks at it every few milliseconds, so the console
 * needs no interrupt routing.  Without the serial console in the build the
 * port is left alone.  The board's platform names its UART and the handoff
 * name of its screen.
 */

#include <stddef.h>
#include <stdint.h>

#include "drivers/platform/s905x5m/s905x5m-console.h"

#include <drivers/generic/boot-fb.h>
#include <drivers/generic/fb-text.h>
#include <kern/boot.h>
#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/klog.h>
#include <kern/platform.h>
#include <kern/pmem.h>
#include <kern/sched.h>
#include <kern/thread.h>
#include <kern/tty.h>
#include <uapi/errno.h>

/*
 * An Amlogic UART's transmit and receive FIFOs, its status register, and
 * the status's receive-empty and transmit-full bits.
 *
 * Values referenced: Linux v7.3 drivers/tty/serial/meson_uart.c (the FIFO
 * and status offsets, and the receive-empty and transmit-full bits).
 */
#define S905X5M_UART_TX		0x00U
#define S905X5M_UART_RX		0x04U
#define S905X5M_UART_STATUS	0x0cU
#define S905X5M_UART_RX_EMPTY	0x00100000U
#define S905X5M_UART_TX_FULL	0x00200000U

/* The bytes of the UART's registers the console maps. */
#define S905X5M_UART_WINDOW	0x1000U

/* How long a write waits for room in the transmitter before dropping the byte. */
#define S905X5M_UART_SPIN_LIMIT	100000U

/* Whether the serial line is the console (the board's serial-console option). */
#ifdef ARM64_SERIAL_CONSOLE
#define S905X5M_CONSOLE_SERIAL	1
#else
#define S905X5M_CONSOLE_SERIAL	0
#endif

/*
 * The bits every pixel carries set.  The boot loader may leave a plane
 * that blends by the top byte, which shows only pixels whose alpha is
 * full; the byte is padding on every other form, so it is set always.
 */
#define S905X5M_FRAMEBUFFER_OPAQUE	0xff000000U

/*
 * The boot loader's pixel order as the screen's format value names it:
 * blue in the low byte, or red.
 */
#define S905X5M_FRAMEBUFFER_BLUE_FIRST	0U
#define S905X5M_FRAMEBUFFER_RED_FIRST	1U

/* How often the receive FIFO is read, and how much of it at a time. */
#define S905X5M_CONSOLE_POLL_MS	10U
#define S905X5M_CONSOLE_DRAIN	64U

/* The terminal's Enter and Delete, and what the line discipline expects. */
#define S905X5M_CONSOLE_RETURN	0x0dU
#define S905X5M_CONSOLE_DELETE	0x7fU
#define S905X5M_CONSOLE_BACKSPACE	0x08U

/*
 * The UART's registers, mapped once by drv_s905x5m_console_init() when the
 * serial line is the console; NULL otherwise and when the mapping failed.
 * Never changed afterwards.
 */
static volatile uint8_t *uart_registers;

/*
 * What the board gives the console: the serial line when it is the
 * console.  Filled once by drv_s905x5m_console_init() before the console
 * starts and not changed afterwards.
 */
static struct drv_fb_text_board s905x5m_console_board;

/*
 * The kernel thread that reads the serial port, or NULL before input was
 * started.  It is created once and runs for the life of the system; the
 * pointer only keeps a second start from creating another.
 */
static struct thread *input_worker;

static void map_screen(const char *framebuffer_name);
static void serial_write(int character);
static uint32_t uart_read(unsigned offset);
static void uart_write(unsigned offset, uint32_t value);
static void console_input_worker(void *argument);

/*
 * Publishes the console: the screen, the serial line, the text console
 * over both, and the screen's /dev/graphics backend.
 *
 * uart_physical is the debug UART's register block, and framebuffer_name
 * the prefix under which the platform hands the screen over.
 */
void
drv_s905x5m_console_init(
	uint64_t uart_physical,
	const char *framebuffer_name)
{
	void *mapped;
	int error;

	/* Maps the UART the console writes and reads, when the serial line is the console. */
	if (S905X5M_CONSOLE_SERIAL) {
		error = kern_device_map(uart_physical, S905X5M_UART_WINDOW, KERN_DEVICE_UNCACHED, &mapped);
		if (error == 0) {
			uart_registers = mapped;
			s905x5m_console_board.serial_write = serial_write;
		}
	}

	/* Maps the screen; without one the console is the serial line alone. */
	map_screen(framebuffer_name);

	/* Starts the text console over the screen and the serial line. */
	drv_fb_text_init(&s905x5m_console_board);

	/* Offers the screen to /dev/graphics; without one there is no device. */
	error = drv_boot_fb_register_backend();
	if (error != 0 && error != ENODEV)
		kern_logf("graphics: firmware framebuffer not offered (%d)\n", error);
}

/*
 * Starts reading the serial port into the console.
 */
int
drv_s905x5m_console_start_input(
	void)
{
	int error;

	/* No reader without the serial console's port, nor a second one. */
	if (uart_registers == NULL || input_worker != NULL)
		return 0;

	/* Creates the reader; without it the console stays output only. */
	error = kthread_create(console_input_worker, NULL, SCHED_PRIORITY_DEFAULT, &input_worker);
	if (error != 0)
		return error;

	/* Lets the reader run. */
	thread_start(input_worker);

	/* Succeeded. */
	return 0;
}

/*
 * Hands the screen the platform names to the boot framebuffer driver, its
 * pixel order turned into the kernel's format.
 */
static void
map_screen(
	const char *framebuffer_name)
{
	struct kern_boot_framebuffer screen;
	int error;

	/* Reads the screen; a boot loader that left none has nothing to map. */
	error = kern_boot_framebuffer(framebuffer_name, &screen);
	if (error != 0 || screen.size == 0U)
		return;

	/* Names the pixel order in the kernel's terms; another value is left unmapped. */
	if (screen.format == S905X5M_FRAMEBUFFER_BLUE_FIRST) {
		screen.format = KERN_BOOT_FRAMEBUFFER_BGRX8888;
	} else if (screen.format == S905X5M_FRAMEBUFFER_RED_FIRST) {
		screen.format = KERN_BOOT_FRAMEBUFFER_RGBX8888;
	} else {
		return;
	}

	/* Maps it; a screen that cannot be mapped leaves the console on the serial line. */
	error = drv_boot_fb_init(&screen, S905X5M_FRAMEBUFFER_OPAQUE);
	if (error != 0)
		kern_logf("console: firmware framebuffer not mapped (%d)\n", error);
}

/*
 * Writes one byte to the UART, a carriage return before a newline; a byte
 * the transmitter never takes is dropped.
 */
static void
serial_write(
	int character)
{
	unsigned spins;
	uint32_t status;

	/* Nothing to write without the serial console's port. */
	if (uart_registers == NULL)
		return;

	/* A terminal needs the carriage return to start the next line at its left. */
	if (character == '\n')
		serial_write('\r');

	/* Waits for room in the transmit FIFO. */
	for (spins = 0; spins < S905X5M_UART_SPIN_LIMIT; spins++) {
		status = uart_read(S905X5M_UART_STATUS);
		if ((status & S905X5M_UART_TX_FULL) == 0)
			break;
	}

	/* Drops the byte on a port that never drains. */
	if (spins == S905X5M_UART_SPIN_LIMIT)
		return;

	/* Queues the byte. */
	uart_write(S905X5M_UART_TX, (uint32_t)character & 0xffU);
}

/* Reads one register of the debug UART. */
static uint32_t
uart_read(
	unsigned offset)
{
	uint32_t value;

	/* Reads the register through the console's mapping of the device. */
	value = kern_mmio_read32(uart_registers + offset);

	/* Succeeded: the register value. */
	return value;
}

/* Writes one register of the debug UART. */
static void
uart_write(
	unsigned offset,
	uint32_t value)
{
	/* Writes the register through the console's mapping of the device. */
	kern_mmio_write32(uart_registers + offset, value);
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
	uint32_t status;
	uint8_t byte;
	uint64_t wake;
	uint64_t now;
	uint64_t poll_ticks;

	/* The worker is given no argument. */
	(void)argument;

	/* Reads the port for the life of the system. */
	for (;;) {
		/* Moves at most one batch of received bytes into the console. */
		for (drained = 0; drained < S905X5M_CONSOLE_DRAIN; drained++) {
			/* Stops when the receive FIFO is empty. */
			status = uart_read(S905X5M_UART_STATUS);
			if ((status & S905X5M_UART_RX_EMPTY) != 0)
				break;

			/* Takes the byte and translates Enter and backspace. */
			byte = (uint8_t)uart_read(S905X5M_UART_RX);
			if (byte == S905X5M_CONSOLE_RETURN) {
				byte = (uint8_t)'\n';
			} else if (byte == S905X5M_CONSOLE_DELETE) {
				byte = S905X5M_CONSOLE_BACKSPACE;
			}

			/* Hands the byte to the console's line discipline. */
			tty_console_input_byte(byte);
		}

		/* Waits until the next look at the port. */
		now = sched_ticks();
		poll_ticks = kern_ms_to_ticks(S905X5M_CONSOLE_POLL_MS);
		wake = now + poll_ticks;
		sched_sleep(wake);
	}
}
