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
#include "kern/thread.h"
#include "kern/sched.h"

#include <errno.h>
#include <hal/hal.h>
#include <string.h>

#include "hal/i386/i386.h"

#define LGY_IO_BASE       0x00d0U
#define LGY_DATA_PORT     0x02d0U
#define LGY_RESET_PORT    0x03d0U
#define LGY_ID_PORT       0x03daU
#define LGY_IRQ           6
#define LGY_TX_START      0x40U
#define LGY_RX_START      0x46U
#define LGY_STOP          0x80U
#define LGY_DCR           0x49U

static struct dp8390 lgy_dp;
static struct net_device *lgy_device;
static struct thread *lgy_irq_thread;

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

static int
lgy_board_present(void)
{
	static const uint8_t identifier[4] = { 0x00U, 0x40U, 0x26U, 0x0bU };
	unsigned index;

	for (index = 0; index < sizeof(identifier); index++)
		if (asm_inb((uint16_t)(LGY_ID_PORT + index)) != identifier[index])
			return 0;
	return 1;
}

static void
lgy_irq_service(void *argument)
{
	struct dp8390 *dp = argument;

	for (;;) {
		hal_irq_ack_t acknowledge;
		if (hal_irq_service_wait(LGY_IRQ, &acknowledge) != HAL_OK)
			HAL_FATAL("LGY-98 IRQ service wait failed");
		dp8390_interrupt(dp);
		hal_irq_send_eoi(acknowledge);
	}
}

int
zedbsd_pc98_lgy98_init(void)
{
	uint8_t prom[16];
	int error;

	if (!lgy_board_present())
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
	if (error == 0)
		error = net_device_create(lgy_device);
	if (error == 0)
		error = net_device_open(lgy_device);
	if (error == 0)
		error = kthread_create(lgy_irq_service, &lgy_dp,
		    SCHED_PRIORITY_DEFAULT, &lgy_irq_thread);
	if (error == 0) {
		thread_start(lgy_irq_thread);
		return 0;
	}
	if (lgy_device->open_count != 0)
		net_device_close(lgy_device);
	if (net_device_find("ne0") == lgy_device)
		net_device_gone(lgy_device);
	net_device_destroy(lgy_device);
	lgy_device = NULL;
	return error;
}
