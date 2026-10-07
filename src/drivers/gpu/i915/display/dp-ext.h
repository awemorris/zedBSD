/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The sink of an external DisplayPort port (dp-ext.c).
 *
 * An external DP port -- a Type-C port in DP Alternate Mode, or a connector
 * wired to one -- has a sink the display learns about over the port's AUX
 * channel: its receiver capabilities (DPCD), the link-training repeaters
 * in front of it (LTTPR), whether it is a branch device (a USB-C to HDMI
 * or DP adapter, a protocol converter) with a display behind it, and the
 * display's EDID.  The probe follows the Linux v6.8.12 intel_dp_detect()
 * for a port that is not eDP (MIT): intel_dp_init_lttpr_and_dprx_caps(),
 * intel_dp_get_dpcd(), intel_dp_detect_dpcd(), intel_dp_set_edid() and
 * intel_dp_update_dfp(), with the DRM helpers they call; the code is
 * written for this driver.
 *
 * What the probe does not do, and says so: it does not switch repeaters
 * to non-transparent mode (link training does that), MST is not
 * supported (a sink is always driven as one stream), and no DPCD quirk
 * applies.
 *
 * The core is free of the kernel: the AUX transfers and the log come
 * through struct i915_dp_ext_env, so the host tests run it over a fake
 * sink.  What it found lives in struct i915_dp_ext_sink, the cache of one
 * port that the modeset and the display inventory read.
 */

#ifndef DRIVERS_GPU_I915_DISPLAY_DP_EXT_H
#define DRIVERS_GPU_I915_DISPLAY_DP_EXT_H

#include <stddef.h>
#include <stdint.h>

/* The receiver capability bytes at DPCD 0x000 (DP_RECEIVER_CAP_SIZE). */
#define I915_DP_EXT_DPCD_SIZE		15u

/* The downstream port capability bytes at DPCD 0x080 (DP_MAX_DOWNSTREAM_PORTS). */
#define I915_DP_EXT_DOWNSTREAM_SIZE	16u

/* The repeaters' common capability bytes at DPCD 0xf0000 (DP_LTTPR_COMMON_CAP_SIZE). */
#define I915_DP_EXT_LTTPR_SIZE		8u

/* The device identification string of a sink or branch (DPCD 0x403 or 0x503). */
#define I915_DP_EXT_DEVICE_ID_SIZE	6u

/* The EDID blocks kept: the base block and up to three extensions. */
#define I915_DP_EXT_EDID_BLOCKS		4u

/* The bytes of one EDID block. */
#define I915_DP_EXT_EDID_BLOCK_SIZE	128u

/* The most link rates one side offers (8b/10b, the display version 13 table). */
#define I915_DP_EXT_MAX_RATES		8u

/*
 * What a probe concluded about the port (the Linux enum
 * drm_connector_status, by meaning).
 */
enum i915_dp_ext_status {
	I915_DP_EXT_DISCONNECTED = 0,
	I915_DP_EXT_CONNECTED,
	I915_DP_EXT_UNKNOWN
};

/*
 * How far a probe got, for the log: the step it stopped at, or DONE.
 *
 * PHY: the port's PHY could not be taken for DP.  CAPS: the receiver did
 * not answer.  SINK_COUNT: a branch reported no display behind it, or the
 * count could not be read.  DOWNSTREAM: the branch's port capabilities
 * could not be read.  BRANCH: a branch that gave no sign of a display.
 */
enum i915_dp_ext_step {
	I915_DP_EXT_STEP_NONE = 0,
	I915_DP_EXT_STEP_PHY,
	I915_DP_EXT_STEP_CAPS,
	I915_DP_EXT_STEP_SINK_COUNT,
	I915_DP_EXT_STEP_DOWNSTREAM,
	I915_DP_EXT_STEP_BRANCH,
	I915_DP_EXT_STEP_DONE
};

/*
 * How a mode fares against a branch's downstream limits (the Linux
 * enum drm_mode_status, by meaning).
 */
enum i915_dp_ext_mode_status {
	I915_DP_EXT_MODE_OK = 0,
	I915_DP_EXT_MODE_CLOCK_HIGH,
	I915_DP_EXT_MODE_CLOCK_LOW
};

/*
 * What the probe needs from the device: the port's AUX channel and the
 * log.
 *
 * The device binds one per port.  The DPCD calls return the bytes
 * transferred or a negative Linux errno; read_caps and probe return 0 or a
 * negative Linux errno; edid_read returns the valid blocks read or a
 * negative Linux errno; ddc_probe returns nonzero when something answers
 * at the EDID address.
 */
struct i915_dp_ext_env {
	/* What every callback is given. */
	void *ctx;

	/* DPCD reads and writes over the port's AUX channel. */
	long (*dpcd_read)(void *ctx, unsigned offset, uint8_t *buffer, size_t size);
	long (*dpcd_write)(void *ctx, unsigned offset, const uint8_t *buffer, size_t size);

	/* A throw-away read that wakes the sink (drm_dp_dpcd_probe()). */
	int (*dpcd_probe)(void *ctx, unsigned offset);

	/* The receiver capabilities, the extended ones where the sink has them (drm_dp_read_dpcd_caps()). */
	int (*read_caps)(void *ctx, uint8_t dpcd[I915_DP_EXT_DPCD_SIZE]);

	/* The DDC behind the channel (I2C over AUX). */
	int (*ddc_probe)(void *ctx);
	int (*edid_read)(void *ctx, uint8_t *buffer, unsigned max_blocks, unsigned *extensions);

	/* A line of the driver's log. */
	void (*log)(void *ctx, const char *format, ...) __attribute__((format(printf, 2, 3)));
};

/*
 * What the source offers on a port, which the probe meets with the sink.
 *
 * Link rates are in units of 10 kbit/s per lane, as Linux counts them
 * (162000 is RBR, 810000 is HBR3).  max_rate is the platform's highest link
 * rate for the port (0: no limit); vbt_max_rate and vbt_max_lanes are the
 * VBT's limits (0: none); max_lanes is what the DDI drives; fia_lanes is
 * what the Type-C subsystem assigned to DisplayPort (0: not restricted).
 */
struct i915_dp_ext_source {
	int max_rate;
	int vbt_max_rate;
	int max_lanes;
	int vbt_max_lanes;
	int fia_lanes;
};

/*
 * The identification of a sink or a branch device (the Linux struct
 * drm_dp_desc without the quirks).
 */
struct i915_dp_ext_desc {
	/* The IEEE OUI of the vendor. */
	uint8_t oui[3];

	/* The device identification, not terminated. */
	char device_id[I915_DP_EXT_DEVICE_ID_SIZE];

	/* The hardware revision (major in the high nibble) and the firmware revision. */
	uint8_t hw_rev;
	uint8_t sw_major_rev;
	uint8_t sw_minor_rev;
};

/*
 * What the last probe of one external DP port found (the external DP
 * object's cache).
 *
 * The port owns one for the display's lifetime; a probe replaces all of it
 * under the port's lock.  Everything is zero until the first probe, which
 * means disconnected.
 */
struct i915_dp_ext_sink {
	/* What the probe concluded, where it stopped, and the AUX error that stopped it (0 when none). */
	enum i915_dp_ext_status status;
	enum i915_dp_ext_step step;
	int error;

	/* The receiver capabilities (DPCD 0x000), valid from the CAPS step on. */
	uint8_t dpcd[I915_DP_EXT_DPCD_SIZE];
	int dpcd_valid;

	/* The repeaters' common capabilities and how many repeaters they name (0 when none or invalid). */
	uint8_t lttpr[I915_DP_EXT_LTTPR_SIZE];
	int lttpr_valid;
	int lttpr_count;

	/* The sink's or the branch's identification, when it could be read. */
	struct i915_dp_ext_desc desc;
	int desc_valid;

	/* Nonzero for a branch device (DP_DWN_STRM_PORT_PRESENT). */
	int branch;

	/* Whether the sink reports a sink count, and the count it reported. */
	int has_sink_count;
	int sink_count;

	/* The branch's downstream port capabilities (zero when not a branch). */
	uint8_t downstream[I915_DP_EXT_DOWNSTREAM_SIZE];

	/* The link rates (10 kbit/s per lane) each side offers and both share, lowest first. */
	int sink_rates[I915_DP_EXT_MAX_RATES];
	int num_sink_rates;
	int source_rates[I915_DP_EXT_MAX_RATES];
	int num_source_rates;
	int common_rates[I915_DP_EXT_MAX_RATES];
	int num_common_rates;

	/* The lanes the sink takes, and the lanes source, sink, repeaters and FIA all allow. */
	int max_sink_lanes;
	int max_lanes;

	/* The highest rate both sides share (10 kbit/s per lane). */
	int max_rate;

	/* The branch's downstream limits (0: not limited): TMDS clock range, dot clock, bits per colour. */
	int min_tmds_clock_khz;
	int max_tmds_clock_khz;
	int max_dotclock_khz;
	int max_bpc;

	/* The display's EDID: the valid blocks read and the extensions the base block announced. */
	uint8_t edid[I915_DP_EXT_EDID_BLOCKS * I915_DP_EXT_EDID_BLOCK_SIZE];
	unsigned edid_blocks;
	unsigned edid_extensions;

	/* Nonzero when the EDID announces an HDMI sink (an HDMI vendor-specific data block). */
	int has_hdmi_sink;
};

void drv_i915_dp_ext_forget(struct i915_dp_ext_sink *sink);
void drv_i915_dp_ext_detect(const struct i915_dp_ext_env *env, const struct i915_dp_ext_source *source, struct i915_dp_ext_sink *sink);
int drv_i915_dp_ext_short_pulse(const struct i915_dp_ext_env *env, const struct i915_dp_ext_sink *sink);
int drv_i915_dp_ext_link_needs_retrain(const struct i915_dp_ext_env *env, int lane_count);
int drv_i915_dp_ext_configure_converter(const struct i915_dp_ext_env *env, const struct i915_dp_ext_sink *sink);
enum i915_dp_ext_mode_status drv_i915_dp_ext_mode_valid(const struct i915_dp_ext_sink *sink, int clock_khz);
int drv_i915_dp_ext_fallback_values(const struct i915_dp_ext_sink *sink, int rate, int lanes, int *max_rate, int *max_lanes);
const char *drv_i915_dp_ext_status_name(enum i915_dp_ext_status status);
const char *drv_i915_dp_ext_step_name(enum i915_dp_ext_step step);
void drv_i915_dp_ext_log(const struct i915_dp_ext_env *env, const struct i915_dp_ext_sink *sink, const char *name);

#endif
