/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The board's entry to the Mali Valhall GPU driver and its display.
 */

#ifndef KERN_DRIVERS_GPU_MALI_ATTACH_H
#define KERN_DRIVERS_GPU_MALI_ATTACH_H

#include <stdint.h>

/*
 * Finds the Mali GPU and the display in the device tree at fdt_phys,
 * registers the display node, powers the GPU and starts the thread that
 * loads its firmware once the root file system is mounted.
 *
 * Returns 0 when the GPU came up as far as the thread's start, or a
 * negative errno; every failure is written to the log in one line and
 * leaves the boot going.  The display node does not depend on the GPU
 * and stays when the GPU fails.  Called once, from the platform's thread
 * context with interrupts on.
 */
int drv_mali_gpu_attach(uint64_t fdt_phys);

#endif
