/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The DisplayPort link of the display 12 and 13 code (see xd-dp.h).
 */

#include "xd-dp.h"

#include "../intel/display-dpcd-xd.h"

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The 8b/10b rates a sink may offer by its rate code: RBR, HBR, HBR2,
 * HBR3; and those a display 11 to 13 source offers, which add the
 * intermediate eDP rates.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dp.c
 * (intel_dp_set_dpcd_sink_rates' dp_rates, icl_rates).
 */
static const uint32_t i915_xd_dp_sink_table[] = {
	162000U, 270000U, 540000U, 810000U
};
static const uint32_t i915_xd_dp_source_table[] = {
	162000U, 216000U, 270000U, 324000U, 432000U, 540000U, 648000U, 810000U
};

/*
 * The rate a link falls back to when the ends share nothing, and the
 * unit of an 8b/10b rate code.
 *
 * Values referenced: Linux v6.8.12 include/drm/display/drm_dp_helper.h
 * (drm_dp_bw_code_to_link_rate), Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_dp.c (intel_dp_set_dpcd_sink_rates).
 */
#define I915_XD_DP_RATE_RBR		162000U
#define I915_XD_DP_CODE_UNIT		27000U

/*
 * The rate codes that name 128b/132b rates (10, 13.5 and 20 Gbit/s), and
 * where the 128b/132b rates begin.
 *
 * Values referenced: Linux v6.8.12 include/drm/display/drm_dp.h
 * (DP_LINK_BW_10, DP_LINK_BW_13_5, DP_LINK_BW_20), Linux v6.8.12
 * drivers/gpu/drm/display/drm_dp_helper.c (drm_dp_is_uhbr_rate).
 */
#define I915_XD_DP_CODE_UHBR10		0x01U
#define I915_XD_DP_CODE_UHBR20		0x02U
#define I915_XD_DP_CODE_UHBR13_5	0x04U
#define I915_XD_DP_RATE_UHBR10		1000000U
#define I915_XD_DP_RATE_UHBR13_5	1350000U
#define I915_XD_DP_RATE_UHBR20		2000000U

/*
 * The share of a link's bits that carries data: 80 % for 8b/10b, 96.71 %
 * for 128b/132b (parts per million); the bits of one symbol of each.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/display/drm_dp_helper.c
 * (drm_dp_bw_channel_coding_efficiency), Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_dp.c (intel_dp_link_symbol_size).
 */
#define I915_XD_DP_EFFICIENCY_8B10B	800000U
#define I915_XD_DP_EFFICIENCY_128B132B	967100U
#define I915_XD_DP_SYMBOL_8B10B		10U
#define I915_XD_DP_SYMBOL_128B132B	32U

/*
 * The M/N values: the transfer unit, the fixed N of the data and of the
 * link pair, and the width of the register fields both terms must fit.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_display.c
 * (intel_link_compute_m_n, compute_m_n, intel_reduce_m_n_ratio), Linux
 * v6.8.12 drivers/gpu/drm/i915/i915_reg.h (DATA_LINK_M_N_MASK).
 */
#define I915_XD_DP_TU			64U
#define I915_XD_DP_DATA_N		0x8000000U
#define I915_XD_DP_LINK_N		0x80000U
#define I915_XD_DP_M_N_MAX		0xffffffU

/*
 * The waits between the steps of a training: the defaults of clock
 * recovery and of channel equalization, the unit of a read interval,
 * the highest interval the standard names.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/display/drm_dp_helper.c
 * (__8b10b_clock_recovery_delay_us, __8b10b_channel_eq_delay_us,
 * __read_delay).
 */
#define I915_XD_DP_CR_WAIT_US		100U
#define I915_XD_DP_EQ_WAIT_US		400U
#define I915_XD_DP_INTERVAL_US		4000U
#define I915_XD_DP_INTERVAL_TOP		4U

/*
 * The tries of a training: clock recovery on a sink older than DPCD 1.4
 * and on a newer one, the same requests in a row, channel equalization.
 *
 * Values referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_dp_link_training.c
 * (intel_dp_link_training_clock_recovery,
 * intel_dp_link_training_channel_equalization).
 */
#define I915_XD_DP_CR_TRIES_OLD		80U
#define I915_XD_DP_CR_TRIES		10U
#define I915_XD_DP_SAME_TRIES		5U
#define I915_XD_DP_EQ_TRIES		5U

/* The receiver a training step talks to: the sink, or repeater 1..8. */
#define I915_XD_DP_HOP_SINK		0U

/* The bytes of the link status a repeater reports (it has no sink status byte). */
#define I915_XD_DP_REPEATER_STATUS_SIZE	5U

/*
 * The receiver capabilities a probe reads (DPCD 0x000 to 0x00e): the
 * revision, the highest rate, the lane count, whether a downstream port
 * is present, and how many ports a branch has.
 *
 * Values referenced: Linux v6.8.12 include/drm/display/drm_dp.h
 * (DP_DPCD_REV, DP_MAX_LINK_RATE, DP_MAX_LANE_COUNT, DP_MAX_LANE_COUNT_MASK,
 * DP_DOWNSTREAMPORT_PRESENT, DP_DOWN_STREAM_PORT_COUNT, DP_PORT_COUNT_MASK).
 */
#define I915_XD_DP_PROBE_DPCD_REV			0x000u
#define I915_XD_DP_PROBE_MAX_LINK_RATE		0x001u
#define I915_XD_DP_PROBE_MAX_LANE_COUNT		0x002u
#define I915_XD_DP_PROBE_MAX_LANE_COUNT_MASK		0x1fu
#define I915_XD_DP_PROBE_DOWNSTREAMPORT_PRESENT	0x005u
#define I915_XD_DP_PROBE_DOWN_STREAM_PORT_COUNT	0x007u
#define I915_XD_DP_PROBE_PORT_COUNT_MASK		0x0fu

/*
 * The downstream port byte: a branch, its port type before DPCD 1.1, and
 * whether it lists detailed port capabilities.
 *
 * Values referenced: Linux v6.8.12 include/drm/display/drm_dp.h
 * (DP_DWN_STRM_PORT_PRESENT, DP_DWN_STRM_PORT_TYPE_MASK, DP_DWN_STRM_PORT_TYPE_DP,
 * DP_DWN_STRM_PORT_TYPE_ANALOG, DP_DWN_STRM_PORT_TYPE_TMDS,
 * DP_DWN_STRM_PORT_TYPE_OTHER, DP_DETAILED_CAP_INFO_AVAILABLE).
 */
#define I915_XD_DP_PROBE_DWN_STRM_PORT_PRESENT	0x01u
#define I915_XD_DP_PROBE_DWN_STRM_PORT_TYPE_MASK	0x06u
#define I915_XD_DP_PROBE_DWN_STRM_PORT_TYPE_DP	0x00u
#define I915_XD_DP_PROBE_DWN_STRM_PORT_TYPE_ANALOG	0x02u
#define I915_XD_DP_PROBE_DWN_STRM_PORT_TYPE_TMDS	0x04u
#define I915_XD_DP_PROBE_DWN_STRM_PORT_TYPE_OTHER	0x06u
#define I915_XD_DP_PROBE_DETAILED_CAP_INFO_AVAILABLE	0x10u

/*
 * The DPCD revisions the probe tells apart.
 *
 * Values referenced: Linux v6.8.12 include/drm/display/drm_dp.h
 * (DP_DPCD_REV_10, DP_DPCD_REV_11, DP_DPCD_REV_13, DP_DPCD_REV_14).
 */
#define I915_XD_DP_PROBE_DPCD_REV_10			0x10u
#define I915_XD_DP_PROBE_DPCD_REV_11			0x11u
#define I915_XD_DP_PROBE_DPCD_REV_13			0x13u
#define I915_XD_DP_PROBE_DPCD_REV_14			0x14u

/*
 * The branch's downstream port capabilities (DPCD 0x080), 4 bytes a port
 * when detailed: the port type, its hot plug, its colour depth.
 *
 * Values referenced: Linux v6.8.12 include/drm/display/drm_dp.h
 * (DP_DOWNSTREAM_PORT_0, DP_DS_PORT_TYPE_MASK, DP_DS_PORT_TYPE_DP,
 * DP_DS_PORT_TYPE_VGA, DP_DS_PORT_TYPE_DVI, DP_DS_PORT_TYPE_HDMI,
 * DP_DS_PORT_TYPE_NON_EDID, DP_DS_PORT_TYPE_DP_DUALMODE, DP_DS_PORT_HPD,
 * DP_DS_MAX_BPC_MASK, DP_MAX_DOWNSTREAM_PORTS).
 */
#define I915_XD_DP_PROBE_DOWNSTREAM_PORT_0		0x080u
#define I915_XD_DP_PROBE_DS_PORT_TYPE_MASK		0x07u
#define I915_XD_DP_PROBE_DS_PORT_TYPE_DP		0u
#define I915_XD_DP_PROBE_DS_PORT_TYPE_VGA		1u
#define I915_XD_DP_PROBE_DS_PORT_TYPE_DVI		2u
#define I915_XD_DP_PROBE_DS_PORT_TYPE_HDMI		3u
#define I915_XD_DP_PROBE_DS_PORT_TYPE_NON_EDID	4u
#define I915_XD_DP_PROBE_DS_PORT_TYPE_DP_DUALMODE	5u
#define I915_XD_DP_PROBE_DS_PORT_HPD			0x08u
#define I915_XD_DP_PROBE_DS_MAX_BPC_MASK		0x03u
#define I915_XD_DP_PROBE_DETAILED_PORTS_MAX		4u
#define I915_XD_DP_PROBE_DETAILED_PORT_SIZE		4u

/*
 * The sink count (DPCD 0x200): bits 0..5 and bit 7 as bit 6.
 *
 * Values referenced: Linux v6.8.12 include/drm/display/drm_dp.h
 * (DP_SINK_COUNT, DP_GET_SINK_COUNT).
 */
#define I915_XD_DP_PROBE_SINK_COUNT			0x200u
#define I915_XD_DP_PROBE_SINK_COUNT_LOW_MASK		0x3fu
#define I915_XD_DP_PROBE_SINK_COUNT_HIGH_BIT		0x80u

/*
 * The sink's and the branch's identification (DPCD 0x400 and 0x500, 12
 * bytes).
 *
 * Values referenced: Linux v6.8.12 include/drm/display/drm_dp.h
 * (DP_SINK_OUI, DP_BRANCH_OUI).
 */
#define I915_XD_DP_PROBE_SINK_OUI			0x400u
#define I915_XD_DP_PROBE_BRANCH_OUI			0x500u
#define I915_XD_DP_PROBE_DESC_SIZE			12u

/*
 * The sink's service interrupt vectors, which a short pulse raises (DPCD
 * 0x201 and 0x2005).
 *
 * Values referenced: Linux v6.8.12 include/drm/display/drm_dp.h
 * (DP_DEVICE_SERVICE_IRQ_VECTOR, DP_LINK_SERVICE_IRQ_VECTOR_ESI0).
 */
#define I915_XD_DP_PROBE_DEVICE_SERVICE_IRQ_VECTOR	0x201u
#define I915_XD_DP_PROBE_LINK_SERVICE_IRQ_VECTOR_ESI0	0x2005u

/*
 * The DPRX's link status (DP_LANE0_1_STATUS onwards, 6 bytes): a 4-bit
 * field per lane, two lanes a byte (lane 0 in the low bits), whose clock
 * recovery, channel equalisation and symbol lock must all be done, and
 * the inter-lane alignment bit of DP_LANE_ALIGN_STATUS_UPDATED.
 *
 * Values referenced: Linux v6.8.12 include/drm/display/drm_dp.h
 * (DP_LANE0_1_STATUS, DP_LINK_STATUS_SIZE, DP_INTERLANE_ALIGN_DONE,
 * DP_CHANNEL_EQ_BITS).
 */
#define I915_XD_DP_PROBE_LANE0_1_STATUS		0x202u
#define I915_XD_DP_PROBE_LINK_STATUS_SIZE		6u
#define I915_XD_DP_PROBE_LANE_ALIGN_INDEX		2u
#define I915_XD_DP_PROBE_INTERLANE_ALIGN_DONE	0x01u
#define I915_XD_DP_PROBE_LANE_EQ_BITS		0x07u
#define I915_XD_DP_PROBE_LANE_STATUS_BITS		4u
#define I915_XD_DP_PROBE_LANES_MAX			4

/*
 * The protocol converter controls of a DPCD 1.3 branch (DPCD 0x3050 to
 * 0x3052).
 *
 * Values referenced: Linux v6.8.12 include/drm/display/drm_dp.h
 * (DP_PROTOCOL_CONVERTER_CONTROL_0, DP_PROTOCOL_CONVERTER_CONTROL_1,
 * DP_PROTOCOL_CONVERTER_CONTROL_2, DP_HDMI_DVI_OUTPUT_CONFIG,
 * DP_CONVERSION_RGB_YCBCR_MASK).
 */
#define I915_XD_DP_PROBE_CONVERTER_CONTROL_0		0x3050u
#define I915_XD_DP_PROBE_CONVERTER_CONTROL_1		0x3051u
#define I915_XD_DP_PROBE_CONVERTER_CONTROL_2		0x3052u
#define I915_XD_DP_PROBE_HDMI_DVI_OUTPUT_CONFIG	0x01u
#define I915_XD_DP_PROBE_CONVERSION_RGB_YCBCR_MASK	0x70u

/*
 * The repeaters' common capabilities (DPCD 0xf0000): revision, rate,
 * count, lanes.
 *
 * Values referenced: Linux v6.8.12 include/drm/display/drm_dp.h
 * (DP_LT_TUNABLE_PHY_REPEATER_FIELD_DATA_STRUCTURE_REV, DP_LTTPR_COMMON_CAP_SIZE).
 */
#define I915_XD_DP_PROBE_LTTPR_CAPS			0xf0000u
#define I915_XD_DP_PROBE_LTTPR_REV			0u
#define I915_XD_DP_PROBE_LTTPR_MAX_LINK_RATE		1u
#define I915_XD_DP_PROBE_LTTPR_COUNT			2u
#define I915_XD_DP_PROBE_LTTPR_MAX_LANE_COUNT	4u
#define I915_XD_DP_PROBE_LTTPR_MIN_REV		0x14u

/*
 * The EDID bytes the probe reads: the revision, the input definition, and
 * the extension tags.
 *
 * Values referenced: VESA Enhanced EDID Standard, release A revision 2
 * (the base block's version and video input definition);
 * CTA-861 (the extension tag and the vendor-specific data block).
 */
#define I915_XD_DP_PROBE_EDID_REVISION		19u
#define I915_XD_DP_PROBE_EDID_INPUT			20u
#define I915_XD_DP_PROBE_EDID_INPUT_DIGITAL		0x80u
#define I915_XD_DP_PROBE_EDID_DIGITAL_TYPE_MASK	0x0fu
#define I915_XD_DP_PROBE_EDID_DIGITAL_TYPE_DP	0x05u
#define I915_XD_DP_PROBE_EDID_CEA_TAG		0x02u
#define I915_XD_DP_PROBE_CEA_VENDOR_BLOCK		3u
#define I915_XD_DP_PROBE_HDMI_VSDB_MIN_LENGTH	5u

/* The negative error the probe reports for a short read (EIO). */
#define I915_XD_DP_PROBE_EIO				5

/*
 * The link rates a display 9 DDI drives (RBR, HBR and HBR2, all 8b/10b),
 * slowest first, in the 10 kbit/s a lane of the other rates here count,
 * and the colour depth its links are planned for (8 bits a channel).
 *
 * Values referenced: Intel Kaby Lake PRM Volume 12, Display Engine (the
 * DisplayPort link rates of the DDIs).
 */
static const uint32_t i915_xd9_dp_rates[3] = { 162000U, 270000U, 540000U };
#define I915_XD9_DP_BPC			8U
#define I915_XD9_DP_MAX_LANES		4U

/*
 * The sink's MAX_LINK_RATE codes of the three rates; a sink whose code is
 * HBR2's or above offers display 9 HBR2.
 *
 * Values referenced: VESA DisplayPort Standard 1.4 (DPCD MAX_LINK_RATE).
 */
#define I915_XD9_DP_CODE_RBR		0x06U
#define I915_XD9_DP_CODE_HBR		0x0aU
#define I915_XD9_DP_CODE_HBR2		0x14U

/* A single stream's data rate is the mode's own: a scale of one million millionths. */
#define I915_XD9_DP_SST_OVERHEAD	1000000U

static uint32_t i915_xd_dp_div_round_up(uint64_t numerator, uint64_t denominator);
static void i915_xd_dp_note(const struct i915_xd_dp_link *link, unsigned kind, const char *text);
static void i915_xd_dp_sleep(const struct i915_xd_dp_link *link, unsigned wait_us);
static void i915_xd_dp_ratio(uint32_t m, uint32_t n, uint32_t fixed_n, uint32_t *ret_m, uint32_t *ret_n);
static unsigned i915_xd_dp_lane_bits(const uint8_t *status, unsigned lane);
static uint8_t i915_xd_dp_request(const uint8_t *status, unsigned lane);
static int i915_xd_dp_repeater_count(const uint8_t *common);
static void i915_xd_dp_read_repeater_caps(struct i915_xd_dp_link *link, const uint8_t *caps, uint32_t address, uint8_t *buffer, unsigned size, int *error);
static unsigned i915_xd_dp_find_repeaters(struct i915_xd_dp_link *link, const uint8_t *caps);
static int i915_xd_dp_set_repeater_mode(struct i915_xd_dp_link *link, uint8_t mode);
static int i915_xd_dp_refresh_caps(struct i915_xd_dp_link *link);
static void i915_xd_dp_configure(struct i915_xd_dp_link *link);
static int i915_xd_dp_nearest(const struct i915_xd_dp_link *link, unsigned hop);
static unsigned i915_xd_dp_wait(struct i915_xd_dp_link *link, unsigned hop, int equalize);
static uint8_t i915_xd_dp_limit(const struct i915_xd_dp_link *link, unsigned hop, uint8_t swing, uint8_t preemph);
static void i915_xd_dp_follow(struct i915_xd_dp_link *link, unsigned hop, const uint8_t *status);
static int i915_xd_dp_send_pattern(struct i915_xd_dp_link *link, unsigned hop, uint8_t pattern);
static int i915_xd_dp_send_levels(struct i915_xd_dp_link *link, unsigned hop);
static int i915_xd_dp_requests_differ(const struct i915_xd_dp_link *link, const uint8_t *before, const uint8_t *now);
static int i915_xd_dp_at_top(const struct i915_xd_dp_link *link);
static int i915_xd_dp_recover_clock(struct i915_xd_dp_link *link, unsigned hop);
static uint8_t i915_xd_dp_eq_pattern(const struct i915_xd_dp_link *link, unsigned hop);
static int i915_xd_dp_equalize(struct i915_xd_dp_link *link, unsigned hop);
static void i915_xd_dp_end_hop(struct i915_xd_dp_link *link, unsigned hop);
static void i915_xd_dp_probe_read_lttpr(const struct i915_xd_dp_probe_env *env, struct i915_xd_dp_probe *sink);
static int i915_xd_dp_probe_lttpr_count(uint8_t count_bits);
static int i915_xd_dp_probe_read_caps(const struct i915_xd_dp_probe_env *env, struct i915_xd_dp_probe *sink);
static void i915_xd_dp_probe_read_desc(const struct i915_xd_dp_probe_env *env, struct i915_xd_dp_probe *sink);
static void i915_xd_dp_probe_set_rates(const struct i915_xd_dp_probe_env *env, const struct i915_xd_dp_probe_source *source, struct i915_xd_dp_probe *sink);
static void i915_xd_dp_probe_set_lanes(const struct i915_xd_dp_probe_env *env, const struct i915_xd_dp_probe_source *source, struct i915_xd_dp_probe *sink);
static int i915_xd_dp_probe_read_sink_count(const struct i915_xd_dp_probe_env *env, struct i915_xd_dp_probe *sink);
static int i915_xd_dp_probe_read_downstream(const struct i915_xd_dp_probe_env *env, struct i915_xd_dp_probe *sink);
static enum i915_xd_dp_probe_status i915_xd_dp_probe_classify(const struct i915_xd_dp_probe_env *env, struct i915_xd_dp_probe *sink);
static void i915_xd_dp_probe_read_edid(const struct i915_xd_dp_probe_env *env, struct i915_xd_dp_probe *sink);
static int i915_xd_dp_probe_edid_is_digital_dp(const struct i915_xd_dp_probe *sink);
static int i915_xd_dp_probe_edid_has_hdmi(const struct i915_xd_dp_probe *sink);
static int i915_xd_dp_probe_cea_block_has_hdmi(const uint8_t *block);
static void i915_xd_dp_probe_set_downstream_limits(struct i915_xd_dp_probe *sink);
static int i915_xd_dp_probe_downstream_max_tmds(const struct i915_xd_dp_probe *sink);
static int i915_xd_dp_probe_downstream_min_tmds(const struct i915_xd_dp_probe *sink);
static int i915_xd_dp_probe_downstream_max_dotclock(const struct i915_xd_dp_probe *sink);
static int i915_xd_dp_probe_downstream_max_bpc(const struct i915_xd_dp_probe *sink);
static int i915_xd_dp_probe_dpcd_writeb(const struct i915_xd_dp_probe_env *env, unsigned offset, uint8_t value);
static void i915_xd_dp_probe_ack_service_irq(const struct i915_xd_dp_probe_env *env, unsigned offset, const char *what);
static int i915_xd_dp_probe_first_error(int first_error, int error);
static int i915_xd_dp_probe_same_bytes(const uint8_t *a, const uint8_t *b, size_t size);
static void i915_xd_dp_probe_zero(void *bytes, size_t size);

/*
 * Converts a link rate to the code the sink's rate setting takes: the
 * code of a standard rate, otherwise the rate in units of 0.27 Gbit/s.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/display/drm_dp_helper.c
 * (drm_dp_link_rate_to_bw_code).
 */
uint8_t
drv_i915_xd_dp_rate_code(
	uint32_t rate_khz)
{
	/* The 128b/132b rates have codes of their own. */
	switch (rate_khz) {
	case I915_XD_DP_RATE_UHBR10:
		return (uint8_t)I915_XD_DP_CODE_UHBR10;
	case I915_XD_DP_RATE_UHBR13_5:
		return (uint8_t)I915_XD_DP_CODE_UHBR13_5;
	case I915_XD_DP_RATE_UHBR20:
		return (uint8_t)I915_XD_DP_CODE_UHBR20;
	default:
		break;
	}

	/* Succeeded: an 8b/10b rate counts units of the code. */
	return (uint8_t)(rate_khz / I915_XD_DP_CODE_UNIT);
}

/*
 * Converts a rate code the sink reports to its link rate.
 *
 * Values referenced: Linux v6.8.12 include/drm/display/drm_dp_helper.h
 * (drm_dp_bw_code_to_link_rate).
 */
uint32_t
drv_i915_xd_dp_code_rate(
	uint8_t code)
{
	/* Three codes name 128b/132b rates. */
	switch (code) {
	case I915_XD_DP_CODE_UHBR10:
		return I915_XD_DP_RATE_UHBR10;
	case I915_XD_DP_CODE_UHBR13_5:
		return I915_XD_DP_RATE_UHBR13_5;
	case I915_XD_DP_CODE_UHBR20:
		return I915_XD_DP_RATE_UHBR20;
	default:
		break;
	}

	/* Succeeded: every other code counts units of 0.27 Gbit/s. */
	return (uint32_t)code * I915_XD_DP_CODE_UNIT;
}

/*
 * Lists the rates each end of a link offers and those both share.
 *
 * The source offers its table up to the lower of the platform's and the
 * VBT's limit (0: none); the sink offers the standard rates up to the
 * rate of its code and, when repeaters name one, theirs.  A sink left
 * with nothing, or ends that meet nowhere, get RBR.  Returns the
 * I915_XD_DP_RATES_ bits of what had to be made up.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dp.c
 * (intel_dp_set_source_rates, intel_dp_set_dpcd_sink_rates,
 * intel_dp_set_common_rates).
 */
unsigned
drv_i915_xd_dp_rates(
	uint32_t source_max_khz,
	uint32_t vbt_max_khz,
	uint8_t sink_code,
	uint8_t repeater_code,
	struct i915_xd_dp_rates *rates)
{
	uint32_t source_limit;
	uint32_t sink_limit;
	uint32_t repeater_limit;
	unsigned index;
	unsigned source_index;
	unsigned sink_index;
	unsigned made_up;

	/* The source's limit: the platform's, and the VBT's when it is lower or the only one. */
	source_limit = source_max_khz;
	if (vbt_max_khz != 0U) {
		if (source_limit == 0U || vbt_max_khz < source_limit)
			source_limit = vbt_max_khz;
	}

	/* The source's rates up to its limit; the table rises, so the first rate above it ends the list. */
	rates->source_count = 0U;
	for (index = 0U; index < sizeof(i915_xd_dp_source_table) / sizeof(i915_xd_dp_source_table[0]); index++) {
		if (source_limit != 0U && i915_xd_dp_source_table[index] > source_limit)
			break;

		/* Keeps the rate. */
		rates->source[rates->source_count] = i915_xd_dp_source_table[index];
		rates->source_count++;
	}

	/* The sink's limit: its own code's rate, lowered by the repeaters' when they name one. */
	sink_limit = drv_i915_xd_dp_code_rate(sink_code);
	repeater_limit = drv_i915_xd_dp_code_rate(repeater_code);
	if (repeater_limit != 0U && repeater_limit < sink_limit)
		sink_limit = repeater_limit;

	/* The sink's standard rates up to its limit. */
	rates->sink_count = 0U;
	for (index = 0U; index < sizeof(i915_xd_dp_sink_table) / sizeof(i915_xd_dp_sink_table[0]); index++) {
		if (i915_xd_dp_sink_table[index] > sink_limit)
			break;

		/* Keeps the rate. */
		rates->sink[rates->sink_count] = i915_xd_dp_sink_table[index];
		rates->sink_count++;
	}

	/* A sink that offers nothing is given RBR. */
	made_up = 0U;
	if (rates->sink_count == 0U) {
		rates->sink[0] = I915_XD_DP_RATE_RBR;
		rates->sink_count = 1U;
		made_up |= I915_XD_DP_RATES_SINK_NONE;
	}

	/* The shared rates: both lists rise, so the list with the lower rate moves on until they meet. */
	rates->common_count = 0U;
	source_index = 0U;
	sink_index = 0U;
	while (source_index < rates->source_count && sink_index < rates->sink_count) {
		if (rates->source[source_index] == rates->sink[sink_index]) {
			rates->common[rates->common_count] = rates->source[source_index];
			rates->common_count++;
			source_index++;
			sink_index++;
		} else if (rates->source[source_index] < rates->sink[sink_index]) {
			source_index++;
		} else {
			sink_index++;
		}
	}

	/* Ends that meet nowhere share RBR. */
	if (rates->common_count == 0U) {
		rates->common[0] = I915_XD_DP_RATE_RBR;
		rates->common_count = 1U;
		made_up |= I915_XD_DP_RATES_SHARED_NONE;
	}

	/* Succeeded: what had to be made up. */
	return made_up;
}

/*
 * Reads the link rates an eDP 1.4 panel lists in its DPCD (16 bytes from
 * I915_XD_DPCD_EDP_RATES: entries of 200 kHz units, little endian, up to
 * the first zero) into rates, in kHz of the port clock.  Returns how
 * many it stored (0: the panel lists none).
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dp.c
 * (intel_edp_set_sink_rates).
 */
unsigned
drv_i915_xd_dp_edp_rates(
	const uint8_t *table,
	uint32_t *rates,
	unsigned room)
{
	unsigned count;
	unsigned index;
	uint32_t entry;

	/* Takes each entry up to the first zero or the room given. */
	count = 0U;
	for (index = 0U; index < I915_XD_DPCD_EDP_RATE_COUNT; index++) {
		entry = (uint32_t)table[2U * index] | ((uint32_t)table[2U * index + 1U] << 8);
		if (entry == 0U)
			break;
		if (count == room)
			break;

		/* An entry counts 200 kbit/s of a lane; the port clock counts 10 kbit/s. */
		rates[count] = entry * 200U / 10U;
		count++;
	}

	/* Succeeded: reports how many rates were listed. */
	return count;
}

/*
 * Finds the limits of the next try after a link failed to train at a rate
 * and lane count: the next lower shared rate with the same lanes, or
 * below the lowest rate (or at a rate not shared) half the lanes at the
 * highest rate.  Returns 1 with the new limits, or 0 when nothing lower
 * is left.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dp.c
 * (intel_dp_get_link_train_fallback_values).
 */
int
drv_i915_xd_dp_fallback(
	const uint32_t *common,
	unsigned count,
	uint32_t rate_khz,
	unsigned lanes,
	uint32_t *max_rate_khz,
	unsigned *max_lanes)
{
	unsigned index;
	int found;

	/* Finds the failed rate among the shared ones. */
	found = -1;
	for (index = 0U; index < count; index++) {
		if (common[index] == rate_khz)
			found = (int)index;
	}

	/* A rate above the lowest falls to the next lower one. */
	if (found > 0) {
		*max_rate_khz = common[found - 1];
		*max_lanes = lanes;
		return 1;
	}

	/* The lowest rate: half the lanes at the highest rate. */
	if (lanes > 1U && count > 0U) {
		*max_rate_khz = common[count - 1U];
		*max_lanes = lanes >> 1;
		return 1;
	}

	/* Succeeded: one lane at the lowest rate leaves nothing to try. */
	return 0;
}

/*
 * Tells the data rate a mode needs (kB/s): the pixel clock times the bits
 * per pixel, in bytes, rounded up.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dp.c
 * (intel_dp_link_required).
 */
uint32_t
drv_i915_xd_dp_need_kbps(
	uint32_t clock_khz,
	unsigned bpp)
{
	/* Succeeded: the bits of a second of pixels, in bytes. */
	return i915_xd_dp_div_round_up((uint64_t)clock_khz * bpp, 8U);
}

/*
 * Tells the data rate a link carries (kB/s): the bit rate of its lanes
 * times the share of data in its coding, in bytes, rounded down.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dp.c
 * (intel_dp_max_data_rate).
 */
uint32_t
drv_i915_xd_dp_carry_kbps(
	uint32_t rate_khz,
	unsigned lanes)
{
	uint64_t bits;
	uint32_t efficiency;

	/* The share of data: 8b/10b below the 128b/132b rates. */
	efficiency = I915_XD_DP_EFFICIENCY_8B10B;
	if (rate_khz >= I915_XD_DP_RATE_UHBR10)
		efficiency = I915_XD_DP_EFFICIENCY_128B132B;

	/* The rate counts 10 kbit/s per lane. */
	bits = (uint64_t)rate_khz * 10U * lanes * efficiency;

	/* Succeeded: the data share of the bits, in bytes. */
	return (uint32_t)(bits / (1000000U * 8U));
}

/*
 * Tells the data rate of a stream with its allocation overhead (kB/s):
 * the pixel clock times the bits per pixel in sixteenths, scaled by the
 * overhead in parts per million, rounded up.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dp.c
 * (intel_dp_effective_data_rate).
 */
uint32_t
drv_i915_xd_dp_stream_kbps(
	uint32_t clock_khz,
	unsigned bpp_x16,
	uint32_t overhead_ppm)
{
	uint64_t bits;

	/* The pixel bits of a second, scaled by the overhead. */
	bits = (uint64_t)((uint32_t)(clock_khz * bpp_x16)) * overhead_ppm;

	/* Succeeded: in bytes, without the sixteenths and the millionths. */
	return i915_xd_dp_div_round_up(bits, 1000000ULL * 16U * 8U);
}

/*
 * Tells the bits of one symbol of a link rate's coding: 10 for 8b/10b,
 * 32 for 128b/132b.
 */
unsigned
drv_i915_xd_dp_symbol_bits(
	uint32_t rate_khz)
{
	/* A 128b/132b rate. */
	if (rate_khz >= I915_XD_DP_RATE_UHBR10)
		return I915_XD_DP_SYMBOL_128B132B;

	/* Succeeded: an 8b/10b rate. */
	return I915_XD_DP_SYMBOL_8B10B;
}

/*
 * Tells the symbol clock of a link rate in kHz, rounded to the nearest.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dp.c
 * (intel_dp_link_symbol_clock).
 */
uint32_t
drv_i915_xd_dp_symbol_khz(
	uint32_t rate_khz)
{
	unsigned bits;

	/* The bits of one symbol. */
	bits = drv_i915_xd_dp_symbol_bits(rate_khz);

	/* Succeeded: the rate counts 10 kbit/s; the symbols of that. */
	return (rate_khz * 10U + bits / 2U) / bits;
}

/*
 * Chooses the link that carries a mode: for each depth from max_bpc down
 * to min_bpc in steps of 2, each shared rate from the lowest (up to the
 * rate limit, 0: none), and 1, 2 then 4 lanes (up to the lane limit);
 * the first whose data rate holds the mode's.  Returns 0 with the
 * choice, or ENOSPC when no link carries the mode.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dp.c
 * (intel_dp_compute_link_config_wide).
 */
int
drv_i915_xd_dp_choose(
	uint32_t clock_khz,
	unsigned max_bpc,
	unsigned min_bpc,
	const uint32_t *common,
	unsigned count,
	uint32_t rate_limit_khz,
	unsigned lane_limit,
	struct i915_xd_dp_choice *choice)
{
	unsigned bpc;
	unsigned index;
	unsigned lanes;
	uint32_t need;
	uint32_t carry;

	/* Tries each depth from the deepest. */
	for (bpc = max_bpc; bpc >= min_bpc && bpc != 0U; bpc -= 2U) {
		need = drv_i915_xd_dp_need_kbps(clock_khz, 3U * bpc);

		/* Tries each shared rate from the lowest, under the limit. */
		for (index = 0U; index < count; index++) {
			if (rate_limit_khz != 0U && common[index] > rate_limit_khz)
				continue;

			/* Tries 1, 2 and 4 lanes. */
			for (lanes = 1U; lanes <= lane_limit; lanes <<= 1) {
				carry = drv_i915_xd_dp_carry_kbps(common[index], lanes);
				if (need > carry)
					continue;

				/* The first link that carries the mode. */
				choice->rate_khz = common[index];
				choice->lanes = lanes;
				choice->bpp = 3U * bpc;
				choice->need_kbps = need;
				choice->carry_kbps = carry;
				return 0;
			}
		}

		/* The depth 1 would wrap below 0. */
		if (bpc < 2U)
			break;
	}

	/* No link carries the mode. */
	return ENOSPC;
}

/*
 * Computes the M/N values of a stream on a link: the data pair is the
 * stream's rate (with its overhead) over the link's payload rate with a
 * fixed N, the link pair the pixel clock over the link's symbol clock
 * with a fixed N; both are halved until they fit the register fields.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_display.c
 * (intel_link_compute_m_n, compute_m_n).
 */
void
drv_i915_xd_dp_m_n(
	unsigned bpp_x16,
	unsigned lanes,
	uint32_t clock_khz,
	uint32_t rate_khz,
	uint32_t overhead_ppm,
	struct i915_xd_dp_m_n *m_n)
{
	uint32_t stream;
	uint32_t payload;
	uint32_t symbol;

	/* The stream's rate, the link's payload rate and its symbol clock. */
	stream = drv_i915_xd_dp_stream_kbps(clock_khz, bpp_x16, overhead_ppm);
	payload = drv_i915_xd_dp_carry_kbps(rate_khz, lanes);
	symbol = drv_i915_xd_dp_symbol_khz(rate_khz);

	/* The transfer unit and the two pairs. */
	m_n->tu = I915_XD_DP_TU;
	i915_xd_dp_ratio(stream, payload, I915_XD_DP_DATA_N, &m_n->data_m, &m_n->data_n);
	i915_xd_dp_ratio(clock_khz, symbol, I915_XD_DP_LINK_N, &m_n->link_m, &m_n->link_n);
}

/*
 * Tells whether every lane in use of a link status has recovered the
 * clock.  Returns 1 or 0.
 */
int
drv_i915_xd_dp_clock_recovered(
	const uint8_t *status,
	unsigned lanes)
{
	unsigned lane;
	unsigned bits;

	/* Looks for a lane in use without the clock. */
	for (lane = 0U; lane < lanes; lane++) {
		bits = i915_xd_dp_lane_bits(status, lane);
		if ((bits & I915_XD_DPCD_LANE_CR) == 0U)
			return 0;
	}

	/* Succeeded: every lane in use has the clock. */
	return 1;
}

/*
 * Tells whether a link status shows the channel equalized: the lanes
 * aligned, and every lane in use with the clock, equalized and its
 * symbols locked.  Returns 1 or 0.
 */
int
drv_i915_xd_dp_equalized(
	const uint8_t *status,
	unsigned lanes)
{
	unsigned lane;
	unsigned bits;
	unsigned all;

	/* The lanes must be aligned with each other. */
	if ((status[I915_XD_DPCD_ALIGN - I915_XD_DPCD_LANE_STATUS] & I915_XD_DPCD_ALIGN_DONE) == 0U)
		return 0;

	/* Looks for a lane in use that lacks one of the three. */
	all = I915_XD_DPCD_LANE_CR | I915_XD_DPCD_LANE_EQ | I915_XD_DPCD_LANE_LOCKED;
	for (lane = 0U; lane < lanes; lane++) {
		bits = i915_xd_dp_lane_bits(status, lane);
		if ((bits & all) != all)
			return 0;
	}

	/* Succeeded: the channel is equalized. */
	return 1;
}

/*
 * Reads the link status of the sink (hop 0) or of repeater hop into the
 * sink's six-byte layout; a repeater's status, which has no sink status
 * byte, gets a zero one.  A short read is used as read.  Returns 0, or
 * the positive error of the read.
 */
int
drv_i915_xd_dp_read_status(
	const struct i915_xd_dp_sink *sink,
	unsigned hop,
	uint8_t *status)
{
	int moved;

	/* The sink's status is read in its own layout. */
	if (hop == I915_XD_DP_HOP_SINK) {
		moved = sink->read(sink->context, I915_XD_DPCD_LANE_STATUS, status, I915_XD_DPCD_STATUS_SIZE);
		if (moved < 0)
			return -moved;

		/* Succeeded: the transfer is done. */
		return 0;
	}

	/* A repeater's five bytes lack the sink status; the requests move up one byte. */
	moved = sink->read(sink->context, I915_XD_DPCD_REPEATER_STATUS(hop), status, I915_XD_DP_REPEATER_STATUS_SIZE);
	if (moved < 0)
		return -moved;
	status[5] = status[4];
	status[4] = status[3];
	status[3] = 0U;

	/* Succeeded: the status is in the sink's layout. */
	return 0;
}

/*
 * Trains a link: finds the repeaters (when asked to), reads the sink's
 * capabilities again, lets the source prepare the port, writes the link's
 * rate, lanes and coding to the sink, trains each repeater and then the
 * sink, and lets the source send the idle pattern.  Returns 0 when every
 * receiver was trained, EIO otherwise (the caller decides on a fallback).
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_dp_link_training.c
 * (intel_dp_start_link_train, intel_dp_init_lttpr_and_dprx_caps,
 * intel_dp_prepare_link_train, intel_dp_link_train_all_phys,
 * intel_dp_link_train_phy).
 */
int
drv_i915_xd_dp_train(
	struct i915_xd_dp_link *link)
{
	unsigned hop;
	int trained;
	int error;

	/* The repeaters and the sink's capabilities; a sink that does not answer is trained without repeaters. */
	error = i915_xd_dp_refresh_caps(link);
	if (error != 0)
		link->repeaters = 0U;

	/* The port and the sink are set up for the link. */
	i915_xd_dp_configure(link);

	/* Trains the repeaters from the highest numbered one, each left without its pattern. */
	trained = 1;
	for (hop = link->repeaters; hop > I915_XD_DP_HOP_SINK; hop--) {
		trained = i915_xd_dp_recover_clock(link, hop);
		if (trained)
			trained = i915_xd_dp_equalize(link, hop);
		i915_xd_dp_end_hop(link, hop);
		if (!trained)
			break;
	}

	/* Trains the sink once every repeater is trained. */
	if (trained) {
		trained = i915_xd_dp_recover_clock(link, I915_XD_DP_HOP_SINK);
		if (trained)
			trained = i915_xd_dp_equalize(link, I915_XD_DP_HOP_SINK);
	}

	/* Notes the outcome. */
	if (trained) {
		i915_xd_dp_note(link, I915_XD_DP_NOTE_DEBUG, "xd-dp: the link is trained\n");
	} else {
		i915_xd_dp_note(link, I915_XD_DP_NOTE_DEBUG, "xd-dp: the link did not train\n");
	}

	/* The source sends the idle pattern. */
	if (link->source.idle != NULL)
		link->source.idle(link->source.context);

	/* Reports a link that did not train. */
	if (!trained)
		return EIO;

	/* Succeeded: every receiver of the link is trained. */
	return 0;
}

/*
 * Ends a training: the sink and then the port leave the training
 * pattern, and the port sends the stream's symbols from now on.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_dp_link_training.c
 * (intel_dp_stop_link_train).
 */
void
drv_i915_xd_dp_train_end(
	struct i915_xd_dp_link *link)
{
	/* The sink, then the port. */
	i915_xd_dp_end_hop(link, I915_XD_DP_HOP_SINK);
	link->source.pattern(link->source.context, (uint8_t)I915_XD_DPCD_PATTERN_NONE);
}

/*
 * Forgets what a port's sink was: the cache becomes that of a port with
 * nothing plugged in.
 */
void
drv_i915_xd_dp_probe_forget(
	struct i915_xd_dp_probe *sink)
{
	/* Every field's zero means "not known", and the status's zero is disconnected. */
	i915_xd_dp_probe_zero(sink, sizeof(*sink));
	sink->status = I915_XD_DP_PROBE_DISCONNECTED;
	sink->step = I915_XD_DP_PROBE_STEP_NONE;
}

/*
 * Probes the sink of an external DP port, after the live status said
 * something is plugged in.
 *
 * The receiver, the repeaters, the identification, the rates and lanes,
 * the sink count of a branch and its downstream ports are read; a branch
 * is judged by its sink count, its DDC or its port type; the EDID is read
 * last, and an EDID makes any sink connected.  The result replaces the
 * port's cache.  The caller holds the port's PHY for the whole probe.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_dp.c (intel_dp_detect,
 * intel_dp_get_dpcd, intel_dp_detect_dpcd, intel_dp_set_edid,
 * intel_dp_update_dfp).
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_dp_link_training.c
 * (intel_dp_init_lttpr_and_dprx_caps).
 */
void
drv_i915_xd_dp_probe_detect(
	const struct i915_xd_dp_probe_env *env,
	const struct i915_xd_dp_probe_source *source,
	struct i915_xd_dp_probe *sink)
{
	enum i915_xd_dp_probe_status status;
	int error;

	/* Starts from a sink that is not there. */
	drv_i915_xd_dp_probe_forget(sink);

	/* Reads the repeaters and the receiver. */
	error = i915_xd_dp_probe_read_caps(env, sink);
	if (error != 0) {
		sink->step = I915_XD_DP_PROBE_STEP_CAPS;
		sink->error = error;
		return;
	}

	/* A sink that names a downstream port is a branch device. */
	if ((sink->dpcd[I915_XD_DP_PROBE_DOWNSTREAMPORT_PRESENT] & I915_XD_DP_PROBE_DWN_STRM_PORT_PRESENT) != 0u)
		sink->branch = 1;

	/* Reads who the sink or branch is, then what both sides offer. */
	i915_xd_dp_probe_read_desc(env, sink);
	i915_xd_dp_probe_set_rates(env, source, sink);
	i915_xd_dp_probe_set_lanes(env, source, sink);

	/* A branch that counts its sinks must count one: an adapter with no display is disconnected. */
	error = i915_xd_dp_probe_read_sink_count(env, sink);
	if (error != 0) {
		sink->step = I915_XD_DP_PROBE_STEP_SINK_COUNT;
		sink->error = error;
		return;
	}

	/* Reads the branch's downstream ports. */
	error = i915_xd_dp_probe_read_downstream(env, sink);
	if (error != 0) {
		sink->step = I915_XD_DP_PROBE_STEP_DOWNSTREAM;
		sink->error = error;
		return;
	}

	/* Judges whether a display is there. */
	status = i915_xd_dp_probe_classify(env, sink);
	if (status == I915_XD_DP_PROBE_DISCONNECTED) {
		sink->step = I915_XD_DP_PROBE_STEP_BRANCH;
		return;
	}

	/* Reads the EDID; a display with an EDID is connected whatever the branch said. */
	i915_xd_dp_probe_read_edid(env, sink);
	if (sink->edid_blocks != 0u)
		status = I915_XD_DP_PROBE_CONNECTED;

	/* Takes the branch's limits and whether the display is HDMI. */
	i915_xd_dp_probe_set_downstream_limits(sink);
	sink->has_hdmi_sink = i915_xd_dp_probe_edid_has_hdmi(sink);

	/* Publishes the conclusion. */
	sink->status = status;
	sink->step = I915_XD_DP_PROBE_STEP_DONE;
}

/*
 * Handles a short pulse (IRQ_HPD) of a sink the last probe found, up to
 * its service interrupts.
 *
 * The receiver capabilities are read again, and a branch's sink count; a
 * read that fails, or capabilities or a count that changed, ask for a
 * full detection.  Otherwise the device and link service interrupts are
 * acknowledged and logged (their handling -- link retraining, HDCP, the
 * HDMI link status -- is not done here).  Returns 1 when the pulse is
 * handled, 0 when the port must be detected again.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_dp.c (intel_dp_short_pulse).
 */
int
drv_i915_xd_dp_probe_short_pulse(
	const struct i915_xd_dp_probe_env *env,
	const struct i915_xd_dp_probe *sink)
{
	uint8_t dpcd[I915_XD_DP_PROBE_DPCD_SIZE];
	uint8_t count;
	long read;
	int error;
	int same;
	int now;

	/* A sink the last probe did not find connected is detected afresh. */
	if (sink->status != I915_XD_DP_PROBE_CONNECTED)
		return 0;

	/* Reads the receiver capabilities again; a failure asks for a detection. */
	error = env->read_caps(env->ctx, dpcd);
	if (error != 0)
		return 0;

	/* Changed capabilities ask for a detection. */
	same = i915_xd_dp_probe_same_bytes(dpcd, sink->dpcd, sizeof(dpcd));
	if (!same)
		return 0;

	/* A branch's sink count is read again: a failure or another count asks for a detection. */
	if (sink->has_sink_count) {
		read = env->dpcd_read(env->ctx, I915_XD_DP_PROBE_SINK_COUNT, &count, 1u);
		if (read != 1)
			return 0;

		/* Bit 7 of the field is bit 6 of the count. */
		now = (int)(count & I915_XD_DP_PROBE_SINK_COUNT_LOW_MASK);
		if ((count & I915_XD_DP_PROBE_SINK_COUNT_HIGH_BIT) != 0u)
			now |= 0x40;

		/* Another count asks for a detection. */
		if (now != sink->sink_count)
			return 0;
	}

	/* Acknowledges the device and link service interrupts of a DPCD 1.1 sink. */
	if (sink->dpcd[I915_XD_DP_PROBE_DPCD_REV] >= I915_XD_DP_PROBE_DPCD_REV_11) {
		i915_xd_dp_probe_ack_service_irq(env, I915_XD_DP_PROBE_DEVICE_SERVICE_IRQ_VECTOR, "device");
		i915_xd_dp_probe_ack_service_irq(env, I915_XD_DP_PROBE_LINK_SERVICE_IRQ_VECTOR_ESI0, "link");
	}

	/* Succeeded: the sink is as the probe found it. */
	return 1;
}

/*
 * Tells whether a trained link must be trained again: the sink's link
 * status is read, and the link needs it when the lanes
 * lost their alignment or a lane of the link's lane_count lost clock
 * recovery, equalisation or symbol lock.  A status that cannot be read, or
 * a lane count out of 1 to 4, does not ask for it: a link is trained
 * again only on a status that was read.  Returns 1 or 0.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_dp.c (intel_dp_needs_link_retrain).
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/display/drm_dp_helper.c (drm_dp_channel_eq_ok).
 */
int
drv_i915_xd_dp_probe_link_needs_retrain(
	const struct i915_xd_dp_probe_env *env,
	int lane_count)
{
	uint8_t status[I915_XD_DP_PROBE_LINK_STATUS_SIZE];
	unsigned lane_bits;
	long read;
	int lane;

	/* Only a link of one to four lanes. */
	if (lane_count < 1 || lane_count > I915_XD_DP_PROBE_LANES_MAX)
		return 0;

	/* The status; one that cannot be read asks for nothing. */
	read = env->dpcd_read(env->ctx, I915_XD_DP_PROBE_LANE0_1_STATUS, status, sizeof(status));
	if (read != (long)sizeof(status))
		return 0;

	/* Lanes that lost their alignment. */
	if ((status[I915_XD_DP_PROBE_LANE_ALIGN_INDEX] & I915_XD_DP_PROBE_INTERLANE_ALIGN_DONE) == 0u)
		return 1;

	/* Each lane of the link: clock recovery, equalisation and symbol lock, all done. */
	for (lane = 0; lane < lane_count; lane++) {
		lane_bits = ((unsigned)status[lane / 2] >> ((unsigned)(lane % 2) * I915_XD_DP_PROBE_LANE_STATUS_BITS)) & I915_XD_DP_PROBE_LANE_EQ_BITS;
		if (lane_bits != I915_XD_DP_PROBE_LANE_EQ_BITS)
			return 1;
	}

	/* The link is as it was trained. */
	return 0;
}

/*
 * Sets up the protocol converter of a DPCD 1.3 branch for an RGB stream:
 * HDMI or DVI output by the display's EDID, no 4:2:0 conversion and no
 * RGB to YCbCr conversion.
 *
 * A sink that is no such branch is left alone.  Returns 0, or the
 * negative errno of the first write that failed; every write is tried,
 * a failure only being reported.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_dp.c
 * (intel_dp_configure_protocol_converter).
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/display/drm_dp_helper.c
 * (drm_dp_pcon_convert_rgb_to_ycbcr).
 */
int
drv_i915_xd_dp_probe_configure_converter(
	const struct i915_xd_dp_probe_env *env,
	const struct i915_xd_dp_probe *sink)
{
	uint8_t control;
	long read;
	int written;
	int first_error;

	/* A sink before DPCD 1.3 has no converter controls. */
	if (sink->dpcd[I915_XD_DP_PROBE_DPCD_REV] < I915_XD_DP_PROBE_DPCD_REV_13)
		return 0;

	/* A sink that is not a branch converts nothing. */
	if (!sink->branch)
		return 0;

	/* Selects HDMI output for an HDMI display, DVI otherwise. */
	first_error = 0;
	control = 0u;
	if (sink->has_hdmi_sink)
		control = I915_XD_DP_PROBE_HDMI_DVI_OUTPUT_CONFIG;

	/* Writes the selection; a failure is logged and kept. */
	written = i915_xd_dp_probe_dpcd_writeb(env, I915_XD_DP_PROBE_CONVERTER_CONTROL_0, control);
	if (written != 0) {
		env->log(env->ctx, "i915: DP-ext: protocol converter HDMI mode %d not set: rc %d\n", sink->has_hdmi_sink, written);
		first_error = i915_xd_dp_probe_first_error(first_error, written);
	}

	/* Turns the YCbCr 4:4:4 to 4:2:0 conversion off: the stream is RGB. */
	written = i915_xd_dp_probe_dpcd_writeb(env, I915_XD_DP_PROBE_CONVERTER_CONTROL_1, 0u);
	if (written != 0) {
		env->log(env->ctx, "i915: DP-ext: protocol converter 4:2:0 conversion not turned off: rc %d\n", written);
		first_error = i915_xd_dp_probe_first_error(first_error, written);
	}

	/* Turns the RGB to YCbCr conversion off, keeping the control's other bits. */
	read = env->dpcd_read(env->ctx, I915_XD_DP_PROBE_CONVERTER_CONTROL_2, &control, 1u);
	written = -I915_XD_DP_PROBE_EIO;
	if (read == 1) {
		control = (uint8_t)(control & ~I915_XD_DP_PROBE_CONVERSION_RGB_YCBCR_MASK);
		written = i915_xd_dp_probe_dpcd_writeb(env, I915_XD_DP_PROBE_CONVERTER_CONTROL_2, control);
	}

	/* Reports a control that could not be read or written back. */
	if (written != 0) {
		env->log(env->ctx, "i915: DP-ext: protocol converter RGB to YCbCr conversion not turned off: read %ld, rc %d\n", read, written);
		first_error = i915_xd_dp_probe_first_error(first_error, written);
	}

	/* Reports the first write that failed. */
	if (first_error != 0)
		return first_error;

	/* Succeeded: the converter passes RGB through to the display's interface. */
	return 0;
}

/*
 * Computes the link limits after a link training failed at a rate and a
 * lane count, through the xd DisplayPort code: a rate above the lowest
 * the source and sink share falls to the next lower shared rate with the
 * same lanes; otherwise the lanes are halved at the highest shared rate.
 * Returns 1 with *max_rate and *max_lanes the new limits, or 0 when one
 * lane is left (nothing lower to try).
 */
int
drv_i915_xd_dp_probe_fallback_values(
	const struct i915_xd_dp_probe *sink,
	int rate,
	int lanes,
	int *max_rate,
	int *max_lanes)
{
	uint32_t common[I915_XD_DP_RATES];
	uint32_t next_rate;
	unsigned next_lanes;
	unsigned count;
	unsigned index;
	int lower;

	/* The shared rates, as the xd link takes them. */
	count = 0;
	for (index = 0; index < (unsigned)sink->num_common_rates && index < I915_XD_DP_RATES; index++) {
		common[index] = (uint32_t)sink->common_rates[index];
		count++;
	}

	/* The next try's limits. */
	lower = drv_i915_xd_dp_fallback(common, count, (uint32_t)rate, (unsigned)lanes, &next_rate, &next_lanes);
	if (!lower)
		return 0;

	/* Succeeded: the limits of the next try. */
	*max_rate = (int)next_rate;
	*max_lanes = (int)next_lanes;
	return 1;
}

/*
 * Checks a mode's pixel clock against a branch's downstream limits, for
 * RGB at 8 bits per colour, where the TMDS clock is the pixel clock.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_dp.c
 * (intel_dp_mode_valid_downstream).
 */
enum i915_xd_dp_probe_mode_status
drv_i915_xd_dp_probe_mode_valid(
	const struct i915_xd_dp_probe *sink,
	int clock_khz)
{
	/* A clock above the branch's dot clock limit. */
	if (sink->max_dotclock_khz != 0 && clock_khz > sink->max_dotclock_khz)
		return I915_XD_DP_PROBE_MODE_CLOCK_HIGH;

	/* A TMDS clock below what the branch's HDMI or DVI port sends. */
	if (sink->min_tmds_clock_khz != 0 && clock_khz < sink->min_tmds_clock_khz)
		return I915_XD_DP_PROBE_MODE_CLOCK_LOW;

	/* A TMDS clock above what the branch's HDMI or DVI port sends. */
	if (sink->max_tmds_clock_khz != 0 && clock_khz > sink->max_tmds_clock_khz)
		return I915_XD_DP_PROBE_MODE_CLOCK_HIGH;

	/* Succeeded: the branch passes the mode. */
	return I915_XD_DP_PROBE_MODE_OK;
}

/*
 * Names a probe's conclusion for the log.
 */
const char *
drv_i915_xd_dp_probe_status_name(
	enum i915_xd_dp_probe_status status)
{
	/* Names the conclusion. */
	switch (status) {
	case I915_XD_DP_PROBE_DISCONNECTED:
		return "disconnected";
	case I915_XD_DP_PROBE_CONNECTED:
		return "connected";
	case I915_XD_DP_PROBE_UNKNOWN:
		return "unknown";
	default:
		break;
	}

	/* Reports a value no conclusion has. */
	return "invalid";
}

/*
 * Names the step a probe stopped at for the log.
 */
const char *
drv_i915_xd_dp_probe_step_name(
	enum i915_xd_dp_probe_step step)
{
	/* Names the step. */
	switch (step) {
	case I915_XD_DP_PROBE_STEP_NONE:
		return "none";
	case I915_XD_DP_PROBE_STEP_PHY:
		return "PHY";
	case I915_XD_DP_PROBE_STEP_CAPS:
		return "receiver capabilities";
	case I915_XD_DP_PROBE_STEP_SINK_COUNT:
		return "sink count";
	case I915_XD_DP_PROBE_STEP_DOWNSTREAM:
		return "downstream ports";
	case I915_XD_DP_PROBE_STEP_BRANCH:
		return "branch device";
	case I915_XD_DP_PROBE_STEP_DONE:
		return "done";
	default:
		break;
	}

	/* Reports a value no step has. */
	return "invalid";
}

/*
 * Logs what a port's last probe found: the conclusion, the receiver, the
 * repeaters, the identification, the link, the branch and the EDID.
 */
void
drv_i915_xd_dp_probe_log(
	const struct i915_xd_dp_probe_env *env,
	const struct i915_xd_dp_probe *sink,
	const char *name)
{
	const uint8_t *d;
	const uint8_t *e;
	char id[I915_XD_DP_PROBE_DEVICE_ID_SIZE + 1u];
	unsigned i;
	int sink_top;
	int source_top;

	/* The conclusion and where the probe stopped. */
	env->log(env->ctx, "i915: DP-ext %s: %s (step %s, rc %d)\n",
		 name,
		 drv_i915_xd_dp_probe_status_name(sink->status),
		 drv_i915_xd_dp_probe_step_name(sink->step),
		 sink->error);

	/* Nothing more to tell without the receiver capabilities. */
	if (!sink->dpcd_valid)
		return;

	/* The receiver capabilities, byte by byte. */
	d = sink->dpcd;
	env->log(env->ctx, "i915: DP-ext %s DPCD 000: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
		 name, d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], d[8], d[9], d[10], d[11], d[12], d[13], d[14]);

	/* The identification string, its unprintable bytes as spaces. */
	for (i = 0u; i < I915_XD_DP_PROBE_DEVICE_ID_SIZE; i++) {
		id[i] = sink->desc.device_id[i];
		if (id[i] < 0x20 || id[i] > 0x7e)
			id[i] = ' ';
	}

	/* Ends the string. */
	id[I915_XD_DP_PROBE_DEVICE_ID_SIZE] = '\0';

	/* The highest rate each side offers. */
	sink_top = 0;
	if (sink->num_sink_rates != 0)
		sink_top = sink->sink_rates[sink->num_sink_rates - 1];
	source_top = 0;
	if (sink->num_source_rates != 0)
		source_top = sink->source_rates[sink->num_source_rates - 1];

	/* The repeaters, the identification, and the link both sides share. */
	env->log(env->ctx, "i915: DP-ext %s: LTTPR valid %d count %d | %s OUI %02x%02x%02x id '%s' hw %u.%u sw %u.%u (valid %d) | rate max %d (sink %d, source %d) lanes %d (sink %d)\n",
		 name,
		 sink->lttpr_valid,
		 sink->lttpr_count,
		 sink->branch ? "branch" : "sink",
		 sink->desc.oui[0], sink->desc.oui[1], sink->desc.oui[2],
		 id,
		 (unsigned)(sink->desc.hw_rev >> 4), (unsigned)(sink->desc.hw_rev & 0x0fu),
		 (unsigned)sink->desc.sw_major_rev, (unsigned)sink->desc.sw_minor_rev,
		 sink->desc_valid,
		 sink->max_rate,
		 sink_top,
		 source_top,
		 sink->max_lanes,
		 sink->max_sink_lanes);

	/* A branch's sink count, first downstream port and limits. */
	if (sink->branch) {
		env->log(env->ctx, "i915: DP-ext %s branch: sink count %d (reported %d) | port 0: %02x %02x %02x %02x | TMDS %d-%d kHz, dot clock %d kHz, %d bpc\n",
			 name,
			 sink->sink_count,
			 sink->has_sink_count,
			 sink->downstream[0], sink->downstream[1], sink->downstream[2], sink->downstream[3],
			 sink->min_tmds_clock_khz, sink->max_tmds_clock_khz,
			 sink->max_dotclock_khz,
			 sink->max_bpc);
	}

	/* The EDID's size, maker, product and first detailed mode. */
	if (sink->edid_blocks != 0u) {
		e = sink->edid;
		env->log(env->ctx, "i915: DP-ext %s EDID: %u block(s) (extensions %u) mfg %02x%02x product %02x%02x | DTD1 %ux%u pixel clock %u kHz | HDMI %d\n",
			 name,
			 sink->edid_blocks,
			 sink->edid_extensions,
			 e[8], e[9], e[11], e[10],
			 (unsigned)e[56] | ((unsigned)(e[58] & 0xf0u) << 4),
			 (unsigned)e[59] | ((unsigned)(e[61] & 0xf0u) << 4),
			 ((unsigned)e[54] | ((unsigned)e[55] << 8)) * 10u,
			 sink->has_hdmi_sink);
	}
}

/*
 * Chooses a display 9 DisplayPort link for a mode at 8 bits a channel: the
 * slowest rate the DDI and the sink share, then the fewest lanes, whose
 * data rate holds the mode's; with its M/N values.  Returns 0, or ENOSPC
 * when the sink offers no rate display 9 drives or no link carries the
 * mode.
 */
int
drv_i915_xd9_dp_link(
	uint32_t sink_max_khz,
	unsigned sink_lanes,
	uint32_t pixel_khz,
	struct i915_xd_dp_choice *choice,
	struct i915_xd_dp_m_n *m_n)
{
	unsigned lanes;
	int error;

	/* A sink slower than RBR shares no rate with the DDI. */
	if (sink_max_khz < i915_xd9_dp_rates[0])
		return ENOSPC;

	/* The lanes both ends have: the sink's, up to the DDI's four. */
	lanes = sink_lanes;
	if (lanes > I915_XD9_DP_MAX_LANES)
		lanes = I915_XD9_DP_MAX_LANES;

	/* The slowest link under the sink's rate. */
	error = drv_i915_xd_dp_choose(
		pixel_khz,
		I915_XD9_DP_BPC,
		I915_XD9_DP_BPC,
		i915_xd9_dp_rates,
		sizeof(i915_xd9_dp_rates) / sizeof(i915_xd9_dp_rates[0]),
		sink_max_khz,
		lanes,
		choice);
	if (error != 0)
		return error;

	/* The M/N values of a single stream on it. */
	drv_i915_xd_dp_m_n(choice->bpp * 16U, choice->lanes, pixel_khz, choice->rate_khz, I915_XD9_DP_SST_OVERHEAD, m_n);

	/* Succeeded: the link and its M/N. */
	return 0;
}

/*
 * Reports the fastest rate display 9 drives that a sink's MAX_LINK_RATE
 * code offers, or 0 for a code below RBR or between the rates.
 */
uint32_t
drv_i915_xd9_dp_sink_rate(
	uint8_t code)
{
	/* A code of HBR2 or a faster rate offers HBR2. */
	if (code >= I915_XD9_DP_CODE_HBR2)
		return i915_xd9_dp_rates[2];

	/* HBR. */
	if (code == I915_XD9_DP_CODE_HBR)
		return i915_xd9_dp_rates[1];

	/* RBR. */
	if (code == I915_XD9_DP_CODE_RBR)
		return i915_xd9_dp_rates[0];

	/* Succeeded: no rate display 9 drives. */
	return 0U;
}

/*
 * Reports the LINK_BW_SET code of a rate display 9 drives, or 0 for
 * another rate.
 */
uint8_t
drv_i915_xd9_dp_rate_code(
	uint32_t rate_khz)
{
	unsigned index;
	uint8_t code;

	/* Only the three rates have a code here. */
	for (index = 0U; index < sizeof(i915_xd9_dp_rates) / sizeof(i915_xd9_dp_rates[0]); index++) {
		if (i915_xd9_dp_rates[index] != rate_khz)
			continue;

		/* Succeeded: the rate's code. */
		code = drv_i915_xd_dp_rate_code(rate_khz);
		return code;
	}

	/* Succeeded: no code. */
	return 0U;
}

/* Divides, rounding up. */
static uint32_t
i915_xd_dp_div_round_up(
	uint64_t numerator,
	uint64_t denominator)
{
	/* Succeeded: the quotient, one more when a remainder is left. */
	return (uint32_t)((numerator + denominator - 1U) / denominator);
}

/* Hands a note of the training to the caller, when it takes notes. */
static void
i915_xd_dp_note(
	const struct i915_xd_dp_link *link,
	unsigned kind,
	const char *text)
{
	/* A caller without the hook takes no notes. */
	if (link->source.note == NULL)
		return;

	/* Hands the note over. */
	link->source.note(link->source.context, kind, text);
}

/* Waits between two steps of a training: at least the wait, at most twice it. */
static void
i915_xd_dp_sleep(
	const struct i915_xd_dp_link *link,
	unsigned wait_us)
{
	/* The source's sleep. */
	link->source.sleep(link->source.context, wait_us, 2U * wait_us);
}

/*
 * Computes one M/N pair with a fixed N: M keeps the ratio m / n, and both
 * are halved until they fit the register fields.
 */
static void
i915_xd_dp_ratio(
	uint32_t m,
	uint32_t n,
	uint32_t fixed_n,
	uint32_t *ret_m,
	uint32_t *ret_n)
{
	/* M over the fixed N keeps the ratio. */
	*ret_n = fixed_n;
	*ret_m = (uint32_t)(((uint64_t)m * fixed_n) / n);

	/* Drops a bit of both terms at a time until they fit. */
	while (*ret_m > I915_XD_DP_M_N_MAX || *ret_n > I915_XD_DP_M_N_MAX) {
		*ret_m >>= 1;
		*ret_n >>= 1;
	}
}

/* Takes the status nibble of one lane: two lanes share a byte, the odd one in the high nibble. */
static unsigned
i915_xd_dp_lane_bits(
	const uint8_t *status,
	unsigned lane)
{
	uint8_t byte;

	/* The byte of the lane pair. */
	byte = status[lane >> 1];

	/* Succeeded: the lane's nibble. */
	return (unsigned)(byte >> (4U * (lane & 1U))) & 0xfU;
}

/*
 * Takes the levels one lane's receiver asks for, as a level byte: the
 * swing in the low two bits of the lane's nibble, the pre-emphasis in the
 * next two.
 */
static uint8_t
i915_xd_dp_request(
	const uint8_t *status,
	unsigned lane)
{
	uint8_t byte;
	unsigned nibble;
	uint8_t swing;
	uint8_t preemph;

	/* The lane's nibble of the request bytes. */
	byte = status[I915_XD_DPCD_REQUEST - I915_XD_DPCD_LANE_STATUS + (lane >> 1)];
	nibble = (unsigned)(byte >> (4U * (lane & 1U))) & 0xfU;

	/* The two levels at their places in a level byte. */
	swing = (uint8_t)(nibble & 0x3U);
	preemph = (uint8_t)(((nibble >> 2) & 0x3U) << I915_XD_DPCD_PREEMPH_SHIFT);

	/* Succeeded: the requested levels. */
	return (uint8_t)(swing | preemph);
}

/*
 * Decodes the repeater count of the repeaters' common capabilities: one
 * bit set counts 1 (bit 7) to 8 (bit 0) repeaters, none counts 0.
 * Returns the count, or -1 for any other value (more than one bit set).
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/display/drm_dp_helper.c
 * (drm_dp_lttpr_count).
 */
static int
i915_xd_dp_repeater_count(
	const uint8_t *common)
{
	uint8_t bits;
	int count;

	/* No bit: no repeater. */
	bits = common[I915_XD_DPCD_REPEATER_COUNT - I915_XD_DPCD_REPEATER_REV];
	if (bits == 0U)
		return 0;

	/* More than one bit says nothing usable. */
	if ((bits & (uint8_t)(bits - 1U)) != 0U)
		return -1;

	/* The bit counts from the top: bit 7 is one repeater. */
	count = 8;
	while (bits > 1U) {
		bits >>= 1;
		count--;
	}

	/* Succeeded: the count. */
	return count;
}

/*
 * Reads a run of the repeaters' registers: a byte at a time from a sink
 * older than DPCD 1.4 (at least one monitor answers wrong bytes to longer
 * reads of that range), otherwise in one read.  A short read is used as
 * read.  *error is set to the positive error of a failed read.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/display/drm_dp_helper.c
 * (drm_dp_read_lttpr_regs).
 */
static void
i915_xd_dp_read_repeater_caps(
	struct i915_xd_dp_link *link,
	const uint8_t *caps,
	uint32_t address,
	uint8_t *buffer,
	unsigned size,
	int *error)
{
	unsigned block;
	unsigned offset;
	int moved;

	/* The size of one read. */
	block = size;
	if (caps[I915_XD_DPCD_REV] < I915_XD_DPCD_REV_1_4)
		block = 1U;

	/* Reads the run block by block. */
	*error = 0;
	for (offset = 0U; offset < size; offset += block) {
		moved = link->sink.read(link->sink.context, address + offset, buffer + offset, block);
		if (moved < 0) {
			*error = -moved;
			return;
		}
	}
}

/*
 * Finds the repeaters in front of the sink and switches them to
 * non-transparent mode, so each is trained on its own; reads the
 * capabilities of each.  A link that is up keeps the mode its repeaters
 * are in (changing it would drop the link).  Returns how many repeaters
 * are to be trained: 0 for none, for transparent ones, for an unusable
 * count, or when the switch was refused.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_dp_link_training.c
 * (intel_dp_init_lttpr, intel_dp_init_lttpr_phys,
 * intel_dp_read_lttpr_common_caps, intel_dp_read_lttpr_phy_caps).
 */
static unsigned
i915_xd_dp_find_repeaters(
	struct i915_xd_dp_link *link,
	const uint8_t *caps)
{
	uint8_t *common;
	unsigned index;
	int count;
	int error;
	int switched;

	/* The common capabilities; unreadable ones or a revision below 1.4 are forgotten. */
	common = link->repeater_caps;
	i915_xd_dp_read_repeater_caps(link, caps, I915_XD_DPCD_REPEATER_REV, common, I915_XD_DPCD_REPEATER_COMMON_SIZE, &error);
	if (error != 0 || common[0] < I915_XD_DPCD_REPEATER_REV_1_4) {
		for (index = 0U; index < I915_XD_DPCD_REPEATER_COMMON_SIZE; index++)
			common[index] = 0U;
		return 0U;
	}

	/* No repeater: the mode is left alone (a forced transparent mode breaks at least one dock). */
	count = i915_xd_dp_repeater_count(common);
	if (count == 0)
		return 0U;

	/* A link that is up keeps its repeaters' mode: only non-transparent ones are trained. */
	if (link->active) {
		if (count < 0 ||
		    common[I915_XD_DPCD_REPEATER_MODE - I915_XD_DPCD_REPEATER_REV] == I915_XD_DPCD_REPEATER_TRANSPARENT) {
			common[I915_XD_DPCD_REPEATER_COUNT - I915_XD_DPCD_REPEATER_REV] = 0U;
			return 0U;
		}
	} else {
		/* The standard asks for transparent mode before non-transparent; an unusable count stops there. */
		(void)i915_xd_dp_set_repeater_mode(link, (uint8_t)I915_XD_DPCD_REPEATER_TRANSPARENT);
		if (count < 0)
			return 0U;

		/* Non-transparent mode; a refusal goes back to transparent mode and trains no repeater. */
		switched = i915_xd_dp_set_repeater_mode(link, (uint8_t)I915_XD_DPCD_REPEATER_OPAQUE);
		if (!switched) {
			i915_xd_dp_note(link, I915_XD_DP_NOTE_DEBUG, "xd-dp: the repeaters stay transparent\n");
			(void)i915_xd_dp_set_repeater_mode(link, (uint8_t)I915_XD_DPCD_REPEATER_TRANSPARENT);
			common[I915_XD_DPCD_REPEATER_COUNT - I915_XD_DPCD_REPEATER_REV] = 0U;
			return 0U;
		}
	}

	/* The capabilities of each repeater; one that cannot be read is trained with what was there. */
	for (index = 1U; index <= (unsigned)count; index++) {
		i915_xd_dp_read_repeater_caps(link, caps, I915_XD_DPCD_REPEATER_INTERVAL(index),
			link->repeater_phy_caps[index - 1U], I915_XD_DPCD_REPEATER_CAP_SIZE, &error);
		if (error != 0)
			i915_xd_dp_note(link, I915_XD_DP_NOTE_DEBUG, "xd-dp: a repeater's capabilities could not be read\n");
	}

	/* Succeeded: the repeaters to train. */
	return (unsigned)count;
}

/* Writes the repeaters' mode.  Returns 1 when the sink took it, 0 otherwise. */
static int
i915_xd_dp_set_repeater_mode(
	struct i915_xd_dp_link *link,
	uint8_t mode)
{
	int moved;

	/* The mode register. */
	moved = link->sink.write(link->sink.context, I915_XD_DPCD_REPEATER_MODE, &mode, 1U);
	if (moved != 1)
		return 0;

	/* Succeeded: the sink took the mode. */
	return 1;
}

/*
 * Looks for repeaters (when the link asks for it) on a fresh read of the
 * sink's capabilities, then reads the capabilities again into the link's
 * storage, as the standard asks after the repeaters are found.  Returns
 * 0, or EIO when the sink did not answer (the repeaters then count 0;
 * when the last read failed the repeaters' capabilities are forgotten).
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_dp_link_training.c
 * (intel_dp_init_lttpr_and_dprx_caps).
 */
static int
i915_xd_dp_refresh_caps(
	struct i915_xd_dp_link *link)
{
	uint8_t fresh[I915_XD_DPCD_CAPS_SIZE];
	unsigned found;
	unsigned index;
	int error;

	/* Repeaters are looked for on a sink that answers. */
	found = 0U;
	if (link->find_repeaters) {
		error = link->sink.probe(link->sink.context, I915_XD_DPCD_REPEATER_REV);
		if (error != 0)
			return EIO;
		error = link->sink.read_caps(link->sink.context, fresh);
		if (error != 0)
			return EIO;
		found = i915_xd_dp_find_repeaters(link, fresh);
	}

	/* The capabilities again, into the link's storage. */
	error = link->sink.read_caps(link->sink.context, link->caps);
	if (error != 0) {
		for (index = 0U; index < I915_XD_DPCD_REPEATER_COMMON_SIZE; index++)
			link->repeater_caps[index] = 0U;
		return EIO;
	}

	/* Succeeded: the repeaters to train. */
	link->repeaters = found;
	return 0;
}

/*
 * Sets up the port and the sink for the link: the source's preparation,
 * the rate list of an eDP 1.4 panel read again (a common eDP mux forgets
 * the rates it snooped when it powers down), the coding and the timing
 * attribute bit, then the rate and the lanes.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_dp_link_training.c
 * (intel_dp_prepare_link_train, intel_dp_update_downspread_ctrl,
 * intel_dp_update_link_bw_set).
 */
static void
i915_xd_dp_configure(
	struct i915_xd_dp_link *link)
{
	uint8_t table[2U * I915_XD_DPCD_EDP_RATE_COUNT];
	uint8_t bytes[2];
	uint8_t lanes;
	uint8_t code;

	/* The source prepares the port (the DDI is enabled here). */
	if (link->source.prepare != NULL)
		link->source.prepare(link->source.context);

	/* An eDP 1.4 panel set by its rate list reads the list again; the bytes are not used. */
	code = 0U;
	if (link->rate_index < 0)
		code = drv_i915_xd_dp_rate_code(link->rate_khz);
	if (code == 0U)
		(void)link->sink.read(link->sink.context, I915_XD_DPCD_EDP_RATES, table, sizeof(table));

	/* The timing attribute bit and the 8b/10b coding, before the rate. */
	bytes[0] = 0U;
	if (link->msa_ignore)
		bytes[0] = (uint8_t)I915_XD_DPCD_MSA_IGNORE;
	bytes[1] = (uint8_t)I915_XD_DPCD_CODING_8B10B;
	(void)link->sink.write(link->sink.context, I915_XD_DPCD_DOWNSPREAD, bytes, 2U);

	/* The lanes, with the enhanced framing bit. */
	lanes = (uint8_t)link->lanes;
	if (link->enhanced)
		lanes |= (uint8_t)I915_XD_DPCD_LANES_ENHANCED;

	/*
	 * The rate by its code with the lanes in one write, or by its index
	 * (an eDP 1.4 sink ignores the index once a code is set, so no code
	 * is written).
	 */
	if (code != 0U) {
		bytes[0] = code;
		bytes[1] = lanes;
		(void)link->sink.write(link->sink.context, I915_XD_DPCD_BW_SET, bytes, 2U);
	} else {
		bytes[0] = 0U;
		if (link->rate_index >= 0)
			bytes[0] = (uint8_t)link->rate_index;
		(void)link->sink.write(link->sink.context, I915_XD_DPCD_LANES_SET, &lanes, 1U);
		(void)link->sink.write(link->sink.context, I915_XD_DPCD_RATE_SET, bytes, 1U);
	}
}

/*
 * Tells whether the source drives a receiver directly: the sink when
 * there are no repeaters, otherwise the highest numbered repeater.
 * Every other receiver is driven by the repeater numbered one above it.
 */
static int
i915_xd_dp_nearest(
	const struct i915_xd_dp_link *link,
	unsigned hop)
{
	/* Without repeaters the source drives the sink. */
	if (link->repeaters == 0U)
		return 1;

	/* The highest numbered repeater. */
	if (hop == link->repeaters)
		return 1;

	/* Succeeded: a repeater drives this receiver. */
	return 0;
}

/*
 * Finds the wait between two steps of a phase on a receiver: clock
 * recovery waits the fixed 100 us on a repeater and on a DPCD 1.4 sink,
 * otherwise the sink's read interval; channel equalization waits the read
 * interval (the sink's from its capabilities, a repeater's read from it,
 * 400 us when it cannot be read).  An interval of 0 is the phase's
 * default, each step of it 4 ms.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/display/drm_dp_helper.c
 * (__read_delay, drm_dp_read_clock_recovery_delay,
 * drm_dp_read_channel_eq_delay).
 */
static unsigned
i915_xd_dp_wait(
	struct i915_xd_dp_link *link,
	unsigned hop,
	int equalize)
{
	uint8_t interval;
	int moved;

	/* Clock recovery on a repeater or a DPCD 1.4 sink waits the fixed time. */
	if (!equalize) {
		if (hop != I915_XD_DP_HOP_SINK)
			return I915_XD_DP_CR_WAIT_US;
		if (link->caps[I915_XD_DPCD_REV] >= I915_XD_DPCD_REV_1_4)
			return I915_XD_DP_CR_WAIT_US;
	}

	/* The interval: the sink's capability, or a repeater's register. */
	if (hop == I915_XD_DP_HOP_SINK) {
		interval = link->caps[I915_XD_DPCD_READ_INTERVAL];
	} else {
		moved = link->sink.read(link->sink.context, I915_XD_DPCD_REPEATER_INTERVAL(hop), &interval, 1U);
		if (moved != 1)
			return I915_XD_DP_EQ_WAIT_US;
	}

	/* An interval beyond the named ones is noted and used. */
	interval &= (uint8_t)I915_XD_DPCD_READ_INTERVAL_MASK;
	if (interval > I915_XD_DP_INTERVAL_TOP)
		i915_xd_dp_note(link, I915_XD_DP_NOTE_DEBUG, "xd-dp: the sink's read interval is out of range\n");

	/* No interval is the phase's default. */
	if (interval == 0U) {
		if (equalize)
			return I915_XD_DP_EQ_WAIT_US;
		return I915_XD_DP_CR_WAIT_US;
	}

	/* Succeeded: the interval counts 4 ms. */
	return (unsigned)interval * I915_XD_DP_INTERVAL_US;
}

/*
 * Limits the levels a receiver asks for to what its transmitter drives:
 * the pre-emphasis to the transmitter's highest (marked as the top), the
 * swing to what that pre-emphasis leaves (swing and pre-emphasis add up
 * to 3 at most) and to the transmitter's highest (marked as the top).
 * The transmitter is the source for the receiver it drives directly,
 * otherwise the repeater numbered one above (level 3 when its
 * capabilities say so, else 2).
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_dp_link_training.c
 * (intel_dp_get_lane_adjust_vswing_preemph, dp_voltage_max,
 * intel_dp_phy_voltage_max, intel_dp_phy_preemph_max).
 */
static uint8_t
i915_xd_dp_limit(
	const struct i915_xd_dp_link *link,
	unsigned hop,
	uint8_t swing,
	uint8_t preemph)
{
	uint8_t swing_max;
	uint8_t preemph_max;
	uint8_t room;
	uint8_t capability;
	int nearest;

	/* The transmitter's highest levels. */
	nearest = i915_xd_dp_nearest(link, hop);
	if (nearest) {
		swing_max = link->swing_max;
		preemph_max = (uint8_t)(link->preemph_max << I915_XD_DPCD_PREEMPH_SHIFT);
	} else {
		capability = link->repeater_phy_caps[hop][I915_XD_DPCD_REPEATER_TX_CAP - I915_XD_DPCD_REPEATER_INTERVAL(1U)];
		swing_max = 2U;
		if ((capability & I915_XD_DPCD_REPEATER_SWING_3) != 0U)
			swing_max = 3U;
		preemph_max = (uint8_t)(2U << I915_XD_DPCD_PREEMPH_SHIFT);
		if ((capability & I915_XD_DPCD_REPEATER_PREEMPH_3) != 0U)
			preemph_max = (uint8_t)(3U << I915_XD_DPCD_PREEMPH_SHIFT);
	}

	/* The pre-emphasis at most the transmitter's, marked when it is there. */
	if (preemph >= preemph_max)
		preemph = (uint8_t)(preemph_max | I915_XD_DPCD_PREEMPH_TOP);

	/* The swing at most what the pre-emphasis leaves. */
	room = (uint8_t)(3U - ((preemph & I915_XD_DPCD_PREEMPH) >> I915_XD_DPCD_PREEMPH_SHIFT));
	if (swing > room)
		swing = room;

	/* The swing at most the transmitter's, marked when it is there. */
	if (swing >= swing_max)
		swing = (uint8_t)(swing_max | I915_XD_DPCD_SWING_TOP);

	/* Succeeded: the lane's level byte. */
	return (uint8_t)(swing | preemph);
}

/*
 * Follows the levels a receiver asks for: each lane takes its own request
 * when its transmitter sets the lanes one by one (lanes beyond the link
 * take the last lane's), otherwise every lane the highest request of the
 * lanes in use; then the transmitter's limits.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_dp_link_training.c
 * (intel_dp_get_adjust_train, has_per_lane_signal_levels).
 */
static void
i915_xd_dp_follow(
	struct i915_xd_dp_link *link,
	unsigned hop,
	const uint8_t *status)
{
	uint8_t request;
	uint8_t swing;
	uint8_t preemph;
	unsigned lane;
	unsigned asked;
	int per_lane;
	int nearest;

	/* A repeater sets its lanes one by one; the source when it says so. */
	per_lane = 1;
	nearest = i915_xd_dp_nearest(link, hop);
	if (nearest)
		per_lane = link->per_lane;

	/* The highest request of the lanes in use, for a transmitter that sets all lanes alike. */
	swing = 0U;
	preemph = 0U;
	if (!per_lane) {
		for (lane = 0U; lane < link->lanes; lane++) {
			request = i915_xd_dp_request(status, lane);
			if ((request & I915_XD_DPCD_SWING) > swing)
				swing = (uint8_t)(request & I915_XD_DPCD_SWING);
			if ((request & I915_XD_DPCD_PREEMPH) > preemph)
				preemph = (uint8_t)(request & I915_XD_DPCD_PREEMPH);
		}
	}

	/* The level byte of each of the four lanes. */
	for (lane = 0U; lane < 4U; lane++) {
		if (per_lane) {
			asked = lane;
			if (asked > link->lanes - 1U)
				asked = link->lanes - 1U;
			request = i915_xd_dp_request(status, asked);
			swing = (uint8_t)(request & I915_XD_DPCD_SWING);
			preemph = (uint8_t)(request & I915_XD_DPCD_PREEMPH);
		}

		/* Limits the levels to what the link can drive. */
		link->set[lane] = i915_xd_dp_limit(link, hop, swing, preemph);
	}
}

/*
 * Sends a training pattern: the source's port first (its levels and its
 * pattern), then the receiver's pattern register with the level byte of
 * each lane in one write.  Returns 1 when the sink took every byte, 0
 * otherwise.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_dp_link_training.c
 * (intel_dp_set_link_train, intel_dp_program_link_training_pattern).
 */
static int
i915_xd_dp_send_pattern(
	struct i915_xd_dp_link *link,
	unsigned hop,
	uint8_t pattern)
{
	uint8_t bytes[5];
	uint32_t address;
	unsigned lane;
	int moved;

	/* The port's pattern. */
	link->source.pattern(link->source.context, pattern);

	/* The receiver's pattern register, the lanes' levels after it. */
	address = I915_XD_DPCD_PATTERN_SET;
	if (hop != I915_XD_DP_HOP_SINK)
		address = I915_XD_DPCD_REPEATER_PATTERN(hop);
	bytes[0] = pattern;
	for (lane = 0U; lane < link->lanes; lane++)
		bytes[1U + lane] = link->set[lane];
	moved = link->sink.write(link->sink.context, address, bytes, link->lanes + 1U);
	if (moved != (int)(link->lanes + 1U))
		return 0;

	/* Succeeded: the receiver took the pattern and the levels. */
	return 1;
}

/*
 * Sends the lanes' levels: the source's port when it drives the receiver,
 * then the receiver's level registers.  Returns 1 when the sink took
 * every lane, 0 otherwise.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_dp_link_training.c
 * (intel_dp_update_link_train, intel_dp_set_signal_levels).
 */
static int
i915_xd_dp_send_levels(
	struct i915_xd_dp_link *link,
	unsigned hop)
{
	uint32_t address;
	int moved;
	int nearest;

	/* The port's levels, when it drives this receiver. */
	nearest = i915_xd_dp_nearest(link, hop);
	if (nearest)
		link->source.levels(link->source.context, link->set);

	/* The receiver's level registers. */
	address = I915_XD_DPCD_LEVELS_SET;
	if (hop != I915_XD_DP_HOP_SINK)
		address = I915_XD_DPCD_REPEATER_LEVELS(hop);
	moved = link->sink.write(link->sink.context, address, link->set, link->lanes);
	if (moved != (int)link->lanes)
		return 0;

	/* Succeeded: the receiver took every lane. */
	return 1;
}

/* Tells whether the requests of a lane in use changed between two statuses.  Returns 1 or 0. */
static int
i915_xd_dp_requests_differ(
	const struct i915_xd_dp_link *link,
	const uint8_t *before,
	const uint8_t *now)
{
	unsigned lane;
	uint8_t old_request;
	uint8_t new_request;

	/* Compares each lane in use. */
	for (lane = 0U; lane < link->lanes; lane++) {
		old_request = i915_xd_dp_request(before, lane);
		new_request = i915_xd_dp_request(now, lane);
		if (old_request != new_request)
			return 1;
	}

	/* Succeeded: the receiver asks for the same levels. */
	return 0;
}

/*
 * Tells whether every lane in use is at the top of the levels: its swing
 * marked as the transmitter's highest, and swing and pre-emphasis adding
 * up to 3.  Returns 1 or 0.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_dp_link_training.c
 * (intel_dp_link_max_vswing_reached, intel_dp_lane_max_vswing_reached).
 */
static int
i915_xd_dp_at_top(
	const struct i915_xd_dp_link *link)
{
	unsigned lane;
	unsigned levels;

	/* Looks for a lane in use below the top. */
	for (lane = 0U; lane < link->lanes; lane++) {
		if ((link->set[lane] & I915_XD_DPCD_SWING_TOP) == 0U)
			return 0;
		levels = (unsigned)(link->set[lane] & I915_XD_DPCD_SWING) +
			 ((unsigned)(link->set[lane] & I915_XD_DPCD_PREEMPH) >> I915_XD_DPCD_PREEMPH_SHIFT);
		if (levels != 3U)
			return 0;
	}

	/* Succeeded: every lane is at the top. */
	return 1;
}

/*
 * Recovers the clock on a receiver: pattern 1 without scrambling from the
 * lowest levels, then after each wait the receiver's status, following
 * its requests until every lane has the clock.  It gives up when the same
 * requests came 5 times in a row, when the lanes were already at the top,
 * after 10 tries on a DPCD 1.4 sink (80 on an older one, which the
 * standard left without a limit), or when the sink stops answering.
 * Returns 1 when the clock is recovered, 0 otherwise.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_dp_link_training.c
 * (intel_dp_link_training_clock_recovery, intel_dp_reset_link_train).
 */
static int
i915_xd_dp_recover_clock(
	struct i915_xd_dp_link *link,
	unsigned hop)
{
	uint8_t status[I915_XD_DPCD_STATUS_SIZE];
	uint8_t before[I915_XD_DPCD_STATUS_SIZE];
	unsigned wait_us;
	unsigned tries;
	unsigned limit;
	unsigned same;
	unsigned lane;
	int topped;
	int sent;
	int error;
	int differ;
	int nearest;
	int done;

	/* The wait between the steps. */
	wait_us = i915_xd_dp_wait(link, hop, 0);

	/* Starts from the lowest levels with pattern 1. */
	for (lane = 0U; lane < 4U; lane++)
		link->set[lane] = 0U;
	nearest = i915_xd_dp_nearest(link, hop);
	if (nearest)
		link->source.levels(link->source.context, link->set);
	sent = i915_xd_dp_send_pattern(link, hop, (uint8_t)(I915_XD_DPCD_PATTERN_1 | I915_XD_DPCD_PATTERN_NO_SCRAMBLE));
	if (!sent) {
		i915_xd_dp_note(link, I915_XD_DP_NOTE_ERROR, "xd-dp: the sink did not take pattern 1\n");
		return 0;
	}

	/* The tries a sink of its revision is given. */
	limit = I915_XD_DP_CR_TRIES_OLD;
	if (link->caps[I915_XD_DPCD_REV] >= I915_XD_DPCD_REV_1_4)
		limit = I915_XD_DP_CR_TRIES;

	/* Follows the receiver's requests until the clock is recovered. */
	for (lane = 0U; lane < I915_XD_DPCD_STATUS_SIZE; lane++)
		before[lane] = 0U;
	same = 1U;
	topped = 0;
	for (tries = 0U; tries < limit; tries++) {
		i915_xd_dp_sleep(link, wait_us);

		/* The receiver's status. */
		error = drv_i915_xd_dp_read_status(&link->sink, hop, status);
		if (error != 0) {
			i915_xd_dp_note(link, I915_XD_DP_NOTE_ERROR, "xd-dp: the link status could not be read\n");
			return 0;
		}

		/* The clock on every lane ends the phase. */
		done = drv_i915_xd_dp_clock_recovered(status, link->lanes);
		if (done)
			break;

		/* The same requests five times, or nothing higher to give: the phase has failed. */
		if (same == I915_XD_DP_SAME_TRIES) {
			i915_xd_dp_note(link, I915_XD_DP_NOTE_DEBUG, "xd-dp: the same levels were asked for 5 times\n");
			return 0;
		}

		/* Stops when the levels are at the top. */
		if (topped) {
			i915_xd_dp_note(link, I915_XD_DP_NOTE_DEBUG, "xd-dp: the levels are at the top\n");
			return 0;
		}

		/* The requested levels. */
		i915_xd_dp_follow(link, hop, status);
		sent = i915_xd_dp_send_levels(link, hop);
		if (!sent) {
			i915_xd_dp_note(link, I915_XD_DP_NOTE_ERROR, "xd-dp: the sink did not take the levels\n");
			return 0;
		}

		/* Counts requests that did not change. */
		differ = i915_xd_dp_requests_differ(link, before, status);
		if (differ) {
			same = 1U;
		} else {
			same++;
		}

		/* Keeps the status for the next comparison. */
		for (lane = 0U; lane < I915_XD_DPCD_STATUS_SIZE; lane++)
			before[lane] = status[lane];

		/* Remembers lanes that cannot go higher. */
		done = i915_xd_dp_at_top(link);
		if (done)
			topped = 1;
	}

	/* A receiver that never recovered the clock. */
	if (tries == limit) {
		i915_xd_dp_note(link, I915_XD_DP_NOTE_ERROR, "xd-dp: the clock was not recovered\n");
		return 0;
	}

	/* Succeeded: every lane has the clock. */
	return 1;
}

/*
 * Picks the pattern of channel equalization: pattern 4 when both ends
 * know it (a repeater always does), else pattern 3 when both know it,
 * else pattern 2.  Patterns 2 and 3 are sent without scrambling.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_dp_link_training.c
 * (intel_dp_training_pattern).
 */
static uint8_t
i915_xd_dp_eq_pattern(
	const struct i915_xd_dp_link *link,
	unsigned hop)
{
	int sink_tps3;
	int sink_tps4;

	/* What the receiver knows: a repeater both, the sink what its capabilities of DPCD 1.2 and 1.4 say. */
	sink_tps3 = 1;
	sink_tps4 = 1;
	if (hop == I915_XD_DP_HOP_SINK) {
		sink_tps3 = 0;
		if (link->caps[I915_XD_DPCD_REV] >= I915_XD_DPCD_REV_1_2 &&
		    (link->caps[I915_XD_DPCD_MAX_LANES] & I915_XD_DPCD_TPS3) != 0U)
			sink_tps3 = 1;
		sink_tps4 = 0;
		if (link->caps[I915_XD_DPCD_REV] >= I915_XD_DPCD_REV_1_4 &&
		    (link->caps[I915_XD_DPCD_MAX_DOWNSPREAD] & I915_XD_DPCD_TPS4) != 0U)
			sink_tps4 = 1;
	}

	/* Pattern 4, scrambled. */
	if (link->tps4 && sink_tps4)
		return (uint8_t)I915_XD_DPCD_PATTERN_4;

	/* Pattern 3. */
	if (link->tps3 && sink_tps3)
		return (uint8_t)(I915_XD_DPCD_PATTERN_3 | I915_XD_DPCD_PATTERN_NO_SCRAMBLE);

	/* Succeeded: pattern 2, which every receiver knows. */
	return (uint8_t)(I915_XD_DPCD_PATTERN_2 | I915_XD_DPCD_PATTERN_NO_SCRAMBLE);
}

/*
 * Equalizes the channel on a receiver: the equalization pattern with the
 * levels clock recovery left, then after each wait the receiver's status,
 * following its requests until the channel is equalized, at most 5
 * times.  A lane that loses the clock, or a sink that stops answering,
 * ends it.  Returns 1 when the channel is equalized, 0 otherwise.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_dp_link_training.c
 * (intel_dp_link_training_channel_equalization).
 */
static int
i915_xd_dp_equalize(
	struct i915_xd_dp_link *link,
	unsigned hop)
{
	uint8_t status[I915_XD_DPCD_STATUS_SIZE];
	unsigned wait_us;
	unsigned tries;
	uint8_t pattern;
	int sent;
	int error;
	int done;

	/* The wait between the steps, then the pattern. */
	wait_us = i915_xd_dp_wait(link, hop, 1);
	pattern = i915_xd_dp_eq_pattern(link, hop);
	sent = i915_xd_dp_send_pattern(link, hop, pattern);
	if (!sent) {
		i915_xd_dp_note(link, I915_XD_DP_NOTE_ERROR, "xd-dp: the sink did not take the equalization pattern\n");
		return 0;
	}

	/* Follows the receiver's requests until the channel is equalized. */
	for (tries = 0U; tries < I915_XD_DP_EQ_TRIES; tries++) {
		i915_xd_dp_sleep(link, wait_us);

		/* The receiver's status. */
		error = drv_i915_xd_dp_read_status(&link->sink, hop, status);
		if (error != 0) {
			i915_xd_dp_note(link, I915_XD_DP_NOTE_ERROR, "xd-dp: the link status could not be read\n");
			return 0;
		}

		/* A lane that lost the clock ends the phase. */
		done = drv_i915_xd_dp_clock_recovered(status, link->lanes);
		if (!done) {
			i915_xd_dp_note(link, I915_XD_DP_NOTE_DEBUG, "xd-dp: a lane lost the clock during equalization\n");
			return 0;
		}

		/* An equalized channel ends the training of the receiver. */
		done = drv_i915_xd_dp_equalized(status, link->lanes);
		if (done)
			return 1;

		/* The requested levels. */
		i915_xd_dp_follow(link, hop, status);
		sent = i915_xd_dp_send_levels(link, hop);
		if (!sent) {
			i915_xd_dp_note(link, I915_XD_DP_NOTE_ERROR, "xd-dp: the sink did not take the levels\n");
			return 0;
		}
	}

	/* Five tries without an equalized channel. */
	i915_xd_dp_note(link, I915_XD_DP_NOTE_DEBUG, "xd-dp: the channel was not equalized\n");
	return 0;
}

/* Takes a receiver out of the training pattern. */
static void
i915_xd_dp_end_hop(
	struct i915_xd_dp_link *link,
	unsigned hop)
{
	uint8_t none;
	uint32_t address;

	/* The receiver's pattern register. */
	none = (uint8_t)I915_XD_DPCD_PATTERN_NONE;
	address = I915_XD_DPCD_PATTERN_SET;
	if (hop != I915_XD_DP_HOP_SINK)
		address = I915_XD_DPCD_REPEATER_PATTERN(hop);
	(void)link->sink.write(link->sink.context, address, &none, 1U);
}

/*
 * Reads the repeaters' common capabilities and counts the repeaters.
 *
 * A sink below DPCD 1.4 is read a byte at a time: at least one monitor
 * returns corrupted values for larger reads of that range.  Capabilities
 * that cannot be read, or below revision 1.4, are dropped; a repeater
 * count out of range keeps the capabilities' limits but counts no
 * repeater.  Repeaters are not switched to non-transparent mode here.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_dp_link_training.c
 * (intel_dp_read_lttpr_common_caps).
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/display/drm_dp_helper.c (drm_dp_lttpr_count).
 */
static void
i915_xd_dp_probe_read_lttpr(
	const struct i915_xd_dp_probe_env *env,
	struct i915_xd_dp_probe *sink)
{
	unsigned offset;
	unsigned block;
	long read;

	/* Reads the block whole, or a byte at a time before DPCD 1.4. */
	block = I915_XD_DP_PROBE_LTTPR_SIZE;
	if (sink->dpcd[I915_XD_DP_PROBE_DPCD_REV] < I915_XD_DP_PROBE_DPCD_REV_14)
		block = 1u;

	/* Reads the capabilities; a failed read drops them. */
	for (offset = 0u; offset < I915_XD_DP_PROBE_LTTPR_SIZE; offset += block) {
		/* Reads the next block. */
		read = env->dpcd_read(env->ctx, I915_XD_DP_PROBE_LTTPR_CAPS + offset, &sink->lttpr[offset], block);
		if (read < 0) {
			i915_xd_dp_probe_zero(sink->lttpr, sizeof(sink->lttpr));
			return;
		}
	}

	/* Capabilities below revision 1.4 are not to be trusted. */
	if (sink->lttpr[I915_XD_DP_PROBE_LTTPR_REV] < I915_XD_DP_PROBE_LTTPR_MIN_REV) {
		i915_xd_dp_probe_zero(sink->lttpr, sizeof(sink->lttpr));
		return;
	}

	/* Keeps the capabilities and counts the repeaters. */
	sink->lttpr_valid = 1;
	sink->lttpr_count = i915_xd_dp_probe_lttpr_count(sink->lttpr[I915_XD_DP_PROBE_LTTPR_COUNT]);
}

/*
 * Counts the repeaters a count field names: one bit, 0x80 for one repeater
 * down to 0x01 for eight; anything else is not a count and counts none.
 */
static int
i915_xd_dp_probe_lttpr_count(
	uint8_t count_bits)
{
	int count;
	uint8_t bit;

	/* Finds the one bit set, from the top. */
	count = 0;
	bit = 0x80u;
	while (bit != 0u) {
		/* The bit stands for one more repeater than the one above it. */
		count++;

		/* A field of exactly this bit names the count. */
		if (count_bits == bit)
			return count;

		/* Goes on to the next bit down. */
		bit = (uint8_t)(bit >> 1);
	}

	/* No repeater, or a field with more than one bit set. */
	return 0;
}

/*
 * Reads the repeaters and the receiver of a port that is not eDP: a
 * throw-away read at the repeaters' range wakes them, the receiver's
 * capabilities are read, then the repeaters', then the receiver's again,
 * as the source must read them after detecting the repeaters.
 *
 * Returns 0, or the negative errno of the read that failed.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_dp_link_training.c
 * (intel_dp_init_lttpr_and_dprx_caps).
 */
static int
i915_xd_dp_probe_read_caps(
	const struct i915_xd_dp_probe_env *env,
	struct i915_xd_dp_probe *sink)
{
	int error;

	/* Wakes the repeaters and the sink. */
	error = env->dpcd_probe(env->ctx, I915_XD_DP_PROBE_LTTPR_CAPS);
	if (error != 0)
		return error;

	/* Reads the receiver capabilities, which tell how to read the repeaters'. */
	error = env->read_caps(env->ctx, sink->dpcd);
	if (error != 0)
		return error;

	/* Reads the repeaters' capabilities. */
	i915_xd_dp_probe_read_lttpr(env, sink);

	/* Reads the receiver capabilities again, now behind the repeaters. */
	error = env->read_caps(env->ctx, sink->dpcd);
	if (error != 0) {
		i915_xd_dp_probe_zero(sink->lttpr, sizeof(sink->lttpr));
		sink->lttpr_valid = 0;
		sink->lttpr_count = 0;
		return error;
	}

	/* Succeeded: the receiver capabilities are valid. */
	sink->dpcd_valid = 1;
	return 0;
}

/*
 * Reads the identification of a branch, or of the sink when it is no
 * branch; a read that fails leaves it unknown and is not reported.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/display/drm_dp_helper.c (drm_dp_read_desc).
 */
static void
i915_xd_dp_probe_read_desc(
	const struct i915_xd_dp_probe_env *env,
	struct i915_xd_dp_probe *sink)
{
	uint8_t bytes[I915_XD_DP_PROBE_DESC_SIZE];
	unsigned offset;
	unsigned i;
	long read;

	/* A branch identifies itself at 0x500, a sink at 0x400. */
	offset = I915_XD_DP_PROBE_SINK_OUI;
	if (sink->branch)
		offset = I915_XD_DP_PROBE_BRANCH_OUI;

	/* Reads the twelve bytes. */
	read = env->dpcd_read(env->ctx, offset, bytes, sizeof(bytes));
	if (read != (long)sizeof(bytes))
		return;

	/* The OUI, the identification string, and the revisions. */
	sink->desc.oui[0] = bytes[0];
	sink->desc.oui[1] = bytes[1];
	sink->desc.oui[2] = bytes[2];
	for (i = 0u; i < I915_XD_DP_PROBE_DEVICE_ID_SIZE; i++)
		sink->desc.device_id[i] = (char)bytes[3u + i];
	sink->desc.hw_rev = bytes[9];
	sink->desc.sw_major_rev = bytes[10];
	sink->desc.sw_minor_rev = bytes[11];
	sink->desc_valid = 1;
}

/*
 * Sets the link rates each side offers and both share, through the xd
 * DisplayPort code: the source offers its table up to the lower of the
 * platform's and the VBT's limits; the sink offers the standard rates up
 * to its maximum and that of the repeaters.  A side left with nothing
 * falls back to RBR.
 */
static void
i915_xd_dp_probe_set_rates(
	const struct i915_xd_dp_probe_env *env,
	const struct i915_xd_dp_probe_source *source,
	struct i915_xd_dp_probe *sink)
{
	struct i915_xd_dp_rates rates;
	unsigned made_up;
	unsigned index;
	uint8_t repeater_code;

	/* The repeaters' limit counts only when their capabilities were read. */
	repeater_code = 0;
	if (sink->lttpr_valid)
		repeater_code = sink->lttpr[I915_XD_DP_PROBE_LTTPR_MAX_LINK_RATE];

	/* The rates of both ends. */
	made_up = drv_i915_xd_dp_rates((uint32_t)source->max_rate, (uint32_t)source->vbt_max_rate,
				       sink->dpcd[I915_XD_DP_PROBE_MAX_LINK_RATE], repeater_code, &rates);

	/* Notes a sink that offered nothing, and ends that meet nowhere. */
	if ((made_up & I915_XD_DP_RATES_SINK_NONE) != 0)
		env->log(env->ctx, "i915: DP-ext: invalid DPCD with no link rates (max code 0x%02x), using RBR\n", sink->dpcd[I915_XD_DP_PROBE_MAX_LINK_RATE]);
	if ((made_up & I915_XD_DP_RATES_SHARED_NONE) != 0)
		env->log(env->ctx, "i915: DP-ext: the source and the sink share no link rate, using RBR\n");

	/* The source's rates. */
	sink->num_source_rates = (int)rates.source_count;
	for (index = 0; index < rates.source_count; index++)
		sink->source_rates[index] = (int)rates.source[index];

	/* The sink's rates. */
	sink->num_sink_rates = (int)rates.sink_count;
	for (index = 0; index < rates.sink_count; index++)
		sink->sink_rates[index] = (int)rates.sink[index];

	/* The shared rates, and the highest of them. */
	sink->num_common_rates = (int)rates.common_count;
	for (index = 0; index < rates.common_count; index++)
		sink->common_rates[index] = (int)rates.common[index];
	sink->max_rate = sink->common_rates[sink->num_common_rates - 1];
}

/*
 * Sets the lanes the sink takes and the lanes every party allows.
 *
 * The sink takes 1, 2 or 4 lanes (1 when its DPCD says otherwise); the
 * repeaters, the source with the VBT's limit, and the FIA lower that.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_dp.c
 * (intel_dp_set_max_sink_lane_count, intel_dp_max_common_lane_count).
 */
static void
i915_xd_dp_probe_set_lanes(
	const struct i915_xd_dp_probe_env *env,
	const struct i915_xd_dp_probe_source *source,
	struct i915_xd_dp_probe *sink)
{
	int sink_max;
	int source_max;
	int lttpr_max;
	int lanes;

	/* The sink's lanes; a count other than 1, 2 or 4 is invalid and becomes 1. */
	sink_max = (int)(sink->dpcd[I915_XD_DP_PROBE_MAX_LANE_COUNT] & I915_XD_DP_PROBE_MAX_LANE_COUNT_MASK);
	if (sink_max != 1 &&
	    sink_max != 2 &&
	    sink_max != 4) {
		env->log(env->ctx, "i915: DP-ext: invalid DPCD max lane count (%d), using 1\n", sink_max);
		sink_max = 1;
	}

	/* Records what the sink takes before the others lower it. */
	sink->max_sink_lanes = sink_max;

	/* The repeaters' lanes lower the sink's. */
	lttpr_max = 0;
	if (sink->lttpr_valid)
		lttpr_max = (int)(sink->lttpr[I915_XD_DP_PROBE_LTTPR_MAX_LANE_COUNT] & I915_XD_DP_PROBE_MAX_LANE_COUNT_MASK);
	if (lttpr_max != 0 && lttpr_max < sink_max)
		sink_max = lttpr_max;

	/* The source's lanes, lowered by the VBT's limit. */
	source_max = source->max_lanes;
	if (source->vbt_max_lanes != 0 && source->vbt_max_lanes < source_max)
		source_max = source->vbt_max_lanes;

	/* The fewest of source, sink and the lanes the FIA gave DisplayPort. */
	lanes = sink_max;
	if (source_max < lanes)
		lanes = source_max;
	if (source->fia_lanes != 0 && source->fia_lanes < lanes)
		lanes = source->fia_lanes;

	/* Records the lanes of the link. */
	sink->max_lanes = lanes;
}

/*
 * Reads the sink count of a sink that reports one.
 *
 * A branch of DPCD 1.1 or later reports how many displays are behind it.
 * Returns 0 when the count is not reported or is not zero, the negative
 * errno of a failed read, or -EIO for a count of zero: an adapter
 * with no display behind it is disconnected.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/display/drm_dp_helper.c (drm_dp_read_sink_count_cap,
 * drm_dp_read_sink_count).
 */
static int
i915_xd_dp_probe_read_sink_count(
	const struct i915_xd_dp_probe_env *env,
	struct i915_xd_dp_probe *sink)
{
	uint8_t count;
	long read;

	/* Only a branch of DPCD 1.1 or later reports a sink count. */
	if (sink->dpcd[I915_XD_DP_PROBE_DPCD_REV] < I915_XD_DP_PROBE_DPCD_REV_11)
		return 0;
	if (!sink->branch)
		return 0;

	/* Reads the count; a failed or short read is an error. */
	sink->has_sink_count = 1;
	read = env->dpcd_read(env->ctx, I915_XD_DP_PROBE_SINK_COUNT, &count, 1u);
	if (read < 0)
		return (int)read;
	if (read != 1)
		return -I915_XD_DP_PROBE_EIO;

	/* Bit 7 of the field is bit 6 of the count. */
	sink->sink_count = (int)(count & I915_XD_DP_PROBE_SINK_COUNT_LOW_MASK);
	if ((count & I915_XD_DP_PROBE_SINK_COUNT_HIGH_BIT) != 0u)
		sink->sink_count |= 0x40;

	/* No display behind the branch. */
	if (sink->sink_count == 0)
		return -I915_XD_DP_PROBE_EIO;

	/* Succeeded: a display is behind the branch. */
	return 0;
}

/*
 * Reads a branch's downstream port capabilities.
 *
 * A sink that is no branch, or a DPCD 1.0 one, has none to read, and a
 * branch may name zero ports.  Detailed capabilities take four bytes a
 * port, for at most four ports.  Returns 0, or the negative errno of the
 * read (-EIO for a short one).
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/display/drm_dp_helper.c (drm_dp_read_downstream_info).
 */
static int
i915_xd_dp_probe_read_downstream(
	const struct i915_xd_dp_probe_env *env,
	struct i915_xd_dp_probe *sink)
{
	unsigned length;
	long read;

	/* Nothing to read without a branch, or from a DPCD 1.0 one. */
	if (!sink->branch)
		return 0;
	if (sink->dpcd[I915_XD_DP_PROBE_DPCD_REV] == I915_XD_DP_PROBE_DPCD_REV_10)
		return 0;

	/* The ports the branch names, at most four when they are detailed. */
	length = sink->dpcd[I915_XD_DP_PROBE_DOWN_STREAM_PORT_COUNT] & I915_XD_DP_PROBE_PORT_COUNT_MASK;
	if ((sink->dpcd[I915_XD_DP_PROBE_DOWNSTREAMPORT_PRESENT] & I915_XD_DP_PROBE_DETAILED_CAP_INFO_AVAILABLE) != 0u) {
		if (length > I915_XD_DP_PROBE_DETAILED_PORTS_MAX)
			length = I915_XD_DP_PROBE_DETAILED_PORTS_MAX;

		/* Four bytes describe each port. */
		length *= I915_XD_DP_PROBE_DETAILED_PORT_SIZE;
	}

	/* Some branches name a downstream port and then zero of them. */
	if (length == 0u)
		return 0;

	/* Reads the capabilities; a short read is an I/O error. */
	read = env->dpcd_read(env->ctx, I915_XD_DP_PROBE_DOWNSTREAM_PORT_0, sink->downstream, length);
	if (read < 0)
		return (int)read;
	if (read != (long)length)
		return -I915_XD_DP_PROBE_EIO;

	/* Succeeded: the ports' capabilities are read. */
	return 0;
}

/*
 * Judges whether a display is behind the port once its DPCD was read
 * (MST is not supported).
 *
 * A sink that is no branch is the display.  A branch with hot plug detect
 * on its port is believed by its sink count; otherwise a DDC that answers
 * is a display; a port type with no EDID (VGA, or one without EDID) may
 * have a display the probe cannot see; anything else is out of
 * specification and ignored.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_dp.c (intel_dp_detect_dpcd).
 */
static enum i915_xd_dp_probe_status
i915_xd_dp_probe_classify(
	const struct i915_xd_dp_probe_env *env,
	struct i915_xd_dp_probe *sink)
{
	uint8_t type;
	int answered;

	/* A sink that is no branch is the display itself. */
	if (!sink->branch)
		return I915_XD_DP_PROBE_CONNECTED;

	/* A branch whose port detects its display counts it; the count was not zero. */
	if (sink->has_sink_count && (sink->downstream[0] & I915_XD_DP_PROBE_DS_PORT_HPD) != 0u)
		return I915_XD_DP_PROBE_CONNECTED;

	/* A branch without that is asked over its DDC. */
	answered = env->ddc_probe(env->ctx);
	if (answered)
		return I915_XD_DP_PROBE_CONNECTED;

	/* A port type without an EDID may still have a display (DPCD 1.1 port types, then the older ones). */
	if (sink->dpcd[I915_XD_DP_PROBE_DPCD_REV] >= I915_XD_DP_PROBE_DPCD_REV_11) {
		type = (uint8_t)(sink->downstream[0] & I915_XD_DP_PROBE_DS_PORT_TYPE_MASK);
		if (type == I915_XD_DP_PROBE_DS_PORT_TYPE_VGA || type == I915_XD_DP_PROBE_DS_PORT_TYPE_NON_EDID)
			return I915_XD_DP_PROBE_UNKNOWN;
	} else {
		type = (uint8_t)(sink->dpcd[I915_XD_DP_PROBE_DOWNSTREAMPORT_PRESENT] & I915_XD_DP_PROBE_DWN_STRM_PORT_TYPE_MASK);
		if (type == I915_XD_DP_PROBE_DWN_STRM_PORT_TYPE_ANALOG || type == I915_XD_DP_PROBE_DWN_STRM_PORT_TYPE_OTHER)
			return I915_XD_DP_PROBE_UNKNOWN;
	}

	/* A branch that gives no sign of a display is out of specification and ignored. */
	env->log(env->ctx, "i915: DP-ext: broken DP branch device (port 0 %02x), ignoring\n", sink->downstream[0]);
	return I915_XD_DP_PROBE_DISCONNECTED;
}

/*
 * Reads the display's EDID over the port's DDC.
 *
 * The valid blocks and the extensions the base block announces are kept;
 * a read without a valid block leaves no EDID.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/drm_edid.c
 * (drm_edid_read_ddc).
 */
static void
i915_xd_dp_probe_read_edid(
	const struct i915_xd_dp_probe_env *env,
	struct i915_xd_dp_probe *sink)
{
	unsigned extensions;
	int blocks;

	/* Reads the base block and up to three extensions. */
	extensions = 0u;
	blocks = env->edid_read(env->ctx, sink->edid, I915_XD_DP_PROBE_EDID_BLOCKS, &extensions);
	if (blocks < 1) {
		i915_xd_dp_probe_zero(sink->edid, sizeof(sink->edid));
		return;
	}

	/* Keeps the blocks read and the extensions announced. */
	sink->edid_blocks = (unsigned)blocks;
	sink->edid_extensions = extensions;
}

/*
 * Tells whether the EDID describes a DisplayPort input: EDID 1.4, a
 * digital input, the DisplayPort interface.
 *
 * Values referenced: VESA Enhanced EDID Standard, release A revision 2
 * (the base block's version and video input definition).
 */
static int
i915_xd_dp_probe_edid_is_digital_dp(
	const struct i915_xd_dp_probe *sink)
{
	uint8_t input;

	/* No EDID describes no input. */
	if (sink->edid_blocks == 0u)
		return 0;

	/* Only EDID 1.4 names the interface. */
	if (sink->edid[I915_XD_DP_PROBE_EDID_REVISION] < 4u)
		return 0;

	/* An analog input is no DP input. */
	input = sink->edid[I915_XD_DP_PROBE_EDID_INPUT];
	if ((input & I915_XD_DP_PROBE_EDID_INPUT_DIGITAL) == 0u)
		return 0;

	/* A digital input of another interface is no DP input. */
	if ((input & I915_XD_DP_PROBE_EDID_DIGITAL_TYPE_MASK) != I915_XD_DP_PROBE_EDID_DIGITAL_TYPE_DP)
		return 0;

	/* Succeeded: the display takes DisplayPort. */
	return 1;
}

/*
 * Tells whether the EDID announces an HDMI sink: a CTA extension with an
 * HDMI vendor-specific data block.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/drm_edid.c
 * (drm_parse_cea_ext).
 */
static int
i915_xd_dp_probe_edid_has_hdmi(
	const struct i915_xd_dp_probe *sink)
{
	unsigned block;
	int found;

	/* Looks at every extension block that was read. */
	for (block = 1u; block < sink->edid_blocks; block++) {
		/* Asks the extension. */
		found = i915_xd_dp_probe_cea_block_has_hdmi(&sink->edid[block * I915_XD_DP_PROBE_EDID_BLOCK_SIZE]);
		if (found)
			return 1;
	}

	/* No extension announces HDMI. */
	return 0;
}

/*
 * Tells whether one EDID extension block is a CTA extension with an HDMI
 * vendor-specific data block (IEEE OUI 00-0C-03, at least five bytes).
 */
static int
i915_xd_dp_probe_cea_block_has_hdmi(
	const uint8_t *block)
{
	unsigned end;
	unsigned at;
	unsigned tag;
	unsigned length;

	/* Only a CTA extension has data blocks. */
	if (block[0] != I915_XD_DP_PROBE_EDID_CEA_TAG)
		return 0;

	/* The data blocks run from byte 4 to the first detailed timing (byte 2), inside the block. */
	end = block[2];
	if (end < 4u || end > I915_XD_DP_PROBE_EDID_BLOCK_SIZE - 1u)
		return 0;

	/* Walks the data blocks: a header byte (tag, length), then the payload. */
	at = 4u;
	while (at < end) {
		tag = (unsigned)(block[at] >> 5);
		length = (unsigned)(block[at] & 0x1fu);

		/* A block running past the collection ends the walk. */
		if (at + 1u + length > end)
			break;

		/* The HDMI vendor block names the HDMI Licensing OUI, least significant byte first. */
		if (tag == I915_XD_DP_PROBE_CEA_VENDOR_BLOCK && length >= I915_XD_DP_PROBE_HDMI_VSDB_MIN_LENGTH) {
			if (block[at + 1u] == 0x03u &&
			    block[at + 2u] == 0x0cu &&
			    block[at + 3u] == 0x00u)
				return 1;
		}

		/* Steps over the header and the payload. */
		at += 1u + length;
	}

	/* No HDMI vendor block. */
	return 0;
}

/*
 * Takes a branch's downstream limits from its first port: the bits per
 * colour, the pixel clock and the TMDS clock range.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_dp.c (intel_dp_update_dfp).
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/display/drm_dp_helper.c (drm_dp_downstream_max_bpc,
 * drm_dp_downstream_max_dotclock).
 */
static void
i915_xd_dp_probe_set_downstream_limits(
	struct i915_xd_dp_probe *sink)
{
	/* The limits of the first downstream port; a sink that is no branch has none. */
	sink->max_bpc = i915_xd_dp_probe_downstream_max_bpc(sink);
	sink->max_dotclock_khz = i915_xd_dp_probe_downstream_max_dotclock(sink);
	sink->min_tmds_clock_khz = i915_xd_dp_probe_downstream_min_tmds(sink);
	sink->max_tmds_clock_khz = i915_xd_dp_probe_downstream_max_tmds(sink);
}

/*
 * Reports the highest TMDS clock a branch's HDMI or DVI port sends, in
 * kHz, or 0 when it sets no limit.
 *
 * Without detailed capabilities an HDMI port is taken for 300 MHz (an HDMI
 * 1.4 converter: DPCD 1.4 HDMI 2.0 ports must give details) and a DVI port
 * for 165 MHz; a dual-mode port in front of a DP display sets no limit.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/display/drm_dp_helper.c
 * (drm_dp_downstream_max_tmds_clock).
 */
static int
i915_xd_dp_probe_downstream_max_tmds(
	const struct i915_xd_dp_probe *sink)
{
	uint8_t type;
	int detailed;
	int dp_display;

	/* A sink that is no branch sets no limit. */
	if (!sink->branch)
		return 0;

	/* Before DPCD 1.1 only the port type is known: TMDS is single-link DVI. */
	if (sink->dpcd[I915_XD_DP_PROBE_DPCD_REV] < I915_XD_DP_PROBE_DPCD_REV_11) {
		if ((sink->dpcd[I915_XD_DP_PROBE_DOWNSTREAMPORT_PRESENT] & I915_XD_DP_PROBE_DWN_STRM_PORT_TYPE_MASK) == I915_XD_DP_PROBE_DWN_STRM_PORT_TYPE_TMDS)
			return 165000;

		/* Any other type sets no limit. */
		return 0;
	}

	/* The port type, and whether its capabilities are detailed. */
	type = (uint8_t)(sink->downstream[0] & I915_XD_DP_PROBE_DS_PORT_TYPE_MASK);
	detailed = 0;
	if ((sink->dpcd[I915_XD_DP_PROBE_DOWNSTREAMPORT_PRESENT] & I915_XD_DP_PROBE_DETAILED_CAP_INFO_AVAILABLE) != 0u)
		detailed = 1;

	/* A dual-mode port in front of a DP display sets no limit. */
	if (type == I915_XD_DP_PROBE_DS_PORT_TYPE_DP_DUALMODE) {
		dp_display = i915_xd_dp_probe_edid_is_digital_dp(sink);
		if (dp_display)
			return 0;
	}

	/* An HDMI port, or a dual-mode one sending HDMI: 300 MHz, or its detailed limit in 2.5 MHz units. */
	if (type == I915_XD_DP_PROBE_DS_PORT_TYPE_DP_DUALMODE || type == I915_XD_DP_PROBE_DS_PORT_TYPE_HDMI) {
		if (!detailed)
			return 300000;

		/* The detailed limit. */
		return (int)sink->downstream[1] * 2500;
	}

	/* A DVI port: 165 MHz, or its detailed limit in 2.5 MHz units. */
	if (type == I915_XD_DP_PROBE_DS_PORT_TYPE_DVI) {
		if (!detailed)
			return 165000;

		/* The detailed limit. */
		return (int)sink->downstream[1] * 2500;
	}

	/* Any other port sets no TMDS limit. */
	return 0;
}

/*
 * Reports the lowest TMDS clock a branch's HDMI or DVI port sends, in kHz,
 * or 0 when it sets no limit: 25 MHz, as the converter cannot be assumed
 * to repeat pixels.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/display/drm_dp_helper.c
 * (drm_dp_downstream_min_tmds_clock).
 */
static int
i915_xd_dp_probe_downstream_min_tmds(
	const struct i915_xd_dp_probe *sink)
{
	uint8_t type;
	int dp_display;

	/* A sink that is no branch sets no limit. */
	if (!sink->branch)
		return 0;

	/* Before DPCD 1.1 only the port type is known. */
	if (sink->dpcd[I915_XD_DP_PROBE_DPCD_REV] < I915_XD_DP_PROBE_DPCD_REV_11) {
		if ((sink->dpcd[I915_XD_DP_PROBE_DOWNSTREAMPORT_PRESENT] & I915_XD_DP_PROBE_DWN_STRM_PORT_TYPE_MASK) == I915_XD_DP_PROBE_DWN_STRM_PORT_TYPE_TMDS)
			return 25000;

		/* Any other type sets no limit. */
		return 0;
	}

	/* A dual-mode port in front of a DP display sets no limit. */
	type = (uint8_t)(sink->downstream[0] & I915_XD_DP_PROBE_DS_PORT_TYPE_MASK);
	if (type == I915_XD_DP_PROBE_DS_PORT_TYPE_DP_DUALMODE) {
		dp_display = i915_xd_dp_probe_edid_is_digital_dp(sink);
		if (dp_display)
			return 0;
	}

	/* A dual-mode, DVI or HDMI port sends at least 25 MHz. */
	if (type == I915_XD_DP_PROBE_DS_PORT_TYPE_DP_DUALMODE ||
	    type == I915_XD_DP_PROBE_DS_PORT_TYPE_DVI ||
	    type == I915_XD_DP_PROBE_DS_PORT_TYPE_HDMI)
		return 25000;

	/* Any other port sets no TMDS limit. */
	return 0;
}

/*
 * Reports the highest pixel clock a branch's VGA port takes, in kHz, or 0
 * when it sets no limit.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/display/drm_dp_helper.c
 * (drm_dp_downstream_max_dotclock).
 */
static int
i915_xd_dp_probe_downstream_max_dotclock(
	const struct i915_xd_dp_probe *sink)
{
	uint8_t type;

	/* A sink that is no branch, or one before DPCD 1.1, sets no limit. */
	if (!sink->branch)
		return 0;
	if (sink->dpcd[I915_XD_DP_PROBE_DPCD_REV] < I915_XD_DP_PROBE_DPCD_REV_11)
		return 0;

	/* Only a VGA port with detailed capabilities names one, in 8 MHz units. */
	type = (uint8_t)(sink->downstream[0] & I915_XD_DP_PROBE_DS_PORT_TYPE_MASK);
	if (type != I915_XD_DP_PROBE_DS_PORT_TYPE_VGA)
		return 0;
	if ((sink->dpcd[I915_XD_DP_PROBE_DOWNSTREAMPORT_PRESENT] & I915_XD_DP_PROBE_DETAILED_CAP_INFO_AVAILABLE) == 0u)
		return 0;

	/* Succeeded: the VGA port's limit. */
	return (int)sink->downstream[1] * 8000;
}

/*
 * Reports the most bits per colour a branch's port passes, or 0 when it
 * sets no limit.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/display/drm_dp_helper.c (drm_dp_downstream_max_bpc).
 */
static int
i915_xd_dp_probe_downstream_max_bpc(
	const struct i915_xd_dp_probe *sink)
{
	uint8_t type;
	int dp_display;

	/* A sink that is no branch sets no limit. */
	if (!sink->branch)
		return 0;

	/* Before DPCD 1.1: a DP port sets no limit, any other passes 8. */
	if (sink->dpcd[I915_XD_DP_PROBE_DPCD_REV] < I915_XD_DP_PROBE_DPCD_REV_11) {
		if ((sink->dpcd[I915_XD_DP_PROBE_DOWNSTREAMPORT_PRESENT] & I915_XD_DP_PROBE_DWN_STRM_PORT_TYPE_MASK) == I915_XD_DP_PROBE_DWN_STRM_PORT_TYPE_DP)
			return 0;

		/* Any other type passes 8. */
		return 8;
	}

	/* A DP port, and a dual-mode one in front of a DP display, set no limit. */
	type = (uint8_t)(sink->downstream[0] & I915_XD_DP_PROBE_DS_PORT_TYPE_MASK);
	if (type == I915_XD_DP_PROBE_DS_PORT_TYPE_DP)
		return 0;
	if (type == I915_XD_DP_PROBE_DS_PORT_TYPE_DP_DUALMODE) {
		dp_display = i915_xd_dp_probe_edid_is_digital_dp(sink);
		if (dp_display)
			return 0;
	}

	/* Only dual-mode, HDMI, DVI and VGA ports with detailed capabilities name their limit. */
	if (type != I915_XD_DP_PROBE_DS_PORT_TYPE_DP_DUALMODE &&
	    type != I915_XD_DP_PROBE_DS_PORT_TYPE_HDMI &&
	    type != I915_XD_DP_PROBE_DS_PORT_TYPE_DVI &&
	    type != I915_XD_DP_PROBE_DS_PORT_TYPE_VGA)
		return 8;
	if ((sink->dpcd[I915_XD_DP_PROBE_DOWNSTREAMPORT_PRESENT] & I915_XD_DP_PROBE_DETAILED_CAP_INFO_AVAILABLE) == 0u)
		return 8;

	/* The detailed limit: 8, 10, 12 or 16 bits per colour. */
	switch (sink->downstream[2] & I915_XD_DP_PROBE_DS_MAX_BPC_MASK) {
	case 1u:
		return 10;
	case 2u:
		return 12;
	case 3u:
		return 16;
	default:
		break;
	}

	/* Succeeded: 8 bits per colour. */
	return 8;
}

/* Writes one DPCD byte: 0, or a negative errno (-EIO for a short write). */
static int
i915_xd_dp_probe_dpcd_writeb(
	const struct i915_xd_dp_probe_env *env,
	unsigned offset,
	uint8_t value)
{
	long written;

	/* Writes the byte. */
	written = env->dpcd_write(env->ctx, offset, &value, 1u);
	if (written < 0)
		return (int)written;
	if (written != 1)
		return -I915_XD_DP_PROBE_EIO;

	/* Succeeded: the sink took the byte. */
	return 0;
}

/*
 * Acknowledges one service interrupt vector of the sink: a nonzero vector
 * is written back, which clears it, and logged.
 */
static void
i915_xd_dp_probe_ack_service_irq(
	const struct i915_xd_dp_probe_env *env,
	unsigned offset,
	const char *what)
{
	uint8_t vector;
	long read;
	int written;

	/* Reads the vector; nothing raised, or nothing read, needs no acknowledgement. */
	read = env->dpcd_read(env->ctx, offset, &vector, 1u);
	if (read != 1)
		return;
	if (vector == 0u)
		return;

	/* Writes the raised bits back, which clears them, and logs what was not handled. */
	written = i915_xd_dp_probe_dpcd_writeb(env, offset, vector);
	env->log(env->ctx, "i915: DP-ext: %s service interrupt 0x%02x acknowledged (rc %d), not handled\n", what, vector, written);
}

/* Keeps the first of a series of errors: the one already kept, or this one. */
static int
i915_xd_dp_probe_first_error(
	int first_error,
	int error)
{
	/* An error already kept stays. */
	if (first_error != 0)
		return first_error;

	/* Succeeded: this error is the first. */
	return error;
}

/* Tells whether two byte strings are the same: 1 or 0. */
static int
i915_xd_dp_probe_same_bytes(
	const uint8_t *a,
	const uint8_t *b,
	size_t size)
{
	size_t i;

	/* Compares byte by byte. */
	for (i = 0u; i < size; i++) {
		/* A byte that differs ends the comparison. */
		if (a[i] != b[i])
			return 0;
	}

	/* Succeeded: every byte is the same. */
	return 1;
}

/* Clears bytes; the core links against neither the kernel nor libc. */
static void
i915_xd_dp_probe_zero(
	void *bytes,
	size_t size)
{
	uint8_t *byte;
	size_t i;

	/* Clears each byte. */
	byte = bytes;
	for (i = 0u; i < size; i++)
		byte[i] = 0u;
}
