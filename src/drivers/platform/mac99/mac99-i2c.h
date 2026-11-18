/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PowerPC Mac's I2C controller (Apple's keywest, in the system
 * controller and in the I/O controller), driven by polling.
 */

#ifndef KERN_DRIVERS_PLATFORM_MAC99_I2C_H
#define KERN_DRIVERS_PLATFORM_MAC99_I2C_H

#include <stdint.h>

struct drv_i2c_bus;

/*
 * Maps one keywest controller and registers one of its channels as a bus
 * of the kernel's I2C registry.
 *
 * registers is the physical address of its first register and step the
 * bytes between two registers (a power of two); channel is the bus the
 * controller drives the transfers on (0 to 15) and rate_khz the bus's rate
 * the firmware named (100, 50 or 25; any other value takes 25).  A bus is
 * a combined write and read: at most one byte written before a read (the
 * device's register), any number written alone, any number read alone.
 * Returns 0 and the bus, EINVAL for a step or channel the controller
 * cannot have, ENOSPC when a controller is already attached, or the
 * mapping's or the registry's errno.
 */
int drv_mac99_i2c_attach(uint64_t registers, uint32_t step, uint32_t channel, uint32_t rate_khz, struct drv_i2c_bus **bus);

#endif
