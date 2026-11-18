/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The JH7110's glue for the VisionFive 2's two Ethernet ports.
 *
 * The glue finds a port's DesignWare controller in the device tree,
 * switches its pins, clocks and resets on, selects RGMII in the system
 * control block the node names, reads the PHY's settings, and hands the
 * controller to the DesignWare driver.
 */

#ifndef KERN_DRIVERS_VF2_ETHERNET_H
#define KERN_DRIVERS_VF2_ETHERNET_H

#include <drivers/generic/fdt.h>

/* How many Ethernet ports the board has: GMAC0 as en0 and GMAC1 as en1. */
#define DRV_VF2_ETHERNET_PORTS	2U

/*
 * Brings up one Ethernet port (0 for GMAC0, 1 for GMAC1) and publishes
 * it as en0 or en1.
 *
 * The system control blocks, the clock and reset generators and the pin
 * controllers must be up.  dma_default_coherent is the board's answer when
 * the device tree does not say whether the controller's DMA is coherent.
 * Reports EINVAL for a port beyond the board's, ENOENT for a tree without
 * the port or one that disables it, or the errno of the step that failed.
 */
int drv_vf2_ethernet_attach(const struct drv_fdt *fdt, unsigned port, int dma_default_coherent);

#endif
