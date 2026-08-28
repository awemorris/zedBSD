/*
 * NEC PC-98 uPD8255 bus mouse drive
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 *
 * The interface follows the PC-98 PPI wiring also implemented by
 * qemu-pc98: 0x7fd9/0x7fdb/0x7fdd/0x7fdf and IRQ 13.
 */

#include "drivers/pc98-busmouse.h"
#include "kern/mouse-device.h"
#include "kern/sched.h"
#include "kern/thread.h"

#include <errno.h>
#include <hal/hal.h>
#include <stdint.h>
#include <zedbsd/mouse.h>

#define MOUSE_PORT_A   0x7fd9U
#define MOUSE_PORT_C   0x7fddU
#define MOUSE_CONTROL  0x7fdfU
#define MOUSE_IRQ      13

#define PPI_MODE       0x93U
#define PORTC_HOLD     0x80U
#define PORTC_Y        0x40U
#define PORTC_HIGH     0x20U

#define PORTA_LEFT_RELEASED  0x80U
#define PORTA_RIGHT_RELEASED 0x20U

static uint32_t last_buttons;
static int worker_started;

static uint8_t
inb(uint16_t port)
{
	uint8_t value;
	__asm__ volatile("inb %w1,%0" : "=a"(value) : "Nd"(port));
	return value;
}

static void
outb(uint16_t port, uint8_t value)
{
	__asm__ volatile("outb %0,%w1" : : "a"(value), "Nd"(port));
}

static uint8_t
read_nibble(uint8_t control)
{
	outb(MOUSE_PORT_C, control);
	return inb(MOUSE_PORT_A);
}

static void
read_sample(int32_t *dx, int32_t *dy, uint32_t *buttons)
{
	uint8_t xlow, xhigh, ylow, yhigh, state;
	int8_t x, y;

	/* A fresh low-to-high HOLD edge snapshots and clears both counters. */
	outb(MOUSE_PORT_C, 0x00U);
	xlow = read_nibble(PORTC_HOLD);
	xhigh = read_nibble(PORTC_HOLD | PORTC_HIGH);
	ylow = read_nibble(PORTC_HOLD | PORTC_Y);
	yhigh = read_nibble(PORTC_HOLD | PORTC_Y | PORTC_HIGH);
	outb(MOUSE_PORT_C, 0x00U);
	x = (int8_t)(((xhigh & 0x0fU) << 4) | (xlow & 0x0fU));
	y = (int8_t)(((yhigh & 0x0fU) << 4) | (ylow & 0x0fU));
	state = xlow;
	*dx = x;
	*dy = y;
	*buttons = 0;
	if ((state & PORTA_LEFT_RELEASED) == 0)
		*buttons |= ZEDBSD_MOUSE_BUTTON_LEFT;
	if ((state & PORTA_RIGHT_RELEASED) == 0)
		*buttons |= ZEDBSD_MOUSE_BUTTON_RIGHT;
}

static void
mouse_service(void *argument)
{
	(void)argument;
	for (;;) {
		hal_irq_ack_t acknowledge;
		int32_t dx, dy;
		uint32_t buttons;
		if (hal_irq_service_wait(MOUSE_IRQ, &acknowledge) != HAL_OK)
			HAL_FATAL("PC-98 bus mouse IRQ service failed");
		read_sample(&dx, &dy, &buttons);
		hal_irq_send_eoi(acknowledge);
		if (dx != 0 || dy != 0 || buttons != last_buttons) {
			last_buttons = buttons;
			mouse_input_report(0, dx, dy, buttons);
		}
	}
}

static int
mouse_start(void)
{
	struct thread *worker;
	int error;
	last_buttons = 0;
	if (!worker_started) {
		error = kthread_create(mouse_service, NULL, SCHED_PRIOR_LOW, &worker);
		if (error != 0)
			return error;
		worker_started = 1;
		thread_start(worker);
	}
	/* The task IRQ service unmasks IRQ13 when it first waits. */
	outb(MOUSE_PORT_C, 0x00U);
	return 0;
}

static void
mouse_stop(void)
{
	outb(MOUSE_PORT_C, 0x10U);
}

int
pc98_busmouse_init(void)
{
	/* Leave the periodic IRQ masked until /dev/mouse is opened. */
	outb(MOUSE_CONTROL, PPI_MODE);
	outb(MOUSE_PORT_C, 0x10U);
	worker_started = 0;
	return mouse_device_set_backend(mouse_start, mouse_stop);
}
