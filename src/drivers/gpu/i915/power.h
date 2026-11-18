/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * GT power management.
 *
 * The PCODE mailbox, through which the driver asks the power controller
 * firmware for memory, clock and frequency information and announces clock
 * changes, and the GT power states built on it.
 */

#ifndef DRIVERS_GPU_I915_POWER_H
#define DRIVERS_GPU_I915_POWER_H

#include <stdint.h>

struct i915_mmio;
struct mutex;

/*
 * The GT driver mailbox (PCODE).
 *
 * The device-owned sideband lock, a mutex, serializes every exchange: an
 * exchange first busy-polls for the answer and then, for a read, polls in a
 * way that may sleep.  drv_i915_pcode_request() asks until the masked reply
 * is the one wanted.  Every function returns 0 or a positive errno: the
 * status code the firmware reported, ETIMEDOUT, EAGAIN for a mailbox the
 * firmware still owns, or EIO when the time base failed.
 */
int drv_i915_pcode_read(struct mutex *sb_lock, struct i915_mmio *mmio, uint32_t mbox, uint32_t *val, uint32_t *val1);
int drv_i915_pcode_write(struct mutex *sb_lock, struct i915_mmio *mmio, uint32_t mbox, uint32_t val);
int drv_i915_pcode_request(struct mutex *sb_lock, struct i915_mmio *mmio, uint32_t mbox, uint32_t request, uint32_t reply_mask, uint32_t reply, int timeout_base_ms);

#endif
