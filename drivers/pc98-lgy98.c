/*
 * Melco LGY-98 Ethernet driver
 * Copyright (C) 2026 Awe Morris
 *
 * LGY-98 is a C-bus dp8390-compatible board manufactured by Melco.
 *
 * SPDX-License-Identifier: Zlib
 */

#include "drivers/pc98-lgy98.h"
#include "drivers/dp8390.h"
#include "kern/net/net-device.h"

#include <errno.h>
#include <hal/hal.h>
#include <string.h>

#include "hal/i386/i386.h"

#define LGY_IO_BASE       0x00d0U
#define LGY_DATA_PORT     0x02d0U
#define LGY_RESET_PORT    0x03d0U
#define LGY_IRQ           6
#define LGY_TX_START      0x40U
#define LGY_RX_START      0x46U
#define LGY_STOP          0x80U
#define LGY_DCR           0x49U

static struct dp8390 lgy_dp;
static struct net_device *lgy_device;
static const struct net_device_ops *lgy_dp_ops;

static uint8_t lgy_read_reg(void *cookie, unsigned reg)
{
	(void)cookie;
	return asm_inb((uint16_t)(LGY_IO_BASE + reg));
}

static void lgy_write_reg(void *cookie, unsigned reg, uint8_t value)
{
	(void)cookie;
	asm_outb((uint16_t)(LGY_IO_BASE + reg), value);
}

static uint8_t lgy_read_data8(void *cookie)
{
	(void)cookie;
	return asm_inb(LGY_DATA_PORT);
}

static uint16_t lgy_read_data16(void *cookie)
{
	(void)cookie;
	return asm_inw(LGY_DATA_PORT);
}

static void lgy_write_data16(void *cookie, uint16_t value)
{
	(void)cookie;
	asm_outw(LGY_DATA_PORT, value);
}

static int
lgy_reset(void *cookie)
{
	unsigned spin;
	uint8_t value;

	(void)cookie;
	value = asm_inb(LGY_RESET_PORT);
	asm_outb(LGY_RESET_PORT, value);
	for (spin = 0; spin < 100000U; spin++)
		if ((asm_inb(LGY_IO_BASE + 7U) & 0x80U) != 0) {
			asm_outb(LGY_IO_BASE + 7U, 0x80U);
			return 0;
		}
	return ETIMEDOUT;
}

static const struct dp8390_bus_ops lgy_bus_ops = {
	.read_reg = lgy_read_reg,
	.write_reg = lgy_write_reg,
	.read_data8 = lgy_read_data8,
	.read_data16 = lgy_read_data16,
	.write_data16 = lgy_write_data16,
	.reset = lgy_reset,
};

static void
lgy_irq_handler(int irq, hal_irq_ack_t acknowledge, void *argument)
{
	struct dp8390 *dp = argument;

	(void)irq;
	dp8390_interrupt(dp);
	hal_irq_send_eoi(acknowledge);
}

static int
lgy_open(struct net_device *device)
{
	int error = lgy_dp_ops->open != NULL ? lgy_dp_ops->open(device) : 0;

	if (error == 0)
		hal_irq_unmask(LGY_IRQ);
	return error;
}

static void
lgy_close(struct net_device *device)
{
	hal_irq_mask(LGY_IRQ);
	if (lgy_dp_ops->close != NULL)
		lgy_dp_ops->close(device);
}

static int
lgy_transmit(struct net_device *device, struct packet_buf *packet)
{
	return lgy_dp_ops->transmit(device, packet);
}

static unsigned
lgy_poll_receive(struct net_device *device, unsigned budget)
{
	return lgy_dp_ops->poll_receive != NULL ?
		lgy_dp_ops->poll_receive(device, budget) : 0;
}

static const struct net_device_ops lgy_net_ops = {
	.open = lgy_open,
	.close = lgy_close,
	.transmit = lgy_transmit,
	.poll_receive = lgy_poll_receive,
};

int
zedbsd_pc98_lgy98_init(void)
{
	uint8_t prom[16];
	int error;
	int irq_registered = 0;

	/* An unused C-bus port reads as 0xff.  Avoid modifying unrelated ports. */
	if (asm_inb(LGY_IO_BASE) == 0xffU)
		return ENODEV;
	memset(&lgy_dp, 0, sizeof(lgy_dp));
	lgy_dp.bus = &lgy_bus_ops;
	lgy_dp.tx_start_page = LGY_TX_START;
	lgy_dp.rx_start_page = LGY_RX_START;
	lgy_dp.stop_page = LGY_STOP;
	lgy_dp.dcr = LGY_DCR;
	error = dp8390_read_prom(&lgy_dp, prom);
	if (error != 0)
		return error;
	lgy_device = net_device_alloc();
	if (lgy_device == NULL)
		return ENOSPC;
	strcpy(lgy_device->name, "ne0");
	lgy_device->mtu = 1500;
	lgy_device->hwaddr_len = 6;
	lgy_device->flags = NET_DEVICE_BROADCAST;
	memcpy(lgy_device->hwaddr, prom, 6);
	error = dp8390_attach(&lgy_dp, lgy_device);
	if (error == 0) {
		lgy_dp_ops = lgy_device->ops;
		lgy_device->ops = &lgy_net_ops;
	}
	if (error == 0)
		error = net_device_create(lgy_device);
	if (error == 0) {
		hal_irq_mask(LGY_IRQ);
		if (hal_irq_set_handler(LGY_IRQ, lgy_irq_handler, &lgy_dp) ==
		    HAL_OK)
			irq_registered = 1;
		else
			error = EBUSY;
	}
	if (error == 0)
		return 0;
	hal_irq_mask(LGY_IRQ);
	if (irq_registered)
		(void)hal_irq_set_handler(LGY_IRQ, NULL, NULL);
	if (lgy_device->open_count != 0)
		net_device_close(lgy_device);
	{
		struct net_device *registered = net_device_find_ref("ne0");
		if (registered == lgy_device)
			net_device_gone(lgy_device);
		net_device_release(registered);
	}
	net_device_destroy(lgy_device);
	lgy_device = NULL;
	return error;
}
