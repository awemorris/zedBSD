/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The S905X5M's SD card: the SD/eMMC controller of the microSD slot
 * (sd_emmc_b), and the card in it as a disk.
 */

#ifndef KERN_DRIVERS_PLATFORM_S905X5M_MMC_H
#define KERN_DRIVERS_PLATFORM_S905X5M_MMC_H

#include <stdint.h>

struct disk;

/*
 * Starts the microSD slot's controller the device tree at fdt_phys names,
 * and the card in it.  Returns 0 when the card answers and its disk is
 * published, or a negative errno.
 */
int drv_s905x5m_mmc_init(uint64_t fdt_phys);

/* Returns the SD card's disk, or NULL before it was published. */
struct disk *drv_s905x5m_mmc_disk(void);

#endif
