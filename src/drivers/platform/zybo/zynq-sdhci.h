/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Zynq-7000's SD controller SDIO0, handed to the SDHCI driver.
 */

#ifndef KERN_DRIVERS_PLATFORM_ZYBO_ZYNQ_SDHCI_H
#define KERN_DRIVERS_PLATFORM_ZYBO_ZYNQ_SDHCI_H

struct disk;

int drv_zynq_sdhci_init(struct disk **disk);

#endif
