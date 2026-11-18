/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The runs of the resident node's outputs (resident.c).
 *
 * The resident run lights the resident node's output on screen 0 with two
 * full-size buffers, lets the request worker serve presentations while the
 * picture is up, and stops it again; the second output's run uses the
 * same hooks, preflight and configuration on screen 1.  A run whose stop
 * was not confirmed keeps what the display may still read, and the
 * device's teardown asks these functions before it releases anything.
 */

#ifndef DRIVERS_GPU_I915_DISPLAY_RESIDENT_H
#define DRIVERS_GPU_I915_DISPLAY_RESIDENT_H

#include "internal.h"

/* Lights the resident output, serves while its picture is up and stops it: 0, EINVAL, EBUSY, EAGAIN or EIO. */
int drv_i915_resident_run(struct i915_display *display, const struct i915_lcd_kernel_deps *d, int (*serve)(void *ctx), void *ctx);

/* Reports whether a run left resources the display or the GPU may still use. */
int drv_i915_resident_retained(struct i915_display *display);

/* Reports whether a run, a screen or the second output kept buffers because a stop was not confirmed. */
int drv_i915_resident_abandoned(struct i915_display *display);

/* Reports whether the GPU was not shown to be done with a buffer a run kept. */
int drv_i915_resident_gpu_retained(struct i915_display *display);

/* Fills the parameters of an output that is not the built-in panel: 0, or EOPNOTSUPP. */
int drv_i915_resident_output_params(const struct i915_display_output *output, struct i915_lcd_run_params *params);

/* Binds a run's hooks over the display's objects; params names the output (NULL: the built-in panel). */
void drv_i915_resident_bind(struct i915_display *display, struct i915_lcd_kernel *k, const struct i915_lcd_kernel_deps *d, const struct i915_lcd_run_params *params);

/* Checks that the run's pipe and port are idle and its inputs complete: 0, or EBUSY. */
int drv_i915_resident_preflight(struct i915_lcd_kernel *k);

/* Fills the configuration a screen is prepared from for an output, all but its framebuffer: 0, or EINVAL. */
int drv_i915_resident_config(struct i915_lcd_kernel *k, const struct i915_display_output *output, struct i915_xd_picture_config *config);

/* Lowers an external DP output's link after it did not train: 0, ENOSPC or EINVAL. */
int drv_i915_resident_link_fallback(struct i915_display *display, struct i915_display_output *output, int enable_rc);

#endif /* DRIVERS_GPU_I915_DISPLAY_RESIDENT_H */
