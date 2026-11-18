/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The memory's bandwidth for the display 11 to 13 code (see
 * xd-bandwidth.h).
 */

#include "xd-bandwidth.h"
#include "xd-io.h"
#include "xd-platform.h"

#include "../intel/display-regs-xd.h"

#include <kern/kcrt.h>
#include <kern/klog.h>

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>

/*
 * How long one mailbox command may take to be answered: the fast wait
 * and the slow one together.
 */
#define I915_XD_BW_MAILBOX_US		20500U

/*
 * How long the restriction of the points is asked for: 1 ms with 10 us
 * between the requests, then 50 ms more.
 */
#define I915_XD_BW_RESTRICT_GAP_US	10U
#define I915_XD_BW_RESTRICT_US		1000U
#define I915_XD_BW_RESTRICT_MORE_US	50000U

/*
 * The fields of the firmware's answer about the memory: the type, the
 * channels, the QGV points and the PSF points; and the codes of the type
 * field.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/soc/intel_dram.c
 * (icl_pcode_read_mem_global_info: the gen12 decode of the type, the
 * channels, the QGV and the PSF GV points).
 */
#define I915_XD_BW_DRAM_TYPE		0x0000000fU
#define I915_XD_BW_DRAM_CHANNELS	0x000000f0U
#define I915_XD_BW_DRAM_CHANNELS_SHIFT	4U
#define I915_XD_BW_DRAM_QGV		0x00000f00U
#define I915_XD_BW_DRAM_QGV_SHIFT	8U
#define I915_XD_BW_DRAM_PSF		0x00003000U
#define I915_XD_BW_DRAM_PSF_SHIFT	12U
#define I915_XD_BW_CODE_DDR4		0U
#define I915_XD_BW_CODE_DDR5		1U
#define I915_XD_BW_CODE_LPDDR5		2U
#define I915_XD_BW_CODE_LPDDR4		3U
#define I915_XD_BW_CODE_DDR3		4U
#define I915_XD_BW_CODE_LPDDR3		5U

/*
 * The fields of a QGV point's answer: the DRAM clock in units of
 * 16.667 MHz, tRP and tRCD in the first word; tRDPRE and tRAS in the
 * second.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_bw.c
 * (icl_pcode_read_qgv_point_info).
 */
#define I915_XD_BW_QGV_DCLK		0x0000ffffU
#define I915_XD_BW_QGV_TRP_SHIFT	16U
#define I915_XD_BW_QGV_TRCD_SHIFT	24U
#define I915_XD_BW_QGV_TRAS_SHIFT	8U
#define I915_XD_BW_QGV_BYTE		0xffU
#define I915_XD_BW_DCLK_UNIT_KHZ	16667U

/*
 * The system agent's numbers the display's share is worked out with
 * (Alder Lake-P's, used for every platform as before), and the depth of
 * the PCH's request queue.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_bw.c
 * (adlp_sa_info: deburst, deprogbwlimit, displayrtids, derating;
 * tgl_get_bw_info: ipqdepthpch).
 */
#define I915_XD_BW_DEBURST		16
#define I915_XD_BW_DEPROG_GBPS		38
#define I915_XD_BW_DISPLAY_RTIDS	256
#define I915_XD_BW_DERATING_PERCENT	20
#define I915_XD_BW_PCH_QUEUE		16

/*
 * A memory type's burst length, most channels, channel width (bits) and
 * deinterleave for Y-tiled surfaces.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_bw.c
 * (icl_get_qgv_points: the display 12 values of each DRAM type).
 */
struct i915_xd_bw_geometry {
	unsigned burst;
	unsigned max_channels;
	unsigned channel_width;
	unsigned deinterleave;
};

/*
 * One QGV point as the firmware reports it: the DRAM clock in MHz and
 * its timings in clocks.
 */
struct i915_xd_bw_point {
	unsigned dclk_mhz;
	unsigned t_rp;
	unsigned t_rcd;
	unsigned t_rdpre;
	unsigned t_ras;
	unsigned t_rc;
};

static void i915_xd_bw_geometry(unsigned dram_type, struct i915_xd_bw_geometry *geometry);
static int i915_xd_bw_read_points(struct i915_xd_io *io, struct i915_xd_bandwidth *bw, struct i915_xd_bw_point *points, unsigned *psf_clocks);
static void i915_xd_bw_table(struct i915_xd_bandwidth *bw, const struct i915_xd_bw_point *points, const unsigned *psf_clocks);
static unsigned i915_xd_bw_low_bits(unsigned count);
static unsigned i915_xd_bw_div_up(unsigned value, unsigned divisor);
static uint16_t i915_xd_bw_advertised(const struct i915_xd_bandwidth *bw);
static int i915_xd_bw_restrict(struct i915_xd_io *io, uint16_t mask);

/*
 * Reads the memory's type, channels and points from the firmware.
 *
 * Returns 0, the mailbox's error, or EINVAL for a type the decode does
 * not know (the counts then stay 0).  A failure is tolerated by the
 * caller: the display goes on without the memory's numbers.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/soc/intel_dram.c
 * (gen12_get_dram_info, icl_pcode_read_mem_global_info).
 */
int
drv_i915_xd_bw_read_dram(
	struct i915_xd_io *io,
	struct i915_xd_bandwidth *bw)
{
	uint32_t answer;
	unsigned code;
	int error;

	/* Asks the firmware about the memory. */
	answer = 0U;
	error = drv_i915_xd_io_mailbox(io, I915_XD_MAILBOX_MEMORY_INFO | I915_XD_MAILBOX_MEMORY_INFO_GLOBAL, &answer, NULL, I915_XD_BW_MAILBOX_US);
	if (error != 0) {
		kern_logf("i915: display: memory: the firmware did not describe the memory (%d)\n", error);
		return error;
	}

	/* The type of the memory. */
	code = answer & I915_XD_BW_DRAM_TYPE;
	switch (code) {
	case I915_XD_BW_CODE_DDR4:
		bw->dram_type = I915_XD_DRAM_DDR4;
		break;
	case I915_XD_BW_CODE_DDR5:
		bw->dram_type = I915_XD_DRAM_DDR5;
		break;
	case I915_XD_BW_CODE_LPDDR5:
		bw->dram_type = I915_XD_DRAM_LPDDR5;
		break;
	case I915_XD_BW_CODE_LPDDR4:
		bw->dram_type = I915_XD_DRAM_LPDDR4;
		break;
	case I915_XD_BW_CODE_DDR3:
		bw->dram_type = I915_XD_DRAM_DDR3;
		break;
	case I915_XD_BW_CODE_LPDDR3:
		bw->dram_type = I915_XD_DRAM_LPDDR3;
		break;
	default:
		bw->dram_type = 0U;
		kern_logf("i915: display: memory: a memory type the decode does not know (0x%x)\n", code);
		return EINVAL;
	}

	/* The channels and the points the firmware advertises. */
	bw->channels = (answer & I915_XD_BW_DRAM_CHANNELS) >> I915_XD_BW_DRAM_CHANNELS_SHIFT;
	bw->qgv_points = (answer & I915_XD_BW_DRAM_QGV) >> I915_XD_BW_DRAM_QGV_SHIFT;
	bw->psf_points = (answer & I915_XD_BW_DRAM_PSF) >> I915_XD_BW_DRAM_PSF_SHIFT;
	bw->dram_read = 1;
	kern_logf("i915: display: memory: raw=0x%08x type=%u channels=%u qgv_points=%u psf_points=%u\n",
	    answer,
	    bw->dram_type,
	    bw->channels,
	    bw->qgv_points,
	    bw->psf_points);

	/* Succeeded: the memory is described. */
	return 0;
}

/*
 * Reads the memory and works out the display's share of each point's
 * bandwidth.
 *
 * A memory the firmware did not describe has no points: the table is
 * empty.  A point that cannot be read leaves the bandwidth unknown
 * (sagv stays I915_XD_SAGV_UNKNOWN) and is returned.  One QGV point
 * means the firmware keeps the memory at it: SAGV is then out of the
 * display's control.  Returns 0, or the mailbox's error of a point.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_bw.c
 * (intel_bw_init_hw, tgl_get_bw_info, icl_get_qgv_points).
 */
int
drv_i915_xd_bw_read(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform,
	struct i915_xd_bandwidth *bw)
{
	struct i915_xd_bw_point points[I915_XD_BW_QGV_POINTS];
	unsigned psf_clocks[I915_XD_BW_PSF_POINTS];
	int error;

	UNUSED_PARAMETER(platform);

	/* Starts with nothing known. */
	kern_memset(bw, 0, sizeof(*bw));
	kern_memset(points, 0, sizeof(points));
	kern_memset(psf_clocks, 0, sizeof(psf_clocks));

	/* The memory itself; a memory not described has no points. */
	(void)drv_i915_xd_bw_read_dram(io, bw);

	/* No more points than the table holds. */
	if (bw->qgv_points > I915_XD_BW_QGV_POINTS)
		bw->qgv_points = I915_XD_BW_QGV_POINTS;

	/* The points' clocks and timings. */
	error = i915_xd_bw_read_points(io, bw, points, psf_clocks);
	if (error != 0) {
		kern_logf("i915: display: memory: the points could not be read (%d): the bandwidth limits are not known\n", error);
		return error;
	}

	/* The display's share of each point for each group of plane counts. */
	i915_xd_bw_table(bw, points, psf_clocks);

	/* One QGV point: the firmware keeps the memory there. */
	bw->sagv = I915_XD_SAGV_ON;
	if (bw->qgv_points == 1U)
		bw->sagv = I915_XD_SAGV_NOT_CONTROLLED;

	/* Logs what the memory's description said. */
	kern_logf("i915: display: memory: sagv=%d qgv_points=%u psf_points=%u table_valid=%d\n",
	    bw->sagv,
	    bw->qgv_points,
	    bw->psf_points,
	    bw->table_valid);

	/* Succeeded: the table is worked out. */
	return 0;
}

/*
 * Keeps the memory at its points of the most bandwidth, where the
 * display controls SAGV (display 11 to 13).
 *
 * The QGV point of the most bandwidth for one plane and every PSF point
 * of the most bandwidth are kept; the firmware is asked to leave the
 * other advertised points.  With one QGV point left the memory no longer
 * switches (SAGV off).  A refusal is reported in bw->restrict_error and
 * returned; the display goes on either way.  Returns 0, or the
 * mailbox's error.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_bw.c
 * (intel_bw_init, icl_force_disable_sagv, icl_max_bw_qgv_point_mask,
 * icl_max_bw_psf_gv_point_mask, icl_prepare_qgv_points_mask,
 * icl_pcode_restrict_qgv_points).
 */
int
drv_i915_xd_bw_hold_sagv_off(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform,
	struct i915_xd_bandwidth *bw)
{
	uint32_t best;
	uint32_t mbps;
	uint16_t kept;
	unsigned point;
	unsigned left;
	int error;

	/* Only display 11 to 13 keep the points through this mailbox. */
	if (platform == NULL ||
	    platform->display_ver < 11U ||
	    platform->display_ver > 13U)
		return 0;

	/* SAGV the firmware keeps out of the display's control is left alone. */
	if (bw->sagv == I915_XD_SAGV_NOT_CONTROLLED)
		return 0;

	/* The first QGV point of the most bandwidth for one plane. */
	best = 0U;
	bw->kept_qgv = 0U;
	for (point = 0U; point < bw->qgv_points; point++) {
		mbps = drv_i915_xd_bw_point_mbps(bw, platform, 1U, point);
		if (mbps > best) {
			bw->kept_qgv = 1U << point;
			best = mbps;
		}
	}

	/* Every PSF point of the most bandwidth. */
	best = 0U;
	bw->kept_psf = 0U;
	for (point = 0U; point < bw->psf_points; point++) {
		mbps = bw->psf_mbps[point];
		if (mbps > best) {
			bw->kept_psf = 1U << point;
			best = mbps;
		} else if (mbps == best) {
			bw->kept_psf |= 1U << point;
		}
	}

	/* The mask of every advertised point but the kept ones. */
	kept = (uint16_t)((bw->kept_qgv & I915_XD_MAILBOX_SAGV_REQUEST_QGV) | ((bw->kept_psf << 8) & I915_XD_MAILBOX_SAGV_REQUEST_PSF));
	bw->restriction = (uint16_t)(~kept & i915_xd_bw_advertised(bw));
	bw->restricted = 1;
	kern_logf("i915: display: memory: keeping SAGV off: mask 0x%x (qgv kept 0x%x, psf kept 0x%x)\n",
	    (unsigned)bw->restriction,
	    bw->kept_qgv,
	    bw->kept_psf);

	/* Asks the firmware for the restriction. */
	error = i915_xd_bw_restrict(io, bw->restriction);
	bw->restrict_error = error;
	if (error != 0) {
		kern_logf("i915: display: memory: the firmware refused the points' restriction (%d), mask 0x%x\n", error, (unsigned)bw->restriction);
		return error;
	}

	/* One QGV point left: the memory no longer switches. */
	left = (unsigned)(~bw->restriction) & (unsigned)i915_xd_bw_advertised(bw) & I915_XD_MAILBOX_SAGV_REQUEST_QGV;
	bw->sagv = I915_XD_SAGV_ON;
	if (left != 0U && (left & (left - 1U)) == 0U)
		bw->sagv = I915_XD_SAGV_OFF;

	/* Succeeded: the memory is kept at the points. */
	return 0;
}

/*
 * Reports the display's share of a QGV point's bandwidth, in MB/s, for a
 * number of planes fetched at once (0 counts as one), or 0 for a point
 * the firmware did not advertise.
 *
 * Display 12 and later take the group of the fewest planes that still
 * covers the count, and group 0 for more; display 11 takes the first
 * group whose plane count the count reaches, and nothing when none does.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_bw.c
 * (icl_qgv_bw, tgl_max_bw_index, icl_max_bw_index).
 */
uint32_t
drv_i915_xd_bw_point_mbps(
	const struct i915_xd_bandwidth *bw,
	const struct i915_xd_platform *platform,
	unsigned planes,
	unsigned point)
{
	unsigned group;

	/* A point the firmware did not advertise has no bandwidth. */
	if (point >= bw->qgv_points || point >= I915_XD_BW_QGV_POINTS)
		return 0U;

	/* No planes are counted as one. */
	if (planes < 1U)
		planes = 1U;

	/* Display 11: the first group whose plane count the planes reach. */
	if (platform != NULL && platform->display_ver < 12U) {
		for (group = 0U; group < I915_XD_BW_GROUPS; group++) {
			if (planes >= bw->group_planes[group])
				return bw->derated_mbps[group][point];
		}

		/* A plane count past the groups has no bandwidth. */
		return 0U;
	}

	/* Display 12 and later: the last group whose plane count covers the planes. */
	for (group = I915_XD_BW_GROUPS; group > 0U; group--) {
		if (planes <= bw->group_planes[group - 1U])
			return bw->derated_mbps[group - 1U][point];
	}

	/* Succeeded: the first group serves any other count. */
	return bw->derated_mbps[0][point];
}

/*
 * Reports the QGV points the memory may run at: every advertised point
 * the start's restriction left (all of them before it).
 */
unsigned
drv_i915_xd_bw_allowed_points(
	const struct i915_xd_bandwidth *bw)
{
	unsigned advertised;

	/* The advertised QGV points. */
	advertised = i915_xd_bw_low_bits(bw->qgv_points);

	/* Succeeded: the points the restriction did not take away. */
	return (unsigned)(~bw->restriction) & advertised & I915_XD_MAILBOX_SAGV_REQUEST_QGV;
}

/*
 * Gives a memory type's burst length, most channels, channel width and
 * deinterleave for Y-tiled surfaces; a type not known gets a burst of 16
 * on one channel.
 */
static void
i915_xd_bw_geometry(
	unsigned dram_type,
	struct i915_xd_bw_geometry *geometry)
{
	/* The values of each type. */
	switch (dram_type) {
	case I915_XD_DRAM_DDR4:
		geometry->burst = 8U;
		geometry->max_channels = 2U;
		geometry->channel_width = 64U;
		geometry->deinterleave = 1U;
		break;
	case I915_XD_DRAM_DDR5:
		geometry->burst = 16U;
		geometry->max_channels = 4U;
		geometry->channel_width = 32U;
		geometry->deinterleave = 1U;
		break;
	case I915_XD_DRAM_LPDDR4:
	case I915_XD_DRAM_LPDDR5:
		geometry->burst = 16U;
		geometry->max_channels = 8U;
		geometry->channel_width = 16U;
		geometry->deinterleave = 2U;
		break;
	default:
		geometry->burst = 16U;
		geometry->max_channels = 1U;
		geometry->channel_width = 0U;
		geometry->deinterleave = 0U;
		break;
	}
}

/*
 * Reads every advertised QGV point's clock and timings and the PSF
 * points' clocks.  PSF points that cannot be read are dropped (the
 * bandwidth is worked out without them).  Returns 0, or the mailbox's
 * error of a QGV point.
 */
static int
i915_xd_bw_read_points(
	struct i915_xd_io *io,
	struct i915_xd_bandwidth *bw,
	struct i915_xd_bw_point *points,
	unsigned *psf_clocks)
{
	struct i915_xd_bw_point *point;
	uint32_t first;
	uint32_t second;
	unsigned index;
	int error;

	/* Reads each QGV point. */
	for (index = 0U; index < bw->qgv_points; index++) {
		/* Asks the firmware for the point. */
		first = 0U;
		second = 0U;
		error = drv_i915_xd_io_mailbox(io, I915_XD_MAILBOX_MEMORY_INFO | I915_XD_MAILBOX_MEMORY_INFO_QGV(index), &first, &second, I915_XD_BW_MAILBOX_US);
		if (error != 0)
			return error;

		/* The clock in MHz, rounded, and the timings. */
		point = &points[index];
		point->dclk_mhz = i915_xd_bw_div_up(I915_XD_BW_DCLK_UNIT_KHZ * (first & I915_XD_BW_QGV_DCLK) + 500U, 1000U);
		point->t_rp = (first >> I915_XD_BW_QGV_TRP_SHIFT) & I915_XD_BW_QGV_BYTE;
		point->t_rcd = (first >> I915_XD_BW_QGV_TRCD_SHIFT) & I915_XD_BW_QGV_BYTE;
		point->t_rdpre = second & I915_XD_BW_QGV_BYTE;
		point->t_ras = (second >> I915_XD_BW_QGV_TRAS_SHIFT) & I915_XD_BW_QGV_BYTE;
		point->t_rc = point->t_rp + point->t_ras;
		kern_logf("i915: display: memory: QGV %u: dclk=%u MHz tRP=%u tRDPRE=%u tRAS=%u tRCD=%u tRC=%u\n",
		    index,
		    point->dclk_mhz,
		    point->t_rp,
		    point->t_rdpre,
		    point->t_ras,
		    point->t_rcd,
		    point->t_rc);
	}

	/* Without PSF points nothing more is read. */
	if (bw->psf_points == 0U)
		return 0;

	/* The PSF points' clocks, one byte each; a failed read drops them. */
	first = 0U;
	error = drv_i915_xd_io_mailbox(io, I915_XD_MAILBOX_MEMORY_INFO | I915_XD_MAILBOX_MEMORY_INFO_PSF, &first, NULL, I915_XD_BW_MAILBOX_US);
	if (error != 0) {
		kern_logf("i915: display: memory: the PSF points could not be read (%d): left out\n", error);
		bw->psf_points = 0U;
		return 0;
	}

	/* Takes each point's clock. */
	for (index = 0U; index < I915_XD_BW_PSF_POINTS; index++) {
		psf_clocks[index] = first & I915_XD_BW_QGV_BYTE;
		first >>= 8;
	}

	/* Succeeded: every point is read. */
	return 0;
}

/*
 * Works out each group's plane count and the display's share and the
 * peak of each QGV point, and each PSF point's bandwidth.
 *
 * A group holds the cache lines one request of its size moves; a group
 * of fewer lines than a channel group leaves room in the request queue
 * for more planes, which is the next group's plane count.  A point's
 * share is the lines' bandwidth over the row cycle, derated and capped by
 * the system agent's limit.  A point whose row cycle is not positive
 * gets nothing and leaves the table not valid.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_bw.c
 * (tgl_get_bw_info, icl_sagv_max_dclk).
 */
static void
i915_xd_bw_table(
	struct i915_xd_bandwidth *bw,
	const struct i915_xd_bw_point *points,
	const unsigned *psf_clocks)
{
	struct i915_xd_bw_geometry geometry;
	const struct i915_xd_bw_point *point;
	unsigned channels;
	unsigned dclk_max;
	unsigned group;
	unsigned index;
	int peak;
	int deprog;
	int queue;
	int lines_per_group;
	int lines;
	int cycle;
	int raw;
	int derated;
	int valid;

	/* The memory's geometry and channels; LPDDR4 and LPDDR5 count two a channel. */
	i915_xd_bw_geometry(bw->dram_type, &geometry);
	channels = bw->channels;
	if (channels < 1U)
		channels = 1U;
	if (bw->dram_type == I915_XD_DRAM_LPDDR4 || bw->dram_type == I915_XD_DRAM_LPDDR5)
		channels *= 2U;

	/* A deinterleave the type left open follows from the channels; fewer channels than the most halve it. */
	if (geometry.deinterleave == 0U)
		geometry.deinterleave = i915_xd_bw_div_up(channels, 4U);
	if (channels < geometry.max_channels) {
		geometry.deinterleave = i915_xd_bw_div_up(geometry.deinterleave, 2U);
		if (geometry.deinterleave < 1U)
			geometry.deinterleave = 1U;
	}

	/* More channels than the type has are reported and counted as the most. */
	if (channels > geometry.max_channels)
		kern_logf("i915: display: memory: %u channels, more than the type's %u\n", channels, geometry.max_channels);
	if (geometry.max_channels != 0U && channels > geometry.max_channels)
		channels = geometry.max_channels;

	/* The highest DRAM clock of the points. */
	dclk_max = 0U;
	for (index = 0U; index < bw->qgv_points; index++) {
		if (points[index].dclk_mhz > dclk_max)
			dclk_max = points[index].dclk_mhz;
	}

	/* The peak and the cap, the request queue's depth, and the cache lines of a channel group. */
	peak = (int)(channels * i915_xd_bw_div_up(geometry.channel_width, 8U) * dclk_max);
	deprog = I915_XD_BW_DEPROG_GBPS * 1000;
	if (peak * 6 / 10 < deprog)
		deprog = peak * 6 / 10;
	queue = I915_XD_BW_DISPLAY_RTIDS / (int)channels;
	if (queue > I915_XD_BW_PCH_QUEUE)
		queue = I915_XD_BW_PCH_QUEUE;
	lines_per_group = 4 * (int)i915_xd_bw_div_up(8U, channels) * (int)geometry.deinterleave;

	/* Fills every group. */
	valid = 1;
	for (group = 0U; group < I915_XD_BW_GROUPS; group++) {
		lines = (I915_XD_BW_DEBURST * (int)geometry.deinterleave / (int)channels) << group;

		/* The next group's plane count: the queue's room left by a group of fewer lines than a channel group. */
		if (group + 1U < I915_XD_BW_GROUPS) {
			bw->group_planes[group + 1U] = 0U;
			if (lines < lines_per_group)
				bw->group_planes[group + 1U] = (unsigned)((queue - lines) / lines + 1);
		}

		/* The share and the peak of each QGV point. */
		for (index = 0U; index < bw->qgv_points; index++) {
			point = &points[index];
			cycle = (int)point->t_rp + (int)point->t_rcd + (lines - 1) * (int)geometry.burst + (int)point->t_rdpre;
			if ((int)point->t_rc > cycle)
				cycle = (int)point->t_rc;

			/* A row cycle that is not positive gives nothing. */
			if (cycle <= 0) {
				bw->derated_mbps[group][index] = 0U;
				bw->peak_mbps[group][index] = 0U;
				valid = 0;
				kern_logf("i915: display: memory: group %u QGV %u: row cycle %d, no bandwidth\n", group, index, cycle);
				continue;
			}

			/* The lines' bandwidth over the row cycle, derated and capped. */
			raw = (int)i915_xd_bw_div_up(point->dclk_mhz * (unsigned)lines * 32U * channels, (unsigned)cycle);
			derated = raw * (100 - I915_XD_BW_DERATING_PERCENT) / 100;
			if (derated > deprog)
				derated = deprog;
			bw->derated_mbps[group][index] = (uint32_t)derated;
			bw->peak_mbps[group][index] = (point->dclk_mhz * channels * geometry.channel_width + 4U) / 8U;
		}
	}

	/* Each PSF point's bandwidth, the same for every group. */
	for (index = 0U; index < bw->psf_points; index++)
		bw->psf_mbps[index] = (64U * psf_clocks[index] * 100U + 3U) / 6U;

	/* Logs the first group, which a single plane uses. */
	for (index = 0U; index < bw->qgv_points; index++) {
		kern_logf("i915: display: memory: QGV %u: %u MB/s for one plane (peak %u), group planes %u/%u/%u/%u/%u/%u\n",
		    index,
		    bw->derated_mbps[0][index],
		    bw->peak_mbps[0][index],
		    bw->group_planes[0],
		    bw->group_planes[1],
		    bw->group_planes[2],
		    bw->group_planes[3],
		    bw->group_planes[4],
		    bw->group_planes[5]);
	}

	/* Records whether the table holds. */
	bw->table_valid = valid;
}

/* Reports a mask of the low count bits. */
static unsigned
i915_xd_bw_low_bits(
	unsigned count)
{
	/* No bits, and every bit. */
	if (count == 0U)
		return 0U;
	if (count >= 32U)
		return 0xffffffffU;

	/* Succeeded: the low bits. */
	return (1U << count) - 1U;
}

/* Divides, rounding up. */
static unsigned
i915_xd_bw_div_up(
	unsigned value,
	unsigned divisor)
{
	/* Succeeded: the quotient rounded up. */
	return (value + divisor - 1U) / divisor;
}

/* Reports every advertised point in the mailbox's layout: the QGV points' bits and the PSF points' above them. */
static uint16_t
i915_xd_bw_advertised(
	const struct i915_xd_bandwidth *bw)
{
	unsigned qgv;
	unsigned psf;

	/* One bit per advertised point of each kind. */
	qgv = i915_xd_bw_low_bits(bw->qgv_points) & I915_XD_MAILBOX_SAGV_REQUEST_QGV;
	psf = (i915_xd_bw_low_bits(bw->psf_points) << 8) & I915_XD_MAILBOX_SAGV_REQUEST_PSF;

	/* Succeeded: both fields. */
	return (uint16_t)(qgv | psf);
}

/*
 * Asks the firmware to keep the memory off the masked points, until it
 * answers that both the QGV and the PSF points are safe: for 1 ms with
 * 10 us between the requests and then 50 ms more.  Returns 0, the last
 * request's mailbox error, ETIMEDOUT when the firmware never answered
 * safe, or EIO when the time base failed.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/intel_pcode.c
 * (skl_pcode_request, skl_pcode_try_request).
 */
static int
i915_xd_bw_restrict(
	struct i915_xd_io *io,
	uint16_t mask)
{
	uint32_t data;
	unsigned waited_us;
	int delayed;
	int error;

	/* Asks until the answer is safe or the time is spent. */
	waited_us = 0U;
	error = 0;
	for (;;) {
		/* One request; a busy mailbox, a refusal or an answer that is not safe asks again. */
		data = mask;
		error = drv_i915_xd_io_mailbox(io, I915_XD_MAILBOX_SAGV_CONFIG, &data, NULL, I915_XD_BW_MAILBOX_US);
		if (error == 0 && (data & (I915_XD_MAILBOX_SAGV_REPLY_QGV | I915_XD_MAILBOX_SAGV_REPLY_PSF)) == 0U)
			return 0;

		/* Stops once the whole time is spent. */
		if (waited_us >= I915_XD_BW_RESTRICT_US + I915_XD_BW_RESTRICT_MORE_US)
			break;

		/* Waits before the next request; a time base that fails stops at once. */
		delayed = drv_i915_xd_io_delay(io, I915_XD_BW_RESTRICT_GAP_US);
		if (delayed != 0)
			return EIO;
		waited_us += I915_XD_BW_RESTRICT_GAP_US;
	}

	/* Reports the mailbox's error. */
	if (error != 0)
		return error;

	/* The firmware answered, but never that the points were safe. */
	return ETIMEDOUT;
}
