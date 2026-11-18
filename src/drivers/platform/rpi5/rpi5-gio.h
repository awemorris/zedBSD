/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The BCM2712's GPIO blocks of the brcmstb kind (gio and gio_aon): the
 * output pins the Wi-Fi and Bluetooth power lines are.
 */

#ifndef KERN_DRIVERS_PLATFORM_RPI5_GIO_H
#define KERN_DRIVERS_PLATFORM_RPI5_GIO_H

#include <stdint.h>

#include <drivers/generic/fdt.h>

/* The most pins of one block (two banks of 32). */
#define DRV_RPI5_GIO_PIN_MAX	64U

/*
 * One mapped GPIO block.
 *
 * Filled by drv_rpi5_gio_open() and constant afterwards; the block's
 * registers are changed under the lock, taken with interrupts off.
 */
struct drv_rpi5_gio {
	volatile uint8_t *registers;
	uint64_t size;
	unsigned pin_count;
};

/*
 * Maps the GPIO block a device tree node is, which must be a
 * brcm,brcmstb-gpio.
 */
int drv_rpi5_gio_open(const struct drv_fdt *fdt, uint32_t node, struct drv_rpi5_gio *gio);

/* Makes a pin an output at the given level (0 low, otherwise high). */
int drv_rpi5_gio_set_output(struct drv_rpi5_gio *gio, unsigned pin, int level);

/* Reads a pin's level and whether it is an output. */
int drv_rpi5_gio_get(struct drv_rpi5_gio *gio, unsigned pin, int *level, int *output);

/*
 * Reads the data and direction words of the bank that holds a pin: every
 * pin's level, and a set bit for each input.
 */
int drv_rpi5_gio_get_bank(struct drv_rpi5_gio *gio, unsigned pin, uint32_t *data, uint32_t *direction);

#endif
