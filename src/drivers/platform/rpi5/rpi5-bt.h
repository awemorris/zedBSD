/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Raspberry Pi 5's onboard Bluetooth: the BCM43455's HCI on the
 * BCM2712's uarta, powered through BT_ON on the gio GPIO block.
 */

#ifndef KERN_DRIVERS_PLATFORM_RPI5_BT_H
#define KERN_DRIVERS_PLATFORM_RPI5_BT_H

#include <stdint.h>

/*
 * Looks for the controller in the device tree at fdt_phys and starts the
 * thread that brings it up and publishes it through the HCI class.
 * Returns 0, EALREADY on a second call, ENODEV without a controller, or
 * another errno.
 */
int drv_rpi5_bt_probe(uint64_t fdt_phys);

#endif
