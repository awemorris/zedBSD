/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the display 9 arithmetic of the xd code (display/xd-clock.c,
 * xd-dp.c, xd-ddi.c, xd-dbuf.c, xd9-outputs.c): the CD clock, the choice
 * of a mode from an EDID, the DP link and its M and N, the DPLL words of
 * DP rates and HDMI clocks, the DDI buffer tables and signal levels, the
 * clock recovery and equalisation tests, and the watermarks and DDB of
 * one, two and three pipes.
 *
 * The expected numbers come from a separate model of the Linux kernel
 * 6.8.12's computations (intel_link_compute_m_n, skl_ddi_calculate_wrpll,
 * skl_compute_plane_wm, skl_cursor_allocation), not from this code; the
 * WRPLL word for 148.5 MHz (0x80400173, 0x3a5) is the one Skylake parts
 * program for 1080p HDMI.
 */

#include "host-test.h"

#include "../../display/xd-clock.h"
#include "../../display/xd-dbuf.h"
#include "../../display/xd-ddi.h"
#include "../../display/xd-dp.h"
#include "../../display/xd-platform.h"
#include "../../display/xd9-outputs.h"
#include "../../intel/display-regs-xd9.h"

#include <uapi/errno.h>

#include <stdio.h>
#include <string.h>

static void i915_host_cdclk(void);
static void i915_host_pick_mode(void);
static void i915_host_dp_link(void);
static void i915_host_dpll(void);
static void i915_host_buf_trans(void);
static void i915_host_training(void);
static void i915_host_watermarks(void);
static int i915_host_level_is(const struct i915_xd_wm_level *level, uint32_t blocks, uint32_t lines);
static const struct i915_xd_platform *i915_host_row(unsigned kind, unsigned power_class);
static unsigned i915_host_class(uint16_t device);
static unsigned i915_host_level_row(uint8_t set);

/* The memory latencies the watermark checks use, in microseconds. */
static const uint16_t i915_host_latency[8] = { 2U, 4U, 10U, 18U, 28U, 40U, 60U, 90U };

/*
 * Checks the Gen9 mode set's arithmetic.
 */
int
main(void)
{
	int status;

	/* Each part. */
	i915_host_cdclk();
	i915_host_pick_mode();
	i915_host_dp_link();
	i915_host_dpll();
	i915_host_buf_trans();
	i915_host_training();
	i915_host_watermarks();

	/* The tally. */
	status = i915_host_report("gen9_mode_host_test");
	if (status != 0)
		return status;

	/* Succeeded: the arithmetic is the one the xd code describes. */
	return 0;
}

/* Checks the CD clock of both VCOs and each select, and a DPLL0 that is off or unlocked. */
static void
i915_host_cdclk(void)
{
	uint32_t locked;
	uint32_t vco_8640;
	uint32_t vco_8100;

	/* A running, locked DPLL0 at either VCO. */
	locked = (1U << 31) | (1U << 30);
	vco_8640 = I915_XD9_LINK_RATE_1080 << I915_XD9_DPLL_CTRL1_RATE_SHIFT;
	vco_8100 = I915_XD9_LINK_RATE_1350 << I915_XD9_DPLL_CTRL1_RATE_SHIFT;

	/* The 8640 MHz VCO. */
	i915_host_check(drv_i915_xd9_cdclk_khz(locked, vco_8640, 0U << 26) == 432000U, "cdclk 8640 select 0 is 432 MHz");
	i915_host_check(drv_i915_xd9_cdclk_khz(locked, vco_8640, 2U << 26) == 308571U, "cdclk 8640 select 2 is 308.571 MHz");
	i915_host_check(drv_i915_xd9_cdclk_khz(locked, vco_8640, 3U << 26) == 617143U, "cdclk 8640 select 3 is 617.143 MHz");

	/* The 8100 MHz VCO. */
	i915_host_check(drv_i915_xd9_cdclk_khz(locked, vco_8100, 1U << 26) == 540000U, "cdclk 8100 select 1 is 540 MHz");
	i915_host_check(drv_i915_xd9_cdclk_khz(locked, vco_8100, 3U << 26) == 675000U, "cdclk 8100 select 3 is 675 MHz");

	/* DPLL0 off or not locked. */
	i915_host_check(drv_i915_xd9_cdclk_khz(0U, vco_8100, 0U) == 0U, "cdclk is 0 with DPLL0 off");
	i915_host_check(drv_i915_xd9_cdclk_khz(1U << 31, vco_8100, 0U) == 0U, "cdclk is 0 with DPLL0 unlocked");
}

/* Checks the mode chosen from a base block's 1080p timing, under a low clock limit, and without an EDID. */
static void
i915_host_pick_mode(void)
{
	static const uint8_t dtd_1080p[18] = {
		0x02, 0x3a, 0x80, 0x18, 0x71, 0x38, 0x2d, 0x40, 0x58,
		0x2c, 0x45, 0x00, 0xc4, 0x8e, 0x21, 0x00, 0x00, 0x1e
	};
	struct i915_display_mode mode;
	uint8_t edid[256];
	int error;

	/* A base block whose first descriptor is 1080p at 148.5 MHz. */
	memset(edid, 0, sizeof(edid));
	memcpy(edid + 54, dtd_1080p, sizeof(dtd_1080p));

	/* The timing fits a 540 MHz CD clock. */
	error = drv_i915_xd9_pick_mode(edid, 128U, 540000U, &mode);
	i915_host_check(error == 0 && mode.clock_khz == 148500 && mode.hdisplay == 1920U && mode.vdisplay == 1080U, "mode 1080p from the base block");
	i915_host_check(mode.hsync_start == 2008U && mode.hsync_end == 2052U && mode.htotal == 2200U, "mode 1080p horizontal timing");
	i915_host_check(mode.vsync_start == 1084U && mode.vsync_end == 1089U && mode.vtotal == 1125U, "mode 1080p vertical timing");
	i915_host_check(mode.hsync_positive && mode.vsync_positive, "mode 1080p positive syncs");

	/* Under 100 MHz the safe mode is taken. */
	error = drv_i915_xd9_pick_mode(edid, 128U, 100000U, &mode);
	i915_host_check(error == 0 && mode.clock_khz == 65000 && mode.hdisplay == 1024U && mode.vtotal == 806U, "mode safe 1024x768 above the limit");

	/* No EDID, and a limit below even the safe mode. */
	error = drv_i915_xd9_pick_mode(NULL, 0U, 540000U, &mode);
	i915_host_check(error == 0 && mode.hdisplay == 1024U, "mode safe without an EDID");
	error = drv_i915_xd9_pick_mode(NULL, 0U, 50000U, &mode);
	i915_host_check(error == EINVAL, "mode EINVAL below the safe mode");
}

/* Checks the link chosen for 1080p and 4K60, the M and N, and the DPCD codes. */
static void
i915_host_dp_link(void)
{
	struct i915_xd_dp_choice link;
	struct i915_xd_dp_m_n m_n;
	int error;

	/* 1080p at 148.5 MHz: RBR on four lanes, data M/N 0x580000/0x800000, link 0x75555/0x80000. */
	error = drv_i915_xd9_dp_link(540000U, 4U, 148500U, &link, &m_n);
	i915_host_check(error == 0 && link.rate_khz == 162000U && link.lanes == 4U, "link 1080p on RBR x4");
	i915_host_check(m_n.data_m == 0x580000U && m_n.data_n == 0x800000U && m_n.tu == 64U, "link 1080p data M/N");
	i915_host_check(m_n.link_m == 0x75555U && m_n.link_n == 0x80000U, "link 1080p link M/N");

	/* 4K60 at 594 MHz: HBR2 on four lanes, data 0x699999/0x800000, link 0x8cccc/0x80000. */
	error = drv_i915_xd9_dp_link(540000U, 4U, 594000U, &link, &m_n);
	i915_host_check(error == 0 && link.rate_khz == 540000U && link.lanes == 4U, "link 4K60 on HBR2 x4");
	i915_host_check(m_n.data_m == 0x699999U && m_n.data_n == 0x800000U, "link 4K60 data M/N");
	i915_host_check(m_n.link_m == 0x8ccccU && m_n.link_n == 0x80000U, "link 4K60 link M/N");

	/* A sink of HBR on four lanes cannot carry 4K60; one of two lanes takes 1080p on HBR. */
	error = drv_i915_xd9_dp_link(270000U, 4U, 594000U, &link, &m_n);
	i915_host_check(error == ENOSPC, "link 4K60 ENOSPC on HBR");
	error = drv_i915_xd9_dp_link(540000U, 2U, 148500U, &link, &m_n);
	i915_host_check(error == 0 && link.rate_khz == 270000U && link.lanes == 2U, "link 1080p on HBR x2 for a two-lane sink");

	/* The DPCD codes both ways. */
	i915_host_check(drv_i915_xd9_dp_sink_rate(0x06U) == 162000U && drv_i915_xd9_dp_sink_rate(0x14U) == 540000U, "dpcd rate codes");
	i915_host_check(drv_i915_xd9_dp_sink_rate(0x1eU) == 540000U && drv_i915_xd9_dp_sink_rate(0x00U) == 0U, "dpcd HBR3 capped, 0 unknown");
	i915_host_check(drv_i915_xd9_dp_rate_code(270000U) == 0x0aU && drv_i915_xd9_dp_rate_code(300000U) == 0U, "dpcd LINK_BW_SET codes");
}

/* Checks the DPLL words of the DP rates and of five HDMI clocks. */
static void
i915_host_dpll(void)
{
	struct i915_xd9_dpll_words words;
	int error;

	/* DP: the override and the code of half the port clock. */
	error = drv_i915_xd9_dpll_dp_words(162000U, &words);
	i915_host_check(error == 0 && words.ctrl1 == 0x5U && words.cfgcr1 == 0U, "dpll DP RBR");
	error = drv_i915_xd9_dpll_dp_words(270000U, &words);
	i915_host_check(error == 0 && words.ctrl1 == 0x3U, "dpll DP HBR");
	error = drv_i915_xd9_dpll_dp_words(540000U, &words);
	i915_host_check(error == 0 && words.ctrl1 == 0x1U, "dpll DP HBR2");
	error = drv_i915_xd9_dpll_dp_words(300000U, &words);
	i915_host_check(error == EINVAL, "dpll DP EINVAL for 3 GHz");

	/* HDMI: the WRPLL words. */
	error = drv_i915_xd9_dpll_hdmi_words(25175U, &words);
	i915_host_check(error == 0 && words.ctrl1 == 0x21U && words.cfgcr1 == 0x809aab8eU && words.cfgcr2 == 0x13a4U, "dpll HDMI 25.175 MHz");
	error = drv_i915_xd9_dpll_hdmi_words(65000U, &words);
	i915_host_check(error == 0 && words.cfgcr1 == 0x802aab7bU && words.cfgcr2 == 0x7a4U, "dpll HDMI 65 MHz");
	error = drv_i915_xd9_dpll_hdmi_words(74250U, &words);
	i915_host_check(error == 0 && words.cfgcr1 == 0x80400173U && words.cfgcr2 == 0x6a5U, "dpll HDMI 74.25 MHz");
	error = drv_i915_xd9_dpll_hdmi_words(148500U, &words);
	i915_host_check(error == 0 && words.cfgcr1 == 0x80400173U && words.cfgcr2 == 0x3a5U, "dpll HDMI 148.5 MHz");
	error = drv_i915_xd9_dpll_hdmi_words(297000U, &words);
	i915_host_check(error == 0 && words.cfgcr1 == 0x80400173U && words.cfgcr2 == 0x145U, "dpll HDMI 297 MHz");
}

/* Checks the power class, the table choice and counts, and the signal levels. */
static void
i915_host_buf_trans(void)
{
	const struct i915_xd9_buf_trans *table;
	unsigned count;

	/* The power classes. */
	i915_host_check(i915_host_class(0x5916U) == I915_XD_POWER_CLASS_U, "class KBL GT2 U is U");
	i915_host_check(i915_host_class(0x591eU) == I915_XD_POWER_CLASS_Y, "class KBL GT2 Y is Y");
	i915_host_check(i915_host_class(0x3e92U) == I915_XD_POWER_CLASS_H, "class CFL GT2 S is H");

	/* Skylake H DP on port B: nine entries, the first 0x2016/0xa0. */
	table = drv_i915_xd9_ddi_buf_table(i915_host_row(I915_XD_KIND_SKL, I915_XD_POWER_CLASS_H), I915_XD9_OUTPUT_DP, 1U, &count);
	i915_host_check(count == 9U && table[0].trans1 == 0x00002016U && table[0].trans2 == 0x000000a0U, "trans SKL DP port B");
	i915_host_check(table[3].iboost == 1U && table[8].trans1 == 0x80005012U, "trans SKL DP boost entries");

	/* HDMI: eleven entries, the default entry 8 boosted. */
	table = drv_i915_xd9_ddi_buf_table(i915_host_row(I915_XD_KIND_KBL, I915_XD_POWER_CLASS_U), I915_XD9_OUTPUT_HDMI, 2U, &count);
	i915_host_check(count == 11U && drv_i915_xd9_ddi_hdmi_row(i915_host_row(I915_XD_KIND_KBL, I915_XD_POWER_CLASS_H)) == 8U, "trans HDMI eleven entries, level 8");
	i915_host_check(table[8].trans1 == 0x80003015U && table[8].trans2 == 0x000000cdU && table[8].iboost == 1U, "trans HDMI entry 8");

	/* eDP on port A keeps ten entries; elsewhere nine. */
	(void)drv_i915_xd9_ddi_buf_table(i915_host_row(I915_XD_KIND_SKL, I915_XD_POWER_CLASS_H), I915_XD9_OUTPUT_EDP, 0U, &count);
	i915_host_check(count == 10U, "trans eDP port A ten entries");
	(void)drv_i915_xd9_ddi_buf_table(i915_host_row(I915_XD_KIND_SKL, I915_XD_POWER_CLASS_H), I915_XD9_OUTPUT_EDP, 3U, &count);
	i915_host_check(count == 9U, "trans eDP port D nine entries");

	/* The signal levels and the swing ceilings. */
	i915_host_check(i915_host_level_row(0x00U) == 0U && i915_host_level_row(0x09U) == 5U && i915_host_level_row(0x03U) == 9U, "signal levels");
	i915_host_check(i915_host_level_row(0x0aU | 0x04U | 0x20U) == 8U, "signal level ignores the reached flags");
	i915_host_check(drv_i915_xd_dp_swing_max(9U) == 2U && drv_i915_xd_dp_swing_max(10U) == 3U, "voltage max of nine and ten entries");
}

/* Checks the clock recovery and equalisation tests. */
static void
i915_host_training(void)
{
	uint8_t status[6];

	/* Clock recovery on two lanes; equalisation needs the alignment too. */
	memset(status, 0, sizeof(status));
	status[0] = 0x11U;
	i915_host_check(drv_i915_xd_dp_clock_recovered(status, 2U) && !drv_i915_xd_dp_clock_recovered(status, 4U), "CR done on two of four lanes");
	status[0] = 0x77U;
	status[1] = 0x77U;
	i915_host_check(!drv_i915_xd_dp_equalized(status, 4U), "EQ waits for the alignment");
	status[2] = 0x01U;
	i915_host_check(drv_i915_xd_dp_equalized(status, 4U), "EQ done with the alignment");
	status[1] = 0x37U;
	i915_host_check(!drv_i915_xd_dp_equalized(status, 4U), "EQ needs symbol lock on every lane");
}

/* Checks the DDB split and levels of 1080p alone, 1080p with 720p, and three 4K pipes. */
static void
i915_host_watermarks(void)
{
	struct i915_xd9_pipe_plan pipes[3];
	const struct i915_xd_platform *row;
	unsigned pipe;
	int error;

	/* The rows every display 9 part shares for its display buffer. */
	row = i915_host_row(I915_XD_KIND_KBL, I915_XD_POWER_CLASS_H);

	/* One 1080p pipe: the whole DBUF, 32 cursor blocks, every level. */
	memset(pipes, 0, sizeof(pipes));
	pipes[0].active = 1;
	pipes[0].hdisplay = 1920U;
	pipes[0].htotal = 2200U;
	pipes[0].pixel_khz = 148500U;
	pipes[0].plane_width = 1920U;
	error = drv_i915_xd9_dbuf_plan(row, i915_host_latency, pipes, 3U);
	i915_host_check(error == 0 && pipes[0].pipe_ddb.start == 0U && pipes[0].pipe_ddb.end == 892U, "wm one pipe takes the DBUF");
	i915_host_check(pipes[0].plane_ddb.start == 0U && pipes[0].plane_ddb.end == 860U && pipes[0].cursor_ddb.start == 860U && pipes[0].cursor_ddb.end == 892U, "wm one pipe plane and cursor");
	i915_host_check(i915_host_level_is(&pipes[0].wm.level[0], 4U, 0U) && i915_host_level_is(&pipes[0].wm.level[1], 17U, 1U), "wm one pipe levels 0 and 1");
	i915_host_check(i915_host_level_is(&pipes[0].wm.level[3], 23U, 2U) && i915_host_level_is(&pipes[0].wm.level[7], 107U, 7U), "wm one pipe levels 3 and 7");
	i915_host_check(pipes[0].linetime == 119U, "wm one pipe line time");

	/* 1080p and 720p: split by width, eight-block cursors at least. */
	pipes[1].active = 1;
	pipes[1].hdisplay = 1280U;
	pipes[1].htotal = 1650U;
	pipes[1].pixel_khz = 74250U;
	pipes[1].plane_width = 1280U;
	error = drv_i915_xd9_dbuf_plan(row, i915_host_latency, pipes, 3U);
	i915_host_check(error == 0 && pipes[0].pipe_ddb.end == 535U && pipes[0].cursor_ddb.start == 511U && pipes[0].plane_ddb.end == 511U, "wm two pipes first share");
	i915_host_check(pipes[1].pipe_ddb.start == 535U && pipes[1].pipe_ddb.end == 892U && pipes[1].cursor_ddb.start == 874U && pipes[1].plane_ddb.end == 874U, "wm two pipes second share");
	i915_host_check(i915_host_level_is(&pipes[1].wm.level[0], 3U, 0U) && i915_host_level_is(&pipes[1].wm.level[7], 55U, 5U) && pipes[1].linetime == 178U, "wm two pipes second levels");

	/* Three 4K pipes: levels 6 and 7 do not fit and are off. */
	memset(pipes, 0, sizeof(pipes));
	for (pipe = 0U; pipe < 3U; pipe++) {
		pipes[pipe].active = 1;
		pipes[pipe].hdisplay = 3840U;
		pipes[pipe].htotal = 4400U;
		pipes[pipe].pixel_khz = 594000U;
		pipes[pipe].plane_width = 3840U;
	}

	/* Their split. */
	error = drv_i915_xd9_dbuf_plan(row, i915_host_latency, pipes, 3U);
	i915_host_check(error == 0 && pipes[1].pipe_ddb.start == 297U && pipes[1].pipe_ddb.end == 594U && pipes[1].cursor_ddb.start == 552U, "wm three pipes split");
	i915_host_check(i915_host_level_is(&pipes[2].wm.level[5], 188U, 6U) && !pipes[2].wm.level[6].enable && !pipes[2].wm.level[7].enable, "wm three pipes trim levels 6 and 7");
	i915_host_check(pipes[0].linetime == 60U, "wm three pipes line time");
}

/* Reports nonzero when a level is enabled with these blocks and lines. */
static int
i915_host_level_is(
	const struct i915_xd_wm_level *level,
	uint32_t blocks,
	uint32_t lines)
{
	/* Each field. */
	if (!level->enable)
		return 0;
	if (level->blocks != blocks || level->lines != lines)
		return 0;

	/* Succeeded: the level matches. */
	return 1;
}

/* Reports the display 9 row of a kind and a power class. */
static const struct i915_xd_platform *
i915_host_row(
	unsigned kind,
	unsigned power_class)
{
	/* Succeeded: the row. */
	return drv_i915_xd_platform_display9_class(kind, power_class);
}

/* Reports the power class of a display 9 device id (H for an id without a row). */
static unsigned
i915_host_class(
	uint16_t device)
{
	struct i915_xd_choice choice;
	int error;

	/* Succeeded: the row's class. */
	error = drv_i915_xd_platform_choose_display9(device, 0U, 0, 0U, &choice);
	if (error != 0)
		return I915_XD_POWER_CLASS_H;
	return choice.platform->power_class;
}

/* Reports the translation row a training set names (0 for none). */
static unsigned
i915_host_level_row(
	uint8_t set)
{
	unsigned row;

	/* Succeeded: the row. */
	if (drv_i915_xd_combo_level_row(set, &row) != 0)
		return 0U;
	return row;
}
