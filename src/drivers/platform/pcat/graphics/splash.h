/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The boot splash's spinner on the firmware framebuffer.
 */

#ifndef KERN_DRIVERS_PLATFORM_PCAT_GRAPHICS_SPLASH_H
#define KERN_DRIVERS_PLATFORM_PCAT_GRAPHICS_SPLASH_H

#include <stdint.h>

void drv_pcat_splash_start(volatile uint32_t *pixels, unsigned width, unsigned height, unsigned stride, int rgbx);
void drv_pcat_splash_retarget(volatile uint32_t *pixels);
void drv_pcat_splash_step(void);
void drv_pcat_splash_stop(void);

#endif
