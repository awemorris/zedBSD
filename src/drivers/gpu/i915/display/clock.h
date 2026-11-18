/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The display's CD clock (clock.c): its state over the xd clock, read,
 * started, changed and stopped.
 */

#ifndef DRIVERS_GPU_I915_DISPLAY_CLOCK_H
#define DRIVERS_GPU_I915_DISPLAY_CLOCK_H

#include "internal.h"

struct i915_xd;

void drv_i915_cdclk_prepare(struct i915_cdclk_dev *cd, struct i915_xd *xd);
void drv_i915_cdclk_read(struct i915_cdclk_dev *cd, struct i915_cdclk_config *cfg);
void drv_i915_cdclk_refresh(struct i915_cdclk_dev *cd);
void drv_i915_cdclk_start(struct i915_cdclk_dev *cd);
void drv_i915_cdclk_stop(struct i915_cdclk_dev *cd);
uint32_t drv_i915_cdclk_max(const struct i915_cdclk_dev *cd);

#endif
