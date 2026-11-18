/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Realtek RTL8125 family of PCI Express Ethernet controllers.
 */

#ifndef DRIVERS_ETHERNET_RTL8125_H
#define DRIVERS_ETHERNET_RTL8125_H

/*
 * Registers the driver for the RTL8125, RTL8126 and RTL8127 controllers.
 */
int
drv_pci_rtl8125_driver_register(void);

#endif
