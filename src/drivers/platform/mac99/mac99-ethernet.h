/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PowerPC Mac's built-in Ethernet: what the board tells the GMAC's
 * driver.
 */

#ifndef KERN_DRIVERS_PLATFORM_MAC99_ETHERNET_H
#define KERN_DRIVERS_PLATFORM_MAC99_ETHERNET_H

/*
 * Gives the GEM driver the board's station addresses and PHY reset line,
 * and registers the driver.  Called before the PCI buses are enumerated.
 * Returns an errno.
 */
int drv_mac99_ethernet_init(void);

#endif
