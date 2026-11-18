/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The standard management registers of an Ethernet PHY, over a clause-22
 * MDIO bus that the caller owns.
 *
 * The functions read the PHY's identity, reset it, advertise what it can
 * do and start autonegotiation, and read back the speed and duplex the
 * link settled on.  A PHY with registers of its own beyond the standard
 * ones has its own driver, which uses these for the common part.
 */

#ifndef DRIVERS_ETHERNET_MII_PHY_H
#define DRIVERS_ETHERNET_MII_PHY_H

#include <stdint.h>

/*
 * One MDIO bus.
 *
 * The caller serializes every transaction on the bus; the callbacks
 * report an errno, and a read stores the register's sixteen bits.
 */
struct drv_mii_bus {
	void *context;
	int (*read)(void *context, unsigned phy, unsigned reg, uint16_t *data);
	int (*write)(void *context, unsigned phy, unsigned reg, uint16_t data);
};

/*
 * What a link settled on.
 *
 * The speed is zero while the link is down or has not finished
 * autonegotiation; the duplex then means nothing.
 */
struct drv_mii_link {
	unsigned speed_mbps;
	int full_duplex;
};

int drv_mii_phy_read_identity(const struct drv_mii_bus *bus, unsigned phy, uint32_t *identity);
int drv_mii_phy_reset(const struct drv_mii_bus *bus, unsigned phy);
int drv_mii_phy_start_autonegotiation(const struct drv_mii_bus *bus, unsigned phy, int gigabit);
int drv_mii_phy_read_link(const struct drv_mii_bus *bus, unsigned phy, struct drv_mii_link *link);

#endif
