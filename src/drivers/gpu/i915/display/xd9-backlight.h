/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The panel's backlight of a display 9 node (xd9-backlight.c).
 *
 * The south display's PWM drives the eDP panel's backlight.  Its period
 * and its duty are offered as the kernel's backlight device
 * (/dev/backlight, kern/backlight.h), the same one the Settings app's
 * brightness uses on a display 12 or 13 node.  The firmware's period is
 * kept; only the duty changes.
 */

#ifndef DRIVERS_GPU_I915_DISPLAY_XD9_BACKLIGHT_H
#define DRIVERS_GPU_I915_DISPLAY_XD9_BACKLIGHT_H

struct i915_device;

void drv_i915_xd9_backlight_start(struct i915_device *device);
void drv_i915_xd9_backlight_stop(struct i915_device *device);
void drv_i915_xd9_backlight_suspend(struct i915_device *device);
void drv_i915_xd9_backlight_resume(struct i915_device *device);

#endif /* DRIVERS_GPU_I915_DISPLAY_XD9_BACKLIGHT_H */
