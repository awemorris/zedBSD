/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Common dp8390 Ethernet driver
 */

#ifndef ZEDBSD_DRIVERS_DP8390_H
#define ZEDBSD_DRIVERS_DP8390_H

#include <stddef.h>
#include <stdint.h>

struct net_device;
struct packet_buf;

struct dp8390_bus_ops {
	uint8_t (
		*read_reg)(
		void *cookie,
		unsigned reg);
	void (
		*write_reg)(
		void *cookie,
		unsigned reg,
		uint8_t value);
	uint8_t (
		*read_data8)(
		void *cookie);
	uint16_t (
		*read_data16)(
		void *cookie);
	void (
		*write_data16)(
		void *cookie,
		uint16_t value);
	int (
		*reset)(
		void *cookie);
};

struct dp8390 {
	const struct dp8390_bus_ops *bus;
	void *bus_cookie;
	struct net_device *device;
	uint8_t tx_start_page;
	uint8_t rx_start_page;
	uint8_t stop_page;
	uint8_t next_packet;
	uint8_t dcr;
	uint8_t tx_busy;
	uint8_t opened;
	struct packet_buf *tx_pending;
};

int
dp8390_read_prom(
	struct dp8390 *dp,
	uint8_t prom[16]);
int
dp8390_attach(
	struct dp8390 *dp,
	struct net_device *device);
void
dp8390_interrupt(
	struct dp8390 *dp);

#endif
