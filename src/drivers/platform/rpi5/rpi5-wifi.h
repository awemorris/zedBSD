/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Raspberry Pi 5's onboard Wi-Fi: the BCM43455 on the BCM2712's second
 * SD controller (sdio2), powered through WL_ON on the gio GPIO block.
 */

#ifndef KERN_DRIVERS_PLATFORM_RPI5_WIFI_H
#define KERN_DRIVERS_PLATFORM_RPI5_WIFI_H

#include <stdint.h>

/*
 * Looks for the chip in the device tree at fdt_phys and starts the thread
 * that brings it up once the root file system and the firmware package
 * are there.  Returns 0, EALREADY on a second call, ENODEV without a
 * chip, or another errno.
 */
int drv_rpi5_wifi_probe(uint64_t fdt_phys);

/* Stops the thread and waits until it has ended. */
void drv_rpi5_wifi_stop(void);

#endif
