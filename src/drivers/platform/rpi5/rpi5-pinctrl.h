/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The BCM2712's pin controller for the pins the onboard Wi-Fi and
 * Bluetooth use (GPIO 24 to 35): each pin's function and its pull, as a
 * device tree pin group asks.
 */

#ifndef KERN_DRIVERS_PLATFORM_RPI5_PINCTRL_H
#define KERN_DRIVERS_PLATFORM_RPI5_PINCTRL_H

#include <stdint.h>

#include <drivers/generic/fdt.h>

/*
 * Finds and maps the pin controller of the tree, the C0 or the D0 one,
 * taking the stepping from the chip when the tree names the other one.
 * Calling it again does nothing.
 */
int drv_rpi5_pinctrl_init(const struct drv_fdt *fdt);

/*
 * Applies a pin group: the function and the pull of each pin it names,
 * in the group itself or in its subnodes.  A pin or a function the module
 * does not know is refused with ENOTSUP.
 */
int drv_rpi5_pinctrl_apply(const struct drv_fdt *fdt, uint32_t group);

/* Applies every group a node's pinctrl-0 names. */
int drv_rpi5_pinctrl_apply_default(const struct drv_fdt *fdt, uint32_t node);

/* Logs the controller's stepping and registers, saying when they were read. */
void drv_rpi5_pinctrl_report(const char *when);

#endif
