/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The start and the stop of the display core of the display 12 and 13
 * code (xd-core.c): the order in which the display's shared parts are
 * brought up before any output, and taken down after the last.
 *
 * The start: the DC states the firmware left cleared; on a TGP or ADP
 * south display the DPMG unit's clock gate off; the PCH reset handshake;
 * every combo PHY that is not up; power well 1; the CD clock; on display
 * 12 the tracker service of every DBUF slice; the DBUF (the slices the
 * firmware left, and at least slice 1); on display 12 the memory bus
 * ABOX credits; the bandwidth buddy from the memory type and channels
 * (turned off for a memory the tables do not know); the four DC power
 * request chicken bits; on display 13 the fatal error mask.  A fault of
 * the register access after the PHYs, power well 1 or the CD clock stops
 * the start there.
 *
 * The stop: the DC states left, the DBUF off, the CD clock down, power
 * well 1 off, the combo PHYs down.
 *
 * Power well 1, the CD clock and the DBUF slices' power go through hooks
 * of the caller, which owns their counts, their state and their locks.
 */

#ifndef DRIVERS_GPU_I915_DISPLAY_XD_CORE_H
#define DRIVERS_GPU_I915_DISPLAY_XD_CORE_H

#include <stdint.h>

#include "xd-io.h"
#include "xd-platform.h"
#include "xd-power.h"

/*
 * The caller's parts of the start and the stop: power well 1 on (0, or
 * EIO for a fault of the register access) and off, the CD clock up and
 * down, the DBUF slices' power, and whether the register access has
 * faulted since the start.
 */
struct i915_xd_core_hooks {
	int (*well1_on)(void *context);
	void (*well1_off)(void *context);
	void (*cdclk_on)(void *context);
	void (*cdclk_off)(void *context);
	void (*dbuf)(void *context, uint8_t slices);
	int (*faulted)(void *context);
	void *context;
};

/*
 * What the start needs to know beyond the platform: the kind of the
 * south display, the display stepping, and the memory type
 * (I915_XD_DRAM_, 0 for unknown) and channel count.
 */
struct i915_xd_core_input {
	int pch_kind;
	uint8_t step;
	unsigned dram_type;
	unsigned dram_channels;
};

/* Where a start stopped. */
#define I915_XD_CORE_DONE		0U
#define I915_XD_CORE_STOP_PHYS		1U
#define I915_XD_CORE_STOP_WELL1		2U
#define I915_XD_CORE_STOP_CDCLK		3U

int drv_i915_xd_core_start(struct i915_xd_io *io, const struct i915_xd_platform *platform, struct i915_xd_power *power, const struct i915_xd_core_input *input, const struct i915_xd_core_hooks *hooks, unsigned *stop, unsigned *phys);
void drv_i915_xd_display_wa(struct i915_xd_io *io, const struct i915_xd_platform *platform);
void drv_i915_xd_core_stop(struct i915_xd_io *io, const struct i915_xd_platform *platform, struct i915_xd_power *power, const struct i915_xd_core_hooks *hooks, struct i915_xd_dc_report *report, unsigned *phy_a_down);

#endif /* DRIVERS_GPU_I915_DISPLAY_XD_CORE_H */
