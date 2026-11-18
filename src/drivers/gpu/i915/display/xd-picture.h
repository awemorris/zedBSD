/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The pictures of a display: what each screen shows and how it is lit,
 * flipped and stopped (xd-picture.c).
 *
 * A display drives up to two screens: the resident one (the panel, or
 * the output the resident run moved to) and the head (a second output a
 * session claims).  A screen is prepared from a configuration (its
 * output, its mode and link, its framebuffer and what the display's
 * start found), lit by a light commit, flipped between buffers and
 * stopped by a dark commit.  The screens share the device's PLLs and its
 * display buffer, whose states live beside them.  Every hardware access
 * goes through the backend the screen was prepared with.
 */

#ifndef DRIVERS_GPU_I915_DISPLAY_XD_PICTURE_H
#define DRIVERS_GPU_I915_DISPLAY_XD_PICTURE_H

#include "display-internal.h"
#include "xd-backend.h"
#include "xd-clock.h"
#include "xd-dbuf.h"
#include "xd-io.h"
#include "xd-output.h"
#include "xd-pipe.h"
#include "xd-platform.h"
#include "xd-screen.h"
#include "xd-tc.h"

#include <stdint.h>

/* The screens of a display: the resident one and the head. */
#define I915_XD_PICTURE_SCREENS		2U
#define I915_XD_PICTURE_RESIDENT	0U
#define I915_XD_PICTURE_HEAD		1U

/*
 * The device's port PLLs, by their select number: the two combo PLLs,
 * the Thunderbolt PLL the Type-C ports share, and the four Type-C ports'
 * own PLLs.
 */
#define I915_XD_PICTURE_PLLS		7U
#define I915_XD_PICTURE_PLL_TBT		2U
#define I915_XD_PICTURE_PLL_FIRST_TC	3U

/* The DDI of the first Type-C port. */
#define I915_XD_PICTURE_FIRST_TC_DDI	3U

/*
 * What a commit, a plane change or a brightness change answers: done;
 * the screen is not in a state that allows it; an error was reported on
 * the way (the status names the first one); a DisplayPort link did not
 * train; something is still held after a dark.
 */
#define I915_XD_PICTURE_OK			0
#define I915_XD_PICTURE_NOT_READY		(-1)
#define I915_XD_PICTURE_ERRORS			(-2)
#define I915_XD_PICTURE_LINK_NOT_TRAINED	(-3)
#define I915_XD_PICTURE_STILL_OWNED		(-4)

/*
 * What a flip did: the new buffer is shown and the old one is free; the
 * event came without the new buffer; no event came; nothing was written;
 * armed, to latch at the next vertical blank.  Only the first frees the
 * old buffer.
 */
#define I915_XD_PICTURE_FLIP_DONE		0
#define I915_XD_PICTURE_FLIP_NOT_LATCHED	1
#define I915_XD_PICTURE_FLIP_TIMEOUT		2
#define I915_XD_PICTURE_FLIP_REFUSED		3
#define I915_XD_PICTURE_FLIP_ARMED		4

/*
 * The configuration a screen is prepared from.
 *
 * The output: its kind (I915_XD_SCREEN_PANEL, _HDMI or _DP), its DDI, its
 * pipe and transcoder, its AUX channel, the PLL the caller expects it to
 * run on, the other pipes the same configuration lights, the VBT's HDMI
 * level (below 0: none), the DDI buffer bits to keep (the lane reversal),
 * and the hooks of a sink with an AUX channel of its own (NULL: the
 * backend's, the panel's).  The sink's capabilities and the panel's VBT
 * data.  What the display's start found: the raw clock, the memory's
 * watermark latencies, the display buffer's size and slices and the
 * slices on now, the CD clock and its limit, the memory bandwidth
 * allowed, the DMC programs loaded, and whether the VBT overrides the
 * PLLs' AFC startup.  The mode and the link (rate, lanes, depth, M/N) the
 * output's plan chose, the reference clock and a combo PLL's words, and
 * the framebuffer the screen shows first.
 */
struct i915_xd_picture_config {
	unsigned kind;
	unsigned ddi;
	unsigned pipe;
	unsigned trans;
	unsigned aux;
	unsigned pll;
	uint8_t also_pipes;
	int hdmi_level;
	uint32_t kept;
	const struct i915_xd_sink_hooks *sink;
	uint8_t caps[15];
	uint8_t edp_caps[3];
	int vbt_low_vswing;
	int vbt_hobl;
	int vbt_backlight_present;
	int vbt_backlight_active_low;
	int vbt_backlight_controller;
	uint16_t vbt_backlight_hz;
	uint8_t vbt_backlight_min;
	uint32_t rawclk_khz;
	uint16_t wm_latency[8];
	uint8_t wm_levels;
	int wm_ipc;
	uint8_t sagv_block_us;
	uint32_t dbuf_size;
	uint8_t dbuf_slice_mask;
	uint8_t dbuf_slices_now;
	int mbus_joined_now;
	struct i915_xd_cdclk cdclk;
	uint32_t cdclk_max_khz;
	uint32_t qgv_mbps;
	uint32_t dmc_programs;
	int afc_override;
	struct i915_display_mode mode;
	uint32_t rate_khz;
	unsigned lanes;
	unsigned bpp;
	struct i915_xd_dp_m_n m_n;
	uint32_t pll_ref_khz;
	struct i915_xd_dpll_words pll_words;
	uint32_t fb_fourcc;
	uint64_t fb_modifier;
	uint32_t fb_width;
	uint32_t fb_height;
	uint32_t fb_pitch;
	uint32_t fb_surf;
};

/*
 * What a screen holds, as its status reports it.
 *
 * How far it got (prepared, the pipe lit, the plane armed); the link as
 * it was set and trained, and the sink's link status when it was read
 * (link_status_rc 0, cr_ok and eq_ok judged from it); the PLL it runs on
 * (its id, on, the pipes running on it and those that reserved it) and
 * the port's power cookies; the backlight; the watermarks and display
 * buffer the plan gave; what the CD clock and the bandwidth need; the
 * display kept out of DC states, the power held for the pipe, the
 * display buffer now; a dark not confirmed or a screen retained; the
 * flips; the user's brightness; the commits and their errors.
 */
struct i915_xd_picture_status {
	int prepared;
	int crtc_active;
	int plane_armed;
	int link_rate;
	int lane_count;
	int link_trained_flag;
	int link_status_rc;
	uint8_t link_status[6];
	int cr_ok;
	int eq_ok;
	uint8_t train_set[4];
	uint32_t ddi_buf_ctl_value;
	int backlight_present;
	int backlight_enabled;
	int backlight_setup_rc;
	uint32_t backlight_pwm_max;
	uint32_t backlight_level;
	int wm_rc;
	uint16_t ddb_start;
	uint16_t ddb_end;
	uint16_t wm0_blocks;
	uint16_t wm0_lines;
	int wm0_enable;
	uint8_t dbuf_slices_wanted;
	int mbus_joined;
	int cdclk_rc;
	int cdclk_crtc_min;
	int cdclk_bw_min;
	int cdclk_required_khz;
	int cdclk_required_vco;
	int cdclk_required_level;
	int cdclk_change_needed;
	unsigned bw_data_rate;
	int dc_off_held;
	unsigned crtc_domains_held;
	uint8_t dbuf_slices_now;
	int mbus_joined_now;
	unsigned dbuf_active_pipes_now;
	int stop_unconfirmed;
	int retained;
	int dither;
	uint32_t cur_surf;
	uint32_t pend_surf;
	int flip_pending;
	int flip_stuck;
	unsigned flip_gen;
	int flip_event_ref;
	unsigned events_cancelled;
	uint32_t backlight_min;
	uint32_t backlight_max;
	uint32_t backlight_user;
	uint32_t backlight_user_max;
	unsigned commits;
	int pll_on;
	int pll_active_mask;
	int pll_wakeref;
	int pll_id;
	int pll_pipe_mask;
	int ddi_io_wakeref;
	int aux_wakeref;
	unsigned errors;
	const char *first_error;
};

/*
 * What one flip did: its generation, the buffers before and after and
 * the one the plane scanned out before and after, the pipe's frame
 * counter before and after, the event's answer, the errors reported
 * during the update, and the I915_XD_PICTURE_FLIP_* result.
 */
struct i915_xd_picture_flip {
	unsigned gen;
	uint32_t old_surf;
	uint32_t new_surf;
	uint32_t live_before;
	uint32_t live_after;
	uint32_t frame_before;
	uint32_t frame_after;
	int event_rc;
	int update_errors;
	int result;
};

/*
 * The display buffer of the device as the last commit left it: whether
 * one was left yet, the pipes it is laid out for (those a running screen
 * left room for included), the slices of each pipe and those powered,
 * whether the bus is joined, and each pipe's weight and range.
 */
struct i915_xd_picture_dbuf {
	uint8_t pipes;
	uint8_t slices[4];
	uint8_t enabled;
	int joined;
	uint16_t weight[4];
	struct i915_xd_ddb ddb[4];
};

/*
 * One port PLL of the device: the pipes that reserved it with their
 * words (a prepare reserves, a confirmed dark gives back), the words they
 * share, and the pipes running on it and whether it is on (the lights and
 * darks keep these).
 */
struct i915_xd_picture_pll {
	uint8_t reserved;
	struct i915_xd_dpll_words words;
	struct i915_xd_tc_pll_words tc_words;
	struct i915_xd_mg_pll_words mg_words;
	uint8_t pipes;
	int on;
};

struct i915_xd_pictures;

/*
 * One screen.
 *
 * Prepare clears it and fills it from the configuration; from then on
 * the commits, the flips and the backlight work on it through its
 * backend, whose register window and Dekel route it keeps.  Only the
 * display's owner thread prepares, lights and darkens a screen, one
 * operation at a time; a presenting thread only waits for a flip to
 * latch, reading the plane's live surface through the backend.
 */
struct i915_xd_picture {
	struct i915_xd_pictures *pictures;
	const struct i915_xd_backend *backend;
	const struct i915_xd_sink_hooks *sink;
	struct i915_xd_io window;
	struct i915_xd_dkl dkl;
	struct i915_xd_picture_config config;
	int prepared;
	unsigned pll;
	struct i915_xd_dpll_words tbt_words;
	struct i915_xd_output out;
	struct i915_xd_screen screen;
	struct i915_xd_plan plan;
	struct i915_xd_plane plane;
	struct i915_xd_flip flip;
	struct i915_xd_picture_dbuf old_dbuf;
	struct i915_xd_picture_dbuf new_dbuf;
	int wm_rc;
	int cdclk_rc;
	int cdclk_crtc_min;
	int cdclk_bw_min;
	int cdclk_min;
	int cdclk_khz;
	int cdclk_vco;
	int cdclk_voltage;
	int cdclk_change_needed;
	unsigned bw_data_rate;
	uint32_t link_rate;
	unsigned link_lanes;
	int backlight_setup_rc;
	uint32_t user_level;
	uint32_t user_max;
	int stop_unconfirmed;
	int plane_armed;
	int flip_event_ref;
	unsigned events_cancelled;
	unsigned commits;
	unsigned errors;
	const char *first_error;
};

/*
 * The screens of a display and what they share.
 *
 * platform describes the device.  retained (with the backend it belongs
 * to) outlives a prepare's clearing of its screen: only discarding a
 * model's state clears it.  The display buffer and the PLLs are the
 * device's (dbuf_valid is zero until a commit left a layout).  irqs_off
 * is nonzero inside a flip's short update with interrupts off.  current
 * is the screen whose operation runs: an error the backend reports
 * outside a screen's own hooks is counted there.
 */
struct i915_xd_pictures {
	const struct i915_xd_platform *platform;
	struct i915_xd_picture screen[I915_XD_PICTURE_SCREENS];
	int retained[I915_XD_PICTURE_SCREENS];
	const struct i915_xd_backend *retained_backend[I915_XD_PICTURE_SCREENS];
	struct i915_xd_picture_dbuf dbuf;
	int dbuf_valid;
	struct i915_xd_picture_pll pll[I915_XD_PICTURE_PLLS];
	int irqs_off;
	unsigned current;
};

void drv_i915_xd_pictures_init(struct i915_xd_pictures *pictures, const struct i915_xd_platform *platform);
void drv_i915_xd_pictures_platform(struct i915_xd_pictures *pictures, const struct i915_xd_platform *platform);
void drv_i915_xd_pictures_forget(struct i915_xd_pictures *pictures);
void drv_i915_xd_pictures_fault(struct i915_xd_pictures *pictures, const char *what);
int drv_i915_xd_picture_prepare(struct i915_xd_pictures *pictures, unsigned screen, const struct i915_xd_picture_config *config, const struct i915_xd_backend *backend);
int drv_i915_xd_picture_light(struct i915_xd_pictures *pictures, unsigned screen);
int drv_i915_xd_picture_dark(struct i915_xd_pictures *pictures, unsigned screen);
int drv_i915_xd_picture_plane_off(struct i915_xd_pictures *pictures, unsigned screen);
void drv_i915_xd_picture_released(struct i915_xd_pictures *pictures, unsigned screen);
void drv_i915_xd_picture_abandoned(struct i915_xd_pictures *pictures, unsigned screen);
int drv_i915_xd_picture_retained(struct i915_xd_pictures *pictures, unsigned screen);
int drv_i915_xd_picture_discard_model(struct i915_xd_pictures *pictures, unsigned screen, const struct i915_xd_backend *backend);
void drv_i915_xd_picture_status(struct i915_xd_pictures *pictures, unsigned screen, struct i915_xd_picture_status *status);
void drv_i915_xd_picture_link_status(struct i915_xd_pictures *pictures, unsigned screen, struct i915_xd_picture_status *status);
int drv_i915_xd_picture_flip(struct i915_xd_pictures *pictures, unsigned screen, uint32_t surface, struct i915_xd_picture_flip *result);
int drv_i915_xd_picture_flip_nowait(struct i915_xd_pictures *pictures, unsigned screen, uint32_t surface, struct i915_xd_picture_flip *result);
int drv_i915_xd_picture_flip_poll(struct i915_xd_pictures *pictures, unsigned screen);
int drv_i915_xd_picture_flip_wait(struct i915_xd_pictures *pictures, unsigned screen);
int drv_i915_xd_picture_flip_settle(struct i915_xd_pictures *pictures, unsigned screen);
int drv_i915_xd_picture_brightness(struct i915_xd_pictures *pictures, unsigned screen, uint32_t level, uint32_t max);
int drv_i915_xd_picture_brightness_get(struct i915_xd_pictures *pictures, unsigned screen, uint32_t max, uint32_t *level);
int drv_i915_xd_picture_backlight(struct i915_xd_pictures *pictures, unsigned screen, int on);

#endif /* DRIVERS_GPU_I915_DISPLAY_XD_PICTURE_H */
