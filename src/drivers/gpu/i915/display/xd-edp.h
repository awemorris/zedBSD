/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The built-in panel's sink of the display 12 and 13 code (xd-edp.c):
 * the panel's power sequencer as the driver keeps it, and the panel's
 * DPCD and EDID reached over its AUX channel.
 *
 * The panel answers on its AUX channel only while its logic supply (VDD)
 * is on.  The sequencer can hold VDD on by itself; whoever needs the
 * channel asks for VDD, and when nobody needs it any more VDD is let go
 * a while later, so a run of accesses does not switch the panel's supply
 * for each one.  The panel must also stay off for its power cycle before
 * it comes on again, and the backlight waits its delays after the panel
 * came on and after it went off.  This file keeps those times and who
 * wants VDD, and runs the sequencer's steps through xd-panel.c.
 *
 * The sink is taken once when the display starts: the sequencer is
 * chosen and its delays programmed, VDD comes on, the receiver
 * capabilities, the eDP display control capabilities and the EDID are
 * read, and VDD stays on until the panel's final delays are known.  The
 * panel's steps (on, off, VDD on and off, the backlight enable) then
 * come from the screen's light and dark, and the DPCD accesses of the
 * link's training come through here too.
 *
 * Everything outside the registers is the caller's, through the
 * environment: the clock, the power references (the display core while
 * the sequencer is held, the AUX channel while VDD or a transfer is),
 * the sequencer's and the channel's locks, the delayed VDD-off (a timer
 * that calls drv_i915_xd_edp_vdd_work()), and the notes.
 */

#ifndef DRIVERS_GPU_I915_DISPLAY_XD_EDP_H
#define DRIVERS_GPU_I915_DISPLAY_XD_EDP_H

#include <stdint.h>

#include "xd-aux.h"
#include "xd-io.h"
#include "xd-panel.h"
#include "xd-platform.h"

/* The most EDID blocks the sink keeps: the base block and three extensions. */
#define I915_XD_EDP_EDID_BLOCKS		4U

/* The bytes of the eDP display control capabilities read from the sink. */
#define I915_XD_EDP_CAPS_BYTES		3U

/* What the panel's power is held for: the display core while the sequencer is held, and the AUX channel. */
#define I915_XD_EDP_POWER_CORE		0U
#define I915_XD_EDP_POWER_AUX		1U

/* The locks the sink takes: the sequencer's, and the AUX channel's (taken first). */
#define I915_XD_EDP_LOCK_PPS		0U
#define I915_XD_EDP_LOCK_AUX		1U

/*
 * The panel's steps a screen asks for: the power on, the power off (VDD
 * and the backlight enable go with it), VDD held on, VDD let go at once,
 * and the backlight enable set and cleared.
 */
#define I915_XD_EDP_PANEL_ON		0U
#define I915_XD_EDP_PANEL_OFF		1U
#define I915_XD_EDP_PANEL_VDD_ON	2U
#define I915_XD_EDP_PANEL_VDD_OFF	3U
#define I915_XD_EDP_BACKLIGHT_ON	4U
#define I915_XD_EDP_BACKLIGHT_OFF	5U

/*
 * The stages of taking the sink, which a failed start reports: the
 * sequencer, the receiver capabilities, the EDID.
 */
#define I915_XD_EDP_STAGE_NONE		0U
#define I915_XD_EDP_STAGE_PPS		1U
#define I915_XD_EDP_STAGE_CAPS		2U
#define I915_XD_EDP_STAGE_EDID		3U
#define I915_XD_EDP_STAGE_TAKEN		4U

/*
 * What the caller lets the sink reach.
 *
 * now_ms reads a clock in milliseconds that only grows.  power_get takes
 * a reference for a use (I915_XD_EDP_POWER_*) and returns a nonzero
 * cookie, or 0 when it could not; power_put gives it back, and
 * power_put_later lets it go a little later (an AUX transfer's, so a
 * burst of transfers does not switch the channel's well each time).
 * lock takes (taken nonzero) or drops one of the sink's locks.
 * vdd_later arms the delayed VDD-off to call drv_i915_xd_edp_vdd_work()
 * in so many milliseconds (an armed one is moved); vdd_cancel disarms it,
 * and with wait nonzero also waits for a call already running.  error
 * and debug take the sink's notes.
 */
struct i915_xd_edp_env {
	uint64_t (*now_ms)(void *context);
	int (*power_get)(void *context, unsigned use);
	void (*power_put)(void *context, unsigned use, int cookie);
	void (*power_put_later)(void *context, unsigned use, int cookie);
	void (*lock)(void *context, unsigned lock, int taken);
	void (*vdd_later)(void *context, unsigned delay_ms);
	void (*vdd_cancel)(void *context, int wait);
	void (*error)(void *context, const char *text);
	void (*debug)(void *context, const char *text);
	void *context;
};

/*
 * What the caller knows of the panel before the sink is taken: its AUX
 * channel, how many sequencers the south display has and whether the
 * second shares its pins (muxed), the sequencer the VBT names (-1 for
 * none), the VBT's delays (all 0 for none), and whether the machine
 * needs the longer power cycle of the quirk.
 */
struct i915_xd_edp_config {
	unsigned aux_channel;
	unsigned sequencers;
	int muxed;
	int controller;
	struct i915_xd_pps_delays vbt;
	int long_power_cycle;
};

/*
 * The panel's sink.
 *
 * The owner prepares it with drv_i915_xd_edp_init() and keeps it for the
 * display's life.  Under the sequencer's lock: the sequencer chosen, the
 * firmware's delays as first found, the VBT's, the programmed delays and
 * the waits in milliseconds; whether the delays are still the early ones
 * (VDD is then not let go); whether somebody wants VDD and the AUX
 * reference VDD holds (0: none); and the times the panel last lost its
 * power, came on, and lost its backlight.  What the start read: the
 * receiver capabilities, the eDP display control capabilities (and
 * whether the sink has them), the EDID's blocks and the extensions the
 * base block announces, and the stage reached.  live is nonzero from a
 * start that took the sink until the stop.
 */
struct i915_xd_edp {
	struct i915_xd_io *io;
	const struct i915_xd_platform *platform;
	struct i915_xd_edp_env env;
	struct i915_xd_aux aux;
	struct i915_xd_edp_config config;
	unsigned pps;
	struct i915_xd_pps_delays firmware;
	int firmware_known;
	struct i915_xd_pps_delays program;
	struct i915_xd_pps_waits waits;
	int initializing;
	int vdd_wanted;
	int vdd_cookie;
	uint64_t power_off_ms;
	uint64_t power_on_ms;
	uint64_t backlight_off_ms;
	uint8_t caps[15];
	uint8_t edp_caps[I915_XD_EDP_CAPS_BYTES];
	int edp_caps_read;
	uint8_t edid[I915_XD_EDP_EDID_BLOCKS * 128U];
	unsigned edid_blocks;
	unsigned edid_extensions;
	unsigned stage;
	int live;
};

void drv_i915_xd_edp_init(struct i915_xd_edp *edp, struct i915_xd_io *io, const struct i915_xd_platform *platform, const struct i915_xd_edp_env *env);
int drv_i915_xd_edp_start(struct i915_xd_edp *edp, const struct i915_xd_edp_config *config);
void drv_i915_xd_edp_late(struct i915_xd_edp *edp, int controller, const struct i915_xd_pps_delays *vbt);
void drv_i915_xd_edp_stop(struct i915_xd_edp *edp);
int drv_i915_xd_edp_panel(struct i915_xd_edp *edp, unsigned step);
void drv_i915_xd_edp_vdd_work(struct i915_xd_edp *edp);
int drv_i915_xd_edp_dpcd_read(struct i915_xd_edp *edp, uint32_t address, uint8_t *buffer, unsigned size);
int drv_i915_xd_edp_dpcd_write(struct i915_xd_edp *edp, uint32_t address, const uint8_t *buffer, unsigned size);
int drv_i915_xd_edp_read_caps(struct i915_xd_edp *edp, uint8_t *caps);

#endif /* DRIVERS_GPU_I915_DISPLAY_XD_EDP_H */
