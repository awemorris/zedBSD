/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Raspberry Pi 5's PCIe root complexes other than RP1's: the external
 * x1 port of the M.2 HATs, and the NVMe drive behind it.
 *
 * RP1's root complex is left as the firmware set it up.  Any other
 * BCM2712 root complex the device tree enables (config.txt's
 * dtparam=pciex1 enables the external port) is reset and started, its
 * bridge reset through the SoC's reset block and its message interrupts
 * through its MIP.
 */

#ifndef KERN_DRIVERS_PLATFORM_RPI5_RPI5_PCIE_H
#define KERN_DRIVERS_PLATFORM_RPI5_RPI5_PCIE_H

#include <stdint.h>

/*
 * Starts every enabled root complex but RP1's, from the device tree at
 * fdt_phys, and registers the drivers behind them.  Returns 0 when one
 * came up, ENODEV when none did, or another errno; the boot goes on
 * either way.
 */
int drv_rpi5_pcie_init(uint64_t fdt_phys);

/*
 * Publishes the NVMe namespaces once interrupts are on.
 */
void drv_rpi5_pcie_refresh(void);

#endif
