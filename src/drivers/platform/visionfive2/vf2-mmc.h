/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The JH7110's glue for the SD card slot's DesignWare controller.
 *
 * The glue finds the slot's controller in the device tree, switches its
 * pins, clocks and reset on, sets its card clock, reads its card detect
 * pin, and hands the controller to the DesignWare driver.
 */

#ifndef KERN_DRIVERS_VF2_MMC_H
#define KERN_DRIVERS_VF2_MMC_H

#include <drivers/generic/fdt.h>

struct disk;

/*
 * Brings up the SD card slot (SDIO1) and publishes its card as mmcblk0.
 *
 * The system control blocks, the clock and reset generators and the pin
 * controllers must be up.  Reports ENOENT for a tree without the slot,
 * ENODEV for an empty slot, or the errno of the step that failed.
 */
int drv_vf2_mmc_attach(const struct drv_fdt *fdt, struct disk **disk);

#endif
