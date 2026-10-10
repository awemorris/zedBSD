/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * BCM54213PE external PHY control over a caller-owned clause-22 MDIO bus.
 */

#ifndef DRIVERS_ETHERNET_BCM54213PE_H
#define DRIVERS_ETHERNET_BCM54213PE_H

#include <stdint.h>

/* One PHY's serialized MDIO callbacks and board-requested RGMII clock delays. */
struct drv_bcm54213pe {
	void *context;
	int (*read)(void *context, unsigned reg, uint16_t *data);
	int (*write)(void *context, unsigned reg, uint16_t data);
	unsigned rx_delay;
	unsigned tx_delay;
};

int drv_bcm54213pe_initialize(const struct drv_bcm54213pe *phy,
			      uint32_t *identity);
int drv_bcm54213pe_read_link(const struct drv_bcm54213pe *phy,
			     unsigned *speed_mbps);

#endif
