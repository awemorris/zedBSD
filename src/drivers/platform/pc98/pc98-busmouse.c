/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * NEC PC-98 uPD8255 bus mouse drive
 */

#include "drivers/hid/pc98-busmouse.h"
#include "kern/input-device.h"
#include "kern/lock.h"
#ifdef WS018_INPUT_HID_HOST_TEST
struct thread;
int kthread_create(void (*)(void *), void *, int, struct thread **);
void thread_start(struct thread *);
#define SCHED_PRIOR_LOW 15
#else
#include "kern/sched.h"
#include "kern/thread.h"
#endif

#include <errno.h>
#include <hal/hal.h>
#include <limits.h>
#include <stdint.h>
#include "kern/irq.h"

#define MOUSE_PORT_A 0x7fd9U
#define MOUSE_PORT_C 0x7fddU
#define MOUSE_CONTROL 0x7fdfU
#define MOUSE_IRQ 13

#define PPI_MODE 0x93U
#define PORTC_HOLD 0x80U
#define PORTC_Y 0x40U
#define PORTC_HIGH 0x20U

#define PORTA_LEFT_RELEASED 0x80U
#define PORTA_RIGHT_RELEASED 0x20U

#define MOUSE_BUTTON_LEFT 0x01U
#define MOUSE_BUTTON_MIDDLE 0x02U
#define MOUSE_BUTTON_RIGHT 0x04U

static struct mutex lifecycle_lock;
static struct input_device *mouse_input;
static uint32_t last_buttons;
static unsigned reader_count;
static int handler_registered;
static int mouse_active;

static const struct input_capability mouse_capabilities[] = {
	{EV_SYN, SYN_REPORT}, {EV_REL, REL_X},	   {EV_REL, REL_Y},
	{EV_KEY, BTN_LEFT},   {EV_KEY, BTN_RIGHT}, {EV_KEY, BTN_MIDDLE},
};

#ifdef WS018_INPUT_HID_HOST_TEST
uint8_t ws018_pc98_mouse_test_inb(uint16_t);
void ws018_pc98_mouse_test_outb(uint16_t, uint8_t);
#define inb ws018_pc98_mouse_test_inb
#define outb ws018_pc98_mouse_test_outb
#else
static uint8_t inb(uint16_t port);

static void outb(uint16_t port, uint8_t value);
static uint8_t read_nibble(uint8_t control);
static void read_sample(int32_t *dx, int32_t *dy, uint32_t *buttons);
static void mouse_publish(int32_t dx, int32_t dy, uint32_t previous, uint32_t buttons);
static void mouse_interrupt(int interrupt,
			    kern_irq_ack_t acknowledge, void *argument);
static int mouse_start(void);
static void mouse_stop(void);
static int mouse_input_open(void *context);
static void mouse_input_close(void *context);

/* Supports the inb operation. */
static uint8_t
inb(
	uint16_t port)
{
	uint8_t value;

	__asm__ volatile("inb %w1,%0" : "=a"(value) : "Nd"(port));

	/* Returns the computed result. */
	return value;
}

/* Supports the outb operation. */
static void
outb(
	uint16_t port,
	uint8_t value)
{
	__asm__ volatile("outb %0,%w1" : : "a"(value), "Nd"(port));
}
#endif

/* Supports the read nibble operation. */
static uint8_t
read_nibble(
	uint8_t control)
{
	uint8_t function_result;

	outb(MOUSE_PORT_C, control);

	/* Obtains the inb result. */
	function_result = inb(MOUSE_PORT_A);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the read sample operation. */
static void
read_sample(
	int32_t *dx,
	int32_t *dy,
	uint32_t *buttons)
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
	/* The PC-98 counter already uses evdev's positive-down REL_Y sign. */
	*dy = y;
	*buttons = 0;
	/* Handles the state condition. */
	if ((state & PORTA_LEFT_RELEASED) == 0)
		*buttons |= MOUSE_BUTTON_LEFT;
	/* Handles the state condition. */
	if ((state & PORTA_RIGHT_RELEASED) == 0)
		*buttons |= MOUSE_BUTTON_RIGHT;
}

/* Supports the mouse publish operation. */
static void
mouse_publish(
	int32_t dx,
	int32_t dy,
	uint32_t previous,
	uint32_t buttons)
{
	/* Handles the dx condition. */
	if (dx != 0)
		drv_input_device_emit(mouse_input, EV_REL, REL_X, dx);

	/* Handles the dy condition. */
	if (dy != 0)
		drv_input_device_emit(mouse_input, EV_REL, REL_Y, dy);

	/* Handles the buttons condition. */
	if ((buttons ^ previous) & MOUSE_BUTTON_LEFT) {
		drv_input_device_emit(mouse_input, EV_KEY, BTN_LEFT,
				      (buttons & MOUSE_BUTTON_LEFT) != 0);
	}

	/* Handles the buttons condition. */
	if ((buttons ^ previous) & MOUSE_BUTTON_RIGHT) {
		drv_input_device_emit(mouse_input, EV_KEY, BTN_RIGHT,
				      (buttons & MOUSE_BUTTON_RIGHT) != 0);
	}

	/* Handles the buttons condition. */
	if ((buttons ^ previous) & MOUSE_BUTTON_MIDDLE) {
		drv_input_device_emit(mouse_input, EV_KEY, BTN_MIDDLE,
				      (buttons & MOUSE_BUTTON_MIDDLE) != 0);
	}

	drv_input_device_emit(mouse_input, EV_SYN, SYN_REPORT, 0);
}

/*
 * Reads one sample and reports it.
 *
 * This runs in interrupt context, so it takes no lock. It reads the
 * hardware, releases the line, and then emits: only this handler writes
 * the button state while the mouse is active.
 */
static void
mouse_interrupt(
	int interrupt,
	kern_irq_ack_t acknowledge,
	void *argument)
{
	int32_t dx = 0, dy = 0;
	uint32_t buttons = 0, previous = 0;
	int report = 0;

	(void)interrupt;
	(void)argument;

	/* Collects the sample only while a reader wants one. */
	if (__atomic_load_n(&mouse_active, __ATOMIC_ACQUIRE)) {
		read_sample(&dx, &dy, &buttons);

		/* Reports only a change in position or buttons. */
		previous = last_buttons;
		if (dx != 0 || dy != 0 || buttons != previous) {
			last_buttons = buttons;
			report = 1;
		}
	}

	/* Releases the line before the evdev publication. */
	kern_irq_send_eoi(acknowledge);

	/* Publishes the change to the input layer. */
	if (report)
		mouse_publish(dx, dy, previous, buttons);
}

/* Supports the mouse start operation. */
static int
mouse_start(
	void)
{
	int error;

	/* Installs the handler while the line is still masked. */
	if (!handler_registered) {
		kern_irq_mask(MOUSE_IRQ);
		error = kern_irq_register(MOUSE_IRQ, mouse_interrupt, NULL);

		/* Leaves an inactive, retryable backend on failure. */
		if (error != 0)
			return error;
		handler_registered = 1;
	}

	/* Enables reporting, then lets the hardware and the line run. */
	__atomic_store_n(&mouse_active, 1, __ATOMIC_RELEASE);
	outb(MOUSE_PORT_C, 0x00U);
	kern_irq_unmask(MOUSE_IRQ);

	/* Succeeded. */
	return 0;
}

/* Supports the mouse stop operation. */
static void
mouse_stop(
	void)
{
	/* Quiets the line before the hardware stops producing samples. */
	kern_irq_mask(MOUSE_IRQ);
	__atomic_store_n(&mouse_active, 0, __ATOMIC_RELEASE);
	outb(MOUSE_PORT_C, 0x10U);

	/* Releases any button the last sample left pressed. */
	if (last_buttons != 0) {
		mouse_publish(0, 0, last_buttons, 0);
		last_buttons = 0;
	}
}

/* Supports the mouse input open operation. */
static int
mouse_input_open(
	void *context)
{
	int error = 0;

	(void)context;
	mutex_lock(&lifecycle_lock);

	/* Handles the reader count condition. */
	if (reader_count == UINT_MAX) {
		error = EMFILE;
	} else if (reader_count == 0 && (error = mouse_start()) != 0) {
		/*
		 * A failed first start leaves an inactive, retryable backend.
		 */
		mouse_active = 0;
	} else {
		reader_count++;
	}

	mutex_unlock(&lifecycle_lock);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Supports the mouse input close operation. */
static void
mouse_input_close(
	void *context)
{
	(void)context;
	mutex_lock(&lifecycle_lock);

	/* Handles the reader count condition. */
	if (reader_count != 0 && --reader_count == 0)
		mouse_stop();

	mutex_unlock(&lifecycle_lock);
}

/*
 * Implements the drv pc98 busmouse init operation.
 */
int
drv_pc98_busmouse_init(
	void)
{
	int error;
	const struct input_device_info mouse_info = {
		.name = "NEC PC-98 bus mouse",
		.physical_path = "pc98/ppi-mouse0",
		.id = {.bustype = BUS_HOST, .product = 0x0098, .version = 1},
		.capabilities = mouse_capabilities,
		.capability_count = sizeof(mouse_capabilities) /
				    sizeof(mouse_capabilities[0]),
		.open = mouse_input_open,
		.close = mouse_input_close,
	};

	/*
	 * Leave the periodic mouse IRQ masked until its evdev node is opened.
	 */
	outb(MOUSE_CONTROL, PPI_MODE);
	outb(MOUSE_PORT_C, 0x10U);
	(void)mutex_init(&lifecycle_lock, LOCK_RANK_DEVICE,
			 "pc98 mouse lifecycle");
	mouse_input = NULL;
	last_buttons = 0;
	reader_count = 0;
	handler_registered = 0;
	mouse_active = 0;

	/* Obtains the drv input device register result. */
	error = drv_input_device_register(&mouse_info, &mouse_input);

	/* Returns the computed result. */
	return error;
}
