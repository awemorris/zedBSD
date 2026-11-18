/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Raspberry Pi 5's Ethernet port: RP1's Cadence GEM with the board's
 * BCM54213PE PHY, published as en0.
 */

#ifndef KERN_DRIVERS_PLATFORM_RPI5_RP1_ETHERNET_H
#define KERN_DRIVERS_PLATFORM_RPI5_RP1_ETHERNET_H

/*
 * Starts the controller and the PHY and publishes the interface.
 * drv_rp1_init() must have succeeded.  Returns 0, or ENODEV when RP1 has
 * no usable Ethernet controller, or another errno.
 */
int drv_rp1_ethernet_init(void);

/*
 * Starts the periodic service of the link and of missed interrupts, once
 * interrupts are enabled.
 */
void drv_rp1_ethernet_refresh(void);

#endif
