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
 * display path.  This driver brings up the render engine; the display stays
 * the firmware's.
 */

#ifndef KERN_DRIVERS_GPU_BCM2712_GPU_H
#define KERN_DRIVERS_GPU_BCM2712_GPU_H

#include <stdint.h>

/*
 * Finds the V3D 7.1 engine in the device tree and holds its bring-up for
 * root.
 *
 * fdt_phys is the physical address of the device tree the firmware handed
 * over.  At boot the driver only reads the tree, maps the windows and asks
 * the firmware about the engine's clock; powering the engine (and its
 * self-test, which runs only under rpi4gpu.selftest=1) waits until root
 * writes hw.gpu.start=1, so a failure there never stops the boot.  Returns 0 when the engine was found, ENODEV otherwise;
 * the boot goes on either way.
 */
int drv_bcm2712_gpu_attach(uint64_t fdt_phys);

#endif
