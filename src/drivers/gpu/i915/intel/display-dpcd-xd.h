/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The addresses and fields of a DisplayPort sink's configuration data
 * (DPCD) that the xd display code (display/xd-dp.c) reads and writes
 * while it sets up and trains a link.
 *
 * The DPCD is the sink's register space, reached over the AUX channel.
 * The receiver capabilities sit at the start of it, the link settings
 * the source writes at 0x100, the link status the sink reports at 0x202,
 * and the settings and status of each link training tunable repeater
 * (LTTPR) in a range of their own at 0xf0000.  Each name has the
 * I915_XD_DPCD_ prefix.
 */

#ifndef DRIVERS_GPU_I915_INTEL_DISPLAY_DPCD_XD_H
#define DRIVERS_GPU_I915_INTEL_DISPLAY_DPCD_XD_H

/*
 * The receiver capabilities: the DPCD revision (training patterns 3 and 4
 * count from revisions 1.2 and 1.4), the highest link rate
 * code, the lane count (with the training pattern 3 bit), the
 * training pattern 4 bit, the training read interval, and where an eDP
 * 1.4 sink lists its link rates (8 entries of 16 bits in units of
 * 200 kHz, little endian, a zero entry ends the list).  The capabilities
 * a source reads in one go are the first 15 bytes.
 *
 * Values referenced: Linux v6.8.12 include/drm/display/drm_dp.h
 * (DP_DPCD_REV, DP_DPCD_REV_12, DP_DPCD_REV_14, DP_MAX_LINK_RATE, DP_MAX_LANE_COUNT,
 * DP_MAX_LANE_COUNT_MASK, DP_TPS3_SUPPORTED, DP_MAX_DOWNSPREAD,
 * DP_TPS4_SUPPORTED, DP_TRAINING_AUX_RD_INTERVAL,
 * DP_TRAINING_AUX_RD_MASK, DP_SUPPORTED_LINK_RATES,
 * DP_MAX_SUPPORTED_RATES, DP_RECEIVER_CAP_SIZE).
 */
#define I915_XD_DPCD_REV			0x000U
#define I915_XD_DPCD_REV_1_2			0x12U
#define I915_XD_DPCD_REV_1_4			0x14U
#define I915_XD_DPCD_MAX_RATE			0x001U
#define I915_XD_DPCD_MAX_LANES			0x002U
#define I915_XD_DPCD_LANES_MASK			0x1fU
#define I915_XD_DPCD_TPS3			0x40U
#define I915_XD_DPCD_MAX_DOWNSPREAD		0x003U
#define I915_XD_DPCD_TPS4			0x80U
#define I915_XD_DPCD_READ_INTERVAL		0x00eU
#define I915_XD_DPCD_READ_INTERVAL_MASK		0x7fU
#define I915_XD_DPCD_EDP_RATES			0x010U
#define I915_XD_DPCD_EDP_RATE_COUNT		8U
#define I915_XD_DPCD_CAPS_SIZE			15U

/*
 * The codes of the 8b/10b link rates in the rate setting: RBR, HBR,
 * HBR2 and HBR3 (units of 0.27 Gbit/s).
 *
 * Values referenced: Linux v6.8.12 include/drm/display/drm_dp.h
 * (DP_LINK_BW_1_62, DP_LINK_BW_2_7, DP_LINK_BW_5_4, DP_LINK_BW_8_1).
 */
#define I915_XD_DPCD_BW_RBR			0x06U
#define I915_XD_DPCD_BW_HBR			0x0aU
#define I915_XD_DPCD_BW_HBR2			0x14U
#define I915_XD_DPCD_BW_HBR3			0x1eU

/*
 * The link settings the source writes: the rate code, the lane count
 * with the enhanced framing bit, the training pattern (with the bit that
 * turns scrambling off) followed by one level byte per lane, the
 * downspread control (with the bit that tells an eDP sink to ignore the
 * stream's timing attributes) followed by the channel coding (8b/10b),
 * the index into an eDP 1.4 sink's rate list, and the sink's power
 * state.
 *
 * Values referenced: Linux v6.8.12 include/drm/display/drm_dp.h
 * (DP_LINK_BW_SET, DP_LANE_COUNT_SET, DP_LANE_COUNT_ENHANCED_FRAME_EN,
 * DP_TRAINING_PATTERN_SET, DP_TRAINING_PATTERN_DISABLE,
 * DP_TRAINING_PATTERN_1, DP_TRAINING_PATTERN_2, DP_TRAINING_PATTERN_3,
 * DP_TRAINING_PATTERN_4, DP_LINK_SCRAMBLING_DISABLE,
 * DP_TRAINING_LANE0_SET, DP_DOWNSPREAD_CTRL, DP_MSA_TIMING_PAR_IGNORE_EN,
 * DP_SET_ANSI_8B10B, DP_LINK_RATE_SET).
 */
#define I915_XD_DPCD_BW_SET			0x100U
#define I915_XD_DPCD_LANES_SET			0x101U
#define I915_XD_DPCD_LANES_ENHANCED		0x80U
#define I915_XD_DPCD_PATTERN_SET		0x102U
#define I915_XD_DPCD_PATTERN_NONE		0x00U
#define I915_XD_DPCD_PATTERN_1			0x01U
#define I915_XD_DPCD_PATTERN_2			0x02U
#define I915_XD_DPCD_PATTERN_3			0x03U
#define I915_XD_DPCD_PATTERN_4			0x07U
#define I915_XD_DPCD_PATTERN_NO_SCRAMBLE	0x20U
#define I915_XD_DPCD_LEVELS_SET			0x103U
#define I915_XD_DPCD_DOWNSPREAD			0x107U
#define I915_XD_DPCD_MSA_IGNORE			0x80U
#define I915_XD_DPCD_CODING_8B10B		0x01U
#define I915_XD_DPCD_RATE_SET			0x115U

/*
 * One lane's level byte: the voltage swing level and the bit that says
 * it is the highest the source drives, the pre-emphasis level and the
 * bit that says it is the highest.
 *
 * Values referenced: Linux v6.8.12 include/drm/display/drm_dp.h
 * (DP_TRAIN_VOLTAGE_SWING_MASK, DP_TRAIN_MAX_SWING_REACHED,
 * DP_TRAIN_PRE_EMPHASIS_MASK, DP_TRAIN_PRE_EMPHASIS_SHIFT,
 * DP_TRAIN_MAX_PRE_EMPHASIS_REACHED).
 */
#define I915_XD_DPCD_SWING			0x03U
#define I915_XD_DPCD_SWING_TOP			0x04U
#define I915_XD_DPCD_PREEMPH			0x18U
#define I915_XD_DPCD_PREEMPH_SHIFT		3U
#define I915_XD_DPCD_PREEMPH_TOP		0x20U

/*
 * The link status the sink reports: two bytes of lane status (a nibble
 * per lane: clock recovered, channel equalized, symbols locked), the
 * alignment of the lanes, the sink status, and two bytes of level
 * requests (a nibble per lane: the swing in the low two bits, the
 * pre-emphasis in the next two).  The six bytes are read together.
 *
 * Values referenced: Linux v6.8.12 include/drm/display/drm_dp.h
 * (DP_LANE0_1_STATUS, DP_LANE_CR_DONE, DP_LANE_CHANNEL_EQ_DONE,
 * DP_LANE_SYMBOL_LOCKED, DP_LANE_ALIGN_STATUS_UPDATED,
 * DP_INTERLANE_ALIGN_DONE, DP_SINK_STATUS, DP_ADJUST_REQUEST_LANE0_1,
 * DP_LINK_STATUS_SIZE).
 */
#define I915_XD_DPCD_LANE_STATUS		0x202U
#define I915_XD_DPCD_LANE_CR			0x1U
#define I915_XD_DPCD_LANE_EQ			0x2U
#define I915_XD_DPCD_LANE_LOCKED		0x4U
#define I915_XD_DPCD_ALIGN			0x204U
#define I915_XD_DPCD_ALIGN_DONE			0x01U
#define I915_XD_DPCD_SINK_STATUS		0x205U
#define I915_XD_DPCD_REQUEST			0x206U
#define I915_XD_DPCD_STATUS_SIZE		6U

/*
 * The sink's power state: D0 (normal operation) and D3 (power down, the
 * AUX channel still answers), and the mask of the field.
 *
 * Values referenced: Linux v6.8.12 include/drm/display/drm_dp.h
 * (DP_SET_POWER, DP_SET_POWER_D0, DP_SET_POWER_D3, DP_SET_POWER_MASK).
 */
#define I915_XD_DPCD_SET_POWER			0x600U
#define I915_XD_DPCD_POWER_D0			0x01U
#define I915_XD_DPCD_POWER_D3			0x02U
#define I915_XD_DPCD_POWER_MASK			0x07U

/*
 * The AUX message of a native DPCD access: the request codes in the top
 * of the first byte (with the top four bits of the 20-bit address in its
 * bottom), and the native reply codes in the top of the reply's first
 * byte (the field's mask after the shift, acknowledgement, refusal and
 * the sink's request to retry later).
 *
 * Values referenced: Linux v6.8.12 include/drm/display/drm_dp.h
 * (DP_AUX_NATIVE_WRITE, DP_AUX_NATIVE_READ, DP_AUX_NATIVE_REPLY_MASK,
 * DP_AUX_NATIVE_REPLY_ACK, DP_AUX_NATIVE_REPLY_NACK,
 * DP_AUX_NATIVE_REPLY_DEFER).
 */
#define I915_XD_AUX_NATIVE_WRITE		0x8U
#define I915_XD_AUX_NATIVE_READ			0x9U
#define I915_XD_AUX_NATIVE_REPLY_MASK		0x3U
#define I915_XD_AUX_NATIVE_ACK			0x0U
#define I915_XD_AUX_NATIVE_NACK			0x1U
#define I915_XD_AUX_NATIVE_DEFER		0x2U

/*
 * The AUX message of an I2C transfer over the channel: the request codes
 * (a write, a read, a write's status asked again) with the bit that keeps
 * the I2C transaction open after the message (middle of transaction),
 * and the I2C reply codes, which sit above the native ones in the top of
 * the reply's first byte (the field's mask after the shift,
 * acknowledgement, refusal and the request to retry later).
 *
 * Values referenced: Linux v6.8.12 include/drm/display/drm_dp.h
 * (DP_AUX_I2C_WRITE, DP_AUX_I2C_READ, DP_AUX_I2C_WRITE_STATUS_UPDATE,
 * DP_AUX_I2C_MOT, DP_AUX_I2C_REPLY_MASK, DP_AUX_I2C_REPLY_ACK,
 * DP_AUX_I2C_REPLY_NACK, DP_AUX_I2C_REPLY_DEFER).
 */
#define I915_XD_AUX_I2C_WRITE			0x0U
#define I915_XD_AUX_I2C_READ			0x1U
#define I915_XD_AUX_I2C_WRITE_STATUS		0x2U
#define I915_XD_AUX_I2C_MOT			0x4U
#define I915_XD_AUX_I2C_REPLY_MASK		0xcU
#define I915_XD_AUX_I2C_ACK			0x0U
#define I915_XD_AUX_I2C_NACK			0x4U
#define I915_XD_AUX_I2C_DEFER			0x8U

/*
 * The capabilities a sink adds to the receiver's: the bit of the
 * training read interval byte that says the extended receiver
 * capabilities exist and where they are (the same fifteen bytes, which
 * replace the receiver's when the revision there is not lower), and the
 * eDP display control capabilities (their revision first, three bytes).
 *
 * Values referenced: Linux v6.8.12 include/drm/display/drm_dp.h
 * (DP_EXTENDED_RECEIVER_CAP_FIELD_PRESENT, DP_DP13_DPCD_REV,
 * DP_EDP_DPCD_REV, EDP_DISPLAY_CTL_CAP_SIZE).
 */
#define I915_XD_DPCD_EXTENDED_PRESENT		0x80U
#define I915_XD_DPCD_EXTENDED_REV		0x2200U
#define I915_XD_DPCD_EDP_REV			0x700U
#define I915_XD_DPCD_EDP_CAPS_SIZE		3U

/*
 * The repeaters' common range: the structure revision, the repeater
 * count (one bit per count, bit 7 for one repeater), the repeater mode
 * (transparent or not); the eight bytes are read together.  Each
 * repeater then has a range of 0x50 bytes of its own, from repeater 1's:
 * its training pattern followed by its lanes' levels, its training read
 * interval and transmitter capability (swing and pre-emphasis level 3),
 * and its link status (five bytes, without the sink status).
 *
 * Values referenced: Linux v6.8.12 include/drm/display/drm_dp.h
 * (DP_LT_TUNABLE_PHY_REPEATER_FIELD_DATA_STRUCTURE_REV,
 * DP_PHY_REPEATER_CNT, DP_PHY_REPEATER_MODE,
 * DP_PHY_REPEATER_MODE_TRANSPARENT, DP_PHY_REPEATER_MODE_NON_TRANSPARENT,
 * DP_LTTPR_COMMON_CAP_SIZE, __DP_LTTPR1_BASE, __DP_LTTPR2_BASE,
 * DP_TRAINING_PATTERN_SET_PHY_REPEATER1,
 * DP_TRAINING_LANE0_SET_PHY_REPEATER1,
 * DP_TRAINING_AUX_RD_INTERVAL_PHY_REPEATER1,
 * DP_TRANSMITTER_CAPABILITY_PHY_REPEATER1,
 * DP_VOLTAGE_SWING_LEVEL_3_SUPPORTED, DP_PRE_EMPHASIS_LEVEL_3_SUPPORTED,
 * DP_LTTPR_PHY_CAP_SIZE, DP_LANE0_1_STATUS_PHY_REPEATER1).
 */
#define I915_XD_DPCD_REPEATER_REV		0xf0000U
#define I915_XD_DPCD_REPEATER_REV_1_4		0x14U
#define I915_XD_DPCD_REPEATER_COUNT		0xf0002U
#define I915_XD_DPCD_REPEATER_MODE		0xf0003U
#define I915_XD_DPCD_REPEATER_TRANSPARENT	0x55U
#define I915_XD_DPCD_REPEATER_OPAQUE		0xaaU
#define I915_XD_DPCD_REPEATER_COMMON_SIZE	8U
#define I915_XD_DPCD_REPEATER_STRIDE		0x50U
#define I915_XD_DPCD_REPEATER_PATTERN(n)	(0xf0010U + I915_XD_DPCD_REPEATER_STRIDE * ((n) - 1U))
#define I915_XD_DPCD_REPEATER_LEVELS(n)		(0xf0011U + I915_XD_DPCD_REPEATER_STRIDE * ((n) - 1U))
#define I915_XD_DPCD_REPEATER_INTERVAL(n)	(0xf0020U + I915_XD_DPCD_REPEATER_STRIDE * ((n) - 1U))
#define I915_XD_DPCD_REPEATER_TX_CAP		0xf0021U
#define I915_XD_DPCD_REPEATER_SWING_3		0x01U
#define I915_XD_DPCD_REPEATER_PREEMPH_3		0x02U
#define I915_XD_DPCD_REPEATER_CAP_SIZE		3U
#define I915_XD_DPCD_REPEATER_STATUS(n)		(0xf0030U + I915_XD_DPCD_REPEATER_STRIDE * ((n) - 1U))
#define I915_XD_DPCD_REPEATER_MAX		8U

#endif /* DRIVERS_GPU_I915_INTEL_DISPLAY_DPCD_XD_H */
