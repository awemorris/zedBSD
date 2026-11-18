/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#include <stdint.h>

#include "serial-mirror.h"

#include "kern/irq.h"
#include "kern/tty.h"

/*
 * The console on the PC/AT first serial port.
 *
 * The console itself draws to the framebuffer, which is what a person at the
 * machine reads.  This repeats the same stream where a host running the
 * emulator, or a serial cable, can read it, and delivers what is typed there
 * to the same terminal the keyboard feeds.  It is built only when
 * CONFIG_PCAT_SERIAL_MIRROR selects it.
 *
 * Input matters for more than convenience.  Driving the console by injecting
 * key events into the emulator loses characters out of the middle of a line
 * once the guest is busy, which makes anything that has to type a command
 * unreliable.  A character that arrives on this line arrives whole.
 *
 * Reading is by interrupt, not by polling: the console must answer while the
 * machine is doing something else, and a poll that runs only when the console
 * is written would never see the first character of a command.
 *
 * Every wait is bounded.  A machine with no UART at this address leaves the
 * line-status register reading as all ones or all zeros, and output is then
 * dropped rather than spun on.
 */
#ifdef PCAT_SERIAL_MIRROR

#define PCAT_SERIAL_BASE	0x3f8U
#define PCAT_SERIAL_DATA	(PCAT_SERIAL_BASE + 0U)
#define PCAT_SERIAL_INTERRUPT	(PCAT_SERIAL_BASE + 1U)
#define PCAT_SERIAL_FIFO	(PCAT_SERIAL_BASE + 2U)
#define PCAT_SERIAL_LINE	(PCAT_SERIAL_BASE + 3U)
#define PCAT_SERIAL_MODEM	(PCAT_SERIAL_BASE + 4U)
#define PCAT_SERIAL_STATUS	(PCAT_SERIAL_BASE + 5U)

/* Line-status bit reporting that the transmit holding register is free. */
#define PCAT_SERIAL_TRANSMIT_READY	0x20U

/* Line-status bit reporting that a received character is waiting. */
#define PCAT_SERIAL_RECEIVE_READY	0x01U

/* Interrupt-enable bit for a received character. */
#define PCAT_SERIAL_RECEIVE_INTERRUPT	0x01U

/* The first serial port's interrupt on this machine. */
#define PCAT_SERIAL_IRQ			4

/*
 * How many characters one interrupt will take.
 *
 * The handler drains what the FIFO holds, and no more: a line that never
 * stops delivering must not keep the processor in the handler for ever.
 */
#define PCAT_SERIAL_DRAIN_LIMIT		64U

/* Bound on how long output waits for the transmitter, in reads. */
#define PCAT_SERIAL_WAIT_LIMIT		100000U

static int serial_ready;
static int serial_input_ready;

/* Supports the serial out operation. */
static void
serial_out(
	uint16_t port,
	uint8_t value)
{
	__asm__ volatile("outb %0,%w1" : : "a"(value), "Nd"(port));
}

/* Supports the serial in operation. */
static uint8_t
serial_in(
	uint16_t port)
{
	uint8_t value;

	__asm__ volatile("inb %w1,%0" : "=a"(value) : "Nd"(port));

	/* Returns the computed result. */
	return value;
}

/* Supports the serial start operation. */
static void
serial_start(
	void)
{
	/* Handles the already started condition. */
	if (serial_ready != 0)
		return;
	serial_ready = 1;

	/* Silences the device, then selects 115200 8N1 with the FIFO on. */
	serial_out(PCAT_SERIAL_INTERRUPT, 0x00U);
	serial_out(PCAT_SERIAL_LINE, 0x80U);
	serial_out(PCAT_SERIAL_DATA, 0x01U);
	serial_out(PCAT_SERIAL_INTERRUPT, 0x00U);
	serial_out(PCAT_SERIAL_LINE, 0x03U);
	serial_out(PCAT_SERIAL_FIFO, 0xc7U);
	serial_out(PCAT_SERIAL_MODEM, 0x03U);
}

/* Supports the serial put operation. */
static void
serial_put(
	uint8_t value)
{
	unsigned waited;
	uint8_t status;

	serial_start();

	/* Waits a bounded time for the transmit holding register. */
	for (waited = 0; waited < PCAT_SERIAL_WAIT_LIMIT; waited++) {
		status = serial_in(PCAT_SERIAL_STATUS);

		/* Reports an absent device rather than waiting for one. */
		if (status == 0x00U || status == 0xffU)
			return;

		/* Selects the ready transmitter. */
		if ((status & PCAT_SERIAL_TRANSMIT_READY) != 0)
			break;
	}

	/* Drops the byte when the transmitter never became free. */
	if (waited >= PCAT_SERIAL_WAIT_LIMIT)
		return;
	serial_out(PCAT_SERIAL_DATA, value);
}

/*
 * Takes every character the port is holding and gives it to the console.
 *
 * A carriage return is what a terminal sends for the return key, and the
 * line discipline expects a newline; the two are the same key at opposite
 * ends of a cable, so one becomes the other here.  A delete is what most
 * terminals send for backspace, and the discipline erases on a backspace.
 */
static void
serial_receive(
	void)
{
	unsigned drained;
	uint8_t status;
	uint8_t byte;

	/* Process each character the port is holding. */
	for (drained = 0; drained < PCAT_SERIAL_DRAIN_LIMIT; drained++) {
		status = serial_in(PCAT_SERIAL_STATUS);

		/* Reports an absent device rather than reading from one. */
		if (status == 0x00U || status == 0xffU)
			return;

		/* Stops once the port is holding nothing. */
		if ((status & PCAT_SERIAL_RECEIVE_READY) == 0)
			return;
		byte = serial_in(PCAT_SERIAL_DATA);

		/*
		 * Characters arriving before anything can read them are
		 * dropped, not queued: a line connected since power-on may
		 * have carried anything while nothing was listening.
		 */
		if (!serial_input_ready)
			continue;

		/* Maps what a terminal sends to what the discipline reads. */
		if (byte == (uint8_t)'\r')
			byte = (uint8_t)'\n';
		else if (byte == 0x7fU)
			byte = 8U;
		tty_console_input_byte(byte);
	}
}

/* Supports the serial interrupt operation. */
static void
serial_interrupt(
	int interrupt,
	kern_irq_ack_t acknowledge,
	void *argument)
{
	(void)interrupt;
	(void)argument;
	serial_receive();
	kern_irq_send_eoi(acknowledge);
}

/*
 * Implements the drv pcat serial mirror start input operation.
 */
void
drv_pcat_serial_mirror_start_input(
	void)
{
	/* Handles the already started condition. */
	if (serial_input_ready != 0)
		return;
	serial_start();

	/* Handles a machine with no port at this address. */
	if (serial_in(PCAT_SERIAL_STATUS) == 0xffU)
		return;

	/* Handles a port whose interrupt nothing will deliver. */
	if (kern_irq_register(PCAT_SERIAL_IRQ, serial_interrupt, NULL) != 0)
		return;

	/*
	 * Whatever the line carried before this is discarded, so that the
	 * first thing the console reads is the first thing that was typed
	 * to it.
	 */
	serial_receive();
	serial_input_ready = 1;
	serial_out(PCAT_SERIAL_INTERRUPT, PCAT_SERIAL_RECEIVE_INTERRUPT);
	kern_irq_unmask(PCAT_SERIAL_IRQ);
}

/*
 * Repeats one console character on the serial port.
 */
void
drv_pcat_serial_mirror(
	int character)
{
	static int previous;

	/*
	 * A terminal needs a carriage return before a line feed.  The console
	 * discipline already sends one for terminal output, so it is only
	 * supplied here when the stream did not carry it -- the kernel log
	 * writes bare line feeds.
	 */
	if (character == '\n' && previous != '\r')
		serial_put((uint8_t)'\r');
	serial_put((uint8_t)character);
	previous = character;
}

#else

/*
 * Repeats one console character on the serial port.
 */
void
drv_pcat_serial_mirror(
	int character)
{
	(void)character;
}

/*
 * Implements the drv pcat serial mirror start input operation.
 */
void
drv_pcat_serial_mirror_start_input(
	void)
{
}

#endif

