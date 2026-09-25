/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The panel runs of the display scenarios: LCD-B and LCD reuse.
 *
 * LCD-B shows one known picture on the panel through the reference's
 * modeset, keeps it up for a finite observation window and stops it
 * through the reference's stop path; everything the run took is given
 * back.  LCD reuse does that three times in one driver lifetime and, in
 * the first window, checks the pipe's vblank interrupts and drives the
 * brightness and the backlight.  No GPU submission happens in either.
 *
 * Every input of the modeset comes from the objects the device start
 * built (the resident eDP, the VBT, the watermark latencies, the DBUF,
 * CDCLK and QGV state, the DMC); none is a constant of this file and none
 * is a value of Linux's register dump, which is only compared against.
 * Software flags, hardware observation (frame counter, underrun status,
 * sink link status) and the photograph are separate evidence; the verdict
 * lines cover the first two.
 */

#include "lcd-run.h"
#include "scenarios.h"
#include "../../display/clock.h"
#include "../../display/diagnostics.h"
#include "../../display/display.h"
#include "../../display/interrupts.h"
#include "../../display/modeset.h"
#include "../../display/panel-backlight.h"
#include "../../display/vbt-parse.h"
#include "../../display/watermark.h"
#include "../../ggtt.h"
#include "../../i915.h"
#include "../../memory.h"
#include "../../mmio.h"
#include <kern/kcrt.h>

#include <kern/klog.h>

#include <uapi/errno.h>
#include <stddef.h>

/* The frame counter register of pipe A, by its name in the register table. */
#define I915_TEST_LCD_FRAME_REG		"PIPE_FRMCOUNT_G4X"

/* One step of the scenario's own sleep. */
#define I915_TEST_LCD_SLEEP_STEP_US	100000U

/* GEN8_DE_PIPE_IMR(A) and GEN8_DE_PIPE_IER(A). */
#define I915_TEST_LCD_PIPE_A_IMR	0x44404U
#define I915_TEST_LCD_PIPE_A_IER	0x4440cU

/* The vblank bit of the pipe interrupt registers. */
#define I915_TEST_LCD_VBLANK_BIT	0x00000001U

/* The enable bit of the backlight PWM control register. */
#define I915_TEST_LCD_PWM_ENABLE	0x80000000U

/* How long the vblank waits of LCD reuse may take, and how many vblanks each needs. */
#define I915_TEST_LCDR_VBLANK_WAIT_MS	100U
#define I915_TEST_LCDR_VBLANK_WAITS	3U

/* How long the interrupt drain after the last vblank reference may take. */
#define I915_TEST_LCDR_DRAIN_US		10000U

/*
 * Whether LCD-B probes what the plane really fetches once the picture is
 * up (a diagnostic of the test build: 0 or 1, given with the scenario).
 */
#ifndef I915_TEST_LCDB_SCANOUT_PROBE
#define I915_TEST_LCDB_SCANOUT_PROBE	0
#endif

/* How many samples the scanout probe takes, and how far apart. */
#define I915_TEST_PROBE_SAMPLES		30U
#define I915_TEST_PROBE_STEP_US		300000U

/* PIPE_CRC_CTL(A): enabled with plane 1 as the source. */
#define I915_TEST_PIPE_CRC_CTL_A	0x60050U
#define I915_TEST_PIPE_CRC_PLANE1	0x80000000U

/* The three show / stop cycles of LCD reuse. */
#define I915_TEST_LCDR_CYCLES		3U

/* The brightness steps of the first LCD reuse window, and how long each holds. */
#define I915_TEST_LCDR_STEPS		6U
#define I915_TEST_LCDR_HOLD_MS		7000U
#define I915_TEST_LCDR_SHORT_HOLD_MS	1000U

/*
 * What one brightness step of LCD reuse asks for.
 *
 * op is 0 for a user level (level of the user range), -1 to switch the
 * backlight off with the scanout running, and 1 to switch it on again.
 */
struct i915_test_lcdr_step {
	/* The name the step is logged under. */
	const char *name;

	/* The operation: 0 a level, -1 backlight off, 1 backlight on. */
	int op;

	/* The level as numerator and denominator of the user range (op 0 only). */
	unsigned level_num;
	unsigned level_den;

	/* How long the step holds for the camera. */
	unsigned hold_ms;
};

/*
 * The show environment and report of a scenario's run.
 *
 * Too large for the stack; one run at a time uses them (the scenarios run
 * serially on the start worker).  They are rewritten by each run.
 */
static struct i915_lcd_show_env i915_test_lcd_env;
static struct i915_lcd_show_report i915_test_lcd_report;

/*
 * The scanout storage of the one-picture runs.
 *
 * It outlives a run whose stop was not confirmed: the buffer is then
 * abandoned in it and the next run finds the storage in use and refuses.
 */
static struct i915_scanout i915_test_lcd_scanout;

/*
 * The brightness steps of the first LCD reuse window.
 *
 * The last one goes back to half before the backlight is switched off
 * and on again; the user level found at the start is restored after them.
 */
static const struct i915_test_lcdr_step i915_test_lcdr_steps[I915_TEST_LCDR_STEPS] = {
	{ "max", 0, 1U, 1U, I915_TEST_LCDR_HOLD_MS },
	{ "half", 0, 1U, 2U, I915_TEST_LCDR_HOLD_MS },
	{ "min(user 0)", 0, 0U, 1U, I915_TEST_LCDR_HOLD_MS },
	{ "half-again", 0, 1U, 2U, I915_TEST_LCDR_SHORT_HOLD_MS },
	{ "backlight-off(scanout continues)", -1, 0U, 1U, I915_TEST_LCDR_HOLD_MS },
	{ "backlight-on", 1, 0U, 1U, I915_TEST_LCDR_HOLD_MS },
};

static void i915_test_lcd_log_run(const struct i915_lcd_kernel *k, const struct i915_lcd_run_params *params, const struct i915_lcd_show_report *rep, int held);
static int i915_test_lcd_irq_hooks_ran(const struct i915_lcd_kernel *k);
static void i915_test_lcd_at_stage(void *ctx, int stage);
static void i915_test_lcd_probe_scanout(struct i915_lcd_kernel *k);
static void i915_test_lcd_log_regs(const struct i915_lcd_kernel *k, const char *what, const char *const *names, const uint32_t *regs, unsigned count);
static int i915_test_lcdr_irq_check(struct i915_lcd_kernel *k);
static uint32_t i915_test_lcdr_expected_duty(const struct i915_lcd_modeset_status *status, uint32_t user);
static int i915_test_lcdr_brightness(struct i915_lcd_kernel *k, unsigned step, const char *name, int op, uint32_t level, uint32_t max, unsigned hold_ms);
static int i915_test_lcdr_window_first(void *ctx, struct i915_lcd_observer *observer);
static int i915_test_lcdr_window_again(void *ctx, struct i915_lcd_observer *observer);

/*
 * Returns the display a scenario drives.
 *
 * A device without a display, or one the firmware display check left
 * without it, has nothing to show on: the scenario fails with that reason
 * and NULL is returned.  The panel dependencies are filled when the start
 * has not filled them yet.
 */
struct i915_display *
drv_i915_test_lcd_display(
	struct i915_device *device,
	const char *tag)
{
	struct i915_display *display;

	/* A device without a display has no panel. */
	display = device->display;
	if (display == NULL || display->absent) {
		kern_logf("i915: %s verdict: FAIL (the device has no display)\n", tag);
		return NULL;
	}

	/* The panel dependencies of the runs, over the objects the start built. */
	if (display->rlcd.edp == NULL)
		drv_i915_display_resident_deps(device);

	/* The runs need the resident eDP and the GT memory. */
	if (display->rlcd.edp == NULL || display->rlcd.gm == NULL) {
		kern_logf("i915: %s verdict: FAIL (a dependency is missing)\n", tag);
		return NULL;
	}

	/* Succeeded: the display has a panel to drive. */
	return display;
}

/*
 * Starts a panel run over the display's objects.
 *
 * Creates the two modeset locks once, clears the run and binds its hooks;
 * the run is display->lk, the only storage the hooks accept.
 */
struct i915_lcd_kernel *
drv_i915_test_lcd_start(
	struct i915_display *display,
	const struct i915_lcd_run_params *params)
{
	struct i915_lcd_kernel *k;

	/* The two modeset mutexes live as long as the device. */
	drv_i915_lcd_kernel_locks_init(display);

	/* A fresh run over the panel dependencies of the start. */
	k = &display->lk;
	kern_memset(k, 0, sizeof(*k));
	k->locks = display->lcdb_locks;
	k->d = &display->rlcd;
	drv_i915_lcd_kernel_bind_ops(k);
	k->p = params;

	/* Succeeded: the run is ready for its preflight. */
	return k;
}

/*
 * Reports whether an earlier run kept resources the display may still
 * read: a stop that was not confirmed, in the show body or the modeset.
 */
int
drv_i915_test_lcd_retained(
	struct i915_display *display)
{
	int retained;

	/* The show body's latch. */
	retained = drv_i915_lcd_show_retained(display);
	if (retained)
		return 1;

	/* The modeset object's latch. */
	retained = drv_i915_lcd_modeset_retained(display);
	if (retained)
		return 1;

	/* Nothing is retained. */
	return 0;
}

/*
 * Shows one picture through the show body and stops it.
 *
 * The run refuses before anything is initialised when an earlier run's
 * resources are retained, and writes nothing when the preflight finds the
 * display not idle.  Returns 0 when the run passed; EBUSY or EIO when it
 * was refused or did not pass.
 */
int
drv_i915_test_lcd_run_one(
	struct i915_display *display,
	const struct i915_lcd_run_params *params)
{
	const struct i915_vbt_encoder *encoder;
	const struct i915_lcd_kernel_deps *d;
	struct i915_lcd_show_report *rep;
	struct i915_lcd_show_env *env;
	struct i915_lcd_kernel *k;
	int retained;
	int preflight_error;
	int fill_error;
	int held;

	env = &i915_test_lcd_env;
	rep = &i915_test_lcd_report;
	d = &display->rlcd;

	/* An earlier run left a buffer the display may still read: refused before any initialisation. */
	retained = drv_i915_test_lcd_retained(display);
	if (retained) {
		kern_logf("i915: LCD-B verdict: FAIL (refused before any initialisation: an earlier run's resources are retained)\n");
		return EBUSY;
	}

	/* The run over the start's objects. */
	k = drv_i915_test_lcd_start(display, params);

	/* A panel run owns the device's PLL pool and its DBUF state; an HDMI run only when it starts alone. */
	if (!params->output_hdmi || params->reset_dplls) {
		drv_i915_lcd_dplls_reset(display->lcd_world);
		drv_i915_lcd_dbuf_forget(display->wm_world);
	}

	/* The hardware must be as the initialisation left it, and the inputs complete. */
	kern_memset(env, 0, sizeof(*env));
	preflight_error = drv_i915_lcd_kernel_preflight(k);
	fill_error = 0;
	if (preflight_error == 0)
		fill_error = drv_i915_lcd_kernel_fill_cfg(k, &env->cfg);
	if (preflight_error != 0 || fill_error != 0) {
		kern_logf("i915: LCD-B verdict: FAIL (preflight: nothing was written to the display)\n");
		return EBUSY;
	}

	/* The show environment: the panel's state, or the caller's HDMI state. */
	env->hw = &k->ops;
	env->gm = d->gm;
	env->so = &i915_test_lcd_scanout;
	env->lcd = &d->edp->lcd;
	env->pipe = params->pipe;
	if (params->output_hdmi)
		env->lcd = params->state;

	/* What intel_ddi_init() would have left for the HDMI encoder of the port. */
	if (params->output_hdmi) {
		env->cfg.output_hdmi = 1;
		env->cfg.port = params->port;
		env->cfg.pipe = params->pipe;
		env->cfg.cpu_transcoder = params->cpu_transcoder;
		env->cfg.dpll_id = params->dpll_id;
		env->cfg.aux_ch = params->port;
		env->cfg.saved_port_bits = drv_i915_read32(d->mmio, I915_TEST_HDMI_DDI_BUF_CTL_B) & I915_TEST_SAVED_PORT_BITS;
		env->cfg.vbt_backlight_present = 0;

		/* The VBT child of the port: its HDMI level shift, when it names one. */
		encoder = drv_i915_vbt_encoder_for_port(&d->edp->vbt->parsed, params->port);
		env->cfg.vbt_hdmi_level_shift = -1;
		if (encoder != NULL)
			env->cfg.vbt_hdmi_level_shift = encoder->hdmi_level_shift;

		kern_logf("i915: HDMI-B input: VBT child for port %d: hdmi_level_shift=%d (< 0 = not in the VBT: the buffer-translation table's default entry is used) hdmi_boost=%d ddc_pin=%d\n",
		    params->port,
		    env->cfg.vbt_hdmi_level_shift,
		    encoder != NULL ? encoder->hdmi_boost_level : -1,
		    encoder != NULL ? encoder->ddc_pin : -1);
		kern_logf("i915: HDMI-B input: mode %ux%u %d kHz | port=%d pipe=%d transcoder=%d DPLL%d | PLL cfgcr0=0x%08x cfgcr1=0x%08x div0=0x%08x | saved DDI_BUF_CTL bits 0x%x\n",
		    params->state->mode.hdisplay,
		    params->state->mode.vdisplay,
		    params->state->mode.clock_khz,
		    params->port,
		    params->pipe,
		    params->cpu_transcoder,
		    params->dpll_id,
		    params->state->pll.cfgcr0,
		    params->state->pll.cfgcr1,
		    params->state->pll.div0,
		    env->cfg.saved_port_bits);
	}

	/* The picture; its pinned hash is a 1920x1080 measurement and says nothing on another panel. */
	env->pattern_id = params->pattern_id;
	env->pattern_fnv = params->pattern_fnv;
	if (params->pattern_fnv != 0ULL &&
	    (env->lcd->mode.hdisplay != 1920U ||
	     env->lcd->mode.vdisplay != 1080U)) {
		kern_logf("i915: LCD-B picture: the pinned hash is a 1920x1080 measurement and this panel is %ux%u -- the read-back check runs, the pin is not compared\n",
		    env->lcd->mode.hdisplay,
		    env->lcd->mode.vdisplay);
		env->pattern_fnv = 0ULL;
	}

	/* The window, the hook that runs in it, and the stage hook of the log. */
	env->first_frames_ms = I915_TEST_LCD_FIRST_FRAMES_MS;
	env->window_ms = params->window_ms;
	env->in_window = params->in_window;
	env->in_window_ctx = k;
	env->at_stage = i915_test_lcd_at_stage;
	env->at_stage_ctx = k;
	k->pattern_id = params->pattern_id;
	k->window_ms = params->window_ms;

	/* The power-well interrupt hooks counted so far: the run must add to both. */
	if (d->irq->vbl != NULL) {
		k->post_before = d->irq->vbl->post_enable_calls;
		k->pre_before = d->irq->vbl->pre_disable_calls;
	}

	/* Shows the picture, runs the window and stops through the reference's path. */
	(void)drv_i915_lcd_show_run(display, env, rep);

	/* What the run still holds. */
	held = drv_i915_test_lcd_power_held(k);

	/* Logs the run log, the observer, the states and the buffer. */
	i915_test_lcd_log_run(k, params, rep, held);

	/* A run that holds power, left a step unresolved or saw the time base fail did not pass. */
	if (rep->pass && (held != 0 || k->unresolved_steps != 0U || k->time_faults != 0U))
		rep->pass = 0;

	/* The pipe's well went on (its interrupt registers restored) and off again (stopped). */
	if (rep->pass && !i915_test_lcd_irq_hooks_ran(k))
		rep->pass = 0;

	kern_logf("i915: LCD-B verdict: %s (furthest stage=%s; software flags, hardware observation and the photograph are separate evidence: this line covers the first two)\n",
	    rep->pass ? "PASS" : "FAIL",
	    drv_i915_test_lcd_stage_name(rep->stage));

	/* Reports a run that did not pass. */
	if (!rep->pass)
		return EIO;

	/* Succeeded: the picture was up, stopped and given back. */
	return 0;
}

/*
 * Sleeps in steps of 100 ms on the run's sleep hook, the resident eDP's
 * tick sleep, which also watches the time base.
 */
void
drv_i915_test_lcd_sleep_ms(
	struct i915_lcd_kernel *k,
	unsigned ms)
{
	unsigned slept;

	/* Sleeps one step at a time until the time is used up. */
	for (slept = 0U; slept < ms; slept += I915_TEST_LCD_SLEEP_STEP_US / 1000U)
		k->ops.usleep(k->ops.ctx, I915_TEST_LCD_SLEEP_STEP_US);
}

/*
 * Reads the frame counter of pipe A.
 *
 * ctx is the run; the register comes from the display's register table.
 */
uint32_t
drv_i915_test_lcd_frame(
	void *ctx)
{
	struct i915_lcd_kernel *k;
	struct i915_display *display;
	uint32_t reg;
	uint32_t frame;

	k = ctx;
	display = container_of(k, struct i915_display, lk);

	/* Finds the counter and reads it. */
	reg = drv_i915_lcd_reg_by_name(display->lcd_world, I915_TEST_LCD_FRAME_REG);
	frame = drv_i915_read32(k->d->mmio, reg);

	/* Reports the frame. */
	return frame;
}

/*
 * Counts the power references the run took and has not returned.
 */
int
drv_i915_test_lcd_power_held(
	const struct i915_lcd_kernel *k)
{
	unsigned domain;
	int held;

	/* Adds the references of every domain. */
	held = 0;
	for (domain = 0U; domain < I915_PW_DOMAIN_NUM; domain++)
		held += k->power_refs[domain];

	/* Reports the sum. */
	return held;
}

/*
 * Names a stage of the show body.
 */
const char *
drv_i915_test_lcd_stage_name(
	int stage)
{
	static const char *const names[] = {
		"none",
		"buffer-ready",
		"prepared",
		"enable-returned",
		"picture-up",
		"window-done",
		"disable-returned",
		"stop-confirmed",
		"released",
		"ABANDONED",
	};

	/* A value outside the stages. */
	if (stage < 0 || stage > I915_LCD_SHOW_ABANDONED)
		return "?";

	/* Reports the stage's name. */
	return names[stage];
}

/*
 * Names the result of a flip.
 */
const char *
drv_i915_test_lcd_flip_name(
	int result)
{
	/* One name per I915_LCD_FLIP_* result. */
	switch (result) {
	case I915_LCD_FLIP_DONE:
		return "DONE";
	case I915_LCD_FLIP_NOT_LATCHED:
		return "NOT-LATCHED";
	case I915_LCD_FLIP_TIMEOUT:
		return "TIMEOUT";
	default:
		break;
	}

	/* Anything else was refused. */
	return "REFUSED";
}

/*
 * Runs LCD-B: one known picture on the panel, the observation window and
 * the reference's stop path.
 */
void
drv_i915_test_display_lcdb(
	struct i915_device *device)
{
	static const struct i915_lcd_run_params params = {
		.pattern_id = I915_TEST_LCDB_PATTERN_ID,
		.pattern_fnv = I915_TEST_LCDB_PATTERN_FNV,
		.window_ms = I915_TEST_LCDB_WINDOW_MS,
	};
	struct i915_display *display;

	/* The display to show on. */
	display = drv_i915_test_lcd_display(device, "LCD-B");
	if (display == NULL)
		return;

	/* One picture; the verdict line is the run's. */
	(void)drv_i915_test_lcd_run_one(display, &params);
}

/*
 * Reports that N1 does not run in this driver.
 *
 * N1 took over the display the firmware left running (readout, takeover,
 * re-light).  The device start now refuses such a display before any
 * scenario runs, and keeping it alive through the start would need a
 * production switch; the scenario only says so.
 */
void
drv_i915_test_display_n1(
	struct i915_device *device)
{
	UNUSED_PARAMETER(device);

	kern_logf("i915: N1 verdict: NOT RUN (the device start refuses a display the firmware left running; the takeover needs a production switch the test build may not add)\n");
}

/*
 * Runs LCD reuse: three show / stop cycles in one driver lifetime, the
 * pipe's vblank interrupts in every window, and the brightness and the
 * backlight in the first one.
 *
 * One display owner runs them serially: the modeset object is global and
 * the DPLL and backlight mutexes do not make the whole modeset reentrant.
 * The first cycle that fails ends the scenario.
 */
void
drv_i915_test_display_lcdr(
	struct i915_device *device)
{
	static const struct i915_lcd_run_params cycles[I915_TEST_LCDR_CYCLES] = {
		{ .pattern_id = 110U, .pattern_fnv = I915_TEST_LCDB_PATTERN_FNV, .window_ms = 2000U, .in_window = i915_test_lcdr_window_first },
		{ .pattern_id = 111U, .window_ms = 3000U, .in_window = i915_test_lcdr_window_again },
		{ .pattern_id = 112U, .window_ms = 3000U, .in_window = i915_test_lcdr_window_again },
	};
	struct i915_display *display;
	unsigned passed;
	unsigned cycle;
	int error;

	/* The display to show on. */
	display = drv_i915_test_lcd_display(device, "LCD-R");
	if (display == NULL)
		return;

	kern_logf("i915: LCD-R begin: three show / stop cycles in one driver lifetime (patterns 110, 111, 112); one display owner, run serially (the modeset object is global; the DPLL / backlight mutexes do not make the whole modeset reentrant)\n");

	/* Runs the cycles until the first one fails. */
	passed = 0U;
	for (cycle = 0U; cycle < I915_TEST_LCDR_CYCLES; cycle++) {
		kern_logf("i915: LCD-R cycle %u begin (pattern %u)\n",
		    cycle + 1U,
		    cycles[cycle].pattern_id);

		/* One show / stop cycle. */
		error = drv_i915_test_lcd_run_one(display, &cycles[cycle]);
		kern_logf("i915: LCD-R cycle %u verdict: %s\n",
		    cycle + 1U,
		    error == 0 ? "PASS" : "FAIL");
		if (error != 0)
			break;

		passed++;
	}

	kern_logf("i915: LCD-R verdict: %s (cycles passed %u/3; photographs are separate evidence)\n",
	    passed == I915_TEST_LCDR_CYCLES ? "PASS" : "FAIL",
	    passed);
}

/* Logs what a one-picture run did: its log, observer, states, buffer, anomaly and backend. */
static void
i915_test_lcd_log_run(
	const struct i915_lcd_kernel *k,
	const struct i915_lcd_run_params *params,
	const struct i915_lcd_show_report *rep,
	int held)
{
	const struct i915_lcd_kernel_deps *d;
	const char *first_anomaly;

	d = k->d;

	/* The run log, the observer and the states the commits left. */
	drv_i915_lcd_log_trace(rep->trace);
	drv_i915_lcd_log_observer(&rep->obs);
	drv_i915_lcd_log_status("enable-returned", &rep->at_enable);
	if (rep->stage >= I915_LCD_SHOW_WINDOW_DONE)
		drv_i915_lcd_log_status("window-end", &rep->at_window_end);

	drv_i915_lcd_log_status("disable-returned", &rep->at_disable);

	/* The buffer and what became of it. */
	kern_logf("i915: LCD-B buffer: surf=0x%08llx pattern id=%u fnv=%016llx (pinned %016llx) readback_bad before=%u after=%u | released=%d abandoned=%d unpin=%d destroy=%d | display pages in use=%u\n",
	    (unsigned long long)rep->surf,
	    params->pattern_id,
	    (unsigned long long)rep->pattern_hash,
	    (unsigned long long)params->pattern_fnv,
	    rep->readback_bad_before,
	    rep->readback_bad_after,
	    rep->released,
	    rep->abandoned,
	    rep->unpin_rc,
	    rep->destroy_rc,
	    d->gm->display_allocated_pages);

	/* The hardware observation: first frames, steady rounds, the stop. */
	kern_logf("i915: LCD-B hardware observation: first frames rc=%d counter %u -> %u | steady rounds=%u rc=%d counter %u -> %u | after stop rc=%d counter %u -> %u TRANSCONF=0x%08x\n",
	    rep->first_frames_rc,
	    rep->frame_first,
	    rep->frame_last,
	    rep->steady_rounds,
	    rep->steady_rc,
	    rep->steady_frame_first,
	    rep->steady_frame_last,
	    rep->stopped_rc,
	    rep->stop_frame_first,
	    rep->stop_frame_last,
	    rep->transconf_after_stop);

	/* The first anomaly and the return codes in order. */
	first_anomaly = "none";
	if (rep->first_anomaly != NULL)
		first_anomaly = rep->first_anomaly;

	kern_logf("i915: LCD-B first anomaly: %s (stage=%s rc=%d run-log index=%d) | bring-up errors=%u | cleanup errors=%u (first at run-log index %d) | rcs: window=%d create=%d pin=%d prepare=%d begin=%d enable=%d disable=%d\n",
	    first_anomaly,
	    drv_i915_test_lcd_stage_name(rep->first_anomaly_stage),
	    rep->first_anomaly_rc,
	    rep->first_error_trace_at,
	    rep->enable_errors,
	    rep->cleanup_errors,
	    rep->cleanup_first_error_trace_at,
	    rep->window_rc,
	    rep->create_rc,
	    rep->pin_rc,
	    rep->prepare_rc,
	    rep->begin_rc,
	    rep->enable_rc,
	    rep->disable_rc);

	/* The backend's own counts. */
	kern_logf("i915: LCD-B backend: power refs of this run still held=%d get_failures=%u | wait timeouts=%u time faults=%u | unresolved steps=%u decided=%u | eDP tick sleeps=%u\n",
	    held,
	    k->power_get_failures,
	    k->wait_timeouts,
	    k->time_faults,
	    k->unresolved_steps,
	    k->decided,
	    d->edp->k.tick_sleeps);

	/* The power-well interrupt hooks of the run. */
	if (d->irq->vbl != NULL) {
		kern_logf("i915: LCD-B power-well IRQ hooks during this run: post_enable +%u pre_disable +%u (sync calls %u, sync timeouts %u) | pipe A IMR now 0x%08x (well off reads 0)\n",
		    d->irq->vbl->post_enable_calls - k->post_before,
		    d->irq->vbl->pre_disable_calls - k->pre_before,
		    d->irq->vbl->sync_calls,
		    d->irq->vbl->sync_timeouts,
		    drv_i915_read32(d->mmio, I915_TEST_LCD_PIPE_A_IMR));
	}
}

/* Reports whether the pipe's well went on and off again during the run with no sync timeout. */
static int
i915_test_lcd_irq_hooks_ran(
	const struct i915_lcd_kernel *k)
{
	const struct i915_irq_vblank *vbl;

	vbl = k->d->irq->vbl;

	/* Without the vblank delivery there are no hooks to judge. */
	if (vbl == NULL)
		return 1;

	/* The post-enable hook restored the pipe's interrupt registers. */
	if (vbl->post_enable_calls == k->post_before)
		return 0;

	/* The pre-disable hook stopped them. */
	if (vbl->pre_disable_calls == k->pre_before)
		return 0;

	/* No interrupt sync timed out. */
	if (vbl->sync_timeouts != 0U)
		return 0;

	/* Both hooks ran. */
	return 1;
}

/*
 * The stage hook of a one-picture run: the run's own (the cleanup phase
 * and the registers next to Linux's dump), then the scanout probe once
 * the picture is up when the test build asks for it.
 */
static void
i915_test_lcd_at_stage(
	void *ctx,
	int stage)
{
	/* The run's stage hook. */
	drv_i915_lcd_kernel_at_stage(ctx, stage);

	/* What the plane fetches, once the picture is up. */
	if (I915_TEST_LCDB_SCANOUT_PROBE && stage == I915_LCD_SHOW_PICTURE_UP)
		i915_test_lcd_probe_scanout(ctx);
}

/*
 * Probes what the plane really fetches.
 *
 * DSPSURFLIVE is the surface address the hardware scans out now and
 * PIPEDSL the line it is on; PLANE_SURF is what the driver armed.  The
 * DDB and watermark are read back too, because the DMC may rewrite them
 * behind the driver when a DC state is entered.  The pipe's own checksum
 * (PIPE_CRC_RES_*) is the same every frame for a still picture.  The
 * probe leaves the CRC unit as it found it.
 */
static void
i915_test_lcd_probe_scanout(
	struct i915_lcd_kernel *k)
{
	static const char *const timing_names[] = { "HTOTAL", "HBLANK", "HSYNC", "VTOTAL", "VBLANK", "VSYNC", "VSYNCSHIFT", "MULT" };
	static const uint32_t timing_regs[] = { 0x60000U, 0x60004U, 0x60008U, 0x6000cU, 0x60010U, 0x60014U, 0x60028U, 0x6002cU };
	static const char *const transport_names[] = { "VRR_CTL", "VRR_VMAX", "VRR_VMIN", "VRR_STATUS", "MSA_MISC", "DP_TP_CTL", "DP_TP_STATUS", "DATA_M", "DATA_N", "LINK_M", "LINK_N" };
	static const uint32_t transport_regs[] = { 0x60420U, 0x60424U, 0x60434U, 0x6042cU, 0x60410U, 0x64040U, 0x64044U, 0x60030U, 0x60034U, 0x60040U, 0x60044U };
	static const char *const engine_names[] = { "DPFC_A_CTL", "DPFC_A_STATUS", "DPFC_B_CTL", "SRD_CTL", "SRD_STATUS", "PSR2_CTL", "PSR2_STATUS", "DC_STATE_EN", "TGL_DP_TP_CTL", "TGL_DP_TP_STATUS" };
	static const uint32_t engine_regs[] = { 0x43208U, 0x43210U, 0x43248U, 0x60800U, 0x60840U, 0x60900U, 0x60940U, 0x45504U, 0x60540U, 0x60544U };
	static const char *const clock_names[] = { "CDCLK_CTL", "CDCLK_PLL_ENABLE", "DSSM", "PIPESRC", "PLANE_POS", "PLANE_OFFSET", "PLANE_KEYMAX", "PS_CTRL_1", "PS_CTRL_2" };
	static const uint32_t clock_regs[] = { 0x46000U, 0x46070U, 0x51004U, 0x6001cU, 0x7018cU, 0x701a4U, 0x701a0U, 0x68180U, 0x68280U };
	static const char *const sample_names[] = { "SURF", "SURFLIVE", "CTL", "PIPESTATUS", "frame", "DSL", "BUF_CFG", "WM0", "DBUF_S1" };
	static const uint32_t sample_regs[] = { 0x7019cU, 0x701acU, 0x70180U, 0x70058U, 0x70040U, 0x70000U, 0x7027cU, 0x70240U, 0x45008U };
	static const char *const crc_names[] = { "CRC0", "CRC1", "CRC2", "CRC3", "CRC4" };
	static const uint32_t crc_regs[] = { 0x60064U, 0x60068U, 0x6006cU, 0x60070U, 0x60074U };
	const struct i915_gt_object *object;
	const struct i915_scanout *so;
	const uint32_t *cpu;
	uint64_t pte_first;
	uint64_t pte_second;
	uint64_t pte_last;
	unsigned sample;

	so = &i915_test_lcd_scanout;

	/* The transcoder's timing and the DP transport, read once: the MSA is generated from these. */
	i915_test_lcd_log_regs(k, "timing", timing_names, timing_regs, sizeof(timing_regs) / sizeof(timing_regs[0]));
	i915_test_lcd_log_regs(k, "transport", transport_names, transport_regs, sizeof(transport_regs) / sizeof(transport_regs[0]));

	/* The two engines that can stand between the plane and the panel: FBC and PSR. */
	i915_test_lcd_log_regs(k, "engines", engine_names, engine_regs, sizeof(engine_regs) / sizeof(engine_regs[0]));

	/* The bytes the plane is pointed at, from both sides: the GGTT entries and the CPU view. */
	object = so->obj;
	cpu = so->cpu;
	if (object != NULL && so->gm != NULL && cpu != NULL) {
		pte_first = drv_i915_gt_ggtt_read_pte(so->gm, object->ggtt_page);
		pte_second = drv_i915_gt_ggtt_read_pte(so->gm, object->ggtt_page + 1U);
		pte_last = drv_i915_gt_ggtt_read_pte(so->gm, object->ggtt_page + object->pages - 1U);
		kern_logf("i915: LCD-B scanout probe memory surf=0x%08x page=%u pages=%u pitch=%u PTE[0]=0x%016llx PTE[1]=0x%016llx PTE[last]=0x%016llx | CPU view %08x %08x %08x %08x\n",
		    (uint32_t)so->surf,
		    object->ggtt_page,
		    object->pages,
		    so->pitch,
		    (unsigned long long)pte_first,
		    (unsigned long long)pte_second,
		    (unsigned long long)pte_last,
		    cpu[0],
		    cpu[1],
		    cpu[so->pitch / 4U],
		    cpu[(so->pitch / 4U) * 400U + 400U]);
	}

	/* The clock the display runs on, and the plane's own geometry. */
	i915_test_lcd_log_regs(k, "clocks", clock_names, clock_regs, sizeof(clock_regs) / sizeof(clock_regs[0]));

	/* The pipe's checksum of what it scans out, from plane 1. */
	drv_i915_write32(k->d->mmio, I915_TEST_PIPE_CRC_CTL_A, I915_TEST_PIPE_CRC_PLANE1);
	(void)drv_i915_read32(k->d->mmio, I915_TEST_PIPE_CRC_CTL_A);

	/* Samples the plane, the pipe and the checksum over a few seconds. */
	for (sample = 0U; sample < I915_TEST_PROBE_SAMPLES; sample++) {
		i915_test_lcd_log_regs(k, "sample", sample_names, sample_regs, sizeof(sample_regs) / sizeof(sample_regs[0]));
		i915_test_lcd_log_regs(k, "CRC", crc_names, crc_regs, sizeof(crc_regs) / sizeof(crc_regs[0]));
		k->ops.usleep(k->ops.ctx, I915_TEST_PROBE_STEP_US);
	}

	/* The CRC unit as the probe found it. */
	drv_i915_write32(k->d->mmio, I915_TEST_PIPE_CRC_CTL_A, 0U);
}

/* Logs one group of the scanout probe's registers, one line per register. */
static void
i915_test_lcd_log_regs(
	const struct i915_lcd_kernel *k,
	const char *what,
	const char *const *names,
	const uint32_t *regs,
	unsigned count)
{
	unsigned index;

	/* Reads and logs every register of the group. */
	for (index = 0U; index < count; index++) {
		kern_logf("i915: LCD-B scanout probe %s %s=0x%08x\n",
		    what,
		    names[index],
		    drv_i915_read32(k->d->mmio, regs[index]));
	}
}

/*
 * Checks the pipe's interrupts, driver-managed: the registers the
 * power-well post-enable hook restored, then a vblank reference (the IMR
 * vblank bit cleared), three waits that each need a new pipe A vblank
 * interrupt and a moving frame counter, the last put (the bit set again),
 * and no further vblank interrupt once it is masked.  0, or EIO.
 */
static int
i915_test_lcdr_irq_check(
	struct i915_lcd_kernel *k)
{
	const struct i915_lcd_kernel_deps *d;
	struct i915_display_irq *irq;
	uint32_t seen[I915_TEST_LCDR_VBLANK_WAITS];
	int waits[I915_TEST_LCDR_VBLANK_WAITS];
	uint32_t extra;
	uint32_t judged;
	uint32_t imr;
	uint32_t ier;
	uint32_t imr_on;
	uint32_t imr_off;
	uint32_t want_ier;
	uint32_t frame_before;
	uint32_t frame_after;
	unsigned raw_before;
	unsigned raw_after;
	unsigned raw_masked;
	unsigned wait;
	int get_error;
	int drain_error;
	int ok;

	d = k->d;
	irq = d->irq;

	/* The vblank delivery must exist. */
	if (irq->vbl == NULL || !irq->vbl->inited)
		return EINVAL;

	/* The bits this path relies on: vblank, the underrun bits and flip done. */
	extra = I915_TEST_LCD_VBLANK_BIT | drv_i915_gen8_de_pipe_underrun_mask(13) | drv_i915_gen8_de_pipe_flip_done_mask(13);
	judged = extra | I915_TEST_LCD_VBLANK_BIT;

	/* The registers the power-well post-enable hook restored. */
	imr = drv_i915_read32(d->mmio, I915_TEST_LCD_PIPE_A_IMR);
	ier = drv_i915_read32(d->mmio, I915_TEST_LCD_PIPE_A_IER);
	want_ier = ~irq->de_irq_mask[0] | extra;

	/* A vblank reference unmasks the vblank interrupt. */
	get_error = drv_i915_drm_vblank_get(irq, 0U);
	imr_on = drv_i915_read32(d->mmio, I915_TEST_LCD_PIPE_A_IMR);
	raw_before = irq->de_vblank_count[0];
	frame_before = drv_i915_test_lcd_frame(k);

	/* Three waits, each for a new vblank interrupt and a moving frame counter. */
	for (wait = 0U; wait < I915_TEST_LCDR_VBLANK_WAITS; wait++) {
		seen[wait] = 0U;
		waits[wait] = EIO;
		if (get_error == 0)
			waits[wait] = drv_i915_wait_vblank(irq, 0U, 1U, I915_TEST_LCDR_VBLANK_WAIT_MS, drv_i915_test_lcd_frame, k, &seen[wait]);
	}

	frame_after = drv_i915_test_lcd_frame(k);
	raw_after = irq->de_vblank_count[0];

	/* The last put masks it again. */
	drv_i915_drm_vblank_put(irq, 0U);

	/*
	 * The baseline is taken only after the mask reads back set and handler
	 * work already inside pipe A has drained: a legitimate vblank handled
	 * between the last wait and the put is not counted as after masking.
	 */
	imr_off = drv_i915_read32(d->mmio, I915_TEST_LCD_PIPE_A_IMR);
	drain_error = drv_i915_irq_drain_pipes(irq, 1U << 0, I915_TEST_LCDR_DRAIN_US);
	raw_masked = irq->de_vblank_count[0];
	drv_i915_test_lcd_sleep_ms(k, 100U);

	/*
	 * The IMR is judged on the bits this path relies on, which must read
	 * back as the driver's mask; bits 17 / 18 were seen written 1 but
	 * reading 0, and the reference never reads IMR back, so they are
	 * logged, not judged.
	 */
	ok = 1;
	if (get_error != 0 ||
	    ((imr ^ irq->de_irq_mask[0]) & judged) != 0U ||
	    (imr & I915_TEST_LCD_VBLANK_BIT) == 0U ||
	    ier != want_ier ||
	    (imr_on & I915_TEST_LCD_VBLANK_BIT) != 0U)
		ok = 0;

	/* Every wait saw a vblank interrupt and the counter moved. */
	for (wait = 0U; wait < I915_TEST_LCDR_VBLANK_WAITS; wait++) {
		if (waits[wait] != 0)
			ok = 0;
	}

	/* The handler counted the vblanks, the drain finished and none arrived once masked. */
	if (raw_after - raw_before < I915_TEST_LCDR_VBLANK_WAITS ||
	    frame_after == frame_before ||
	    drain_error != 0 ||
	    (imr_off & I915_TEST_LCD_VBLANK_BIT) == 0U ||
	    irq->de_vblank_count[0] != raw_masked ||
	    irq->vbl->refs[0] != 0U)
		ok = 0;

	kern_logf("i915: LCD-R IRQ: after the well came on: GEN8_DE_PIPE_IMR(A)=0x%08x (driver's de_irq_mask 0x%08x) GEN8_DE_PIPE_IER(A)=0x%08x (expected ~mask|vblank|underrun|flip done = 0x%08x) | vblank get rc=%d IMR=0x%08x | waits rc=%d/%d/%d new-vblanks %u/%u/%u | handler vblank IRQs +%u frames %u->%u | put IMR=0x%08x | vblank IRQs in the next 100 ms after masking: %u | refs %u | IMR bits differing from the written mask 0x%08x (judged bits 0x%08x) -> %s\n",
	    imr,
	    irq->de_irq_mask[0],
	    ier,
	    want_ier,
	    get_error,
	    imr_on,
	    waits[0],
	    waits[1],
	    waits[2],
	    seen[0],
	    seen[1],
	    seen[2],
	    raw_after - raw_before,
	    frame_before,
	    frame_after,
	    imr_off,
	    irq->de_vblank_count[0] - raw_masked,
	    irq->vbl->refs[0],
	    imr ^ irq->de_irq_mask[0],
	    judged,
	    ok ? "OK" : "FAIL");

	/* Reports interrupts that did not behave. */
	if (!ok)
		return EIO;

	/* Succeeded: the pipe's vblank interrupts are driver-managed. */
	return 0;
}

/*
 * Returns the PWM duty the reference gives a user brightness:
 * scale_user_to_hw(), then intel_backlight_level_to_pwm(), which is the
 * identity here (the minimum and maximum are the PWM's).
 */
static uint32_t
i915_test_lcdr_expected_duty(
	const struct i915_lcd_modeset_status *status,
	uint32_t user)
{
	uint64_t span;
	uint32_t duty;

	/* Scales the user level into [min, max], rounding to the closest. */
	span = (uint64_t)(status->backlight_max - status->backlight_min) * user;
	duty = status->backlight_min + (uint32_t)((span + status->backlight_user_max / 2U) / status->backlight_user_max);

	/* Reports the duty. */
	return duty;
}

/*
 * Runs one brightness step of LCD reuse and checks it on the registers:
 * the PWM period never changes; off means the PWM disabled and the
 * backlight off; on or a level means the PWM enabled and the duty the
 * reference conversion gives.  The scanout must continue through every
 * step.  0, or EIO.
 */
static int
i915_test_lcdr_brightness(
	struct i915_lcd_kernel *k,
	unsigned step,
	const char *name,
	int op,
	uint32_t level,
	uint32_t max,
	unsigned hold_ms)
{
	struct i915_lcd_modeset_status status;
	const struct i915_lcd_kernel_deps *d;
	struct i915_display *display;
	uint32_t frame_before;
	uint32_t frame_after;
	uint32_t ctl;
	uint32_t freq;
	uint32_t duty;
	uint32_t want;
	int error;
	int regs_ok;

	d = k->d;
	display = container_of(k, struct i915_display, lk);

	/* Sets the level, or switches the backlight. */
	if (op == 0) {
		error = drv_i915_lcd_modeset_brightness(display, level, max);
	} else {
		error = drv_i915_lcd_modeset_backlight(display, op > 0);
	}

	/* Reads the software state and the PWM registers. */
	frame_before = drv_i915_test_lcd_frame(k);
	drv_i915_lcd_modeset_status(display, &status);
	ctl = drv_i915_read32(d->mmio, drv_i915_lcd_reg_by_name(display->lcd_world, "BXT_BLC_PWM_CTL"));
	freq = drv_i915_read32(d->mmio, drv_i915_lcd_reg_by_name(display->lcd_world, "BXT_BLC_PWM_FREQ"));
	duty = drv_i915_read32(d->mmio, drv_i915_lcd_reg_by_name(display->lcd_world, "BXT_BLC_PWM_DUTY"));
	want = i915_test_lcdr_expected_duty(&status, status.backlight_user);

	/* Judges the registers against the operation. */
	regs_ok = 1;
	if (freq != k->bl_freq0) {
		regs_ok = 0;
	} else if (op < 0) {
		/* Off: the PWM is disabled and the device's backlight is off. */
		if ((ctl & I915_TEST_LCD_PWM_ENABLE) != 0U || status.backlight_enabled)
			regs_ok = 0;
	} else {
		/* On or a level: the PWM runs the reference's duty. */
		if ((ctl & I915_TEST_LCD_PWM_ENABLE) == 0U || !status.backlight_enabled || duty != want)
			regs_ok = 0;
	}

	kern_logf("i915: LCD-R step=%u %s registers: user %u/%u -> expected DUTY %u, read DUTY %u FREQ 0x%08x (start 0x%08x) PWM_CTL 0x%08x -> %s\n",
	    step,
	    name,
	    status.backlight_user,
	    status.backlight_user_max,
	    op < 0 ? 0U : want,
	    duty,
	    freq,
	    k->bl_freq0,
	    ctl,
	    regs_ok ? "OK" : "MISMATCH");
	kern_logf("i915: LCD-R step=%u %s rc=%d | BXT_BLC_PWM_CTL=0x%08x FREQ=0x%08x DUTY=0x%08x (level %u of [%u,%u]) backlight_enabled=%d PP_CONTROL backlight bit via eDP | crtc_active=%d plane_armed=%d (take the photograph)\n",
	    step,
	    name,
	    error,
	    ctl,
	    freq,
	    duty,
	    status.backlight_level,
	    status.backlight_min,
	    status.backlight_max,
	    status.backlight_enabled,
	    status.crtc_active,
	    status.plane_armed);

	/* Holds the step for the camera and watches the scanout continue. */
	drv_i915_test_lcd_sleep_ms(k, hold_ms);
	frame_after = drv_i915_test_lcd_frame(k);
	kern_logf("i915: LCD-R step=%u %s done: frame counter %u -> %u over %u ms (the scanout %s)\n",
	    step,
	    name,
	    frame_before,
	    frame_after,
	    hold_ms,
	    frame_after != frame_before ? "continued" : "STOPPED");

	/* A step that failed, left other registers or stopped the scanout. */
	if (error != I915_LCD_MS_OK ||
	    !regs_ok ||
	    frame_after == frame_before ||
	    !status.crtc_active ||
	    !status.plane_armed)
		return EIO;

	/* Succeeded: the step is on the panel and the scanout continued. */
	return 0;
}

/*
 * Runs the first window of LCD reuse: the interrupt check, then the
 * brightness steps and the backlight off and on, and the user level found
 * at the start restored (the user brightness is what is restored, not the
 * hardware level).
 */
static int
i915_test_lcdr_window_first(
	void *ctx,
	struct i915_lcd_observer *observer)
{
	const struct i915_test_lcdr_step *step;
	struct i915_lcd_modeset_status status;
	struct i915_display *display;
	struct i915_lcd_kernel *k;
	uint32_t user_max;
	uint32_t user_start;
	uint32_t level;
	uint32_t duty;
	unsigned index;
	int error;

	UNUSED_PARAMETER(observer);

	k = ctx;
	display = container_of(k, struct i915_display, lk);

	/* The pipe's interrupts first. */
	error = i915_test_lcdr_irq_check(k);
	if (error != 0)
		return error;

	/* The user range, the level to restore and the PWM period to keep. */
	drv_i915_lcd_modeset_status(display, &status);
	user_max = status.backlight_user_max;
	user_start = status.backlight_user;
	k->bl_freq0 = drv_i915_read32(k->d->mmio, drv_i915_lcd_reg_by_name(display->lcd_world, "BXT_BLC_PWM_FREQ"));
	kern_logf("i915: LCD-R brightness: VBT min_brightness %u (a 0..255 coefficient) -> backlight.min %u of max %u (get_backlight_min_vbt); current level %u; user range [0, %u] (intel_backlight_device_register)\n",
	    k->vbt_min,
	    status.backlight_min,
	    user_max,
	    user_start,
	    user_max);

	/* Runs the steps until the first one fails. */
	for (index = 0U; index < I915_TEST_LCDR_STEPS; index++) {
		step = &i915_test_lcdr_steps[index];
		level = user_max * step->level_num / step->level_den;
		error = i915_test_lcdr_brightness(k, index, step->name, step->op, level, user_max, step->hold_ms);
		if (error != 0)
			return error;
	}

	/* Notes a level after the backlight came back that is neither half nor the duty. */
	drv_i915_lcd_modeset_status(display, &status);
	duty = drv_i915_read32(k->d->mmio, drv_i915_lcd_reg_by_name(display->lcd_world, "BXT_BLC_PWM_DUTY"));
	if (status.backlight_level != user_max / 2U && status.backlight_level != duty) {
		kern_logf("i915: LCD-R note: level after backlight-on %u, duty 0x%08x\n",
		    status.backlight_level,
		    duty);
	}

	/* Restores the user level found at the start. */
	error = i915_test_lcdr_brightness(k, I915_TEST_LCDR_STEPS, "restore", 0, user_start, user_max, I915_TEST_LCDR_SHORT_HOLD_MS);
	if (error != 0)
		return error;

	/* Succeeded: every step was on the panel and the level was restored. */
	return 0;
}

/* Runs a later window of LCD reuse: the interrupt check only. */
static int
i915_test_lcdr_window_again(
	void *ctx,
	struct i915_lcd_observer *observer)
{
	int error;

	UNUSED_PARAMETER(observer);

	/* The pipe's interrupts on this cycle's well. */
	error = i915_test_lcdr_irq_check(ctx);
	if (error != 0)
		return error;

	/* Succeeded: the interrupts behaved again. */
	return 0;
}
