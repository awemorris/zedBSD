/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The sink of an external DisplayPort port (see dp-ext.h).
 *
 * The DPCD addresses and fields are the DisplayPort standard's, as Linux
 * v6.8.12 include/drm/display/drm_dp.h names them; the steps follow the
 * Linux intel_dp.c, intel_dp_link_training.c and drm_dp_helper.c (MIT).
 */

#include "dp-ext.h"

/* The receiver capabilities (DPCD 0x000 to 0x00e). */
#define I915_DP_EXT_DPCD_REV			0x000u
#define I915_DP_EXT_MAX_LINK_RATE		0x001u
#define I915_DP_EXT_MAX_LANE_COUNT		0x002u
#define I915_DP_EXT_MAX_LANE_COUNT_MASK		0x1fu
#define I915_DP_EXT_DOWNSTREAMPORT_PRESENT	0x005u
#define I915_DP_EXT_DOWN_STREAM_PORT_COUNT	0x007u
#define I915_DP_EXT_PORT_COUNT_MASK		0x0fu

/* DP_DOWNSTREAMPORT_PRESENT: a branch, its port type before DPCD 1.1, and detailed port capabilities. */
#define I915_DP_EXT_DWN_STRM_PORT_PRESENT	0x01u
#define I915_DP_EXT_DWN_STRM_PORT_TYPE_MASK	0x06u
#define I915_DP_EXT_DWN_STRM_PORT_TYPE_DP	0x00u
#define I915_DP_EXT_DWN_STRM_PORT_TYPE_ANALOG	0x02u
#define I915_DP_EXT_DWN_STRM_PORT_TYPE_TMDS	0x04u
#define I915_DP_EXT_DWN_STRM_PORT_TYPE_OTHER	0x06u
#define I915_DP_EXT_DETAILED_CAP_INFO_AVAILABLE	0x10u

/* The DPCD revisions the probe tells apart. */
#define I915_DP_EXT_DPCD_REV_10			0x10u
#define I915_DP_EXT_DPCD_REV_11			0x11u
#define I915_DP_EXT_DPCD_REV_13			0x13u
#define I915_DP_EXT_DPCD_REV_14			0x14u

/* The branch's downstream port capabilities (DPCD 0x080), 4 bytes a port when detailed. */
#define I915_DP_EXT_DOWNSTREAM_PORT_0		0x080u
#define I915_DP_EXT_DS_PORT_TYPE_MASK		0x07u
#define I915_DP_EXT_DS_PORT_TYPE_DP		0u
#define I915_DP_EXT_DS_PORT_TYPE_VGA		1u
#define I915_DP_EXT_DS_PORT_TYPE_DVI		2u
#define I915_DP_EXT_DS_PORT_TYPE_HDMI		3u
#define I915_DP_EXT_DS_PORT_TYPE_NON_EDID	4u
#define I915_DP_EXT_DS_PORT_TYPE_DP_DUALMODE	5u
#define I915_DP_EXT_DS_PORT_HPD			0x08u
#define I915_DP_EXT_DS_MAX_BPC_MASK		0x03u
#define I915_DP_EXT_DETAILED_PORTS_MAX		4u
#define I915_DP_EXT_DETAILED_PORT_SIZE		4u

/* The sink count (DPCD 0x200): bits 0..5 and bit 7 as bit 6. */
#define I915_DP_EXT_SINK_COUNT			0x200u
#define I915_DP_EXT_SINK_COUNT_LOW_MASK		0x3fu
#define I915_DP_EXT_SINK_COUNT_HIGH_BIT		0x80u

/* The sink's and the branch's identification (DPCD 0x400 and 0x500, 12 bytes). */
#define I915_DP_EXT_SINK_OUI			0x400u
#define I915_DP_EXT_BRANCH_OUI			0x500u
#define I915_DP_EXT_DESC_SIZE			12u

/* The sink's service interrupt vectors, which a short pulse raises (DPCD 0x201 and 0x2005). */
#define I915_DP_EXT_DEVICE_SERVICE_IRQ_VECTOR	0x201u
#define I915_DP_EXT_LINK_SERVICE_IRQ_VECTOR_ESI0	0x2005u

/*
 * The DPRX's link status (DP_LANE0_1_STATUS onwards, 6 bytes): a 4-bit
 * field per lane, two lanes a byte (lane 0 in the low bits), whose clock
 * recovery, channel equalisation and symbol lock must all be done, and
 * the inter-lane alignment bit of DP_LANE_ALIGN_STATUS_UPDATED.
 */
#define I915_DP_EXT_LANE0_1_STATUS		0x202u
#define I915_DP_EXT_LINK_STATUS_SIZE		6u
#define I915_DP_EXT_LANE_ALIGN_INDEX		2u
#define I915_DP_EXT_INTERLANE_ALIGN_DONE	0x01u
#define I915_DP_EXT_LANE_EQ_BITS		0x07u
#define I915_DP_EXT_LANE_STATUS_BITS		4u
#define I915_DP_EXT_LANES_MAX			4

/* The protocol converter controls of a DPCD 1.3 branch (DPCD 0x3050 to 0x3052). */
#define I915_DP_EXT_CONVERTER_CONTROL_0		0x3050u
#define I915_DP_EXT_CONVERTER_CONTROL_1		0x3051u
#define I915_DP_EXT_CONVERTER_CONTROL_2		0x3052u
#define I915_DP_EXT_HDMI_DVI_OUTPUT_CONFIG	0x01u
#define I915_DP_EXT_CONVERSION_RGB_YCBCR_MASK	0x70u

/* The repeaters' common capabilities (DPCD 0xf0000): revision, rate, count, lanes. */
#define I915_DP_EXT_LTTPR_CAPS			0xf0000u
#define I915_DP_EXT_LTTPR_REV			0u
#define I915_DP_EXT_LTTPR_MAX_LINK_RATE		1u
#define I915_DP_EXT_LTTPR_COUNT			2u
#define I915_DP_EXT_LTTPR_MAX_LANE_COUNT	4u
#define I915_DP_EXT_LTTPR_MIN_REV		0x14u

/* A link bandwidth code is the rate in units of 0.27 Gbit/s; three codes name 128b/132b rates. */
#define I915_DP_EXT_LINK_BW_UNIT		27000
#define I915_DP_EXT_LINK_BW_10			0x01u
#define I915_DP_EXT_LINK_BW_20			0x02u
#define I915_DP_EXT_LINK_BW_13_5		0x04u

/* The EDID bytes the probe reads: the revision, the input definition, and the extension tags. */
#define I915_DP_EXT_EDID_REVISION		19u
#define I915_DP_EXT_EDID_INPUT			20u
#define I915_DP_EXT_EDID_INPUT_DIGITAL		0x80u
#define I915_DP_EXT_EDID_DIGITAL_TYPE_MASK	0x0fu
#define I915_DP_EXT_EDID_DIGITAL_TYPE_DP	0x05u
#define I915_DP_EXT_EDID_CEA_TAG		0x02u
#define I915_DP_EXT_CEA_VENDOR_BLOCK		3u
#define I915_DP_EXT_HDMI_VSDB_MIN_LENGTH	5u

/* The Linux error the probe reports for a short read (-EIO). */
#define I915_DP_EXT_EIO				5

/* The rate the defaults fall back to: RBR. */
#define I915_DP_EXT_RATE_RBR			162000

/* The 8b/10b link rates a sink can offer (RBR, HBR, HBR2, HBR3). */
static const int i915_dp_ext_sink_rate_table[] = {
	162000, 270000, 540000, 810000
};

/*
 * The 8b/10b link rates display versions 11 to 13 offer (the Linux
 * icl_rates); the 128b/132b rates of that table lie above every display
 * version 13 limit and are left out.
 */
static const int i915_dp_ext_source_rate_table[] = {
	162000, 216000, 270000, 324000, 432000, 540000, 648000, 810000
};

static int i915_dp_ext_bw_code_to_rate(uint8_t code);
static void i915_dp_ext_read_lttpr(const struct i915_dp_ext_env *env, struct i915_dp_ext_sink *sink);
static int i915_dp_ext_lttpr_count(uint8_t count_bits);
static int i915_dp_ext_read_caps(const struct i915_dp_ext_env *env, struct i915_dp_ext_sink *sink);
static void i915_dp_ext_read_desc(const struct i915_dp_ext_env *env, struct i915_dp_ext_sink *sink);
static void i915_dp_ext_set_rates(const struct i915_dp_ext_env *env, const struct i915_dp_ext_source *source, struct i915_dp_ext_sink *sink);
static void i915_dp_ext_set_lanes(const struct i915_dp_ext_env *env, const struct i915_dp_ext_source *source, struct i915_dp_ext_sink *sink);
static int i915_dp_ext_read_sink_count(const struct i915_dp_ext_env *env, struct i915_dp_ext_sink *sink);
static int i915_dp_ext_read_downstream(const struct i915_dp_ext_env *env, struct i915_dp_ext_sink *sink);
static enum i915_dp_ext_status i915_dp_ext_classify(const struct i915_dp_ext_env *env, struct i915_dp_ext_sink *sink);
static void i915_dp_ext_read_edid(const struct i915_dp_ext_env *env, struct i915_dp_ext_sink *sink);
static int i915_dp_ext_edid_is_digital_dp(const struct i915_dp_ext_sink *sink);
static int i915_dp_ext_edid_has_hdmi(const struct i915_dp_ext_sink *sink);
static int i915_dp_ext_cea_block_has_hdmi(const uint8_t *block);
static void i915_dp_ext_set_downstream_limits(struct i915_dp_ext_sink *sink);
static int i915_dp_ext_downstream_max_tmds(const struct i915_dp_ext_sink *sink);
static int i915_dp_ext_downstream_min_tmds(const struct i915_dp_ext_sink *sink);
static int i915_dp_ext_downstream_max_dotclock(const struct i915_dp_ext_sink *sink);
static int i915_dp_ext_downstream_max_bpc(const struct i915_dp_ext_sink *sink);
static int i915_dp_ext_dpcd_writeb(const struct i915_dp_ext_env *env, unsigned offset, uint8_t value);
static void i915_dp_ext_ack_service_irq(const struct i915_dp_ext_env *env, unsigned offset, const char *what);
static int i915_dp_ext_first_error(int first_error, int error);
static int i915_dp_ext_same_bytes(const uint8_t *a, const uint8_t *b, size_t size);
static void i915_dp_ext_zero(void *bytes, size_t size);

/*
 * Forgets what a port's sink was: the cache becomes that of a port with
 * nothing plugged in.
 */
void
drv_i915_dp_ext_forget(
	struct i915_dp_ext_sink *sink)
{
	/* Every field's zero means "not known", and the status's zero is disconnected. */
	i915_dp_ext_zero(sink, sizeof(*sink));
	sink->status = I915_DP_EXT_DISCONNECTED;
	sink->step = I915_DP_EXT_STEP_NONE;
}

/*
 * Probes the sink of an external DP port (the Linux intel_dp_detect() for
 * a port that is not eDP, after the live status said something is
 * plugged in).
 *
 * The receiver, the repeaters, the identification, the rates and lanes,
 * the sink count of a branch and its downstream ports are read; a branch
 * is judged by its sink count, its DDC or its port type; the EDID is read
 * last, and an EDID makes any sink connected.  The result replaces the
 * port's cache.  The caller holds the port's PHY for the whole probe.
 */
void
drv_i915_dp_ext_detect(
	const struct i915_dp_ext_env *env,
	const struct i915_dp_ext_source *source,
	struct i915_dp_ext_sink *sink)
{
	enum i915_dp_ext_status status;
	int error;

	/* Starts from a sink that is not there. */
	drv_i915_dp_ext_forget(sink);

	/* Reads the repeaters and the receiver (intel_dp_init_lttpr_and_dprx_caps()). */
	error = i915_dp_ext_read_caps(env, sink);
	if (error != 0) {
		sink->step = I915_DP_EXT_STEP_CAPS;
		sink->error = error;
		return;
	}

	/* A sink that names a downstream port is a branch device. */
	if ((sink->dpcd[I915_DP_EXT_DOWNSTREAMPORT_PRESENT] & I915_DP_EXT_DWN_STRM_PORT_PRESENT) != 0u)
		sink->branch = 1;

	/* Reads who the sink or branch is, then what both sides offer (intel_dp_get_dpcd()). */
	i915_dp_ext_read_desc(env, sink);
	i915_dp_ext_set_rates(env, source, sink);
	i915_dp_ext_set_lanes(env, source, sink);

	/* A branch that counts its sinks must count one: an adapter with no display is disconnected. */
	error = i915_dp_ext_read_sink_count(env, sink);
	if (error != 0) {
		sink->step = I915_DP_EXT_STEP_SINK_COUNT;
		sink->error = error;
		return;
	}

	/* Reads the branch's downstream ports. */
	error = i915_dp_ext_read_downstream(env, sink);
	if (error != 0) {
		sink->step = I915_DP_EXT_STEP_DOWNSTREAM;
		sink->error = error;
		return;
	}

	/* Judges whether a display is there (intel_dp_detect_dpcd()). */
	status = i915_dp_ext_classify(env, sink);
	if (status == I915_DP_EXT_DISCONNECTED) {
		sink->step = I915_DP_EXT_STEP_BRANCH;
		return;
	}

	/* Reads the EDID (intel_dp_set_edid()); a display with an EDID is connected whatever the branch said. */
	i915_dp_ext_read_edid(env, sink);
	if (sink->edid_blocks != 0u)
		status = I915_DP_EXT_CONNECTED;

	/* Takes the branch's limits and whether the display is HDMI (intel_dp_update_dfp()). */
	i915_dp_ext_set_downstream_limits(sink);
	sink->has_hdmi_sink = i915_dp_ext_edid_has_hdmi(sink);

	/* Publishes the conclusion. */
	sink->status = status;
	sink->step = I915_DP_EXT_STEP_DONE;
}

/*
 * Handles a short pulse (IRQ_HPD) of a sink the last probe found (the
 * Linux intel_dp_short_pulse() up to the service interrupts).
 *
 * The receiver capabilities are read again, and a branch's sink count; a
 * read that fails, or capabilities or a count that changed, ask for a
 * full detection.  Otherwise the device and link service interrupts are
 * acknowledged and logged (their handling -- link retraining, HDCP, the
 * HDMI link status -- is not done here).  Returns 1 when the pulse is
 * handled, 0 when the port must be detected again.
 */
int
drv_i915_dp_ext_short_pulse(
	const struct i915_dp_ext_env *env,
	const struct i915_dp_ext_sink *sink)
{
	uint8_t dpcd[I915_DP_EXT_DPCD_SIZE];
	uint8_t count;
	long read;
	int error;
	int same;
	int now;

	/* A sink the last probe did not find connected is detected afresh. */
	if (sink->status != I915_DP_EXT_CONNECTED)
		return 0;

	/* Reads the receiver capabilities again; a failure asks for a detection. */
	error = env->read_caps(env->ctx, dpcd);
	if (error != 0)
		return 0;

	/* Changed capabilities ask for a detection. */
	same = i915_dp_ext_same_bytes(dpcd, sink->dpcd, sizeof(dpcd));
	if (!same)
		return 0;

	/* A branch's sink count is read again: a failure or another count asks for a detection. */
	if (sink->has_sink_count) {
		read = env->dpcd_read(env->ctx, I915_DP_EXT_SINK_COUNT, &count, 1u);
		if (read != 1)
			return 0;

		/* Bit 7 of the field is bit 6 of the count. */
		now = (int)(count & I915_DP_EXT_SINK_COUNT_LOW_MASK);
		if ((count & I915_DP_EXT_SINK_COUNT_HIGH_BIT) != 0u)
			now |= 0x40;

		/* Another count asks for a detection. */
		if (now != sink->sink_count)
			return 0;
	}

	/* Acknowledges the device and link service interrupts of a DPCD 1.1 sink. */
	if (sink->dpcd[I915_DP_EXT_DPCD_REV] >= I915_DP_EXT_DPCD_REV_11) {
		i915_dp_ext_ack_service_irq(env, I915_DP_EXT_DEVICE_SERVICE_IRQ_VECTOR, "device");
		i915_dp_ext_ack_service_irq(env, I915_DP_EXT_LINK_SERVICE_IRQ_VECTOR_ESI0, "link");
	}

	/* Succeeded: the sink is as the probe found it. */
	return 1;
}

/*
 * Tells whether a trained link must be trained again (the Linux
 * intel_dp_needs_link_retrain() with drm_dp_channel_eq_ok(), ws051-p005b):
 * the sink's link status is read, and the link needs it when the lanes
 * lost their alignment or a lane of the link's lane_count lost clock
 * recovery, equalisation or symbol lock.  A status that cannot be read, or
 * a lane count out of 1 to 4, does not ask for it (as Linux, which
 * retrains only on a status it read).  Returns 1 or 0.
 */
int
drv_i915_dp_ext_link_needs_retrain(
	const struct i915_dp_ext_env *env,
	int lane_count)
{
	uint8_t status[I915_DP_EXT_LINK_STATUS_SIZE];
	unsigned lane_bits;
	long read;
	int lane;

	/* Only a link of one to four lanes. */
	if (lane_count < 1 || lane_count > I915_DP_EXT_LANES_MAX)
		return 0;

	/* The status; one that cannot be read asks for nothing. */
	read = env->dpcd_read(env->ctx, I915_DP_EXT_LANE0_1_STATUS, status, sizeof(status));
	if (read != (long)sizeof(status))
		return 0;

	/* Lanes that lost their alignment. */
	if ((status[I915_DP_EXT_LANE_ALIGN_INDEX] & I915_DP_EXT_INTERLANE_ALIGN_DONE) == 0u)
		return 1;

	/* Each lane of the link: clock recovery, equalisation and symbol lock, all done. */
	for (lane = 0; lane < lane_count; lane++) {
		lane_bits = ((unsigned)status[lane / 2] >> ((unsigned)(lane % 2) * I915_DP_EXT_LANE_STATUS_BITS)) & I915_DP_EXT_LANE_EQ_BITS;
		if (lane_bits != I915_DP_EXT_LANE_EQ_BITS)
			return 1;
	}

	/* The link is as it was trained. */
	return 0;
}

/*
 * Sets up the protocol converter of a DPCD 1.3 branch for an RGB stream
 * (the Linux intel_dp_configure_protocol_converter() for an RGB sink
 * format): HDMI or DVI output by the display's EDID, no 4:2:0 conversion
 * and no RGB to YCbCr conversion.
 *
 * A sink that is no such branch is left alone.  Returns 0, or the
 * negative Linux errno of the first write that failed; every write is
 * tried, as Linux only logs a failure.
 */
int
drv_i915_dp_ext_configure_converter(
	const struct i915_dp_ext_env *env,
	const struct i915_dp_ext_sink *sink)
{
	uint8_t control;
	long read;
	int written;
	int first_error;

	/* A sink before DPCD 1.3 has no converter controls. */
	if (sink->dpcd[I915_DP_EXT_DPCD_REV] < I915_DP_EXT_DPCD_REV_13)
		return 0;

	/* A sink that is not a branch converts nothing. */
	if (!sink->branch)
		return 0;

	/* Selects HDMI output for an HDMI display, DVI otherwise. */
	first_error = 0;
	control = 0u;
	if (sink->has_hdmi_sink)
		control = I915_DP_EXT_HDMI_DVI_OUTPUT_CONFIG;

	/* Writes the selection; a failure is logged and kept. */
	written = i915_dp_ext_dpcd_writeb(env, I915_DP_EXT_CONVERTER_CONTROL_0, control);
	if (written != 0) {
		env->log(env->ctx, "i915: DP-ext: protocol converter HDMI mode %d not set: rc %d\n", sink->has_hdmi_sink, written);
		first_error = i915_dp_ext_first_error(first_error, written);
	}

	/* Turns the YCbCr 4:4:4 to 4:2:0 conversion off: the stream is RGB. */
	written = i915_dp_ext_dpcd_writeb(env, I915_DP_EXT_CONVERTER_CONTROL_1, 0u);
	if (written != 0) {
		env->log(env->ctx, "i915: DP-ext: protocol converter 4:2:0 conversion not turned off: rc %d\n", written);
		first_error = i915_dp_ext_first_error(first_error, written);
	}

	/* Turns the RGB to YCbCr conversion off, keeping the control's other bits (drm_dp_pcon_convert_rgb_to_ycbcr()). */
	read = env->dpcd_read(env->ctx, I915_DP_EXT_CONVERTER_CONTROL_2, &control, 1u);
	written = -I915_DP_EXT_EIO;
	if (read == 1) {
		control = (uint8_t)(control & ~I915_DP_EXT_CONVERSION_RGB_YCBCR_MASK);
		written = i915_dp_ext_dpcd_writeb(env, I915_DP_EXT_CONVERTER_CONTROL_2, control);
	}

	/* Reports a control that could not be read or written back. */
	if (written != 0) {
		env->log(env->ctx, "i915: DP-ext: protocol converter RGB to YCbCr conversion not turned off: read %ld, rc %d\n", read, written);
		first_error = i915_dp_ext_first_error(first_error, written);
	}

	/* Reports the first write that failed. */
	if (first_error != 0)
		return first_error;

	/* Succeeded: the converter passes RGB through to the display's interface. */
	return 0;
}

/*
 * Computes the link limits after a link training failed at a rate and a
 * lane count (the Linux intel_dp_get_link_train_fallback_values() for a
 * port that is not eDP, ws051-p004b).
 *
 * A rate above the lowest the source and sink share falls to the next
 * lower shared rate with the same lanes; otherwise the lanes are halved
 * at the highest shared rate.  Returns 1 with *max_rate and *max_lanes
 * the new limits, or 0 when one lane is left (nothing lower to try).
 */
int
drv_i915_dp_ext_fallback_values(
	const struct i915_dp_ext_sink *sink,
	int rate,
	int lanes,
	int *max_rate,
	int *max_lanes)
{
	int index;
	int found;

	/* Finds the failed rate among the shared ones (-1: not shared). */
	found = -1;
	for (index = 0; index < sink->num_common_rates; index++) {
		if (sink->common_rates[index] == rate)
			found = index;
	}

	/* A higher rate falls to the next lower one, with the same lanes. */
	if (found > 0) {
		*max_rate = sink->common_rates[found - 1];
		*max_lanes = lanes;
		return 1;
	}

	/* The lowest rate (or one not shared): half the lanes at the highest rate. */
	if (lanes > 1 && sink->num_common_rates > 0) {
		*max_rate = sink->common_rates[sink->num_common_rates - 1];
		*max_lanes = lanes >> 1;
		return 1;
	}

	/* One lane at the lowest rate: the link training was unsuccessful. */
	return 0;
}

/*
 * Checks a mode's pixel clock against a branch's downstream limits (the
 * Linux intel_dp_mode_valid_downstream() for RGB at 8 bits per colour,
 * where the TMDS clock is the pixel clock).
 */
enum i915_dp_ext_mode_status
drv_i915_dp_ext_mode_valid(
	const struct i915_dp_ext_sink *sink,
	int clock_khz)
{
	/* A clock above the branch's dot clock limit. */
	if (sink->max_dotclock_khz != 0 && clock_khz > sink->max_dotclock_khz)
		return I915_DP_EXT_MODE_CLOCK_HIGH;

	/* A TMDS clock below what the branch's HDMI or DVI port sends. */
	if (sink->min_tmds_clock_khz != 0 && clock_khz < sink->min_tmds_clock_khz)
		return I915_DP_EXT_MODE_CLOCK_LOW;

	/* A TMDS clock above what the branch's HDMI or DVI port sends. */
	if (sink->max_tmds_clock_khz != 0 && clock_khz > sink->max_tmds_clock_khz)
		return I915_DP_EXT_MODE_CLOCK_HIGH;

	/* Succeeded: the branch passes the mode. */
	return I915_DP_EXT_MODE_OK;
}

/*
 * Names a probe's conclusion for the log.
 */
const char *
drv_i915_dp_ext_status_name(
	enum i915_dp_ext_status status)
{
	/* Names the conclusion. */
	switch (status) {
	case I915_DP_EXT_DISCONNECTED:
		return "disconnected";
	case I915_DP_EXT_CONNECTED:
		return "connected";
	case I915_DP_EXT_UNKNOWN:
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
drv_i915_dp_ext_step_name(
	enum i915_dp_ext_step step)
{
	/* Names the step. */
	switch (step) {
	case I915_DP_EXT_STEP_NONE:
		return "none";
	case I915_DP_EXT_STEP_PHY:
		return "PHY";
	case I915_DP_EXT_STEP_CAPS:
		return "receiver capabilities";
	case I915_DP_EXT_STEP_SINK_COUNT:
		return "sink count";
	case I915_DP_EXT_STEP_DOWNSTREAM:
		return "downstream ports";
	case I915_DP_EXT_STEP_BRANCH:
		return "branch device";
	case I915_DP_EXT_STEP_DONE:
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
drv_i915_dp_ext_log(
	const struct i915_dp_ext_env *env,
	const struct i915_dp_ext_sink *sink,
	const char *name)
{
	const uint8_t *d;
	const uint8_t *e;
	char id[I915_DP_EXT_DEVICE_ID_SIZE + 1u];
	unsigned i;
	int sink_top;
	int source_top;

	/* The conclusion and where the probe stopped. */
	env->log(env->ctx, "i915: DP-ext %s: %s (step %s, rc %d)\n",
		 name,
		 drv_i915_dp_ext_status_name(sink->status),
		 drv_i915_dp_ext_step_name(sink->step),
		 sink->error);

	/* Nothing more to tell without the receiver capabilities. */
	if (!sink->dpcd_valid)
		return;

	/* The receiver capabilities, byte by byte. */
	d = sink->dpcd;
	env->log(env->ctx, "i915: DP-ext %s DPCD 000: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
		 name, d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], d[8], d[9], d[10], d[11], d[12], d[13], d[14]);

	/* The identification string, its unprintable bytes as spaces. */
	for (i = 0u; i < I915_DP_EXT_DEVICE_ID_SIZE; i++) {
		id[i] = sink->desc.device_id[i];
		if (id[i] < 0x20 || id[i] > 0x7e)
			id[i] = ' ';
	}

	/* Ends the string. */
	id[I915_DP_EXT_DEVICE_ID_SIZE] = '\0';

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

/* Converts a link bandwidth code to its rate (the Linux drm_dp_bw_code_to_link_rate()). */
static int
i915_dp_ext_bw_code_to_rate(
	uint8_t code)
{
	/* Three codes name 128b/132b rates; every other code counts units of 0.27 Gbit/s. */
	switch (code) {
	case I915_DP_EXT_LINK_BW_10:
		return 1000000;
	case I915_DP_EXT_LINK_BW_13_5:
		return 1350000;
	case I915_DP_EXT_LINK_BW_20:
		return 2000000;
	default:
		break;
	}

	/* Succeeded: an 8b/10b rate. */
	return (int)code * I915_DP_EXT_LINK_BW_UNIT;
}

/*
 * Reads the repeaters' common capabilities (the Linux
 * intel_dp_read_lttpr_common_caps() and drm_dp_lttpr_count()).
 *
 * A sink below DPCD 1.4 is read a byte at a time: at least one monitor
 * returns corrupted values for larger reads of that range.  Capabilities
 * that cannot be read, or below revision 1.4, are dropped; a repeater
 * count out of range keeps the capabilities' limits but counts no
 * repeater.  Repeaters are not switched to non-transparent mode here.
 */
static void
i915_dp_ext_read_lttpr(
	const struct i915_dp_ext_env *env,
	struct i915_dp_ext_sink *sink)
{
	unsigned offset;
	unsigned block;
	long read;

	/* Reads the block whole, or a byte at a time before DPCD 1.4. */
	block = I915_DP_EXT_LTTPR_SIZE;
	if (sink->dpcd[I915_DP_EXT_DPCD_REV] < I915_DP_EXT_DPCD_REV_14)
		block = 1u;

	/* Reads the capabilities; a failed read drops them. */
	for (offset = 0u; offset < I915_DP_EXT_LTTPR_SIZE; offset += block) {
		/* Reads the next block. */
		read = env->dpcd_read(env->ctx, I915_DP_EXT_LTTPR_CAPS + offset, &sink->lttpr[offset], block);
		if (read < 0) {
			i915_dp_ext_zero(sink->lttpr, sizeof(sink->lttpr));
			return;
		}
	}

	/* Capabilities below revision 1.4 are not to be trusted. */
	if (sink->lttpr[I915_DP_EXT_LTTPR_REV] < I915_DP_EXT_LTTPR_MIN_REV) {
		i915_dp_ext_zero(sink->lttpr, sizeof(sink->lttpr));
		return;
	}

	/* Keeps the capabilities and counts the repeaters. */
	sink->lttpr_valid = 1;
	sink->lttpr_count = i915_dp_ext_lttpr_count(sink->lttpr[I915_DP_EXT_LTTPR_COUNT]);
}

/*
 * Counts the repeaters a count field names: one bit, 0x80 for one repeater
 * down to 0x01 for eight; anything else counts none (Linux reports an
 * error, which its caller treats as none).
 */
static int
i915_dp_ext_lttpr_count(
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
 * Reads the repeaters and the receiver (the Linux
 * intel_dp_init_lttpr_and_dprx_caps() for a port that is not eDP): a
 * throw-away read at the repeaters' range wakes them, the receiver's
 * capabilities are read, then the repeaters', then the receiver's again,
 * as the source must read them after detecting the repeaters.
 *
 * Returns 0, or the negative Linux errno of the read that failed.
 */
static int
i915_dp_ext_read_caps(
	const struct i915_dp_ext_env *env,
	struct i915_dp_ext_sink *sink)
{
	int error;

	/* Wakes the repeaters and the sink. */
	error = env->dpcd_probe(env->ctx, I915_DP_EXT_LTTPR_CAPS);
	if (error != 0)
		return error;

	/* Reads the receiver capabilities, which tell how to read the repeaters'. */
	error = env->read_caps(env->ctx, sink->dpcd);
	if (error != 0)
		return error;

	/* Reads the repeaters' capabilities. */
	i915_dp_ext_read_lttpr(env, sink);

	/* Reads the receiver capabilities again, now behind the repeaters. */
	error = env->read_caps(env->ctx, sink->dpcd);
	if (error != 0) {
		i915_dp_ext_zero(sink->lttpr, sizeof(sink->lttpr));
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
 * branch (the Linux drm_dp_read_desc()); a read that fails leaves it
 * unknown, as Linux does not check it.
 */
static void
i915_dp_ext_read_desc(
	const struct i915_dp_ext_env *env,
	struct i915_dp_ext_sink *sink)
{
	uint8_t bytes[I915_DP_EXT_DESC_SIZE];
	unsigned offset;
	unsigned i;
	long read;

	/* A branch identifies itself at 0x500, a sink at 0x400. */
	offset = I915_DP_EXT_SINK_OUI;
	if (sink->branch)
		offset = I915_DP_EXT_BRANCH_OUI;

	/* Reads the twelve bytes. */
	read = env->dpcd_read(env->ctx, offset, bytes, sizeof(bytes));
	if (read != (long)sizeof(bytes))
		return;

	/* The OUI, the identification string, and the revisions. */
	sink->desc.oui[0] = bytes[0];
	sink->desc.oui[1] = bytes[1];
	sink->desc.oui[2] = bytes[2];
	for (i = 0u; i < I915_DP_EXT_DEVICE_ID_SIZE; i++)
		sink->desc.device_id[i] = (char)bytes[3u + i];
	sink->desc.hw_rev = bytes[9];
	sink->desc.sw_major_rev = bytes[10];
	sink->desc.sw_minor_rev = bytes[11];
	sink->desc_valid = 1;
}

/*
 * Sets the link rates each side offers and both share (the Linux
 * intel_dp_set_source_rates(), intel_dp_set_dpcd_sink_rates() for 8b/10b
 * and intel_dp_set_common_rates()).
 *
 * The source offers its table up to the lower of the platform's and the
 * VBT's limits; the sink offers the standard rates up to its maximum and
 * that of the repeaters.  A side left with nothing falls back to RBR.
 */
static void
i915_dp_ext_set_rates(
	const struct i915_dp_ext_env *env,
	const struct i915_dp_ext_source *source,
	struct i915_dp_ext_sink *sink)
{
	unsigned count;
	unsigned i;
	unsigned j;
	int source_max;
	int sink_max;
	int lttpr_max;

	/* The source's limit: the platform's, and the VBT's when it is lower or the only one. */
	source_max = source->max_rate;
	if (source->vbt_max_rate != 0) {
		if (source_max == 0 || source->vbt_max_rate < source_max)
			source_max = source->vbt_max_rate;
	}

	/* The source's rates up to its limit. */
	count = sizeof(i915_dp_ext_source_rate_table) / sizeof(i915_dp_ext_source_rate_table[0]);
	sink->num_source_rates = 0;
	for (i = 0u; i < count; i++) {
		/* The table is in increasing order: the first rate above the limit ends it. */
		if (source_max != 0 && i915_dp_ext_source_rate_table[i] > source_max)
			break;

		/* Offers the rate. */
		sink->source_rates[sink->num_source_rates] = i915_dp_ext_source_rate_table[i];
		sink->num_source_rates++;
	}

	/* The sink's limit: its own, and the repeaters' when they name one. */
	sink_max = i915_dp_ext_bw_code_to_rate(sink->dpcd[I915_DP_EXT_MAX_LINK_RATE]);
	lttpr_max = 0;
	if (sink->lttpr_valid)
		lttpr_max = i915_dp_ext_bw_code_to_rate(sink->lttpr[I915_DP_EXT_LTTPR_MAX_LINK_RATE]);
	if (lttpr_max != 0 && lttpr_max < sink_max)
		sink_max = lttpr_max;

	/* The sink's rates up to its limit. */
	count = sizeof(i915_dp_ext_sink_rate_table) / sizeof(i915_dp_ext_sink_rate_table[0]);
	sink->num_sink_rates = 0;
	for (i = 0u; i < count; i++) {
		/* The table is in increasing order: the first rate above the limit ends it. */
		if (i915_dp_ext_sink_rate_table[i] > sink_max)
			break;

		/* Offers the rate. */
		sink->sink_rates[sink->num_sink_rates] = i915_dp_ext_sink_rate_table[i];
		sink->num_sink_rates++;
	}

	/* A sink that offers no rate is given RBR. */
	if (sink->num_sink_rates == 0) {
		env->log(env->ctx, "i915: DP-ext: invalid DPCD with no link rates (max code 0x%02x), using RBR\n", sink->dpcd[I915_DP_EXT_MAX_LINK_RATE]);
		sink->sink_rates[0] = I915_DP_EXT_RATE_RBR;
		sink->num_sink_rates = 1;
	}

	/* The rates both sides offer, lowest first; both lists are in increasing order. */
	sink->num_common_rates = 0;
	i = 0u;
	j = 0u;
	while (i < (unsigned)sink->num_source_rates && j < (unsigned)sink->num_sink_rates) {
		/* A rate on both lists is shared; otherwise the list with the lower rate moves on. */
		if (sink->source_rates[i] == sink->sink_rates[j]) {
			sink->common_rates[sink->num_common_rates] = sink->source_rates[i];
			sink->num_common_rates++;
			i++;
			j++;
		} else if (sink->source_rates[i] < sink->sink_rates[j]) {
			i++;
		} else {
			j++;
		}
	}

	/* Something is always shared: RBR when the lists meet nowhere. */
	if (sink->num_common_rates == 0) {
		env->log(env->ctx, "i915: DP-ext: the source and the sink share no link rate, using RBR\n");
		sink->common_rates[0] = I915_DP_EXT_RATE_RBR;
		sink->num_common_rates = 1;
	}

	/* The highest shared rate. */
	sink->max_rate = sink->common_rates[sink->num_common_rates - 1];
}

/*
 * Sets the lanes the sink takes and the lanes every party allows (the
 * Linux intel_dp_set_max_sink_lane_count() and
 * intel_dp_max_common_lane_count()).
 *
 * The sink takes 1, 2 or 4 lanes (1 when its DPCD says otherwise); the
 * repeaters, the source with the VBT's limit, and the FIA lower that.
 */
static void
i915_dp_ext_set_lanes(
	const struct i915_dp_ext_env *env,
	const struct i915_dp_ext_source *source,
	struct i915_dp_ext_sink *sink)
{
	int sink_max;
	int source_max;
	int lttpr_max;
	int lanes;

	/* The sink's lanes; a count other than 1, 2 or 4 is invalid and becomes 1. */
	sink_max = (int)(sink->dpcd[I915_DP_EXT_MAX_LANE_COUNT] & I915_DP_EXT_MAX_LANE_COUNT_MASK);
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
		lttpr_max = (int)(sink->lttpr[I915_DP_EXT_LTTPR_MAX_LANE_COUNT] & I915_DP_EXT_MAX_LANE_COUNT_MASK);
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
 * Reads the sink count of a sink that reports one (the Linux
 * drm_dp_read_sink_count_cap() and drm_dp_read_sink_count(), no quirk).
 *
 * A branch of DPCD 1.1 or later reports how many displays are behind it.
 * Returns 0 when the count is not reported or is not zero, the negative
 * Linux errno of a failed read, or -EIO for a count of zero: an adapter
 * with no display behind it is disconnected.
 */
static int
i915_dp_ext_read_sink_count(
	const struct i915_dp_ext_env *env,
	struct i915_dp_ext_sink *sink)
{
	uint8_t count;
	long read;

	/* Only a branch of DPCD 1.1 or later reports a sink count. */
	if (sink->dpcd[I915_DP_EXT_DPCD_REV] < I915_DP_EXT_DPCD_REV_11)
		return 0;
	if (!sink->branch)
		return 0;

	/* Reads the count; a failed or short read is an error. */
	sink->has_sink_count = 1;
	read = env->dpcd_read(env->ctx, I915_DP_EXT_SINK_COUNT, &count, 1u);
	if (read < 0)
		return (int)read;
	if (read != 1)
		return -I915_DP_EXT_EIO;

	/* Bit 7 of the field is bit 6 of the count. */
	sink->sink_count = (int)(count & I915_DP_EXT_SINK_COUNT_LOW_MASK);
	if ((count & I915_DP_EXT_SINK_COUNT_HIGH_BIT) != 0u)
		sink->sink_count |= 0x40;

	/* No display behind the branch. */
	if (sink->sink_count == 0)
		return -I915_DP_EXT_EIO;

	/* Succeeded: a display is behind the branch. */
	return 0;
}

/*
 * Reads a branch's downstream port capabilities (the Linux
 * drm_dp_read_downstream_info()).
 *
 * A sink that is no branch, or a DPCD 1.0 one, has none to read, and a
 * branch may name zero ports.  Detailed capabilities take four bytes a
 * port, for at most four ports.  Returns 0, or the negative Linux errno of
 * the read (-EIO for a short one).
 */
static int
i915_dp_ext_read_downstream(
	const struct i915_dp_ext_env *env,
	struct i915_dp_ext_sink *sink)
{
	unsigned length;
	long read;

	/* Nothing to read without a branch, or from a DPCD 1.0 one. */
	if (!sink->branch)
		return 0;
	if (sink->dpcd[I915_DP_EXT_DPCD_REV] == I915_DP_EXT_DPCD_REV_10)
		return 0;

	/* The ports the branch names, at most four when they are detailed. */
	length = sink->dpcd[I915_DP_EXT_DOWN_STREAM_PORT_COUNT] & I915_DP_EXT_PORT_COUNT_MASK;
	if ((sink->dpcd[I915_DP_EXT_DOWNSTREAMPORT_PRESENT] & I915_DP_EXT_DETAILED_CAP_INFO_AVAILABLE) != 0u) {
		if (length > I915_DP_EXT_DETAILED_PORTS_MAX)
			length = I915_DP_EXT_DETAILED_PORTS_MAX;

		/* Four bytes describe each port. */
		length *= I915_DP_EXT_DETAILED_PORT_SIZE;
	}

	/* Some branches name a downstream port and then zero of them. */
	if (length == 0u)
		return 0;

	/* Reads the capabilities; a short read is an I/O error. */
	read = env->dpcd_read(env->ctx, I915_DP_EXT_DOWNSTREAM_PORT_0, sink->downstream, length);
	if (read < 0)
		return (int)read;
	if (read != (long)length)
		return -I915_DP_EXT_EIO;

	/* Succeeded: the ports' capabilities are read. */
	return 0;
}

/*
 * Judges whether a display is behind the port (the Linux
 * intel_dp_detect_dpcd() after the DPCD was read; MST is not supported).
 *
 * A sink that is no branch is the display.  A branch with hot plug detect
 * on its port is believed by its sink count; otherwise a DDC that answers
 * is a display; a port type with no EDID (VGA, or one without EDID) may
 * have a display the probe cannot see; anything else is out of
 * specification and ignored.
 */
static enum i915_dp_ext_status
i915_dp_ext_classify(
	const struct i915_dp_ext_env *env,
	struct i915_dp_ext_sink *sink)
{
	uint8_t type;
	int answered;

	/* A sink that is no branch is the display itself. */
	if (!sink->branch)
		return I915_DP_EXT_CONNECTED;

	/* A branch whose port detects its display counts it; the count was not zero. */
	if (sink->has_sink_count && (sink->downstream[0] & I915_DP_EXT_DS_PORT_HPD) != 0u)
		return I915_DP_EXT_CONNECTED;

	/* A branch without that is asked over its DDC. */
	answered = env->ddc_probe(env->ctx);
	if (answered)
		return I915_DP_EXT_CONNECTED;

	/* A port type without an EDID may still have a display (DPCD 1.1 port types, then the older ones). */
	if (sink->dpcd[I915_DP_EXT_DPCD_REV] >= I915_DP_EXT_DPCD_REV_11) {
		type = (uint8_t)(sink->downstream[0] & I915_DP_EXT_DS_PORT_TYPE_MASK);
		if (type == I915_DP_EXT_DS_PORT_TYPE_VGA || type == I915_DP_EXT_DS_PORT_TYPE_NON_EDID)
			return I915_DP_EXT_UNKNOWN;
	} else {
		type = (uint8_t)(sink->dpcd[I915_DP_EXT_DOWNSTREAMPORT_PRESENT] & I915_DP_EXT_DWN_STRM_PORT_TYPE_MASK);
		if (type == I915_DP_EXT_DWN_STRM_PORT_TYPE_ANALOG || type == I915_DP_EXT_DWN_STRM_PORT_TYPE_OTHER)
			return I915_DP_EXT_UNKNOWN;
	}

	/* A branch that gives no sign of a display is out of specification and ignored. */
	env->log(env->ctx, "i915: DP-ext: broken DP branch device (port 0 %02x), ignoring\n", sink->downstream[0]);
	return I915_DP_EXT_DISCONNECTED;
}

/*
 * Reads the display's EDID over the port's DDC (drm_edid_read_ddc()).
 *
 * The valid blocks and the extensions the base block announces are kept;
 * a read without a valid block leaves no EDID.
 */
static void
i915_dp_ext_read_edid(
	const struct i915_dp_ext_env *env,
	struct i915_dp_ext_sink *sink)
{
	unsigned extensions;
	int blocks;

	/* Reads the base block and up to three extensions. */
	extensions = 0u;
	blocks = env->edid_read(env->ctx, sink->edid, I915_DP_EXT_EDID_BLOCKS, &extensions);
	if (blocks < 1) {
		i915_dp_ext_zero(sink->edid, sizeof(sink->edid));
		return;
	}

	/* Keeps the blocks read and the extensions announced. */
	sink->edid_blocks = (unsigned)blocks;
	sink->edid_extensions = extensions;
}

/*
 * Tells whether the EDID describes a DisplayPort input (the Linux
 * is_edid_digital_input_dp(): EDID 1.4, digital, interface DP).
 */
static int
i915_dp_ext_edid_is_digital_dp(
	const struct i915_dp_ext_sink *sink)
{
	uint8_t input;

	/* No EDID describes no input. */
	if (sink->edid_blocks == 0u)
		return 0;

	/* Only EDID 1.4 names the interface. */
	if (sink->edid[I915_DP_EXT_EDID_REVISION] < 4u)
		return 0;

	/* An analog input is no DP input. */
	input = sink->edid[I915_DP_EXT_EDID_INPUT];
	if ((input & I915_DP_EXT_EDID_INPUT_DIGITAL) == 0u)
		return 0;

	/* A digital input of another interface is no DP input. */
	if ((input & I915_DP_EXT_EDID_DIGITAL_TYPE_MASK) != I915_DP_EXT_EDID_DIGITAL_TYPE_DP)
		return 0;

	/* Succeeded: the display takes DisplayPort. */
	return 1;
}

/*
 * Tells whether the EDID announces an HDMI sink: a CTA extension with an
 * HDMI vendor-specific data block (what drm_parse_cea_ext() records as
 * is_hdmi).
 */
static int
i915_dp_ext_edid_has_hdmi(
	const struct i915_dp_ext_sink *sink)
{
	unsigned block;
	int found;

	/* Looks at every extension block that was read. */
	for (block = 1u; block < sink->edid_blocks; block++) {
		/* Asks the extension. */
		found = i915_dp_ext_cea_block_has_hdmi(&sink->edid[block * I915_DP_EXT_EDID_BLOCK_SIZE]);
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
i915_dp_ext_cea_block_has_hdmi(
	const uint8_t *block)
{
	unsigned end;
	unsigned at;
	unsigned tag;
	unsigned length;

	/* Only a CTA extension has data blocks. */
	if (block[0] != I915_DP_EXT_EDID_CEA_TAG)
		return 0;

	/* The data blocks run from byte 4 to the first detailed timing (byte 2), inside the block. */
	end = block[2];
	if (end < 4u || end > I915_DP_EXT_EDID_BLOCK_SIZE - 1u)
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
		if (tag == I915_DP_EXT_CEA_VENDOR_BLOCK && length >= I915_DP_EXT_HDMI_VSDB_MIN_LENGTH) {
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
 * Takes a branch's downstream limits from its first port (the Linux
 * intel_dp_update_dfp(): drm_dp_downstream_max_bpc(),
 * drm_dp_downstream_max_dotclock() and the TMDS clock range).
 */
static void
i915_dp_ext_set_downstream_limits(
	struct i915_dp_ext_sink *sink)
{
	/* The limits of the first downstream port; a sink that is no branch has none. */
	sink->max_bpc = i915_dp_ext_downstream_max_bpc(sink);
	sink->max_dotclock_khz = i915_dp_ext_downstream_max_dotclock(sink);
	sink->min_tmds_clock_khz = i915_dp_ext_downstream_min_tmds(sink);
	sink->max_tmds_clock_khz = i915_dp_ext_downstream_max_tmds(sink);
}

/*
 * Reports the highest TMDS clock a branch's HDMI or DVI port sends, in
 * kHz, or 0 when it sets no limit (the Linux
 * drm_dp_downstream_max_tmds_clock()).
 *
 * Without detailed capabilities an HDMI port is taken for 300 MHz (an HDMI
 * 1.4 converter: DPCD 1.4 HDMI 2.0 ports must give details) and a DVI port
 * for 165 MHz; a dual-mode port in front of a DP display sets no limit.
 */
static int
i915_dp_ext_downstream_max_tmds(
	const struct i915_dp_ext_sink *sink)
{
	uint8_t type;
	int detailed;
	int dp_display;

	/* A sink that is no branch sets no limit. */
	if (!sink->branch)
		return 0;

	/* Before DPCD 1.1 only the port type is known: TMDS is single-link DVI. */
	if (sink->dpcd[I915_DP_EXT_DPCD_REV] < I915_DP_EXT_DPCD_REV_11) {
		if ((sink->dpcd[I915_DP_EXT_DOWNSTREAMPORT_PRESENT] & I915_DP_EXT_DWN_STRM_PORT_TYPE_MASK) == I915_DP_EXT_DWN_STRM_PORT_TYPE_TMDS)
			return 165000;

		/* Any other type sets no limit. */
		return 0;
	}

	/* The port type, and whether its capabilities are detailed. */
	type = (uint8_t)(sink->downstream[0] & I915_DP_EXT_DS_PORT_TYPE_MASK);
	detailed = 0;
	if ((sink->dpcd[I915_DP_EXT_DOWNSTREAMPORT_PRESENT] & I915_DP_EXT_DETAILED_CAP_INFO_AVAILABLE) != 0u)
		detailed = 1;

	/* A dual-mode port in front of a DP display sets no limit. */
	if (type == I915_DP_EXT_DS_PORT_TYPE_DP_DUALMODE) {
		dp_display = i915_dp_ext_edid_is_digital_dp(sink);
		if (dp_display)
			return 0;
	}

	/* An HDMI port, or a dual-mode one sending HDMI: 300 MHz, or its detailed limit in 2.5 MHz units. */
	if (type == I915_DP_EXT_DS_PORT_TYPE_DP_DUALMODE || type == I915_DP_EXT_DS_PORT_TYPE_HDMI) {
		if (!detailed)
			return 300000;

		/* The detailed limit. */
		return (int)sink->downstream[1] * 2500;
	}

	/* A DVI port: 165 MHz, or its detailed limit in 2.5 MHz units. */
	if (type == I915_DP_EXT_DS_PORT_TYPE_DVI) {
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
 * or 0 when it sets no limit (the Linux
 * drm_dp_downstream_min_tmds_clock()): 25 MHz, as the converter cannot be
 * assumed to repeat pixels.
 */
static int
i915_dp_ext_downstream_min_tmds(
	const struct i915_dp_ext_sink *sink)
{
	uint8_t type;
	int dp_display;

	/* A sink that is no branch sets no limit. */
	if (!sink->branch)
		return 0;

	/* Before DPCD 1.1 only the port type is known. */
	if (sink->dpcd[I915_DP_EXT_DPCD_REV] < I915_DP_EXT_DPCD_REV_11) {
		if ((sink->dpcd[I915_DP_EXT_DOWNSTREAMPORT_PRESENT] & I915_DP_EXT_DWN_STRM_PORT_TYPE_MASK) == I915_DP_EXT_DWN_STRM_PORT_TYPE_TMDS)
			return 25000;

		/* Any other type sets no limit. */
		return 0;
	}

	/* A dual-mode port in front of a DP display sets no limit. */
	type = (uint8_t)(sink->downstream[0] & I915_DP_EXT_DS_PORT_TYPE_MASK);
	if (type == I915_DP_EXT_DS_PORT_TYPE_DP_DUALMODE) {
		dp_display = i915_dp_ext_edid_is_digital_dp(sink);
		if (dp_display)
			return 0;
	}

	/* A dual-mode, DVI or HDMI port sends at least 25 MHz. */
	if (type == I915_DP_EXT_DS_PORT_TYPE_DP_DUALMODE ||
	    type == I915_DP_EXT_DS_PORT_TYPE_DVI ||
	    type == I915_DP_EXT_DS_PORT_TYPE_HDMI)
		return 25000;

	/* Any other port sets no TMDS limit. */
	return 0;
}

/*
 * Reports the highest pixel clock a branch's VGA port takes, in kHz, or 0
 * when it sets no limit (the Linux drm_dp_downstream_max_dotclock()).
 */
static int
i915_dp_ext_downstream_max_dotclock(
	const struct i915_dp_ext_sink *sink)
{
	uint8_t type;

	/* A sink that is no branch, or one before DPCD 1.1, sets no limit. */
	if (!sink->branch)
		return 0;
	if (sink->dpcd[I915_DP_EXT_DPCD_REV] < I915_DP_EXT_DPCD_REV_11)
		return 0;

	/* Only a VGA port with detailed capabilities names one, in 8 MHz units. */
	type = (uint8_t)(sink->downstream[0] & I915_DP_EXT_DS_PORT_TYPE_MASK);
	if (type != I915_DP_EXT_DS_PORT_TYPE_VGA)
		return 0;
	if ((sink->dpcd[I915_DP_EXT_DOWNSTREAMPORT_PRESENT] & I915_DP_EXT_DETAILED_CAP_INFO_AVAILABLE) == 0u)
		return 0;

	/* Succeeded: the VGA port's limit. */
	return (int)sink->downstream[1] * 8000;
}

/*
 * Reports the most bits per colour a branch's port passes, or 0 when it
 * sets no limit (the Linux drm_dp_downstream_max_bpc()).
 */
static int
i915_dp_ext_downstream_max_bpc(
	const struct i915_dp_ext_sink *sink)
{
	uint8_t type;
	int dp_display;

	/* A sink that is no branch sets no limit. */
	if (!sink->branch)
		return 0;

	/* Before DPCD 1.1: a DP port sets no limit, any other passes 8. */
	if (sink->dpcd[I915_DP_EXT_DPCD_REV] < I915_DP_EXT_DPCD_REV_11) {
		if ((sink->dpcd[I915_DP_EXT_DOWNSTREAMPORT_PRESENT] & I915_DP_EXT_DWN_STRM_PORT_TYPE_MASK) == I915_DP_EXT_DWN_STRM_PORT_TYPE_DP)
			return 0;

		/* Any other type passes 8. */
		return 8;
	}

	/* A DP port, and a dual-mode one in front of a DP display, set no limit. */
	type = (uint8_t)(sink->downstream[0] & I915_DP_EXT_DS_PORT_TYPE_MASK);
	if (type == I915_DP_EXT_DS_PORT_TYPE_DP)
		return 0;
	if (type == I915_DP_EXT_DS_PORT_TYPE_DP_DUALMODE) {
		dp_display = i915_dp_ext_edid_is_digital_dp(sink);
		if (dp_display)
			return 0;
	}

	/* Only dual-mode, HDMI, DVI and VGA ports with detailed capabilities name their limit. */
	if (type != I915_DP_EXT_DS_PORT_TYPE_DP_DUALMODE &&
	    type != I915_DP_EXT_DS_PORT_TYPE_HDMI &&
	    type != I915_DP_EXT_DS_PORT_TYPE_DVI &&
	    type != I915_DP_EXT_DS_PORT_TYPE_VGA)
		return 8;
	if ((sink->dpcd[I915_DP_EXT_DOWNSTREAMPORT_PRESENT] & I915_DP_EXT_DETAILED_CAP_INFO_AVAILABLE) == 0u)
		return 8;

	/* The detailed limit: 8, 10, 12 or 16 bits per colour. */
	switch (sink->downstream[2] & I915_DP_EXT_DS_MAX_BPC_MASK) {
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

/* Writes one DPCD byte: 0, or the negative Linux errno (-EIO for a short write). */
static int
i915_dp_ext_dpcd_writeb(
	const struct i915_dp_ext_env *env,
	unsigned offset,
	uint8_t value)
{
	long written;

	/* Writes the byte. */
	written = env->dpcd_write(env->ctx, offset, &value, 1u);
	if (written < 0)
		return (int)written;
	if (written != 1)
		return -I915_DP_EXT_EIO;

	/* Succeeded: the sink took the byte. */
	return 0;
}

/*
 * Acknowledges one service interrupt vector of the sink: a nonzero vector
 * is written back, which clears it, and logged.
 */
static void
i915_dp_ext_ack_service_irq(
	const struct i915_dp_ext_env *env,
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
	written = i915_dp_ext_dpcd_writeb(env, offset, vector);
	env->log(env->ctx, "i915: DP-ext: %s service interrupt 0x%02x acknowledged (rc %d), not handled\n", what, vector, written);
}

/* Keeps the first of a series of errors: the one already kept, or this one. */
static int
i915_dp_ext_first_error(
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
i915_dp_ext_same_bytes(
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
i915_dp_ext_zero(
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
