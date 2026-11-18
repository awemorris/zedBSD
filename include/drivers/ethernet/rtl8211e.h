/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Realtek RTL8211E gigabit Ethernet PHY's register of its own: the
 * RGMII clock delays.
 *
 * Everything else the PHY does is in its standard management registers
 * (drivers/ethernet/mii-phy.h).  The caller owns the MDIO bus and
 * serializes every transaction on it.
 */

#ifndef DRIVERS_ETHERNET_RTL8211E_H
#define DRIVERS_ETHERNET_RTL8211E_H

#include <drivers/ethernet/mii-phy.h>

#include <stdint.h>

/*
 * The PHY's identity as its identity registers read: the OUI, the model
 * and the revision.  DRV_RTL8211E_IDENTITY_MASK leaves the revision out.
 *
 * Value referenced: Linux v6.12 drivers/net/phy/realtek.c (the RTL8211E
 * entry, PHY_ID_MATCH_EXACT(0x001cc915)).
 */
#define DRV_RTL8211E_IDENTITY		0x001cc915U
#define DRV_RTL8211E_IDENTITY_MASK	0xfffffff0U

int drv_rtl8211e_set_rgmii_delays(const struct drv_mii_bus *bus, unsigned phy, int tx_delay, int rx_delay);

#endif
