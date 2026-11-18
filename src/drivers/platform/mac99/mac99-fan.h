/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PowerPC Mac laptop's fan controller (an ADT7460 or ADT7467 on the
 * system controller's I2C bus): its temperatures and fans, and the limits
 * that let its own control cool the machine.
 */

#ifndef KERN_DRIVERS_PLATFORM_MAC99_FAN_H
#define KERN_DRIVERS_PLATFORM_MAC99_FAN_H

#include <stdbool.h>

/*
 * Finds the fan controller the firmware named, logs what it holds, lets
 * its automatic control cool the machine (unless may_write is false: then
 * nothing is written to it), and publishes its temperatures and fans to
 * the thermal core.  Called once, in thread context, at boot.  Returns 0,
 * ENODEV when the board has no fan controller the driver knows, or the
 * errno of the bus or of the chip.
 */
int drv_mac99_fan_init(bool may_write);

#endif
