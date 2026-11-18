/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The runs of the resident node's outputs.
 *
 * The resident run puts the resident output's picture on screen 0: two
 * full-size buffers, one light, flips driven by presentations while the
 * request worker serves, and the dark when the display is given back.
 * The second output's run lights screen 1 from the same hooks, preflight
 * and configuration.  Three records of a run are kept apart: the first
 * anomaly, what the light left, and what the dark returned; "the call
 * returned" is never reported as "it worked".  A run whose stop was not
 * confirmed keeps its buffers and what the screen holds for ever, and the
 * device's teardown asks this file before it releases anything the
 * display might still read.
 */

#include "internal.h"
#include "resident.h"
#include "run-log.h"
#include "xd-dp.h"
#include "dp-ext-kern.h"
#include "edp-device.h"
#include "head.h"
#include "output.h"
#include "display-power.h"
#include "present.h"
#include "scanout.h"
#include "tc-port.h"
#include "interrupts.h"
#include "vbt-parse.h"
#include "clock.h"
#include <kern/kcrt.h>

#include "../intel/display-regs-xd.h"
#include "../i915.h"
#include "../ggtt.h"
#include "../memory.h"
#include "../mmio.h"
#include "../sync.h"

#include <kern/irq.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/lock.h>

#include <uapi/errno.h>
#include <stddef.h>

/* The DDI_BUF_CTL bits a port keeps from the firmware: the port reversal and the four-lane bit of DDI A. */
#define I915_RESIDENT_SAVED_PORT_BITS	((1U << 16) | (1U << 4))

/* How long a run waits for the first frames, and how many it must see. */
#define I915_RESIDENT_FIRST_FRAMES_MS	1000U
#define I915_RESIDENT_FIRST_FRAMES	10U

/* The frames each steady round of the window must see within 1.5 s, and the step between rounds. */
#define I915_RESIDENT_STEADY_FRAMES	30U
#define I915_RESIDENT_STEADY_ROUND_MS	1500U
#define I915_RESIDENT_STEADY_STEP_MS	500U

/* How long the stop confirmation watches the frame counter. */
#define I915_RESIDENT_STOP_WATCH_MS	200U

/* The display buffer of Tiger Lake (two slices of 1024 blocks) and of display 13 (four slices of 1024). */
#define I915_RESIDENT_DBUF_TGL_SLICES	0x03U
#define I915_RESIDENT_DBUF_TGL_BLOCKS	2048U
#define I915_RESIDENT_DBUF_D13_BLOCKS	4096U
#define I915_RESIDENT_DBUF_D13_SLICES	0x0fU

/* The south display's interrupt status and the two combo PLLs' enables, logged by the HDMI preflight. */
#define I915_RESIDENT_SDE_ISR		0xc4000U
#define I915_RESIDENT_DPLL0_ENABLE	0x46010U
#define I915_RESIDENT_DPLL1_ENABLE	0x46014U

/* The enable bit of a transcoder, a plane, a PLL and a DDI buffer, and the transcoder's enable and state bits. */
#define I915_RESIDENT_ENABLE_BIT	0x80000000U
#define I915_RESIDENT_TRANS_ON_MASK	0xc0000000U

/* The pipe A interrupt bits that must stay masked: vblank and underrun, plus display 13's soft and hard underrun. */
#define I915_RESIDENT_KEEP_MASKED_D13	0x80600001U
#define I915_RESIDENT_KEEP_MASKED_D12	0x80000001U

/*
 * The negative errors the run's hooks answer, in the numbering the
 * backend's callers compare: an invalid request, a wait that ran out, a
 * broken time base.
 */
#define I915_RESIDENT_EINVAL		(-22)
#define I915_RESIDENT_ETIMEDOUT		(-110)
#define I915_RESIDENT_EIO		(-5)

/* The scan line register of pipe A the update's probe watches, and its line bits. */
#define I915_RESIDENT_PIPE_DSL_A	0x70000U
#define I915_RESIDENT_PIPE_DSL_MASK	0x1fffU

static void resident_anomaly(struct i915_lcd_show_report *r, const char *what, int rc);
static void resident_reached(struct i915_lcd_show_env *env, struct i915_lcd_show_report *r, int stage);
static int resident_first_error_after(const struct i915_lcd_trace *trace, unsigned from);
static int resident_show(struct i915_display *display, struct i915_lcd_show_env *env, struct i915_scanout *so, struct i915_lcd_show_report *r);
static int resident_show_display(struct i915_display *display, struct i915_lcd_show_env *env, struct i915_scanout *so, struct i915_lcd_show_report *r);
static int resident_show_passed(struct i915_display *display, const struct i915_lcd_show_report *r);
static int resident_show_begin(struct i915_display *display, struct i915_lcd_show_env *env, struct i915_lcd_show_report *r);
static void resident_at_stage(void *ctx, int stage);
static void resident_locks_init(struct i915_display *display);
static uint32_t resident_read32(void *ctx, uint32_t reg);
static void resident_write32(void *ctx, uint32_t reg, uint32_t value);
static uint32_t resident_rmw32(void *ctx, uint32_t reg, uint32_t clear, uint32_t set);
static void resident_posting_read(void *ctx, uint32_t reg);
static int resident_wait_reg(void *ctx, uint32_t reg, uint32_t mask, uint32_t value, unsigned timeout_ms);
static void resident_usleep(void *ctx, unsigned us);
static void resident_udelay(void *ctx, unsigned us);
static void resident_dbuf_slices_update(void *ctx, unsigned slices);
static void resident_lock(void *ctx, int which, int take);
static void resident_irq_off(void *ctx);
static int resident_power_domain(void *ctx, unsigned use, unsigned index, int thunderbolt);
static int resident_power_number(uint64_t function);
static uint64_t resident_power_functions(int number);
static int resident_power_get(void *ctx, int number);
static int resident_power_get_if_enabled(void *ctx, int number);
static void resident_power_put(void *ctx, int number, int cookie);
static void resident_power_put_async(void *ctx, int number, int cookie, int delay_ms);
static int resident_vblank_get(void *ctx, int pipe);
static void resident_vblank_put(void *ctx, int pipe);
static long resident_vblank_sleep(void *ctx, int pipe, long ticks);
static void resident_arm_event(void *ctx, int pipe);
static int resident_wait_event(void *ctx, int pipe, unsigned timeout_ms);
static void resident_cancel_event(void *ctx, int pipe);
static uint32_t resident_frame(void *ctx);
static void resident_irq_on(void *ctx);
static int resident_preflight_panel(struct i915_lcd_kernel *k);
static int resident_preflight_hdmi(struct i915_lcd_kernel *k);
static int resident_preflight_dp_ext(struct i915_lcd_kernel *k);
static int resident_config_panel(struct i915_lcd_kernel *k, struct i915_xd_picture_config *config);
static void resident_config_hdmi(struct i915_lcd_kernel *k, const struct i915_display_output *output, struct i915_xd_picture_config *config);
static void resident_config_dp_ext(struct i915_lcd_kernel *k, const struct i915_display_output *output, struct i915_xd_picture_config *config);
static void resident_config_link(const struct i915_display_mode *mode, const struct i915_display_link *link, struct i915_xd_picture_config *config);
static int resident_window(void *ctx, struct i915_lcd_observer *o);
static int resident_buffers(struct i915_display *display, const struct i915_lcd_kernel_deps *d, const struct i915_display_mode *mode);
static int resident_buffers_kept(struct i915_display *display);
static int resident_release(struct i915_display *display, const struct i915_lcd_show_report *rep);
static int resident_passed(const struct i915_lcd_show_report *rep);
static int resident_takeover(struct i915_display *display);
static int resident_takeover_power_get(void *context, uint64_t functions);
static void resident_takeover_power_put(void *context, uint64_t functions);
static void resident_log_run(struct i915_display *display, const struct i915_lcd_show_report *rep, int passed, int released, int held);

/*
 * Lights the resident output as the display of the resident node.
 *
 * Two full-size buffers, one light, synchronous flips: buffer A is shown
 * first, and serve runs inside the picture's window (after the first
 * frames were seen) until the display is to be given back; the dark
 * follows and both buffers are released.  The output is the panel, or the
 * HDMI or external DisplayPort display output.c chose in its place.
 * Returns 0 when the output came up and was stopped and released cleanly;
 * EINVAL (a dependency is missing), EBUSY (resources of an earlier run are
 * retained), EAGAIN (an external DP link did not train and the port's link
 * was lowered: the caller runs again) or EIO otherwise.
 */
int
drv_i915_resident_run(
	struct i915_display *display,
	const struct i915_lcd_kernel_deps *d,
	int (*serve)(void *ctx),
	void *ctx)
{
	struct i915_lcd_run_params output_params;
	const struct i915_lcd_run_params *params;
	struct i915_lcd_kernel *k;
	struct i915_lcd_show_env *env;
	struct i915_lcd_show_report *rep;
	struct i915_xd_run_outcome outcome;
	struct i915_xd_run_result run;
	const struct i915_display_mode *mode;
	const char *verdict;
	const char *stop;
	const char *first_anomaly;
	unsigned domain;
	int show_result;
	int released;
	int passed;
	int held;
	int retained;
	int kept;
	int unplugged;
	int buffers_error;
	int preflight_error;
	int config_error;
	int takeover_error;
	int params_error;
	int fallback_error;
	int run_error;

	/* The run, its environment and its report, the display's own. */
	k = &display->lk;
	env = &display->resident_run_env;
	rep = &display->resident_run_rep;

	/* The run needs the panel, the registers, the GT memory, the interrupts and a serve loop. */
	if (d == NULL ||
	    d->edp == NULL ||
	    d->mmio == NULL ||
	    d->gm == NULL ||
	    d->irq == NULL ||
	    serve == NULL) {
		kern_logf("i915: resident display: not started (a dependency is missing)\n");
		return EINVAL;
	}

	/*
	 * An earlier run left resources the display may still read: refused.
	 * Buffers an earlier run kept pinned for this one, when the resident
	 * output is lit again for two pipes, are this run's.
	 */
	retained = drv_i915_resident_retained(display);
	if (!retained)
		retained = drv_i915_xd_picture_retained(&display->xd.pictures, I915_XD_PICTURE_RESIDENT);
	kept = resident_buffers_kept(display);
	if (retained) {
		kern_logf("i915: resident display: refused (resources of an earlier run are retained)\n");
		return EBUSY;
	}

	/* Buffers of an earlier run that were not kept for this one are retained. */
	if (!kept &&
	    (display->resident_buf[0].state != I915_SCANOUT_NONE ||
	     display->resident_buf[1].state != I915_SCANOUT_NONE)) {
		kern_logf("i915: resident display: refused (resources of an earlier run are retained)\n");
		return EBUSY;
	}

	/* The output's port and pipe; the panel has no parameters of its own. */
	params = NULL;
	if (display->output.kind != I915_OUTPUT_KIND_PANEL) {
		params_error = drv_i915_resident_output_params(&display->output, &output_params);
		if (params_error != 0) {
			kern_logf("i915: resident display: not started: this driver does not light the output's kind (%d) yet\n", (int)display->output.kind);
			return EIO;
		}

		/* The run drives the other output. */
		params = &output_params;
	}

	/* Starts the run with its hooks bound over the display's objects. */
	drv_i915_resident_bind(display, k, d, NULL);

	/*
	 * The display a UEFI boot's firmware left lit (the firmware check found
	 * a lit pipe): stopped before this run writes.
	 */
	if (display->xd.firmware.take_over) {
		takeover_error = resident_takeover(display);
		if (takeover_error != 0)
			return EIO;

		/* Taken over once: a later run starts from the driver's own display. */
		display->xd.firmware.take_over = 0;

		/* The start's power reference kept for the takeover goes back now. */
		if (display->dprobe.power_release_deferred) {
			display->dprobe.power_release_deferred = 0;
			drv_i915_display_power_serve(&display->power, &display->dprobe);
			kern_logf("i915: takeover: the start's power reference went back: wells on %u -> %u\n", display->dprobe.wells_on_before, display->dprobe.wells_on_after);
		}
	}

	/*
	 * Another output in the panel's place: the run's parameters name its
	 * port and pipe and, as the only screen, it owns the device's PLLs and
	 * display buffer from their empty start.
	 */
	if (params != NULL) {
		k->p = params;
		drv_i915_xd_pictures_forget(&display->xd.pictures);
	}

	/* The hardware must be as the start left it, and the inputs must be complete. */
	kern_memset(env, 0, sizeof(*env));
	preflight_error = drv_i915_resident_preflight(k);
	config_error = 0;
	if (preflight_error == 0)
		config_error = drv_i915_resident_config(k, &display->output, &env->cfg);
	if (preflight_error != 0 || config_error != 0) {
		kern_logf("i915: resident display: not started (preflight: nothing was written)\n");
		k->p = NULL;
		return EIO;
	}

	/*
	 * The display buffer's share leaves room for a claimed second output's
	 * pipe; without one the output has it to itself.  The window remembers
	 * which pipes the run was lit for.
	 */
	display->window.run_pipes = drv_i915_head_run_pipes(display);
	env->cfg.also_pipes = (uint8_t)display->window.run_pipes;

	/* Kept buffers of another size (the output changed meanwhile) are given back and made again. */
	mode = &env->cfg.mode;
	if (kept &&
	    (display->resident_buf[0].width != (uint32_t)mode->hdisplay ||
	     display->resident_buf[0].height != (uint32_t)mode->vdisplay)) {
		(void)drv_i915_scanout_unpin(&display->resident_buf[1]);
		(void)drv_i915_scanout_destroy(&display->resident_buf[1]);
		(void)drv_i915_scanout_unpin(&display->resident_buf[0]);
		(void)drv_i915_scanout_destroy(&display->resident_buf[0]);
		display->window.keep_buffers = 0;
		kept = 0;
	}

	/* Creates, pins, clears and publishes both buffers at the output's size, or takes the ones kept for this run. */
	if (kept) {
		display->window.keep_buffers = 0;
		kern_logf("i915: resident display: the buffers and their picture are kept for this lighting (pipes besides it 0x%x)\n", display->window.run_pipes);
	} else {
		buffers_error = resident_buffers(display, d, mode);
		if (buffers_error != 0) {
			k->p = NULL;
			return EIO;
		}
	}

	/* Buffer A is shown first; the serve loop runs in the window. */
	display->resident_front = 0U;
	display->resident_serve = serve;
	display->resident_serve_ctx = ctx;
	k->flip_a = &display->resident_buf[0];
	k->flip_b = &display->resident_buf[1];

	/* The show environment: the window is the time serve runs, on the output's pipe. */
	env->hw = &k->ops;
	env->gm = d->gm;
	env->pipe = (int)drv_i915_display_output_pipe(&display->output);
	env->first_frames_ms = I915_RESIDENT_FIRST_FRAMES_MS;
	env->window_ms = 0U;
	env->in_window = resident_window;
	env->in_window_ctx = k;
	env->at_stage = resident_at_stage;
	env->at_stage_ctx = k;
	k->window_ms = 0U;

	/* Shows buffer A, serves in the window, and stops it through the dark. */
	show_result = resident_show(display, env, &display->resident_buf[0], rep);

	/* Counts the power references the run still holds. */
	held = 0;
	for (domain = 0U; domain < I915_LCD_POWER_SLOTS; domain++)
		held += k->power_refs[domain];

	/* Releases both buffers, or abandons them when the display may still read them. */
	released = resident_release(display, rep);

	/* Judges the run on its counters and logs its run log. */
	passed = resident_passed(rep);
	resident_log_run(display, rep, passed, released, held);

	/* Reports how the run ended. */
	verdict = "FAIL";
	if (passed &&
	    released &&
	    held == 0)
		verdict = "PASS";

	/* Whether the display confirmed the stop. */
	stop = "NOT confirmed";
	if (rep->display_released)
		stop = "confirmed";

	/* The first thing that went wrong. */
	first_anomaly = "none";
	if (rep->first_anomaly != NULL)
		first_anomaly = rep->first_anomaly;

	/* Logs how the run ended. */
	kern_logf("i915: resident display: ended %s (show rc=%d; flips %u, stop %s, buffers released=%d, power refs held %d, first anomaly: %s)\n",
	    verdict,
	    show_result,
	    k->flips_done,
	    stop,
	    released,
	    held,
	    first_anomaly);

	/* The serve loop is no longer called, and the run's parameters are gone. */
	display->resident_serve = NULL;
	k->p = NULL;

	/*
	 * An external DP link that did not train, stopped and given back
	 * cleanly: the port's link limits are lowered and the run is to be
	 * tried again at the lower link.
	 */
	unplugged = __atomic_load_n(&display->window.unplugged, __ATOMIC_ACQUIRE);
	kern_memset(&outcome, 0, sizeof(outcome));
	if (!passed &&
	    released &&
	    held == 0 &&
	    display->output.kind == I915_OUTPUT_KIND_DP_EXT) {
		fallback_error = drv_i915_resident_link_fallback(display, &display->output, rep->enable_rc);
		if (fallback_error == 0)
			outcome.lowered = 1;
	}

	/* The screen's verdict on the run. */
	outcome.passed = passed;
	outcome.released = released;
	outcome.held = held;
	outcome.unplugged = unplugged;
	outcome.acquired = rep->display_acquired;
	run_error = drv_i915_xd_screen_run_verdict(&outcome, &run);
	if (run_error != 0)
		return run_error;

	/* Succeeded: the output came up, stopped and was released. */
	return 0;
}

/*
 * Reports whether a run left resources the display or the GPU may still
 * use.
 *
 * The latches are set when a run ends abandoned and never cleared by a
 * later run; every run refuses before it touches anything while one is
 * set, and the teardown asks before releasing the DMA device, the scratch
 * page, the BARs or bus mastering.
 */
int
drv_i915_resident_retained(
	struct i915_display *display)
{
	/* The display was not shown to have stopped. */
	if (display->show_retained)
		return 1;

	/* The GPU was not shown to be done. */
	if (display->show_gpu_retained)
		return 1;

	/* Nothing is retained. */
	return 0;
}

/*
 * Reports whether buffers were kept because a stop was not confirmed: by
 * the resident run, by the resident screen or by the second output.
 * Returns 1 when they were, 0 otherwise.
 */
int
drv_i915_resident_abandoned(
	struct i915_display *display)
{
	int retained;

	/* The resident run kept its buffers and hooks. */
	retained = drv_i915_resident_retained(display);
	if (retained)
		return 1;

	/* The resident screen kept what it holds. */
	retained = drv_i915_xd_picture_retained(&display->xd.pictures, I915_XD_PICTURE_RESIDENT);
	if (retained)
		return 1;

	/* The second output's stop was not confirmed: its pipe may still read its buffers. */
	if (display->head.broken)
		return 1;

	/* Succeeded: nothing was kept. */
	return 0;
}

/*
 * Reports whether the GPU was not shown to be done with a buffer a run
 * kept.
 */
int
drv_i915_resident_gpu_retained(
	struct i915_display *display)
{
	/* Reports the latch. */
	return display->show_gpu_retained;
}

/*
 * Fills the run parameters of an output that is not the built-in panel:
 * the kind, its DDI port and its pipe.  The second output's run and the
 * resident run of that output both use them.  Returns 0, or EOPNOTSUPP
 * for the panel (which has no parameters of its own) or a kind this
 * driver does not light.
 */
int
drv_i915_resident_output_params(
	const struct i915_display_output *output,
	struct i915_lcd_run_params *params)
{
	/* Starts empty. */
	kern_memset(params, 0, sizeof(*params));

	/* Each kind's port and pipe. */
	switch (output->kind) {
	case I915_OUTPUT_KIND_HDMI:
		/* The HDMI port on the output's pipe, in DVI mode. */
		params->output_hdmi = 1;
		params->port = output->port;
		params->pipe = (int)output->pipe;
		break;
	case I915_OUTPUT_KIND_DP_EXT:
		/* The Type-C port's DDI on the output's pipe. */
		params->output_dp_ext = 1;
		params->port = output->port;
		params->pipe = (int)output->pipe;
		break;
	default:
		return EOPNOTSUPP;
	}

	/* Succeeded: the parameters name the output. */
	return 0;
}

/*
 * Binds the hooks of a run: the registers, waits and time here; the
 * panel's sink and panel power through the resident eDP; power, vblank and
 * events, the Type-C links and the Dekel PHYs, and the step, error and
 * debug hooks through their owners.  The run starts empty and belongs to
 * the display; params names the output it drives (NULL for the panel).
 */
void
drv_i915_resident_bind(
	struct i915_display *display,
	struct i915_lcd_kernel *k,
	const struct i915_lcd_kernel_deps *d,
	const struct i915_lcd_run_params *params)
{
	/* The two locks of the runs live as long as the device. */
	resident_locks_init(display);

	/* Starts empty: the run's display, its objects, its locks and its parameters. */
	kern_memset(k, 0, sizeof(*k));
	k->display = display;
	k->d = d;
	k->locks = display->lcdb_locks;
	k->p = params;

	/* Every hook gets the run as its context. */
	k->ops.ctx = k;

	/* Registers, waits and time. */
	k->ops.write32 = resident_write32;
	k->ops.rmw32 = resident_rmw32;
	k->ops.posting_read = resident_posting_read;
	k->ops.read32 = resident_read32;
	k->ops.wait_reg = resident_wait_reg;
	k->ops.usleep = resident_usleep;
	k->ops.udelay = resident_udelay;

	/* The panel's sink and its panel power sequencer. */
	k->ops.dpcd_read = drv_i915_edp_emit_dpcd_read;
	k->ops.dpcd_write = drv_i915_edp_emit_dpcd_write;
	k->ops.read_dpcd_caps = drv_i915_edp_emit_read_dpcd_caps;
	k->ops.panel = drv_i915_edp_emit_panel;

	/* Display power and the display buffer's slices. */
	k->ops.power_domain = resident_power_domain;
	k->ops.power_get = resident_power_get;
	k->ops.power_get_if_enabled = resident_power_get_if_enabled;
	k->ops.power_put = resident_power_put;
	k->ops.power_put_async = resident_power_put_async;
	k->ops.dbuf_slices_update = resident_dbuf_slices_update;

	/* The locks, the steps, the errors and the debug messages. */
	k->ops.lock = resident_lock;
	k->ops.step = drv_i915_lcd_kernel_step;
	k->ops.error = drv_i915_lcd_kernel_error;
	k->ops.debug = drv_i915_lcd_kernel_debug;

	/* The Type-C ports' links, which an output's dark gives back. */
	k->ops.tc_put_link = drv_i915_lcd_tc_put_link;
	k->ops.tc_get_link = drv_i915_lcd_tc_get_link;
	k->ops.tc_mode = drv_i915_lcd_tc_mode;
	k->ops.tc_set_fia_lane_count = drv_i915_lcd_tc_set_fia_lane_count;
	k->ops.tc_pin_assignment = drv_i915_lcd_tc_pin_assignment;

	/* The Type-C ports' Dekel PHYs, behind the bank index their lock orders. */
	k->ops.dkl_read = drv_i915_lcd_dkl_read;
	k->ops.dkl_write = drv_i915_lcd_dkl_write;
	k->ops.dkl_rmw = drv_i915_lcd_dkl_rmw;

	/* The update within one frame: vblank references, the vblank sleep, the section's interrupts and the event. */
	k->ops.vblank_get = resident_vblank_get;
	k->ops.vblank_put = resident_vblank_put;
	k->ops.vblank_sleep = resident_vblank_sleep;
	k->ops.irq_off = resident_irq_off;
	k->ops.irq_on = resident_irq_on;
	k->ops.arm_event = resident_arm_event;
	k->ops.wait_event = resident_wait_event;
	k->ops.cancel_event = resident_cancel_event;
}

/*
 * Checks that the hardware is as the start left it for the run's output:
 * its pipe and port idle and the GT memory there for the buffers.
 * Anything else is not the run's to fix.  Returns 0, or EBUSY when nothing
 * may be written.
 */
int
drv_i915_resident_preflight(
	struct i915_lcd_kernel *k)
{
	int error;

	/* An HDMI run checks its own pipe and port. */
	if (k->p != NULL && k->p->output_hdmi) {
		error = resident_preflight_hdmi(k);
		if (error != 0)
			return error;

		/* Succeeded: the HDMI output may be written. */
		return 0;
	}

	/* So does an external DP run. */
	if (k->p != NULL && k->p->output_dp_ext) {
		error = resident_preflight_dp_ext(k);
		if (error != 0)
			return error;

		/* Succeeded: the external DP output may be written. */
		return 0;
	}

	/* The panel's run checks pipe A, the panel's port and the interrupt mask. */
	error = resident_preflight_panel(k);
	if (error != 0)
		return error;

	/* Succeeded: the panel may be written. */
	return 0;
}

/*
 * Fills the configuration a screen is prepared from, for an output: what
 * the display's start found (the panel's VBT data, the memory's
 * watermarks, the display buffer, the CD clock, the bandwidth and the DMC
 * programs), then the output's port, pipe, PLL, mode and link.  The
 * framebuffer and the other pipes lit beside it are the caller's.
 * Returns 0, or EINVAL when the VBT has no panel data for port A.
 */
int
drv_i915_resident_config(
	struct i915_lcd_kernel *k,
	const struct i915_display_output *output,
	struct i915_xd_picture_config *config)
{
	int error;

	/* What the start found, and the panel as the output. */
	error = resident_config_panel(k, config);
	if (error != 0)
		return error;

	/* The output's encoder in the panel's place: an HDMI display, or an external DP display. */
	if (output->kind == I915_OUTPUT_KIND_HDMI) {
		resident_config_hdmi(k, output, config);
	} else if (output->kind == I915_OUTPUT_KIND_DP_EXT) {
		resident_config_dp_ext(k, output, config);
	}

	/* Succeeded: the configuration is complete. */
	return 0;
}

/*
 * Lowers an external DP output's link after its link did not train, and
 * plans the output's link again for the next run.  The output is the
 * resident one or the second output.  Only a light that reported the link
 * as not trained is retried.  Returns 0 when the next run may try the
 * lower link, ENOSPC when no lower link carries the mode, or EINVAL for an
 * output or a light that did not fail at the link.
 */
int
drv_i915_resident_link_fallback(
	struct i915_display *display,
	struct i915_display_output *output,
	int enable_rc)
{
	struct i915_display_link next;
	unsigned long irq;
	int max_rate;
	int max_lanes;
	int error;

	/* Only an external DP output trains a link, and only a link that did not train is retried. */
	if (output->kind != I915_OUTPUT_KIND_DP_EXT)
		return EINVAL;
	if (enable_rc != I915_XD_PICTURE_LINK_NOT_TRAINED)
		return EINVAL;

	/* Lowers the port's limits below the link that failed. */
	error = drv_i915_dp_ext_link_fallback(display, output->port, (int)output->link.rate_khz, (int)output->link.lanes);
	if (error != 0)
		return ENOSPC;

	/* The limits now in force. */
	error = drv_i915_dp_ext_link_limits(display, output->port, &max_rate, &max_lanes);
	if (error != 0)
		return ENOSPC;

	/* The mode's link under them, planned apart so a refusal leaves the output as it was. */
	error = drv_i915_output_link_dp(display->xd.choice.platform, (unsigned)output->port, &output->mode, &output->sink, max_rate, max_lanes, output->link.ref_khz, &next);
	if (error != 0) {
		kern_logf("i915: resident display: DP on TC%u: no lower link carries the mode (%d)\n", output->tc_port + 1U, error);
		return ENOSPC;
	}

	/* Logs the retry. */
	kern_logf("i915: resident display: DP on TC%u: retrying at %u kHz x%u (was %u kHz x%u)\n",
	    output->tc_port + 1U,
	    next.rate_khz,
	    next.lanes,
	    output->link.rate_khz,
	    output->link.lanes);

	/* The next run drives the lower link, under the lock a change of output takes. */
	irq = spin_lock_irqsave(&display->device->irq_lock);

	output->link = next;

	spin_unlock_irqrestore(&display->device->irq_lock, irq);

	/* Succeeded: the run may be tried again. */
	return 0;
}

/* Records the first anomaly of a run; later ones are its consequences. */
static void
resident_anomaly(
	struct i915_lcd_show_report *r,
	const char *what,
	int rc)
{
	/* The first one is the finding. */
	if (r->first_anomaly != NULL)
		return;

	/* Keeps the first anomaly, its stage and its code. */
	r->first_anomaly = what;
	r->first_anomaly_stage = r->stage;
	r->first_anomaly_rc = rc;
}

/* Records a stage the run reached and tells the run's stage hook. */
static void
resident_reached(
	struct i915_lcd_show_env *env,
	struct i915_lcd_show_report *r,
	int stage)
{
	/* The furthest stage so far. */
	r->stage = stage;

	/* The run logs registers at some stages. */
	if (env->at_stage != NULL)
		env->at_stage(env->at_stage_ctx, stage);
}

/* Finds the first error entry of the run log at or after an index; -1 when none. */
static int
resident_first_error_after(
	const struct i915_lcd_trace *trace,
	unsigned from)
{
	unsigned index;

	/* Scans the entries from the index on. */
	for (index = from; index < trace->n; index++) {
		if (trace->e[index].kind == I915_LCD_T_ERROR)
			return (int)index;
	}

	/* No error was recorded there. */
	return -1;
}

/*
 * Shows a buffer the run prepared and stops it safely.
 *
 * The pixels are not touched and the buffer is not created, unpinned or
 * destroyed here; it must be pinned and unused.  On return it is either
 * pinned again (display_released: the stop was confirmed) or abandoned
 * (the stop was not confirmed: kept for ever, the device latch is set).
 * Returns 0 when the picture was up for the whole window and the stop was
 * confirmed; EBUSY when nothing was touched; EINVAL for a buffer that is
 * not pinned; EIO when the run did not pass.
 */
static int
resident_show(
	struct i915_display *display,
	struct i915_lcd_show_env *env,
	struct i915_scanout *so,
	struct i915_lcd_show_report *r)
{
	int error;

	/* An earlier run left resources the display or the GPU may still use: nothing is touched. */
	error = resident_show_begin(display, env, r);
	if (error != 0)
		return error;

	/* The buffer must be pinned and unused. */
	if (so->state != I915_SCANOUT_PINNED) {
		resident_anomaly(r, "the prepared buffer is not a pinned, unused scanout buffer", EINVAL);
		return EINVAL;
	}

	/* The buffer is ready. */
	resident_reached(env, r, I915_LCD_SHOW_BUFFER_READY);

	/* Shows it and stops it. */
	(void)resident_show_display(display, env, so, r);

	/* The display let go; the run releases the buffer. */
	r->released = r->display_released;
	r->pass = resident_show_passed(display, r);
	if (!r->pass)
		return EIO;

	/* Succeeded: the picture was up and the display let go of it. */
	return 0;
}

/*
 * The display part of a run: the prepare, the light, the picture's
 * window, the dark, and the decision whether the display has let go of
 * the buffer.  Never creates, fills, unpins or destroys the buffer.
 * Returns 1 when the buffer was handed to the display (the light was
 * attempted), 0 when it never was.
 */
static int
resident_show_display(
	struct i915_display *display,
	struct i915_lcd_show_env *env,
	struct i915_scanout *so,
	struct i915_lcd_show_report *r)
{
	struct i915_xd_pictures *pictures;
	struct i915_lcd_trace *trace;
	unsigned disable_from;
	unsigned waited;
	uint32_t frame0;

	/* The trace and the pictures the show runs on, and the buffer's address. */
	trace = &display->show_trace;
	pictures = &display->xd.pictures;
	r->surf = so->surf;

	/* The framebuffer is the one the buffer describes. */
	env->cfg.fb_fourcc = so->format;
	env->cfg.fb_modifier = so->modifier;
	env->cfg.fb_width = so->width;
	env->cfg.fb_height = so->height;
	env->cfg.fb_pitch = so->pitch;
	env->cfg.fb_surf = (uint32_t)so->surf;

	/* The run log stacks on the backend, and the observer samples at every commit point. */
	drv_i915_lcd_trace_init(trace, env->hw);
	drv_i915_lcd_observer_init(&r->obs, env->hw, env->pipe);
	trace->tap = drv_i915_lcd_observer_point;
	trace->tap_ctx = &r->obs;

	/* Prepares the screen; a refusal writes nothing. */
	r->prepare_rc = drv_i915_xd_picture_prepare(pictures, I915_XD_PICTURE_RESIDENT, &env->cfg, &trace->ops);
	drv_i915_xd_picture_status(pictures, I915_XD_PICTURE_RESIDENT, &r->at_enable);
	if (r->prepare_rc != 0) {
		if (r->at_enable.first_error != NULL) {
			resident_anomaly(r, r->at_enable.first_error, r->prepare_rc);
		} else {
			resident_anomaly(r, "the screen's prepare refused the configuration", r->prepare_rc);
		}

		/* The show ends; the report says why. */
		return 0;
	}

	/* The prepare is done. */
	resident_reached(env, r, I915_LCD_SHOW_PREPARED);

	/* The light: from here on the display may read the buffer. */
	r->begin_rc = drv_i915_scanout_begin(so);
	if (r->begin_rc != 0) {
		resident_anomaly(r, "scanout begin", r->begin_rc);
		return 0;
	}

	/* From here on the display may read the buffer: the light. */
	r->display_acquired = 1;
	drv_i915_lcd_trace_phase(trace, "commit: enable");
	r->enable_rc = drv_i915_xd_picture_light(pictures, I915_XD_PICTURE_RESIDENT);
	drv_i915_xd_picture_status(pictures, I915_XD_PICTURE_RESIDENT, &r->at_enable);
	drv_i915_xd_picture_link_status(pictures, I915_XD_PICTURE_RESIDENT, &r->at_enable);
	r->enable_errors = r->at_enable.errors;
	r->first_error_trace_at = trace->first_error_at;
	if (r->enable_rc != I915_XD_PICTURE_OK) {
		if (r->at_enable.first_error != NULL) {
			resident_anomaly(r, r->at_enable.first_error, r->enable_rc);
		} else {
			resident_anomaly(r, "the enable commit did not succeed", r->enable_rc);
		}
	}

	/* The light returned. */
	resident_reached(env, r, I915_LCD_SHOW_ENABLE_RETURNED);

	/* "The arm was written" is a software fact; whether the timing runs is asked of the frame counter. */
	if (r->enable_rc == I915_XD_PICTURE_OK) {
		r->first_frames_rc = drv_i915_lcd_observer_frames(&r->obs, env->first_frames_ms, I915_RESIDENT_FIRST_FRAMES, &r->frame_first, &r->frame_last);
		if (r->first_frames_rc != 0)
			resident_anomaly(r, "the pipe's frame counter does not advance after the enable", r->first_frames_rc);
	}

	/* The picture is up: the in-window work, then the finite window. */
	if (r->enable_rc == I915_XD_PICTURE_OK && r->first_frames_rc == 0) {
		drv_i915_lcd_observer_steady_begin(&r->obs);
		resident_reached(env, r, I915_LCD_SHOW_PICTURE_UP);

		/* The work that runs while the picture is up. */
		if (env->in_window != NULL) {
			r->window_hook_rc = env->in_window(env->in_window_ctx, &r->obs);
			if (r->window_hook_rc != 0)
				resident_anomaly(r, "the in-window test failed while the picture was up", r->window_hook_rc);
		}

		/* The finite window: every round wants 30 more frames (about half a second at 60 Hz) within 1.5 s. */
		r->steady_frame_first = r->frame_last;
		for (waited = 0U; r->window_hook_rc == 0 && waited < env->window_ms; waited += I915_RESIDENT_STEADY_STEP_MS) {
			frame0 = 0U;
			r->steady_rc = drv_i915_lcd_observer_frames(&r->obs, I915_RESIDENT_STEADY_ROUND_MS, I915_RESIDENT_STEADY_FRAMES, &frame0, &r->steady_frame_last);
			drv_i915_lcd_observer_steady_sample(&r->obs);
			r->steady_rounds++;
			if (r->steady_rc != 0) {
				resident_anomaly(r, "the frame counter stopped advancing while the picture was up", r->steady_rc);
				break;
			}
		}

		/* The state at the end of the window. */
		drv_i915_lcd_observer_steady_end(&r->obs);
		drv_i915_xd_picture_status(pictures, I915_XD_PICTURE_RESIDENT, &r->at_window_end);
		drv_i915_xd_picture_link_status(pictures, I915_XD_PICTURE_RESIDENT, &r->at_window_end);

		/* A DP sink that lost the link while the picture was up. */
		if (!r->output_hdmi && r->steady_rc == 0) {
			if (!r->at_window_end.cr_ok || !r->at_window_end.eq_ok)
				resident_anomaly(r, "the sink lost the link while the picture was up", EIO);
		}

		/* An underrun while the picture stood. */
		if (r->obs.seen_steady != 0U)
			resident_anomaly(r, "underrun status during the steady picture", EIO);

		/* The window is done. */
		resident_reached(env, r, I915_LCD_SHOW_WINDOW_DONE);
	}

	/* The dark, whatever happened above. */
	disable_from = trace->n;
	drv_i915_lcd_trace_phase(trace, "commit: disable");
	r->disable_rc = drv_i915_xd_picture_dark(pictures, I915_XD_PICTURE_RESIDENT);
	drv_i915_xd_picture_status(pictures, I915_XD_PICTURE_RESIDENT, &r->at_disable);
	r->cleanup_errors = r->at_disable.errors - r->enable_errors;
	r->cleanup_first_error_trace_at = resident_first_error_after(trace, disable_from);
	if (r->disable_rc != I915_XD_PICTURE_OK)
		resident_anomaly(r, "the disable commit did not succeed", r->disable_rc);

	/* The dark returned. */
	resident_reached(env, r, I915_LCD_SHOW_DISABLE_RETURNED);

	/* Has the display let go of the buffer?  Only when the hardware says the pipe stands. */
	r->stopped_rc = EBUSY;
	if (r->disable_rc == I915_XD_PICTURE_OK) {
		r->stopped_rc = drv_i915_lcd_observer_stopped(&r->obs, I915_RESIDENT_STOP_WATCH_MS, &r->stop_frame_first, &r->stop_frame_last);

		/* Read at the pipe-disabled point, the well still on. */
		r->transconf_after_stop = r->obs.stop_transconf;
		if (r->stopped_rc != 0)
			resident_anomaly(r, "after the disable the pipe still reports activity", r->stopped_rc);
	}

	/* The stop is confirmed: the buffer is the run's again. */
	if (r->disable_rc == I915_XD_PICTURE_OK && r->stopped_rc == 0) {
		resident_reached(env, r, I915_LCD_SHOW_STOP_CONFIRMED);
		drv_i915_xd_picture_released(pictures, I915_XD_PICTURE_RESIDENT);
		drv_i915_scanout_end(so);
		r->display_released = 1;
	} else {
		/*
		 * Not confirmed: the backing pages, their DMA mapping, the GGTT
		 * entries, the pin and its owner stay for ever; the screen keeps
		 * what it holds (DC off, power, the display buffer).
		 */
		drv_i915_scanout_abandon(so);
		drv_i915_xd_picture_abandoned(pictures, I915_XD_PICTURE_RESIDENT);
		display->show_retained = 1;
		display->show_retained_hw = env->hw;
		r->abandoned = 1;
		resident_reached(env, r, I915_LCD_SHOW_ABANDONED);
	}

	/* The buffer was handed to the display. */
	return 1;
}

/* Judges a show: no anomaly, the picture up for the window, the link trained, no underrun, nothing unresolved. */
static int
resident_show_passed(
	struct i915_display *display,
	const struct i915_lcd_show_report *r)
{
	/* The first anomaly, the in-window work and the release decide first. */
	if (r->first_anomaly != NULL)
		return 0;
	if (r->window_hook_rc != 0)
		return 0;
	if (!r->display_released)
		return 0;

	/* The light and the window. */
	if (r->enable_rc != I915_XD_PICTURE_OK)
		return 0;
	if (r->steady_rc != 0)
		return 0;

	/* A DP sink must have reported a trained link after the light. */
	if (!r->output_hdmi) {
		if (!r->at_enable.cr_ok || !r->at_enable.eq_ok)
			return 0;
	}

	/* No underrun in the steady picture, and the vblank interrupt never unmasked. */
	if (r->obs.seen_steady != 0U)
		return 0;
	if (r->obs.vblank_unmasked_seen != 0)
		return 0;

	/* A complete run log without unresolved steps. */
	if (display->show_trace.steps != 0U)
		return 0;
	if (display->show_trace.dropped != 0U)
		return 0;

	/* Succeeded: the run passed. */
	return 1;
}

/* Starts a show's report, refusing when an earlier run's resources are retained. */
static int
resident_show_begin(
	struct i915_display *display,
	struct i915_lcd_show_env *env,
	struct i915_lcd_show_report *r)
{
	int retained;

	/* An earlier run left resources the display or the GPU may still use: nothing is touched. */
	retained = drv_i915_resident_retained(display);
	if (retained)
		return EBUSY;

	/* A screen the pictures still retain is busy. */
	retained = drv_i915_xd_picture_retained(&display->xd.pictures, I915_XD_PICTURE_RESIDENT);
	if (retained)
		return EBUSY;

	/* A fresh report of this run over the display's run log. */
	kern_memset(r, 0, sizeof(*r));
	r->first_error_trace_at = -1;
	r->cleanup_first_error_trace_at = -1;
	r->trace = &display->show_trace;

	/* An HDMI sink trains no link the judgement could ask for. */
	if (env->cfg.kind == I915_XD_SCREEN_HDMI)
		r->output_hdmi = 1;

	/* Succeeded: the run may start. */
	return 0;
}

/* Logs the registers and the run's milestones at its stages (ctx: the run). */
static void
resident_at_stage(
	void *ctx,
	int stage)
{
	struct i915_lcd_kernel *k;
	struct i915_display *display;
	int debug;

	/* The run and its display. */
	k = ctx;
	display = k->display;

	/* Reacts to the stages the run log names. */
	switch (stage) {
	case I915_LCD_SHOW_ENABLE_RETURNED:
		/* Whatever is reported from here on belongs to the way down: counted apart. */
		k->phase_cleanup = 1;

		/* i915.debug=display prints the light's run log now, failed or not. */
		debug = drv_i915_lcd_debug_enabled();
		if (debug) {
			kern_logf("i915: resident display: the run log up to the light's return follows\n");
			drv_i915_lcd_log_trace(&display->show_trace);
		}

		break;
	default:
		break;
	}
}

/* Creates the two locks of the runs (the PLLs' and the backlight's) once; they live as long as the device. */
static void
resident_locks_init(
	struct i915_display *display)
{
	/* The locks exist already. */
	if (display->lcdb_locks_live)
		return;

	/* The PLL lock and the backlight lock. */
	(void)mutex_init(&display->lcdb_locks[I915_LCD_LOCK_DPLL], LOCK_RANK_DEVICE, "i915-lcd-dpll");
	(void)mutex_init(&display->lcdb_locks[I915_LCD_LOCK_BACKLIGHT], LOCK_RANK_DEVICE, "i915-lcd-backlight");
	display->lcdb_locks_live = 1;
}

/*
 * The register read hook of a run.  While the register trace is on every
 * access is printed; the update's probe records the first scan line the
 * update reads after its trigger.
 */
static uint32_t
resident_read32(
	void *ctx,
	uint32_t reg)
{
	struct i915_lcd_kernel *k;
	uint32_t value;

	/* The run the hook belongs to. */
	k = ctx;

	/* Reads the register. */
	value = drv_i915_read32(k->d->mmio, reg);

	/* The register trace prints every access (a diagnostic switch). */
	if (k->display->i915_lcd_reg_trace)
		kern_logf("i915: takeover: read 0x%05x = 0x%08x\n", reg, value);

	/* The update's probe (observation only): the first scan line read after the trigger. */
	if (k->probe_watch && reg == I915_RESIDENT_PIPE_DSL_A) {
		k->probe_first_dsl = value & I915_RESIDENT_PIPE_DSL_MASK;
		k->probe_watch = 0;
	}

	/* Reports the value. */
	return value;
}

/* The register write hook of a run. */
static void
resident_write32(
	void *ctx,
	uint32_t reg,
	uint32_t value)
{
	struct i915_lcd_kernel *k;

	/* The run the hook belongs to. */
	k = ctx;

	/* The register trace prints every access (a diagnostic switch). */
	if (k->display->i915_lcd_reg_trace)
		kern_logf("i915: takeover: write 0x%05x = 0x%08x\n", reg, value);

	/* Writes the register. */
	drv_i915_write32(k->d->mmio, reg, value);
}

/* The read-modify-write hook of a run: returns the old value. */
static uint32_t
resident_rmw32(
	void *ctx,
	uint32_t reg,
	uint32_t clear,
	uint32_t set)
{
	struct i915_lcd_kernel *k;
	uint32_t old;

	/* The run the hook belongs to. */
	k = ctx;

	/* Reads, clears, sets and writes back. */
	old = drv_i915_read32(k->d->mmio, reg);
	drv_i915_write32(k->d->mmio, reg, (old & ~clear) | set);

	/* Reports the value before the write. */
	return old;
}

/* The posting-read hook of a run. */
static void
resident_posting_read(
	void *ctx,
	uint32_t reg)
{
	struct i915_lcd_kernel *k;

	/* The run the hook belongs to. */
	k = ctx;

	/* Reads the register and drops the value. */
	(void)drv_i915_read32(k->d->mmio, reg);
}

/*
 * The register wait hook of a run: 2 us of polling without sleeping, then
 * sleeping up to timeout_ms.  Returns 0, I915_RESIDENT_ETIMEDOUT, or
 * I915_RESIDENT_EIO for a time base or wait primitive fault, which is also the
 * run's anomaly.
 */
static int
resident_wait_reg(
	void *ctx,
	uint32_t reg,
	uint32_t mask,
	uint32_t value,
	unsigned timeout_ms)
{
	struct i915_lcd_kernel *k;
	uint32_t last;
	int error;

	/* The run, and no value read yet. */
	k = ctx;
	last = 0U;

	/* Polls, then sleeps, until the masked register holds the value. */
	error = drv_i915_wait_reg(k->d->mmio, reg, mask, value, 2U, timeout_ms, &last);
	if (error == 0)
		return 0;

	/* The device did not reach the condition in time. */
	if (error == ETIMEDOUT) {
		k->wait_timeouts++;
		return I915_RESIDENT_ETIMEDOUT;
	}

	/* Anything else is the time source failing: not a timeout, and the run's first anomaly. */
	k->time_faults++;
	drv_i915_xd_pictures_fault(&k->display->xd.pictures, "time base / wait primitive fault in a register wait (not a timeout)\n");
	return I915_RESIDENT_EIO;
}

/* The sleep hook of a run: the resident eDP's tick sleep. */
static void
resident_usleep(
	void *ctx,
	unsigned us)
{
	struct i915_lcd_kernel *k;
	unsigned before;

	/* The run the hook belongs to. */
	k = ctx;

	/* Sleeps on the eDP's kernel backend and watches its time-fault count. */
	before = k->d->edp->k.time_faults;
	drv_i915_dp_kernel_sleep_us(&k->d->edp->k, us);

	/* A sleep that returned early or found the clock failed is the run's anomaly. */
	if (k->d->edp->k.time_faults != before) {
		k->time_faults++;
		drv_i915_xd_pictures_fault(&k->display->xd.pictures, "time base fault during a sleep (returned early or the clock failed)\n");
	}
}

/* The short-delay hook of a run. */
static void
resident_udelay(
	void *ctx,
	unsigned us)
{
	struct i915_lcd_kernel *k;
	int error;

	/* The run the hook belongs to. */
	k = ctx;

	/* Spins for the delay; a failed time base is the run's anomaly. */
	error = drv_i915_udelay(us);
	if (error != 0) {
		k->time_faults++;
		drv_i915_xd_pictures_fault(&k->display->xd.pictures, "time base fault during a short delay\n");
	}
}

/* The display buffer hook of a run: powers exactly these slices through the display core. */
static void
resident_dbuf_slices_update(
	void *ctx,
	unsigned slices)
{
	struct i915_lcd_kernel *k;

	/* The run the hook belongs to. */
	k = ctx;

	/* The same update the display core's start uses. */
	drv_i915_display_power_dbuf(k->d->power, (uint8_t)slices);
}

/* The lock hook of a run: takes or gives back the PLL lock or the backlight lock. */
static void
resident_lock(
	void *ctx,
	int which,
	int take)
{
	struct i915_lcd_kernel *k;

	/* The run the hook belongs to. */
	k = ctx;

	/* Only the two locks of the runs exist. */
	if (which < 0 || which > 1)
		return;

	/* The register trace names every lock step (a diagnostic switch). */
	if (k->display->i915_lcd_reg_trace) {
		if (take) {
			kern_logf("i915: takeover: lock %d take\n", which);
		} else {
			kern_logf("i915: takeover: lock %d give back\n", which);
		}
	}

	/* Takes or releases the lock. */
	if (take) {
		mutex_lock(&k->locks[which]);
	} else {
		mutex_unlock(&k->locks[which]);
	}
}

/* The interrupts-off hook of an update within one frame: remembers whether interrupts were on. */
static void
resident_irq_off(
	void *ctx)
{
	struct i915_lcd_kernel *k;

	/* The run the hook belongs to. */
	k = ctx;

	/* Turns the CPU's interrupts off and keeps the state they were in. */
	k->irq_was_on = kern_irq_disable();
}

/* The interrupts-on hook of an update within one frame: turns interrupts back on if they were. */
static void
resident_irq_on(
	void *ctx)
{
	struct i915_lcd_kernel *k;

	/* The run the hook belongs to. */
	k = ctx;

	/* Interrupts that were off before the section stay off. */
	if (k->irq_was_on)
		kern_irq_enable();
}

/*
 * The power hook of a run: names the power number of one power use of
 * the screen's output (the DC states held off, a pipe, a transcoder, a
 * DDI's lanes or IO, an AUX channel in its legacy or Thunderbolt use).
 *
 * A number is the index of the use's power function (I915_XD_POWERS_*),
 * I915_LCD_POWER_SLOTS - 1 for a use that needs no well, or -1 for an AUX
 * channel that has no Thunderbolt well, which the get refuses.
 */
static int
resident_power_domain(
	void *ctx,
	unsigned use,
	unsigned index,
	int thunderbolt)
{
	struct i915_lcd_kernel *k;
	unsigned version;
	uint64_t function;
	int number;

	/* The run the hook belongs to. */
	k = ctx;

	/* The AUX wells are named by the display's version. */
	version = 13U;
	if (k->display->xd.choice.platform != NULL)
		version = k->display->xd.choice.platform->display_ver;

	/* The function of each use; DDIs and AUX channels are numbered as the platform's ports are. */
	switch (use) {
	case I915_XD_OUTPUT_POWER_DC_OFF:
		function = I915_XD_POWERS_DC_OFF;
		break;
	case I915_XD_OUTPUT_POWER_PIPE:
		function = I915_XD_POWERS_PIPE(index);
		break;
	case I915_XD_OUTPUT_POWER_TRANS:
		function = I915_XD_POWERS_TRANS(index);
		break;
	case I915_XD_OUTPUT_POWER_LANES:
		function = I915_XD_POWERS_LANES(index);
		break;
	case I915_XD_OUTPUT_POWER_DDI_IO:
		function = I915_XD_POWERS_DDI_IO(index);
		break;
	case I915_XD_OUTPUT_POWER_AUX:
		function = drv_i915_display_power_aux(version, (int)index, thunderbolt);

		/* A channel without a Thunderbolt well has no number. */
		if (function == 0ULL)
			return -1;
		break;
	default:
		function = 0ULL;
		break;
	}

	/* Numbers the function. */
	number = resident_power_number(function);

	/* Succeeded: the function's number. */
	return number;
}

/* Numbers one power function by its bit, or a use that needs no well by the last slot. */
static int
resident_power_number(
	uint64_t function)
{
	int bit;

	/* A use that needs no well. */
	if (function == 0ULL)
		return (int)I915_LCD_POWER_SLOTS - 1;

	/* The lowest bit of the function. */
	bit = __builtin_ctzll(function);

	/* Succeeded: the function's bit. */
	return bit;
}

/* Gives the power functions of a power number: one bit, or none for the last slot. */
static uint64_t
resident_power_functions(
	int number)
{
	/* The last slot needs no well. */
	if (number == (int)I915_LCD_POWER_SLOTS - 1)
		return 0ULL;

	/* Succeeded: the function's bit. */
	return 1ULL << (unsigned)number;
}

/*
 * The power hook of a run: takes a power reference.
 *
 * Returns the cookie number + 1, or 0 when the number is unknown or its
 * wells did not come up.  The run counts its references per number.
 */
static int
resident_power_get(
	void *ctx,
	int number)
{
	struct i915_lcd_kernel *k;
	uint64_t functions;
	int error;

	/* The run the hook belongs to. */
	k = ctx;

	/* Refuses an unknown number. */
	if (number < 0 || number >= (int)I915_LCD_POWER_SLOTS) {
		k->power_get_failures++;
		return 0;
	}

	/* Takes the wells. */
	functions = resident_power_functions(number);
	error = drv_i915_display_power_get(k->d->power, functions);
	if (error != 0) {
		k->power_get_failures++;
		kern_logf("i915: display run: power get %d FAILED rc=%d\n", number, error);
		return 0;
	}

	/* The run's count of the references it holds on the number. */
	k->power_refs[number]++;

	/* Succeeded: the cookie names the number. */
	return number + 1;
}

/*
 * The power hook of a run: takes a power reference only when its wells
 * are on already, so that a readout never turns a well on.  Returns the
 * cookie, or 0 when they are off or the number is unknown.
 */
static int
resident_power_get_if_enabled(
	void *ctx,
	int number)
{
	struct i915_lcd_kernel *k;
	uint64_t functions;
	int powered;
	int cookie;

	/* The run the hook belongs to. */
	k = ctx;

	/* An unknown number is not on. */
	if (number < 0 || number >= (int)I915_LCD_POWER_SLOTS)
		return 0;

	/* The wells must all be on. */
	functions = resident_power_functions(number);
	powered = drv_i915_display_power_is_on(k->d->power, functions);
	if (!powered)
		return 0;

	/* Takes the reference. */
	cookie = resident_power_get(ctx, number);

	/* Succeeded: the cookie (0 when the get failed). */
	return cookie;
}

/*
 * The power hook of a run: gives a power reference back.
 *
 * A put the run holds no reference for is counted and not forwarded.  A
 * well that could not be turned off (a failed pipe interrupt drain) is a
 * fault of the run's backend: the stop is not confirmed.
 */
static void
resident_power_put(
	void *ctx,
	int number,
	int cookie)
{
	struct i915_lcd_kernel *k;
	uint64_t functions;
	unsigned refusals;

	UNUSED_PARAMETER(cookie);

	/* The run the hook belongs to. */
	k = ctx;

	/* Refuses a put without a reference of this run. */
	if (number < 0 ||
	    number >= (int)I915_LCD_POWER_SLOTS ||
	    k->power_refs[number] <= 0) {
		k->errors++;
		kern_logf("i915: display run: power put %d without a reference of this run: NOT forwarded\n", number);
		return;
	}

	/* The run's count of the references it holds on the number. */
	k->power_refs[number]--;

	/* Gives the wells back, watching for a refused well off. */
	functions = resident_power_functions(number);
	refusals = k->d->power->refusals;
	drv_i915_display_power_put(k->d->power, functions);
	if (k->d->power->refusals != refusals) {
		drv_i915_xd_pictures_fault(&k->display->xd.pictures, "a power well was NOT turned off: the pipe interrupt drain failed (the stop is not "
			"confirmed; display power is kept)\n");
	}
}

/*
 * The power hook of a run: gives a power reference back after a delay.
 * A put the run holds no reference for is counted and not forwarded.
 */
static void
resident_power_put_async(
	void *ctx,
	int number,
	int cookie,
	int delay_ms)
{
	struct i915_lcd_kernel *k;
	uint64_t functions;

	UNUSED_PARAMETER(cookie);

	/* The run the hook belongs to. */
	k = ctx;

	/* Refuses a put without a reference of this run. */
	if (number < 0 ||
	    number >= (int)I915_LCD_POWER_SLOTS ||
	    k->power_refs[number] <= 0) {
		k->errors++;
		kern_logf("i915: display run: delayed power put %d without a reference of this run: NOT forwarded\n", number);
		return;
	}

	/* The run's count of the references it holds on the number. */
	k->power_refs[number]--;

	/* Hands the reference to the delayed give-back. */
	functions = resident_power_functions(number);
	drv_i915_display_power_put_later(k->d->power, functions, delay_ms);
}

/* The vblank hook of a run: takes a vblank reference of the pipe; 0, or I915_RESIDENT_EINVAL when its interrupts are off. */
static int
resident_vblank_get(
	void *ctx,
	int pipe)
{
	struct i915_lcd_kernel *k;
	int error;

	/* The run the hook belongs to. */
	k = ctx;

	/* Takes the reference on the display's interrupt state. */
	error = drv_i915_display_vblank_get(k->d->irq, (unsigned)pipe);
	if (error != 0)
		return I915_RESIDENT_EINVAL;

	/* Succeeded: the pipe's vblank interrupt is on and referenced. */
	return 0;
}

/* The vblank hook of a run: gives a vblank reference of the pipe back; the last one turns the interrupt off. */
static void
resident_vblank_put(
	void *ctx,
	int pipe)
{
	struct i915_lcd_kernel *k;

	/* The run the hook belongs to. */
	k = ctx;

	/* Gives the reference back. */
	drv_i915_display_vblank_put(k->d->irq, (unsigned)pipe);
}

/*
 * The sleep hook of a run: sleeps until the pipe's next vblank interrupt
 * or the ticks (10 ms each) run out.  Returns the ticks left (at least 1)
 * when the vblank came, 0 when it did not.  A sleep entered with the
 * interrupts off is counted, and a broken time base is the run's anomaly.
 */
static long
resident_vblank_sleep(
	void *ctx,
	int pipe,
	long ticks)
{
	struct i915_lcd_kernel *k;
	bool on;
	int error;

	/* The run the hook belongs to. */
	k = ctx;

	/* Counts a sleep entered with the interrupts off; their state is put back as it was. */
	on = kern_irq_disable();
	if (on) {
		kern_irq_enable();
	} else {
		k->sleep_irq_off++;
	}

	/* Waits for one new vblank of the pipe. */
	error = drv_i915_wait_vblank(k->d->irq, (unsigned)pipe, 1U, (unsigned)ticks * 10U, resident_frame, k, 0);
	k->vblank_sleeps++;

	/* A broken time base is the run's anomaly, not a timeout. */
	if (error == EIO)
		drv_i915_xd_pictures_fault(&k->display->xd.pictures, "time base fault while waiting for a vblank\n");

	/* A sleep that ended without the vblank. */
	if (error != 0)
		return 0;

	/* A sleep that ended on the vblank leaves at least one tick. */
	if (ticks > 1)
		return ticks - 1;

	/* Succeeded: the vblank came in the last tick. */
	return 1;
}

/* The event hook of a run: arms the flip event, which completes at the pipe's next vblank after now. */
static void
resident_arm_event(
	void *ctx,
	int pipe)
{
	struct i915_lcd_kernel *k;

	/* The run the hook belongs to. */
	k = ctx;

	/* The armed event, its pipe and the frame it was armed in. */
	k->event_armed = 1;
	k->event_pipe = pipe;
	k->event_frame = resident_frame(k);
}

/*
 * The event hook of a run: waits for the armed flip event, which
 * completes with a new vblank of its pipe and the frame counter past the
 * arming frame.  Returns 0, I915_RESIDENT_EINVAL when no event of the pipe is
 * armed, I915_RESIDENT_ETIMEDOUT when it did not complete in time, or
 * I915_RESIDENT_EIO when the time base failed.
 */
static int
resident_wait_event(
	void *ctx,
	int pipe,
	unsigned timeout_ms)
{
	struct i915_lcd_kernel *k;
	uint32_t frame;
	int error;

	/* The run the hook belongs to. */
	k = ctx;

	/* Refuses a wait for an event that is not armed on this pipe. */
	if (!k->event_armed)
		return I915_RESIDENT_EINVAL;
	if (pipe != k->event_pipe)
		return I915_RESIDENT_EINVAL;

	/* Waits for a new vblank of the pipe. */
	error = drv_i915_wait_vblank(k->d->irq, (unsigned)pipe, 1U, timeout_ms, resident_frame, k, 0);

	/* A vblank that left the frame counter at the arming frame did not complete the event. */
	if (error == 0) {
		frame = resident_frame(k);
		if (frame == k->event_frame)
			error = ETIMEDOUT;
	}

	/* A broken time base. */
	if (error == EIO)
		return I915_RESIDENT_EIO;

	/* An event that did not complete in time. */
	if (error != 0)
		return I915_RESIDENT_ETIMEDOUT;

	/* Succeeded: the event completed and is no longer armed. */
	k->event_armed = 0;
	return 0;
}

/* The event hook of a run: forgets the armed flip event; a later wait is refused and a late vblank completes nothing. */
static void
resident_cancel_event(
	void *ctx,
	int pipe)
{
	struct i915_lcd_kernel *k;

	UNUSED_PARAMETER(pipe);

	/* The run the hook belongs to. */
	k = ctx;

	/* Counts an event that was still armed, and forgets it. */
	if (k->event_armed)
		k->events_cancelled++;
	k->event_armed = 0;
}

/* Reads the frame counter of the run's pipe: its parameters' pipe, or the panel's pipe A. */
static uint32_t
resident_frame(
	void *ctx)
{
	struct i915_lcd_kernel *k;
	uint32_t frame;
	unsigned pipe;

	/* The run the hook belongs to. */
	k = ctx;

	/* The pipe the run drives. */
	pipe = 0U;
	if (k->p != NULL)
		pipe = (unsigned)k->p->pipe;

	/* Succeeded: the pipe's frame counter. */
	frame = drv_i915_read32(k->d->mmio, I915_XD_PIPE_FRMCNT(pipe));
	return frame;
}

/*
 * Checks that the panel's pipe A, its PLL and its port are idle, that the
 * panel is live and its link planned, that the GT memory is there, and that
 * the interrupt state keeps vblank and underrun masked.
 */
static int
resident_preflight_panel(
	struct i915_lcd_kernel *k)
{
	const struct i915_lcd_kernel_deps *d;
	uint32_t transconf;
	uint32_t plane_ctl;
	uint32_t pll;
	uint32_t ddi;
	uint32_t imr;
	uint32_t want;
	int idle;
	int masked;
	int ok;

	/* The run's objects; nothing failed yet. */
	d = k->d;
	ok = 1;

	/* The resident eDP must be live and its link planned. */
	if (!d->edp->connector_live || d->edp->link_rc != 0) {
		kern_logf("i915: resident display: preflight: the panel is not live (connector_live=%d link rc=%d)\n",
		    d->edp->connector_live,
		    d->edp->link_rc);
		return EBUSY;
	}

	/* The scanout buffer needs the GT memory's GGTT. */
	if (!d->gm->inited) {
		kern_logf("i915: resident display: preflight: no GT memory (GGTT) for the scanout buffer\n");
		return EBUSY;
	}

	/* Reads pipe A's transcoder and first plane, the first combo PLL, port A's buffer and pipe A's interrupt mask. */
	transconf = drv_i915_read32(d->mmio, I915_XD_TRANS_CONF(0U));
	plane_ctl = drv_i915_read32(d->mmio, I915_XD_PLANE_CTL(0U));
	pll = drv_i915_read32(d->mmio, I915_XD_DPLL0_ENABLE);
	ddi = drv_i915_read32(d->mmio, I915_XD_DDI_BUF_CTL(0U));
	imr = drv_i915_read32(d->mmio, I915_XD_DE_PIPE_ISR(0U) + I915_XD_DE_IMR_OFFSET);

	/* A running display means the start conditions are not the prepared ones: nothing may be written. */
	if ((transconf & I915_RESIDENT_TRANS_ON_MASK) != 0U ||
	    (plane_ctl & I915_RESIDENT_ENABLE_BIT) != 0U ||
	    (pll & I915_RESIDENT_ENABLE_BIT) != 0U ||
	    (ddi & I915_RESIDENT_ENABLE_BIT) != 0U) {
		kern_logf("i915: resident display: preflight: the display is not idle (TRANSCONF 0x%08x PLANE_CTL 0x%08x DPLL 0x%08x DDI_BUF_CTL 0x%08x)\n",
		    transconf,
		    plane_ctl,
		    pll,
		    ddi);
		ok = 0;
	}

	/*
	 * Pipe A's registers sit in its power well, which is off until the
	 * light takes the pipe's power: they read 0 here and say nothing.
	 * Checked instead: the interrupt state's mask, which the well's enable
	 * programs, keeps vblank (bit 0) and underrun (bit 31, and bits 22 and
	 * 21 from display version 13) masked.
	 */
	want = I915_RESIDENT_KEEP_MASKED_D12;
	if (k->display->device->gt.display_ver >= 13U)
		want = I915_RESIDENT_KEEP_MASKED_D13;

	/* Refuses an interrupt state that leaves vblank or underrun unmasked. */
	if ((d->irq->de_irq_mask[0] & want) != want) {
		kern_logf("i915: resident display: preflight: the interrupt state's pipe A mask 0x%08x would leave vblank / underrun unmasked (this display needs 0x%08x)\n",
		    d->irq->de_irq_mask[0],
		    want);
		ok = 0;
	}

	/* Logs what was found. */
	idle = 0;
	if ((pll & I915_RESIDENT_ENABLE_BIT) == 0U && (ddi & I915_RESIDENT_ENABLE_BIT) == 0U)
		idle = 1;

	/* Whether pipe A's interrupts are masked as the run wants. */
	masked = 0;
	if ((d->irq->de_irq_mask[0] & I915_RESIDENT_KEEP_MASKED_D13) == I915_RESIDENT_KEEP_MASKED_D13)
		masked = 1;

	/* Logs what the preflight found. */
	kern_logf("i915: resident display: preflight: DDI / PLL idle=%d (pipe A registers read TRANSCONF 0x%08x PLANE_CTL 0x%08x IMR 0x%08x with its power well off) | interrupt state pipe A mask 0x%08x: vblank + underrun masked=%d\n",
	    idle,
	    transconf,
	    plane_ctl,
	    imr,
	    d->irq->de_irq_mask[0],
	    masked);

	/* The display is not in the prepared state. */
	if (!ok)
		return EBUSY;

	/* Succeeded: the run may write the display. */
	return 0;
}

/* Checks that pipe B and DDI B are idle for an HDMI run, and that the GT memory is there. */
static int
resident_preflight_hdmi(
	struct i915_lcd_kernel *k)
{
	const struct i915_lcd_kernel_deps *d;
	uint32_t transconf;
	uint32_t buf_ctl;
	uint32_t south;
	uint32_t pll0;
	uint32_t pll1;

	/* The objects the run reads. */
	d = k->d;

	/* Reads the pipe, the port, the south display status and both combo PLLs. */
	transconf = drv_i915_read32(d->mmio, I915_XD_TRANS_CONF((uint32_t)k->p->pipe));
	buf_ctl = drv_i915_read32(d->mmio, I915_XD_DDI_BUF_CTL((uint32_t)k->p->port));
	south = drv_i915_read32(d->mmio, I915_RESIDENT_SDE_ISR);
	pll0 = drv_i915_read32(d->mmio, I915_RESIDENT_DPLL0_ENABLE);
	pll1 = drv_i915_read32(d->mmio, I915_RESIDENT_DPLL1_ENABLE);

	/* The scanout buffer needs the GT memory's GGTT. */
	if (!d->gm->inited) {
		kern_logf("i915: HDMI preflight: no GT memory (GGTT) for the scanout buffer\n");
		return EBUSY;
	}

	/* Logs what was read. */
	kern_logf("i915: HDMI preflight: TRANSCONF(%c)=0x%08x DDI_BUF_CTL(%c)=0x%08x SDEISR=0x%08x DPLL0=0x%08x DPLL1=0x%08x\n",
	    (char)('A' + k->p->pipe),
	    transconf,
	    (char)('A' + k->p->port),
	    buf_ctl,
	    south,
	    pll0,
	    pll1);

	/* The pipe and the port must be off. */
	if ((transconf & I915_RESIDENT_ENABLE_BIT) != 0U || (buf_ctl & I915_RESIDENT_ENABLE_BIT) != 0U) {
		kern_logf("i915: HDMI preflight: the pipe / the port are not idle\n");
		return EBUSY;
	}

	/* Succeeded: the HDMI output may be written. */
	return 0;
}

/* Checks that the run's pipe and the Type-C port's DDI are idle for an external DP run, and that the GT memory is there. */
static int
resident_preflight_dp_ext(
	struct i915_lcd_kernel *k)
{
	const struct i915_lcd_kernel_deps *d;
	uint32_t transconf;
	uint32_t buf_ctl;
	uint32_t buf_ctl_reg;

	/* The objects the run reads. */
	d = k->d;

	/* Reads the pipe's TRANS_CONF and the port's DDI_BUF_CTL. */
	buf_ctl_reg = I915_XD_DDI_BUF_CTL((uint32_t)k->p->port);
	transconf = drv_i915_read32(d->mmio, I915_XD_TRANS_CONF((uint32_t)k->p->pipe));
	buf_ctl = drv_i915_read32(d->mmio, buf_ctl_reg);

	/* The scanout buffer needs the GT memory's GGTT. */
	if (!d->gm->inited) {
		kern_logf("i915: DP-ext preflight: no GT memory (GGTT) for the scanout buffer\n");
		return EBUSY;
	}

	/* Logs the pipe and the port. */
	kern_logf("i915: DP-ext preflight: TRANSCONF(%c)=0x%08x DDI_BUF_CTL(0x%05x)=0x%08x\n",
	    (char)('A' + k->p->pipe),
	    transconf,
	    buf_ctl_reg,
	    buf_ctl);

	/* The pipe and the port's DDI must be off. */
	if ((transconf & I915_RESIDENT_ENABLE_BIT) != 0U || (buf_ctl & I915_RESIDENT_ENABLE_BIT) != 0U) {
		kern_logf("i915: DP-ext preflight: the pipe / the Type-C port's DDI are not idle\n");
		return EBUSY;
	}

	/* Succeeded: the external DP output may be written. */
	return 0;
}

/*
 * Fills the configuration of the panel on port A, pipe and transcoder A,
 * DPLL 0 and AUX A from what the display's start found: the panel's VBT
 * data, its sink and its planned link, the memory's watermarks, the
 * display buffer, the CD clock, the bandwidth of the allowed QGV point and
 * the DMC programs loaded.  Returns 0, or EINVAL when the VBT has no panel
 * data for port A.
 */
static int
resident_config_panel(
	struct i915_lcd_kernel *k,
	struct i915_xd_picture_config *config)
{
	struct i915_display *display;
	const struct i915_lcd_kernel_deps *d;
	struct i915_edp_device *edp;
	struct i915_vbt_panel *pn;
	unsigned allowed;
	unsigned point;
	unsigned i;
	uint32_t mbus;
	int error;

	/* The display, the run's objects and the panel. */
	display = k->display;
	d = k->d;
	edp = d->edp;
	pn = &display->fill_cfg_pn;

	/* Reads the panel's VBT data for port A. */
	kern_memset(config, 0, sizeof(*config));
	error = drv_i915_vbt_init_panel(&edp->vbt->parsed, 0, edp->res.edid, pn);
	if (error != 0) {
		kern_logf("i915: resident display: inputs: the VBT has no panel data for port A (rc=%d)\n", error);
		return EINVAL;
	}

	/* The panel on port A, pipe and transcoder A, DPLL 0, AUX A. */
	config->kind = I915_XD_SCREEN_PANEL;
	config->ddi = 0U;
	config->pipe = 0U;
	config->trans = 0U;
	config->aux = 0U;
	config->pll = 0U;

	/* The port reversal and four-lane bits the firmware left in DDI_BUF_CTL. */
	config->kept = edp->ddi_buf_ctl_readout & I915_RESIDENT_SAVED_PORT_BITS;

	/* The sink's capabilities. */
	kern_memcpy(config->caps, edp->res.dpcd, sizeof(config->caps));
	kern_memcpy(config->edp_caps, edp->res.edp_dpcd, sizeof(config->edp_caps));

	/* The panel's VBT eDP and backlight blocks, and the raw clock its PWM divides. */
	config->vbt_low_vswing = pn->edp_low_vswing;
	config->vbt_hobl = pn->edp_hobl;
	config->afc_override = pn->override_afc_startup;
	config->vbt_backlight_present = pn->bl_present;
	config->vbt_backlight_active_low = pn->bl_active_low_pwm;
	config->vbt_backlight_controller = pn->bl_controller;
	config->vbt_backlight_hz = pn->bl_pwm_freq_hz;
	config->vbt_backlight_min = pn->bl_min_brightness;
	config->rawclk_khz = edp->cfg.rawclk_khz;

	/* The watermark latencies and the SAGV block time the start read. */
	for (i = 0U; i < 8U; i++)
		config->wm_latency[i] = display->xd.memory.latency[i];

	/* The watermark levels, IPC and the SAGV block time. */
	config->wm_levels = (uint8_t)display->xd.memory.levels;
	config->wm_ipc = d->ipc_enabled;
	config->sagv_block_us = (uint8_t)display->xd.memory.sagv_block_us;

	/* The display buffer of the device: display 13 has 4096 blocks over four slices, Tiger Lake 2048 over two. */
	config->dbuf_size = I915_RESIDENT_DBUF_D13_BLOCKS;
	if (d->power->dbuf_mask == I915_RESIDENT_DBUF_TGL_SLICES)
		config->dbuf_size = I915_RESIDENT_DBUF_TGL_BLOCKS;

	/* The slices: the display's, or display 13's four. */
	config->dbuf_slice_mask = I915_RESIDENT_DBUF_D13_SLICES;
	if (d->power->dbuf_mask != 0U)
		config->dbuf_slice_mask = d->power->dbuf_mask;

	/* The slices powered now, and whether the bus is joined. */
	config->dbuf_slices_now = d->power->dbuf_slices;
	mbus = drv_i915_read32(d->mmio, I915_XD_MBUS_CTL);
	config->mbus_joined_now = (int)((mbus >> 31) & 1U);

	/* The CD clock the start left and its highest step. */
	config->cdclk.cdclk_khz = d->cdclk->hw.cdclk;
	config->cdclk.vco_khz = d->cdclk->hw.vco;
	config->cdclk.ref_khz = d->cdclk->hw.ref;
	config->cdclk.bypass_khz = d->cdclk->hw.bypass;
	config->cdclk.voltage = d->cdclk->hw.voltage_level;
	config->cdclk_max_khz = drv_i915_cdclk_max(d->cdclk);

	/* SAGV is kept off, which leaves exactly one QGV point allowed: its bandwidth for one active plane. */
	allowed = drv_i915_xd_bw_allowed_points(d->bandwidth);
	if (allowed != 0U && (allowed & (allowed - 1U)) == 0U) {
		/* Finds the one allowed point. */
		point = 0U;
		while ((allowed & (1U << point)) == 0U)
			point++;

		/* The bandwidth of the highest allowed point for one plane. */
		config->qgv_mbps = drv_i915_xd_bw_point_mbps(d->bandwidth, d->platform, 1U, point);
	}

	/* The DMC programs whose payload is loaded. */
	for (i = 0U; i < I915_DMC_FW_MAX; i++) {
		if (d->dmc->dmc.dmc_info[i].payload != NULL && d->dmc->dmc.dmc_info[i].present)
			config->dmc_programs |= 1U << i;
	}

	/* The panel's mode and the link its plan chose. */
	resident_config_link(&edp->mode, &edp->link, config);

	/* Logs every input. */
	kern_logf("i915: resident display: inputs: VBT low_vswing=%d hobl=%d afc_override=%d backlight present=%d active_low=%d controller=%d pwm=%u Hz min=%u | rawclk=%u kHz | DDI_BUF_CTL at connector init 0x%08x -> saved bits 0x%x\n",
	    config->vbt_low_vswing,
	    config->vbt_hobl,
	    config->afc_override,
	    config->vbt_backlight_present,
	    config->vbt_backlight_active_low,
	    config->vbt_backlight_controller,
	    config->vbt_backlight_hz,
	    config->vbt_backlight_min,
	    config->rawclk_khz,
	    edp->ddi_buf_ctl_readout,
	    config->kept);
	kern_logf("i915: resident display: inputs: wm levels=%u latency %u/%u/%u/%u/%u/%u/%u/%u us ipc=%d sagv_block=%u us | DBUF slices now 0x%x MBUS_CTL=0x%08x joined=%d | CDCLK now %u kHz vco %u ref %u bypass %u level %u max %u | QGV mask 0x%x -> allowed 0x%x bw %u MB/s | DMC programs 0x%x\n",
	    config->wm_levels,
	    config->wm_latency[0],
	    config->wm_latency[1],
	    config->wm_latency[2],
	    config->wm_latency[3],
	    config->wm_latency[4],
	    config->wm_latency[5],
	    config->wm_latency[6],
	    config->wm_latency[7],
	    config->wm_ipc,
	    config->sagv_block_us,
	    config->dbuf_slices_now,
	    mbus,
	    config->mbus_joined_now,
	    config->cdclk.cdclk_khz,
	    config->cdclk.vco_khz,
	    config->cdclk.ref_khz,
	    config->cdclk.bypass_khz,
	    config->cdclk.voltage,
	    config->cdclk_max_khz,
	    (unsigned)d->bandwidth->restriction,
	    allowed,
	    config->qgv_mbps,
	    config->dmc_programs);

	/* Succeeded: the panel's configuration is complete. */
	return 0;
}

/*
 * Turns the panel's configuration into the HDMI display's: DDI B, pipe
 * and transcoder B, DPLL 0, the port's kept DDI_BUF_CTL bits, no
 * backlight, the VBT child's HDMI level, and the output's mode and link.
 */
static void
resident_config_hdmi(
	struct i915_lcd_kernel *k,
	const struct i915_display_output *output,
	struct i915_xd_picture_config *config)
{
	const struct i915_vbt_encoder *encoder;
	const struct i915_lcd_kernel_deps *d;
	uint32_t buf_ctl;

	/* The objects the run reads. */
	d = k->d;

	/* The output's port, pipe and transcoder, DPLL 0, and the port's reversal and lane bits the firmware left. */
	buf_ctl = drv_i915_read32(d->mmio, I915_XD_DDI_BUF_CTL((uint32_t)output->port));
	config->kind = I915_XD_SCREEN_HDMI;
	config->ddi = (unsigned)output->port;
	config->pipe = output->pipe;
	config->trans = output->pipe;
	config->pll = 0U;
	config->aux = (unsigned)output->port;
	config->kept = buf_ctl & I915_RESIDENT_SAVED_PORT_BITS;

	/* An external display has no backlight of ours. */
	config->vbt_backlight_present = 0;

	/* The VBT child of the port: its HDMI level, or the table's default entry when it names none. */
	encoder = drv_i915_vbt_encoder_for_port(&d->edp->vbt->parsed, output->port);
	config->hdmi_level = -1;
	if (encoder != NULL)
		config->hdmi_level = encoder->hdmi_level_shift;

	/* The output's mode and link. */
	resident_config_link(&output->mode, &output->link, config);

	/* Logs what the run drives. */
	kern_logf("i915: resident display: HDMI on port %u, pipe %u, DVI mode: %ux%u %d kHz | PLL cfgcr0=0x%08x cfgcr1=0x%08x | VBT level shift %d | saved DDI_BUF_CTL bits 0x%x\n",
	    config->ddi,
	    config->pipe,
	    output->mode.hdisplay,
	    output->mode.vdisplay,
	    output->mode.clock_khz,
	    output->link.cfgcr0,
	    output->link.cfgcr1,
	    config->hdmi_level,
	    config->kept);
}

/*
 * Turns the panel's configuration into an external DP display's: the
 * Type-C port's DDI, pipe and transcoder B, the port's TC PLL, its AUX
 * channel and its own sink hooks, the sink's capabilities from the
 * claim's probe, no lane reversal, no panel and no backlight, and the
 * output's mode and link.
 */
static void
resident_config_dp_ext(
	struct i915_lcd_kernel *k,
	const struct i915_display_output *output,
	struct i915_xd_picture_config *config)
{
	struct i915_display *display;

	/* The display the run belongs to. */
	display = k->display;

	/* The Type-C port's DDI, the output's pipe and transcoder, the port's TC PLL. */
	config->kind = I915_XD_SCREEN_DP;
	config->ddi = (unsigned)output->port;
	config->pipe = output->pipe;
	config->trans = output->pipe;
	config->pll = I915_XD_PICTURE_PLL_FIRST_TC + output->tc_port;
	config->kept = 0U;

	/* The port's AUX channel, and the sink reached over it. */
	config->aux = drv_i915_tc_port_aux(display, output->tc_port);
	config->sink = drv_i915_dp_ext_aux_emit(display, output->port);
	kern_memcpy(config->caps, output->sink.dpcd, sizeof(config->caps));
	kern_memset(config->edp_caps, 0, sizeof(config->edp_caps));

	/* An external display has no panel power sequencer and no backlight of ours. */
	config->vbt_low_vswing = 0;
	config->vbt_hobl = 0;
	config->vbt_backlight_present = 0;
	config->hdmi_level = -1;

	/* The output's mode and link. */
	resident_config_link(&output->mode, &output->link, config);

	/* Logs what the run drives. */
	kern_logf("i915: resident display: DP on TC%u (port %u), pipe %u: %ux%u %d kHz | link %u kHz x%u %u bpp | TC PLL %u | DPCD rev 0x%02x branch %d\n",
	    output->tc_port + 1U,
	    config->ddi,
	    config->pipe,
	    output->mode.hdisplay,
	    output->mode.vdisplay,
	    output->mode.clock_khz,
	    output->link.rate_khz,
	    output->link.lanes,
	    output->link.bpp,
	    config->pll,
	    config->caps[0],
	    output->sink.branch);
}

/* Copies an output's mode and planned link into a screen's configuration. */
static void
resident_config_link(
	const struct i915_display_mode *mode,
	const struct i915_display_link *link,
	struct i915_xd_picture_config *config)
{
	/* The mode, the link and its M/N. */
	config->mode = *mode;
	config->rate_khz = link->rate_khz;
	config->lanes = link->lanes;
	config->bpp = link->bpp;
	config->m_n.tu = link->tu;
	config->m_n.data_m = link->data_m;
	config->m_n.data_n = link->data_n;
	config->m_n.link_m = link->link_m;
	config->m_n.link_n = link->link_n;

	/* The reference clock and a combo PLL's words. */
	config->pll_ref_khz = link->ref_khz;
	config->pll_words.cfgcr0 = link->cfgcr0;
	config->pll_words.cfgcr1 = link->cfgcr1;
	config->pll_words.div0 = link->div0;
}

/*
 * The picture's window of the resident run: the serve loop runs until the
 * display is to be given back, then the output goes back to buffer A, the
 * one the show began with and ends with.
 */
static int
resident_window(
	void *ctx,
	struct i915_lcd_observer *o)
{
	struct i915_lcd_kernel *k;
	struct i915_display *display;
	int flip_error;

	UNUSED_PARAMETER(o);

	/* The run and its display. */
	k = ctx;
	display = k->display;

	/* The buffers are the GPU's to draw into from here on. */
	display->resident_up = 1;
	kern_logf("i915: resident display: picture up (buffer A surf 0x%08x); serving presentation\n", (uint32_t)display->resident_buf[0].surf);

	/* Serves until the display is to be given back. */
	(void)display->resident_serve(display->resident_serve_ctx);

	/* A flip armed without waiting completes before the buffers change hands (it logs a failure). */
	(void)drv_i915_xd_picture_flip_settle(&display->xd.pictures, I915_XD_PICTURE_RESIDENT);

	/*
	 * Lit again for two pipes next: buffer A, which the next lighting
	 * shows first, takes the picture B shows, so the output comes back
	 * with its last picture and not an older one.  The GPU wrote B through
	 * an uncached mapping, so the CPU's stale lines go first.
	 */
	if (display->window.relight && display->resident_front != 0U) {
		drv_i915_gt_clflush(display->resident_buf[1].cpu, display->resident_buf[1].size);
		kern_memcpy(display->resident_buf[0].cpu, display->resident_buf[1].cpu, display->resident_buf[0].size);
		drv_i915_scanout_publish(&display->resident_buf[0]);
	}

	/* Ends on buffer A; the flip back is only armed, so it is settled too. */
	if (display->resident_front != 0U) {
		flip_error = drv_i915_lcd_resident_flip(display);
		if (flip_error != 0)
			kern_logf("i915: resident display: could not flip back to buffer A before the stop\n");

		/* Waits for the flip back to settle. */
		(void)drv_i915_xd_picture_flip_settle(&display->xd.pictures, I915_XD_PICTURE_RESIDENT);
	}

	/* The buffers are not the GPU's any more. */
	display->resident_up = 0;
	kern_logf("i915: resident display: released after %u flip(s); the stop follows\n", k->flips_done);

	/* The window passed. */
	return 0;
}

/*
 * Creates and pins both resident buffers at the output's size, black
 * outside the application's image, and publishes them.  A failure unpins
 * and destroys whatever was made.
 */
static int
resident_buffers(
	struct i915_display *display,
	const struct i915_lcd_kernel_deps *d,
	const struct i915_display_mode *mode)
{
	struct i915_scanout *a;
	struct i915_scanout *b;
	int window_error;
	int error;
	int i;

	/* The two buffers. */
	a = &display->resident_buf[0];
	b = &display->resident_buf[1];

	/* The display window of the GGTT; EBUSY means it was claimed earlier, which is fine. */
	window_error = drv_i915_gt_display_window_init(d->gm, I915_GT_DISPLAY_PAGES);
	if (window_error != 0 && window_error != EBUSY)
		return window_error;

	/* Creates and pins A at the output's size, then B at A's size. */
	error = drv_i915_scanout_create(d->gm, mode->hdisplay, mode->vdisplay, I915_FOURCC_XRGB8888, I915_MOD_LINEAR, a);
	if (error == 0)
		error = drv_i915_scanout_pin(a, "resident A");
	if (error == 0)
		error = drv_i915_scanout_create(d->gm, a->width, a->height, I915_FOURCC_XRGB8888, I915_MOD_LINEAR, b);
	if (error == 0)
		error = drv_i915_scanout_pin(b, "resident B");

	/* Gives back whatever was made, B first. */
	if (error != 0) {
		kern_logf("i915: resident display: not started (buffers rc=%d)\n", error);
		for (i = 1; i >= 0; i--) {
			(void)drv_i915_scanout_unpin(&display->resident_buf[i]);
			(void)drv_i915_scanout_destroy(&display->resident_buf[i]);
		}

		/* Reports the failure. */
		return error;
	}

	/* Black outside the application's image, visible to the display. */
	for (i = 0; i < 2; i++) {
		kern_memset(display->resident_buf[i].cpu, 0, display->resident_buf[i].size);
		drv_i915_scanout_publish(&display->resident_buf[i]);
	}

	/* Names both buffers: the kernel addresses let a debugger or a virtual machine's monitor read the picture. */
	kern_logf("i915: resident display: buffers %ux%u pitch %u: A surf 0x%08x cpu %p, B surf 0x%08x cpu %p\n",
	    a->width,
	    a->height,
	    a->pitch,
	    (uint32_t)a->surf,
	    (void *)a->cpu,
	    (uint32_t)b->surf,
	    (void *)b->cpu);

	/* Succeeded: both buffers are pinned and black. */
	return 0;
}

/*
 * Tells whether an earlier run kept both resident buffers pinned for this
 * one (the resident output lit again for two pipes, with its picture).  A
 * keep that does not find both buffers pinned is forgotten.
 */
static int
resident_buffers_kept(
	struct i915_display *display)
{
	/* Nothing was kept. */
	if (!display->window.keep_buffers)
		return 0;

	/* Both must be pinned and unused, as the earlier run left them. */
	if (display->resident_buf[0].state != I915_SCANOUT_PINNED ||
	    display->resident_buf[1].state != I915_SCANOUT_PINNED) {
		display->window.keep_buffers = 0;
		return 0;
	}

	/* Succeeded: the buffers are this run's. */
	return 1;
}

/*
 * Releases both resident buffers when the display provably reads neither,
 * or abandons them when it may.  Reports whether both were released.
 */
static int
resident_release(
	struct i915_display *display,
	const struct i915_lcd_show_report *rep)
{
	struct i915_scanout *so;
	int error;
	int i;

	/* The display may still read the buffers: both are kept for ever. */
	if (!rep->display_released && rep->display_acquired) {
		for (i = 0; i < 2; i++) {
			so = &display->resident_buf[i];
			if (so->state >= I915_SCANOUT_PINNED && so->state != I915_SCANOUT_ABANDONED)
				drv_i915_scanout_abandon(so);
		}

		/* The buffers are abandoned, not released. */
		return 0;
	}

	/* The show ended A; B's last use ends here. */
	drv_i915_scanout_end(&display->resident_buf[1]);

	/* Kept pinned, with their picture, for the lighting for two pipes that follows. */
	if (display->window.relight && display->window.keep_buffers)
		return 1;

	/* Unpins and destroys A then B, stopping at the first refusal. */
	error = drv_i915_scanout_unpin(&display->resident_buf[0]);
	if (error == 0)
		error = drv_i915_scanout_destroy(&display->resident_buf[0]);
	if (error == 0)
		error = drv_i915_scanout_unpin(&display->resident_buf[1]);
	if (error == 0)
		error = drv_i915_scanout_destroy(&display->resident_buf[1]);
	if (error != 0)
		return 0;

	/* Succeeded: both buffers are gone. */
	return 1;
}

/*
 * Judges the resident run: the show's judgement without the complete run
 * log (a display that flips for as long as an application presents always
 * outgrows it); the counters keep counting past the last kept entry.
 */
static int
resident_passed(
	const struct i915_lcd_show_report *rep)
{
	/* No anomaly, the window and the release. */
	if (rep->first_anomaly != NULL)
		return 0;
	if (rep->window_hook_rc != 0)
		return 0;
	if (!rep->display_released)
		return 0;

	/* The light, the first frames and, on DP, the trained link (HDMI trains none). */
	if (rep->enable_rc != I915_XD_PICTURE_OK)
		return 0;
	if (rep->steady_rc != 0)
		return 0;
	if (!rep->output_hdmi) {
		if (!rep->at_enable.cr_ok || !rep->at_enable.eq_ok)
			return 0;
	}

	/* No underrun and the vblank interrupt never unmasked. */
	if (rep->obs.seen_steady != 0U)
		return 0;
	if (rep->obs.vblank_unmasked_seen != 0)
		return 0;

	/* The run log's counters: no unresolved step, no error, no timed-out wait. */
	if (rep->trace->steps != 0U)
		return 0;
	if (rep->trace->errors != 0U)
		return 0;
	if (rep->trace->wait_timeouts != 0U)
		return 0;

	/* Succeeded: the run passed. */
	return 1;
}

/*
 * Takes over the display the firmware left running: every pipe the
 * firmware check found lit is stopped through the xd takeover, with the
 * start's power reference kept for it and the display's power for the
 * wells of each stopped port.  The PLLs and the display buffer
 * start empty for the run's own light.  The firmware's framebuffer is never
 * written.  Returns 0, or the errno of the pipe that could not be stopped
 * (logged).
 */
static int
resident_takeover(
	struct i915_display *display)
{
	struct i915_xd_takeover_outcome outcome;
	struct i915_xd_takeover_power power;
	int error;

	/* What the firmware left lit. */
	kern_logf("i915: takeover: lit pipes 0x%x, output %s (pipe %c, DDI %d)\n",
	    display->xd.firmware.lit_pipes,
	    drv_i915_xd_firmware_output_name(display->xd.firmware.output),
	    'A' + (int)display->xd.firmware.output_pipe,
	    display->xd.firmware.output_ddi);

	/* Stops every lit pipe, the ports' wells taken through the display's power. */
	power.get = resident_takeover_power_get;
	power.put = resident_takeover_power_put;
	power.context = display;
	error = drv_i915_xd_takeover(&display->xd, &power, &outcome);
	kern_logf("i915: takeover: rc=%d, crtcs stopped %u, still active 0x%x\n",
	    error,
	    (unsigned)__builtin_popcount(outcome.stopped),
	    outcome.still_lit);
	if (error != 0)
		return error;

	/* The run's own light starts from empty PLLs and an empty display buffer. */
	drv_i915_xd_pictures_forget(&display->xd.pictures);

	/* Succeeded: the firmware's display is stopped and the pipes are the driver's. */
	return 0;
}

/* Takes the wells of a takeover's port through the display's power: 0, or the error. */
static int
resident_takeover_power_get(
	void *context,
	uint64_t functions)
{
	struct i915_display *display;
	int error;

	/* The display's power. */
	display = context;
	error = drv_i915_display_power_get(&display->power, functions);
	if (error != 0)
		return error;

	/* Succeeded: the wells are held. */
	return 0;
}

/* Gives the wells of a takeover's port back through the display's power. */
static void
resident_takeover_power_put(
	void *context,
	uint64_t functions)
{
	struct i915_display *display;

	/* The display's power. */
	display = context;
	drv_i915_display_power_put(&display->power, functions);
}

/*
 * Logs the run log of a resident run: whole under i915.debug=display or
 * after a failure (unless the display was unplugged, when it would only
 * delay the output's return), else its counters.
 */
static void
resident_log_run(
	struct i915_display *display,
	const struct i915_lcd_show_report *rep,
	int passed,
	int released,
	int held)
{
	int debug;
	int unplugged;

	/* The switch and whether the display went away. */
	debug = drv_i915_lcd_debug_enabled();
	unplugged = __atomic_load_n(&display->window.unplugged, __ATOMIC_ACQUIRE);

	/* The whole log and the observer, when asked for or when the run failed for its own reasons. */
	if (debug ||
	    ((!passed ||
	      !released ||
	      held != 0) &&
	     !unplugged)) {
		drv_i915_lcd_log_trace(rep->trace);
		drv_i915_lcd_log_observer(&rep->obs);
		return;
	}

	/* A run whose display was unplugged fails as expected. */
	if (unplugged) {
		kern_logf("i915: resident display: run log not printed: its display was unplugged (%u entries kept)\n", rep->trace->n);
		return;
	}

	/* A run that passed: its counters. */
	kern_logf("i915: resident display: run log %u writes, %u rmw, %u waits (0 timed out), 0 errors, 0 unresolved steps (%u entries kept, %u not kept)\n",
	    rep->trace->writes,
	    rep->trace->rmws,
	    rep->trace->waits,
	    rep->trace->n,
	    rep->trace->dropped);
}
