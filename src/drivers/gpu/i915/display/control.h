/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The display control of the resident output (control.c): its
 * refresh boundaries (GPU_DISPLAY_REFRESH) and its power
 * (GPU_DISPLAY_POWER).  The display operations (display.c) check the
 * display and its generation, then call these.
 */

#ifndef DRIVERS_GPU_I915_DISPLAY_CONTROL_H
#define DRIVERS_GPU_I915_DISPLAY_CONTROL_H

#include <stdint.h>

#include <uapi/gpu-display.h>

struct i915_device;
struct i915_display;

void drv_i915_display_refresh_init(struct i915_display *display);
void drv_i915_display_refresh_up(struct i915_display *display, int up);
int drv_i915_display_refresh_wait(struct i915_device *device, int lit_possible, struct gpu_display_refresh *request);
int drv_i915_display_power_set(struct i915_device *device, void *session, int off);
void drv_i915_display_power_restore_locked(struct i915_device *device);

#endif
