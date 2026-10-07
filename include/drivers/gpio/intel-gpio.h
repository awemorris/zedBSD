/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The pads of the Intel PCH's GPIO controller (INTC1055 and its kin) that
 * ACPI GpioInt resources name (ws159-p006): a pad found from its
 * controller's path and its ACPI pin, the level of its input, and its
 * interrupt through the controller's own interrupt line.
 */

#ifndef DRIVERS_GPIO_INTEL_GPIO_H
#define DRIVERS_GPIO_INTEL_GPIO_H

#include <stdint.h>

struct drv_intel_gpio_pad;

int drv_intel_gpio_pad_find(const char *controller, uint32_t pin, struct drv_intel_gpio_pad **result);
int drv_intel_gpio_pad_level(const struct drv_intel_gpio_pad *pad);

/*
 * A pad's interrupt (ws159-p006).  The handler runs in interrupt context,
 * must not sleep, and is called once a firing: the pad's interrupt is then
 * off until its user, done with the device, turns it on again with
 * drv_intel_gpio_pad_irq_arm.  drv_intel_gpio_pad_irq_enable returns 0, or
 * an errno when the pad cannot interrupt (another controller family, a pad
 * the firmware keeps, a controller interrupt the HAL cannot configure);
 * the user then watches the pad's level instead.  drv_intel_gpio_pad_irq_alive
 * says 0 once the controller's line was given up (it kept firing for no
 * pad, BUG-261).
 */
typedef void (*drv_intel_gpio_handler_t)(void *argument);

int drv_intel_gpio_pad_irq_enable(struct drv_intel_gpio_pad *pad, drv_intel_gpio_handler_t handler, void *argument);
void drv_intel_gpio_pad_irq_arm(struct drv_intel_gpio_pad *pad);
int drv_intel_gpio_pad_irq_alive(const struct drv_intel_gpio_pad *pad);

#endif
