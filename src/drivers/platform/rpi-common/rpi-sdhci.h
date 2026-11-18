/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Raspberry Pi 4's and Pi 5's SD controller, handed to the SDHCI driver.
 */

#ifndef KERN_DRIVERS_RPI_SDHCI_H
#define KERN_DRIVERS_RPI_SDHCI_H

#include <stdint.h>

struct disk;

int drv_rpi_sdhci_init(uintptr_t physical_base, uint64_t base_clock_hz);
struct disk *drv_rpi_sdhci_disk(void);

#endif
