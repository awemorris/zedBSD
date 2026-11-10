/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The BCM2712 graphics driver's entry point for the Raspberry Pi 5
 * platform.
 *
 * The VideoCore VII of the BCM2712 has a V3D 7.1 render engine and an HVS6
 * display path.  This driver brings up both; the display keeps the mode the
 * firmware set.
 */

#ifndef KERN_DRIVERS_GPU_BCM2712_GPU_H
#define KERN_DRIVERS_GPU_BCM2712_GPU_H

#include <stdint.h>

/*
 * Finds the V3D 7.1 engine and the HVS6 display in the device tree and
 * starts them.
 *
 * fdt_phys is the physical address of the device tree the firmware handed
 * over.  The display and the engine start at boot, unless the boot line
 * holds them with rpi5gpu.start=hold; root then starts them by writing
 * hw.gpu.start=1.  A failed start is logged and never stops the boot.
 * Returns 0 when the engine or the display was found, ENODEV otherwise; the
 * boot goes on either way.
 */
int drv_bcm2712_gpu_attach(uint64_t fdt_phys);

#endif
