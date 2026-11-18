/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The memory's bandwidth for the display 11 to 13 code (xd-bandwidth.c).
 *
 * The firmware tells the memory's type and channels and the points the
 * memory may run at (QGV points: a DRAM clock and its timings; PSF
 * points: a fabric clock).  From them the display's share of each
 * point's bandwidth is worked out for each number of planes the display
 * fetches at once.  The start then keeps the memory at the points of the
 * most bandwidth (SAGV off) where the display controls SAGV, so that a
 * picture never waits for a point change.
 */

#ifndef DRIVERS_GPU_I915_DISPLAY_XD_BANDWIDTH_H
#define DRIVERS_GPU_I915_DISPLAY_XD_BANDWIDTH_H

#include <stdint.h>

#include "xd-io.h"
#include "xd-platform.h"

/* The QGV and PSF points the firmware may report, and the groups of plane counts. */
#define I915_XD_BW_QGV_POINTS		8U
#define I915_XD_BW_PSF_POINTS		3U
#define I915_XD_BW_GROUPS		6U

/*
 * Whether the memory switches points by itself (SAGV): not known yet,
 * kept at one point by the display, switching, or out of the display's
 * control (the firmware reported a single point).
 */
#define I915_XD_SAGV_UNKNOWN		0
#define I915_XD_SAGV_OFF		1
#define I915_XD_SAGV_ON			2
#define I915_XD_SAGV_NOT_CONTROLLED	3

/*
 * What one display's memory gives it, read once by the start.
 *
 * dram_read and dram_type, channels: the firmware's answer about the
 * memory (dram_type is I915_XD_DRAM_*, 0 when not known).  qgv_points and
 * psf_points: the points the firmware advertised.  group_planes[g]: the
 * most planes group g serves (group 0 serves any count the others do
 * not).  derated_mbps[g][p], peak_mbps[g][p]: the display's share and
 * the peak of QGV point p for group g, in MB/s.  psf_mbps[p]: PSF point
 * p's bandwidth.  table_valid: every point's share could be worked out.
 * sagv: I915_XD_SAGV_*.  restriction: the mailbox's mask of the points
 * the memory may not use (0 until the start restricts it), with the
 * points kept and the mailbox's answer.
 */
struct i915_xd_bandwidth {
	int dram_read;
	unsigned dram_type;
	unsigned channels;
	unsigned qgv_points;
	unsigned psf_points;
	unsigned group_planes[I915_XD_BW_GROUPS];
	uint32_t derated_mbps[I915_XD_BW_GROUPS][I915_XD_BW_QGV_POINTS];
	uint32_t peak_mbps[I915_XD_BW_GROUPS][I915_XD_BW_QGV_POINTS];
	uint32_t psf_mbps[I915_XD_BW_PSF_POINTS];
	int table_valid;
	int sagv;
	int restricted;
	uint16_t restriction;
	unsigned kept_qgv;
	unsigned kept_psf;
	int restrict_error;
};

int drv_i915_xd_bw_read_dram(struct i915_xd_io *io, struct i915_xd_bandwidth *bw);
int drv_i915_xd_bw_read(struct i915_xd_io *io, const struct i915_xd_platform *platform, struct i915_xd_bandwidth *bw);
int drv_i915_xd_bw_hold_sagv_off(struct i915_xd_io *io, const struct i915_xd_platform *platform, struct i915_xd_bandwidth *bw);
uint32_t drv_i915_xd_bw_point_mbps(const struct i915_xd_bandwidth *bw, const struct i915_xd_platform *platform, unsigned planes, unsigned point);
unsigned drv_i915_xd_bw_allowed_points(const struct i915_xd_bandwidth *bw);

#endif /* DRIVERS_GPU_I915_DISPLAY_XD_BANDWIDTH_H */
