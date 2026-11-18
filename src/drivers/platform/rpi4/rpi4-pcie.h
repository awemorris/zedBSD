/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Raspberry Pi 4 PCIe: the BCM2711 root complex and what sits behind it.
 */

#ifndef KERN_DRIVERS_RPI4_PCIE_H
#define KERN_DRIVERS_RPI4_PCIE_H

#include <stdint.h>

/*
 * Starts the PCI core and, when the device tree at fdt_phys describes an
 * enabled PCIe controller, brings it up and enumerates its bus.  Returns an
 * errno; ENODEV means there is no usable controller, which is not an error
 * for the boot.
 */
int drv_rpi4_pcie_init(uint64_t fdt_phys);

/*
 * Finishes discovery behind PCIe once interrupts are enabled: the USB host
 * controllers look at their root ports.  Does nothing without them.
 */
void drv_rpi4_pcie_refresh(void);

#endif
