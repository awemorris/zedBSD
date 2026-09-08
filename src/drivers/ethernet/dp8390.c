/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Common dp8390 Ethernet driver
 */

#include "drivers/dp8390.h"
#include "kern/net/net-device.h"
#include "kern/net/packet-buf.h"

#include <errno.h>
#include <string.h>

/* DP8390 page-zero registers. */
#define DP_CR 0x00U
#define DP_PSTART 0x01U
#define DP_PSTOP 0x02U
#define DP_BNRY 0x03U
#define DP_TPSR 0x04U
#define DP_TBCR0 0x05U
#define DP_TBCR1 0x06U
#define DP_ISR 0x07U
#define DP_RSAR0 0x08U
#define DP_RSAR1 0x09U
#define DP_RBCR0 0x0aU
#define DP_RBCR1 0x0bU
#define DP_RCR 0x0cU
#define DP_TCR 0x0dU
#define DP_DCR 0x0eU
#define DP_IMR 0x0fU

/* Page-one registers. */
#define DP_PAR0 0x01U
#define DP_CURR 0x07U
#define DP_MAR0 0x08U

#define DP_CR_STOP 0x01U
#define DP_CR_START 0x02U
#define DP_CR_TXP 0x04U
#define DP_CR_RREAD 0x08U
#define DP_CR_RWRITE 0x10U
#define DP_CR_NODMA 0x20U
#define DP_CR_PAGE0 0x00U
#define DP_CR_PAGE1 0x40U

#define DP_ISR_PRX 0x01U
#define DP_ISR_PTX 0x02U
#define DP_ISR_RXE 0x04U
#define DP_ISR_TXE 0x08U
#define DP_ISR_OVW 0x10U
#define DP_ISR_CNT 0x20U
#define DP_ISR_RDC 0x40U
#define DP_ISR_RST 0x80U

#define DP_RCR_AB 0x04U
#define DP_RCR_MON 0x20U
#define DP_TCR_LB0 0x02U
#define DP_IMR_RUN                                                             \
	(DP_ISR_PRX | DP_ISR_PTX | DP_ISR_RXE | DP_ISR_TXE | DP_ISR_OVW)
#define DP_DMA_SPINS 100000U
#define DP_MIN_FRAME 60U
#define DP_MAX_FRAME 1518U

struct dp_receive_header {
	uint8_t status;
	uint8_t next;
	uint8_t count_low;
	uint8_t count_high;
};

static void wr(struct dp8390 *dp, unsigned reg, uint8_t value);
static void dma_begin(struct dp8390 *dp, uint16_t address, size_t length, uint8_t command);
static int wait_rdc(struct dp8390 *dp);
static uint8_t rd(struct dp8390 *dp, unsigned reg);
static int dp_start_transmit(struct dp8390 *dp, struct packet_buf *packet);
static int dma_write(struct dp8390 *dp, uint16_t address, const uint8_t *buffer, size_t length);
static int dma_read(struct dp8390 *dp, uint16_t address, void *buffer, size_t length);
static void chip_stop(struct dp8390 *dp);
static int chip_start(struct dp8390 *dp);
static int dp_open(struct net_device *device);
static void dp_close(struct net_device *device);
static int dp_transmit(struct net_device *device, struct packet_buf *packet);
static uint8_t current_page(struct dp8390 *dp);
static void advance_ring(struct dp8390 *dp, uint8_t next);
static int read_frame(struct dp8390 *dp, uint8_t page, struct packet_buf **result, uint8_t *next_result);
static unsigned dp_poll_receive(struct net_device *device, unsigned budget);

static const struct net_device_ops dp_ops = {
	.open = dp_open,
	.close = dp_close,
	.transmit = dp_transmit,
	.poll_receive = dp_poll_receive,
};

/*
 * Implements the drv dp8390 read prom operation.
 */
int
drv_dp8390_read_prom(
	struct dp8390 *dp,
	uint8_t prom[16])
{
	uint8_t raw[32];
	unsigned index;
	int error;

	/* Handles the dp availability. */
	if (dp == NULL || dp->bus == NULL || prom == NULL)
		return EINVAL;

	/* Checks the reset result. */
	if (dp->bus->reset != NULL && dp->bus->reset(dp->bus_cookie) != 0)
		return ENODEV;
	wr(dp, DP_CR, DP_CR_STOP | DP_CR_NODMA | DP_CR_PAGE0);
	wr(dp, DP_DCR, 0x48U);
	wr(dp, DP_RBCR0, 0);
	wr(dp, DP_RBCR1, 0);
	wr(dp, DP_IMR, 0);
	wr(dp, DP_ISR, 0xffU);
	wr(dp, DP_RCR, DP_RCR_MON);
	wr(dp, DP_TCR, DP_TCR_LB0);
	dma_begin(dp, 0, sizeof(raw), DP_CR_RREAD);
	/* Process each remaining element. */
	for (index = 0; index < sizeof(raw); index++)
		raw[index] = dp->bus->read_data8(dp->bus_cookie);
	error = wait_rdc(dp);

	/* Probing must not leave an unconfigured adapter running. */
	wr(dp, DP_IMR, 0);
	wr(dp, DP_CR, DP_CR_STOP | DP_CR_NODMA | DP_CR_PAGE0);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	/* Process each remaining element. */
	for (index = 0; index < 16U; index++) {
		/* Handles the raw condition. */
		if (raw[index * 2U] != raw[index * 2U + 1U])
			return ENODEV;
		prom[index] = raw[index * 2U];
	}

	/* Handles the prom condition. */
	if (prom[14] != 0x57U || prom[15] != 0x57U)
		return ENODEV;
	wr(dp, DP_DCR, dp->dcr);

	/* Succeeded. */
	return 0;
}

/*
 * Implements the drv dp8390 attach operation.
 */
int
drv_dp8390_attach(
	struct dp8390 *dp,
	struct net_device *device)
{
	/* Handles the dp availability. */
	if (dp == NULL || device == NULL || dp->bus == NULL ||
	    dp->bus->read_reg == NULL || dp->bus->write_reg == NULL ||
	    dp->bus->read_data8 == NULL || dp->bus->read_data16 == NULL ||
	    dp->bus->write_data16 == NULL || dp->rx_start_page == 0 ||
	    dp->rx_start_page >= dp->stop_page) {
		/* Failed. */
		return EINVAL;
	}
	dp->device = device;
	spin_init(&dp->lock, LOCK_RANK_DEVICE, "dp8390");
	device->driver_data = dp;
	device->ops = &dp_ops;

	/* Succeeded. */
	return 0;
}

/*
 * Implements the drv dp8390 interrupt operation.
 */
void
drv_dp8390_interrupt(
	struct dp8390 *dp)
{
	struct packet_buf *packet;
	unsigned long irq;
	int schedule_poll = 0;
	uint8_t status;

	/* Handles the dp availability. */
	if (dp == NULL)
		return;
	irq = spin_lock_irqsave(&dp->lock);

	/* Handles the dp condition. */
	if (!dp->opened)
		goto out;

	/* Checks the operation status. */
	status = rd(dp, DP_ISR);
	if (status == 0 || status == 0xffU)
		goto out;
	wr(dp, DP_ISR, status);
	if ((status & (DP_ISR_PTX | DP_ISR_TXE)) != 0)
		dp->tx_busy = 0;

	/* Handles the tx pending availability. */
	if (!dp->tx_busy && dp->tx_pending != NULL) {
		packet = dp->tx_pending;

		dp->tx_pending = NULL;

		/* Checks the dp start transmit result. */
		if (dp_start_transmit(dp, packet) != 0)
			dp->device->tx_errors++;
	}

	/* Checks the operation status. */
	if ((status & (DP_ISR_PRX | DP_ISR_RXE | DP_ISR_OVW)) != 0)
		schedule_poll = 1;

	/* Checks the operation status. */
	if ((status & DP_ISR_CNT) != 0) {
		(void)rd(dp, 0x0dU);
		(void)rd(dp, 0x0eU);
		(void)rd(dp, 0x0fU);
	}

out:

	spin_unlock_irqrestore(&dp->lock, irq);

	/* Handles the schedule poll condition. */
	if (schedule_poll)
		net_device_schedule_poll(dp->device);
}

/* Supports the wr operation. */
static void
wr(
	struct dp8390 *dp,
	unsigned reg,
	uint8_t value)
{
	dp->bus->write_reg(dp->bus_cookie, reg, value);
}

/* Supports the dma begin operation. */
static void
dma_begin(
	struct dp8390 *dp,
	uint16_t address,
	size_t length,
	uint8_t command)
{
	wr(dp, DP_CR, DP_CR_START | DP_CR_NODMA | DP_CR_PAGE0);
	wr(dp, DP_ISR, DP_ISR_RDC);
	wr(dp, DP_RBCR0, (uint8_t)length);
	wr(dp, DP_RBCR1, (uint8_t)(length >> 8));
	wr(dp, DP_RSAR0, (uint8_t)address);
	wr(dp, DP_RSAR1, (uint8_t)(address >> 8));
	wr(dp, DP_CR, DP_CR_START | command | DP_CR_PAGE0);
}

/* Supports the wait rdc operation. */
static int
wait_rdc(
	struct dp8390 *dp)
{
	unsigned spin;

	/* Process each element required by the operation. */
	for (spin = 0; spin < DP_DMA_SPINS; spin++) {
		/* Checks the rd result. */
		if ((rd(dp, DP_ISR) & DP_ISR_RDC) != 0) {
			wr(dp, DP_ISR, DP_ISR_RDC);

			/* Succeeded. */
			return 0;
		}
	}

	/* Failed. */
	return ETIMEDOUT;
}

/* Supports the rd operation. */
static uint8_t
rd(
	struct dp8390 *dp,
	unsigned reg)
{
	uint8_t function_result;

	/* Computes the function result. */
	function_result = dp->bus->read_reg(dp->bus_cookie, reg);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the dp start transmit operation. */
static int
dp_start_transmit(
	struct dp8390 *dp,
	struct packet_buf *packet)
{
	uint8_t frame[DP_MIN_FRAME];
	size_t length;
	int error;

	/* Checks the current data length. */
	length = packet->length < DP_MIN_FRAME ? DP_MIN_FRAME : packet->length;
	if (length > DP_MAX_FRAME) {
		packet_buf_free(packet);

		/* Failed. */
		return EMSGSIZE;
	}

	/* Handles the packet condition. */
	if (packet->length < DP_MIN_FRAME) {
		memset(frame, 0, sizeof(frame));
		memcpy(frame, packet->data, packet->length);
		error = dma_write(dp, (uint16_t)dp->tx_start_page << 8, frame,
				  sizeof(frame));
	} else {
		error = dma_write(dp, (uint16_t)dp->tx_start_page << 8,
				  packet->data, length);
	}

	packet_buf_free(packet);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	wr(dp, DP_TPSR, dp->tx_start_page);
	wr(dp, DP_TBCR0, (uint8_t)length);
	wr(dp, DP_TBCR1, (uint8_t)(length >> 8));

	/* TX completion may interrupt immediately after setting TXP. */
	dp->tx_busy = 1;
	wr(dp, DP_CR, DP_CR_START | DP_CR_NODMA | DP_CR_TXP | DP_CR_PAGE0);

	/* Succeeded. */
	return 0;
}

/* Supports the dma write operation. */
static int
dma_write(
	struct dp8390 *dp,
	uint16_t address,
	const uint8_t *buffer,
	size_t length)
{
	int error;
	uint16_t word;
	size_t dma_length = (length + 1U) & ~1U;
	size_t index;

	dma_begin(dp, address, dma_length, DP_CR_RWRITE);
	/* Process each remaining element. */
	for (index = 0; index < dma_length; index += 2U) {
		/* Checks the current index. */
		word = index < length ? buffer[index] : 0;
		if (index + 1U < length)
			word |= (uint16_t)buffer[index + 1U] << 8;
		dp->bus->write_data16(dp->bus_cookie, word);
	}

	/* Obtains the wait rdc result. */
	error = wait_rdc(dp);

	/* Returns the computed result. */
	return error;
}

/* Supports the dma read operation. */
static int
dma_read(
	struct dp8390 *dp,
	uint16_t address,
	void *buffer,
	size_t length)
{
	int error;
	uint16_t word;
	uint8_t *output = buffer;
	size_t index;

	dma_begin(dp, address, length, DP_CR_RREAD);
	/* Process each remaining element. */
	for (index = 0; index + 1U < length; index += 2U) {
		word = dp->bus->read_data16(dp->bus_cookie);
		output[index] = (uint8_t)word;
		output[index + 1U] = (uint8_t)(word >> 8);
	}

	/* Checks the current index. */
	if (index < length)
		output[index] = dp->bus->read_data8(dp->bus_cookie);

	/* Obtains the wait rdc result. */
	error = wait_rdc(dp);

	/* Returns the computed result. */
	return error;
}

/* Supports the chip stop operation. */
static void
chip_stop(
	struct dp8390 *dp)
{
	wr(dp, DP_IMR, 0);
	wr(dp, DP_CR, DP_CR_STOP | DP_CR_NODMA | DP_CR_PAGE0);
	dp->opened = 0;
	dp->tx_busy = 0;
	packet_buf_free(dp->tx_pending);
	dp->tx_pending = NULL;
}

/* Supports the chip start operation. */
static int
chip_start(
	struct dp8390 *dp)
{
	unsigned index;

	/* Checks the reset result. */
	if (dp->bus->reset != NULL && dp->bus->reset(dp->bus_cookie) != 0)
		return EIO;
	wr(dp, DP_CR, DP_CR_STOP | DP_CR_NODMA | DP_CR_PAGE0);
	wr(dp, DP_DCR, dp->dcr);
	wr(dp, DP_RBCR0, 0);
	wr(dp, DP_RBCR1, 0);
	wr(dp, DP_RCR, DP_RCR_MON);
	wr(dp, DP_TCR, DP_TCR_LB0);
	wr(dp, DP_TPSR, dp->tx_start_page);
	wr(dp, DP_PSTART, dp->rx_start_page);
	wr(dp, DP_PSTOP, dp->stop_page);
	wr(dp, DP_BNRY, dp->rx_start_page);
	wr(dp, DP_ISR, 0xffU);
	wr(dp, DP_CR, DP_CR_STOP | DP_CR_NODMA | DP_CR_PAGE1);
	/* Process each remaining element. */
	for (index = 0; index < 6U; index++)
		wr(dp, DP_PAR0 + index, dp->device->hwaddr[index]);
	/* Process each remaining element. */
	for (index = 0; index < 8U; index++)
		wr(dp, DP_MAR0 + index, 0);
	dp->next_packet = (uint8_t)(dp->rx_start_page + 1U);
	wr(dp, DP_CURR, dp->next_packet);
	wr(dp, DP_CR, DP_CR_START | DP_CR_NODMA | DP_CR_PAGE0);
	wr(dp, DP_TCR, 0);
	wr(dp, DP_RCR, DP_RCR_AB);
	wr(dp, DP_ISR, 0xffU);
	dp->opened = 1;
	dp->tx_busy = 0;
	(void)net_device_set_carrier(dp->device, 1);
	wr(dp, DP_IMR, DP_IMR_RUN);

	/* Succeeded. */
	return 0;
}

/* Supports the dp open operation. */
static int
dp_open(
	struct net_device *device)
{
	struct dp8390 *dp = device->driver_data;
	unsigned long irq = spin_lock_irqsave(&dp->lock);
	int error = chip_start(dp);

	spin_unlock_irqrestore(&dp->lock, irq);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Supports the dp close operation. */
static void
dp_close(
	struct net_device *device)
{
	struct dp8390 *dp = device->driver_data;
	unsigned long irq = spin_lock_irqsave(&dp->lock);

	chip_stop(dp);

	spin_unlock_irqrestore(&dp->lock, irq);

	(void)net_device_set_carrier(device, 0);
}

/* Supports the dp transmit operation. */
static int
dp_transmit(
	struct net_device *device,
	struct packet_buf *packet)
{
	struct packet_buf *pending;
	struct dp8390 *dp = device->driver_data;
	unsigned long irq;
	int error = 0;

	/* Handles the packet availability. */
	if (packet == NULL)
		return EINVAL;
	irq = spin_lock_irqsave(&dp->lock);

	/* Handles the dp condition. */
	if (!dp->opened) {
		packet_buf_free(packet);
		error = ENETDOWN;
		goto out;
	}

	/* Checks the rd result. */
	if (dp->tx_busy && (rd(dp, DP_ISR) & (DP_ISR_PTX | DP_ISR_TXE)) != 0) {
		wr(dp, DP_ISR, DP_ISR_PTX | DP_ISR_TXE);
		dp->tx_busy = 0;

		/* Handles the tx pending availability. */
		if (dp->tx_pending != NULL) {
			pending = dp->tx_pending;

			dp->tx_pending = NULL;
			(void)dp_start_transmit(dp, pending);
		}
	}

	/* Handles the dp condition. */
	if (!dp->tx_busy) {
		error = dp_start_transmit(dp, packet);
		goto out;
	}

	/* Handles the tx pending availability. */
	if (dp->tx_pending != NULL) {
		packet_buf_free(packet);
		error = ENOBUFS;
		goto out;
	}

	dp->tx_pending = packet;
out:

	spin_unlock_irqrestore(&dp->lock, irq);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Supports the current page operation. */
static uint8_t
current_page(
	struct dp8390 *dp)
{
	uint8_t current;

	wr(dp, DP_CR, DP_CR_START | DP_CR_NODMA | DP_CR_PAGE1);
	current = rd(dp, DP_CURR);
	wr(dp, DP_CR, DP_CR_START | DP_CR_NODMA | DP_CR_PAGE0);

	/* Returns the computed result. */
	return current;
}

/* Supports the advance ring operation. */
static void
advance_ring(
	struct dp8390 *dp,
	uint8_t next)
{
	uint8_t boundary = next == dp->rx_start_page
				   ? (uint8_t)(dp->stop_page - 1U)
				   : (uint8_t)(next - 1U);

	dp->next_packet = next;
	wr(dp, DP_BNRY, boundary);
}

/* Supports the read frame operation. */
static int
read_frame(
	struct dp8390 *dp,
	uint8_t page,
	struct packet_buf **result,
	uint8_t *next_result)
{
	struct dp_receive_header header;
	struct packet_buf *packet;
	uint16_t count;
	size_t length, first;
	uint32_t address, ring_end;
	void *data;
	int error;

	/* Checks the operation status. */
	error = dma_read(dp, (uint16_t)page << 8, &header, sizeof(header));
	if (error != 0)
		return error;

	/* Handles the header condition. */
	count = (uint16_t)header.count_low | ((uint16_t)header.count_high << 8);
	if (header.next < dp->rx_start_page || header.next >= dp->stop_page ||
	    count < sizeof(header) + DP_MIN_FRAME ||
	    count > sizeof(header) + DP_MAX_FRAME) {
		/* Failed. */
		return EIO;
	}
	length = count - sizeof(header);

	/* Handles the packet availability. */
	packet = packet_buf_alloc(0);
	if (packet == NULL)
		return ENOBUFS;

	/* Handles the data availability. */
	data = packet_buf_append(packet, length);
	if (data == NULL) {
		packet_buf_free(packet);

		/* Failed. */
		return EMSGSIZE;
	}

	address = ((uint32_t)page << 8) + sizeof(header);
	ring_end = (uint32_t)dp->stop_page << 8;
	first = length < ring_end - address ? length : ring_end - address;

	/* Checks the operation status. */
	error = dma_read(dp, (uint16_t)address, data, first);
	if (error == 0 && first < length) {
		error = dma_read(dp, (uint16_t)dp->rx_start_page << 8,
				 (uint8_t *)data + first, length - first);
	}

	/* Checks the operation status. */
	if (error != 0) {
		packet_buf_free(packet);

		/* Failed. */
		return error;
	}

	*next_result = header.next;
	*result = packet;
	/* Succeeded. */
	return 0;
}

/* Supports the dp poll receive operation. */
static unsigned
dp_poll_receive(
	struct net_device *device,
	unsigned budget)
{
	struct packet_buf *packet;
	uint8_t next;
	int error;
	struct dp8390 *dp = device->driver_data;
	unsigned long irq = spin_lock_irqsave(&dp->lock);
	unsigned received = 0;
	int reschedule;
	uint8_t current;

	/* Handles the dp condition. */
	if (!dp->opened) {
		spin_unlock_irqrestore(&dp->lock, irq);

		/* Succeeded. */
		return 0;
	}

	current = current_page(dp);
	/* Process each linked entry. */
	while (received < budget && dp->next_packet != current) {
		packet = NULL;
		next = dp->next_packet;

		/* Checks the operation status. */
		error = read_frame(dp, dp->next_packet, &packet, &next);
		if (error != 0) {
			device->rx_errors++;

			/* Handles the next condition. */
			if (next == dp->next_packet) {
				(void)chip_start(dp);
				break;
			}
		} else {
			received++;
		}

		advance_ring(dp, next);
		current = current_page(dp);

		/*
		 * Protocol input can eventually transmit through this device.
		 */
		spin_unlock_irqrestore(&dp->lock, irq);

		/* Handles the packet availability. */
		if (packet != NULL)
			net_device_receive(device, packet);
		irq = spin_lock_irqsave(&dp->lock);

		/* Handles the dp condition. */
		if (!dp->opened)
			break;
	}

	reschedule = dp->opened && dp->next_packet != current;

	spin_unlock_irqrestore(&dp->lock, irq);

	/* Handles the reschedule condition. */
	if (reschedule)
		net_device_schedule_poll(device);

	/* Returns the computed result. */
	return received;
}
