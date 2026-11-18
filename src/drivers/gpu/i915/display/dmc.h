/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The DMC loader of the display and the pipe DMC switches of the mode set
 * (dmc-shell.c).  The parse and the load are the xd DMC code's (xd-dmc.h).
 *
 * The pipe DMC enable and disable take types of the modeset environment and
 * are declared in modeset-internal.h.
 */

#ifndef DRIVERS_GPU_I915_DISPLAY_DMC_H
#define DRIVERS_GPU_I915_DISPLAY_DMC_H

#include "internal.h"

int drv_i915_dmc_has_payload(const struct i915_dmc *dmc);
void drv_i915_dmc_init(struct i915_dmc_dev *dev, struct i915_workqueue *wq, struct i915_mmio *mmio, struct i915_display_power *power, struct i915_xd *xd);
void drv_i915_dmc_fini(struct i915_dmc_dev *dev, uint64_t deadline);
int drv_i915_dmc_resume(struct i915_dmc_dev *dev);

#endif
