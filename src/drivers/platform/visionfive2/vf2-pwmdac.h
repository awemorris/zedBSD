/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The VisionFive 2's headphone jack: the JH7110's PWM audio converter.
 */

#ifndef KERN_DRIVERS_VF2_PWMDAC_H
#define KERN_DRIVERS_VF2_PWMDAC_H

#include <drivers/generic/fdt.h>

/*
 * Brings up the converter, its pins and its DMA channel and publishes the
 * jack as an audio device.
 *
 * ENODEV means the tree enables no converter.  Board default is the DMA
 * coherency the board assumes.
 */
int drv_vf2_pwmdac_attach(const struct drv_fdt *fdt, int board_default_coherent);

#endif
