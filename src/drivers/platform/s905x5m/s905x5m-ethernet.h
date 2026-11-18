/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The S905X5M's Ethernet: its DesignWare EQoS MAC with the Amlogic glue,
 * and the ODROID-C5's gigabit PHY on RGMII.
 */

#ifndef KERN_DRIVERS_PLATFORM_S905X5M_ETHERNET_H
#define KERN_DRIVERS_PLATFORM_S905X5M_ETHERNET_H

#include <stdint.h>

/*
 * Brings up the MAC the device tree at fdt_phys names, its glue and the
 * PHY, and publishes the interface.  Returns 0 on success or a negative
 * errno; the boot goes on without the port on failure.
 */
int drv_s905x5m_ethernet_init(uint64_t fdt_phys);

/* Starts the link's service once interrupts are on. */
void drv_s905x5m_ethernet_refresh(void);

#endif
