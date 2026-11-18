/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A screen of the display 12 and 13 code (see xd-screen.h).
 */

#include "xd-screen.h"
#include "xd-clock.h"
#include "xd-dbuf.h"
#include "xd-dp.h"
#include "xd-io.h"
#include "xd-platform.h"
#include "display-internal.h"

#include "../intel/display-dpcd-xd.h"

#include <kern/clock.h>
#include <kern/kcrt.h>

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The eDP revision from which a panel lists its own link rates (eDP 1.4),
 * and the bandwidth overhead of a single stream link in parts per
 * million of the payload (none on an 8b/10b link).
 *
 * Values referenced: Linux v6.8.12 include/drm/display/drm_dp.h
 * (DP_EDP_14), drivers/gpu/drm/i915/display/intel_dp.c
 * (intel_dp_bw_fec_overhead without FEC or DSC).
 */
#define I915_XD_SCREEN_EDP_14		0x03U
#define I915_XD_SCREEN_SST_OVERHEAD	1000000U

/*
 * The bits per colour an external DisplayPort sink is driven at, at most
 * and at least; the bits per pixel of a sink that names no depth and of
 * an HDMI sink.
 */
#define I915_XD_SCREEN_DP_MAX_BPC	8U
#define I915_XD_SCREEN_DP_MIN_BPC	6U
#define I915_XD_SCREEN_DEFAULT_BPP	24U

/*
 * The port clock above which display 12 and 13 need voltage level 2, and
 * that level.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi.c
 * (tgl_ddi_min_voltage_level).
 */
#define I915_XD_SCREEN_HIGH_PORT_KHZ	594000U
#define I915_XD_SCREEN_HIGH_PORT_LEVEL	2U

/*
 * The CD clock a pipe's memory read bandwidth needs: its data rate times
 * 10 over 512, from display 12 on.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_bw.c
 * (intel_bw_crtc_min_cdclk).
 */
#define I915_XD_SCREEN_BW_CDCLK_MUL	10U
#define I915_XD_SCREEN_BW_CDCLK_DIV	512U

/*
 * How long an update waits for the scan line to leave the lines before
 * the vertical blank, in milliseconds.  The wait is this rounded up to
 * ticks, plus one tick, as a deadline counts from the last tick.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_crtc.c
 * (intel_pipe_update_start: msecs_to_jiffies_timeout(1)).
 */
#define I915_XD_SCREEN_EVADE_MS		1U

/*
 * The steps of a light and of a dark, by the hook each runs: an index
 * into the caller's hooks (i915_xd_screen_hook).
 */
#define I915_XD_STEP_POWER		0U
#define I915_XD_STEP_DBUF		1U
#define I915_XD_STEP_SETUP		2U
#define I915_XD_STEP_DMC_ON		3U
#define I915_XD_STEP_LINK_POWER		4U
#define I915_XD_STEP_PLL		5U
#define I915_XD_STEP_PORT		6U
#define I915_XD_STEP_PIPE		7U
#define I915_XD_STEP_OUTPUT		8U
#define I915_XD_STEP_CHECK		9U
#define I915_XD_STEP_PLANE		10U
#define I915_XD_STEP_SETTLE		11U
#define I915_XD_STEP_DARK_POWER		12U
#define I915_XD_STEP_PLANE_OFF		13U
#define I915_XD_STEP_OUTPUT_OFF		14U
#define I915_XD_STEP_PORT_OFF		15U
#define I915_XD_STEP_PLL_OFF		16U
#define I915_XD_STEP_LINK_POWER_OFF	17U
#define I915_XD_STEP_DMC_OFF		18U
#define I915_XD_STEP_CONFIRM		19U
#define I915_XD_STEP_DARK_SETTLE	20U

/* A step that runs even after a failed one (the commit's settle). */
#define I915_XD_STEP_ALWAYS		0x1U

/* The dark's steps from which a failed light is undone, for a light step that undoes nothing. */
#define I915_XD_UNDO_NONE		0xffU

/*
 * One step of a light or a dark: its hook, its name in the register
 * trace, the phase the screen is in once it ran, whether it runs after a
 * failed step, and (for a light step) the dark's step from which it is
 * undone.
 */
struct i915_xd_screen_step {
	uint8_t hook;
	const char *name;
	uint8_t phase;
	uint8_t flags;
	uint8_t undo_from;
};

/*
 * The light: the order an output is enabled in (the power and the
 * display buffer first, the pipe's DMC, the link's power, the PLL, the
 * port with its link, the pipe, the output, the check of the link, the
 * plane, then the commit's settle).
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_display.c (intel_atomic_commit_tail,
 * skl_commit_modeset_enables, hsw_crtc_enable).
 */
static const struct i915_xd_screen_step i915_xd_screen_light_steps[] = {
	{ I915_XD_STEP_POWER, "xd-screen power", I915_XD_SCREEN_POWERED, 0U, 8U },
	{ I915_XD_STEP_DBUF, "xd-screen dbuf", I915_XD_SCREEN_POWERED, 0U, 8U },
	{ I915_XD_STEP_SETUP, "xd-screen setup", I915_XD_SCREEN_POWERED, 0U, 8U },
	{ I915_XD_STEP_DMC_ON, "xd-screen pipe dmc", I915_XD_SCREEN_POWERED, 0U, 6U },
	{ I915_XD_STEP_LINK_POWER, "xd-screen link power", I915_XD_SCREEN_POWERED, 0U, 5U },
	{ I915_XD_STEP_PLL, "xd-screen pll", I915_XD_SCREEN_POWERED, 0U, 4U },
	{ I915_XD_STEP_PORT, "xd-screen port", I915_XD_SCREEN_LINKED, 0U, 3U },
	{ I915_XD_STEP_PIPE, "xd-screen pipe", I915_XD_SCREEN_LINKED, 0U, 3U },
	{ I915_XD_STEP_OUTPUT, "xd-screen output", I915_XD_SCREEN_LINKED, 0U, 2U },
	{ I915_XD_STEP_CHECK, "xd-screen check", I915_XD_SCREEN_LINKED, 0U, 2U },
	{ I915_XD_STEP_PLANE, "xd-screen plane", I915_XD_SCREEN_LIT, 0U, 1U },
	{ I915_XD_STEP_SETTLE, "xd-screen settle", I915_XD_SCREEN_LIT, I915_XD_STEP_ALWAYS, I915_XD_UNDO_NONE },
};

/*
 * The dark: the order an output is disabled in (the power held, the
 * plane, the output, the port with the transcoder, the PLL, the link's
 * power, the pipe's DMC, the confirmation that the pipe stopped, then the
 * commit's settle).
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_display.c (intel_atomic_commit_tail,
 * intel_old_crtc_state_disables, hsw_crtc_disable).
 */
static const struct i915_xd_screen_step i915_xd_screen_dark_steps[] = {
	{ I915_XD_STEP_DARK_POWER, "xd-screen dark power", I915_XD_SCREEN_LIT, 0U, I915_XD_UNDO_NONE },
	{ I915_XD_STEP_PLANE_OFF, "xd-screen plane off", I915_XD_SCREEN_LINKED, 0U, I915_XD_UNDO_NONE },
	{ I915_XD_STEP_OUTPUT_OFF, "xd-screen output off", I915_XD_SCREEN_LINKED, 0U, I915_XD_UNDO_NONE },
	{ I915_XD_STEP_PORT_OFF, "xd-screen port off", I915_XD_SCREEN_POWERED, 0U, I915_XD_UNDO_NONE },
	{ I915_XD_STEP_PLL_OFF, "xd-screen pll off", I915_XD_SCREEN_POWERED, 0U, I915_XD_UNDO_NONE },
	{ I915_XD_STEP_LINK_POWER_OFF, "xd-screen link power off", I915_XD_SCREEN_POWERED, 0U, I915_XD_UNDO_NONE },
	{ I915_XD_STEP_DMC_OFF, "xd-screen pipe dmc off", I915_XD_SCREEN_POWERED, 0U, I915_XD_UNDO_NONE },
	{ I915_XD_STEP_CONFIRM, "xd-screen confirm", I915_XD_SCREEN_POWERED, 0U, I915_XD_UNDO_NONE },
	{ I915_XD_STEP_DARK_SETTLE, "xd-screen dark settle", I915_XD_SCREEN_OFF, 0U, I915_XD_UNDO_NONE },
};

/*
 * The dark's steps an undo runs end before its confirmation: the light's
 * own settle gives the power back.
 */
#define I915_XD_SCREEN_UNDO_END		7U

/* The number of steps of a light and of a dark. */
#define I915_XD_SCREEN_LIGHT_STEPS	(sizeof(i915_xd_screen_light_steps) / sizeof(i915_xd_screen_light_steps[0]))
#define I915_XD_SCREEN_DARK_STEPS	(sizeof(i915_xd_screen_dark_steps) / sizeof(i915_xd_screen_dark_steps[0]))

static int (*i915_xd_screen_hook(const struct i915_xd_screen_hooks *hooks, unsigned hook))(void *context);
static int i915_xd_screen_step_run(struct i915_xd_io *io, const struct i915_xd_screen_hooks *hooks, const struct i915_xd_screen_step *step);
static unsigned i915_xd_screen_panel_link(const struct i915_xd_plan_input *input, struct i915_xd_plan *plan);
static unsigned i915_xd_screen_hdmi_link(const struct i915_xd_plan_input *input, struct i915_xd_plan *plan);
static unsigned i915_xd_screen_dp_link(const struct i915_xd_plan_input *input, struct i915_xd_plan *plan);
static unsigned i915_xd_screen_combo_pll(const struct i915_xd_plan_input *input, struct i915_xd_plan *plan);
static int i915_xd_screen_plan_wm(const struct i915_xd_plan_input *input, struct i915_xd_plan *plan);
static uint32_t i915_xd_screen_round_up(uint64_t value, uint32_t divisor);

/*
 * Computes the link part of a plan: the link that carries the mode, its
 * M/N and the combo PLL's words.  Returns the refusal (I915_XD_PLAN_OK
 * when the link was found), also kept in plan->refused.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dp.c
 * (intel_dp_compute_link_config, intel_dp_compute_link_config_wide,
 * intel_dp_link_required, intel_dp_max_data_rate), intel_hdmi.c
 * (intel_hdmi_compute_config, intel_hdmi_tmds_clock), intel_dpll_mgr.c
 * (icl_calc_dp_combo_pll, icl_calc_wrpll).
 */
unsigned
drv_i915_xd_screen_plan_link(
	const struct i915_xd_plan_input *input,
	struct i915_xd_plan *plan)
{
	unsigned refused;

	/* Starts from the mode and nothing chosen. */
	kern_memset(plan, 0, sizeof(*plan));
	plan->kind = input->kind;
	plan->pipe = input->pipe;
	plan->mode = *input->mode;

	/* A mode without a clock is no mode a link carries. */
	if (input->mode->clock_khz <= 0) {
		plan->refused = I915_XD_PLAN_LINK;
		return plan->refused;
	}

	/* The link of the kind of output. */
	if (input->kind == I915_XD_SCREEN_PANEL) {
		refused = i915_xd_screen_panel_link(input, plan);
	} else if (input->kind == I915_XD_SCREEN_HDMI) {
		refused = i915_xd_screen_hdmi_link(input, plan);
	} else {
		refused = i915_xd_screen_dp_link(input, plan);
	}

	/* Refuses a link the sink cannot take. */
	if (refused != I915_XD_PLAN_OK) {
		plan->refused = refused;
		return refused;
	}

	/* A DisplayPort link's M/N. */
	if (input->kind != I915_XD_SCREEN_HDMI)
		drv_i915_xd_dp_m_n(plan->bpp * 16U, plan->lanes, (uint32_t)input->mode->clock_khz, plan->rate_khz, I915_XD_SCREEN_SST_OVERHEAD, &plan->m_n);

	/* The PLL's words, when the port runs on a combo DPLL. */
	refused = i915_xd_screen_combo_pll(input, plan);
	plan->refused = refused;

	/* Succeeded: the refusal, or none. */
	return refused;
}

/*
 * Computes the check part of a plan whose link is found: the display
 * buffer's slices for the pipes that run, the plane's watermarks and its
 * share of the buffer, the pixel and data rates, the CD clock the mode
 * needs (half the pixel rate, on Tiger Lake the pixel rate itself, and
 * the memory read bandwidth's), the step that gives it and its voltage
 * level with the port's own need.  The mode is refused when its
 * watermarks do not fit, when it needs another CD clock or more than the
 * highest, another voltage level, or more bandwidth than the allowed QGV
 * point.  Returns the refusal, also kept in plan->refused.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_cdclk.c
 * (intel_crtc_compute_min_cdclk, intel_pixel_rate_to_cdclk,
 * intel_planes_min_cdclk, bxt_calc_cdclk, bxt_modeset_calc_cdclk,
 * intel_cdclk_changed), skl_universal_plane.c (icl_plane_min_cdclk),
 * intel_bw.c (intel_bw_crtc_data_rate, intel_bw_crtc_min_cdclk,
 * intel_bw_check_qgv_points), skl_watermark.c (skl_compute_ddb,
 * skl_compute_wm).
 */
unsigned
drv_i915_xd_screen_plan_check(
	const struct i915_xd_plan_input *input,
	struct i915_xd_plan *plan)
{
	const struct i915_xd_platform *platform;
	uint32_t data_kbps;
	uint32_t need;
	uint32_t clamp;
	uint32_t vco_khz;
	uint8_t level;
	int error;

	/* A plan already refused is not checked. */
	if (plan->refused != I915_XD_PLAN_OK)
		return plan->refused;

	/* The display buffer's slices for the pipes that run together. */
	platform = input->platform;
	plan->joined = drv_i915_xd_dbuf_joined(platform, input->run_pipes);
	plan->slices = drv_i915_xd_dbuf_slices(platform, input->pipe, input->run_pipes, plan->joined);

	/* The pixel rate (no scaling) and the plane's data rate. */
	plan->pixel_khz = (uint32_t)input->mode->clock_khz;
	data_kbps = plan->pixel_khz * input->cpp;
	plan->data_mbps = i915_xd_screen_round_up(data_kbps, 1000U);

	/* The plane's watermarks and the pipe's share of the display buffer. */
	error = i915_xd_screen_plan_wm(input, plan);
	if (error != 0) {
		plan->wm_error = error;
		plan->refused = I915_XD_PLAN_WM;
		return plan->refused;
	}

	/*
	 * The pipe needs one CD clock for two pixels; a platform that can
	 * underrun at half the pixel rate needs the pixel rate itself, but
	 * no more than the highest step.
	 */
	need = i915_xd_screen_round_up(plan->pixel_khz, 2U);
	if ((platform->wa & I915_XD_WA_CDCLK_PIXEL_RATE) != 0U) {
		clamp = plan->pixel_khz;
		if (clamp > input->cdclk_max_khz)
			clamp = input->cdclk_max_khz;
		if (clamp > need)
			need = clamp;
	}

	/* The CD clock the pipe needs. */
	plan->crtc_min_cdclk_khz = need;

	/* The memory read bandwidth's need, from display 12 on. */
	plan->bw_min_cdclk_khz = 0U;
	if (platform->display_ver >= 12U)
		plan->bw_min_cdclk_khz = i915_xd_screen_round_up((uint64_t)data_kbps * I915_XD_SCREEN_BW_CDCLK_MUL, I915_XD_SCREEN_BW_CDCLK_DIV);
	if (plan->bw_min_cdclk_khz > need)
		need = plan->bw_min_cdclk_khz;
	plan->min_cdclk_khz = need;

	/* No step above the highest. */
	if (need > input->cdclk_max_khz) {
		plan->refused = I915_XD_PLAN_CDCLK;
		return plan->refused;
	}

	/* The step that gives it and its VCO. */
	plan->cdclk_khz = drv_i915_xd_cdclk_lowest(platform, input->cdclk.ref_khz, need);
	vco_khz = 0U;
	if (plan->cdclk_khz != 0U)
		vco_khz = drv_i915_xd_cdclk_vco(platform, &input->cdclk, plan->cdclk_khz);
	if (vco_khz == 0U) {
		plan->cdclk_khz = 0U;
		plan->refused = I915_XD_PLAN_CDCLK;
		return plan->refused;
	}

	/* The VCO of the chosen step. */
	plan->vco_khz = vco_khz;

	/* Its voltage level with the port's need. */
	plan->min_voltage = 0U;
	if (plan->rate_khz > I915_XD_SCREEN_HIGH_PORT_KHZ)
		plan->min_voltage = I915_XD_SCREEN_HIGH_PORT_LEVEL;
	level = drv_i915_xd_cdclk_voltage(platform, plan->cdclk_khz);
	if (level < plan->min_voltage)
		level = plan->min_voltage;
	plan->voltage = level;

	/* The CD clock is not changed by a mode set: it must be the one running. */
	if (plan->cdclk_khz != input->cdclk.cdclk_khz || plan->vco_khz != input->cdclk.vco_khz) {
		plan->refused = I915_XD_PLAN_CDCLK;
		return plan->refused;
	}

	/* Refuses a voltage level that differs from the current one. */
	if (plan->voltage != input->cdclk.voltage) {
		plan->refused = I915_XD_PLAN_VOLTAGE;
		return plan->refused;
	}

	/* The allowed QGV point must carry the plane. */
	if (input->qgv_mbps == 0U || plan->data_mbps > input->qgv_mbps) {
		plan->refused = I915_XD_PLAN_QGV;
		return plan->refused;
	}

	/* Succeeded: the mode needs nothing the display does not give. */
	return I915_XD_PLAN_OK;
}

/*
 * Computes a whole plan: its link, then its check (see the two parts).
 * Returns the refusal, also kept in plan->refused.
 */
unsigned
drv_i915_xd_screen_plan(
	const struct i915_xd_plan_input *input,
	struct i915_xd_plan *plan)
{
	unsigned refused;

	/* The link, then the check. */
	refused = drv_i915_xd_screen_plan_link(input, plan);
	if (refused != I915_XD_PLAN_OK)
		return refused;

	/* The check. */
	refused = drv_i915_xd_screen_plan_check(input, plan);
	if (refused != I915_XD_PLAN_OK)
		return refused;

	/* Succeeded: the plan holds. */
	return I915_XD_PLAN_OK;
}

/*
 * Names a plan's refusal for the log.
 */
const char *
drv_i915_xd_screen_refusal_name(
	unsigned refused)
{
	/* One name per refusal. */
	switch (refused) {
	case I915_XD_PLAN_OK:
		return "none";
	case I915_XD_PLAN_SINK:
		return "a sink the plan does not drive";
	case I915_XD_PLAN_LINK:
		return "no link carries the mode";
	case I915_XD_PLAN_PLL:
		return "no PLL words for the link rate";
	case I915_XD_PLAN_CDCLK:
		return "another CD clock";
	case I915_XD_PLAN_VOLTAGE:
		return "another voltage level";
	case I915_XD_PLAN_QGV:
		return "more memory bandwidth than the QGV point";
	case I915_XD_PLAN_WM:
		return "watermarks the blank or the display buffer cannot hold";
	default:
		break;
	}

	/* Succeeded: a value no refusal has. */
	return "unknown";
}

/*
 * Runs an update that must land in one frame.  Outside an empty window
 * it takes a vertical blank reference and, with interrupts off, reads
 * the scan line; while the line is inside the window it sleeps (with
 * interrupts on) until the next blank, giving up after 1 ms; then the
 * reference goes back.  The arming writes follow with interrupts still
 * off, then the end's line and frame are read, the completion event is
 * armed (with its own reference) and interrupts go back on.  An update
 * whose frame counter moved between its start and its end crossed a
 * blank and is reported.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_crtc.c
 * (intel_pipe_update_start, intel_pipe_update_end).
 */
void
drv_i915_xd_screen_update(
	struct i915_xd_io *io,
	const struct i915_xd_update *update,
	const struct i915_xd_update_hooks *hooks,
	struct i915_xd_update_report *report)
{
	long ticks;
	unsigned line;
	int waited;
	int error;

	/* Nothing seen yet. */
	kern_memset(report, 0, sizeof(*report));
	line = 0U;
	waited = 0;

	/* A window that does not lie in the frame is not waited out: interrupts go off for the writes. */
	if (update->window.min <= 0 || update->window.max <= 0) {
		hooks->irq_off(hooks->context);
	} else {
		/* The sleeps need the blank's interrupt; without it the writes go now. */
		error = hooks->vblank_get(hooks->context);
		if (error != 0) {
			hooks->error(hooks->context, "the update could not take the vertical blank interrupt\n");
			hooks->irq_off(hooks->context);
		} else {
			/* Interrupts off, then the scan line until it is outside the window. */
			hooks->irq_off(hooks->context);
			ticks = (long)kern_ms_to_ticks(I915_XD_SCREEN_EVADE_MS) + 1L;
			for (;;) {
				/* Outside the window the writes may go. */
				line = drv_i915_xd_pipe_scanline(io, update->pipe, update->vtotal, update->scanline_offset);
				if ((int)line < update->window.min || (int)line > update->window.max)
					break;

				/* The time is spent: the writes go anyway. */
				if (ticks == 0L) {
					report->gave_up = 1;
					hooks->error(hooks->context, "the update did not get out of the lines before the vertical blank in time\n");
					break;
				}

				/* Sleeps until the next blank with interrupts on. */
				hooks->irq_on(hooks->context);
				ticks = hooks->vblank_sleep(hooks->context, ticks);
				report->sleeps++;
				hooks->irq_off(hooks->context);
			}

			/* The sleeps are over: the reference goes back; where the writes start. */
			hooks->vblank_put(hooks->context);
			report->start_line = line;
			report->start_frame = drv_i915_xd_pipe_frame(io, update->pipe);
			waited = 1;
		}
	}

	/* The arming writes, then where they ended. */
	hooks->arm(hooks->context);
	report->end_line = drv_i915_xd_pipe_scanline(io, update->pipe, update->vtotal, update->scanline_offset);
	report->end_frame = drv_i915_xd_pipe_frame(io, update->pipe);

	/* The completion event for the next blank holds a reference of its own. */
	if (hooks->arm_event != NULL) {
		error = hooks->vblank_get(hooks->context);
		if (error != 0)
			hooks->error(hooks->context, "the update's event could not take the vertical blank interrupt\n");
		hooks->arm_event(hooks->context);
	}

	/* Interrupts back on. */
	hooks->irq_on(hooks->context);

	/* Reports writes that may have landed in two frames. */
	if (waited &&
	    report->start_frame != 0U &&
	    report->start_frame != report->end_frame) {
		report->crossed = 1;
		hooks->error(hooks->context, "the update crossed a vertical blank\n");
	}
}

/*
 * Lights a screen: runs the light's steps in order.  After a failed step
 * the rest are skipped but the settle; with undo set, the steps that ran
 * are undone first by the dark's steps from the last one's undo point up
 * to its confirmation (their own failures do not stop the undo; the
 * light's settle gives the power back).  Returns 0, or the error of
 * the first failed step (screen->failed names it).
 */
int
drv_i915_xd_screen_light(
	struct i915_xd_screen *screen,
	struct i915_xd_io *io,
	const struct i915_xd_screen_hooks *hooks,
	int undo)
{
	const struct i915_xd_screen_step *step;
	unsigned undo_from;
	unsigned index;
	int error;

	/* Nothing ran yet. */
	screen->ran = 0U;
	screen->failed = -1;
	screen->error = 0;
	undo_from = I915_XD_UNDO_NONE;

	/* Each step in order; after a failure only the steps that always run. */
	for (index = 0U; index < I915_XD_SCREEN_LIGHT_STEPS; index++) {
		step = &i915_xd_screen_light_steps[index];
		if (screen->failed >= 0 && (step->flags & I915_XD_STEP_ALWAYS) == 0U)
			continue;

		/* Runs the step and counts it; the first failure is kept. */
		error = i915_xd_screen_step_run(io, hooks, step);
		screen->ran++;
		if (error != 0 && screen->failed < 0) {
			screen->failed = (int)index;
			screen->error = error;

			/* The steps that ran are undone before the settle. */
			if (undo && undo_from != I915_XD_UNDO_NONE) {
				for (; undo_from < I915_XD_SCREEN_UNDO_END; undo_from++)
					(void)i915_xd_screen_step_run(io, hooks, &i915_xd_screen_dark_steps[undo_from]);
				screen->phase = I915_XD_SCREEN_OFF;
			}

			continue;
		}

		/* The phase the step reached, and where it is undone from. */
		if (error == 0 && screen->failed < 0) {
			screen->phase = step->phase;
			undo_from = step->undo_from;
		}
	}

	/* Reports a failed step. */
	if (screen->failed >= 0)
		return screen->error;

	/* Succeeded: the screen is lit. */
	return 0;
}

/*
 * Darkens a screen: runs the dark's steps in order.  A failed step
 * skips the rest; a stop that is not confirmed (any failure before the
 * settle) leaves the screen retained, with nothing given back.  Returns
 * 0, or the error of the failed step.
 */
int
drv_i915_xd_screen_dark(
	struct i915_xd_screen *screen,
	struct i915_xd_io *io,
	const struct i915_xd_screen_hooks *hooks)
{
	const struct i915_xd_screen_step *step;
	unsigned index;
	int error;

	/* Nothing ran yet. */
	screen->ran = 0U;
	screen->failed = -1;
	screen->error = 0;

	/* Each step in order, up to the first that fails. */
	for (index = 0U; index < I915_XD_SCREEN_DARK_STEPS; index++) {
		step = &i915_xd_screen_dark_steps[index];
		error = i915_xd_screen_step_run(io, hooks, step);
		screen->ran++;
		if (error != 0) {
			screen->failed = (int)index;
			screen->error = error;
			break;
		}

		/* The step's phase is reached. */
		screen->phase = step->phase;
	}

	/* A stop not confirmed: retained. */
	if (screen->failed >= 0) {
		screen->phase = I915_XD_SCREEN_RETAINED;
		return screen->error;
	}

	/* Succeeded: the screen is off. */
	return 0;
}

/*
 * Tells why a flip to a surface is refused (I915_XD_FLIP_OK when it is
 * not): a screen that is not lit or is retained, a flip still armed or
 * stuck, the buffer already displayed, a surface not on a 4 KiB page.
 */
unsigned
drv_i915_xd_flip_refused(
	const struct i915_xd_flip *flip,
	int lit,
	int retained,
	uint32_t surface)
{
	/* The screen. */
	if (!lit)
		return I915_XD_FLIP_NOT_LIT;
	if (retained)
		return I915_XD_FLIP_RETAINED;

	/* The flips. */
	if (flip->armed)
		return I915_XD_FLIP_BUSY;
	if (flip->stuck)
		return I915_XD_FLIP_STUCK;

	/* The buffer. */
	if (surface == flip->current)
		return I915_XD_FLIP_SAME;
	if ((surface & 0xfffU) != 0U)
		return I915_XD_FLIP_UNALIGNED;

	/* Succeeded: the flip may be armed. */
	return I915_XD_FLIP_OK;
}

/*
 * Records a flip armed to a surface: from here both buffers may be read
 * by the display until the flip is known to have latched.
 */
void
drv_i915_xd_flip_arm(
	struct i915_xd_flip *flip,
	uint32_t surface)
{
	/* The buffers (the caller counted the flip's generation when it passed the refusals). */
	flip->old = flip->current;
	flip->pending = surface;
	flip->armed = 1;
}

/*
 * Judges a flip whose event was waited for (event_error 0 when it came):
 * done when the live surface is the new buffer (the new buffer is the
 * current one, the old is free), otherwise stuck (not latched, or no
 * event).  Returns I915_XD_FLIP_DONE, _NOT_LATCHED or _TIMEOUT.
 */
unsigned
drv_i915_xd_flip_judge(
	struct i915_xd_flip *flip,
	int event_error,
	uint32_t live)
{
	/* No event: the completion may still come; both buffers stay. */
	if (event_error != 0) {
		flip->stuck = 1;
		return I915_XD_FLIP_TIMEOUT;
	}

	/* The event came without the new buffer. */
	if (live != flip->pending) {
		flip->stuck = 1;
		return I915_XD_FLIP_NOT_LATCHED;
	}

	/* Succeeded: the new buffer is displayed. */
	flip->current = flip->pending;
	flip->armed = 0;
	return I915_XD_FLIP_DONE;
}

/*
 * Tells whether no flip is pending, retiring an armed flip the pipe has
 * latched (the live surface is the new buffer).  Returns 1 when none is
 * pending (a stuck one included), 0 while the armed flip waits for its
 * vertical blank.
 */
int
drv_i915_xd_flip_idle(
	struct i915_xd_flip *flip,
	uint32_t live)
{
	/* Nothing armed, or a flip that will never complete. */
	if (!flip->armed || flip->stuck)
		return 1;

	/* The pipe still scans the old buffer. */
	if (live != flip->pending)
		return 0;

	/* Succeeded: the flip latched; the new buffer is the current one. */
	flip->current = flip->pending;
	flip->armed = 0;
	return 1;
}

/*
 * Judges a flip waited for before the buffers change hands: 0 when it
 * latched (the new buffer is the current one), EIO when the event did
 * not come or the surface did not latch (stuck).
 */
int
drv_i915_xd_flip_settled(
	struct i915_xd_flip *flip,
	int event_error,
	uint32_t live)
{
	/* An event that did not come, or a surface that did not latch. */
	if (event_error != 0 || live != flip->pending) {
		flip->stuck = 1;
		return EIO;
	}

	/* Succeeded: no flip is pending. */
	flip->current = flip->pending;
	flip->armed = 0;
	return 0;
}

/*
 * Gives a resident run's verdict: why it ended (served, unplugged, asked
 * to light again, failed) and whether the display had the buffers.
 * Returns 0 for a run that came up, stopped and gave everything back;
 * EAGAIN for an external DisplayPort link lowered after a clean failure
 * (the caller runs again); EIO otherwise.
 */
int
drv_i915_xd_screen_run_verdict(
	const struct i915_xd_run_outcome *outcome,
	struct i915_xd_run_result *result)
{
	int clean;

	/* Why it ended, and the buffers. */
	result->acquired = outcome->acquired;
	result->reason = I915_XD_RUN_FAILED;
	if (outcome->unplugged) {
		result->reason = I915_XD_RUN_UNPLUGGED;
	} else if (outcome->relight) {
		result->reason = I915_XD_RUN_RELIGHT;
	} else if (outcome->passed) {
		result->reason = I915_XD_RUN_SERVED;
	}

	/* A run that gave everything back. */
	clean = outcome->released && outcome->held == 0;

	/* A link lowered after a clean failure: run again. */
	if (!outcome->passed &&
	    clean &&
	    outcome->lowered)
		return EAGAIN;

	/* A run that did not pass, did not give everything back or still holds power. */
	if (!outcome->passed || !clean)
		return EIO;

	/* Succeeded: the run came up, stopped and was released. */
	return 0;
}

/*
 * The panel's link: the sink's highest rate and lanes (an eDP panel older
 * than 1.4 is trained at its maximum), the bits per pixel of its EDID
 * depth capped by the VBT; refused when the link cannot carry the mode.
 */
static unsigned
i915_xd_screen_panel_link(
	const struct i915_xd_plan_input *input,
	struct i915_xd_plan *plan)
{
	const struct i915_display_mode *mode;

	/* The sink's rate and lanes: one of the four 8b/10b codes, one, two or four lanes. */
	mode = input->mode;
	plan->rate_khz = drv_i915_xd_dp_code_rate(input->sink.rate_code);
	plan->lanes = input->sink.max_lanes;
	switch (input->sink.rate_code) {
	case I915_XD_DPCD_BW_RBR:
	case I915_XD_DPCD_BW_HBR:
	case I915_XD_DPCD_BW_HBR2:
	case I915_XD_DPCD_BW_HBR3:
		break;
	default:
		return I915_XD_PLAN_SINK;
	}

	/* Refuses a lane count the link cannot use. */
	if (plan->lanes != 1U &&
	    plan->lanes != 2U &&
	    plan->lanes != 4U)
		return I915_XD_PLAN_SINK;

	/* An eDP 1.4 panel lists its own rates, which the plan does not take. */
	if (input->sink.edp_revision >= I915_XD_SCREEN_EDP_14)
		return I915_XD_PLAN_SINK;

	/* The bits per pixel: the EDID's depth (24 when it names none), capped by the VBT. */
	plan->bpp = I915_XD_SCREEN_DEFAULT_BPP;
	if (mode->edid_bpc > 0)
		plan->bpp = 3U * (unsigned)mode->edid_bpc;
	if (input->sink.vbt_bpp != 0U && input->sink.vbt_bpp < plan->bpp)
		plan->bpp = input->sink.vbt_bpp;

	/* What the mode needs, and what the link carries. */
	plan->need_kbps = drv_i915_xd_dp_need_kbps((uint32_t)mode->clock_khz, plan->bpp);
	plan->carry_kbps = drv_i915_xd_dp_carry_kbps(plan->rate_khz, plan->lanes);
	if (plan->need_kbps > plan->carry_kbps)
		return I915_XD_PLAN_LINK;

	/* Succeeded: the link carries the mode. */
	return I915_XD_PLAN_OK;
}

/* An HDMI or DVI sink's link: four lanes at the TMDS clock of 8 bits per colour (the pixel clock). */
static unsigned
i915_xd_screen_hdmi_link(
	const struct i915_xd_plan_input *input,
	struct i915_xd_plan *plan)
{
	/* The TMDS clock is the pixel clock. */
	plan->rate_khz = (uint32_t)input->mode->clock_khz;
	plan->lanes = 4U;
	plan->bpp = I915_XD_SCREEN_DEFAULT_BPP;

	/* Succeeded: the link. */
	return I915_XD_PLAN_OK;
}

/*
 * An external DisplayPort sink's link: within a branch's clock limits,
 * the first depth (from the EDID's, at most 8 and a branch's cap, down to
 * 6), rate (lowest first) and lane count that carry the mode.
 */
static unsigned
i915_xd_screen_dp_link(
	const struct i915_xd_plan_input *input,
	struct i915_xd_plan *plan)
{
	const struct i915_xd_sink *sink;
	struct i915_xd_dp_choice choice;
	uint32_t clock;
	unsigned max_bpc;
	int error;
	unsigned max_lanes;

	/* A sink that shares no rate or lane. */
	sink = &input->sink;
	if (sink->nrates == 0U || sink->max_lanes == 0U)
		return I915_XD_PLAN_SINK;

	/* A branch's clock limits. */
	clock = (uint32_t)input->mode->clock_khz;
	if (sink->max_dotclock_khz != 0U && clock > sink->max_dotclock_khz)
		return I915_XD_PLAN_LINK;
	if (sink->min_tmds_khz != 0U && clock < sink->min_tmds_khz)
		return I915_XD_PLAN_LINK;
	if (sink->max_tmds_khz != 0U && clock > sink->max_tmds_khz)
		return I915_XD_PLAN_LINK;

	/* The deepest colour: the EDID's (8 when it names none), at most 8 and a branch's cap. */
	max_bpc = I915_XD_SCREEN_DP_MAX_BPC;
	if (input->mode->edid_bpc > 0 && (unsigned)input->mode->edid_bpc < max_bpc)
		max_bpc = (unsigned)input->mode->edid_bpc;
	if (sink->max_bpc > 0U && sink->max_bpc < max_bpc)
		max_bpc = sink->max_bpc;

	/* The most lanes: the sink's, or a lower limit put on them. */
	max_lanes = sink->max_lanes;
	if (sink->lane_limit != 0U && sink->lane_limit < sink->max_lanes)
		max_lanes = sink->lane_limit;

	/* The first depth, rate and lanes that carry the mode. */
	error = drv_i915_xd_dp_choose(
		clock,
		max_bpc,
		I915_XD_SCREEN_DP_MIN_BPC,
		sink->rates,
		sink->nrates,
		sink->rate_limit_khz,
		max_lanes,
		&choice);
	if (error != 0)
		return I915_XD_PLAN_LINK;

	/* The link the choice names. */
	plan->rate_khz = choice.rate_khz;
	plan->lanes = choice.lanes;
	plan->bpp = choice.bpp;
	plan->need_kbps = choice.need_kbps;
	plan->carry_kbps = choice.carry_kbps;

	/* Succeeded: the link carries the mode. */
	return I915_XD_PLAN_OK;
}

/* The combo DPLL's words of the link, when the port's PHY is a combo PHY; a Type-C port's PLL is not planned here. */
static unsigned
i915_xd_screen_combo_pll(
	const struct i915_xd_plan_input *input,
	struct i915_xd_plan *plan)
{
	const struct i915_xd_port_desc *port;
	int error;

	/* A port without a combo PHY runs on a Type-C or Thunderbolt PLL. */
	port = drv_i915_xd_platform_port(input->platform, input->ddi);
	if (port == NULL || port->phy_kind != I915_XD_PHY_COMBO)
		return I915_XD_PLAN_OK;

	/* The DisplayPort table's words, or the HDMI dividers'. */
	if (input->kind == I915_XD_SCREEN_HDMI) {
		error = drv_i915_xd_dpll_hdmi_words(input->platform, input->ref_khz, plan->rate_khz, I915_XD_DPLL_AFC_NONE, &plan->pll);
	} else {
		error = drv_i915_xd_dpll_dp_words(input->platform, input->ref_khz, plan->rate_khz, I915_XD_DPLL_AFC_NONE, &plan->pll);
	}

	/* Refuses a rate the PLL cannot make. */
	if (error != 0)
		return I915_XD_PLAN_PLL;

	/* Succeeded: the words. */
	plan->pll_set = 1;
	return I915_XD_PLAN_OK;
}

/*
 * Computes the plane's watermarks and the pipe's share of the display
 * buffer: the levels from the memory's latencies, the deeper levels the
 * vertical blank cannot hold turned off, the pipe's weight among the
 * pipes on its slices and its range, then the cursor's fixed part and the
 * plane's allocation by the size of its buffer.  Returns 0, or EINVAL
 * when the memory gives no levels or when no level fits the blank or the
 * buffer.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/skl_watermark.c
 * (skl_build_pipe_wm, skl_wm_check_vblank, skl_compute_ddb,
 * skl_crtc_allocate_ddb, intel_crtc_dbuf_weights, intel_crtc_ddb_weight,
 * skl_crtc_allocate_plane_ddb), intel_atomic_plane.c
 * (intel_plane_relative_data_rate).
 */
static int
i915_xd_screen_plan_wm(
	const struct i915_xd_plan_input *input,
	struct i915_xd_plan *plan)
{
	const struct i915_xd_platform *platform;
	struct i915_xd_wm_pipe pipe;
	struct i915_xd_wm_surface surface;
	uint8_t slices[4];
	uint16_t weight;
	unsigned weight_start;
	unsigned weight_end;
	unsigned weight_total;
	unsigned deepest;
	unsigned index;
	uint16_t cursor_blocks;
	uint64_t relative_rate;
	int sagv_too_short;
	int error;

	/* A memory that names no levels gives the plane no watermark. */
	platform = input->platform;
	if (input->wm.levels == 0U || input->wm.levels > I915_XD_WM_LEVELS)
		return EINVAL;

	/* The pipe at the mode's pixel rate and line total. */
	plan->wm_levels = input->wm.levels;
	pipe = input->wm;
	pipe.pixel_rate_khz = plan->pixel_khz;
	pipe.htotal = input->mode->htotal;

	/* The plane: the whole buffer, linear, unscaled, at the pipe's rate. */
	kern_memset(&surface, 0, sizeof(surface));
	surface.width = input->fb_width;
	surface.cpp = input->cpp;
	surface.layout = I915_XD_WM_LINEAR;
	surface.pixel_rate_khz = plan->pixel_khz;
	drv_i915_xd_wm_plane(platform, &pipe, &surface, &plan->wm);

	/* The deepest level the vertical blank holds. */
	error = drv_i915_xd_wm_vblank_level(&pipe, plan->pixel_khz, (uint32_t)(input->mode->vtotal - input->mode->vdisplay),
		input->frame_start_delay, plan->wm.level[0].lines, &deepest, &sagv_too_short);
	if (error != 0)
		return error;

	/* The deeper levels go off; before display 12 the SAGV's block time does not count. */
	plan->wm_cut = 0;
	if (deepest + 1U < pipe.levels)
		plan->wm_cut = 1;
	if (platform->display_ver < 12U)
		sagv_too_short = 0;
	drv_i915_xd_wm_cut(&plan->wm, pipe.levels, deepest, sagv_too_short);

	/*
	 * The pipe's weight is the width of its mode; it shares its slices
	 * with the pipes that use the same ones, before it the pipes of lower
	 * numbers.
	 */
	plan->weight = input->mode->hdisplay;
	weight_start = 0U;
	weight_end = 0U;
	weight_total = 0U;
	for (index = 0U; index < 4U; index++) {
		/* This pipe's own entries are the plan's; the others are what runs now. */
		slices[index] = input->pipe_slices[index];
		weight = input->pipe_weight[index];
		if (index == input->pipe) {
			slices[index] = plan->slices;
			weight = plan->weight;
		}

		/* A pipe on other slices does not share them. */
		if (slices[index] != plan->slices)
			continue;

		/* Adds the pipe's weight to the share before, up to and around this one. */
		weight_total += weight;
		if (index < input->pipe) {
			weight_start += weight;
			weight_end += weight;
		} else if (index == input->pipe) {
			weight_end += weight;
		}
	}

	/* The slices all the pipes use, and the pipe's range of its slices. */
	plan->enabled_slices = drv_i915_xd_dbuf_enabled(slices, 4U);
	drv_i915_xd_dbuf_pipe_range(platform, plan->slices, weight_start, weight_end, weight_total, &plan->pipe_ddb);

	/* An empty range hands nothing out. */
	if (plan->pipe_ddb.end == plan->pipe_ddb.start)
		return 0;

	/* The cursor's fixed part for the pipes that run, then the plane's by the size of its buffer. */
	cursor_blocks = drv_i915_xd_wm_cursor_blocks(platform, &pipe, (unsigned)__builtin_popcount(input->run_pipes));
	relative_rate = (uint64_t)input->fb_width * input->fb_height * input->cpp;
	error = drv_i915_xd_wm_allocate(&pipe, &plan->pipe_ddb, cursor_blocks, &plan->wm, relative_rate, &plan->plane_ddb, &plan->cursor_ddb);
	if (error != 0)
		return error;

	/* Succeeded: the plane's watermarks fit its part of the buffer. */
	return 0;
}

/* Divides, rounding up. */
static uint32_t
i915_xd_screen_round_up(
	uint64_t value,
	uint32_t divisor)
{
	/* The quotient, one more when a remainder is left. */
	return (uint32_t)((value + divisor - 1U) / divisor);
}

/* Returns the caller's hook of a step (NULL when the caller has none). */
static int
(*i915_xd_screen_hook(
	const struct i915_xd_screen_hooks *hooks,
	unsigned hook))(void *context)
{
	/* One hook per step. */
	switch (hook) {
	case I915_XD_STEP_POWER:
		return hooks->power;
	case I915_XD_STEP_DBUF:
		return hooks->dbuf;
	case I915_XD_STEP_SETUP:
		return hooks->setup;
	case I915_XD_STEP_DMC_ON:
		return hooks->dmc_on;
	case I915_XD_STEP_LINK_POWER:
		return hooks->link_power;
	case I915_XD_STEP_PLL:
		return hooks->pll;
	case I915_XD_STEP_PORT:
		return hooks->port;
	case I915_XD_STEP_PIPE:
		return hooks->pipe;
	case I915_XD_STEP_OUTPUT:
		return hooks->output;
	case I915_XD_STEP_CHECK:
		return hooks->check;
	case I915_XD_STEP_PLANE:
		return hooks->plane;
	case I915_XD_STEP_SETTLE:
		return hooks->settle;
	case I915_XD_STEP_DARK_POWER:
		return hooks->dark_power;
	case I915_XD_STEP_PLANE_OFF:
		return hooks->plane_off;
	case I915_XD_STEP_OUTPUT_OFF:
		return hooks->output_off;
	case I915_XD_STEP_PORT_OFF:
		return hooks->port_off;
	case I915_XD_STEP_PLL_OFF:
		return hooks->pll_off;
	case I915_XD_STEP_LINK_POWER_OFF:
		return hooks->link_power_off;
	case I915_XD_STEP_DMC_OFF:
		return hooks->dmc_off;
	case I915_XD_STEP_CONFIRM:
		return hooks->confirm;
	case I915_XD_STEP_DARK_SETTLE:
		return hooks->dark_settle;
	default:
		break;
	}

	/* Succeeded: no such step. */
	return NULL;
}

/* Runs one step: its name in the trace, then the caller's hook (a missing hook does nothing). */
static int
i915_xd_screen_step_run(
	struct i915_xd_io *io,
	const struct i915_xd_screen_hooks *hooks,
	const struct i915_xd_screen_step *step)
{
	int (*hook)(void *context);
	int error;

	/* The step's name in the trace. */
	drv_i915_xd_io_step(io, step->name);

	/* The caller's part. */
	hook = i915_xd_screen_hook(hooks, step->hook);
	if (hook == NULL)
		return 0;
	error = hook(hooks->context);
	if (error != 0)
		return error;

	/* Succeeded: the step ran. */
	return 0;
}
