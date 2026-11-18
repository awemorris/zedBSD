/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The JH7110's AXI DMA controller as the board wires it: its clocks and
 * resets, before the generic driver takes it over.
 */

#ifndef KERN_DRIVERS_VF2_DMAC_H
#define KERN_DRIVERS_VF2_DMAC_H

#include <stdint.h>

#include <drivers/dma/dw-axi-dmac.h>
#include <drivers/generic/fdt.h>

/*
 * Brings up the DMA controller a node names and hands it out.
 *
 * The first call starts the controller's clocks, releases its resets and
 * attaches the generic driver; later calls for the same node hand out the
 * same controller.  Board default is the DMA coherency the board assumes.
 */
int drv_vf2_dmac_get(const struct drv_fdt *fdt, uint32_t node, int board_default_coherent, struct drv_dw_axi_dmac **controller);

#endif
