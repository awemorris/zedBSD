/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The DisplayPort link of the display 12 and 13 code (xd-dp.c).
 *
 * A link is chosen before it is trained.  The rates a link can run at are
 * those both ends offer: the source's table up to the platform's and the
 * VBT's limit, the sink's standard rates up to its own limit and that of
 * any repeaters (or the list an eDP 1.4 panel gives).  For a mode, the
 * deepest colour first, then the lowest rate and the fewest lanes that
 * carry the mode's pixel rate is the link.  A link that fails to train
 * falls to the next lower shared rate, and below the lowest to half the
 * lanes at the highest rate.  The link's M/N values tell the transcoder
 * the stream's share of the link and the pixel clock against the link's
 * symbol clock.
 *
 * Training brings the sink's receivers up one after the other: any
 * repeaters (link training tunable PHY repeaters) from the highest
 * numbered down to repeater 1, then the sink.  Before it the repeaters are found and switched to
 * non-transparent mode, the sink's capabilities read again, and the
 * rate, lane count and coding written to the sink.  Each receiver is
 * trained in two phases: clock recovery with pattern 1, starting at the
 * lowest levels and following the levels the receiver asks for, and
 * channel equalization with the best pattern both ends know, again
 * following the receiver's requests.  The source's side of each step
 * (its signal levels, its pattern, the preparation of the port, the idle
 * pattern afterwards) is a hook of the caller; the sink's side is DPCD
 * access through another.
 *
 * Only 8b/10b links are trained: the platforms of this code do not drive
 * 128b/132b rates.
 *
 * Before a link, the sink of an external DisplayPort port is probed over
 * its AUX channel (struct i915_xd_dp_probe, the cache of one port): its
 * receiver capabilities, the repeaters in front of it, whether it is a
 * branch device (a USB-C to HDMI or DisplayPort adapter, a protocol
 * converter) with a display behind it, and the display's EDID.  The probe
 * does not switch repeaters to non-transparent mode (the training does),
 * drives every sink as one stream, and applies no DPCD quirk.  The AUX
 * transfers and the log come through struct i915_xd_dp_probe_env.
 */

#ifndef DRIVERS_GPU_I915_DISPLAY_XD_DP_H
#define DRIVERS_GPU_I915_DISPLAY_XD_DP_H

#include <stddef.h>
#include <stdint.h>

/* The most rates a list holds: every 8b/10b rate a source of these platforms offers. */
#define I915_XD_DP_RATES		8U

/* What drv_i915_xd_dp_rates() had to make up: a rate for a sink that offered none, a shared rate. */
#define I915_XD_DP_RATES_SINK_NONE	0x1U
#define I915_XD_DP_RATES_SHARED_NONE	0x2U

/* The kinds of note the training hands its caller. */
#define I915_XD_DP_NOTE_DEBUG		0U
#define I915_XD_DP_NOTE_ERROR		1U

/*
 * The rates both ends of a link offer: the source's, the sink's, and the
 * ones they share, each in increasing order (in kHz of the port clock).
 */
struct i915_xd_dp_rates {
	uint32_t source[I915_XD_DP_RATES];
	unsigned source_count;
	uint32_t sink[I915_XD_DP_RATES];
	unsigned sink_count;
	uint32_t common[I915_XD_DP_RATES];
	unsigned common_count;
};

/*
 * A link chosen for a mode: its rate, lane count and bits per pixel, the
 * data rate the mode needs and the one the link carries (kB/s).
 */
struct i915_xd_dp_choice {
	uint32_t rate_khz;
	unsigned lanes;
	unsigned bpp;
	uint32_t need_kbps;
	uint32_t carry_kbps;
};

/*
 * The M/N values of a stream on a link: the transfer unit size, the data
 * M and N (the stream's share of the link's payload) and the link M and
 * N (the pixel clock against the link's symbol clock).
 */
struct i915_xd_dp_m_n {
	uint32_t tu;
	uint32_t data_m;
	uint32_t data_n;
	uint32_t link_m;
	uint32_t link_n;
};

/*
 * The sink's side of a link: DPCD access through the link's AUX channel.
 *
 * read and write answer the bytes moved, or a negative error; probe
 * answers 0 when the sink is awake to answer at the address; read_caps
 * reads the receiver capabilities (and the extended ones a sink offers
 * in their place) and answers 0 or an error.
 */
struct i915_xd_dp_sink {
	int (*read)(void *context, uint32_t address, uint8_t *buffer, unsigned size);
	int (*write)(void *context, uint32_t address, const uint8_t *buffer, unsigned size);
	int (*probe)(void *context, uint32_t address);
	int (*read_caps)(void *context, uint8_t *caps);
	void *context;
};

/*
 * The source's side of a link: its signal levels from the four level
 * bytes, its training pattern from the pattern byte, the preparation of
 * the port before the sink is configured and the idle pattern after the
 * training (both may be NULL), a sleep of a range of microseconds, and
 * the notes of the training (may be NULL).
 */
struct i915_xd_dp_source {
	void (*levels)(void *context, const uint8_t set[4]);
	void (*pattern)(void *context, uint8_t pattern);
	void (*prepare)(void *context);
	void (*idle)(void *context);
	void (*sleep)(void *context, unsigned min_us, unsigned max_us);
	void (*note)(void *context, unsigned kind, const char *text);
	void *context;
};

/*
 * One training of a link.
 *
 * The caller fills in the link it wants, what the source can do, and the
 * storage the sink's capabilities live in between trainings (the
 * receiver capabilities, re-read by the training; the repeaters' common
 * and per-repeater capabilities).  The training leaves the levels of
 * each lane in set and the number of repeaters it trained in repeaters.
 */
struct i915_xd_dp_link {
	/* The link: rate, lanes, enhanced framing, and whether the sink ignores the stream's timing attributes. */
	uint32_t rate_khz;
	unsigned lanes;
	int enhanced;
	int msa_ignore;

	/* The index into an eDP 1.4 sink's rate list, or -1 to set the rate by its code. */
	int rate_index;

	/* The source: its highest swing and pre-emphasis levels, per-lane levels, patterns 3 and 4. */
	uint8_t swing_max;
	uint8_t preemph_max;
	int per_lane;
	int tps3;
	int tps4;

	/* Whether repeaters are looked for (not on eDP), and whether the link is up (repeaters keep their mode). */
	int find_repeaters;
	int active;

	/* The sink's capabilities the caller keeps. */
	uint8_t *caps;
	uint8_t *repeater_caps;
	uint8_t (*repeater_phy_caps)[3];

	/* The two sides. */
	struct i915_xd_dp_sink sink;
	struct i915_xd_dp_source source;

	/* What the training leaves: the level byte of each lane, and the repeaters trained. */
	uint8_t set[4];
	unsigned repeaters;
};

/* The receiver capability bytes at DPCD 0x000 (DP_RECEIVER_CAP_SIZE). */
#define I915_XD_DP_PROBE_DPCD_SIZE		15u

/* The downstream port capability bytes at DPCD 0x080 (DP_MAX_DOWNSTREAM_PORTS). */
#define I915_XD_DP_PROBE_DOWNSTREAM_SIZE	16u

/* The repeaters' common capability bytes at DPCD 0xf0000 (DP_LTTPR_COMMON_CAP_SIZE). */
#define I915_XD_DP_PROBE_LTTPR_SIZE		8u

/* The device identification string of a sink or branch (DPCD 0x403 or 0x503). */
#define I915_XD_DP_PROBE_DEVICE_ID_SIZE	6u

/* The EDID blocks kept: the base block and up to three extensions. */
#define I915_XD_DP_PROBE_EDID_BLOCKS		4u

/* The bytes of one EDID block. */
#define I915_XD_DP_PROBE_EDID_BLOCK_SIZE	128u

/* The most link rates one side offers (8b/10b, the display version 13 table). */
#define I915_XD_DP_PROBE_MAX_RATES		8u

/* What a probe concluded about the port. */
enum i915_xd_dp_probe_status {
	I915_XD_DP_PROBE_DISCONNECTED = 0,
	I915_XD_DP_PROBE_CONNECTED,
	I915_XD_DP_PROBE_UNKNOWN
};

/*
 * How far a probe got, for the log: the step it stopped at, or DONE.
 *
 * PHY: the port's PHY could not be taken for DP.  CAPS: the receiver did
 * not answer.  SINK_COUNT: a branch reported no display behind it, or the
 * count could not be read.  DOWNSTREAM: the branch's port capabilities
 * could not be read.  BRANCH: a branch that gave no sign of a display.
 */
enum i915_xd_dp_probe_step {
	I915_XD_DP_PROBE_STEP_NONE = 0,
	I915_XD_DP_PROBE_STEP_PHY,
	I915_XD_DP_PROBE_STEP_CAPS,
	I915_XD_DP_PROBE_STEP_SINK_COUNT,
	I915_XD_DP_PROBE_STEP_DOWNSTREAM,
	I915_XD_DP_PROBE_STEP_BRANCH,
	I915_XD_DP_PROBE_STEP_DONE
};

/* How a mode fares against a branch's downstream limits. */
enum i915_xd_dp_probe_mode_status {
	I915_XD_DP_PROBE_MODE_OK = 0,
	I915_XD_DP_PROBE_MODE_CLOCK_HIGH,
	I915_XD_DP_PROBE_MODE_CLOCK_LOW
};

/*
 * What the probe needs from the device: the port's AUX channel and the
 * log.
 *
 * The device binds one per port.  The DPCD calls return the bytes
 * transferred or a negative errno; read_caps and probe return 0 or a
 * negative errno; edid_read returns the valid blocks read or a negative
 * errno; ddc_probe returns nonzero when something answers
 * at the EDID address.
 */
struct i915_xd_dp_probe_env {
	/* What every callback is given. */
	void *ctx;

	/* DPCD reads and writes over the port's AUX channel. */
	long (*dpcd_read)(void *ctx, unsigned offset, uint8_t *buffer, size_t size);
	long (*dpcd_write)(void *ctx, unsigned offset, const uint8_t *buffer, size_t size);

	/* A throw-away read that wakes the sink. */
	int (*dpcd_probe)(void *ctx, unsigned offset);

	/* The receiver capabilities, the extended ones where the sink has them. */
	int (*read_caps)(void *ctx, uint8_t dpcd[I915_XD_DP_PROBE_DPCD_SIZE]);

	/* The DDC behind the channel (I2C over AUX). */
	int (*ddc_probe)(void *ctx);
	int (*edid_read)(void *ctx, uint8_t *buffer, unsigned max_blocks, unsigned *extensions);

	/* A line of the driver's log. */
	void (*log)(void *ctx, const char *format, ...) __attribute__((format(printf, 2, 3)));
};

/*
 * What the source offers on a port, which the probe meets with the sink.
 *
 * Link rates are in units of 10 kbit/s per lane (162000 is RBR, 810000 is HBR3).  max_rate is the platform's highest link
 * rate for the port (0: no limit); vbt_max_rate and vbt_max_lanes are the
 * VBT's limits (0: none); max_lanes is what the DDI drives; fia_lanes is
 * what the Type-C subsystem assigned to DisplayPort (0: not restricted).
 */
struct i915_xd_dp_probe_source {
	int max_rate;
	int vbt_max_rate;
	int max_lanes;
	int vbt_max_lanes;
	int fia_lanes;
};

/* The identification of a sink or a branch device. */
struct i915_xd_dp_probe_desc {
	/* The IEEE OUI of the vendor. */
	uint8_t oui[3];

	/* The device identification, not terminated. */
	char device_id[I915_XD_DP_PROBE_DEVICE_ID_SIZE];

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
struct i915_xd_dp_probe {
	/* What the probe concluded, where it stopped, and the AUX error that stopped it (0 when none). */
	enum i915_xd_dp_probe_status status;
	enum i915_xd_dp_probe_step step;
	int error;

	/* The receiver capabilities (DPCD 0x000), valid from the CAPS step on. */
	uint8_t dpcd[I915_XD_DP_PROBE_DPCD_SIZE];
	int dpcd_valid;

	/* The repeaters' common capabilities and how many repeaters they name (0 when none or invalid). */
	uint8_t lttpr[I915_XD_DP_PROBE_LTTPR_SIZE];
	int lttpr_valid;
	int lttpr_count;

	/* The sink's or the branch's identification, when it could be read. */
	struct i915_xd_dp_probe_desc desc;
	int desc_valid;

	/* Nonzero for a branch device (DP_DWN_STRM_PORT_PRESENT). */
	int branch;

	/* Whether the sink reports a sink count, and the count it reported. */
	int has_sink_count;
	int sink_count;

	/* The branch's downstream port capabilities (zero when not a branch). */
	uint8_t downstream[I915_XD_DP_PROBE_DOWNSTREAM_SIZE];

	/* The link rates (10 kbit/s per lane) each side offers and both share, lowest first. */
	int sink_rates[I915_XD_DP_PROBE_MAX_RATES];
	int num_sink_rates;
	int source_rates[I915_XD_DP_PROBE_MAX_RATES];
	int num_source_rates;
	int common_rates[I915_XD_DP_PROBE_MAX_RATES];
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
	uint8_t edid[I915_XD_DP_PROBE_EDID_BLOCKS * I915_XD_DP_PROBE_EDID_BLOCK_SIZE];
	unsigned edid_blocks;
	unsigned edid_extensions;

	/* Nonzero when the EDID announces an HDMI sink (an HDMI vendor-specific data block). */
	int has_hdmi_sink;
};

int drv_i915_xd9_dp_link(uint32_t sink_max_khz, unsigned sink_lanes, uint32_t pixel_khz, struct i915_xd_dp_choice *choice, struct i915_xd_dp_m_n *m_n);
uint32_t drv_i915_xd9_dp_sink_rate(uint8_t code);
uint8_t drv_i915_xd9_dp_rate_code(uint32_t rate_khz);
uint8_t drv_i915_xd_dp_rate_code(uint32_t rate_khz);
uint32_t drv_i915_xd_dp_code_rate(uint8_t code);
unsigned drv_i915_xd_dp_rates(uint32_t source_max_khz, uint32_t vbt_max_khz, uint8_t sink_code, uint8_t repeater_code, struct i915_xd_dp_rates *rates);
unsigned drv_i915_xd_dp_edp_rates(const uint8_t *table, uint32_t *rates, unsigned room);
int drv_i915_xd_dp_fallback(const uint32_t *common, unsigned count, uint32_t rate_khz, unsigned lanes, uint32_t *max_rate_khz, unsigned *max_lanes);
uint32_t drv_i915_xd_dp_need_kbps(uint32_t clock_khz, unsigned bpp);
uint32_t drv_i915_xd_dp_carry_kbps(uint32_t rate_khz, unsigned lanes);
uint32_t drv_i915_xd_dp_stream_kbps(uint32_t clock_khz, unsigned bpp_x16, uint32_t overhead_ppm);
unsigned drv_i915_xd_dp_symbol_bits(uint32_t rate_khz);
uint32_t drv_i915_xd_dp_symbol_khz(uint32_t rate_khz);
int drv_i915_xd_dp_choose(uint32_t clock_khz, unsigned max_bpc, unsigned min_bpc, const uint32_t *common, unsigned count, uint32_t rate_limit_khz, unsigned lane_limit, struct i915_xd_dp_choice *choice);
void drv_i915_xd_dp_m_n(unsigned bpp_x16, unsigned lanes, uint32_t clock_khz, uint32_t rate_khz, uint32_t overhead_ppm, struct i915_xd_dp_m_n *m_n);
int drv_i915_xd_dp_clock_recovered(const uint8_t *status, unsigned lanes);
int drv_i915_xd_dp_equalized(const uint8_t *status, unsigned lanes);
int drv_i915_xd_dp_read_status(const struct i915_xd_dp_sink *sink, unsigned hop, uint8_t *status);
int drv_i915_xd_dp_train(struct i915_xd_dp_link *link);
void drv_i915_xd_dp_train_end(struct i915_xd_dp_link *link);

void drv_i915_xd_dp_probe_forget(struct i915_xd_dp_probe *sink);
void drv_i915_xd_dp_probe_detect(const struct i915_xd_dp_probe_env *env, const struct i915_xd_dp_probe_source *source, struct i915_xd_dp_probe *sink);
int drv_i915_xd_dp_probe_short_pulse(const struct i915_xd_dp_probe_env *env, const struct i915_xd_dp_probe *sink);
int drv_i915_xd_dp_probe_link_needs_retrain(const struct i915_xd_dp_probe_env *env, int lane_count);
int drv_i915_xd_dp_probe_configure_converter(const struct i915_xd_dp_probe_env *env, const struct i915_xd_dp_probe *sink);
enum i915_xd_dp_probe_mode_status drv_i915_xd_dp_probe_mode_valid(const struct i915_xd_dp_probe *sink, int clock_khz);
int drv_i915_xd_dp_probe_fallback_values(const struct i915_xd_dp_probe *sink, int rate, int lanes, int *max_rate, int *max_lanes);
const char *drv_i915_xd_dp_probe_status_name(enum i915_xd_dp_probe_status status);
const char *drv_i915_xd_dp_probe_step_name(enum i915_xd_dp_probe_step step);
void drv_i915_xd_dp_probe_log(const struct i915_xd_dp_probe_env *env, const struct i915_xd_dp_probe *sink, const char *name);
#endif /* DRIVERS_GPU_I915_DISPLAY_XD_DP_H */
