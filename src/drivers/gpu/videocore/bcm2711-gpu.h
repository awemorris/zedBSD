/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The BCM2711 graphics driver's entry point for the Raspberry Pi 4 platform.
 *
 * The VideoCore VI of the BCM2711 has two independent parts: the display
 * path (the compositor that reads display lists, the timing generators and
 * the HDMI encoders) and the V3D 4.2 render engine.  The platform calls the
 * driver once, while it discovers the board's devices.
 */

#ifndef KERN_DRIVERS_GPU_BCM2711_GPU_H
#define KERN_DRIVERS_GPU_BCM2711_GPU_H

#include <stdbool.h>
#include <stdint.h>

/*
 * The screen the firmware lit before the kernel started.
 *
 * The platform fills it from its boot handoff; physical is an ARM physical
 * address (the firmware's bus address without the VideoCore alias bits).  A
 * zero size means the firmware lit no screen.
 */
struct drv_bcm2711_boot_screen {
	uint64_t physical;
	uint64_t size;
	uint32_t width;
	uint32_t height;
	uint32_t pitch;
	uint32_t format;
	/* Normal non-cacheable pixels are already in RAM, so a flip has no CPU cache to clean for them. */
	bool uncached;
};

/*
 * Finds the display path and the V3D engine in the device tree and prepares
 * them.
 *
 * fdt_phys is the physical address of the device tree the firmware handed
 * over, and screen describes the firmware's screen.  A board or emulator
 * without the hardware, and a boot that turned the driver off, leave the
 * machine without it; the boot goes on either way.
 * Returns 0 when at least one part was prepared, ENODEV when none was.
 */
int drv_bcm2711_gpu_attach(uint64_t fdt_phys, const struct drv_bcm2711_boot_screen *screen);

#endif
