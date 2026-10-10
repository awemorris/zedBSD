/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Board discovery and initialization for Raspberry Pi 4 onboard Ethernet.
 */

#ifndef DRIVERS_PLATFORM_RPI4_ETHERNET_H
#define DRIVERS_PLATFORM_RPI4_ETHERNET_H

#include <drivers/generic/fdt.h>

/* One firmware-described GENET v5 and its external RGMII PHY. */
struct rpi4_ethernet_config {
	uint64_t physical;
	uint64_t size;
	uint64_t dma_limit;
	unsigned irq;
	unsigned phy_address;
	unsigned rx_delay;
	unsigned tx_delay;
	uint8_t mac[6];
};

int drv_rpi4_ethernet_describe(const struct drv_fdt *fdt, uint32_t node,
			       struct rpi4_ethernet_config *config);
int drv_rpi4_ethernet_init(uint64_t fdt_physical);
void drv_rpi4_ethernet_refresh(void);

#endif
