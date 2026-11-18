/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The VisionFive 2's two PCIe root complexes and the drivers behind them.
 *
 * The board's part of each root complex -- the STG system control block's
 * settings, the clocks, resets and pins, the PHY and PERST# -- is done
 * here; the root complex itself is the PLDA driver's.  One root complex
 * leads to the USB controller, the other to the M.2 slot.
 */

#ifndef KERN_DRIVERS_VF2_PCIE_H
#define KERN_DRIVERS_VF2_PCIE_H

#include <drivers/generic/fdt.h>

/*
 * Registers the drivers and starts every enabled root complex.
 *
 * The clock, reset, pin and system control drivers must be up.
 * dma_default_coherent is the board's answer for a tree that does not say
 * whether DMA snoops the caches.  Reports ENODEV when the tree has no
 * root complex; a root complex that fails is logged and skipped.
 */
int drv_vf2_pcie_init(const struct drv_fdt *fdt, int dma_default_coherent);

/*
 * Looks behind the USB root ports and publishes the NVMe namespaces, once
 * interrupts are enabled.
 */
void drv_vf2_pcie_refresh(void);

/* Lets the USB keyboards and mice type once the input core is ready. */
void drv_vf2_pcie_input_ready(void);

#endif
