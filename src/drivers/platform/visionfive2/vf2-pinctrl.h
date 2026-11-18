/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The JH7110's pin controllers (SYS and AON) and their GPIO pins.
 *
 * Each pin's output signal, output enable and pad settings are chosen by
 * fields of shared registers, and each input signal of a device chooses
 * the pin it listens to.  The calls here apply the pin groups a device's
 * node selects with pinctrl-0, and drive and read single pins named by
 * a *-gpios property.  GPIO interrupts are not used.
 */

#ifndef KERN_DRIVERS_VF2_PINCTRL_H
#define KERN_DRIVERS_VF2_PINCTRL_H

#include <stdint.h>

#include <drivers/generic/fdt.h>

/*
 * One of the two pin controllers.
 *
 * The numbers index the controllers' records; COUNT is not a controller.
 */
enum drv_vf2_gpio_bank {
	DRV_VF2_GPIO_SYS = 0,
	DRV_VF2_GPIO_AON = 1,
	DRV_VF2_GPIO_BANK_COUNT = 2
};

/*
 * One GPIO pin as a *-gpios property names it.
 *
 * Active low says the device's active state is a low level on the pin;
 * the read and write calls take and give the active state.
 */
struct drv_vf2_gpio {
	enum drv_vf2_gpio_bank bank;
	uint32_t pin;
	int active_low;
};

/*
 * Finds the two pin controllers in the device tree and maps them.
 *
 * Touches no register.  Reports ENODEV when the SYS controller is missing.
 */
int drv_vf2_pinctrl_init(const struct drv_fdt *fdt);

/*
 * Applies the pin groups a device's pinctrl-0 selects.
 *
 * Each field is read first and written only when it differs from what the
 * group asks for; changed says how many fields were written, so a caller
 * can tell whether the firmware had already set the pins up.  ENOENT means
 * the device selects no groups.
 */
int drv_vf2_pinctrl_apply_default(const struct drv_fdt *fdt, uint32_t node, unsigned *changed);

/*
 * Finds the pin a device's *-gpios property names.
 *
 * Name is the whole property name, such as "cd-gpios".
 */
int drv_vf2_gpio_lookup(const struct drv_fdt *fdt, uint32_t node, const char *name, struct drv_vf2_gpio *gpio);

/*
 * Makes a pin an input: its output driver off and its receiver on.
 */
int drv_vf2_gpio_configure_input(const struct drv_vf2_gpio *gpio);

/*
 * Makes a pin an output driving the given active state.
 *
 * The level is set before the driver is turned on, so the pin never shows
 * the other state.
 */
int drv_vf2_gpio_configure_output(const struct drv_vf2_gpio *gpio, int active);

/*
 * Reads whether a pin is in its active state.
 */
int drv_vf2_gpio_read(const struct drv_vf2_gpio *gpio, int *active);

/*
 * Drives an output pin to its active or inactive state.
 */
int drv_vf2_gpio_write(const struct drv_vf2_gpio *gpio, int active);

#endif
