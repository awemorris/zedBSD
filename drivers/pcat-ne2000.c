/*
 * PC/AT ISA NE2000 Ethernet driver
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#include "drivers/pcat-ne2000.h"
#include "drivers/dp8390.h"
#include "kern/net/net-device.h"
#include "kern/sched.h"
#include "kern/thread.h"

#include <errno.h>
#include <hal/hal.h>
#include <stdint.h>
#include <string.h>

#define NE2000_IO_BASE        0x0300U
#define NE2000_DATA_OFFSET    0x0010U
#define NE2000_RESET_OFFSET   0x001fU
#define NE2000_IRQ            10
#define NE2000_TX_START       0x40U
#define NE2000_RX_START       0x46U
#define NE2000_STOP           0x80U
#define NE2000_DCR            0x49U
#define NE2000_ISR            0x07U
#define NE2000_ISR_RST        0x80U
#define NE2000_RESET_SPINS    100000U

struct pcat_ne2000 {
	uint16_t io_base;
	unsigned irq;
	struct dp8390 dp;
	struct net_device *device;
	struct thread *irq_thread;
};

static struct pcat_ne2000 ne2000;

static uint8_t
port_inb(uint16_t port)
{
	uint8_t value;

	__asm__ volatile("inb %w1,%0" : "=a"(value) : "Nd"(port));
	return value;
}

static uint16_t
port_inw(uint16_t port)
{
	uint16_t value;

	__asm__ volatile("inw %w1,%0" : "=a"(value) : "Nd"(port));
	return value;
}

static void
port_outb(uint16_t port, uint8_t value)
{
	__asm__ volatile("outb %0,%w1" : : "a"(value), "Nd"(port));
}

static void
port_outw(uint16_t port, uint16_t value)
{
	__asm__ volatile("outw %0,%w1" : : "a"(value), "Nd"(port));
}

static uint8_t
ne2000_read_reg(void *cookie, unsigned reg)
{
	struct pcat_ne2000 *state = cookie;

	return port_inb((uint16_t)(state->io_base + reg));
}

static void
ne2000_write_reg(void *cookie, unsigned reg, uint8_t value)
{
	struct pcat_ne2000 *state = cookie;

	port_outb((uint16_t)(state->io_base + reg), value);
}

static uint8_t
ne2000_read_data8(void *cookie)
{
	struct pcat_ne2000 *state = cookie;

	return port_inb((uint16_t)(state->io_base + NE2000_DATA_OFFSET));
}

static uint16_t
ne2000_read_data16(void *cookie)
{
	struct pcat_ne2000 *state = cookie;

	return port_inw((uint16_t)(state->io_base + NE2000_DATA_OFFSET));
}

static void
ne2000_write_data16(void *cookie, uint16_t value)
{
	struct pcat_ne2000 *state = cookie;

	port_outw((uint16_t)(state->io_base + NE2000_DATA_OFFSET), value);
}

static int
ne2000_reset(void *cookie)
{
	struct pcat_ne2000 *state = cookie;
	uint16_t reset_port = (uint16_t)(state->io_base + NE2000_RESET_OFFSET);
	uint8_t value;
	unsigned spin;

	value = port_inb(reset_port);
	if (value == 0xffU)
		return ENODEV;
	port_outb(reset_port, value);
	for (spin = 0; spin < NE2000_RESET_SPINS; spin++) {
		uint16_t isr_port = (uint16_t)(state->io_base + NE2000_ISR);

		if ((port_inb(isr_port) & NE2000_ISR_RST) != 0) {
			port_outb(isr_port, NE2000_ISR_RST);
			return 0;
		}
	}
	return ETIMEDOUT;
}

static const struct dp8390_bus_ops ne2000_bus_ops = {
	.read_reg = ne2000_read_reg,
	.write_reg = ne2000_write_reg,
	.read_data8 = ne2000_read_data8,
	.read_data16 = ne2000_read_data16,
	.write_data16 = ne2000_write_data16,
	.reset = ne2000_reset,
};

static void
ne2000_irq_service(void *argument)
{
	struct pcat_ne2000 *state = argument;

	for (;;) {
		hal_irq_ack_t acknowledge;
		if (hal_irq_service_wait((int)state->irq, &acknowledge) != HAL_OK)
			HAL_FATAL("NE2000 IRQ service wait failed");
		dp8390_interrupt(&state->dp);
		hal_irq_send_eoi(acknowledge);
	}
}

int
zedbsd_pcat_ne2000_init(void)
{
	uint8_t prom[16];
	int error;

	memset(&ne2000, 0, sizeof(ne2000));
	ne2000.io_base = NE2000_IO_BASE;
	ne2000.irq = NE2000_IRQ;
	ne2000.dp.bus = &ne2000_bus_ops;
	ne2000.dp.bus_cookie = &ne2000;
	ne2000.dp.tx_start_page = NE2000_TX_START;
	ne2000.dp.rx_start_page = NE2000_RX_START;
	ne2000.dp.stop_page = NE2000_STOP;
	ne2000.dp.dcr = NE2000_DCR;

	error = dp8390_read_prom(&ne2000.dp, prom);
	if (error != 0)
		return error;
	ne2000.device = net_device_alloc();
	if (ne2000.device == NULL)
		return ENOSPC;
	strcpy(ne2000.device->name, "ne0");
	ne2000.device->mtu = 1500;
	ne2000.device->hwaddr_len = 6;
	ne2000.device->flags = NET_DEVICE_BROADCAST;
	memcpy(ne2000.device->hwaddr, prom, 6);
	error = dp8390_attach(&ne2000.dp, ne2000.device);
	if (error == 0)
		error = net_device_create(ne2000.device);
	if (error == 0)
		error = net_device_open(ne2000.device);
	if (error == 0)
		error = kthread_create(ne2000_irq_service, &ne2000,
		    SCHED_PRIORITY_DEFAULT, &ne2000.irq_thread);
	if (error == 0) {
		thread_start(ne2000.irq_thread);
		return 0;
	}
	if (ne2000.device->open_count != 0)
		net_device_close(ne2000.device);
	{
		struct net_device *registered = net_device_find_ref("ne0");
		if (registered == ne2000.device)
			net_device_gone(ne2000.device);
		net_device_release(registered);
	}
	net_device_destroy(ne2000.device);
	ne2000.device = NULL;
	return error;
}
