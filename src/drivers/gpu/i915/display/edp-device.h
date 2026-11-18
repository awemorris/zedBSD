/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The display's built-in panel device and the kernel backend it runs on
 * (edp-device.c).
 *
 * The device takes the panel's sink through the xd code (xd-edp.c) on the
 * locks, threads, delayed work and power references of its backend,
 * plans the panel's mode and link, and gives the resident run its sink
 * and panel hooks.  The external DisplayPort ports run on the same
 * backend.
 */

#ifndef DRIVERS_GPU_I915_DISPLAY_EDP_DEVICE_H
#define DRIVERS_GPU_I915_DISPLAY_EDP_DEVICE_H

#include "internal.h"

/* The kernel backend: sleeps, its locks and threads, and the hooks of an environment bound to it. */
void drv_i915_dp_kernel_sleep_us(struct i915_dp_kernel *k, unsigned us);
int drv_i915_dp_kernel_sync_start(struct i915_dp_kernel *k);
void drv_i915_dp_kernel_sync_stop(struct i915_dp_kernel *k);
void drv_i915_dp_kernel_bind_sync(struct i915_dp_kernel *k, struct i915_dp_env *env);
void drv_i915_dp_kernel_bind(struct i915_dp_kernel *k, struct i915_dp_env *env);

/* The panel device: prepared, started, its connector built on a port (1: not the panel's port), stopped. */
void drv_i915_edp_device_prepare(struct i915_edp_device *dev, struct i915_mmio *mmio, struct i915_display_power *power, struct i915_vbt_state *vbt);
int drv_i915_edp_device_start(struct i915_edp_device *dev);
int drv_i915_edp_device_init_connector(void *ctx, int port);
void drv_i915_edp_device_fini(struct i915_edp_device *dev);

/* The panel's sink and panel power as a run's backend hooks (ctx: the run). */
long drv_i915_edp_emit_dpcd_read(void *ctx, unsigned offset, uint8_t *buf, size_t size);
long drv_i915_edp_emit_dpcd_write(void *ctx, unsigned offset, const uint8_t *buf, size_t size);
int drv_i915_edp_emit_read_dpcd_caps(void *ctx, uint8_t dpcd[15]);
int drv_i915_edp_emit_panel(void *ctx, int op);

#endif /* DRIVERS_GPU_I915_DISPLAY_EDP_DEVICE_H */
