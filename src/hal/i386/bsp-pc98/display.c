/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The native PC-98 GDC display-switching implementation.
 */

#include "hal/i386/bsp-pc98/display.h"

#include <stdint.h>

#define GDC_FIFO_FULL 0x02U
#define GDC_START 0x0dU
#define GDC_STOP 0x0cU
#define GDC_TIMEOUT 1000000U

static uint8_t display_inb(uint16_t port);
static void display_outb(uint16_t port, uint8_t value);
static int command(uint16_t status, uint16_t port, uint8_t value);

/*
 * Starts the PC-98 graphics display controller.
 */
int
pc98_display_graphics_start(
	void)
{
	int result;

	/* Sends the graphics-controller start command. */
	result = command(0xa0, 0xa2, GDC_START);

	/* Returns the bounded command result. */
	return result;
}

/*
 * Stops the PC-98 graphics display controller.
 */
int
pc98_display_graphics_stop(
	void)
{
	int result;

	/* Sends the graphics-controller stop command. */
	result = command(0xa0, 0xa2, GDC_STOP);

	/* Returns the bounded command result. */
	return result;
}

/*
 * Restores the PC-98 text display controller.
 */
int
pc98_display_text_restore(
	void)
{
	int result;

	/* Stops graphics before addressing the text controller. */
	result = pc98_display_graphics_stop();
	if (!result)
		return 0;

	/* Starts the text display controller. */
	result = command(0x60, 0x62, GDC_START);

	/* Returns the text-controller command result. */
	return result;
}

/* Reads one byte from a PC-98 display-controller port. */
static uint8_t
display_inb(
	uint16_t port)
{
	uint8_t value;

	/* Reads the selected eight-bit I/O port. */
	__asm__ volatile("inb %w1,%0" : "=a"(value) : "Nd"(port));

	/* Returns the sampled port value. */
	return value;
}

/* Writes one byte to a PC-98 display-controller port. */
static void
display_outb(
	uint16_t port,
	uint8_t value)
{
	/* Writes the value to the selected eight-bit I/O port. */
	__asm__ volatile("outb %0,%w1" : : "a"(value), "Nd"(port));
}

/* Sends one display command after waiting for FIFO capacity. */
static int
command(
	uint16_t status,
	uint16_t port,
	uint8_t value)
{
	uint8_t status_value;
	unsigned timeout;

	/* Polls the controller FIFO for a bounded interval. */
	for (timeout = GDC_TIMEOUT; timeout != 0; timeout--) {
		/* Sends the command as soon as the FIFO accepts another byte. */
		status_value = display_inb(status);
		if ((status_value & GDC_FIFO_FULL) == 0) {
			display_outb(port, value);
			return 1;
		}
	}

	/* Reports a controller FIFO timeout. */
	return 0;
}
