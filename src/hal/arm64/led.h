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

#ifndef KERN_HAL_ARM64_LED_H
#define KERN_HAL_ARM64_LED_H

void arm64_led_init(void);
void arm64_led_set(int on);
void arm64_led_stage(unsigned stage);
void arm64_delay_ms(unsigned milliseconds);

#endif
