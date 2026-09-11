/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Melco LGY-98 Ethernet driver
 * LGY-98 is a C-bus dp8390-compatible board manufactured by Melco.
 */

#include "drivers/pc98-lgy98.h"
#include "drivers/dp8390.h"
#include "kern/net/net-device.h"

#include <errno.h>
#include <hal/hal.h>
#include <string.h>

#include "hal/i386/i386.h"
#include "kern/irq.h"

#define LGY_IO_BASE 0x00d0U
#define LGY_DATA_PORT 0x02d0U
#define LGY_RESET_PORT 0x03d0U
#define LGY_IRQ 6
#define LGY_TX_START 0x40U
#define LGY_RX_START 0x46U
#define LGY_STOP 0x80U
#define LGY_DCR 0x49U

static struct dp8390 lgy_dp;
static struct net_device *lgy_device;
static const struct net_device_ops *lgy_dp_ops;

static uint8_t lgy_read_reg(void *cookie, unsigned reg);
static void lgy_write_reg(void *cookie, unsigned reg, uint8_t value);
static uint8_t lgy_read_data8(void *cookie);
static uint16_t lgy_read_data16(void *cookie);
static void lgy_write_data16(void *cookie, uint16_t value);
static int lgy_reset(void *cookie);
static void lgy_irq_handler(int irq, kern_irq_ack_t acknowledge, void *argument);
static int lgy_open(struct net_device *device);
static void lgy_close(struct net_device *device);
static int lgy_transmit(struct net_device *device, struct packet_buf *packet);
static unsigned lgy_poll_receive(struct net_device *device, unsigned budget);

static const struct dp8390_bus_ops lgy_bus_ops = {
	.read_reg = lgy_read_reg,
	.write_reg = lgy_write_reg,
	.read_data8 = lgy_read_data8,
	.read_data16 = lgy_read_data16,
	.write_data16 = lgy_write_data16,
	.reset = lgy_reset,
};

static const struct net_device_ops lgy_net_ops = {
	.open = lgy_open,
	.close = lgy_close,
	.transmit = lgy_transmit,
	.poll_receive = lgy_poll_receive,
};

/*
 * Implements the drv pc98 lgy98 init operation.
 */
int
drv_pc98_lgy98_init(
	void)
{
	struct net_device *registered;
	uint8_t prom[16];
	int error, gone_error = 0;
	int irq_registered = 0;

	/*
	 * An unused C-bus port reads as 0xff.  Avoid modifying unrelated ports.
	 */
	if (asm_inb(LGY_IO_BASE) == 0xffU)
		return ENODEV;
	memset(&lgy_dp, 0, sizeof(lgy_dp));
	lgy_dp.bus = &lgy_bus_ops;
	lgy_dp.tx_start_page = LGY_TX_START;
	lgy_dp.rx_start_page = LGY_RX_START;
	lgy_dp.stop_page = LGY_STOP;
	lgy_dp.dcr = LGY_DCR;

	/* Checks the operation status. */
	error = drv_dp8390_read_prom(&lgy_dp, prom);
	if (error != 0)
		return error;

	/* Handles the lgy device availability. */
	lgy_device = net_device_alloc();
	if (lgy_device == NULL)
		return ENOSPC;
	strcpy(lgy_device->name, "ne0");
	lgy_device->mtu = 1500;
	lgy_device->hwaddr_len = 6;
	lgy_device->flags = NET_DEVICE_BROADCAST;
	memcpy(lgy_device->hwaddr, prom, 6);

	/* Checks the operation status. */
	error = drv_dp8390_attach(&lgy_dp, lgy_device);
	if (error == 0) {
		lgy_dp_ops = lgy_device->ops;
		lgy_device->ops = &lgy_net_ops;
	}

	/* Checks the operation status. */
	if (error == 0)
		error = net_device_create(lgy_device);
	if (error == 0) {
		kern_irq_mask(LGY_IRQ);

		/* Checks the hal irq set handler result. */
		if (kern_irq_register(LGY_IRQ, lgy_irq_handler, &lgy_dp) ==
		    0)
			irq_registered = 1;
		else
			error = EBUSY;
	}

	/* Checks the operation status. */
	if (error == 0)
		return 0;
	kern_irq_mask(LGY_IRQ);

	/* Handles the irq registered condition. */
	if (irq_registered)
		(void)kern_irq_register(LGY_IRQ, NULL, NULL);

	/* Handles the lgy device condition. */
	if (lgy_device->open_count != 0)
		net_device_close(lgy_device);

	/* Handles the registered condition. */
	registered = net_device_find_ref("ne0");
	if (registered == lgy_device)
		gone_error = net_device_gone(lgy_device);
	net_device_release(registered);
	if (gone_error != 0)
		return gone_error;
	net_device_destroy(lgy_device);
	lgy_device = NULL;

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Supports the lgy read reg operation. */
static uint8_t
lgy_read_reg(
	void *cookie,
	unsigned reg)
{
	uint8_t function_result;

	(void)cookie;

	/* Obtains the asm inb result. */
	function_result = asm_inb((uint16_t)(LGY_IO_BASE + reg));

	/* Returns the computed result. */
	return function_result;
}

/* Supports the lgy write reg operation. */
static void
lgy_write_reg(
	void *cookie,
	unsigned reg,
	uint8_t value)
{
	(void)cookie;
	asm_outb((uint16_t)(LGY_IO_BASE + reg), value);
}

/* Supports the lgy read data8 operation. */
static uint8_t
lgy_read_data8(
	void *cookie)
{
	uint8_t function_result;

	(void)cookie;

	/* Obtains the asm inb result. */
	function_result = asm_inb(LGY_DATA_PORT);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the lgy read data16 operation. */
static uint16_t
lgy_read_data16(
	void *cookie)
{
	uint16_t function_result;

	(void)cookie;

	/* Obtains the asm inw result. */
	function_result = asm_inw(LGY_DATA_PORT);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the lgy write data16 operation. */
static void
lgy_write_data16(
	void *cookie,
	uint16_t value)
{
	(void)cookie;
	asm_outw(LGY_DATA_PORT, value);
}

/* Supports the lgy reset operation. */
static int
lgy_reset(
	void *cookie)
{
	unsigned spin;
	uint8_t value;

	(void)cookie;
	value = asm_inb(LGY_RESET_PORT);
	asm_outb(LGY_RESET_PORT, value);
	/* Process each element required by the operation. */
	for (spin = 0; spin < 100000U; spin++) {
		/* Checks the asm inb result. */
		if ((asm_inb(LGY_IO_BASE + 7U) & 0x80U) != 0) {
			asm_outb(LGY_IO_BASE + 7U, 0x80U);

			/* Succeeded. */
			return 0;
		}
	}

	/* Failed. */
	return ETIMEDOUT;
}

/* Supports the lgy irq handler operation. */
static void
lgy_irq_handler(
	int irq,
	kern_irq_ack_t acknowledge,
	void *argument)
{
	struct dp8390 *dp = argument;

	(void)irq;
	drv_dp8390_interrupt(dp);
	kern_irq_send_eoi(acknowledge);
}

/* Supports the lgy open operation. */
static int
lgy_open(
	struct net_device *device)
{
	int error = lgy_dp_ops->open != NULL ? lgy_dp_ops->open(device) : 0;

	/* Checks the operation status. */
	if (error == 0)
		kern_irq_unmask(LGY_IRQ);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Supports the lgy close operation. */
static void
lgy_close(
	struct net_device *device)
{
	kern_irq_mask(LGY_IRQ);

	/* Handles the close availability. */
	if (lgy_dp_ops->close != NULL)
		lgy_dp_ops->close(device);
}

/* Supports the lgy transmit operation. */
static int
lgy_transmit(
	struct net_device *device,
	struct packet_buf *packet)
{
	int error;

	/* Computes the function result. */
	error = lgy_dp_ops->transmit(device, packet);

	/* Returns the computed result. */
	return error;
}

/* Supports the lgy poll receive operation. */
static unsigned
lgy_poll_receive(
	struct net_device *device,
	unsigned budget)
{
	unsigned function_result;

	/* Computes the function result. */
	function_result = lgy_dp_ops->poll_receive != NULL
				  ? lgy_dp_ops->poll_receive(device, budget)
				  : 0;

	/* Returns the computed result. */
	return function_result;
}
