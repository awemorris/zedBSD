/*
 * Common dp8390 Ethernet driver
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#include "drivers/dp8390.h"
#include "kern/net/net-device.h"
#include "kern/net/packet-buf.h"

#include <errno.h>
#include <string.h>

/* DP8390 page-zero registers. */
#define DP_CR       0x00U
#define DP_PSTART   0x01U
#define DP_PSTOP    0x02U
#define DP_BNRY     0x03U
#define DP_TPSR     0x04U
#define DP_TBCR0    0x05U
#define DP_TBCR1    0x06U
#define DP_ISR      0x07U
#define DP_RSAR0    0x08U
#define DP_RSAR1    0x09U
#define DP_RBCR0    0x0aU
#define DP_RBCR1    0x0bU
#define DP_RCR      0x0cU
#define DP_TCR      0x0dU
#define DP_DCR      0x0eU
#define DP_IMR      0x0fU

/* Page-one registers. */
#define DP_PAR0     0x01U
#define DP_CURR     0x07U
#define DP_MAR0     0x08U

#define DP_CR_STOP  0x01U
#define DP_CR_START 0x02U
#define DP_CR_TXP   0x04U
#define DP_CR_RREAD 0x08U
#define DP_CR_RWRITE 0x10U
#define DP_CR_NODMA 0x20U
#define DP_CR_PAGE0 0x00U
#define DP_CR_PAGE1 0x40U

#define DP_ISR_PRX  0x01U
#define DP_ISR_PTX  0x02U
#define DP_ISR_RXE  0x04U
#define DP_ISR_TXE  0x08U
#define DP_ISR_OVW  0x10U
#define DP_ISR_CNT  0x20U
#define DP_ISR_RDC  0x40U
#define DP_ISR_RST  0x80U

#define DP_RCR_AB   0x04U
#define DP_RCR_MON  0x20U
#define DP_TCR_LB0  0x02U
#define DP_IMR_RUN  (DP_ISR_PRX | DP_ISR_PTX | DP_ISR_RXE | DP_ISR_TXE | DP_ISR_OVW)
#define DP_DMA_SPINS 100000U
#define DP_MIN_FRAME 60U
#define DP_MAX_FRAME 1518U

struct dp_receive_header {
	uint8_t status;
	uint8_t next;
	uint8_t count_low;
	uint8_t count_high;
};

static uint8_t rd(struct dp8390 *dp, unsigned reg)
{
	return dp->bus->read_reg(dp->bus_cookie, reg);
}

static void wr(struct dp8390 *dp, unsigned reg, uint8_t value)
{
	dp->bus->write_reg(dp->bus_cookie, reg, value);
}

static int
wait_rdc(struct dp8390 *dp)
{
	unsigned spin;

	for (spin = 0; spin < DP_DMA_SPINS; spin++)
		if ((rd(dp, DP_ISR) & DP_ISR_RDC) != 0) {
			wr(dp, DP_ISR, DP_ISR_RDC);
			return 0;
		}
	return ETIMEDOUT;
}

static void
dma_begin(struct dp8390 *dp, uint16_t address, size_t length, uint8_t command)
{
	wr(dp, DP_CR, DP_CR_START | DP_CR_NODMA | DP_CR_PAGE0);
	wr(dp, DP_ISR, DP_ISR_RDC);
	wr(dp, DP_RBCR0, (uint8_t)length);
	wr(dp, DP_RBCR1, (uint8_t)(length >> 8));
	wr(dp, DP_RSAR0, (uint8_t)address);
	wr(dp, DP_RSAR1, (uint8_t)(address >> 8));
	wr(dp, DP_CR, DP_CR_START | command | DP_CR_PAGE0);
}

static int
dma_read(struct dp8390 *dp, uint16_t address, void *buffer, size_t length)
{
	uint8_t *output = buffer;
	size_t index;

	dma_begin(dp, address, length, DP_CR_RREAD);
	for (index = 0; index + 1U < length; index += 2U) {
		uint16_t word = dp->bus->read_data16(dp->bus_cookie);
		output[index] = (uint8_t)word;
		output[index + 1U] = (uint8_t)(word >> 8);
	}
	if (index < length)
		output[index] = dp->bus->read_data8(dp->bus_cookie);
	return wait_rdc(dp);
}

static int
dma_write(struct dp8390 *dp, uint16_t address, const uint8_t *buffer,
	  size_t length)
{
	size_t dma_length = (length + 1U) & ~1U;
	size_t index;

	dma_begin(dp, address, dma_length, DP_CR_RWRITE);
	for (index = 0; index < dma_length; index += 2U) {
		uint16_t word = index < length ? buffer[index] : 0;

		if (index + 1U < length)
			word |= (uint16_t)buffer[index + 1U] << 8;
		dp->bus->write_data16(dp->bus_cookie, word);
	}
	return wait_rdc(dp);
}

int
dp8390_read_prom(struct dp8390 *dp, uint8_t prom[16])
{
	uint8_t raw[32];
	unsigned index;
	int error;

	if (dp == NULL || dp->bus == NULL || prom == NULL)
		return EINVAL;
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
	for (index = 0; index < sizeof(raw); index++)
		raw[index] = dp->bus->read_data8(dp->bus_cookie);
	error = wait_rdc(dp);
	/* Probing must not leave an unconfigured adapter running. */
	wr(dp, DP_IMR, 0);
	wr(dp, DP_CR, DP_CR_STOP | DP_CR_NODMA | DP_CR_PAGE0);
	if (error != 0)
		return error;
	for (index = 0; index < 16U; index++) {
		if (raw[index * 2U] != raw[index * 2U + 1U])
			return ENODEV;
		prom[index] = raw[index * 2U];
	}
	if (prom[14] != 0x57U || prom[15] != 0x57U)
		return ENODEV;
	wr(dp, DP_DCR, dp->dcr);
	return 0;
}

static void
chip_stop(struct dp8390 *dp)
{
	wr(dp, DP_IMR, 0);
	wr(dp, DP_CR, DP_CR_STOP | DP_CR_NODMA | DP_CR_PAGE0);
	dp->opened = 0;
	dp->tx_busy = 0;
	packet_buf_free(dp->tx_pending);
	dp->tx_pending = NULL;
}

static int
chip_start(struct dp8390 *dp)
{
	unsigned index;

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
	for (index = 0; index < 6U; index++)
		wr(dp, DP_PAR0 + index, dp->device->hwaddr[index]);
	for (index = 0; index < 8U; index++)
		wr(dp, DP_MAR0 + index, 0);
	dp->next_packet = (uint8_t)(dp->rx_start_page + 1U);
	wr(dp, DP_CURR, dp->next_packet);
	wr(dp, DP_CR, DP_CR_START | DP_CR_NODMA | DP_CR_PAGE0);
	wr(dp, DP_TCR, 0);
	wr(dp, DP_RCR, DP_RCR_AB);
	wr(dp, DP_ISR, 0xffU);
	wr(dp, DP_IMR, DP_IMR_RUN);
	dp->opened = 1;
	dp->tx_busy = 0;
	dp->device->flags |= NET_DEVICE_RUNNING;
	return 0;
}

static int dp_open(struct net_device *device)
{
	return chip_start(device->driver_data);
}

static void dp_close(struct net_device *device)
{
	chip_stop(device->driver_data);
}

static int
dp_start_transmit(struct dp8390 *dp, struct packet_buf *packet)
{
	size_t length;
	int error;

	length = packet->length < DP_MIN_FRAME ? DP_MIN_FRAME : packet->length;
	if (length > DP_MAX_FRAME) {
		packet_buf_free(packet);
		return EMSGSIZE;
	}
	if (packet->length < DP_MIN_FRAME) {
		uint8_t frame[DP_MIN_FRAME];

		memset(frame, 0, sizeof(frame));
		memcpy(frame, packet->data, packet->length);
		error = dma_write(dp, (uint16_t)dp->tx_start_page << 8,
		    frame, sizeof(frame));
	} else {
		error = dma_write(dp, (uint16_t)dp->tx_start_page << 8,
		    packet->data, length);
	}
	packet_buf_free(packet);
	if (error != 0)
		return error;
	wr(dp, DP_TPSR, dp->tx_start_page);
	wr(dp, DP_TBCR0, (uint8_t)length);
	wr(dp, DP_TBCR1, (uint8_t)(length >> 8));
	/* TX completion may interrupt immediately after setting TXP. */
	dp->tx_busy = 1;
	wr(dp, DP_CR, DP_CR_START | DP_CR_NODMA | DP_CR_TXP | DP_CR_PAGE0);
	return 0;
}

static int
dp_transmit(struct net_device *device, struct packet_buf *packet)
{
	struct dp8390 *dp = device->driver_data;

	if (packet == NULL)
		return EINVAL;
	if (!dp->opened) {
		packet_buf_free(packet);
		return ENETDOWN;
	}
	if (dp->tx_busy && (rd(dp, DP_ISR) & (DP_ISR_PTX | DP_ISR_TXE)) != 0) {
		wr(dp, DP_ISR, DP_ISR_PTX | DP_ISR_TXE);
		dp->tx_busy = 0;
		if (dp->tx_pending != NULL) {
			struct packet_buf *pending = dp->tx_pending;

			dp->tx_pending = NULL;
			(void)dp_start_transmit(dp, pending);
		}
	}
	if (!dp->tx_busy)
		return dp_start_transmit(dp, packet);
	if (dp->tx_pending != NULL) {
		packet_buf_free(packet);
		return ENOBUFS;
	}
	dp->tx_pending = packet;
	return 0;
}

static uint8_t
current_page(struct dp8390 *dp)
{
	uint8_t current;

	wr(dp, DP_CR, DP_CR_START | DP_CR_NODMA | DP_CR_PAGE1);
	current = rd(dp, DP_CURR);
	wr(dp, DP_CR, DP_CR_START | DP_CR_NODMA | DP_CR_PAGE0);
	return current;
}

static void
advance_ring(struct dp8390 *dp, uint8_t next)
{
	uint8_t boundary = next == dp->rx_start_page ?
		(uint8_t)(dp->stop_page - 1U) : (uint8_t)(next - 1U);

	dp->next_packet = next;
	wr(dp, DP_BNRY, boundary);
}

static int
read_frame(struct dp8390 *dp, uint8_t page, struct packet_buf **result,
	   uint8_t *next_result)
{
	struct dp_receive_header header;
	struct packet_buf *packet;
	uint16_t count;
	size_t length, first;
	uint32_t address, ring_end;
	void *data;
	int error;

	error = dma_read(dp, (uint16_t)page << 8, &header, sizeof(header));
	if (error != 0)
		return error;
	count = (uint16_t)header.count_low |
	    ((uint16_t)header.count_high << 8);
	if (header.next < dp->rx_start_page || header.next >= dp->stop_page ||
	    count < sizeof(header) + DP_MIN_FRAME ||
	    count > sizeof(header) + DP_MAX_FRAME)
		return EIO;
	length = count - sizeof(header);
	packet = packet_buf_alloc(0);
	if (packet == NULL)
		return ENOBUFS;
	data = packet_buf_append(packet, length);
	if (data == NULL) {
		packet_buf_free(packet);
		return EMSGSIZE;
	}
	address = ((uint32_t)page << 8) + sizeof(header);
	ring_end = (uint32_t)dp->stop_page << 8;
	first = length < ring_end - address ? length : ring_end - address;
	error = dma_read(dp, (uint16_t)address, data, first);
	if (error == 0 && first < length)
		error = dma_read(dp, (uint16_t)dp->rx_start_page << 8,
		    (uint8_t *)data + first, length - first);
	if (error != 0) {
		packet_buf_free(packet);
		return error;
	}
	*next_result = header.next;
	*result = packet;
	return 0;
}

static unsigned
dp_poll_receive(struct net_device *device, unsigned budget)
{
	struct dp8390 *dp = device->driver_data;
	unsigned received = 0;
	uint8_t current;

	if (!dp->opened)
		return 0;
	current = current_page(dp);
	while (received < budget && dp->next_packet != current) {
		struct packet_buf *packet = NULL;
		uint8_t next = dp->next_packet;
		int error = read_frame(dp, dp->next_packet, &packet, &next);

		if (error != 0) {
			device->rx_errors++;
			if (next == dp->next_packet) {
				(void)chip_start(dp);
				break;
			}
		} else {
			net_device_receive(device, packet);
			received++;
		}
		advance_ring(dp, next);
		current = current_page(dp);
	}
	if (dp->next_packet != current)
		net_device_schedule_poll(device);
	return received;
}

static const struct net_device_ops dp_ops = {
	.open = dp_open,
	.close = dp_close,
	.transmit = dp_transmit,
	.poll_receive = dp_poll_receive,
};

int
dp8390_attach(struct dp8390 *dp, struct net_device *device)
{
	if (dp == NULL || device == NULL || dp->bus == NULL ||
	    dp->bus->read_reg == NULL || dp->bus->write_reg == NULL ||
	    dp->bus->read_data8 == NULL || dp->bus->read_data16 == NULL ||
	    dp->bus->write_data16 == NULL || dp->rx_start_page == 0 ||
	    dp->rx_start_page >= dp->stop_page)
		return EINVAL;
	dp->device = device;
	device->driver_data = dp;
	device->ops = &dp_ops;
	return 0;
}

void
dp8390_interrupt(struct dp8390 *dp)
{
	uint8_t status;

	if (dp == NULL || !dp->opened)
		return;
	status = rd(dp, DP_ISR);
	if (status == 0 || status == 0xffU)
		return;
	wr(dp, DP_ISR, status);
	if ((status & (DP_ISR_PTX | DP_ISR_TXE)) != 0)
		dp->tx_busy = 0;
	if (!dp->tx_busy && dp->tx_pending != NULL) {
		struct packet_buf *packet = dp->tx_pending;

		dp->tx_pending = NULL;
		if (dp_start_transmit(dp, packet) != 0)
			dp->device->tx_errors++;
	}
	if ((status & (DP_ISR_PRX | DP_ISR_RXE | DP_ISR_OVW)) != 0)
		net_device_schedule_poll(dp->device);
	if ((status & DP_ISR_CNT) != 0) {
		(void)rd(dp, 0x0dU);
		(void)rd(dp, 0x0eU);
		(void)rd(dp, 0x0fU);
	}
}
