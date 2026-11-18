/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The JH7110's glue for its PowerVR GPU core: the power domain, clocks
 * and resets around the core, and where the core is.
 */

#ifndef KERN_DRIVERS_VF2_GPU_H
#define KERN_DRIVERS_VF2_GPU_H

#include <stdint.h>

#include <drivers/generic/fdt.h>

/*
 * Where the board's GPU core is when the device tree does not say.
 *
 * The board's tree usually has no node for the core, so the board keeps
 * this description of its own.  A node the tree enables wins over it.
 */
struct drv_vf2_gpu_place {
	uint64_t registers;
	uint64_t register_size;
	uint32_t interrupt;
};

/*
 * Powers the GPU core and hands it to the PowerVR driver.
 *
 * The core is taken not to snoop the caches and to reach memory below
 * 4 GiB, whichever description is used.
 */
int drv_vf2_gpu_attach(const struct drv_fdt *fdt, const struct drv_vf2_gpu_place *builtin);

#endif
