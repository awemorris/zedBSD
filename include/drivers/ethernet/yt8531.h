/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Motorcomm YT8531 gigabit PHY's own settings, on top of its standard
 * registers.
 *
 * The board describes the RGMII clock delays the PHY adds, the drive
 * strength of its receive pins, and whether the transmit clock is
 * inverted at each speed; the driver writes them to the PHY's extended
 * registers.
 */

#ifndef DRIVERS_ETHERNET_YT8531_H
#define DRIVERS_ETHERNET_YT8531_H

#include <drivers/ethernet/mii-phy.h>

#include <stdint.h>

/*
 * The YT8531's identity in registers 2 and 3.
 *
 * Value referenced: Linux v6.12 drivers/net/phy/motorcomm.c
 * (PHY_ID_YT8531).
 */
#define DRV_YT8531_IDENTITY	0x4f51e91bU

/*
 * What the board says about one YT8531.
 *
 * The delays apply only in the directions the interface mode asks the PHY
 * to delay; a drive strength of zero leaves the PHY's default.  The
 * inversions are used only when tx_clock_adjust is set.
 */
struct drv_yt8531_config {
	int rx_delay;
	int tx_delay;
	uint32_t rx_delay_ps;
	uint32_t tx_delay_ps;
	uint32_t rx_clock_drive_ua;
	uint32_t rx_data_drive_ua;
	int tx_clock_adjust;
	int tx_clock_inverted_10;
	int tx_clock_inverted_100;
	int tx_clock_inverted_1000;
};

int drv_yt8531_configure(const struct drv_mii_bus *bus, unsigned phy, const struct drv_yt8531_config *config);
int drv_yt8531_set_speed(const struct drv_mii_bus *bus, unsigned phy, const struct drv_yt8531_config *config, unsigned speed_mbps);

#endif
