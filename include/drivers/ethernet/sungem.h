/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Sun GEM family of PCI Ethernet controllers, Apple's GMAC among them.
 */

#ifndef DRIVERS_ETHERNET_SUNGEM_H
#define DRIVERS_ETHERNET_SUNGEM_H

#include <stdint.h>
#include <drivers/pci/pci.h>

/*
 * What a board knows of its controllers that the controllers cannot
 * tell the driver: the station address its firmware gives one, and how
 * to pulse the PHY's reset line, which on an Apple board is a GPIO of
 * the I/O controller.  Either hook may be NULL.
 *
 * station_address copies the six octets the firmware gives the
 * controller at the PCI address into mac, and returns ENOENT when the
 * firmware gives it none.  reset_phy pulses the line and returns when
 * the PHY may be talked to; ENODEV means the board has no such line.
 * Both are called while the controller attaches, before the scheduler
 * runs, and must not sleep.
 */
struct drv_sungem_board {
	int (*station_address)(const struct drv_pci_address *address, uint8_t *mac);
	int (*reset_phy)(void);
};

/*
 * Gives the driver the board's hooks; the description must outlive the
 * kernel.  Called before the PCI bus is enumerated.
 */
void drv_sungem_set_board(const struct drv_sungem_board *board);

/*
 * Registers the driver for Apple's GMAC and Sun's GEM controllers.
 */
int drv_pci_sungem_driver_register(void);

#endif
