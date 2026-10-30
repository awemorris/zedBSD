/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The activity LED, which counts the stages of the early boot in blinks.
 *
 * Each board drives its own pin (activity-led.c); the blinking and the
 * waits are common (led.c).
 */

#ifndef KERN_HAL_ARM64_BSP_RPI_COMMON_LED_H
#define KERN_HAL_ARM64_BSP_RPI_COMMON_LED_H

void rpi_led_init(void);
void rpi_led_set(int on);
void rpi_led_stage(unsigned stage);
void rpi_delay_ms(unsigned milliseconds);

#endif
