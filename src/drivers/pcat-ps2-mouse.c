/*
 * IBM PC/AT i8042 PS/2 mouse driver
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include "drivers/pcat-ps2-mouse.h"
#include "kern/lock.h"
#include "kern/mouse-device.h"

#include <errno.h>
#include <hal/hal.h>
#include <stdint.h>
#include <zedbsd/mouse.h>

#define I8042_DATA             0x60U
#define I8042_STATUS           0x64U
#define I8042_COMMAND          0x64U
#define I8042_STATUS_OUTPUT    0x01U
#define I8042_STATUS_INPUT     0x02U
#define I8042_STATUS_AUX       0x20U

#define I8042_READ_CONFIG      0x20U
#define I8042_WRITE_CONFIG     0x60U
#define I8042_ENABLE_AUX       0xa8U
#define I8042_DISABLE_AUX      0xa7U
#define I8042_WRITE_AUX        0xd4U
#define I8042_CONFIG_AUX_IRQ   0x02U
#define I8042_CONFIG_AUX_OFF   0x20U

#define PS2_SET_DEFAULTS       0xf6U
#define PS2_DISABLE_STREAM     0xf5U
#define PS2_ENABLE_STREAM      0xf4U
#define PS2_ACK                0xfaU
#define PS2_RESEND             0xfeU
#define PS2_MOUSE_IRQ          12
#define PS2_WAIT_LOOPS         100000U

static struct spinlock controller_lock;
static uint8_t packet[3];
static unsigned packet_index;
static uint32_t last_buttons;
static int mouse_active;

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

static int
wait_input_empty(void)
{
	unsigned spin;
	for (spin = 0; spin < PS2_WAIT_LOOPS; spin++) {
		if ((inb(I8042_STATUS) & I8042_STATUS_INPUT) == 0)
			return 0;
		__asm__ volatile("pause");
	}
	return ETIMEDOUT;
}

static int
write_command(uint8_t command)
{
	int error = wait_input_empty();
	if (error != 0)
		return error;
	outb(I8042_COMMAND, command);
	return 0;
}

static int
write_data(uint8_t value)
{
	int error = wait_input_empty();
	if (error != 0)
		return error;
	outb(I8042_DATA, value);
	return 0;
}

static int
read_output(uint8_t *value, int auxiliary)
{
	unsigned spin;
	for (spin = 0; spin < PS2_WAIT_LOOPS; spin++) {
		uint8_t status = inb(I8042_STATUS);
		if ((status & I8042_STATUS_OUTPUT) != 0) {
			uint8_t data = inb(I8042_DATA);
			if (((status & I8042_STATUS_AUX) != 0) == auxiliary) {
				*value = data;
				return 0;
			}
			/* A byte from the other 8042 port cannot be left in front
			 * of the response being polled.  Controller transactions are
			 * short, so at most a concurrently typed scan code is lost. */
		}
		__asm__ volatile("pause");
	}
	return ETIMEDOUT;
}

static void
flush_output(void)
{
	unsigned count;
	for (count = 0; count < 64U; count++) {
		if ((inb(I8042_STATUS) & I8042_STATUS_OUTPUT) == 0)
			break;
		(void)inb(I8042_DATA);
	}
}

static int
read_config(uint8_t *configuration)
{
	int error = write_command(I8042_READ_CONFIG);
	if (error != 0)
		return error;
	return read_output(configuration, 0);
}

static int
write_config(uint8_t configuration)
{
	int error = write_command(I8042_WRITE_CONFIG);
	if (error != 0)
		return error;
	return write_data(configuration);
}

static int
mouse_command(uint8_t command)
{
	unsigned attempt;
	for (attempt = 0; attempt < 3U; attempt++) {
		uint8_t response;
		int error = write_command(I8042_WRITE_AUX);
		if (error == 0)
			error = write_data(command);
		if (error == 0)
			error = read_output(&response, 1);
		if (error != 0)
			return error;
		if (response == PS2_ACK)
			return 0;
		if (response != PS2_RESEND)
			return EIO;
	}
	return EIO;
}

static int
consume_byte(uint8_t value, int32_t *dx, int32_t *dy, uint32_t *buttons)
{
	uint8_t first;
	if (packet_index == 0 && (value & 0x08U) == 0)
		return 0;
	packet[packet_index++] = value;
	if (packet_index != 3U)
		return 0;
	packet_index = 0;
	first = packet[0];
	if ((first & 0xc0U) != 0)
		return 0;
	*dx = (int8_t)packet[1];
	/* PS/2 positive Y is upwards; /dev/mouse positive Y is downwards. */
	*dy = -(int32_t)(int8_t)packet[2];
	*buttons = 0;
	if ((first & 0x01U) != 0)
		*buttons |= ZEDBSD_MOUSE_BUTTON_LEFT;
	if ((first & 0x04U) != 0)
		*buttons |= ZEDBSD_MOUSE_BUTTON_MIDDLE;
	if ((first & 0x02U) != 0)
		*buttons |= ZEDBSD_MOUSE_BUTTON_RIGHT;
	return 1;
}

static void
mouse_interrupt(int interrupt, hal_irq_ack_t acknowledge, void *argument)
{
	unsigned long irq;
	uint8_t status;
	int32_t dx = 0, dy = 0;
	uint32_t buttons = 0;
	int report = 0;

	(void)interrupt;
	(void)argument;
	irq = spin_lock_irqsave(&controller_lock);
	status = inb(I8042_STATUS);
	if ((status & (I8042_STATUS_OUTPUT | I8042_STATUS_AUX)) ==
	    (I8042_STATUS_OUTPUT | I8042_STATUS_AUX)) {
		uint8_t value = inb(I8042_DATA);
		int complete = mouse_active ? consume_byte(value, &dx, &dy,
		    &buttons) : 0;
		if (complete && (dx != 0 || dy != 0 ||
		    buttons != last_buttons)) {
			last_buttons = buttons;
			report = 1;
		}
	}
	spin_unlock_irqrestore(&controller_lock, irq);
	/* Read one byte per edge.  The 8042 lowers IRQ12 when its output
	 * buffer is read, allowing the next packet byte to create a fresh edge
	 * after EOI instead of being stranded while a task IRQ is masked. */
	hal_irq_send_eoi(acknowledge);
	if (report)
		mouse_input_report(0, dx, dy, buttons);
}

static int
mouse_start(void)
{
	unsigned long irq;
	uint8_t configuration = 0;
	int error;

	hal_irq_mask(PS2_MOUSE_IRQ);
	irq = spin_lock_irqsave(&controller_lock);
	mouse_active = 0;
	packet_index = 0;
	last_buttons = 0;
	flush_output();
	error = write_command(I8042_ENABLE_AUX);
	if (error == 0)
		error = read_config(&configuration);
	if (error == 0) {
		configuration &= (uint8_t)~(I8042_CONFIG_AUX_IRQ |
		    I8042_CONFIG_AUX_OFF);
		error = write_config(configuration);
	}
	if (error == 0)
		error = mouse_command(PS2_SET_DEFAULTS);
	if (error == 0)
		error = mouse_command(PS2_ENABLE_STREAM);
	if (error == 0) {
		configuration |= I8042_CONFIG_AUX_IRQ;
		error = write_config(configuration);
	}
	if (error == 0)
		mouse_active = 1;
	else {
		(void)write_command(I8042_DISABLE_AUX);
		if (error == ETIMEDOUT)
			error = ENODEV;
	}
	spin_unlock_irqrestore(&controller_lock, irq);
	if (error == 0)
		hal_irq_unmask(PS2_MOUSE_IRQ);
	return error;
}

static void
mouse_stop(void)
{
	unsigned long irq;
	uint8_t configuration;

	hal_irq_mask(PS2_MOUSE_IRQ);
	irq = spin_lock_irqsave(&controller_lock);
	mouse_active = 0;
	packet_index = 0;
	(void)mouse_command(PS2_DISABLE_STREAM);
	if (read_config(&configuration) == 0) {
		configuration &= (uint8_t)~I8042_CONFIG_AUX_IRQ;
		(void)write_config(configuration);
	}
	(void)write_command(I8042_DISABLE_AUX);
	flush_output();
	spin_unlock_irqrestore(&controller_lock, irq);
}

int
pcat_ps2_mouse_init(void)
{
	int error;

	spin_init(&controller_lock, LOCK_RANK_DEVICE, "i8042 mouse");
	packet_index = 0;
	last_buttons = 0;
	mouse_active = 0;
	hal_irq_mask(PS2_MOUSE_IRQ);
	if (hal_irq_set_handler(PS2_MOUSE_IRQ, mouse_interrupt, NULL) != HAL_OK)
		return EBUSY;
	error = mouse_device_set_backend(mouse_start, mouse_stop);
	if (error != 0)
		(void)hal_irq_set_handler(PS2_MOUSE_IRQ, NULL, NULL);
	return error;
}
