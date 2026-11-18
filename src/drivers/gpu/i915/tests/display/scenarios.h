/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The display scenarios the test runner can select.
 *
 * Each runs on the device's start worker after the device has started and
 * before its node is served, and logs its own verdict.  The runner's table
 * names them; the display tests define them.
 */

#ifndef DRIVERS_GPU_I915_TESTS_DISPLAY_SCENARIOS_H
#define DRIVERS_GPU_I915_TESTS_DISPLAY_SCENARIOS_H

struct i915_device;

void drv_i915_test_display_lcdb(struct i915_device *device);
void drv_i915_test_display_lcdc(struct i915_device *device);
void drv_i915_test_display_lcdd(struct i915_device *device);
void drv_i915_test_display_lcdg(struct i915_device *device);
void drv_i915_test_display_lcdo(struct i915_device *device);
void drv_i915_test_display_lcdr(struct i915_device *device);
void drv_i915_test_display_hdmib(struct i915_device *device);
void drv_i915_test_display_dual(struct i915_device *device);
void drv_i915_test_display_dual_share(struct i915_device *device);
void drv_i915_test_display_n1(struct i915_device *device);
void drv_i915_test_display_aux(struct i915_device *device);
void drv_i915_test_display_hdmi_edid(struct i915_device *device);
void drv_i915_test_display_hdmi_hpd(struct i915_device *device);
void drv_i915_test_display_ktest(struct i915_device *device);

#endif
