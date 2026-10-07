/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * GT reset.
 *
 * The full-GT soft reset the device start issues before it programs the GT,
 * and that the recovery path issues after a hang.  The control flow is Linux's
 * __intel_gt_reset(ALL_ENGINES): an uncore-lock section per attempt, a retry
 * only while the acknowledge times out, two write and poll passes per attempt
 * on Alder Lake-P, and a settle delay after the acknowledge.
 *
 * The reset of one engine, which the recovery from a video engine hang
 * issues (ws083-p007): __intel_gt_reset(engine->mask) with the engine's
 * ready handshake and the lock of the scaler and format converter it uses.
 */

#ifndef DRIVERS_GPU_I915_RESET_H
#define DRIVERS_GPU_I915_RESET_H

struct i915_engine_info;
struct i915_gt_info;
struct i915_mmio;
struct spinlock;

/* How many microseconds one reset acknowledge poll may take on the device paths. */
#define I915_GT_RESET_ACK_US	2000U

int drv_i915_gt_reset_all(struct spinlock *uncore_lock, struct i915_mmio *mmio, unsigned fast_us);
int drv_i915_gt_reset_engine(struct spinlock *uncore_lock, struct i915_mmio *mmio, const struct i915_gt_info *gt, const struct i915_engine_info *engine, unsigned fast_us);

/*
 * Recovery of the GPU node.
 *
 * The GPU core's recovery operations: stopping a session, the device-wide
 * fault, the checked device reset and the isolation of one session.  They
 * work on the node's request queue and quarantine; the hardware resets they
 * would issue are not connected yet and fail.
 */

struct drv_gpu_ops;

void drv_i915_recovery_bind_ops(struct drv_gpu_ops *ops);

#endif
