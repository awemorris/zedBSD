/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Raspberry Pi 4 GPIO block: pin functions and pull resistors.
 *
 * The BCM2711 keeps the function of ten pins in each GPFSELn register and
 * the resistor of sixteen pins in each pull register, so drivers of
 * different devices share registers.  These helpers are the only writers
 * the wireless and Bluetooth drivers use, and each change is a
 * read-modify-write under one lock.
 */

#ifndef KERN_DRIVERS_RPI4_GPIO_H
#define KERN_DRIVERS_RPI4_GPIO_H

#include <stdint.h>

#include <drivers/generic/fdt.h>

/* The pins the block has: GPIO 0 to 57. */
#define DRV_RPI4_GPIO_PIN_COUNT		58U

/*
 * A pin's function: the 3-bit function select code of BCM2711 GPFSELn.
 *
 * The device tree's brcm,function uses the same numbers.
 */
enum drv_rpi4_gpio_function {
	DRV_RPI4_GPIO_FUNCTION_INPUT = 0,
	DRV_RPI4_GPIO_FUNCTION_OUTPUT = 1,
	DRV_RPI4_GPIO_FUNCTION_ALT5 = 2,
	DRV_RPI4_GPIO_FUNCTION_ALT4 = 3,
	DRV_RPI4_GPIO_FUNCTION_ALT0 = 4,
	DRV_RPI4_GPIO_FUNCTION_ALT1 = 5,
	DRV_RPI4_GPIO_FUNCTION_ALT2 = 6,
	DRV_RPI4_GPIO_FUNCTION_ALT3 = 7
};

/*
 * A pin's resistor.
 *
 * The numbers are deliberately neither the device tree's brcm,pull nor the
 * register's code, which disagree on up and down, so passing either raw
 * number by mistake is refused.  Zero is not a value.
 */
enum drv_rpi4_gpio_pull {
	DRV_RPI4_GPIO_PULL_NONE = 1,
	DRV_RPI4_GPIO_PULL_UP = 2,
	DRV_RPI4_GPIO_PULL_DOWN = 3
};

/*
 * Finds the GPIO block in the device tree and maps its registers.
 *
 * Touches no register.  Returns an errno; a call after a successful one
 * does nothing, and callers on different CPUs may race.  The other
 * functions report ENODEV until it has succeeded.
 */
int drv_rpi4_gpio_init(const struct drv_fdt *fdt);

/*
 * Selects a pin's function.
 *
 * Returns EINVAL for a pin above 57 or an unknown function.
 */
int drv_rpi4_gpio_set_function(unsigned pin, enum drv_rpi4_gpio_function function);

/*
 * Reads a pin's current function.
 */
int drv_rpi4_gpio_get_function(unsigned pin, enum drv_rpi4_gpio_function *function);

/*
 * Sets a pin's pull resistor.
 *
 * Returns EINVAL for a pin above 57 or a value outside the enumeration.
 */
int drv_rpi4_gpio_set_pull(unsigned pin, enum drv_rpi4_gpio_pull pull);

/*
 * Converts the device tree's brcm,pull value to a pull resistor.
 *
 * The tree says 0 for none, 1 for down and 2 for up; anything else is
 * EINVAL.  Needs no initialization.
 */
int drv_rpi4_gpio_pull_from_dt(uint32_t brcm_pull, enum drv_rpi4_gpio_pull *pull);

#endif
