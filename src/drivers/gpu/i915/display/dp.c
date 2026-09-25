/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Derived from the Linux kernel v6.8.12 (drivers/gpu/drm/i915/display/intel_dp_link_training.c),
 * which carries the following notice.
 *
 * Copyright © 2008-2015 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

/*
 * Derived from the Linux kernel v6.8.12 (drivers/gpu/drm/i915/display/intel_dp.c),
 * which carries the following notice.
 *
 * Copyright © 2008 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 *    Keith Packard <keithp@keithp.com>
 */

/*
 * Derived from the Linux kernel v6.8.12 (drivers/gpu/drm/display/drm_dp_helper.c),
 * which carries the following notice.
 *
 * Copyright © 2009 Keith Packard
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation, and
 * that the name of the copyright holders not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no representations
 * about the suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */

/*
 * The DP link of the one-screen modeset path (see dp.h).
 *
 * The functions follow the Linux 6.8.12 text of drm_dp_helper.c (the DPCD
 * link-status helpers, the link-training delays, the LTTPR capabilities and
 * the channel-coding efficiency), intel_dp_link_training.c (the 8b/10b link
 * training) and intel_dp.c (the link rate and bandwidth arithmetic, the sink
 * power state, the source OUI, the eDP backlight enable and the DP infoframe
 * enables).
 *
 * The DPCD accessors, the sleeps and waits, the panel power sequencer, the
 * modeset-retry work and the UHBR training step of this environment reach
 * the current device of the world the modeset object belongs to, as the
 * Linux text reached it through a file-scope pointer (i915_dp_cur_i915()).
 * Register accesses keep the local device of the Linux text.
 *
 * The environment's note, error and warning sinks and I915_LCD_DISPLAY_VER()
 * do not evaluate the Linux device argument.  Where the Linux text computed
 * a device only for them, no device is computed and NULL stands in its
 * place.  The warning macros stringify their condition into the logged
 * text, so each condition keeps the Linux spelling, variable names included.
 *
 * lt_dbg() and lt_err() hand only their format string to the debug and
 * error hooks; their arguments are checked against the format at compile
 * time and never evaluated.
 */

#include "internal.h"
#include "modeset-internal.h"
#include "dp.h"
#include "panel-backlight.h"
#include <kern/kcrt.h>

#include <uapi/errno.h>

static struct drm_i915_private *i915_dp_cur_i915(const struct drm_dp_aux *aux);
static u8 i915_dp_lttpr_common_cap(const u8 caps[DP_LTTPR_COMMON_CAP_SIZE], int r);
static u8 i915_dp_lttpr_phy_cap(const u8 phy_cap[DP_LTTPR_PHY_CAP_SIZE], int r);
static int i915_drm_dp_read_lttpr_regs(struct drm_dp_aux *aux, const u8 dpcd[DP_RECEIVER_CAP_SIZE], int address, u8 *buf, int buf_size);
static u8 i915_dp_link_status(const u8 link_status[DP_LINK_STATUS_SIZE], int r);
static u8 i915_dp_get_lane_status(const u8 link_status[DP_LINK_STATUS_SIZE], int lane);
static u8 i915_drm_dp_get_adjust_request_voltage(const u8 link_status[DP_LINK_STATUS_SIZE], int lane);
static u8 i915_drm_dp_get_adjust_request_pre_emphasis(const u8 link_status[DP_LINK_STATUS_SIZE], int lane);
static u8 i915_drm_dp_get_adjust_tx_ffe_preset(const u8 link_status[DP_LINK_STATUS_SIZE], int lane);
static int i915_8b10b_clock_recovery_delay_us(const struct drm_dp_aux *aux, u8 rd_interval);
static int i915_8b10b_channel_eq_delay_us(const struct drm_dp_aux *aux, u8 rd_interval);
static int i915_128b132b_channel_eq_delay_us(const struct drm_dp_aux *aux, u8 rd_interval);
static int i915_read_delay(struct drm_dp_aux *aux, const u8 dpcd[DP_RECEIVER_CAP_SIZE], enum dp_phy dp_phy, bool uhbr, bool cr);
static int i915_drm_dp_read_clock_recovery_delay(struct drm_dp_aux *aux, const u8 dpcd[DP_RECEIVER_CAP_SIZE], enum dp_phy dp_phy, bool uhbr);
static int i915_drm_dp_read_channel_eq_delay(struct drm_dp_aux *aux, const u8 dpcd[DP_RECEIVER_CAP_SIZE], enum dp_phy dp_phy, bool uhbr);
static int i915_drm_dp_lttpr_count(const u8 caps[DP_LTTPR_COMMON_CAP_SIZE]);
static bool i915_drm_dp_lttpr_voltage_swing_level_3_supported(const u8 caps[DP_LTTPR_PHY_CAP_SIZE]);
static bool i915_drm_dp_lttpr_pre_emphasis_level_3_supported(const u8 caps[DP_LTTPR_PHY_CAP_SIZE]);
static int i915_drm_dp_read_lttpr_common_caps(struct drm_dp_aux *aux, const u8 dpcd[DP_RECEIVER_CAP_SIZE], u8 caps[DP_LTTPR_COMMON_CAP_SIZE]);
static int i915_drm_dp_read_lttpr_phy_caps(struct drm_dp_aux *aux, const u8 dpcd[DP_RECEIVER_CAP_SIZE], enum dp_phy dp_phy, u8 caps[DP_LTTPR_PHY_CAP_SIZE]);
static const char *i915_drm_dp_phy_name(enum dp_phy dp_phy) __maybe_unused;
static u8 i915_drm_dp_link_rate_to_bw_code(int link_rate);
static void i915_dp_reset_lttpr_common_caps(struct intel_dp *intel_dp);
static void i915_dp_reset_lttpr_count(struct intel_dp *intel_dp);
static u8 *i915_dp_lttpr_phy_caps(struct intel_dp *intel_dp, enum dp_phy dp_phy);
static void i915_dp_read_lttpr_phy_caps(struct intel_dp *intel_dp, const u8 dpcd[DP_RECEIVER_CAP_SIZE], enum dp_phy dp_phy);
static bool i915_dp_read_lttpr_common_caps(struct intel_dp *intel_dp, const u8 dpcd[DP_RECEIVER_CAP_SIZE]);
static bool i915_dp_set_lttpr_transparent_mode(struct intel_dp *intel_dp, bool enable);
static bool i915_dp_lttpr_transparent_mode_enabled(struct intel_dp *intel_dp);
static int i915_dp_init_lttpr_phys(struct intel_dp *intel_dp, const u8 dpcd[DP_RECEIVER_CAP_SIZE]);
static int i915_dp_init_lttpr(struct intel_dp *intel_dp, const u8 dpcd[DP_RECEIVER_CAP_SIZE]);
static int i915_dp_init_lttpr_and_dprx_caps(struct intel_dp *intel_dp, int *lttpr_count);
static u8 i915_dp_voltage_max(u8 preemph);
static u8 i915_dp_lttpr_voltage_max(struct intel_dp *intel_dp, enum dp_phy dp_phy);
static u8 i915_dp_lttpr_preemph_max(struct intel_dp *intel_dp, enum dp_phy dp_phy);
static bool i915_dp_phy_is_downstream_of_source(struct intel_dp *intel_dp, enum dp_phy dp_phy);
static u8 i915_dp_phy_voltage_max(struct intel_dp *intel_dp, const struct intel_crtc_state *crtc_state, enum dp_phy dp_phy);
static u8 i915_dp_phy_preemph_max(struct intel_dp *intel_dp, enum dp_phy dp_phy);
static bool i915_has_per_lane_signal_levels(struct intel_dp *intel_dp, enum dp_phy dp_phy);
static u8 i915_dp_get_lane_adjust_tx_ffe_preset(struct intel_dp *intel_dp, const struct intel_crtc_state *crtc_state, enum dp_phy dp_phy, const u8 link_status[DP_LINK_STATUS_SIZE], int lane);
static u8 i915_dp_get_lane_adjust_vswing_preemph(struct intel_dp *intel_dp, const struct intel_crtc_state *crtc_state, enum dp_phy dp_phy, const u8 link_status[DP_LINK_STATUS_SIZE], int lane);
static u8 i915_dp_get_lane_adjust_train(struct intel_dp *intel_dp, const struct intel_crtc_state *crtc_state, enum dp_phy dp_phy, const u8 link_status[DP_LINK_STATUS_SIZE], int lane);
static void i915_dp_get_adjust_train(struct intel_dp *intel_dp, const struct intel_crtc_state *crtc_state, enum dp_phy dp_phy, const u8 link_status[DP_LINK_STATUS_SIZE]);
static int i915_dp_training_pattern_set_reg(struct intel_dp *intel_dp, enum dp_phy dp_phy);
static bool i915_dp_set_link_train(struct intel_dp *intel_dp, const struct intel_crtc_state *crtc_state, enum dp_phy dp_phy, u8 dp_train_pat);
static char i915_dp_training_pattern_name(u8 train_pat);
static void i915_dp_program_link_training_pattern(struct intel_dp *intel_dp, const struct intel_crtc_state *crtc_state, enum dp_phy dp_phy, u8 dp_train_pat);
static void i915_dp_set_signal_levels(struct intel_dp *intel_dp, const struct intel_crtc_state *crtc_state, enum dp_phy dp_phy);
static bool i915_dp_reset_link_train(struct intel_dp *intel_dp, const struct intel_crtc_state *crtc_state, enum dp_phy dp_phy, u8 dp_train_pat);
static bool i915_dp_update_link_train(struct intel_dp *intel_dp, const struct intel_crtc_state *crtc_state, enum dp_phy dp_phy);
static bool i915_dp_lane_max_tx_ffe_reached(u8 train_set_lane);
static bool i915_dp_lane_max_vswing_reached(u8 train_set_lane);
static bool i915_dp_link_max_vswing_reached(struct intel_dp *intel_dp, const struct intel_crtc_state *crtc_state);
static void i915_dp_update_downspread_ctrl(struct intel_dp *intel_dp, const struct intel_crtc_state *crtc_state);
static void i915_dp_update_link_bw_set(struct intel_dp *intel_dp, const struct intel_crtc_state *crtc_state, u8 link_bw, u8 rate_select);
static bool i915_dp_prepare_link_train(struct intel_dp *intel_dp, const struct intel_crtc_state *crtc_state);
static bool i915_dp_adjust_request_changed(const struct intel_crtc_state *crtc_state, const u8 old_link_status[DP_LINK_STATUS_SIZE], const u8 new_link_status[DP_LINK_STATUS_SIZE]);
static void i915_dp_dump_link_status(struct intel_dp *intel_dp, enum dp_phy dp_phy, const u8 link_status[DP_LINK_STATUS_SIZE]);
static bool i915_dp_link_training_clock_recovery(struct intel_dp *intel_dp, const struct intel_crtc_state *crtc_state, enum dp_phy dp_phy);
static u32 i915_dp_training_pattern(struct intel_dp *intel_dp, const struct intel_crtc_state *crtc_state, enum dp_phy dp_phy);
static bool i915_dp_link_training_channel_equalization(struct intel_dp *intel_dp, const struct intel_crtc_state *crtc_state, enum dp_phy dp_phy);
static bool i915_dp_disable_dpcd_training_pattern(struct intel_dp *intel_dp, enum dp_phy dp_phy);
static bool i915_dp_link_train_phy(struct intel_dp *intel_dp, const struct intel_crtc_state *crtc_state, enum dp_phy dp_phy);
static void i915_dp_schedule_fallback_link_training(struct intel_dp *intel_dp, const struct intel_crtc_state *crtc_state);
static bool i915_dp_link_train_all_phys(struct intel_dp *intel_dp, const struct intel_crtc_state *crtc_state, int lttpr_count);
static int i915_dp_rate_index(const int *rates, int len, int rate);
static bool i915_dp_source_supports_tps3(struct drm_i915_private *i915);
static bool i915_dp_source_supports_tps4(struct drm_i915_private *i915);
static int i915_dp_rate_select(struct intel_dp *intel_dp, int rate);
static void i915_dp_compute_rate(struct intel_dp *intel_dp, int port_clock, u8 *link_bw, u8 *rate_select);
static bool i915_downstream_hpd_needs_d0(struct intel_dp *intel_dp);
static void i915_edp_init_source_oui(struct intel_dp *intel_dp, bool careful);
static bool i915_drm_dp_is_uhbr_rate(int link_rate);
static int i915_drm_dp_bw_channel_coding_efficiency(bool is_uhbr);

/*
 * Tells whether the channel equalization of every lane is done.
 *
 * The Linux drm_dp_channel_eq_ok(): the lanes must be aligned, and each
 * lane in use must report clock recovery, channel equalization and symbol
 * lock.
 */
bool
drv_i915_drm_dp_channel_eq_ok(
	const u8 link_status[DP_LINK_STATUS_SIZE],
	int lane_count)
{
	u8 lane_align;
	u8 lane_status;
	int lane;

	/* Reads the inter-lane alignment the sink reports. */
	lane_align = i915_dp_link_status(link_status, DP_LANE_ALIGN_STATUS_UPDATED);
	if ((lane_align & DP_INTERLANE_ALIGN_DONE) == 0)
		return false;

	/* Looks for a lane in use whose equalization is not complete. */
	for (lane = 0; lane < lane_count; lane++) {
		lane_status = i915_dp_get_lane_status(link_status, lane);
		if ((lane_status & DP_CHANNEL_EQ_BITS) != DP_CHANNEL_EQ_BITS)
			return false;
	}

	/* Succeeded: every lane in use is equalized and the lanes are aligned. */
	return true;
}

/*
 * Tells whether the clock recovery of every lane is done.
 *
 * The Linux drm_dp_clock_recovery_ok(): each lane in use must report
 * DP_LANE_CR_DONE.
 */
bool
drv_i915_drm_dp_clock_recovery_ok(
	const u8 link_status[DP_LINK_STATUS_SIZE],
	int lane_count)
{
	int lane;
	u8 lane_status;

	/* Looks for a lane in use whose clock is not recovered. */
	for (lane = 0; lane < lane_count; lane++) {
		lane_status = i915_dp_get_lane_status(link_status, lane);
		if ((lane_status & DP_LANE_CR_DONE) == 0)
			return false;
	}

	/* Succeeded: every lane in use has recovered the clock. */
	return true;
}

/*
 * Reads the link status of the DPRX or of one LTTPR PHY.
 *
 * The Linux drm_dp_dpcd_read_phy_link_status().  The layout of the status
 * handed back is the DPRX layout: an LTTPR's status, which has no sink
 * status byte, is converted to it.  The sink is the one the current device
 * of the modeset object that owns the AUX channel reaches.
 *
 * Returns 0 when the status was read, or the positive magnitude of the
 * negative Linux errno the DPCD read reported.
 */
int
drv_i915_drm_dp_dpcd_read_phy_link_status(
	struct drm_dp_aux *aux,
	enum dp_phy dp_phy,
	u8 link_status[DP_LINK_STATUS_SIZE])
{
	struct drm_i915_private *i915;
	int ret;

	/* Finds the device whose sink the AUX channel reaches. */
	i915 = i915_dp_cur_i915(aux);

	/* The DPRX status is read in its own layout. */
	if (dp_phy == DP_PHY_DPRX) {
		ret = I915_LCD_DRM_DP_DPCD_READ(i915,
						aux,
						DP_LANE0_1_STATUS,
						link_status,
						DP_LINK_STATUS_SIZE);
		if (ret < 0)
			return -ret;

		/* A short read is reported, but the status is used as read. */
		I915_LCD_WARN_ON(ret != DP_LINK_STATUS_SIZE);

		/* Succeeded: the DPRX status is in place. */
		return 0;
	}

	/* Reads the LTTPR status, which is one byte shorter. */
	ret = I915_LCD_DRM_DP_DPCD_READ(i915,
					aux,
					DP_LANE0_1_STATUS_PHY_REPEATER(dp_phy),
					link_status,
					DP_LINK_STATUS_SIZE - 1);
	if (ret < 0)
		return -ret;

	/* A short read is reported, but the status is used as read. */
	I915_LCD_WARN_ON(ret != DP_LINK_STATUS_SIZE - 1);

	/* Convert the LTTPR to the sink PHY link status layout. */
	kern_memmove(&link_status[DP_SINK_STATUS - DP_LANE0_1_STATUS + 1],
		&link_status[DP_SINK_STATUS - DP_LANE0_1_STATUS],
		DP_LINK_STATUS_SIZE - (DP_SINK_STATUS - DP_LANE0_1_STATUS) - 1);
	link_status[DP_SINK_STATUS - DP_LANE0_1_STATUS] = 0;

	/* Succeeded: the LTTPR status is in the DPRX layout. */
	return 0;
}

/*
 * Stops the link training of a DP port.
 *
 * The Linux intel_dp_stop_link_train(): disables the training pattern in
 * the sink's DPCD and the training pattern symbol generation on the port.
 * What the port sends afterwards is the idle pattern with the pipe still
 * disabled.  Must be called after drv_i915_dp_start_link_train().
 */
void
drv_i915_dp_stop_link_train(
	struct intel_dp *intel_dp,
	const struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *cur_i915;
	bool uhbr;
	int intra_hop_wait;

	/*
	 * link_trained tells the LTTPR detection that the link is active, so
	 * the next detection keeps the LTTPR mode instead of changing it.
	 */
	intel_dp->link_trained = true;

	/* Takes the sink and then the port out of the training pattern. */
	i915_dp_disable_dpcd_training_pattern(intel_dp, DP_PHY_DPRX);
	i915_dp_program_link_training_pattern(intel_dp,
					      crtc_state,
					      DP_PHY_DPRX,
					      DP_TRAINING_PATTERN_DISABLE);

	/* A 128b/132b link waits for the sink's intra-hop status to clear. */
	uhbr = drv_i915_dp_is_uhbr(crtc_state);
	if (uhbr) {
		cur_i915 = i915_dp_cur_i915(&intel_dp->aux);
		intra_hop_wait = I915_LCD_WAIT_FOR(cur_i915, intel_dp_128b132b_intra_hop(intel_dp, crtc_state) == 0, 500);
		if (intra_hop_wait != 0)
			lt_dbg(intel_dp, DP_PHY_DPRX, "128b/132b intra-hop not clearing\n");
	}
}

/*
 * Starts the link training of a DP port.
 *
 * The Linux intel_dp_start_link_train(): re-reads the LTTPR and DPRX
 * capabilities, prepares the link, trains every LTTPR and the DPRX, and
 * asks for a fallback retraining when the training fails.
 * drv_i915_dp_stop_link_train() must be called afterwards.
 */
void
drv_i915_dp_start_link_train(
	struct intel_dp *intel_dp,
	const struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *i915;
	struct drm_i915_private *cur_i915;
	bool passed;
	bool uhbr;
	int lttpr_count;
	int caps_error;

	/* Finds the device the port belongs to. */
	i915 = i915_lcd_dp_to_i915(intel_dp);

	/*
	 * Reinit the LTTPRs here to ensure that they are switched to
	 * non-transparent mode. During an earlier LTTPR detection this
	 * could've been prevented by an active link.
	 */
	caps_error = i915_dp_init_lttpr_and_dprx_caps(intel_dp, &lttpr_count);
	if (caps_error != 0) {
		/* Still continue with enabling the port and link training. */
		lttpr_count = 0;
	}

	/* Configures the link parameters in the sink. */
	i915_dp_prepare_link_train(intel_dp, crtc_state);

	/* Trains the link with the channel coding of its rate. */
	uhbr = drv_i915_dp_is_uhbr(crtc_state);
	if (uhbr) {
		cur_i915 = i915_dp_cur_i915(&intel_dp->aux);
		passed = I915_LCD_INTEL_DP_128B132B_LINK_TRAIN(cur_i915, intel_dp, crtc_state, lttpr_count);
	} else {
		passed = i915_dp_link_train_all_phys(intel_dp, crtc_state, lttpr_count);
	}

	/*
	 * A failed training asks for a fallback, unless long HPDs are ignored:
	 * in fixed environments like CI, a link failure that follows ignored
	 * long HPDs is ignored as well.
	 */
	if (!passed) {
		if (i915->display.hotplug.ignore_long_hpd) {
			lt_dbg(intel_dp, DP_PHY_DPRX, "Ignore the link failure\n");
			return;
		}

		i915_dp_schedule_fallback_link_training(intel_dp, crtc_state);
	}
}

/*
 * Tells whether the link rate of a crtc state is UHBR, and thus 128b/132b.
 *
 * The Linux intel_dp_is_uhbr().
 */
bool
drv_i915_dp_is_uhbr(
	const struct intel_crtc_state *crtc_state)
{
	bool uhbr;

	/* Classifies the port clock. */
	uhbr = i915_drm_dp_is_uhbr_rate(crtc_state->port_clock);
	if (!uhbr)
		return false;

	/* Succeeded: the link uses 128b/132b channel coding. */
	return true;
}

/*
 * Returns the link symbol size of a link rate in bits per symbol.
 *
 * The Linux intel_dp_link_symbol_size(): 32 for 128b/132b, 10 for 8b/10b.
 * The rate is in 10 kbit/s units.
 */
int
drv_i915_dp_link_symbol_size(
	int rate)
{
	bool uhbr;

	/* Classifies the rate by its channel coding. */
	uhbr = i915_drm_dp_is_uhbr_rate(rate);
	if (uhbr)
		return 32;

	/* Succeeded: an 8b/10b symbol is 10 bits. */
	return 10;
}

/*
 * Converts a link rate to the link symbol clock in kHz.
 *
 * The Linux intel_dp_link_symbol_clock().  The rate is in 10 kbit/s units.
 */
int
drv_i915_dp_link_symbol_clock(
	int rate)
{
	int symbol_size;

	/* Finds the symbol size of the rate's channel coding. */
	symbol_size = drv_i915_dp_link_symbol_size(rate);

	/* Succeeded: reports the symbol clock. */
	return DIV_ROUND_CLOSEST(rate * 10, symbol_size);
}

/*
 * Returns the data bandwidth a mode needs in kB/s.
 *
 * The Linux intel_dp_link_required(): the net bandwidth for a pixel clock
 * in kHz and a bpp, independent of the channel coding efficiency.
 *
 * TODO: check if callers of this functions should use
 * intel_dp_effective_data_rate() instead.
 */
int
drv_i915_dp_link_required(
	int pixel_clock,
	int bpp)
{
	/* pixel_clock is in kHz, divide bpp by 8 for bit to Byte conversion */
	return DIV_ROUND_UP(pixel_clock * bpp, 8);
}

/*
 * Returns the pixel data rate in kB/s including the BW allocation overhead.
 *
 * The Linux intel_dp_effective_data_rate(): the pixel clock is in kHz, bpp
 * in .4 fixed point, and the SSC, FEC and DSC overhead in 1 ppm units.
 */
int
drv_i915_dp_effective_data_rate(
	int pixel_clock,
	int bpp_x16,
	int bw_overhead)
{
	u64 overhead_rate;

	/* The pixel bit rate scaled by the overhead, in 1 ppm units. */
	overhead_rate = mul_u32_u32(pixel_clock * bpp_x16, bw_overhead);

	/* Succeeded: reports the rate in kB/s, rounded up. */
	return DIV_ROUND_UP_ULL(overhead_rate, 1000000 * 16 * 8);
}

/*
 * Returns the data bandwidth of a link rate and lane count in kB/s.
 *
 * The Linux intel_dp_max_data_rate().  Data bandwidth is the payload rate,
 * which depends on the channel coding efficiency and the link rate.
 *
 * For 8b/10b channel encoding, SST and non-FEC, the efficiency is 80%:
 * a 1.62 Gbps link carries 1.62*10^9 bps * 0.80 * (1/8) = 162000 kBps.
 * With 8-bit symbols the symbol clock is 162000 kHz, so by coincidence the
 * port clock in kHz matches the data bandwidth in kBps (this no longer
 * holds as soon as FEC or MST is taken into account).
 *
 * For 128b/132b channel encoding the efficiency is 96.71%: a 10 Gbps link
 * carries 10*10^9 bps * 0.9671 * (1/8) = 1208875 kBps, with a 312500 kHz
 * symbol clock of 32-bit symbols.  The rate 1000000 matches only the link
 * bit rate in units of 10000 bps.
 */
int
drv_i915_dp_max_data_rate(
	int max_link_rate,
	int max_lanes)
{
	bool uhbr;
	int ch_coding_efficiency;
	int max_link_rate_kbps;
	u64 coded_rate;

	/* Finds the channel coding efficiency of the rate. */
	uhbr = i915_drm_dp_is_uhbr_rate(max_link_rate);
	ch_coding_efficiency = i915_drm_dp_bw_channel_coding_efficiency(uhbr);
	max_link_rate_kbps = max_link_rate * 10;

	/*
	 * UHBR rates always use 128b/132b channel encoding, and have
	 * 97.71% data bandwidth efficiency. Consider max_link_rate the
	 * link bit rate in units of 10000 bps.
	 *
	 * Lower than UHBR rates always use 8b/10b channel encoding, and have
	 * 80% data bandwidth efficiency for SST non-FEC. However, this turns
	 * out to be a nop by coincidence:
	 *
	 *	int max_link_rate_kbps = max_link_rate * 10;
	 *	max_link_rate_kbps = DIV_ROUND_DOWN_ULL(max_link_rate_kbps * 8, 10);
	 *	max_link_rate = max_link_rate_kbps / 8;
	 */
	coded_rate = mul_u32_u32(max_link_rate_kbps * max_lanes, ch_coding_efficiency);

	/* Succeeded: reports the payload rate in kB/s, rounded down. */
	return DIV_ROUND_DOWN_ULL(coded_rate, 1000000 * 8);
}

/*
 * Tells whether a crtc state needs the DP VSC SDP.
 *
 * The Linux intel_dp_needs_vsc_sdp(): as per DP 1.4a spec section 2.2.4.3
 * [MSA Field for Indication of Color Encoding Format and Content Color
 * Gamut], YCbCr 4:2:0 and the BT.2020 / YCC colorimetries are sent through
 * the VSC SDP.
 */
bool
drv_i915_dp_needs_vsc_sdp(
	const struct intel_crtc_state *crtc_state,
	const struct drm_connector_state *conn_state)
{
	/* YCbCr 4:2:0 output is signalled in the VSC SDP. */
	if (crtc_state->output_format == INTEL_OUTPUT_FORMAT_YCBCR420)
		return true;

	/* So are the colorimetries the MSA cannot indicate. */
	switch (conn_state->colorspace) {
	case MODE_COLORIMETRY_SYCC_601:
	case MODE_COLORIMETRY_OPYCC_601:
	case MODE_COLORIMETRY_BT2020_YCC:
	case MODE_COLORIMETRY_BT2020_RGB:
	case MODE_COLORIMETRY_BT2020_CYCC:
		return true;
	default:
		break;
	}

	/* Succeeded: the MSA carries the colour format. */
	return false;
}

/*
 * Tells whether a DP port drives an eDP panel.
 *
 * The Linux intel_dp_is_edp(): not safe to use before the encoder type is
 * set.
 */
bool
drv_i915_dp_is_edp(
	struct intel_dp *intel_dp)
{
	struct intel_digital_port *dig_port;

	/* Finds the port whose encoder type decides. */
	dig_port = i915_lcd_dp_to_dig_port(intel_dp);
	if (dig_port->base.type != INTEL_OUTPUT_EDP)
		return false;

	/* Succeeded: the port drives an eDP panel. */
	return true;
}

/*
 * Records the link parameters of a DP port.
 *
 * The Linux intel_dp_set_link_params(): the training set starts from zero
 * and the link counts as not trained.
 */
void
drv_i915_dp_set_link_params(
	struct intel_dp *intel_dp,
	int link_rate,
	int lane_count)
{
	/* Starts the training set from the lowest levels. */
	kern_memset(intel_dp->train_set, 0, sizeof(intel_dp->train_set));

	/*
	 * link_trained tells the LTTPR detection that the link is not active
	 * yet, so it may switch the LTTPR mode.
	 */
	intel_dp->link_trained = false;

	/* Records the rate and lane count the training will use. */
	intel_dp->link_rate = link_rate;
	intel_dp->lane_count = lane_count;
}

/*
 * Sets the power state of a DP sink, if the sink supports it.
 *
 * The Linux intel_dp_set_power().  Waking a sink writes the source OUI of
 * an eDP panel first and retries for 1 ms to give the sink time to wake
 * up.  The sink and the sleeps are the current device's.
 */
void
drv_i915_dp_set_power(
	struct intel_dp *intel_dp,
	u8 mode)
{
	struct intel_encoder *encoder;
	struct drm_i915_private *cur_i915;
	struct intel_lspcon *lspcon;
	i915_lcd_ssize_t ret;
	bool needs_d0;
	bool edp;
	int i;

	/* Finds the port's encoder, which the failure note names. */
	encoder = &i915_lcd_dp_to_dig_port(intel_dp)->base;

	/* Should have a valid DPCD by this point */
	if (intel_dp->dpcd[DP_DPCD_REV] < 0x11)
		return;

	/* Finds the device whose sink the port reaches. */
	cur_i915 = i915_dp_cur_i915(&intel_dp->aux);

	/* Puts the sink to sleep, or wakes it up. */
	if (mode != DP_SET_POWER_D0) {
		/* A branch device that signals downstream HPD only in D0 stays awake. */
		needs_d0 = i915_downstream_hpd_needs_d0(intel_dp);
		if (needs_d0)
			return;

		/* Writes the new power state. */
		ret = I915_LCD_DRM_DP_DPCD_WRITEB(cur_i915, &intel_dp->aux, DP_SET_POWER, mode);
	} else {
		/* Resumes an LSPCON (none is behind these ports). */
		lspcon = dp_to_lspcon(intel_dp);
		lspcon_resume(i915_lcd_dp_to_dig_port(intel_dp));

		/* Write the source OUI as early as possible */
		edp = drv_i915_dp_is_edp(intel_dp);
		if (edp)
			i915_edp_init_source_oui(intel_dp, false);

		/*
		 * When turning on, we need to retry for 1ms to give the sink
		 * time to wake up.
		 */
		for (i = 0; i < 3; i++) {
			ret = I915_LCD_DRM_DP_DPCD_WRITEB(cur_i915, &intel_dp->aux, DP_SET_POWER, mode);
			if (ret == 1)
				break;

			i915_lcd_msleep(cur_i915, 1);
		}

		/* An awake LSPCON is waited for until it is in PCON mode. */
		if (ret == 1 && lspcon->active)
			lspcon_wait_pcon_mode(lspcon);
	}

	/* Notes a sink that did not take the power state. */
	if (ret != 1) {
		I915_LCD_DRM_DBG_KMS(NULL,
				     "[ENCODER:%d:%s] Set power to %s failed\n",
				     encoder->base.base.id,
				     encoder->base.name,
				     mode == DP_SET_POWER_D0 ? "D0" : "D3");
	}
}

/*
 * Enables the backlight PWM and the backlight PP control of an eDP panel.
 *
 * The Linux intel_edp_backlight_on().  The PP control is the current
 * device's panel power sequencer.
 */
void
drv_i915_edp_backlight_on(
	const struct intel_crtc_state *crtc_state,
	const struct drm_connector_state *conn_state)
{
	struct intel_dp *intel_dp;
	struct drm_i915_private *cur_i915;
	bool edp;

	/* Finds the DP port of the connector's encoder. */
	intel_dp = enc_to_intel_dp(to_intel_encoder(conn_state->best_encoder));

	/* Only an eDP panel has a backlight here. */
	edp = drv_i915_dp_is_edp(intel_dp);
	if (!edp)
		return;

	/* Notes the enable. */
	I915_LCD_DRM_DBG_KMS(NULL, "\n");

	/* Enables the PWM, then the backlight enable bit of the sequencer. */
	drv_i915_backlight_enable(crtc_state, conn_state);
	cur_i915 = i915_dp_cur_i915(&intel_dp->aux);
	i915_lcd_intel_pps_backlight_on(cur_i915, intel_dp);
}

/*
 * Disables the backlight PP control and the backlight PWM of an eDP panel.
 *
 * The Linux intel_edp_backlight_off().  The PP control is the current
 * device's panel power sequencer.
 */
void
drv_i915_edp_backlight_off(
	const struct drm_connector_state *old_conn_state)
{
	struct intel_dp *intel_dp;
	struct drm_i915_private *cur_i915;
	bool edp;

	/* Finds the DP port of the connector's encoder. */
	intel_dp = enc_to_intel_dp(to_intel_encoder(old_conn_state->best_encoder));

	/* Only an eDP panel has a backlight here. */
	edp = drv_i915_dp_is_edp(intel_dp);
	if (!edp)
		return;

	/* Notes the disable. */
	I915_LCD_DRM_DBG_KMS(NULL, "\n");

	/* Clears the backlight enable bit of the sequencer, then disables the PWM. */
	cur_i915 = i915_dp_cur_i915(&intel_dp->aux);
	i915_lcd_intel_pps_backlight_off(cur_i915, intel_dp);
	drv_i915_backlight_disable(old_conn_state);
}

/*
 * Enables or disables the DP infoframes (SDPs) of a transcoder.
 *
 * The Linux intel_dp_set_infoframes(): the DIP enables of the transcoder
 * are cleared, the VSC DIP is kept only for PSR, and on enable the VSC and
 * the gamut metadata SDPs are written.
 */
void
drv_i915_dp_set_infoframes(
	struct intel_encoder *encoder,
	bool enable,
	const struct intel_crtc_state *crtc_state,
	const struct drm_connector_state *conn_state)
{
	struct drm_i915_private *dev_priv;
	i915_reg_t reg;
	u32 dip_enable;
	u32 val;

	UNUSED_PARAMETER(conn_state);

	/* Finds the device and the transcoder's DIP control register. */
	dev_priv = i915_lcd_to_i915(encoder->base.dev);
	reg = HSW_TVIDEO_DIP_CTL(crtc_state->cpu_transcoder);

	/* The DIP enables this function owns. */
	dip_enable = VIDEO_DIP_ENABLE_AVI_HSW | VIDEO_DIP_ENABLE_GCP_HSW |
		     VIDEO_DIP_ENABLE_VS_HSW | VIDEO_DIP_ENABLE_GMP_HSW |
		     VIDEO_DIP_ENABLE_SPD_HSW | VIDEO_DIP_ENABLE_DRM_GLK;

	/* Reads the control word without those enables. */
	val = i915_lcd_intel_de_read(dev_priv, reg);
	val &= ~dip_enable;

	/* TODO: Sanitize DSC enabling wrt. intel_dsc_dp_pps_write(). */
	if (!enable && HAS_DSC(dev_priv))
		val &= ~VDIP_ENABLE_PPS;

	/*
	 * This routine disables VSC DIP if the function is called
	 * to disable SDP or if it does not have PSR
	 */
	if (!enable) {
		val &= ~VIDEO_DIP_ENABLE_VSC_HSW;
	} else if (!crtc_state->has_psr) {
		val &= ~VIDEO_DIP_ENABLE_VSC_HSW;
	}

	/* Writes the control word and flushes it. */
	i915_lcd_intel_de_write(dev_priv, reg, val);
	i915_lcd_intel_de_posting_read(dev_priv, reg);

	/* A disable is complete. */
	if (!enable)
		return;

	/* When PSR is enabled, VSC SDP is handled by PSR routine */
	if (!crtc_state->has_psr)
		intel_write_dp_sdp(encoder, crtc_state, DP_SDP_VSC);

	/* Writes the gamut metadata SDP. */
	intel_write_dp_sdp(encoder, crtc_state, HDMI_PACKET_TYPE_GAMUT_METADATA);
}

/*
 * The device the Linux text reached through the file-scope current-device
 * pointer: the current device of the world of the modeset object that owns
 * the AUX channel.
 */
static struct drm_i915_private *
i915_dp_cur_i915(
	const struct drm_dp_aux *aux)
{
	struct i915_lcd_modeset *ms;

	/* Finds the modeset object the port's AUX channel is part of. */
	ms = container_of(aux, struct i915_lcd_modeset, dig_port.dp.aux);

	/* Succeeded: reports the device its world's entry point recorded. */
	return ms->world->i915_lcd_cur_i915;
}

/* Reads one register of the LTTPR common capabilities (the Linux dp_lttpr_common_cap()). */
static u8
i915_dp_lttpr_common_cap(
	const u8 caps[DP_LTTPR_COMMON_CAP_SIZE],
	int r)
{
	/* The capabilities start at the structure revision register. */
	return caps[r - DP_LT_TUNABLE_PHY_REPEATER_FIELD_DATA_STRUCTURE_REV];
}

/* Reads one register of an LTTPR PHY's capabilities (the Linux dp_lttpr_phy_cap()). */
static u8
i915_dp_lttpr_phy_cap(
	const u8 phy_cap[DP_LTTPR_PHY_CAP_SIZE],
	int r)
{
	/* The capabilities start at the PHY repeater 1 AUX read interval register. */
	return phy_cap[r - DP_TRAINING_AUX_RD_INTERVAL_PHY_REPEATER1];
}

/*
 * Reads a run of LTTPR registers (the Linux drm_dp_read_lttpr_regs()): 0, or
 * the positive magnitude of the negative Linux errno of the DPCD read.
 */
static int
i915_drm_dp_read_lttpr_regs(
	struct drm_dp_aux *aux,
	const u8 dpcd[DP_RECEIVER_CAP_SIZE],
	int address,
	u8 *buf,
	int buf_size)
{
	struct drm_i915_private *i915;
	int block_size;
	int offset;
	int ret;

	/* Finds the device whose sink the AUX channel reaches. */
	i915 = i915_dp_cur_i915(aux);

	/*
	 * At least the DELL P2715Q monitor with a DPCD_REV < 0x14 returns
	 * corrupted values when reading from the 0xF0000- range with a block
	 * size bigger than 1.
	 */
	if (dpcd[DP_DPCD_REV] < 0x14) {
		block_size = 1;
	} else {
		block_size = buf_size;
	}

	/* Reads the registers block by block. */
	for (offset = 0; offset < buf_size; offset += block_size) {
		ret = I915_LCD_DRM_DP_DPCD_READ(i915,
						aux,
						address + offset,
						&buf[offset],
						block_size);
		if (ret < 0)
			return -ret;

		/* A short read is reported, but the block is used as read. */
		I915_LCD_WARN_ON(ret != block_size);
	}

	/* Succeeded: every block was read. */
	return 0;
}

/* Reads one register of a link status (the Linux dp_link_status()). */
static u8
i915_dp_link_status(
	const u8 link_status[DP_LINK_STATUS_SIZE],
	int r)
{
	/* The status starts at the lane 0/1 status register. */
	return link_status[r - DP_LANE0_1_STATUS];
}

/* Reads the 4-bit status of one lane (the Linux dp_get_lane_status()). */
static u8
i915_dp_get_lane_status(
	const u8 link_status[DP_LINK_STATUS_SIZE],
	int lane)
{
	int i;
	int s;
	u8 l;

	/* Two lanes share a register, the odd lane in the high nibble. */
	i = DP_LANE0_1_STATUS + (lane >> 1);
	s = (lane & 1) * 4;
	l = i915_dp_link_status(link_status, i);

	/* Succeeded: reports the lane's nibble. */
	return (l >> s) & 0xf;
}

/* Reads the voltage swing a lane's sink asks for (the Linux drm_dp_get_adjust_request_voltage()). */
static u8
i915_drm_dp_get_adjust_request_voltage(
	const u8 link_status[DP_LINK_STATUS_SIZE],
	int lane)
{
	int i;
	int s;
	u8 l;

	/* Two lanes share a request register; each lane has its own field. */
	i = DP_ADJUST_REQUEST_LANE0_1 + (lane >> 1);
	if (lane & 1) {
		s = DP_ADJUST_VOLTAGE_SWING_LANE1_SHIFT;
	} else {
		s = DP_ADJUST_VOLTAGE_SWING_LANE0_SHIFT;
	}

	/* Reads the register holding the lane. */
	l = i915_dp_link_status(link_status, i);

	/* Succeeded: reports the level in the training-set position. */
	return ((l >> s) & 0x3) << DP_TRAIN_VOLTAGE_SWING_SHIFT;
}

/* Reads the pre-emphasis a lane's sink asks for (the Linux drm_dp_get_adjust_request_pre_emphasis()). */
static u8
i915_drm_dp_get_adjust_request_pre_emphasis(
	const u8 link_status[DP_LINK_STATUS_SIZE],
	int lane)
{
	int i;
	int s;
	u8 l;

	/* Two lanes share a request register; each lane has its own field. */
	i = DP_ADJUST_REQUEST_LANE0_1 + (lane >> 1);
	if (lane & 1) {
		s = DP_ADJUST_PRE_EMPHASIS_LANE1_SHIFT;
	} else {
		s = DP_ADJUST_PRE_EMPHASIS_LANE0_SHIFT;
	}

	/* Reads the register holding the lane. */
	l = i915_dp_link_status(link_status, i);

	/* Succeeded: reports the level in the training-set position. */
	return ((l >> s) & 0x3) << DP_TRAIN_PRE_EMPHASIS_SHIFT;
}

/* Reads the 128b/132b TX FFE preset a lane's sink asks for (the Linux drm_dp_get_adjust_tx_ffe_preset()). */
static u8
i915_drm_dp_get_adjust_tx_ffe_preset(
	const u8 link_status[DP_LINK_STATUS_SIZE],
	int lane)
{
	int i;
	int s;
	u8 l;

	/* Two lanes share a request register; each lane has its own field. */
	i = DP_ADJUST_REQUEST_LANE0_1 + (lane >> 1);
	if (lane & 1) {
		s = DP_ADJUST_TX_FFE_PRESET_LANE1_SHIFT;
	} else {
		s = DP_ADJUST_TX_FFE_PRESET_LANE0_SHIFT;
	}

	/* Reads the register holding the lane. */
	l = i915_dp_link_status(link_status, i);

	/* Succeeded: reports the preset. */
	return (l >> s) & 0xf;
}

/* The 8b/10b clock recovery delay of an AUX read interval in us (the Linux __8b10b_clock_recovery_delay_us()). */
static int
i915_8b10b_clock_recovery_delay_us(
	const struct drm_dp_aux *aux,
	u8 rd_interval)
{
	/* Notes an interval beyond the defined ones; it is used anyway. */
	if (rd_interval > 4) {
		I915_LCD_DRM_DBG_KMS(aux->drm_dev,
				     "%s: invalid AUX interval 0x%02x (max 4)\n",
				     aux->name,
				     rd_interval);
	}

	/* No interval is the 100 us default. */
	if (rd_interval == 0)
		return 100;

	/* Succeeded: the interval is in units of 4 ms. */
	return rd_interval * 4 * USEC_PER_MSEC;
}

/* The 8b/10b channel equalization delay of an AUX read interval in us (the Linux __8b10b_channel_eq_delay_us()). */
static int
i915_8b10b_channel_eq_delay_us(
	const struct drm_dp_aux *aux,
	u8 rd_interval)
{
	/* Notes an interval beyond the defined ones; it is used anyway. */
	if (rd_interval > 4) {
		I915_LCD_DRM_DBG_KMS(aux->drm_dev,
				     "%s: invalid AUX interval 0x%02x (max 4)\n",
				     aux->name,
				     rd_interval);
	}

	/* No interval is the 400 us default. */
	if (rd_interval == 0)
		return 400;

	/* Succeeded: the interval is in units of 4 ms. */
	return rd_interval * 4 * USEC_PER_MSEC;
}

/* The 128b/132b channel equalization delay of an AUX read interval in us (the Linux __128b132b_channel_eq_delay_us()). */
static int
i915_128b132b_channel_eq_delay_us(
	const struct drm_dp_aux *aux,
	u8 rd_interval)
{
	/* Maps the interval code to its delay; an unknown code is noted and taken as 400 us. */
	switch (rd_interval) {
	default:
		I915_LCD_DRM_DBG_KMS(aux->drm_dev,
				     "%s: invalid AUX interval 0x%02x\n",
				     aux->name,
				     rd_interval);
		fallthrough;
	case DP_128B132B_TRAINING_AUX_RD_INTERVAL_400_US:
		return 400;
	case DP_128B132B_TRAINING_AUX_RD_INTERVAL_4_MS:
		return 4000;
	case DP_128B132B_TRAINING_AUX_RD_INTERVAL_8_MS:
		return 8000;
	case DP_128B132B_TRAINING_AUX_RD_INTERVAL_12_MS:
		return 12000;
	case DP_128B132B_TRAINING_AUX_RD_INTERVAL_16_MS:
		return 16000;
	case DP_128B132B_TRAINING_AUX_RD_INTERVAL_32_MS:
		return 32000;
	case DP_128B132B_TRAINING_AUX_RD_INTERVAL_64_MS:
		return 64000;
	}
}

/*
 * Returns a link training delay in us, reading DPCD if necessary (the Linux
 * __read_delay()).  The delays differ for clock recovery vs. channel
 * equalization, DPRX vs. LTTPR, 128b/132b vs. 8b/10b, and DPCD rev 1.3 vs.
 * later.
 */
static int
i915_read_delay(
	struct drm_dp_aux *aux,
	const u8 dpcd[DP_RECEIVER_CAP_SIZE],
	enum dp_phy dp_phy,
	bool uhbr,
	bool cr)
{
	int (*parse)(const struct drm_dp_aux *aux, u8 rd_interval);
	struct drm_i915_private *i915;
	unsigned int offset;
	u8 rd_interval;
	u8 mask;
	i915_lcd_ssize_t transferred;
	int delay_us;

	/* Picks the interval register and its decoder, or a fixed delay. */
	if (dp_phy == DP_PHY_DPRX) {
		if (uhbr) {
			/* The 128b/132b DPRX clock recovery delay is fixed. */
			if (cr)
				return 100;

			offset = DP_128B132B_TRAINING_AUX_RD_INTERVAL;
			mask = DP_128B132B_TRAINING_AUX_RD_INTERVAL_MASK;
			parse = i915_128b132b_channel_eq_delay_us;
		} else {
			/* A DPCD 1.4 DPRX has the fixed clock recovery delay. */
			if (cr && dpcd[DP_DPCD_REV] >= DP_DPCD_REV_14)
				return 100;

			offset = DP_TRAINING_AUX_RD_INTERVAL;
			mask = DP_TRAINING_AUX_RD_MASK;
			if (cr) {
				parse = i915_8b10b_clock_recovery_delay_us;
			} else {
				parse = i915_8b10b_channel_eq_delay_us;
			}
		}
	} else {
		if (uhbr) {
			offset = DP_128B132B_TRAINING_AUX_RD_INTERVAL_PHY_REPEATER(dp_phy);
			mask = DP_128B132B_TRAINING_AUX_RD_INTERVAL_MASK;
			parse = i915_128b132b_channel_eq_delay_us;
		} else {
			/* The 8b/10b LTTPR clock recovery delay is fixed. */
			if (cr)
				return 100;

			offset = DP_TRAINING_AUX_RD_INTERVAL_PHY_REPEATER(dp_phy);
			mask = DP_TRAINING_AUX_RD_MASK;
			parse = i915_8b10b_channel_eq_delay_us;
		}
	}

	/* Takes the interval from the DPRX caps, or reads it from the sink. */
	if (offset < DP_RECEIVER_CAP_SIZE) {
		rd_interval = dpcd[offset];
	} else {
		i915 = i915_dp_cur_i915(aux);
		transferred = I915_LCD_DRM_DP_DPCD_READB(i915, aux, offset, &rd_interval);
		if (transferred != 1) {
			I915_LCD_DRM_DBG_KMS(aux->drm_dev,
					     "%s: failed rd interval read\n",
					     aux->name);

			/* arbitrary default delay */
			return 400;
		}
	}

	/* Decodes the interval. */
	delay_us = parse(aux, rd_interval & mask);

	/* Succeeded: reports the delay. */
	return delay_us;
}

/* Returns the clock recovery delay of a PHY in us (the Linux drm_dp_read_clock_recovery_delay()). */
static int
i915_drm_dp_read_clock_recovery_delay(
	struct drm_dp_aux *aux,
	const u8 dpcd[DP_RECEIVER_CAP_SIZE],
	enum dp_phy dp_phy,
	bool uhbr)
{
	int delay_us;

	/* Reads the clock recovery delay. */
	delay_us = i915_read_delay(aux, dpcd, dp_phy, uhbr, true);

	/* Succeeded: reports the delay. */
	return delay_us;
}

/* Returns the channel equalization delay of a PHY in us (the Linux drm_dp_read_channel_eq_delay()). */
static int
i915_drm_dp_read_channel_eq_delay(
	struct drm_dp_aux *aux,
	const u8 dpcd[DP_RECEIVER_CAP_SIZE],
	enum dp_phy dp_phy,
	bool uhbr)
{
	int delay_us;

	/* Reads the channel equalization delay. */
	delay_us = i915_read_delay(aux, dpcd, dp_phy, uhbr, false);

	/* Succeeded: reports the delay. */
	return delay_us;
}

/*
 * Returns the number of detected LTTPRs (the Linux drm_dp_lttpr_count()).
 *
 * The Linux contract is kept, because every caller compares the count with
 * zero: the count, or -I915_LCD_ERANGE if more than the supported 8 LTTPRs
 * are detected, or -I915_LCD_EINVAL if DP_PHY_REPEATER_CNT holds an invalid
 * value (negative Linux numbers).
 */
static int
i915_drm_dp_lttpr_count(
	const u8 caps[DP_LTTPR_COMMON_CAP_SIZE])
{
	u8 count;
	unsigned int set_bits;

	/* Reads the one-hot repeater count. */
	count = i915_dp_lttpr_common_cap(caps, DP_PHY_REPEATER_CNT);
	set_bits = hweight8(count);

	/* Decodes the count by the number of bits set. */
	switch (set_bits) {
	case 0:
		return 0;
	case 1:
		return 8 - ilog2(count);
	case 8:
		return -I915_LCD_ERANGE;
	default:
		return -I915_LCD_EINVAL;
	}
}

/* Tells whether an LTTPR TX PHY supports voltage swing level 3 (the Linux drm_dp_lttpr_voltage_swing_level_3_supported()). */
static bool
i915_drm_dp_lttpr_voltage_swing_level_3_supported(
	const u8 caps[DP_LTTPR_PHY_CAP_SIZE])
{
	u8 txcap;

	/* Reads the transmitter capability register. */
	txcap = i915_dp_lttpr_phy_cap(caps, DP_TRANSMITTER_CAPABILITY_PHY_REPEATER1);
	if ((txcap & DP_VOLTAGE_SWING_LEVEL_3_SUPPORTED) == 0)
		return false;

	/* Succeeded: level 3 is supported. */
	return true;
}

/* Tells whether an LTTPR TX PHY supports pre-emphasis level 3 (the Linux drm_dp_lttpr_pre_emphasis_level_3_supported()). */
static bool
i915_drm_dp_lttpr_pre_emphasis_level_3_supported(
	const u8 caps[DP_LTTPR_PHY_CAP_SIZE])
{
	u8 txcap;

	/* Reads the transmitter capability register. */
	txcap = i915_dp_lttpr_phy_cap(caps, DP_TRANSMITTER_CAPABILITY_PHY_REPEATER1);
	if ((txcap & DP_PRE_EMPHASIS_LEVEL_3_SUPPORTED) == 0)
		return false;

	/* Succeeded: level 3 is supported. */
	return true;
}

/*
 * Reads the capabilities common to all LTTPRs (the Linux
 * drm_dp_read_lttpr_common_caps()): 0, or the positive magnitude of the
 * negative Linux errno of the DPCD read.
 */
static int
i915_drm_dp_read_lttpr_common_caps(
	struct drm_dp_aux *aux,
	const u8 dpcd[DP_RECEIVER_CAP_SIZE],
	u8 caps[DP_LTTPR_COMMON_CAP_SIZE])
{
	int error;

	/* Reads the common capability registers. */
	error = i915_drm_dp_read_lttpr_regs(aux,
					    dpcd,
					    DP_LT_TUNABLE_PHY_REPEATER_FIELD_DATA_STRUCTURE_REV,
					    caps,
					    DP_LTTPR_COMMON_CAP_SIZE);
	if (error != 0)
		return error;

	/* Succeeded: the capabilities are in caps. */
	return 0;
}

/*
 * Reads the capabilities of one LTTPR PHY (the Linux
 * drm_dp_read_lttpr_phy_caps()): 0, or the positive magnitude of the
 * negative Linux errno of the DPCD read.
 */
static int
i915_drm_dp_read_lttpr_phy_caps(
	struct drm_dp_aux *aux,
	const u8 dpcd[DP_RECEIVER_CAP_SIZE],
	enum dp_phy dp_phy,
	u8 caps[DP_LTTPR_PHY_CAP_SIZE])
{
	int error;

	/* Reads the PHY's capability registers. */
	error = i915_drm_dp_read_lttpr_regs(aux,
					    dpcd,
					    DP_TRAINING_AUX_RD_INTERVAL_PHY_REPEATER(dp_phy),
					    caps,
					    DP_LTTPR_PHY_CAP_SIZE);
	if (error != 0)
		return error;

	/* Succeeded: the capabilities are in caps. */
	return 0;
}

/*
 * Returns the name of a DP PHY (the Linux drm_dp_phy_name()): "DPRX",
 * "LTTPR <N>", or "<INVALID DP PHY>".
 *
 * XXX: nothing calls it.  Linux names the PHY in the prefix of every
 * lt_dbg() / lt_err() line; this environment's lt_dbg() and lt_err() drop
 * that prefix, so the function is kept for the Linux text only.
 */
static const char *
i915_drm_dp_phy_name(
	enum dp_phy dp_phy)
{
	/* The names, indexed by the PHY (DP_PHY_DPRX, then DP_PHY_LTTPR1..8). */
	static const char * const phy_names[] = {
		"DPRX",
		"LTTPR 1",
		"LTTPR 2",
		"LTTPR 3",
		"LTTPR 4",
		"LTTPR 5",
		"LTTPR 6",
		"LTTPR 7",
		"LTTPR 8",
	};
	int warned;

	/* A PHY outside the table has no name. */
	if (dp_phy < 0)
		return "<INVALID DP PHY>";
	if ((size_t)dp_phy >= ARRAY_SIZE(phy_names))
		return "<INVALID DP PHY>";

	/* A hole in the table is reported. */
	warned = I915_LCD_WARN_ON(!phy_names[dp_phy]);
	if (warned)
		return "<INVALID DP PHY>";

	/* Succeeded: reports the name. */
	return phy_names[dp_phy];
}

/* Converts a link rate to its DP_LINK_BW_SET code (the Linux drm_dp_link_rate_to_bw_code()). */
static u8
i915_drm_dp_link_rate_to_bw_code(
	int link_rate)
{
	/* The UHBR rates have codes of their own. */
	switch (link_rate) {
	case 1000000:
		return DP_LINK_BW_10;
	case 1350000:
		return DP_LINK_BW_13_5;
	case 2000000:
		return DP_LINK_BW_20;
	default:
		/* Spec says link_bw = link_rate / 0.27Gbps */
		return link_rate / 27000;
	}
}

/* Forgets the LTTPR common capabilities (the Linux intel_dp_reset_lttpr_common_caps()). */
static void
i915_dp_reset_lttpr_common_caps(
	struct intel_dp *intel_dp)
{
	/* Clears every capability byte. */
	kern_memset(intel_dp->lttpr_common_caps, 0, sizeof(intel_dp->lttpr_common_caps));
}

/* Forgets the LTTPR count, so no LTTPR is trained (the Linux intel_dp_reset_lttpr_count()). */
static void
i915_dp_reset_lttpr_count(
	struct intel_dp *intel_dp)
{
	/* Clears the repeater count register of the common capabilities. */
	intel_dp->lttpr_common_caps[DP_PHY_REPEATER_CNT -
				    DP_LT_TUNABLE_PHY_REPEATER_FIELD_DATA_STRUCTURE_REV] = 0;
}

/* The capabilities of one LTTPR PHY (the Linux intel_dp_lttpr_phy_caps()). */
static u8 *
i915_dp_lttpr_phy_caps(
	struct intel_dp *intel_dp,
	enum dp_phy dp_phy)
{
	/* LTTPR 1 is the first entry. */
	return intel_dp->lttpr_phy_caps[dp_phy - DP_PHY_LTTPR1];
}

/* Reads the capabilities of one LTTPR PHY into the port (the Linux intel_dp_read_lttpr_phy_caps()). */
static void
i915_dp_read_lttpr_phy_caps(
	struct intel_dp *intel_dp,
	const u8 dpcd[DP_RECEIVER_CAP_SIZE],
	enum dp_phy dp_phy)
{
	u8 *phy_caps;
	int error;

	/* Reads the PHY's capabilities into its entry. */
	phy_caps = i915_dp_lttpr_phy_caps(intel_dp, dp_phy);
	error = i915_drm_dp_read_lttpr_phy_caps(&intel_dp->aux, dpcd, dp_phy, phy_caps);
	if (error != 0) {
		lt_dbg(intel_dp, dp_phy, "failed to read the PHY caps\n");
		return;
	}

	/* Notes the capabilities read. */
	lt_dbg(intel_dp, dp_phy, "PHY capabilities: %*ph\n",
	       (int)sizeof(intel_dp->lttpr_phy_caps[0]),
	       phy_caps);
}

/*
 * Reads the LTTPR common capabilities into the port (the Linux
 * intel_dp_read_lttpr_common_caps()); on failure or a structure revision
 * below 1.4 they are cleared and false is reported.
 */
static bool
i915_dp_read_lttpr_common_caps(
	struct intel_dp *intel_dp,
	const u8 dpcd[DP_RECEIVER_CAP_SIZE])
{
	int error;

	/* Reads the common capabilities. */
	error = i915_drm_dp_read_lttpr_common_caps(&intel_dp->aux,
						   dpcd,
						   intel_dp->lttpr_common_caps);
	if (error != 0) {
		i915_dp_reset_lttpr_common_caps(intel_dp);
		return false;
	}

	/* Notes the capabilities read. */
	lt_dbg(intel_dp, DP_PHY_DPRX, "LTTPR common capabilities: %*ph\n",
	       (int)sizeof(intel_dp->lttpr_common_caps),
	       intel_dp->lttpr_common_caps);

	/* The minimum value of LT_TUNABLE_PHY_REPEATER_FIELD_DATA_STRUCTURE_REV is 1.4 */
	if (intel_dp->lttpr_common_caps[0] < 0x14) {
		i915_dp_reset_lttpr_common_caps(intel_dp);
		return false;
	}

	/* Succeeded: the capabilities are usable. */
	return true;
}

/*
 * Switches the LTTPRs to transparent or non-transparent mode (the Linux
 * intel_dp_set_lttpr_transparent_mode()); true when the sink took the write.
 */
static bool
i915_dp_set_lttpr_transparent_mode(
	struct intel_dp *intel_dp,
	bool enable)
{
	struct drm_i915_private *cur_i915;
	i915_lcd_ssize_t written;
	u8 val;

	/* Picks the mode value. */
	if (enable) {
		val = DP_PHY_REPEATER_MODE_TRANSPARENT;
	} else {
		val = DP_PHY_REPEATER_MODE_NON_TRANSPARENT;
	}

	/* Writes the mode. */
	cur_i915 = i915_dp_cur_i915(&intel_dp->aux);
	written = I915_LCD_DRM_DP_DPCD_WRITE(cur_i915, &intel_dp->aux, DP_PHY_REPEATER_MODE, &val, 1);
	if (written != 1)
		return false;

	/* Succeeded: the sink took the mode. */
	return true;
}

/* Tells whether the LTTPRs are in transparent mode (the Linux intel_dp_lttpr_transparent_mode_enabled()). */
static bool
i915_dp_lttpr_transparent_mode_enabled(
	struct intel_dp *intel_dp)
{
	/* The mode register is part of the common capabilities. */
	if (intel_dp->lttpr_common_caps[DP_PHY_REPEATER_MODE -
					DP_LT_TUNABLE_PHY_REPEATER_FIELD_DATA_STRUCTURE_REV] !=
	    DP_PHY_REPEATER_MODE_TRANSPARENT)
		return false;

	/* Succeeded: the LTTPRs are transparent. */
	return true;
}

/*
 * Reads the LTTPR common capabilities and switches the LTTPR PHYs to
 * non-transparent mode if this is supported, preserving the mode on an
 * active link (the Linux intel_dp_init_lttpr_phys()).
 *
 * Returns the number of detected LTTPRs in non-transparent mode, or 0 if
 * the LTTPRs are in transparent mode or the detection failed.
 */
static int
i915_dp_init_lttpr_phys(
	struct intel_dp *intel_dp,
	const u8 dpcd[DP_RECEIVER_CAP_SIZE])
{
	int lttpr_count;
	bool caps_read;
	bool transparent;
	bool switched;

	/* Reads the common capabilities. */
	caps_read = i915_dp_read_lttpr_common_caps(intel_dp, dpcd);
	if (!caps_read)
		return 0;

	/*
	 * Prevent setting LTTPR transparent mode explicitly if no LTTPRs are
	 * detected as this breaks link training at least on the Dell WD19TB
	 * dock.
	 */
	lttpr_count = i915_drm_dp_lttpr_count(intel_dp->lttpr_common_caps);
	if (lttpr_count == 0)
		return 0;

	/*
	 * Don't change the mode on an active link, to prevent a loss of link
	 * synchronization. See DP Standard v2.0 3.6.7. about the LTTPR
	 * resetting its internal state when the mode is changed from
	 * non-transparent to transparent.
	 */
	if (intel_dp->link_trained) {
		/* An invalid count trains no LTTPR. */
		if (lttpr_count < 0) {
			i915_dp_reset_lttpr_count(intel_dp);
			return 0;
		}

		/* Transparent LTTPRs are not trained one by one. */
		transparent = i915_dp_lttpr_transparent_mode_enabled(intel_dp);
		if (transparent) {
			i915_dp_reset_lttpr_count(intel_dp);
			return 0;
		}

		/* Succeeded: the active link keeps its non-transparent LTTPRs. */
		return lttpr_count;
	}

	/*
	 * See DP Standard v2.0 3.6.6.1. about the explicit disabling of
	 * non-transparent mode and the disable->enable non-transparent mode
	 * sequence.
	 */
	i915_dp_set_lttpr_transparent_mode(intel_dp, true);

	/*
	 * In case of unsupported number of LTTPRs or failing to switch to
	 * non-transparent mode fall-back to transparent link training mode,
	 * still taking into account any LTTPR common lane- rate/count limits.
	 */
	if (lttpr_count < 0)
		return 0;

	/* Switches to non-transparent mode, or falls back to transparent mode. */
	switched = i915_dp_set_lttpr_transparent_mode(intel_dp, false);
	if (!switched) {
		lt_dbg(intel_dp, DP_PHY_DPRX,
		       "Switching to LTTPR non-transparent LT mode failed, fall-back to transparent mode\n");

		i915_dp_set_lttpr_transparent_mode(intel_dp, true);
		i915_dp_reset_lttpr_count(intel_dp);

		return 0;
	}

	/* Succeeded: the LTTPRs are in non-transparent mode. */
	return lttpr_count;
}

/*
 * Detects the LTTPRs and reads the capabilities of each (the Linux
 * intel_dp_init_lttpr()); returns the number in non-transparent mode.
 */
static int
i915_dp_init_lttpr(
	struct intel_dp *intel_dp,
	const u8 dpcd[DP_RECEIVER_CAP_SIZE])
{
	int lttpr_count;
	int i;

	/* Detects the LTTPRs and sets their mode. */
	lttpr_count = i915_dp_init_lttpr_phys(intel_dp, dpcd);

	/* Reads the capabilities of every LTTPR PHY. */
	for (i = 0; i < lttpr_count; i++)
		i915_dp_read_lttpr_phy_caps(intel_dp, dpcd, DP_PHY_LTTPR(i));

	/* Succeeded: reports the number of non-transparent LTTPRs. */
	return lttpr_count;
}

/*
 * Detects the LTTPRs and reads the DPRX capabilities (the Linux
 * intel_dp_init_lttpr_and_dprx_caps()).
 *
 * Switches to non-transparent link training mode if LTTPRs are detected and
 * reads the PHY capabilities of each.  On an LTTPR detection error or more
 * than 8 LTTPRs the transparent mode is used.  *lttpr_count receives the
 * number of LTTPRs in non-transparent mode (0 for none or the transparent
 * mode).  Returns 0 once the DPRX capabilities are read, or EIO when they
 * could not be read (Linux: -EIO; *lttpr_count is then not written).
 */
static int
i915_dp_init_lttpr_and_dprx_caps(
	struct intel_dp *intel_dp,
	int *lttpr_count)
{
	struct drm_i915_private *cur_i915;
	u8 dpcd[DP_RECEIVER_CAP_SIZE];
	int detected;
	int probe;
	int caps_error;
	bool edp;
	bool detect;
	int display_ver;

	detected = 0;

	/*
	 * Detecting LTTPRs must be avoided on platforms with an AUX timeout
	 * period < 3.2ms. (see DP Standard v2.0, 2.11.2, 3.6.6.1).
	 */
	detect = false;
	edp = drv_i915_dp_is_edp(intel_dp);
	if (!edp) {
		display_ver = I915_LCD_DISPLAY_VER(NULL);
		if (display_ver >= 10 && !IS_GEMINILAKE(NULL))
			detect = true;
	}

	/* Finds the device whose sink the port reaches. */
	cur_i915 = i915_dp_cur_i915(&intel_dp->aux);

	/* Detects the LTTPRs from a fresh read of the DPRX capabilities. */
	if (detect) {
		probe = I915_LCD_DRM_DP_DPCD_PROBE(cur_i915, &intel_dp->aux, DP_LT_TUNABLE_PHY_REPEATER_FIELD_DATA_STRUCTURE_REV);
		if (probe != 0)
			return EIO;

		caps_error = I915_LCD_DRM_DP_READ_DPCD_CAPS(cur_i915, &intel_dp->aux, dpcd);
		if (caps_error != 0)
			return EIO;

		detected = i915_dp_init_lttpr(intel_dp, dpcd);
	}

	/*
	 * The DPTX shall read the DPRX caps after LTTPR detection, so re-read
	 * it here.
	 */
	caps_error = I915_LCD_DRM_DP_READ_DPCD_CAPS(cur_i915, &intel_dp->aux, intel_dp->dpcd);
	if (caps_error != 0) {
		i915_dp_reset_lttpr_common_caps(intel_dp);
		return EIO;
	}

	/* Succeeded: the DPRX capabilities are read; reports the LTTPRs. */
	*lttpr_count = detected;
	return 0;
}

/* The highest voltage swing allowed with a pre-emphasis level (the Linux dp_voltage_max()). */
static u8
i915_dp_voltage_max(
	u8 preemph)
{
	/* Swing and pre-emphasis levels add up to at most 3. */
	switch (preemph & DP_TRAIN_PRE_EMPHASIS_MASK) {
	case DP_TRAIN_PRE_EMPH_LEVEL_0:
		return DP_TRAIN_VOLTAGE_SWING_LEVEL_3;
	case DP_TRAIN_PRE_EMPH_LEVEL_1:
		return DP_TRAIN_VOLTAGE_SWING_LEVEL_2;
	case DP_TRAIN_PRE_EMPH_LEVEL_2:
		return DP_TRAIN_VOLTAGE_SWING_LEVEL_1;
	case DP_TRAIN_PRE_EMPH_LEVEL_3:
	default:
		return DP_TRAIN_VOLTAGE_SWING_LEVEL_0;
	}
}

/* The highest voltage swing of an LTTPR TX PHY (the Linux intel_dp_lttpr_voltage_max()). */
static u8
i915_dp_lttpr_voltage_max(
	struct intel_dp *intel_dp,
	enum dp_phy dp_phy)
{
	const u8 *phy_caps;
	bool level_3;

	/* Asks the PHY's capabilities for level 3. */
	phy_caps = i915_dp_lttpr_phy_caps(intel_dp, dp_phy);
	level_3 = i915_drm_dp_lttpr_voltage_swing_level_3_supported(phy_caps);
	if (!level_3)
		return DP_TRAIN_VOLTAGE_SWING_LEVEL_2;

	/* Succeeded: the PHY supports level 3. */
	return DP_TRAIN_VOLTAGE_SWING_LEVEL_3;
}

/* The highest pre-emphasis of an LTTPR TX PHY (the Linux intel_dp_lttpr_preemph_max()). */
static u8
i915_dp_lttpr_preemph_max(
	struct intel_dp *intel_dp,
	enum dp_phy dp_phy)
{
	const u8 *phy_caps;
	bool level_3;

	/* Asks the PHY's capabilities for level 3. */
	phy_caps = i915_dp_lttpr_phy_caps(intel_dp, dp_phy);
	level_3 = i915_drm_dp_lttpr_pre_emphasis_level_3_supported(phy_caps);
	if (!level_3)
		return DP_TRAIN_PRE_EMPH_LEVEL_2;

	/* Succeeded: the PHY supports level 3. */
	return DP_TRAIN_PRE_EMPH_LEVEL_3;
}

/*
 * Tells whether a PHY is the one right after the source (the Linux
 * intel_dp_phy_is_downstream_of_source()): the DPRX without LTTPRs, or the
 * last LTTPR.
 */
static bool
i915_dp_phy_is_downstream_of_source(
	struct intel_dp *intel_dp,
	enum dp_phy dp_phy)
{
	int lttpr_count;

	/* Counts the LTTPRs; an LTTPR PHY without LTTPRs is reported. */
	lttpr_count = i915_drm_dp_lttpr_count(intel_dp->lttpr_common_caps);
	drm_WARN_ON_ONCE(NULL, lttpr_count <= 0 && dp_phy != DP_PHY_DPRX);

	/* Without LTTPRs every PHY is taken as the source's neighbour. */
	if (lttpr_count <= 0)
		return true;

	/*
	 * The last LTTPR is the one the source drives.  The PHY is compared
	 * as an int: the count is positive here, so the PHY number is too.
	 */
	if ((int)dp_phy == DP_PHY_LTTPR(lttpr_count - 1))
		return true;

	/* Succeeded: an LTTPR drives this PHY. */
	return false;
}

/*
 * The highest voltage swing of the DPTX PHY (source or LTTPR) upstream from
 * a PHY (the Linux intel_dp_phy_voltage_max()).
 */
static u8
i915_dp_phy_voltage_max(
	struct intel_dp *intel_dp,
	const struct intel_crtc_state *crtc_state,
	enum dp_phy dp_phy)
{
	u8 (*voltage_max_hook)(struct intel_dp *intel_dp, const struct intel_crtc_state *crtc_state);
	bool downstream;
	u8 voltage_max;

	/*
	 * Get voltage_max from the DPTX_PHY (source or LTTPR) upstream from
	 * the DPRX_PHY we train.
	 */
	downstream = i915_dp_phy_is_downstream_of_source(intel_dp, dp_phy);
	if (downstream) {
		voltage_max_hook = intel_dp->voltage_max;
		voltage_max = voltage_max_hook(intel_dp, crtc_state);
	} else {
		voltage_max = i915_dp_lttpr_voltage_max(intel_dp, dp_phy + 1);
	}

	/* Only levels 2 and 3 are expected as a maximum. */
	drm_WARN_ON_ONCE(NULL,
			 voltage_max != DP_TRAIN_VOLTAGE_SWING_LEVEL_2 &&
			 voltage_max != DP_TRAIN_VOLTAGE_SWING_LEVEL_3);

	/* Succeeded: reports the maximum. */
	return voltage_max;
}

/*
 * The highest pre-emphasis of the DPTX PHY (source or LTTPR) upstream from
 * a PHY (the Linux intel_dp_phy_preemph_max()).
 */
static u8
i915_dp_phy_preemph_max(
	struct intel_dp *intel_dp,
	enum dp_phy dp_phy)
{
	u8 (*preemph_max_hook)(struct intel_dp *intel_dp);
	bool downstream;
	u8 preemph_max;

	/*
	 * Get preemph_max from the DPTX_PHY (source or LTTPR) upstream from
	 * the DPRX_PHY we train.
	 */
	downstream = i915_dp_phy_is_downstream_of_source(intel_dp, dp_phy);
	if (downstream) {
		preemph_max_hook = intel_dp->preemph_max;
		preemph_max = preemph_max_hook(intel_dp);
	} else {
		preemph_max = i915_dp_lttpr_preemph_max(intel_dp, dp_phy + 1);
	}

	/* Only levels 2 and 3 are expected as a maximum. */
	drm_WARN_ON_ONCE(NULL,
			 preemph_max != DP_TRAIN_PRE_EMPH_LEVEL_2 &&
			 preemph_max != DP_TRAIN_PRE_EMPH_LEVEL_3);

	/* Succeeded: reports the maximum. */
	return preemph_max;
}

/*
 * Tells whether the DPTX upstream from a PHY sets the signal levels per lane
 * (the Linux has_per_lane_signal_levels()): an LTTPR always does, the source
 * from display version 11.
 */
static bool
i915_has_per_lane_signal_levels(
	struct intel_dp *intel_dp,
	enum dp_phy dp_phy)
{
	bool downstream;
	int display_ver;

	/* An LTTPR sets the levels per lane. */
	downstream = i915_dp_phy_is_downstream_of_source(intel_dp, dp_phy);
	if (!downstream)
		return true;

	/* The source does so from display version 11. */
	display_ver = I915_LCD_DISPLAY_VER(NULL);
	if (display_ver >= 11)
		return true;

	/* Succeeded: one level applies to every lane. */
	return false;
}

/* The 128b/132b TX FFE preset to use on one lane (the Linux intel_dp_get_lane_adjust_tx_ffe_preset()). */
static u8
i915_dp_get_lane_adjust_tx_ffe_preset(
	struct intel_dp *intel_dp,
	const struct intel_crtc_state *crtc_state,
	enum dp_phy dp_phy,
	const u8 link_status[DP_LINK_STATUS_SIZE],
	int lane)
{
	u8 tx_ffe;
	u8 lane_tx_ffe;
	bool per_lane;

	tx_ffe = 0;

	/* Takes the lane's own request, or the highest request of all lanes. */
	per_lane = i915_has_per_lane_signal_levels(intel_dp, dp_phy);
	if (per_lane) {
		lane = min(lane, crtc_state->lane_count - 1);
		tx_ffe = i915_drm_dp_get_adjust_tx_ffe_preset(link_status, lane);
	} else {
		for (lane = 0; lane < crtc_state->lane_count; lane++) {
			lane_tx_ffe = i915_drm_dp_get_adjust_tx_ffe_preset(link_status, lane);
			tx_ffe = max(tx_ffe, lane_tx_ffe);
		}
	}

	/* Succeeded: reports the preset. */
	return tx_ffe;
}

/*
 * The 8b/10b voltage swing and pre-emphasis to use on one lane, limited to
 * what the DPTX supports (the Linux intel_dp_get_lane_adjust_vswing_preemph()).
 */
static u8
i915_dp_get_lane_adjust_vswing_preemph(
	struct intel_dp *intel_dp,
	const struct intel_crtc_state *crtc_state,
	enum dp_phy dp_phy,
	const u8 link_status[DP_LINK_STATUS_SIZE],
	int lane)
{
	u8 v;
	u8 p;
	u8 lane_v;
	u8 lane_p;
	u8 voltage_max;
	u8 preemph_max;
	u8 preemph_voltage_max;
	bool per_lane;

	v = 0;
	p = 0;

	/* Takes the lane's own request, or the highest request of all lanes. */
	per_lane = i915_has_per_lane_signal_levels(intel_dp, dp_phy);
	if (per_lane) {
		lane = min(lane, crtc_state->lane_count - 1);

		v = i915_drm_dp_get_adjust_request_voltage(link_status, lane);
		p = i915_drm_dp_get_adjust_request_pre_emphasis(link_status, lane);
	} else {
		for (lane = 0; lane < crtc_state->lane_count; lane++) {
			lane_v = i915_drm_dp_get_adjust_request_voltage(link_status, lane);
			v = max(v, lane_v);
			lane_p = i915_drm_dp_get_adjust_request_pre_emphasis(link_status, lane);
			p = max(p, lane_p);
		}
	}

	/* Limits the pre-emphasis and marks its maximum. */
	preemph_max = i915_dp_phy_preemph_max(intel_dp, dp_phy);
	if (p >= preemph_max)
		p = preemph_max | DP_TRAIN_MAX_PRE_EMPHASIS_REACHED;

	/* Limits the swing to what the pre-emphasis leaves. */
	preemph_voltage_max = i915_dp_voltage_max(p);
	v = min(v, preemph_voltage_max);

	/* Limits the swing to the DPTX and marks its maximum. */
	voltage_max = i915_dp_phy_voltage_max(intel_dp, crtc_state, dp_phy);
	if (v >= voltage_max)
		v = voltage_max | DP_TRAIN_MAX_SWING_REACHED;

	/* Succeeded: reports the lane's training-set byte. */
	return v | p;
}

/* The training-set byte of one lane for the link's channel coding (the Linux intel_dp_get_lane_adjust_train()). */
static u8
i915_dp_get_lane_adjust_train(
	struct intel_dp *intel_dp,
	const struct intel_crtc_state *crtc_state,
	enum dp_phy dp_phy,
	const u8 link_status[DP_LINK_STATUS_SIZE],
	int lane)
{
	bool uhbr;
	u8 train_set_lane;

	/* 128b/132b sets a TX FFE preset, 8b/10b a swing and pre-emphasis. */
	uhbr = drv_i915_dp_is_uhbr(crtc_state);
	if (uhbr) {
		train_set_lane = i915_dp_get_lane_adjust_tx_ffe_preset(intel_dp,
								       crtc_state,
								       dp_phy,
								       link_status,
								       lane);
	} else {
		train_set_lane = i915_dp_get_lane_adjust_vswing_preemph(intel_dp,
									crtc_state,
									dp_phy,
									link_status,
									lane);
	}

	/* Succeeded: reports the byte. */
	return train_set_lane;
}

/*
 * Updates the training set from the sink's adjust requests (the Linux
 * intel_dp_get_adjust_train()).
 *
 * The data macros TRAIN_REQ_*_ARGS call the Linux helper names, which are
 * static here under the i915_ names; the log arguments are written out with
 * those.  lt_dbg() does not evaluate them.
 */
static void
i915_dp_get_adjust_train(
	struct intel_dp *intel_dp,
	const struct intel_crtc_state *crtc_state,
	enum dp_phy dp_phy,
	const u8 link_status[DP_LINK_STATUS_SIZE])
{
	bool uhbr;
	int lane;

	/* Notes the requests of the sink. */
	uhbr = drv_i915_dp_is_uhbr(crtc_state);
	if (uhbr) {
		lt_dbg(intel_dp, dp_phy,
		       "128b/132b, lanes: %d, "
		       "TX FFE request: " TRAIN_REQ_FMT "\n",
		       crtc_state->lane_count,
		       i915_drm_dp_get_adjust_tx_ffe_preset(link_status, 0),
		       i915_drm_dp_get_adjust_tx_ffe_preset(link_status, 1),
		       i915_drm_dp_get_adjust_tx_ffe_preset(link_status, 2),
		       i915_drm_dp_get_adjust_tx_ffe_preset(link_status, 3));
	} else {
		lt_dbg(intel_dp, dp_phy,
		       "8b/10b, lanes: %d, "
		       "vswing request: " TRAIN_REQ_FMT ", "
		       "pre-emphasis request: " TRAIN_REQ_FMT "\n",
		       crtc_state->lane_count,
		       i915_drm_dp_get_adjust_request_voltage(link_status, 0) >> DP_TRAIN_VOLTAGE_SWING_SHIFT,
		       i915_drm_dp_get_adjust_request_voltage(link_status, 1) >> DP_TRAIN_VOLTAGE_SWING_SHIFT,
		       i915_drm_dp_get_adjust_request_voltage(link_status, 2) >> DP_TRAIN_VOLTAGE_SWING_SHIFT,
		       i915_drm_dp_get_adjust_request_voltage(link_status, 3) >> DP_TRAIN_VOLTAGE_SWING_SHIFT,
		       i915_drm_dp_get_adjust_request_pre_emphasis(link_status, 0) >> DP_TRAIN_PRE_EMPHASIS_SHIFT,
		       i915_drm_dp_get_adjust_request_pre_emphasis(link_status, 1) >> DP_TRAIN_PRE_EMPHASIS_SHIFT,
		       i915_drm_dp_get_adjust_request_pre_emphasis(link_status, 2) >> DP_TRAIN_PRE_EMPHASIS_SHIFT,
		       i915_drm_dp_get_adjust_request_pre_emphasis(link_status, 3) >> DP_TRAIN_PRE_EMPHASIS_SHIFT);
	}

	/* Computes the training-set byte of all four lanes. */
	for (lane = 0; lane < 4; lane++) {
		intel_dp->train_set[lane] = i915_dp_get_lane_adjust_train(intel_dp,
									  crtc_state,
									  dp_phy,
									  link_status,
									  lane);
	}
}

/* The DPCD register that sets the training pattern of a PHY (the Linux intel_dp_training_pattern_set_reg()). */
static int
i915_dp_training_pattern_set_reg(
	struct intel_dp *intel_dp,
	enum dp_phy dp_phy)
{
	UNUSED_PARAMETER(intel_dp);

	/* The DPRX has its own register; each LTTPR has one in its range. */
	if (dp_phy == DP_PHY_DPRX)
		return DP_TRAINING_PATTERN_SET;

	/* Succeeded: reports the LTTPR's register. */
	return DP_TRAINING_PATTERN_SET_PHY_REPEATER(dp_phy);
}

/*
 * Programs a training pattern on the port and writes it with the training
 * set to the sink (the Linux intel_dp_set_link_train()); true when the sink
 * took every byte.
 */
static bool
i915_dp_set_link_train(
	struct intel_dp *intel_dp,
	const struct intel_crtc_state *crtc_state,
	enum dp_phy dp_phy,
	u8 dp_train_pat)
{
	struct drm_i915_private *cur_i915;
	u8 buf[sizeof(intel_dp->train_set) + 1];
	i915_lcd_ssize_t written;
	int reg;
	int len;

	/* Programs the port first. */
	reg = i915_dp_training_pattern_set_reg(intel_dp, dp_phy);
	i915_dp_program_link_training_pattern(intel_dp,
					      crtc_state,
					      dp_phy,
					      dp_train_pat);

	/* DP_TRAINING_LANEx_SET follow DP_TRAINING_PATTERN_SET */
	buf[0] = dp_train_pat;
	kern_memcpy(buf + 1, intel_dp->train_set, crtc_state->lane_count);
	len = crtc_state->lane_count + 1;

	/* Writes the pattern and the lanes' levels in one transfer. */
	cur_i915 = i915_dp_cur_i915(&intel_dp->aux);
	written = I915_LCD_DRM_DP_DPCD_WRITE(cur_i915, &intel_dp->aux, reg, buf, len);
	if (written != len)
		return false;

	/* Succeeded: the sink took the pattern and the levels. */
	return true;
}

/* The digit of a training pattern for a log line (the Linux dp_training_pattern_name()). */
static char
i915_dp_training_pattern_name(
	u8 train_pat)
{
	/* TPS1..3 are named by their value, TPS4 by its number. */
	switch (train_pat) {
	case DP_TRAINING_PATTERN_1:
	case DP_TRAINING_PATTERN_2:
	case DP_TRAINING_PATTERN_3:
		return '0' + train_pat;
	case DP_TRAINING_PATTERN_4:
		return '4';
	default:
		I915_LCD_MISSING_CASE(train_pat);
		return '?';
	}
}

/*
 * Programs a training pattern on the port through the port's hook (the
 * Linux intel_dp_program_link_training_pattern()).
 */
static void
i915_dp_program_link_training_pattern(
	struct intel_dp *intel_dp,
	const struct intel_crtc_state *crtc_state,
	enum dp_phy dp_phy,
	u8 dp_train_pat)
{
	void (*set_link_train)(struct intel_dp *intel_dp, const struct intel_crtc_state *crtc_state, u8 dp_train_pat);
	u8 train_pat;

	/* The PHY only names the log line, which lt_dbg() does not print. */
	UNUSED_PARAMETER(dp_phy);

	/*
	 * Notes a pattern being enabled.  The pattern name is an argument of
	 * lt_dbg(), which does not evaluate it.
	 */
	train_pat = intel_dp_training_pattern_symbol(dp_train_pat);
	if (train_pat != DP_TRAINING_PATTERN_DISABLE) {
		lt_dbg(intel_dp, dp_phy, "Using DP training pattern TPS%c\n",
		       i915_dp_training_pattern_name(train_pat));
	}

	/* Programs the pattern on the port. */
	set_link_train = intel_dp->set_link_train;
	set_link_train(intel_dp, crtc_state, dp_train_pat);
}

/*
 * Programs the training set's signal levels on the port when it drives the
 * PHY (the Linux intel_dp_set_signal_levels()).
 */
static void
i915_dp_set_signal_levels(
	struct intel_dp *intel_dp,
	const struct intel_crtc_state *crtc_state,
	enum dp_phy dp_phy)
{
	void (*set_signal_levels)(struct intel_encoder *encoder, const struct intel_crtc_state *crtc_state);
	struct intel_encoder *encoder;
	bool uhbr;
	bool downstream;

	/* Finds the port's encoder, whose hook sets the levels. */
	encoder = &i915_lcd_dp_to_dig_port(intel_dp)->base;

	/* Notes the levels. */
	uhbr = drv_i915_dp_is_uhbr(crtc_state);
	if (uhbr) {
		lt_dbg(intel_dp, dp_phy,
		       "128b/132b, lanes: %d, "
		       "TX FFE presets: " TRAIN_SET_FMT "\n",
		       crtc_state->lane_count,
		       TRAIN_SET_TX_FFE_ARGS(intel_dp->train_set));
	} else {
		lt_dbg(intel_dp, dp_phy,
		       "8b/10b, lanes: %d, "
		       "vswing levels: " TRAIN_SET_FMT ", "
		       "pre-emphasis levels: " TRAIN_SET_FMT "\n",
		       crtc_state->lane_count,
		       TRAIN_SET_VSWING_ARGS(intel_dp->train_set),
		       TRAIN_SET_PREEMPH_ARGS(intel_dp->train_set));
	}

	/* Only the PHY right after the source is driven by the port. */
	downstream = i915_dp_phy_is_downstream_of_source(intel_dp, dp_phy);
	if (downstream) {
		set_signal_levels = encoder->set_signal_levels;
		set_signal_levels(encoder, crtc_state);
	}
}

/*
 * Starts a training phase from the lowest levels (the Linux
 * intel_dp_reset_link_train()); true when the sink took the pattern.
 */
static bool
i915_dp_reset_link_train(
	struct intel_dp *intel_dp,
	const struct intel_crtc_state *crtc_state,
	enum dp_phy dp_phy,
	u8 dp_train_pat)
{
	bool written;

	/* Programs the lowest levels. */
	kern_memset(intel_dp->train_set, 0, sizeof(intel_dp->train_set));
	i915_dp_set_signal_levels(intel_dp, crtc_state, dp_phy);

	/* Sets the pattern with those levels. */
	written = i915_dp_set_link_train(intel_dp, crtc_state, dp_phy, dp_train_pat);
	if (!written)
		return false;

	/* Succeeded: the training phase started. */
	return true;
}

/*
 * Programs the updated training set on the port and in the sink (the Linux
 * intel_dp_update_link_train()); true when the sink took every lane.
 */
static bool
i915_dp_update_link_train(
	struct intel_dp *intel_dp,
	const struct intel_crtc_state *crtc_state,
	enum dp_phy dp_phy)
{
	struct drm_i915_private *cur_i915;
	i915_lcd_ssize_t ret;
	int reg;

	/* The DPRX has its own lane register; each LTTPR has one in its range. */
	if (dp_phy == DP_PHY_DPRX) {
		reg = DP_TRAINING_LANE0_SET;
	} else {
		reg = DP_TRAINING_LANE0_SET_PHY_REPEATER(dp_phy);
	}

	/* Programs the port. */
	i915_dp_set_signal_levels(intel_dp, crtc_state, dp_phy);

	/* Writes the lanes' levels to the sink. */
	cur_i915 = i915_dp_cur_i915(&intel_dp->aux);
	ret = I915_LCD_DRM_DP_DPCD_WRITE(cur_i915,
					 &intel_dp->aux,
					 reg,
					 intel_dp->train_set,
					 crtc_state->lane_count);
	if (ret != crtc_state->lane_count)
		return false;

	/* Succeeded: the sink took every lane. */
	return true;
}

/* Tells whether a lane's 128b/132b TX FFE preset is at its maximum (the Linux intel_dp_lane_max_tx_ffe_reached()). */
static bool
i915_dp_lane_max_tx_ffe_reached(
	u8 train_set_lane)
{
	/* The maximum preset has every bit of the field set. */
	if ((train_set_lane & DP_TX_FFE_PRESET_VALUE_MASK) != DP_TX_FFE_PRESET_VALUE_MASK)
		return false;

	/* Succeeded: the preset is at its maximum. */
	return true;
}

/*
 * Tells whether a lane's 8b/10b swing is at its maximum (the Linux
 * intel_dp_lane_max_vswing_reached()).
 *
 * FIXME: The DP spec is very confusing here, also the Link CTS spec seems to
 * have self contradicting tests around this area.
 *
 * In lieu of better ideas let's just stop when we've reached the max supported
 * vswing with its max pre-emphasis, which is either 2+1 or 3+0 depending on
 * whether vswing level 3 is supported or not.
 */
static bool
i915_dp_lane_max_vswing_reached(
	u8 train_set_lane)
{
	u8 v;
	u8 p;

	/* Extracts the swing and the pre-emphasis levels. */
	v = (train_set_lane & DP_TRAIN_VOLTAGE_SWING_MASK) >>
		DP_TRAIN_VOLTAGE_SWING_SHIFT;
	p = (train_set_lane & DP_TRAIN_PRE_EMPHASIS_MASK) >>
		DP_TRAIN_PRE_EMPHASIS_SHIFT;

	/* The swing must be marked as the maximum. */
	if ((train_set_lane & DP_TRAIN_MAX_SWING_REACHED) == 0)
		return false;

	/* The levels must add up to the top of the table. */
	if (v + p != 3)
		return false;

	/* Succeeded: the lane is at its maximum. */
	return true;
}

/* Tells whether every lane in use is at its maximum level (the Linux intel_dp_link_max_vswing_reached()). */
static bool
i915_dp_link_max_vswing_reached(
	struct intel_dp *intel_dp,
	const struct intel_crtc_state *crtc_state)
{
	int lane;
	u8 train_set_lane;
	bool uhbr;
	bool reached;

	/* Looks for a lane in use below its maximum. */
	for (lane = 0; lane < crtc_state->lane_count; lane++) {
		train_set_lane = intel_dp->train_set[lane];

		/* The maximum depends on the channel coding. */
		uhbr = drv_i915_dp_is_uhbr(crtc_state);
		if (uhbr) {
			reached = i915_dp_lane_max_tx_ffe_reached(train_set_lane);
		} else {
			reached = i915_dp_lane_max_vswing_reached(train_set_lane);
		}

		if (!reached)
			return false;
	}

	/* Succeeded: every lane in use is at its maximum. */
	return true;
}

/* Writes the sink's MSA-ignore and channel coding bits (the Linux intel_dp_update_downspread_ctrl()). */
static void
i915_dp_update_downspread_ctrl(
	struct intel_dp *intel_dp,
	const struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *cur_i915;
	u8 link_config[2];
	bool uhbr;

	/* VRR needs the sink to ignore the MSA timing parameters. */
	if (crtc_state->vrr.flipline) {
		link_config[0] = DP_MSA_TIMING_PAR_IGNORE_EN;
	} else {
		link_config[0] = 0;
	}

	/* Selects the channel coding of the link rate. */
	uhbr = drv_i915_dp_is_uhbr(crtc_state);
	if (uhbr) {
		link_config[1] = DP_SET_ANSI_128B132B;
	} else {
		link_config[1] = DP_SET_ANSI_8B10B;
	}

	/* Writes both bytes; the result is not looked at. */
	cur_i915 = i915_dp_cur_i915(&intel_dp->aux);
	I915_LCD_DRM_DP_DPCD_WRITE(cur_i915, &intel_dp->aux, DP_DOWNSPREAD_CTRL, link_config, 2);
}

/* Writes the link rate and lane count to the sink (the Linux intel_dp_update_link_bw_set()). */
static void
i915_dp_update_link_bw_set(
	struct intel_dp *intel_dp,
	const struct intel_crtc_state *crtc_state,
	u8 link_bw,
	u8 rate_select)
{
	struct drm_i915_private *cur_i915;
	u8 link_config[2];
	u8 lane_count;

	/* The lane count carries the enhanced framing bit. */
	lane_count = crtc_state->lane_count;
	if (crtc_state->enhanced_framing)
		lane_count |= DP_LANE_COUNT_ENHANCED_FRAME_EN;

	/* Finds the device whose sink the port reaches. */
	cur_i915 = i915_dp_cur_i915(&intel_dp->aux);

	/* Sets the rate by its bandwidth code, or by its index in the sink's rate table. */
	if (link_bw) {
		/* DP and eDP v1.3 and earlier link bw set method. */
		link_config[0] = link_bw;
		link_config[1] = lane_count;

		I915_LCD_DRM_DP_DPCD_WRITE(cur_i915,
					   &intel_dp->aux,
					   DP_LINK_BW_SET,
					   link_config,
					   ARRAY_SIZE(link_config));
	} else {
		/*
		 * eDP v1.4 and later link rate set method.
		 *
		 * eDP v1.4x sinks shall ignore DP_LINK_RATE_SET if
		 * DP_LINK_BW_SET is set. Avoid writing DP_LINK_BW_SET.
		 *
		 * eDP v1.5 sinks allow choosing either, and the last choice
		 * shall be active.
		 */
		I915_LCD_DRM_DP_DPCD_WRITEB(cur_i915, &intel_dp->aux, DP_LANE_COUNT_SET, lane_count);
		I915_LCD_DRM_DP_DPCD_WRITEB(cur_i915, &intel_dp->aux, DP_LINK_RATE_SET, rate_select);
	}
}

/*
 * Prepares link training by configuring the link parameters; on DDI
 * platforms the port is enabled here too (the Linux
 * intel_dp_prepare_link_train()).
 */
static bool
i915_dp_prepare_link_train(
	struct intel_dp *intel_dp,
	const struct intel_crtc_state *crtc_state)
{
	void (*prepare_link_retrain)(struct intel_dp *intel_dp, const struct intel_crtc_state *crtc_state);
	struct drm_i915_private *cur_i915;
	__le16 sink_rates[DP_MAX_SUPPORTED_RATES];
	u8 link_bw;
	u8 rate_select;

	/* Lets the port prepare the retraining (the DDI enables the port here). */
	prepare_link_retrain = intel_dp->prepare_link_retrain;
	if (prepare_link_retrain)
		prepare_link_retrain(intel_dp, crtc_state);

	/* Finds how the link rate is set. */
	i915_dp_compute_rate(intel_dp, crtc_state->port_clock, &link_bw, &rate_select);

	/*
	 * WaEdpLinkRateDataReload
	 *
	 * Parade PS8461E MUX (used on varius TGL+ laptops) needs
	 * to snoop the link rates reported by the sink when we
	 * use LINK_RATE_SET in order to operate in jitter cleaning
	 * mode (as opposed to redriver mode). Unfortunately it
	 * loses track of the snooped link rates when powered down,
	 * so we need to make it re-snoop often. Without this high
	 * link rates are not stable.
	 */
	if (!link_bw) {
		lt_dbg(intel_dp, DP_PHY_DPRX, "Reloading eDP link rates\n");

		cur_i915 = i915_dp_cur_i915(&intel_dp->aux);
		I915_LCD_DRM_DP_DPCD_READ(cur_i915,
					  &intel_dp->aux,
					  DP_SUPPORTED_LINK_RATES,
					  sink_rates,
					  sizeof(sink_rates));
	}

	/* Notes the rate setting. */
	if (link_bw) {
		lt_dbg(intel_dp, DP_PHY_DPRX, "Using LINK_BW_SET value %02x\n",
		       link_bw);
	} else {
		lt_dbg(intel_dp, DP_PHY_DPRX,
		       "Using LINK_RATE_SET value %02x\n",
		       rate_select);
	}

	/*
	 * Spec DP2.1 Section 3.5.2.16
	 * Prior to LT DPTX should set 128b/132b DP Channel coding and then set link rate
	 */
	i915_dp_update_downspread_ctrl(intel_dp, crtc_state);
	i915_dp_update_link_bw_set(intel_dp, crtc_state, link_bw, rate_select);

	/* Succeeded: the link is configured. */
	return true;
}

/* Tells whether the sink's adjust requests changed on a lane in use (the Linux intel_dp_adjust_request_changed()). */
static bool
i915_dp_adjust_request_changed(
	const struct intel_crtc_state *crtc_state,
	const u8 old_link_status[DP_LINK_STATUS_SIZE],
	const u8 new_link_status[DP_LINK_STATUS_SIZE])
{
	int lane;
	u8 old_request;
	u8 new_request;
	u8 old_voltage;
	u8 new_voltage;
	bool uhbr;

	/* Compares the requests of each lane in use. */
	for (lane = 0; lane < crtc_state->lane_count; lane++) {
		/* 128b/132b requests a TX FFE preset, 8b/10b a swing and pre-emphasis. */
		uhbr = drv_i915_dp_is_uhbr(crtc_state);
		if (uhbr) {
			old_request = i915_drm_dp_get_adjust_tx_ffe_preset(old_link_status, lane);
			new_request = i915_drm_dp_get_adjust_tx_ffe_preset(new_link_status, lane);
		} else {
			old_voltage = i915_drm_dp_get_adjust_request_voltage(old_link_status, lane);
			old_request = old_voltage |
				i915_drm_dp_get_adjust_request_pre_emphasis(old_link_status, lane);
			new_voltage = i915_drm_dp_get_adjust_request_voltage(new_link_status, lane);
			new_request = new_voltage |
				i915_drm_dp_get_adjust_request_pre_emphasis(new_link_status, lane);
		}

		if (old_request != new_request)
			return true;
	}

	/* Succeeded: the sink asks for the same levels. */
	return false;
}

/* Notes the six link-status bytes (the Linux intel_dp_dump_link_status()). */
static void
i915_dp_dump_link_status(
	struct intel_dp *intel_dp,
	enum dp_phy dp_phy,
	const u8 link_status[DP_LINK_STATUS_SIZE])
{
	UNUSED_PARAMETER(intel_dp);
	UNUSED_PARAMETER(dp_phy);

	/* Notes the status. */
	lt_dbg(intel_dp, dp_phy,
	       "ln0_1:0x%x ln2_3:0x%x align:0x%x sink:0x%x adj_req0_1:0x%x adj_req2_3:0x%x\n",
	       link_status[0], link_status[1], link_status[2],
	       link_status[3], link_status[4], link_status[5]);
}

/*
 * Performs the clock recovery phase of the link training on a PHY using
 * training pattern 1 (the Linux intel_dp_link_training_clock_recovery()).
 */
static bool
i915_dp_link_training_clock_recovery(
	struct intel_dp *intel_dp,
	const struct intel_crtc_state *crtc_state,
	enum dp_phy dp_phy)
{
	struct drm_i915_private *cur_i915;
	u8 old_link_status[DP_LINK_STATUS_SIZE];
	int voltage_tries;
	int cr_tries;
	int max_cr_tries;
	u8 link_status[DP_LINK_STATUS_SIZE];
	bool max_vswing_reached;
	int delay_us;
	bool uhbr;
	bool started;
	int status_error;
	bool recovered;
	bool updated;
	bool changed;
	bool reached;

	kern_memset(old_link_status, 0, sizeof(old_link_status));
	max_vswing_reached = false;

	/* Finds the device whose sink the port reaches and sleeps on. */
	cur_i915 = i915_dp_cur_i915(&intel_dp->aux);

	/* Reads how long the sink needs between the steps. */
	uhbr = drv_i915_dp_is_uhbr(crtc_state);
	delay_us = i915_drm_dp_read_clock_recovery_delay(&intel_dp->aux,
							 intel_dp->dpcd,
							 dp_phy,
							 uhbr);

	/* clock recovery */
	started = i915_dp_reset_link_train(intel_dp,
					   crtc_state,
					   dp_phy,
					   DP_TRAINING_PATTERN_1 | DP_LINK_SCRAMBLING_DISABLE);
	if (!started) {
		lt_err(intel_dp, dp_phy, "Failed to enable link training\n");
		return false;
	}

	/*
	 * The DP 1.4 spec defines the max clock recovery retries value
	 * as 10 but for pre-DP 1.4 devices we set a very tolerant
	 * retry limit of 80 (4 voltage levels x 4 preemphasis levels x
	 * x 5 identical voltage retries). Since the previous specs didn't
	 * define a limit and created the possibility of an infinite loop
	 * we want to prevent any sync from triggering that corner case.
	 */
	if (intel_dp->dpcd[DP_DPCD_REV] >= DP_DPCD_REV_14) {
		max_cr_tries = 10;
	} else {
		max_cr_tries = 80;
	}

	/* Adjusts the levels until the sink recovers the clock or a limit is hit. */
	voltage_tries = 1;
	for (cr_tries = 0; cr_tries < max_cr_tries; ++cr_tries) {
		i915_lcd_usleep_range(cur_i915, delay_us, 2 * delay_us);

		/* Reads the sink's status. */
		status_error = drv_i915_drm_dp_dpcd_read_phy_link_status(&intel_dp->aux,
									  dp_phy,
									  link_status);
		if (status_error != 0) {
			lt_err(intel_dp, dp_phy, "Failed to get link status\n");
			return false;
		}

		/* A recovered clock ends the phase. */
		recovered = drv_i915_drm_dp_clock_recovery_ok(link_status, crtc_state->lane_count);
		if (recovered)
			break;

		/* The same levels were tried five times. */
		if (voltage_tries == 5) {
			i915_dp_dump_link_status(intel_dp, dp_phy, link_status);
			lt_dbg(intel_dp, dp_phy, "Same voltage tried 5 times\n");
			return false;
		}

		/* Every lane is already at its maximum. */
		if (max_vswing_reached) {
			i915_dp_dump_link_status(intel_dp, dp_phy, link_status);
			lt_dbg(intel_dp, dp_phy, "Max Voltage Swing reached\n");
			return false;
		}

		/* Update training set as requested by target */
		i915_dp_get_adjust_train(intel_dp, crtc_state, dp_phy, link_status);
		updated = i915_dp_update_link_train(intel_dp, crtc_state, dp_phy);
		if (!updated) {
			lt_err(intel_dp, dp_phy, "Failed to update link training\n");
			return false;
		}

		/* Counts the tries of unchanged requests. */
		changed = i915_dp_adjust_request_changed(crtc_state, old_link_status, link_status);
		if (!changed) {
			++voltage_tries;
		} else {
			voltage_tries = 1;
		}

		/* Keeps the status for the next comparison. */
		kern_memcpy(old_link_status, link_status, sizeof(link_status));

		/* Remembers that the levels cannot go higher. */
		reached = i915_dp_link_max_vswing_reached(intel_dp, crtc_state);
		if (reached)
			max_vswing_reached = true;
	}

	/* The sink never recovered the clock. */
	if (cr_tries == max_cr_tries) {
		i915_dp_dump_link_status(intel_dp, dp_phy, link_status);
		lt_err(intel_dp, dp_phy, "Failed clock recovery %d times, giving up!\n",
		       max_cr_tries);

		return false;
	}

	/* Succeeded: the sink recovered the clock. */
	lt_dbg(intel_dp, dp_phy, "Clock recovery OK\n");
	return true;
}

/*
 * Picks the training pattern sequence for channel equalization (the Linux
 * intel_dp_training_pattern()): 128b/132b TPS2 for UHBR+, TPS4 for HBR3 or
 * for 1.4 devices that support it, TPS3 for HBR2 or 1.2 devices that
 * support it, TPS2 otherwise.
 */
static u32
i915_dp_training_pattern(
	struct intel_dp *intel_dp,
	const struct intel_crtc_state *crtc_state,
	enum dp_phy dp_phy)
{
	struct drm_i915_private *i915;
	bool source_tps3;
	bool sink_tps3;
	bool source_tps4;
	bool sink_tps4;
	bool uhbr;

	/* Finds the device the port belongs to. */
	i915 = i915_lcd_dp_to_i915(intel_dp);

	/* UHBR+ use separate 128b/132b TPS2 */
	uhbr = drv_i915_dp_is_uhbr(crtc_state);
	if (uhbr)
		return DP_TRAINING_PATTERN_2;

	/*
	 * TPS4 support is mandatory for all downstream devices that
	 * support HBR3. There are no known eDP panels that support
	 * TPS4 as of Feb 2018 as per VESA eDP_v1.4b_E1 specification.
	 * LTTPRs must support TPS4.
	 */
	source_tps4 = i915_dp_source_supports_tps4(i915);
	if (dp_phy != DP_PHY_DPRX) {
		sink_tps4 = true;
	} else {
		sink_tps4 = dp_tps4_supported(intel_dp->dpcd);
	}

	/* Uses TPS4 when both ends support it; notes an HBR3 link without it. */
	if (source_tps4 && sink_tps4) {
		return DP_TRAINING_PATTERN_4;
	} else if (crtc_state->port_clock == 810000) {
		if (!source_tps4) {
			lt_dbg(intel_dp, dp_phy,
			       "8.1 Gbps link rate without source TPS4 support\n");
		}

		if (!sink_tps4) {
			lt_dbg(intel_dp, dp_phy,
			       "8.1 Gbps link rate without sink TPS4 support\n");
		}
	}

	/*
	 * TPS3 support is mandatory for downstream devices that
	 * support HBR2. However, not all sinks follow the spec.
	 */
	source_tps3 = i915_dp_source_supports_tps3(i915);
	if (dp_phy != DP_PHY_DPRX) {
		sink_tps3 = true;
	} else {
		sink_tps3 = dp_tps3_supported(intel_dp->dpcd);
	}

	/* Uses TPS3 when both ends support it; notes an HBR2+ link without it. */
	if (source_tps3 && sink_tps3) {
		return DP_TRAINING_PATTERN_3;
	} else if (crtc_state->port_clock >= 540000) {
		if (!source_tps3) {
			lt_dbg(intel_dp, dp_phy,
			       ">=5.4/6.48 Gbps link rate without source TPS3 support\n");
		}

		if (!sink_tps3) {
			lt_dbg(intel_dp, dp_phy,
			       ">=5.4/6.48 Gbps link rate without sink TPS3 support\n");
		}
	}

	/* Succeeded: TPS2 is supported by every sink. */
	return DP_TRAINING_PATTERN_2;
}

/*
 * Performs the channel equalization phase of the link training on a PHY
 * using training pattern 2, 3 or 4 depending on the source and sink
 * capabilities (the Linux intel_dp_link_training_channel_equalization()).
 */
static bool
i915_dp_link_training_channel_equalization(
	struct intel_dp *intel_dp,
	const struct intel_crtc_state *crtc_state,
	enum dp_phy dp_phy)
{
	struct drm_i915_private *cur_i915;
	int tries;
	u32 training_pattern;
	u8 link_status[DP_LINK_STATUS_SIZE];
	bool channel_eq;
	int delay_us;
	bool uhbr;
	bool started;
	int status_error;
	bool recovered;
	bool equalized;
	bool updated;

	channel_eq = false;

	/* Finds the device whose sink the port reaches and sleeps on. */
	cur_i915 = i915_dp_cur_i915(&intel_dp->aux);

	/* Reads how long the sink needs between the steps. */
	uhbr = drv_i915_dp_is_uhbr(crtc_state);
	delay_us = i915_drm_dp_read_channel_eq_delay(&intel_dp->aux,
						     intel_dp->dpcd,
						     dp_phy,
						     uhbr);

	/* Scrambling is disabled for TPS2/3 and enabled for TPS4 */
	training_pattern = i915_dp_training_pattern(intel_dp, crtc_state, dp_phy);
	if (training_pattern != DP_TRAINING_PATTERN_4)
		training_pattern |= DP_LINK_SCRAMBLING_DISABLE;

	/* channel equalization */
	started = i915_dp_set_link_train(intel_dp, crtc_state, dp_phy, training_pattern);
	if (!started) {
		lt_err(intel_dp, dp_phy, "Failed to start channel equalization\n");
		return false;
	}

	/* Adjusts the levels until the sink equalizes the channel, at most five times. */
	for (tries = 0; tries < 5; tries++) {
		i915_lcd_usleep_range(cur_i915, delay_us, 2 * delay_us);

		/* Reads the sink's status. */
		status_error = drv_i915_drm_dp_dpcd_read_phy_link_status(&intel_dp->aux,
									  dp_phy,
									  link_status);
		if (status_error != 0) {
			lt_err(intel_dp, dp_phy, "Failed to get link status\n");
			break;
		}

		/* Make sure clock is still ok */
		recovered = drv_i915_drm_dp_clock_recovery_ok(link_status, crtc_state->lane_count);
		if (!recovered) {
			i915_dp_dump_link_status(intel_dp, dp_phy, link_status);
			lt_dbg(intel_dp, dp_phy,
			       "Clock recovery check failed, cannot continue channel equalization\n");
			break;
		}

		/* An equalized channel ends the training. */
		equalized = drv_i915_drm_dp_channel_eq_ok(link_status, crtc_state->lane_count);
		if (equalized) {
			channel_eq = true;
			lt_dbg(intel_dp, dp_phy, "Channel EQ done. DP Training successful\n");
			break;
		}

		/* Update training set as requested by target */
		i915_dp_get_adjust_train(intel_dp, crtc_state, dp_phy, link_status);
		updated = i915_dp_update_link_train(intel_dp, crtc_state, dp_phy);
		if (!updated) {
			lt_err(intel_dp, dp_phy, "Failed to update link training\n");
			break;
		}
	}

	/* Try 5 times, else fail and try at lower BW */
	if (tries == 5) {
		i915_dp_dump_link_status(intel_dp, dp_phy, link_status);
		lt_dbg(intel_dp, dp_phy, "Channel equalization failed 5 times\n");
	}

	/* Reports a channel that was not equalized. */
	if (!channel_eq)
		return false;

	/* Succeeded: the channel is equalized. */
	return true;
}

/*
 * Takes a PHY out of the training pattern in the sink's DPCD (the Linux
 * intel_dp_disable_dpcd_training_pattern()); true when the sink took it.
 */
static bool
i915_dp_disable_dpcd_training_pattern(
	struct intel_dp *intel_dp,
	enum dp_phy dp_phy)
{
	struct drm_i915_private *cur_i915;
	i915_lcd_ssize_t written;
	int reg;
	u8 val;

	/* Writes the disabled pattern to the PHY's register. */
	reg = i915_dp_training_pattern_set_reg(intel_dp, dp_phy);
	val = DP_TRAINING_PATTERN_DISABLE;
	cur_i915 = i915_dp_cur_i915(&intel_dp->aux);
	written = I915_LCD_DRM_DP_DPCD_WRITE(cur_i915, &intel_dp->aux, reg, &val, 1);
	if (written != 1)
		return false;

	/* Succeeded: the sink left the training pattern. */
	return true;
}

/* Trains one PHY: clock recovery, then channel equalization (the Linux intel_dp_link_train_phy()). */
static bool
i915_dp_link_train_phy(
	struct intel_dp *intel_dp,
	const struct intel_crtc_state *crtc_state,
	enum dp_phy dp_phy)
{
	bool passed;
	bool recovered;
	bool equalized;

	passed = false;

	/* Equalizes the channel once the clock is recovered. */
	recovered = i915_dp_link_training_clock_recovery(intel_dp, crtc_state, dp_phy);
	if (recovered) {
		equalized = i915_dp_link_training_channel_equalization(intel_dp, crtc_state, dp_phy);
		if (equalized)
			passed = true;
	}

	/* Notes the outcome. */
	lt_dbg(intel_dp, dp_phy,
	       "Link Training %s at link rate = %d, lane count = %d\n",
	       passed ? "passed" : "failed",
	       crtc_state->port_clock, crtc_state->lane_count);

	/* Reports a failed training. */
	if (!passed)
		return false;

	/* Succeeded: the PHY is trained. */
	return true;
}

/*
 * Asks for a retraining with reduced link parameters after a failed
 * training (the Linux intel_dp_schedule_fallback_link_training()).
 *
 * The fallback is not ported: the environment records the request as an
 * error and answers that no fallback values exist, and the modeset-retry
 * work is a named step on the current device.
 */
static void
i915_dp_schedule_fallback_link_training(
	struct intel_dp *intel_dp,
	const struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *cur_i915;
	bool connected;
	int fallback;

	/* The fallback values of the environment do not read the link parameters. */
	UNUSED_PARAMETER(crtc_state);

	/* A disconnected sink is not retrained. */
	connected = i915_lcd_intel_digital_port_connected(&i915_lcd_dp_to_dig_port(intel_dp)->base);
	if (!connected) {
		lt_dbg(intel_dp, DP_PHY_DPRX, "Link Training failed on disconnected sink.\n");
		return;
	}

	/* HOBL is given up first; otherwise lower link parameters are looked for. */
	if (intel_dp->hobl_active) {
		lt_dbg(intel_dp, DP_PHY_DPRX,
		       "Link Training failed with HOBL active, not enabling it from now on\n");

		/*
		 * hobl_failed keeps the buffer translation from using HOBL on
		 * the next training.
		 */
		intel_dp->hobl_failed = true;
	} else {
		fallback = intel_dp_get_link_train_fallback_values(intel_dp,
								   crtc_state->port_clock,
								   crtc_state->lane_count);
		if (fallback)
			return;
	}

	/*
	 * Schedule a Hotplug Uevent to userspace to start modeset.  The queue
	 * and the work are not evaluated: the step is recorded on the device.
	 */
	cur_i915 = i915_dp_cur_i915(&intel_dp->aux);
	I915_LCD_QUEUE_WORK(cur_i915, NULL, &intel_dp->attached_connector->modeset_retry_work);
}

/* Performs the link training on all LTTPRs and the DPRX on a link (the Linux intel_dp_link_train_all_phys()). */
static bool
i915_dp_link_train_all_phys(
	struct intel_dp *intel_dp,
	const struct intel_crtc_state *crtc_state,
	int lttpr_count)
{
	void (*set_idle_link_train)(struct intel_dp *intel_dp, const struct intel_crtc_state *crtc_state);
	enum dp_phy dp_phy;
	bool passed;
	int i;

	passed = true;

	/* Trains the LTTPRs from the one nearest the sink to the one nearest the source. */
	for (i = lttpr_count - 1; i >= 0; i--) {
		dp_phy = DP_PHY_LTTPR(i);

		passed = i915_dp_link_train_phy(intel_dp, crtc_state, dp_phy);
		i915_dp_disable_dpcd_training_pattern(intel_dp, dp_phy);

		if (!passed)
			break;
	}

	/* Trains the DPRX once every LTTPR is trained. */
	if (passed)
		passed = i915_dp_link_train_phy(intel_dp, crtc_state, DP_PHY_DPRX);

	/* Lets the port send the idle pattern. */
	set_idle_link_train = intel_dp->set_idle_link_train;
	if (set_idle_link_train)
		set_idle_link_train(intel_dp, crtc_state);

	/* Reports a failed training. */
	if (!passed)
		return false;

	/* Succeeded: every PHY of the link is trained. */
	return true;
}

/* The index of a rate in a rate table, or -1 if not found (the Linux intel_dp_rate_index()). */
static int
i915_dp_rate_index(
	const int *rates,
	int len,
	int rate)
{
	int i;

	/* Looks the rate up. */
	for (i = 0; i < len; i++) {
		if (rate == rates[i])
			return i;
	}

	/* Succeeded: the rate is not in the table. */
	return -1;
}

/* Tells whether the source supports TPS3 (the Linux intel_dp_source_supports_tps3()). */
static bool
i915_dp_source_supports_tps3(
	struct drm_i915_private *i915)
{
	int display_ver;

	/* The display version and platform are the probed device's. */
	UNUSED_PARAMETER(i915);

	/* Display version 9 and later support it. */
	display_ver = I915_LCD_DISPLAY_VER(i915);
	if (display_ver >= 9)
		return true;

	/* So do Broadwell and Haswell. */
	if (IS_BROADWELL(i915))
		return true;
	if (IS_HASWELL(i915))
		return true;

	/* Succeeded: the source has no TPS3. */
	return false;
}

/* Tells whether the source supports TPS4 (the Linux intel_dp_source_supports_tps4()). */
static bool
i915_dp_source_supports_tps4(
	struct drm_i915_private *i915)
{
	int display_ver;

	/* The display version is the probed device's. */
	UNUSED_PARAMETER(i915);

	/* Display version 10 and later support it. */
	display_ver = I915_LCD_DISPLAY_VER(i915);
	if (display_ver >= 10)
		return true;

	/* Succeeded: the source has no TPS4. */
	return false;
}

/*
 * The index of a rate in the sink's rate table for DP_LINK_RATE_SET (the
 * Linux intel_dp_rate_select()); a rate the sink does not list is reported
 * and index 0 is used.
 */
static int
i915_dp_rate_select(
	struct intel_dp *intel_dp,
	int rate)
{
	int i;
	int warned;

	/* Looks the rate up in the sink's table. */
	i = i915_dp_rate_index(intel_dp->sink_rates,
			       intel_dp->num_sink_rates,
			       rate);
	warned = I915_LCD_DRM_WARN_ON(NULL, i < 0);
	if (warned)
		i = 0;

	/* Succeeded: reports the index. */
	return i;
}

/*
 * Computes how a port clock is set in the sink (the Linux
 * intel_dp_compute_rate()): a DP_LINK_BW_SET code, or with link_bw 0 an
 * index for DP_LINK_RATE_SET.
 */
static void
i915_dp_compute_rate(
	struct intel_dp *intel_dp,
	int port_clock,
	u8 *link_bw,
	u8 *rate_select)
{
	/* FIXME g4x can't generate an exact 2.7GHz with the 96MHz non-SSC refclk */
	if (IS_G4X(NULL) && port_clock == 268800)
		port_clock = 270000;

	/* eDP 1.4 rate select method. */
	if (intel_dp->use_rate_select) {
		*link_bw = 0;
		*rate_select = i915_dp_rate_select(intel_dp, port_clock);
	} else {
		*link_bw = i915_drm_dp_link_rate_to_bw_code(port_clock);
		*rate_select = 0;
	}
}

/*
 * Tells whether a sink must stay in D0 to signal downstream HPD (the Linux
 * downstream_hpd_needs_d0()).
 *
 * DPCD 1.2+ should support BRANCH_DEVICE_CTRL, and thus be capable of
 * signalling downstream hpd with a long pulse.  Whether or not that means
 * D3 is safe to use is not clear, but let's assume so until proven
 * otherwise.
 *
 * FIXME should really check all downstream ports...
 */
static bool
i915_downstream_hpd_needs_d0(
	struct intel_dp *intel_dp)
{
	bool branch;

	/* Only a DPCD 1.1 sink needs it. */
	if (intel_dp->dpcd[DP_DPCD_REV] != 0x11)
		return false;

	/* Only a branch device has downstream ports. */
	branch = dp_is_branch(intel_dp->dpcd);
	if (!branch)
		return false;

	/* Only a downstream port with HPD needs it. */
	if ((intel_dp->downstream_ports[0] & DP_DS_PORT_HPD) == 0)
		return false;

	/* Succeeded: the sink must stay in D0. */
	return true;
}

/*
 * Writes the source OUI to an eDP sink (the Linux
 * intel_edp_init_source_oui()); a careful write leaves an OUI that is
 * already set alone.
 */
static void
i915_edp_init_source_oui(
	struct intel_dp *intel_dp,
	bool careful)
{
	struct drm_i915_private *cur_i915;
	u8 oui[] = { 0x00, 0xaa, 0x01 };
	u8 buf[3];
	i915_lcd_ssize_t transferred;
	int differs;

	kern_memset(buf, 0, sizeof(buf));

	/* Finds the device whose sink the port reaches. */
	cur_i915 = i915_dp_cur_i915(&intel_dp->aux);

	/*
	 * During driver init, we want to be careful and avoid changing the source OUI if it's
	 * already set to what we want, so as to avoid clearing any state by accident
	 */
	if (careful) {
		transferred = I915_LCD_DRM_DP_DPCD_READ(cur_i915, &intel_dp->aux, DP_SOURCE_OUI, buf, sizeof(buf));
		if (transferred < 0)
			I915_LCD_DRM_ERR(NULL, "Failed to read source OUI\n");

		/* Assume the OUI was written now. */
		differs = kern_memcmp(oui, buf, sizeof(oui));
		if (differs == 0) {
			intel_dp->last_oui_write = I915_LCD_JIFFIES;
			return;
		}
	}

	/* Writes the OUI. */
	transferred = I915_LCD_DRM_DP_DPCD_WRITE(cur_i915, &intel_dp->aux, DP_SOURCE_OUI, oui, sizeof(oui));
	if (transferred < 0)
		I915_LCD_DRM_ERR(NULL, "Failed to write source OUI\n");

	/* Remembers when the OUI was written (the wait that reads it belongs to the PPS side). */
	intel_dp->last_oui_write = I915_LCD_JIFFIES;
}

/*
 * Tells whether a link rate is UHBR (the Linux drm_dp_is_uhbr_rate()); the
 * rate is in 10 kbit/s units.
 */
static bool
i915_drm_dp_is_uhbr_rate(
	int link_rate)
{
	/* The UHBR rates start at 10 Gbps. */
	if (link_rate < 1000000)
		return false;

	/* Succeeded: the rate is UHBR. */
	return true;
}

/*
 * The channel coding efficiency of a DP link in 1 ppm units (the Linux
 * drm_dp_bw_channel_coding_efficiency()): the 8b -> 10b or 128b -> 132b
 * conversion and, for 128b/132b, the link or PHY level control symbols
 * (LLCP, FEC, PHY sync, see DP Standard v2.1 3.5.2.18).  For 8b/10b the FEC
 * overhead is BW allocation specific.
 */
static int
i915_drm_dp_bw_channel_coding_efficiency(
	bool is_uhbr)
{
	/* 128b/132b carries 96.71%. */
	if (is_uhbr)
		return 967100;

	/*
	 * Succeeded: 8b/10b carries 80%.  Note that on 8b/10b MST the
	 * efficiency is only 78.75% due to the 1 out of 64 MTPH packet
	 * overhead, not accounted for here.
	 */
	return 800000;
}
