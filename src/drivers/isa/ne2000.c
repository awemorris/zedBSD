/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/*
 * PC/AT ISA NE2000 Ethernet driver
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#include "drivers/pcat-ne2000.h"
#include "drivers/dp8390.h"
#include "kern/net/net-device.h"

#include <errno.h>
#include <hal/hal.h>
#include <stdint.h>
#include <string.h>

#define NE2000_IO_BASE 0x0300U
#define NE2000_DATA_OFFSET 0x0010U
#define NE2000_RESET_OFFSET 0x001fU
#define NE2000_IRQ 10
#define NE2000_TX_START 0x40U
#define NE2000_RX_START 0x46U
#define NE2000_STOP 0x80U
#define NE2000_DCR 0x49U
#define NE2000_ISR 0x07U
#define NE2000_ISR_RST 0x80U
#define NE2000_RESET_SPINS 100000U

struct pcat_ne2000 {
	uint16_t io_base;
	unsigned irq;
	struct dp8390 dp;
	struct net_device *device;
};

static struct pcat_ne2000 ne2000;

static uint8_t port_inb(uint16_t port);
static uint16_t port_inw(uint16_t port);
static void port_outb(uint16_t port, uint8_t value);
static void port_outw(uint16_t port, uint16_t value);
static uint8_t ne2000_read_reg(void *cookie, unsigned reg);
static void ne2000_write_reg(void *cookie, unsigned reg, uint8_t value);
static uint8_t ne2000_read_data8(void *cookie);
static uint16_t ne2000_read_data16(void *cookie);
static void ne2000_write_data16(void *cookie, uint16_t value);
static int ne2000_reset(void *cookie);
static void ne2000_irq_handler(int irq, hal_irq_ack_t acknowledge, void *argument);

static const struct dp8390_bus_ops ne2000_bus_ops = {
	.read_reg = ne2000_read_reg,
	.write_reg = ne2000_write_reg,
	.read_data8 = ne2000_read_data8,
	.read_data16 = ne2000_read_data16,
	.write_data16 = ne2000_write_data16,
	.reset = ne2000_reset,
};

/*
 * Implements the drv pcat ne2000 init operation.
 */
int
drv_pcat_ne2000_init(
	void)
{
	struct net_device *registered;
	uint8_t prom[16];
	int error, gone_error = 0;
	int irq_registered = 0;

	/* Describes the card at its fixed address and page layout. */
	memset(&ne2000, 0, sizeof(ne2000));
	ne2000.io_base = NE2000_IO_BASE;
	ne2000.irq = NE2000_IRQ;
	ne2000.dp.bus = &ne2000_bus_ops;
	ne2000.dp.bus_cookie = &ne2000;
	ne2000.dp.tx_start_page = NE2000_TX_START;
	ne2000.dp.rx_start_page = NE2000_RX_START;
	ne2000.dp.stop_page = NE2000_STOP;
	ne2000.dp.dcr = NE2000_DCR;

	/* Checks the operation status. */
	error = drv_dp8390_read_prom(&ne2000.dp, prom);
	if (error != 0)
		return error;
	ne2000.device = net_device_alloc();

	/* Handles the device availability. */
	if (ne2000.device == NULL)
		return ENOSPC;
	strcpy(ne2000.device->name, "ne0");
	ne2000.device->mtu = 1500;
	ne2000.device->hwaddr_len = 6;
	ne2000.device->flags = NET_DEVICE_BROADCAST;
	memcpy(ne2000.device->hwaddr, prom, 6);

	/* Checks the operation status. */
	error = drv_dp8390_attach(&ne2000.dp, ne2000.device);
	if (error == 0)
		error = net_device_create(ne2000.device);

	/* Checks the operation status. */
	if (error == 0) {
		/* Checks the hal irq set handler result. */
		if (hal_irq_set_handler((int)ne2000.irq, ne2000_irq_handler,
					&ne2000) == HAL_OK)
			irq_registered = 1;
		else
			error = EBUSY;
	}

	/* Checks the operation status. */
	if (error == 0)
		error = net_device_open(ne2000.device);

	/* Checks the operation status. */
	if (error == 0) {
		hal_irq_unmask((int)ne2000.irq);

		/* Reports successful completion. */
		return 0;
	}

	hal_irq_mask((int)ne2000.irq);

	/* Handles the irq registered condition. */
	if (irq_registered)
		(void)hal_irq_set_handler((int)ne2000.irq, NULL, NULL);

	/* Handles the ne2000 condition. */
	if (ne2000.device->open_count != 0)
		net_device_close(ne2000.device);

	/* Handles the registered condition. */
	registered = net_device_find_ref("ne0");
	if (registered == ne2000.device)
		gone_error = net_device_gone(ne2000.device);
	net_device_release(registered);

	/* Checks the operation status. */
	if (gone_error != 0)
		return gone_error;
	net_device_destroy(ne2000.device);
	ne2000.device = NULL;

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Supports the port inb operation. */
static uint8_t
port_inb(
	uint16_t port)
{
	uint8_t value;

	__asm__ volatile("inb %w1,%0" : "=a"(value) : "Nd"(port));

	/* Returns the computed result. */
	return value;
}

/* Supports the port inw operation. */
static uint16_t
port_inw(
	uint16_t port)
{
	uint16_t value;

	__asm__ volatile("inw %w1,%0" : "=a"(value) : "Nd"(port));

	/* Returns the computed result. */
	return value;
}

/* Supports the port outb operation. */
static void
port_outb(
	uint16_t port,
	uint8_t value)
{
	__asm__ volatile("outb %0,%w1" : : "a"(value), "Nd"(port));
}

/* Supports the port outw operation. */
static void
port_outw(
	uint16_t port,
	uint16_t value)
{
	__asm__ volatile("outw %0,%w1" : : "a"(value), "Nd"(port));
}

/* Supports the ne2000 read reg operation. */
static uint8_t
ne2000_read_reg(
	void *cookie,
	unsigned reg)
{
	uint8_t function_result;
	struct pcat_ne2000 *state = cookie;

	/* Obtains the port inb result. */
	function_result = port_inb((uint16_t)(state->io_base + reg));

	/* Returns the computed result. */
	return function_result;
}

/* Supports the ne2000 write reg operation. */
static void
ne2000_write_reg(
	void *cookie,
	unsigned reg,
	uint8_t value)
{
	struct pcat_ne2000 *state = cookie;

	port_outb((uint16_t)(state->io_base + reg), value);
}

/* Supports the ne2000 read data8 operation. */
static uint8_t
ne2000_read_data8(
	void *cookie)
{
	uint8_t function_result;
	struct pcat_ne2000 *state = cookie;

	/* Obtains the port inb result. */
	function_result =
		port_inb((uint16_t)(state->io_base + NE2000_DATA_OFFSET));

	/* Returns the computed result. */
	return function_result;
}

/* Supports the ne2000 read data16 operation. */
static uint16_t
ne2000_read_data16(
	void *cookie)
{
	uint16_t function_result;
	struct pcat_ne2000 *state = cookie;

	/* Obtains the port inw result. */
	function_result =
		port_inw((uint16_t)(state->io_base + NE2000_DATA_OFFSET));

	/* Returns the computed result. */
	return function_result;
}

/* Supports the ne2000 write data16 operation. */
static void
ne2000_write_data16(
	void *cookie,
	uint16_t value)
{
	struct pcat_ne2000 *state = cookie;

	port_outw((uint16_t)(state->io_base + NE2000_DATA_OFFSET), value);
}

/* Supports the ne2000 reset operation. */
static int
ne2000_reset(
	void *cookie)
{
	uint16_t isr_port;
	struct pcat_ne2000 *state = cookie;
	uint16_t reset_port = (uint16_t)(state->io_base + NE2000_RESET_OFFSET);
	uint8_t value;
	unsigned spin;

	/* Validates the current value. */
	value = port_inb(reset_port);
	if (value == 0xffU)
		return ENODEV;
	port_outb(reset_port, value);
	/* Process each element required by the operation. */
	for (spin = 0; spin < NE2000_RESET_SPINS; spin++) {
		/* Checks the port inb result. */
		isr_port = (uint16_t)(state->io_base + NE2000_ISR);
		if ((port_inb(isr_port) & NE2000_ISR_RST) != 0) {
			port_outb(isr_port, NE2000_ISR_RST);

			/* Reports successful completion. */
			return 0;
		}
	}

	/* Returns the computed result. */
	return ETIMEDOUT;
}

/* Supports the ne2000 irq handler operation. */
static void
ne2000_irq_handler(
	int irq,
	hal_irq_ack_t acknowledge,
	void *argument)
{
	struct pcat_ne2000 *state = argument;

	(void)irq;
	drv_dp8390_interrupt(&state->dp);
	hal_irq_send_eoi(acknowledge);
}
