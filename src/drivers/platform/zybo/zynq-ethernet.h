/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Zynq-7000's first Ethernet controller, GEM0, handed to the Cadence
 * GEM driver, with the board's PHY on its MDIO bus.
 */

#ifndef KERN_DRIVERS_PLATFORM_ZYBO_ZYNQ_ETHERNET_H
#define KERN_DRIVERS_PLATFORM_ZYBO_ZYNQ_ETHERNET_H

int drv_zynq_ethernet_init(void);
void drv_zynq_ethernet_refresh(void);

#endif
