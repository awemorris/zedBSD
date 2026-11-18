/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Raspberry Pi 5's processor temperature and its fan connector.
 */

#ifndef KERN_DRIVERS_PLATFORM_RPI5_RPI5_FAN_H
#define KERN_DRIVERS_PLATFORM_RPI5_RPI5_FAN_H

/*
 * Starts reading the BCM2712's temperature (a sensor of the thermal core)
 * and driving the fan connector from it, and hands the fan to the thermal
 * core (a fan with a boost, and a receiver of the strategy).
 * drv_rp1_init() must have succeeded, and the scheduler must run threads.
 * Returns 0, or an errno when neither the temperature nor the fan could be
 * started.
 */
int drv_rpi5_fan_start(void);

#endif
