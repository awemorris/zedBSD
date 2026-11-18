/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Realtek RTL8211E gigabit Ethernet PHY's RGMII clock delays.
 *
 * On RGMII the receive and transmit clocks have to be delayed against the
 * data by about 2 ns, either by the board's traces, by the MAC or by the
 * PHY.  The RTL8211E takes its delays from two strap pins at reset; a
 * configuration register on extension page 0xa4 of page 7 overrides the
 * straps, so the driver does not depend on how a board strapped them.
 */

#include <drivers/ethernet/rtl8211e.h>
#include <uapi/errno.h>

#include <stddef.h>

/*
 * The page registers, the page and extension page of the delay register,
 * the delay register itself and its bits: the override of the straps and
 * the transmit and receive delays.
 *
 * Values referenced: Linux v6.12 drivers/net/phy/realtek.c
 * (RTL821x_PAGE_SELECT 0x1f, RTL821x_EXT_PAGE_SELECT 0x1e,
 * rtl8211e_config_init(): page 0x7, extension page 0xa4, register 0x1c;
 * RTL8211E_CTRL_DELAY bit 13, RTL8211E_TX_DELAY bit 12,
 * RTL8211E_RX_DELAY bit 11).
 */
#define RTL8211E_PAGE_SELECT		0x1fU
#define RTL8211E_EXT_PAGE_SELECT	0x1eU
#define RTL8211E_PAGE_STANDARD		0x0U
#define RTL8211E_PAGE_EXTENSION		0x7U
#define RTL8211E_EXT_PAGE_DELAY		0xa4U
#define RTL8211E_DELAY_CONFIG		0x1cU
#define RTL8211E_DELAY_FORCE		0x2000U
#define RTL8211E_DELAY_TX		0x1000U
#define RTL8211E_DELAY_RX		0x0800U

static int modify_delays(const struct drv_mii_bus *bus, unsigned phy, uint16_t delays);

/*
 * Sets the PHY's RGMII transmit and receive clock delays, overriding the
 * straps.
 *
 * Returns 0, EINVAL for a missing bus or an address beyond the bus's 32,
 * or the errno of the MDIO transaction that failed.  The PHY is left on
 * its standard page whenever that page can still be selected.
 *
 * Procedure referenced: Linux v6.12 drivers/net/phy/realtek.c
 * (rtl8211e_config_init(): select page 7, select extension page 0xa4,
 * modify register 0x1c, restore the page).
 */
int
drv_rtl8211e_set_rgmii_delays(
	const struct drv_mii_bus *bus,
	unsigned phy,
	int tx_delay,
	int rx_delay)
{
	uint16_t delays;
	int restore_error;
	int error;

	/* Refuses a missing bus or an address beyond the bus's 32. */
	if (bus == NULL || phy > 31U)
		return EINVAL;

	/* The override of the straps, with each delay asked for. */
	delays = RTL8211E_DELAY_FORCE;
	if (tx_delay)
		delays |= RTL8211E_DELAY_TX;
	if (rx_delay)
		delays |= RTL8211E_DELAY_RX;

	/* Writes the delays on their page. */
	error = modify_delays(bus, phy, delays);

	/* Returns to the standard page, which the generic PHY code reads. */
	restore_error = bus->write(bus->context, phy, RTL8211E_PAGE_SELECT, RTL8211E_PAGE_STANDARD);

	/* Reports a failed write of the delays before a failed return to the page. */
	if (error != 0)
		return error;
	if (restore_error != 0)
		return restore_error;

	/* Succeeded: the PHY delays its RGMII clocks as asked. */
	return 0;
}

/*
 * Selects the delay register's page and replaces the delay bits in it;
 * the caller returns to the standard page.
 */
static int
modify_delays(
	const struct drv_mii_bus *bus,
	unsigned phy,
	uint16_t delays)
{
	uint16_t config;
	int error;

	/* Selects the extension pages. */
	error = bus->write(bus->context, phy, RTL8211E_PAGE_SELECT, RTL8211E_PAGE_EXTENSION);
	if (error != 0)
		return error;

	/* Selects the extension page of the delay register. */
	error = bus->write(bus->context, phy, RTL8211E_EXT_PAGE_SELECT, RTL8211E_EXT_PAGE_DELAY);
	if (error != 0)
		return error;

	/* Reads the delay register, whose other bits are Realtek's test settings. */
	error = bus->read(bus->context, phy, RTL8211E_DELAY_CONFIG, &config);
	if (error != 0)
		return error;

	/* Replaces the override and the two delays and keeps the rest. */
	config &= (uint16_t)~(RTL8211E_DELAY_FORCE | RTL8211E_DELAY_TX | RTL8211E_DELAY_RX);
	config |= delays;

	/* Writes the delays. */
	error = bus->write(bus->context, phy, RTL8211E_DELAY_CONFIG, config);
	if (error != 0)
		return error;

	/* Succeeded: the register holds the delays. */
	return 0;
}
