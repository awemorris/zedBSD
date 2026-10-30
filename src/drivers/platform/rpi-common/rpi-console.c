/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The console of the Raspberry Pi 4, the Raspberry Pi 5 and the ODROID-C5:
 * /dev/console over the screen the firmware or the boot loader left on,
 * and over the debug UART when the build makes the serial line the
 * console.
 *
 * The screen and the text on it are the generic boot framebuffer's and
 * framebuffer text console's; this file gives them the board's screen and
 * the UART, and reads the UART's receive FIFO into the console from a
 * kernel thread that looks at it every few milliseconds, so the console
 * needs no interrupt routing.  Without the serial console in the build
 * (CONFIG_RPI4_SERIAL_CONSOLE, off by default) the port is left alone.
 */

#include <stddef.h>
#include <stdint.h>

#include "drivers/platform/rpi4/rpi4-console.h"

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

#if defined(HAL_BOARD_ODROID_C5)
/*
 * The ODROID-C5's debug UART (the S905X5M's uart_B), an Amlogic UART: its
 * transmit and receive FIFOs, its status register, and the status's
 * receive-empty and transmit-full bits.
 *
 * Values referenced: Hardkernel Linux odroids7d-6.12.y common_drivers
 * arch/arm64/boot/dts/amlogic/meson-s7d.dtsi (uart_B under apb4's
 * ranges); Linux v7.3 drivers/tty/serial/meson_uart.c (the FIFO and
 * status offsets, and the receive-empty and transmit-full bits).
 */
#define RPI4_UART_BASE		0xfe07a000ULL
#define RPI4_UART_TX		0x00U
#define RPI4_UART_RX		0x04U
#define RPI4_UART_STATUS	0x0cU
#define RPI4_UART_RX_EMPTY	0x00100000U
#define RPI4_UART_TX_FULL	0x00200000U
#else
#if defined(HAL_BOARD_RPI5)
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

/*
 * A PL011's data and flag registers, and the flag's receive-empty and
 * transmit-full bits.
 *
 * Values referenced: ARM PrimeCell UART (PL011) Technical Reference
 * Manual, r1p5 (UARTDR, UARTFR and its RXFE and TXFF bits).
 */
#define RPI4_UART_TX		0x00U
#define RPI4_UART_RX		0x00U
#define RPI4_UART_STATUS	0x18U
#define RPI4_UART_RX_EMPTY	0x10U
#define RPI4_UART_TX_FULL	0x20U
#endif

/* The bytes of the UART's registers the console maps. */
#define RPI4_UART_WINDOW	0x1000U

/* How long a write waits for room in the transmitter before dropping the byte. */
#define RPI4_UART_SPIN_LIMIT	100000U

/* Whether the serial line is the console (CONFIG_RPI4_SERIAL_CONSOLE). */
#ifdef RPI4_SERIAL_CONSOLE
#define RPI4_CONSOLE_SERIAL	1
#else
#define RPI4_CONSOLE_SERIAL	0
#endif

/* The name prefix under which the platform hands the screen over. */
#if defined(HAL_BOARD_ODROID_C5)
#define RPI4_FRAMEBUFFER_NAME	"odroidc5.framebuffer"
#elif defined(HAL_BOARD_RPI5)
#define RPI4_FRAMEBUFFER_NAME	"rpi5.framebuffer"
#else
#define RPI4_FRAMEBUFFER_NAME	"rpi4.framebuffer"
#endif

/*
 * The platform's pixel order as the screen's format value names it: blue
 * in the low byte, or red.
 */
#define RPI4_FRAMEBUFFER_BLUE_FIRST	0U
#define RPI4_FRAMEBUFFER_RED_FIRST	1U

/*
 * The bits every pixel carries set.  The ODROID-C5's boot loader may leave
 * a plane that blends by the top byte, which shows only pixels whose alpha
 * is full; the byte is padding on every other form, so it is set there
 * always.  The Pi's firmware planes take the byte as it is.
 */
#if defined(HAL_BOARD_ODROID_C5)
#define RPI4_FRAMEBUFFER_OPAQUE	0xff000000U
#else
#define RPI4_FRAMEBUFFER_OPAQUE	0x00000000U
#endif

/* How often the receive FIFO is read, and how much of it at a time. */
#define RPI4_CONSOLE_POLL_MS	10U
#define RPI4_CONSOLE_DRAIN	64U

/* The terminal's Enter and Delete, and what the line discipline expects. */
#define RPI4_CONSOLE_RETURN	0x0dU
#define RPI4_CONSOLE_DELETE	0x7fU
#define RPI4_CONSOLE_BACKSPACE	0x08U

/*
 * The UART's registers, mapped once by drv_rpi4_console_init() when the
 * serial line is the console; NULL otherwise and when the mapping failed.
 * Never changed afterwards.
 */
static volatile uint8_t *uart_registers;

/*
 * What the board gives the console: the serial line when it is the
 * console.  Filled once by drv_rpi4_console_init() before the console
 * starts and not changed afterwards.
 */
static struct drv_fb_text_board rpi4_console_board;

/*
 * The kernel thread that reads the serial port, or NULL before input was
 * started.  It is created once and runs for the life of the system; the
 * pointer only keeps a second start from creating another.
 */
static struct thread *input_worker;

static void map_screen(void);
static void serial_write(int character);
static uint32_t uart_read(unsigned offset);
static void uart_write(unsigned offset, uint32_t value);
static void console_input_worker(void *argument);

/*
 * Publishes the console: the screen, the serial line, the text console
 * over both, and the screen's /dev/graphics backend.
 */
void
drv_rpi4_console_init(
	void)
{
	void *mapped;
	int error;

	/* Maps the UART the console writes and reads, when the serial line is the console. */
	if (RPI4_CONSOLE_SERIAL) {
		error = kern_device_map(RPI4_UART_BASE, RPI4_UART_WINDOW, KERN_DEVICE_UNCACHED, &mapped);
		if (error == 0) {
			uart_registers = mapped;
			rpi4_console_board.serial_write = serial_write;
		}
	}

	/* Maps the screen; without one the console is the serial line alone. */
	map_screen();

	/* Starts the text console over the screen and the serial line. */
	drv_fb_text_init(&rpi4_console_board);

	/* Offers the screen to /dev/graphics; without one there is no device. */
	error = drv_boot_fb_register_backend();
	if (error != 0 && error != ENODEV)
		kern_logf("graphics: firmware framebuffer not offered (%d)\n", error);
}

/*
 * Starts reading the serial port into the console.
 */
int
drv_rpi4_console_start_input(
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
	void)
{
	struct kern_boot_framebuffer screen;
	int error;

	/* Reads the screen; a boot whose firmware allocated none has nothing to map. */
	error = kern_boot_framebuffer(RPI4_FRAMEBUFFER_NAME, &screen);
	if (error != 0 || screen.size == 0U)
		return;

	/* Names the pixel order in the kernel's terms; another value is left unmapped. */
	if (screen.format == RPI4_FRAMEBUFFER_BLUE_FIRST) {
		screen.format = KERN_BOOT_FRAMEBUFFER_BGRX8888;
	} else if (screen.format == RPI4_FRAMEBUFFER_RED_FIRST) {
		screen.format = KERN_BOOT_FRAMEBUFFER_RGBX8888;
	} else {
		return;
	}

	/* Maps it; a screen that cannot be mapped leaves the console on the serial line. */
	error = drv_boot_fb_init(&screen, RPI4_FRAMEBUFFER_OPAQUE);
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
	for (spins = 0; spins < RPI4_UART_SPIN_LIMIT; spins++) {
		status = uart_read(RPI4_UART_STATUS);
		if ((status & RPI4_UART_TX_FULL) == 0)
			break;
	}

	/* Drops the byte on a port that never drains. */
	if (spins == RPI4_UART_SPIN_LIMIT)
		return;

	/* Queues the byte. */
	uart_write(RPI4_UART_TX, (uint32_t)character & 0xffU);
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

	/* The worker is given no argument. */
	(void)argument;

	/* Reads the port for the life of the system. */
	for (;;) {
		/* Moves at most one batch of received bytes into the console. */
		for (drained = 0; drained < RPI4_CONSOLE_DRAIN; drained++) {
			/* Stops when the receive FIFO is empty. */
			status = uart_read(RPI4_UART_STATUS);
			if ((status & RPI4_UART_RX_EMPTY) != 0)
				break;

			/* Takes the byte and translates Enter and backspace. */
			byte = (uint8_t)uart_read(RPI4_UART_RX);
			if (byte == RPI4_CONSOLE_RETURN) {
				byte = (uint8_t)'\n';
			} else if (byte == RPI4_CONSOLE_DELETE) {
				byte = RPI4_CONSOLE_BACKSPACE;
			}

			/* Hands the byte to the console's line discipline. */
			tty_console_input_byte(byte);
		}

		/* Waits until the next look at the port. */
		wake = sched_ticks() + kern_ms_to_ticks(RPI4_CONSOLE_POLL_MS);
		sched_sleep(wake);
	}
}
