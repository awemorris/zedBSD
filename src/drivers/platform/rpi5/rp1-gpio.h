/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * RP1's GPIO pins: the function each pin is connected to, and the pins
 * the drivers drive themselves.
 */

#ifndef KERN_DRIVERS_PLATFORM_RPI5_RP1_GPIO_H
#define KERN_DRIVERS_PLATFORM_RPI5_RP1_GPIO_H

/*
 * The function selector a pin is driven by GPIO software.
 *
 * Value referenced: Raspberry Pi Linux rpi-6.12.y
 * drivers/pinctrl/pinctrl-rp1.c (the sixth column of rp1_gpio_pin_funcs,
 * the "gpio" function, is selector 5 on every pin).
 */
#define DRV_RP1_GPIO_FUNCTION_GPIO	5U

/*
 * The pulls a pin's pad can have: none, down or up.
 *
 * Values referenced: Raspberry Pi Linux rpi-6.12.y
 * drivers/pinctrl/pinctrl-rp1.c (RP1_PUD_OFF, RP1_PUD_DOWN, RP1_PUD_UP).
 */
#define DRV_RP1_GPIO_PULL_NONE		0U
#define DRV_RP1_GPIO_PULL_DOWN		1U
#define DRV_RP1_GPIO_PULL_UP		2U

/*
 * Finds RP1's GPIO block and maps it.  drv_rp1_init() must have
 * succeeded.  Calling it again does nothing.
 */
int drv_rp1_gpio_init(void);

/*
 * Connects a pin to one of its peripheral functions (a selector of 0 to 8),
 * with its input and output enabled and the peripheral controlling both.
 */
int drv_rp1_gpio_set_function(unsigned pin, unsigned function);

/* Sets the pull of a pin's pad (one of DRV_RP1_GPIO_PULL_*). */
int drv_rp1_gpio_set_pull(unsigned pin, unsigned pull);

/* Makes a pin a GPIO output at the given level (0 low, otherwise high). */
int drv_rp1_gpio_set_output(unsigned pin, int level);

#endif
