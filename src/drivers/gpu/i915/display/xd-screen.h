/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A screen of the display 12 and 13 code: one output, its pipe and the
 * plan of its mode set (xd-screen.c).
 *
 * The plan is a pure function of the mode, the sink, the platform's
 * description and what the display runs now: the link (rate, lanes, bits
 * per pixel, M/N), the combo PLL's words, the display buffer's slices for
 * the pipes that run together, the plane's watermarks and its share of
 * the display buffer, and the check that the mode needs no other CD
 * clock, no other voltage level and no more memory bandwidth than the
 * allowed QGV point gives.  Nothing is written.
 *
 * Lighting and darkening a screen run its steps in the order an output is
 * enabled and disabled (the same order for the three kinds; what differs
 * between them is inside a step): for the light, the power, the display
 * buffer, the setup, the pipe's DMC, the link's power, the PLL, the
 * port, the pipe, the output, the check, the plane and the commit's
 * settle; for the dark, the power, the plane, the output, the port, the
 * PLL, the link's power, the pipe's DMC, the confirmation and the settle.
 * Each step is the caller's (it owns the state the steps keep); the order,
 * the trace of each step, what a failure skips, the undo of a failed
 * light and the screen's phase are the screen's.  A failed step skips the
 * rest but the settle of a light; a dark whose confirmation fails is
 * retained: its settle is skipped, so nothing is given back under a pipe
 * that may still run.
 *
 * An update that must land in one frame (a flip's arming writes) stays
 * out of the lines just before the vertical blank: it sleeps one blank
 * at a time until the scan line is outside them, then writes with
 * interrupts off and arms its completion event for the next blank.
 *
 * A flip arms a new buffer on a lit screen; the pipe latches it at its
 * next vertical blank, and only the live surface tells that the old
 * buffer is free.  A flip whose event did not come, or came without the
 * new buffer, is stuck.  A resident run ends served, unplugged, asked to
 * light again for two pipes, or failed; an external DisplayPort link that
 * did not train is lowered and the run is tried again (EAGAIN).
 */

#ifndef DRIVERS_GPU_I915_DISPLAY_XD_SCREEN_H
#define DRIVERS_GPU_I915_DISPLAY_XD_SCREEN_H

#include <stdint.h>

#include "display-internal.h"
#include "xd-clock.h"
#include "xd-dbuf.h"
#include "xd-dp.h"
#include "xd-io.h"
#include "xd-pipe.h"
#include "xd-platform.h"

/* The kinds of output a screen drives: the built-in panel, an HDMI or DVI sink, an external DisplayPort sink. */
#define I915_XD_SCREEN_PANEL		1U
#define I915_XD_SCREEN_HDMI		2U
#define I915_XD_SCREEN_DP		3U

/*
 * Why a plan was refused: a sink the plan does not drive (a rate code or
 * lane count it does not know, an eDP 1.4 panel's rate table), a mode no
 * link carries, a link rate without PLL words, a mode that needs another
 * CD clock (or more than the highest), one that needs another voltage
 * level, one that needs more memory bandwidth than the allowed QGV point,
 * and one whose watermarks the vertical blank or the display buffer
 * cannot hold.
 */
#define I915_XD_PLAN_OK			0U
#define I915_XD_PLAN_SINK		1U
#define I915_XD_PLAN_LINK		2U
#define I915_XD_PLAN_PLL		3U
#define I915_XD_PLAN_CDCLK		4U
#define I915_XD_PLAN_VOLTAGE		5U
#define I915_XD_PLAN_QGV		6U
#define I915_XD_PLAN_WM			7U

/*
 * What the plan knows of the sink.  A panel: its DPCD rate code and lane
 * count, its eDP revision, and the VBT's bits per pixel cap (0: none).  An
 * external DisplayPort sink: the rates both ends share (lowest first),
 * its lanes, a branch's bits per colour cap and dot clock and TMDS clock
 * limits (0: none), and the fallback's rate and lane limits (0: none).
 */
struct i915_xd_sink {
	uint8_t rate_code;
	unsigned max_lanes;
	uint8_t edp_revision;
	unsigned vbt_bpp;
	const uint32_t *rates;
	unsigned nrates;
	unsigned max_bpc;
	uint32_t max_dotclock_khz;
	uint32_t min_tmds_khz;
	uint32_t max_tmds_khz;
	uint32_t rate_limit_khz;
	unsigned lane_limit;
};

/*
 * What a plan is computed from: the kind of output, its DDI and pipe, the
 * mode, the sink, the platform's description and the PLL reference, the
 * pipes that run together (this one among them), and for the check the
 * CD clock now and its highest step, the bandwidth of the allowed QGV
 * point (MB/s) and the plane's bytes per pixel.  For the watermarks: the
 * memory's levels and latencies (the plan fills in the pipe's pixel rate
 * and line total), the plane's buffer size, the pipe's frame start delay,
 * and the slices and weights the other pipes have in the display buffer
 * now (this pipe's entries are not read).
 */
struct i915_xd_plan_input {
	unsigned kind;
	unsigned ddi;
	unsigned pipe;
	const struct i915_display_mode *mode;
	struct i915_xd_sink sink;
	const struct i915_xd_platform *platform;
	uint32_t ref_khz;
	uint8_t run_pipes;
	struct i915_xd_cdclk cdclk;
	uint32_t cdclk_max_khz;
	uint32_t qgv_mbps;
	unsigned cpp;
	struct i915_xd_wm_pipe wm;
	uint32_t fb_width;
	uint32_t fb_height;
	unsigned frame_start_delay;
	uint8_t pipe_slices[4];
	uint16_t pipe_weight[4];
};

/*
 * A plan: the link, its M/N, the combo PLL's words (pll_set zero when the
 * port's PLL is not a combo DPLL), the display buffer's slices and
 * whether the bus is joined, the slices powered for all the pipes, the
 * plane's watermarks (whether the vertical blank turned the deeper levels
 * off), the pipe's weight and its share of the buffer and the plane's and
 * the cursor's parts of it (the error when they do not fit), the check's
 * numbers (pixel and data rates, the CD clock the pipe and the memory
 * read bandwidth need and the larger, the one it gets, its VCO and
 * voltage level), and why it was refused (I915_XD_PLAN_OK when it was
 * not).
 */
struct i915_xd_plan {
	unsigned kind;
	unsigned pipe;
	struct i915_display_mode mode;
	uint32_t rate_khz;
	unsigned lanes;
	unsigned bpp;
	uint32_t need_kbps;
	uint32_t carry_kbps;
	struct i915_xd_dp_m_n m_n;
	struct i915_xd_dpll_words pll;
	int pll_set;
	uint8_t slices;
	uint8_t joined;
	uint8_t enabled_slices;
	unsigned wm_levels;
	struct i915_xd_plane_wm wm;
	int wm_cut;
	uint16_t weight;
	struct i915_xd_ddb pipe_ddb;
	struct i915_xd_ddb plane_ddb;
	struct i915_xd_ddb cursor_ddb;
	int wm_error;
	uint32_t pixel_khz;
	uint32_t data_mbps;
	uint32_t crtc_min_cdclk_khz;
	uint32_t bw_min_cdclk_khz;
	uint32_t min_cdclk_khz;
	uint32_t cdclk_khz;
	uint32_t vco_khz;
	uint8_t min_voltage;
	uint8_t voltage;
	unsigned refused;
};

/* The phases of a screen: off, planned, powered, linked (the port is up), lit, and retained after a stop not confirmed. */
#define I915_XD_SCREEN_OFF		0U
#define I915_XD_SCREEN_PLANNED		1U
#define I915_XD_SCREEN_POWERED		2U
#define I915_XD_SCREEN_LINKED		3U
#define I915_XD_SCREEN_LIT		4U
#define I915_XD_SCREEN_RETAINED		5U

/*
 * The caller's steps of a light and of a dark, each 0 or an error.  A
 * step may be NULL (nothing to do).  The light's settle runs even after a
 * failed step.
 */
struct i915_xd_screen_hooks {
	int (*power)(void *context);
	int (*dbuf)(void *context);
	int (*setup)(void *context);
	int (*dmc_on)(void *context);
	int (*link_power)(void *context);
	int (*pll)(void *context);
	int (*port)(void *context);
	int (*pipe)(void *context);
	int (*output)(void *context);
	int (*check)(void *context);
	int (*plane)(void *context);
	int (*settle)(void *context);
	int (*dark_power)(void *context);
	int (*plane_off)(void *context);
	int (*output_off)(void *context);
	int (*port_off)(void *context);
	int (*pll_off)(void *context);
	int (*link_power_off)(void *context);
	int (*dmc_off)(void *context);
	int (*confirm)(void *context);
	int (*dark_settle)(void *context);
	void *context;
};

/*
 * One screen: its kind, its pipe, its phase, and the steps of the last
 * light or dark (how many ran, the one that failed (-1 for none) and its
 * error).
 */
struct i915_xd_screen {
	unsigned kind;
	unsigned pipe;
	unsigned phase;
	unsigned ran;
	int failed;
	int error;
};

/*
 * A screen's flips: the buffer displayed, the one a flip armed (pending
 * until the pipe latches it), the one it replaces, whether a flip is
 * armed, whether one never completed (stuck: every later flip is refused
 * and both buffers stay the display's until the screen stops), and the
 * generation of the last flip.
 */
struct i915_xd_flip {
	uint32_t current;
	uint32_t pending;
	uint32_t old;
	int armed;
	int stuck;
	unsigned gen;
};

/* Why a flip is refused: the screen is not lit, retained, busy, stuck, the same buffer, an unaligned one. */
#define I915_XD_FLIP_OK			0U
#define I915_XD_FLIP_NOT_LIT		1U
#define I915_XD_FLIP_RETAINED		2U
#define I915_XD_FLIP_BUSY		3U
#define I915_XD_FLIP_STUCK		4U
#define I915_XD_FLIP_SAME		5U
#define I915_XD_FLIP_UNALIGNED		6U

/* What a waited flip did: latched (the old buffer is free), the event came without the new buffer, no event. */
#define I915_XD_FLIP_DONE		0U
#define I915_XD_FLIP_NOT_LATCHED	1U
#define I915_XD_FLIP_TIMEOUT		2U

/*
 * Why a resident run ended: it served until the display was given back,
 * its display was unplugged, the second output asked for it to be lit
 * again for two pipes, or it failed.
 */
#define I915_XD_RUN_SERVED		0U
#define I915_XD_RUN_UNPLUGGED		1U
#define I915_XD_RUN_RELIGHT		2U
#define I915_XD_RUN_FAILED		3U

/*
 * How a resident run went, as the caller saw it: whether the picture was
 * up and stopped cleanly, the buffers released, the power references it
 * still holds, the display unplugged, a relight asked for, whether the
 * buffers were handed to the display, and whether an external
 * DisplayPort link was lowered after it did not train (the run is to be
 * tried again).
 */
struct i915_xd_run_outcome {
	int passed;
	int released;
	int held;
	int unplugged;
	int relight;
	int acquired;
	int lowered;
};

/* The verdict of a resident run: why it ended and whether the display got the buffers. */
struct i915_xd_run_result {
	unsigned reason;
	int acquired;
};

/*
 * The caller's side of an update that must land in one frame: a
 * reference to the pipe's vertical blank interrupt (0, or an error) and
 * its release, a sleep until the pipe's next vertical blank or a number
 * of scheduler ticks (the ticks left, 0 when the blank did not come),
 * interrupts off and on, the arming writes, the completion event armed
 * for the next blank (NULL: the update has none; the event holds a
 * reference of its own), and an error of the update.
 */
struct i915_xd_update_hooks {
	int (*vblank_get)(void *context);
	void (*vblank_put)(void *context);
	long (*vblank_sleep)(void *context, long ticks);
	void (*irq_off)(void *context);
	void (*irq_on)(void *context);
	void (*arm)(void *context);
	void (*arm_event)(void *context);
	void (*error)(void *context, const char *what);
	void *context;
};

/*
 * An update: its pipe, the lines of its frame, the scan line counter's
 * offset, and the lines it stays out of.
 */
struct i915_xd_update {
	unsigned pipe;
	unsigned vtotal;
	unsigned scanline_offset;
	struct i915_xd_evade window;
};

/*
 * What an update saw: the line and the frame it wrote in and ended at
 * (start_frame 0 when it did not wait: then it is not checked), the
 * sleeps, whether it gave up waiting, and whether it crossed a vertical
 * blank (its writes may have landed in two frames).
 */
struct i915_xd_update_report {
	unsigned start_line;
	unsigned end_line;
	uint32_t start_frame;
	uint32_t end_frame;
	unsigned sleeps;
	int gave_up;
	int crossed;
};

unsigned drv_i915_xd_screen_plan_link(const struct i915_xd_plan_input *input, struct i915_xd_plan *plan);
unsigned drv_i915_xd_screen_plan_check(const struct i915_xd_plan_input *input, struct i915_xd_plan *plan);
unsigned drv_i915_xd_screen_plan(const struct i915_xd_plan_input *input, struct i915_xd_plan *plan);
const char *drv_i915_xd_screen_refusal_name(unsigned refused);
void drv_i915_xd_screen_update(struct i915_xd_io *io, const struct i915_xd_update *update, const struct i915_xd_update_hooks *hooks, struct i915_xd_update_report *report);
int drv_i915_xd_screen_light(struct i915_xd_screen *screen, struct i915_xd_io *io, const struct i915_xd_screen_hooks *hooks, int undo);
int drv_i915_xd_screen_dark(struct i915_xd_screen *screen, struct i915_xd_io *io, const struct i915_xd_screen_hooks *hooks);
unsigned drv_i915_xd_flip_refused(const struct i915_xd_flip *flip, int lit, int retained, uint32_t surface);
void drv_i915_xd_flip_arm(struct i915_xd_flip *flip, uint32_t surface);
unsigned drv_i915_xd_flip_judge(struct i915_xd_flip *flip, int event_error, uint32_t live);
int drv_i915_xd_flip_idle(struct i915_xd_flip *flip, uint32_t live);
int drv_i915_xd_flip_settled(struct i915_xd_flip *flip, int event_error, uint32_t live);
int drv_i915_xd_screen_run_verdict(const struct i915_xd_run_outcome *outcome, struct i915_xd_run_result *result);

#endif /* DRIVERS_GPU_I915_DISPLAY_XD_SCREEN_H */
