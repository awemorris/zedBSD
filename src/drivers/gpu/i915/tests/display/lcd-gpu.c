/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GPU-drawn picture scenarios: LCD-G and LCD-D.
 *
 * LCD-G: the GPU draws the full-HD textured image into the scanout buffer
 * through a PPGTT mapping of the same pages; the image is checked pixel by
 * pixel, and that very object is shown with no CPU write since the draw.
 *
 * LCD-D: two buffers, each mapped for the whole run at its own address.
 * The GPU draws into the buffer NOT on the display, the flip shows it, and
 * the old front becomes the next draw target: eight draws, seven flips,
 * every texture variant into both buffers.  Then the evasion probe starts
 * ordinary flips at chosen scanlines until one update sleeps out of the
 * evasion window.
 *
 * The GPU work runs on the render engine while nothing else uses it; the
 * forcewake domains the start holds for the served node cover it.
 */

#include "lcd-gpu.h"
#include "lcd-run.h"
#include "scenarios.h"
#include "../execution/fhd-render.h"
#include "../fixtures/draw-fixture.h"
#include "../../display/diagnostics.h"
#include "../../display/modeset.h"
#include "../../display/scanout.h"
#include "../../display/vblank.h"
#include "../../ggtt.h"
#include "../../i915.h"
#include "../../memory.h"
#include "../../mmio.h"
#include "../../tlb.h"
#include <kern/kcrt.h>

#include <kern/klog.h>

#include <uapi/errno.h>
#include <stddef.h>

/* How long a draw may take. */
#define I915_TEST_GPU_DRAW_TIMEOUT_MS	2000U

/* How long the LCD-G picture stays up. */
#define I915_TEST_LCDG_WINDOW_MS	12000U

/* The draws of LCD-D after the first, and how long each picture holds. */
#define I915_TEST_LCDD_ROUNDS		7U
#ifndef I915_TEST_LCDD_HOLD_MS
#define I915_TEST_LCDD_HOLD_MS		6000U
#endif

/* The short window of the show body; the rounds run inside it. */
#define I915_TEST_LCDD_WINDOW_MS	1000U

/* How many flips the evasion probe may start, and how long it polls for a scanline. */
#define I915_TEST_LCDD_PROBE_MAX	16U
#define I915_TEST_LCDD_PROBE_POLLS	400000U

/* PIPEDSL(A) and its scanline field. */
#define I915_TEST_PIPEDSL_A		0x70000U
#define I915_TEST_PIPEDSL_MASK		0x1fffU

/* The texture variants of LCD-D. */
#define I915_TEST_LCDD_VARIANTS		4U

/* The first scanline the probe saw after its trigger, before one was seen. */
#define I915_TEST_LCDD_NO_SCANLINE	0xffffffffU

/*
 * The results of the evasion probe: reached, not reached, or not run
 * because the window could not be read.
 */
enum i915_test_lcdd_probe {
	I915_TEST_LCDD_PROBE_ERROR = -1,
	I915_TEST_LCDD_PROBE_NOT_REACHED = 0,
	I915_TEST_LCDD_PROBE_REACHED = 1
};

/*
 * The buffer, the draw and the TLB of LCD-G.
 *
 * They outlive a run that could not show the GPU or the display done: the
 * buffer is then abandoned in place and the next run refuses.
 */
static struct i915_scanout i915_test_lcdg_scanout;
static struct i915_test_fhd_render i915_test_lcdg_render;
static struct i915_gt_tlb i915_test_lcdg_tlb;

/*
 * The show environment and report of the GPU scenarios.
 *
 * Too large for the stack; rewritten by each run.
 */
static struct i915_lcd_show_env i915_test_gpu_env;
static struct i915_lcd_show_report i915_test_gpu_report;

/*
 * The two buffers of LCD-D, their run-long render-target mappings, the
 * draw, the verification draw and the TLB.
 *
 * They outlive a run whose draw or stop was not shown done
 * (i915_test_lcdd_gpu_kept); the next run refuses.
 */
static struct i915_scanout i915_test_lcdd_buf[2];
static struct i915_test_fhd_rt_map i915_test_lcdd_map[2];
static struct i915_test_fhd_render i915_test_lcdd_render;
static struct i915_test_fhd_render i915_test_lcdd_check;
static struct i915_gt_tlb i915_test_lcdd_tlb;
static int i915_test_lcdd_gpu_kept;

/*
 * The variant each LCD-D buffer holds now, and the image hash every
 * variant gave in each buffer (with how many draws made it).  Reset by
 * each run; the cross-buffer check compares them at the end.
 */
static unsigned i915_test_lcdd_variant[2];
static uint64_t i915_test_lcdd_hash[2][I915_TEST_LCDD_VARIANTS];
static unsigned i915_test_lcdd_hash_set[2][I915_TEST_LCDD_VARIANTS];

/*
 * What the evasion probe found: its result, how many flips it took, and
 * the lead in lines between the trigger and the first update read.
 */
static int i915_test_lcdd_probe_result;
static unsigned i915_test_lcdd_probe_tries;
static unsigned i915_test_lcdd_probe_lead;

/*
 * The variant of each LCD-D draw: every variant into both buffers, and a
 * buffer's next variant always differs from what it last held.
 */
static const unsigned i915_test_lcdd_seq[I915_TEST_LCDD_ROUNDS + 1U] = { 0U, 1U, 2U, 3U, 1U, 2U, 3U, 0U };

static int i915_test_gpu_begin(struct i915_display *display, const char *tag, struct i915_lcd_kernel **k_out);
static void i915_test_gpu_set_env(struct i915_lcd_kernel *k, int (*in_window)(void *ctx, struct i915_lcd_observer *o), unsigned window_ms);
static uint32_t i915_test_lcdg_verify(void *ctx, const struct i915_scanout *so);
static int i915_test_lcdg_draw(struct i915_display *display, struct i915_lcd_kernel *k);
static uint32_t i915_test_lcdd_verify(void *ctx, const struct i915_scanout *so);
static int i915_test_lcdd_buffers(struct i915_lcd_kernel *k);
static int i915_test_lcdd_draw(struct i915_lcd_kernel *k, unsigned index, unsigned variant, unsigned round);
static int i915_test_lcdd_flip(struct i915_lcd_kernel *k, unsigned back, struct i915_lcd_flip_result *flip);
static int i915_test_lcdd_evasion_probe(struct i915_lcd_kernel *k, unsigned front);
static int i915_test_lcdd_rounds(void *ctx, struct i915_lcd_observer *observer);
static void i915_test_lcdd_abandon_both(void);
static int i915_test_lcdd_reclaim(struct i915_lcd_kernel *k, uint32_t *bad, int *unmapped);
static unsigned i915_test_lcdd_cross(void);

/*
 * Releases a GPU-drawn scanout buffer when both of its users are done.
 *
 * The GPU not shown done keeps the buffer, the state, the batch, the
 * texture and the context; a display that did not let go leaves the
 * buffer to the show body, which already abandoned it; render mappings or
 * a TLB not shown gone keep the buffer too.  Returns 1 when it was freed.
 */
int
drv_i915_test_lcdg_finish(
	struct i915_display *display,
	struct i915_test_fhd_render *fr,
	struct i915_scanout *so,
	int display_acquired,
	int display_released,
	struct i915_gt_mem *gm,
	struct i915_gt_ppgtt *vm,
	struct i915_gt_tlb *tlb,
	struct i915_gt_engines *es,
	struct i915_mmio *m,
	struct spinlock *uncore_lock,
	int *render_rc)
{
	int release_error;
	int unpin_error;
	int destroy_error;

	*render_rc = EBUSY;

	/* The GPU is not shown to have let go: everything its request may use stays. */
	if (fr->t.submitted && !fr->gpu_done) {
		drv_i915_test_fhd_render_keep(fr);
		if (so->state >= I915_SCANOUT_PINNED && so->state != I915_SCANOUT_ABANDONED)
			drv_i915_scanout_abandon(so);

		drv_i915_lcd_show_retain_gpu(display, gm, "the GPU request using the buffer did not finish");
		return 0;
	}

	/* The show body already abandoned a buffer the display did not let go of, and set the latch. */
	if (display_acquired && !display_released)
		return 0;

	/* The draw's mappings, the TLB and its own objects. */
	release_error = drv_i915_test_fhd_render_release(fr, gm, vm, tlb, es, m, uncore_lock);
	*render_rc = release_error;
	if (release_error != 0) {
		/* The pages may still be translated: the buffer stays. */
		if (so->state >= I915_SCANOUT_PINNED && so->state != I915_SCANOUT_ABANDONED)
			drv_i915_scanout_abandon(so);

		drv_i915_lcd_show_retain_gpu(display, gm, "the render mappings / TLB could not be shown released");
		return 0;
	}

	/* The buffer itself, last. */
	unpin_error = drv_i915_scanout_unpin(so);
	if (unpin_error != 0)
		return 0;

	destroy_error = drv_i915_scanout_destroy(so);
	if (destroy_error != 0)
		return 0;

	/* Succeeded: both users were done and the buffer is gone. */
	return 1;
}

/*
 * Runs LCD-G: a GPU-drawn full-HD image shown from the same backing the
 * GPU wrote, then the stop path and the release in the users' order.
 */
void
drv_i915_test_display_lcdg(
	struct i915_device *device)
{
	const struct i915_lcd_kernel_deps *d;
	struct i915_test_fhd_render *fr;
	struct i915_lcd_show_report *rep;
	struct i915_display *display;
	struct i915_scanout *so;
	struct i915_lcd_kernel *k;
	const char *first_anomaly;
	int show_error;
	int released;
	int render_error;
	int held;
	int pass;
	int error;

	so = &i915_test_lcdg_scanout;
	fr = &i915_test_lcdg_render;
	rep = &i915_test_gpu_report;

	/* The display to show on. */
	display = drv_i915_test_lcd_display(device, "LCD-G");
	if (display == NULL)
		return;

	/* An earlier run's buffer may still be read or drawn into: refused. */
	if (so->state != I915_SCANOUT_NONE) {
		kern_logf("i915: LCD-G verdict: FAIL (refused before any initialisation: retained resources)\n");
		return;
	}

	/* The run; the hardware must be idle and the inputs complete. */
	error = i915_test_gpu_begin(display, "LCD-G", &k);
	if (error != 0)
		return;

	d = k->d;

	/* The GPU draws into the buffer; nothing is shown when the image is not the expected one. */
	error = i915_test_lcdg_draw(display, k);
	if (error != 0)
		return;

	/* The show environment: that very object, no CPU write since the draw. */
	i915_test_gpu_set_env(k, NULL, I915_TEST_LCDG_WINDOW_MS);
	k->pattern_id = 0U;
	if (d->irq->vbl != NULL) {
		k->post_before = d->irq->vbl->post_enable_calls;
		k->pre_before = d->irq->vbl->pre_disable_calls;
	}

	kern_logf("i915: LCD-G showing the GPU-drawn buffer (surf 0x%08llx, image fnv %016llx)\n",
	    (unsigned long long)so->surf,
	    (unsigned long long)fr->image_hash);

	/* Shows it and stops through the reference's path. */
	show_error = drv_i915_lcd_show_prepared(display, &i915_test_gpu_env, so, i915_test_lcdg_verify, NULL, rep);
	drv_i915_lcd_log_trace(rep->trace);
	drv_i915_lcd_log_observer(&rep->obs);
	drv_i915_lcd_log_status("enable-returned", &rep->at_enable);
	drv_i915_lcd_log_status("disable-returned", &rep->at_disable);
	held = drv_i915_test_lcd_power_held(k);

	/* Both users done?  The display's stop confirmed, the GPU retired and parked: release in that order. */
	released = drv_i915_test_lcdg_finish(display, fr, so, rep->display_acquired, rep->display_released, d->gm, d->vm, &i915_test_lcdg_tlb, d->es, d->mmio, d->uncore_lock, &render_error);
	kern_logf("i915: LCD-G release: mappings back to scratch %u/%u (first left 0x%llx) TLB rc=%d (invalidations %u, engines %u, timeouts %u) release calls %u\n",
	    fr->maps_scratch,
	    fr->maps_total,
	    (unsigned long long)fr->first_unreleased_va,
	    fr->tlb_rc,
	    i915_test_lcdg_tlb.invalidations,
	    i915_test_lcdg_tlb.engines_invalidated,
	    i915_test_lcdg_tlb.timeouts,
	    fr->release_calls);

	first_anomaly = "none";
	if (rep->first_anomaly != NULL) {
		first_anomaly = rep->first_anomaly;
	} else if (!rep->display_acquired) {
		first_anomaly = "the display part did not start";
	}

	kern_logf("i915: LCD-G stop / release: display released=%d abandoned=%d | render PTEs cleared %u/%u released=%d | buffer unpinned+destroyed=%d | image re-checked after the stop: wrong pixels=%u | display pages in use=%u | power refs held=%d | first anomaly: %s (stage %s)\n",
	    rep->display_released,
	    rep->abandoned,
	    fr->rt_pages_cleared,
	    fr->rt_pages_mapped,
	    render_error == 0,
	    released,
	    rep->readback_bad_after,
	    d->gm->display_allocated_pages,
	    held,
	    first_anomaly,
	    drv_i915_test_lcd_stage_name(rep->first_anomaly_stage));

	/* Judges the run. */
	pass = 0;
	if (show_error == 0 && released && held == 0 && k->unresolved_steps == 0U && k->time_faults == 0U)
		pass = 1;

	kern_logf("i915: LCD-G verdict: %s (GPU pixels %u/%u, same backing, shown from the GPU-written object, stop %s, released=%d; the photograph is separate evidence)\n",
	    pass ? "PASS" : "FAIL",
	    fr->px_match,
	    fr->px_total,
	    rep->display_released ? "confirmed" : "NOT confirmed",
	    released);
}

/*
 * Runs LCD-D: the GPU redraws the buffer not on the display, then the
 * synchronous flip shows it; eight draws, seven flips, the evasion probe,
 * the stop path and both buffers released.
 */
void
drv_i915_test_display_lcdd(
	struct i915_device *device)
{
	struct i915_lcd_show_report *rep;
	struct i915_display *display;
	struct i915_lcd_kernel *k;
	const char *first_anomaly;
	const char *evasion;
	const char *stop;
	uint32_t bad[2];
	int show_error;
	int released;
	int unmapped;
	int held;
	int pass;
	int error;

	rep = &i915_test_gpu_report;
	bad[0] = 0U;
	bad[1] = 0U;
	released = 0;
	unmapped = 0;
	held = 0;
	show_error = EIO;

	/* The display to show on. */
	display = drv_i915_test_lcd_display(device, "LCD-D");
	if (display == NULL)
		return;

	/* An earlier run's buffers or draw may still be in use: refused. */
	if (i915_test_lcdd_buf[0].state != I915_SCANOUT_NONE ||
	    i915_test_lcdd_buf[1].state != I915_SCANOUT_NONE ||
	    i915_test_lcdd_gpu_kept) {
		kern_logf("i915: LCD-D verdict: FAIL (refused before any initialisation: retained resources)\n");
		return;
	}

	/* The run; the hardware must be idle and the inputs complete. */
	error = i915_test_gpu_begin(display, "LCD-D", &k);
	if (error != 0)
		return;

	/* Nothing is known of an earlier run's images. */
	kern_memset(i915_test_lcdd_variant, 0, sizeof(i915_test_lcdd_variant));
	kern_memset(i915_test_lcdd_hash, 0, sizeof(i915_test_lcdd_hash));
	kern_memset(i915_test_lcdd_hash_set, 0, sizeof(i915_test_lcdd_hash_set));
	kern_memset(rep, 0, sizeof(*rep));
	i915_test_lcdd_probe_result = I915_TEST_LCDD_PROBE_NOT_REACHED;
	i915_test_lcdd_probe_tries = 0U;
	i915_test_lcdd_probe_lead = 0U;

	/* Two buffers, each mapped for the run; the GPU draws A (variant 0) before the display gets it. */
	error = i915_test_lcdd_buffers(k);
	if (error == 0)
		error = i915_test_lcdd_draw(k, 0U, 0U, 0U);
	if (error != 0) {
		kern_logf("i915: LCD-D verdict: FAIL (setup / first draw rc=%d; the display was not started)\n", error);
	} else {
		/* Shows A, runs the rounds in the window, stops through the reference's path. */
		i915_test_gpu_set_env(k, i915_test_lcdd_rounds, I915_TEST_LCDD_WINDOW_MS);
		k->pattern_id = 0U;
		show_error = drv_i915_lcd_show_prepared(display, &i915_test_gpu_env, &i915_test_lcdd_buf[0], i915_test_lcdd_verify, NULL, rep);
		drv_i915_lcd_log_trace(rep->trace);
		drv_i915_lcd_log_observer(&rep->obs);
		held = drv_i915_test_lcd_power_held(k);
	}

	/* The stop is not confirmed: the display may read either buffer, both kept with their mappings. */
	if (rep->display_acquired && !rep->display_released) {
		i915_test_lcdd_abandon_both();
	} else {
		released = i915_test_lcdd_reclaim(k, bad, &unmapped);
	}

	/* Every variant must give the same image in both buffers. */
	k->cross_ok = i915_test_lcdd_cross();

	evasion = "ERROR";
	if (i915_test_lcdd_probe_result == I915_TEST_LCDD_PROBE_REACHED) {
		evasion = "REACHED";
	} else if (i915_test_lcdd_probe_result == I915_TEST_LCDD_PROBE_NOT_REACHED) {
		evasion = "NOT-REACHED";
	}

	kern_logf("i915: LCD-D evasion: %s (probe flips %u, measured latency %u lines, IRQs off at a sleep entry %u, events cancelled by the stop %u)\n",
	    evasion,
	    k->probe_flips,
	    i915_test_lcdd_probe_lead,
	    k->sleep_irq_off,
	    k->events_cancelled);

	/* Judges the run; a probe that did not reach the window is recorded, not a failure. */
	pass = 0;
	if (show_error == 0 &&
	    k->draws_ok == I915_TEST_LCDD_ROUNDS + 1U &&
	    k->flips_done == I915_TEST_LCDD_ROUNDS &&
	    released &&
	    held == 0 &&
	    bad[0] == 0U &&
	    bad[1] == 0U &&
	    k->unresolved_steps == 0U &&
	    k->time_faults == 0U &&
	    k->cross_ok == I915_TEST_LCDD_VARIANTS &&
	    k->sleep_irq_off == 0U &&
	    i915_test_lcdd_probe_result >= I915_TEST_LCDD_PROBE_NOT_REACHED)
		pass = 1;

	stop = "not started";
	if (rep->display_released) {
		stop = "confirmed";
	} else if (rep->display_acquired) {
		stop = "NOT confirmed";
	}

	first_anomaly = "none";
	if (rep->first_anomaly != NULL)
		first_anomaly = rep->first_anomaly;

	kern_logf("i915: LCD-D verdict: %s (GPU draws %u/%u, flips done %u/%u (+%u probe), cross-buffer variants %u/4, one modeset, stop %s, targets unmapped=%d, both buffers released=%d, pixels after: A bad %u B bad %u, power refs held %d, first anomaly: %s)\n",
	    pass ? "PASS" : "FAIL",
	    k->draws_ok,
	    I915_TEST_LCDD_ROUNDS + 1U,
	    k->flips_done,
	    I915_TEST_LCDD_ROUNDS,
	    k->probe_flips,
	    k->cross_ok,
	    stop,
	    unmapped,
	    released,
	    bad[0],
	    bad[1],
	    held,
	    first_anomaly);
}

/*
 * Starts a GPU scenario's run: refuses when an earlier run retained
 * resources, then the run's hooks, the preflight and the configuration,
 * and the display window of the GGTT.  0, or EBUSY.
 */
static int
i915_test_gpu_begin(
	struct i915_display *display,
	const char *tag,
	struct i915_lcd_kernel **k_out)
{
	const struct i915_lcd_kernel_deps *d;
	struct i915_lcd_kernel *k;
	int retained;
	int preflight_error;
	int fill_error;
	int window_error;

	d = &display->rlcd;

	/* The draw needs the GT: its engines and its address space. */
	if (d->es == NULL || d->vm == NULL || d->irq == NULL) {
		kern_logf("i915: %s verdict: FAIL (a dependency is missing)\n", tag);
		return EBUSY;
	}

	/* An earlier run left resources the display may still read. */
	retained = drv_i915_test_lcd_retained(display);
	if (retained) {
		kern_logf("i915: %s verdict: FAIL (refused before any initialisation: retained resources)\n", tag);
		return EBUSY;
	}

	/* The run; the hardware must be idle and the inputs complete. */
	k = drv_i915_test_lcd_start(display, NULL);
	kern_memset(&i915_test_gpu_env, 0, sizeof(i915_test_gpu_env));
	preflight_error = drv_i915_lcd_kernel_preflight(k);
	fill_error = 0;
	if (preflight_error == 0)
		fill_error = drv_i915_lcd_kernel_fill_cfg(k, &i915_test_gpu_env.cfg);
	if (preflight_error != 0 || fill_error != 0) {
		kern_logf("i915: %s verdict: FAIL (preflight: nothing was written)\n", tag);
		return EBUSY;
	}

	/* The display window of the GGTT; claimed earlier is fine. */
	window_error = drv_i915_gt_display_window_init(d->gm, I915_GT_DISPLAY_PAGES);
	if (window_error != 0 && window_error != EBUSY) {
		kern_logf("i915: %s verdict: FAIL (display window rc=%d)\n", tag, window_error);
		return EBUSY;
	}

	/* Succeeded: the run may draw and show. */
	*k_out = k;
	return 0;
}

/* Fills the show environment of a GPU scenario: the panel, the window and the stage hook. */
static void
i915_test_gpu_set_env(
	struct i915_lcd_kernel *k,
	int (*in_window)(void *ctx, struct i915_lcd_observer *o),
	unsigned window_ms)
{
	struct i915_lcd_show_env *env;

	env = &i915_test_gpu_env;

	/* The buffer is the caller's; the configuration was filled by the begin. */
	env->hw = &k->ops;
	env->gm = k->d->gm;
	env->so = NULL;
	env->lcd = &k->d->edp->lcd;
	env->pipe = 0;
	env->first_frames_ms = I915_TEST_LCD_FIRST_FRAMES_MS;
	env->window_ms = window_ms;
	env->in_window = in_window;
	env->in_window_ctx = k;
	env->at_stage = drv_i915_lcd_kernel_at_stage;
	env->at_stage_ctx = k;
	k->window_ms = window_ms;
}

/* Counts the pixels of the LCD-G buffer that differ from the GPU image, read from memory. */
static uint32_t
i915_test_lcdg_verify(
	void *ctx,
	const struct i915_scanout *so)
{
	uint32_t bad;

	UNUSED_PARAMETER(ctx);

	/* Reads what is in memory, not a stale CPU line. */
	drv_i915_gt_clflush(so->cpu, so->size);
	bad = drv_i915_test_fhd_render_verify(&i915_test_lcdg_render, so->cpu, so->pitch);

	/* Reports the count. */
	return bad;
}

/*
 * Makes the LCD-G buffer and has the GPU draw into it through its PPGTT
 * mapping; the image and the backing are checked.  0, or EIO when nothing
 * is to be shown (the buffer is then released or kept by the finish).
 */
static int
i915_test_lcdg_draw(
	struct i915_display *display,
	struct i915_lcd_kernel *k)
{
	const struct i915_lcd_kernel_deps *d;
	struct i915_test_fhd_render *fr;
	struct i915_scanout *so;
	uint64_t ggtt_first;
	uint64_t ggtt_last;
	int same_backing;
	int render_error;
	int released;
	int retained;
	int error;

	d = k->d;
	so = &i915_test_lcdg_scanout;
	fr = &i915_test_lcdg_render;

	/* One backing: the scanout buffer, laid out as the render target. */
	error = drv_i915_scanout_create(d->gm, I915_TEX_FHD_WIDTH, I915_TEX_FHD_HEIGHT, I915_FOURCC_XRGB8888, I915_MOD_LINEAR, so);
	if (error == 0 && (so->pitch != I915_TEX_FHD_PITCH || so->size != I915_TEX_FHD_RT_BYTES)) {
		kern_logf("i915: LCD-G: scanout layout pitch=%u size=%u differs from the render target's (isl: %u / %u)\n",
		    so->pitch,
		    so->size,
		    I915_TEX_FHD_PITCH,
		    I915_TEX_FHD_RT_BYTES);
		(void)drv_i915_scanout_destroy(so);
		error = EINVAL;
	}

	/* Pins it for the display. */
	if (error == 0) {
		error = drv_i915_scanout_pin(so, "lcd-g");
		if (error != 0)
			(void)drv_i915_scanout_destroy(so);
	}

	if (error != 0) {
		kern_logf("i915: LCD-G verdict: FAIL (scanout buffer rc=%d)\n", error);
		return EIO;
	}

	/* The GPU draws into the same pages through the PPGTT. */
	kern_memset(fr, 0, sizeof(*fr));
	error = drv_i915_test_fhd_render_run(fr, d->es, d->vm, d->gm, d->mmio, d->uncore_lock, I915_TEST_GPU_DRAW_TIMEOUT_MS, so->obj);
	ggtt_first = drv_i915_gt_ggtt_read_pte(d->gm, so->obj->ggtt_page);
	ggtt_last = drv_i915_gt_ggtt_read_pte(d->gm, so->obj->ggtt_page + fr->rt_pages - 1U);

	/* The GGTT view the display reads and the PPGTT view the GPU wrote name the same pages. */
	same_backing = 0;
	if (fr->rt == so->obj &&
	    fr->rt_walk_ok &&
	    (ggtt_first & ~0xfffULL) == (fr->rt_first_dma & ~0xfffULL) &&
	    (ggtt_last & ~0xfffULL) == (fr->rt_last_dma & ~0xfffULL))
		same_backing = 1;

	kern_logf("i915: LCD-G render: rc=%d where=%s outcome=%s submitted=%d completed=%d parked=%d timed_out=%d | markers before=%08x middraw=%08x after=%08x ps=%08x | pixels match=%u/%u stale=%u first_bad=(%d,%d) expected=%08x observed=%08x | texture changed=%u guard_bad=%u | image fnv=%016llx | batch_dwords=%u pipesel rc=%d | rt mocs=%u\n",
	    error,
	    fr->t.err_where != NULL ? fr->t.err_where : "-",
	    fr->t.outcome == I915_TEST_EU_PASS ? "PASS" : (fr->t.outcome == I915_TEST_EU_HANG ? "HANG" : "ERROR"),
	    fr->t.submitted,
	    fr->t.completed,
	    fr->t.parked,
	    fr->t.timed_out,
	    fr->marker_before,
	    fr->marker_middraw,
	    fr->marker_after,
	    fr->ps_marker,
	    fr->px_match,
	    fr->px_total,
	    fr->px_stale,
	    fr->first_bad_x,
	    fr->first_bad_y,
	    fr->first_bad_expected,
	    fr->first_bad_observed,
	    fr->tex_changed_bytes,
	    fr->guard_bad_bytes,
	    (unsigned long long)fr->image_hash,
	    fr->t.batch_dwords,
	    fr->t.pipesel_rc,
	    fr->rt_rss_mocs);
	kern_logf("i915: LCD-G same backing: object %p | PPGTT 0x%llx.. %u pages mapped, walk ok=%d, first page dma 0x%llx last 0x%llx | GGTT surf 0x%08llx: PTE first 0x%llx last 0x%llx -> %s\n",
	    (void *)so->obj,
	    (unsigned long long)I915_TEX_FHD_RT_VA,
	    fr->rt_pages_mapped,
	    fr->rt_walk_ok,
	    (unsigned long long)fr->rt_first_dma,
	    (unsigned long long)fr->rt_last_dma,
	    (unsigned long long)so->surf,
	    (unsigned long long)ggtt_first,
	    (unsigned long long)ggtt_last,
	    same_backing ? "SAME PAGES" : "DIFFERENT");

	/* An image that is not the expected one, or other pages, is not shown. */
	if (error != 0 || fr->t.outcome != I915_TEST_EU_PASS || !same_backing) {
		released = drv_i915_test_lcdg_finish(display, fr, so, 0, 0, d->gm, d->vm, &i915_test_lcdg_tlb, d->es, d->mmio, d->uncore_lock, &render_error);
		retained = drv_i915_lcd_show_retained(display);
		kern_logf("i915: LCD-G verdict: FAIL (the GPU draw did not produce the expected image; nothing was shown; gpu_done=%d render release rc=%d buffer freed=%d retained=%d)\n",
		    fr->gpu_done,
		    render_error,
		    released,
		    retained);
		return EIO;
	}

	/* Succeeded: the buffer holds the GPU's image in the pages the display reads. */
	return 0;
}

/* Counts the pixels of an LCD-D buffer that differ from the image of the variant it holds. */
static uint32_t
i915_test_lcdd_verify(
	void *ctx,
	const struct i915_scanout *so)
{
	unsigned index;
	uint32_t bad;

	UNUSED_PARAMETER(ctx);

	/* Which buffer, and so which variant. */
	index = 1U;
	if (so == &i915_test_lcdd_buf[0])
		index = 0U;

	/* Reads what is in memory, against the expected image of that variant. */
	drv_i915_gt_clflush(so->cpu, so->size);
	kern_memset(&i915_test_lcdd_check, 0, sizeof(i915_test_lcdd_check));
	i915_test_lcdd_check.variant = i915_test_lcdd_variant[index];
	bad = drv_i915_test_fhd_render_verify(&i915_test_lcdd_check, so->cpu, so->pitch);

	/* Reports the count. */
	return bad;
}

/*
 * Makes the two LCD-D buffers and maps each for the run: A at the first
 * render-target address, B at the second.  0, or the failing step's error.
 */
static int
i915_test_lcdd_buffers(
	struct i915_lcd_kernel *k)
{
	static const char *const owners[2] = { "lcd-d A", "lcd-d B" };
	static const uint64_t vas[2] = { I915_TEX_FHD_RT_VA, I915_TEX_FHD_RT_B_VA };
	const struct i915_lcd_kernel_deps *d;
	unsigned index;
	int error;

	d = k->d;
	error = 0;

	/* Creates, pins and maps each buffer until one fails. */
	for (index = 0U; index < 2U && error == 0; index++) {
		error = drv_i915_scanout_create(d->gm, I915_TEX_FHD_WIDTH, I915_TEX_FHD_HEIGHT, I915_FOURCC_XRGB8888, I915_MOD_LINEAR, &i915_test_lcdd_buf[index]);
		if (error == 0)
			error = drv_i915_scanout_pin(&i915_test_lcdd_buf[index], owners[index]);
		if (error == 0) {
			kern_memset(&i915_test_lcdd_map[index], 0, sizeof(i915_test_lcdd_map[index]));
			error = drv_i915_test_fhd_rt_map(&i915_test_lcdd_map[index], d->gm, d->vm, i915_test_lcdd_buf[index].obj, vas[index]);
		}
	}

	kern_logf("i915: LCD-D buffers: A surf 0x%08llx ggtt page %u -> PPGTT 0x%llx (%u pages, walk %d) | B surf 0x%08llx ggtt page %u -> PPGTT 0x%llx (%u pages, walk %d) | rc=%d\n",
	    (unsigned long long)i915_test_lcdd_buf[0].surf,
	    i915_test_lcdd_buf[0].obj != NULL ? i915_test_lcdd_buf[0].obj->ggtt_page : 0U,
	    (unsigned long long)i915_test_lcdd_map[0].va,
	    i915_test_lcdd_map[0].mapped,
	    i915_test_lcdd_map[0].walk_ok,
	    (unsigned long long)i915_test_lcdd_buf[1].surf,
	    i915_test_lcdd_buf[1].obj != NULL ? i915_test_lcdd_buf[1].obj->ggtt_page : 0U,
	    (unsigned long long)i915_test_lcdd_map[1].va,
	    i915_test_lcdd_map[1].mapped,
	    i915_test_lcdd_map[1].walk_ok,
	    error);

	/* Reports the failing step. */
	if (error != 0)
		return error;

	/* Succeeded: both buffers are pinned and mapped. */
	return 0;
}

/*
 * Has the GPU draw one variant into buffer index (never the one on the
 * display), checked pixel by pixel after retire, park and clflush; then
 * the draw's own mappings, TLB and objects go (the target's mapping
 * stays).  A GPU not shown done keeps every object the request may use and
 * sets the display's latch; no further GPU work or flip follows.  0, or
 * EIO.
 */
static int
i915_test_lcdd_draw(
	struct i915_lcd_kernel *k,
	unsigned index,
	unsigned variant,
	unsigned round)
{
	const struct i915_lcd_kernel_deps *d;
	struct i915_test_fhd_render *x;
	struct i915_display *display;
	int release_error;
	int error;

	d = k->d;
	x = &i915_test_lcdd_render;
	display = container_of(k, struct i915_display, lk);

	/* The draw into the premapped target. */
	kern_memset(x, 0, sizeof(*x));
	error = drv_i915_test_fhd_render_run_ex(x, d->es, d->vm, d->gm, d->mmio, d->uncore_lock, I915_TEST_GPU_DRAW_TIMEOUT_MS, i915_test_lcdd_buf[index].obj, i915_test_lcdd_map[index].va, variant, 1);
	kern_logf("i915: LCD-D draw %u: into buffer %c (not on the display) variant %u at 0x%llx | rc=%d outcome=%d gpu_done=%d | markers %08x/%08x/%08x ps %08x | pixels %u/%u (stale %u, first bad %d,%d) | tex changed %u guard bad %u | walk first/mid/last=%d | hash %016llx\n",
	    round,
	    'A' + (int)index,
	    variant,
	    (unsigned long long)i915_test_lcdd_map[index].va,
	    error,
	    x->t.outcome,
	    x->gpu_done,
	    x->marker_before,
	    x->marker_middraw,
	    x->marker_after,
	    x->ps_marker,
	    x->px_match,
	    x->px_total,
	    x->px_stale,
	    x->first_bad_x,
	    x->first_bad_y,
	    x->tex_changed_bytes,
	    x->guard_bad_bytes,
	    x->rt_walk_ok,
	    (unsigned long long)x->image_hash);

	/* The GPU is not shown done: everything stays. */
	if (x->t.submitted && !x->gpu_done) {
		drv_i915_test_fhd_render_keep(x);
		i915_test_lcdd_gpu_kept = 1;
		drv_i915_lcd_show_retain_gpu(display, d->gm, "an LCD-D draw was not shown to finish");
		return EIO;
	}

	/* The draw's own mappings, TLB and objects. */
	release_error = drv_i915_test_fhd_render_release(x, d->gm, d->vm, &i915_test_lcdd_tlb, d->es, d->mmio, d->uncore_lock);
	kern_logf("i915: LCD-D draw %u release: the draw's PTEs back to scratch %u/%u, TLB rc=%d, released=%d | the target stays mapped (%u pages at 0x%llx)\n",
	    round,
	    x->maps_scratch,
	    x->maps_total,
	    x->tlb_rc,
	    x->released,
	    i915_test_lcdd_map[index].mapped,
	    (unsigned long long)i915_test_lcdd_map[index].va);
	if (release_error != 0) {
		i915_test_lcdd_gpu_kept = 1;
		drv_i915_lcd_show_retain_gpu(display, d->gm, "an LCD-D draw's mappings / TLB could not be shown released");
		return EIO;
	}

	/* A draw that did not produce the expected image. */
	if (error != 0 || x->t.outcome != I915_TEST_EU_PASS)
		return EIO;

	/* Succeeded: the buffer holds the variant, and its image hash is recorded. */
	i915_test_lcdd_variant[index] = variant;
	i915_test_lcdd_hash[index][variant % I915_TEST_LCDD_VARIANTS] = x->image_hash;
	i915_test_lcdd_hash_set[index][variant % I915_TEST_LCDD_VARIANTS]++;
	k->draws_ok++;
	return 0;
}

/*
 * Hands buffer back to the display and flips to it.  0 when the flip is
 * complete; EINVAL when the display could not take the buffer; EIO when
 * the flip did not complete (both buffers stay in use).
 */
static int
i915_test_lcdd_flip(
	struct i915_lcd_kernel *k,
	unsigned back,
	struct i915_lcd_flip_result *flip)
{
	struct i915_display *display;
	int begin_error;
	int error;

	display = container_of(k, struct i915_display, lk);

	/* The display takes the buffer before it is armed. */
	begin_error = drv_i915_scanout_begin(&i915_test_lcdd_buf[back]);
	if (begin_error != 0)
		return EINVAL;

	/* The reference's update of the running crtc. */
	error = drv_i915_lcd_modeset_flip(display, (uint32_t)i915_test_lcdd_buf[back].surf, flip);
	if (error != I915_LCD_MS_OK || flip->result != I915_LCD_FLIP_DONE)
		return EIO;

	/* Succeeded: the new buffer is displayed. */
	return 0;
}

/*
 * Probes the evasion sleep on the real pipe.
 *
 * The ordinary flip (the same update body) is started when the scanline
 * is a chosen number of lines before the evasion window, so that
 * intel_pipe_update_start() finds itself inside it and sleeps to the next
 * vblank.  Only when to call is chosen; nothing is faked.  Bounded to
 * I915_TEST_LCDD_PROBE_MAX flips (A and B alternate, both hold valid
 * pictures); the first flip that slept and completed ends it.  None doing
 * so is NOT-REACHED: recorded, not a failure of the display test.  0, or
 * the flip's error.
 */
static int
i915_test_lcdd_evasion_probe(
	struct i915_lcd_kernel *k,
	unsigned front)
{
	struct i915_lcd_flip_result flip;
	struct i915_display *display;
	uint32_t scanline;
	uint32_t vtotal;
	uint32_t target;
	uint32_t lead;
	unsigned attempt;
	unsigned polls;
	unsigned latency;
	unsigned sleeps_before;
	unsigned back;
	int window_min;
	int window_max;
	int vblank_start;
	int window_error;
	int error;

	display = container_of(k, struct i915_display, lk);
	latency = 0U;
	scanline = 0U;
	vtotal = 0U;
	window_min = 0;
	window_max = 0;
	vblank_start = 0;

	/* The evasion window of the running mode. */
	i915_test_lcdd_probe_result = I915_TEST_LCDD_PROBE_NOT_REACHED;
	window_error = drv_i915_lcd_modeset_evade_window(display, &window_min, &window_max, &vblank_start);
	if (window_error != I915_LCD_MS_OK) {
		i915_test_lcdd_probe_result = I915_TEST_LCDD_PROBE_ERROR;
		return 0;
	}

	/* One frame's line count, from the counter itself: the highest scanline over more than a frame. */
	for (polls = 0U; polls < I915_TEST_LCDD_PROBE_POLLS; polls++) {
		scanline = drv_i915_read32(k->d->mmio, I915_TEST_PIPEDSL_A) & I915_TEST_PIPEDSL_MASK;
		if (scanline + 1U > vtotal)
			vtotal = scanline + 1U;
	}

	kern_logf("i915: LCD-D evasion probe: window scanlines %d..%d (vblank start %d), lines per frame seen %u; up to %u flips\n",
	    window_min,
	    window_max,
	    vblank_start,
	    vtotal,
	    I915_TEST_LCDD_PROBE_MAX);
	if (vtotal <= (uint32_t)window_max) {
		i915_test_lcdd_probe_result = I915_TEST_LCDD_PROBE_ERROR;
		return 0;
	}

	/* Aims each flip so that trigger plus latency lands on the window's second line. */
	for (attempt = 0U; attempt < I915_TEST_LCDD_PROBE_MAX; attempt++) {
		back = front ^ 1U;
		sleeps_before = k->vblank_sleeps;
		target = ((uint32_t)window_min + 1U + vtotal - (latency % vtotal)) % vtotal;

		/* Waits for the target scanline (or the next one). */
		for (polls = 0U; polls < I915_TEST_LCDD_PROBE_POLLS; polls++) {
			scanline = drv_i915_read32(k->d->mmio, I915_TEST_PIPEDSL_A) & I915_TEST_PIPEDSL_MASK;
			if (scanline == target || scanline == (target + 1U) % vtotal)
				break;
		}

		/* A scanline never seen: the next attempt. */
		if (polls == I915_TEST_LCDD_PROBE_POLLS) {
			kern_logf("i915: LCD-D evasion probe %u: scanline %u never seen\n", attempt, target);
			continue;
		}

		/* The flip, watching the first scanline the update body reads. */
		k->probe_first_dsl = I915_TEST_LCDD_NO_SCANLINE;
		k->probe_watch = 1;
		error = i915_test_lcdd_flip(k, back, &flip);
		k->probe_watch = 0;
		lead = 0U;
		if (k->probe_first_dsl != I915_TEST_LCDD_NO_SCANLINE)
			lead = (k->probe_first_dsl + vtotal - scanline) % vtotal;

		kern_logf("i915: LCD-D evasion probe %u: trigger scanline %u (aimed %u), first update read %u (latency %u lines), gen %u %s, evasion sleeps %u, event_rc=%d, update errors %d, IRQs off at a sleep entry %u\n",
		    attempt,
		    scanline,
		    target,
		    k->probe_first_dsl,
		    lead,
		    flip.gen,
		    flip.result == I915_LCD_FLIP_DONE ? "DONE" : "NOT-DONE",
		    k->vblank_sleeps - sleeps_before,
		    flip.event_rc,
		    flip.update_errors,
		    k->sleep_irq_off);
		if (error != 0)
			return error;

		/* The old front is no longer read. */
		drv_i915_scanout_end(&i915_test_lcdd_buf[front]);
		front = back;
		k->probe_flips++;
		i915_test_lcdd_probe_tries = attempt + 1U;

		/* An update that slept out of the window, then armed and completed, ends the probe. */
		if (k->vblank_sleeps != sleeps_before) {
			i915_test_lcdd_probe_result = I915_TEST_LCDD_PROBE_REACHED;
			i915_test_lcdd_probe_lead = lead;
			break;
		}

		/* The next attempt aims with what this one measured. */
		if (k->probe_first_dsl != I915_TEST_LCDD_NO_SCANLINE)
			latency = lead;
	}

	kern_logf("i915: LCD-D evasion probe: %s after %u flip(s)\n",
	    i915_test_lcdd_probe_result == I915_TEST_LCDD_PROBE_REACHED ?
	    "REACHED (an update slept out of the window, then armed and completed)" :
	    "NOT-REACHED (recorded; the sleep path stays unverified on this pipe)",
	    i915_test_lcdd_probe_tries);

	/* The probe ran; its result is recorded. */
	return 0;
}

/*
 * Runs the rounds of LCD-D inside the window: A is up first; each round
 * draws the back buffer (never the displayed one), flips to it and hands
 * the old front back as the next draw target; the evasion probe follows.
 */
static int
i915_test_lcdd_rounds(
	void *ctx,
	struct i915_lcd_observer *observer)
{
	struct i915_lcd_flip_result flip;
	struct i915_lcd_kernel *k;
	unsigned sleeps_before;
	unsigned variant;
	unsigned front;
	unsigned round;
	unsigned back;
	int error;

	UNUSED_PARAMETER(observer);

	k = ctx;
	front = 0U;

	/* The first picture (A) for the camera. */
	kern_logf("i915: LCD-D shown 0: buffer A variant %u (GPU-drawn) -- take the photograph\n", i915_test_lcdd_variant[0]);
	drv_i915_test_lcd_sleep_ms(k, I915_TEST_LCDD_HOLD_MS);

	/* Draws the back buffer and flips to it, round by round. */
	for (round = 1U; round <= I915_TEST_LCDD_ROUNDS; round++) {
		back = front ^ 1U;
		variant = i915_test_lcdd_seq[round];
		sleeps_before = k->vblank_sleeps;

		/* The display may still read a buffer that is not merely pinned: never a draw target. */
		if (i915_test_lcdd_buf[back].state != I915_SCANOUT_PINNED)
			return EINVAL;

		/* The GPU draws the hidden buffer. */
		error = i915_test_lcdd_draw(k, back, variant, round);
		if (error != 0)
			return EIO;

		/* The flip shows it. */
		error = i915_test_lcdd_flip(k, back, &flip);
		kern_logf("i915: LCD-D flip %u gen %u: surf 0x%08x -> 0x%08x | live 0x%08x -> 0x%08x | frame %u -> %u | event_rc=%d | update errors %d | evasion sleeps %u | result %s\n",
		    round,
		    flip.gen,
		    flip.old_surf,
		    flip.new_surf,
		    flip.live_before,
		    flip.live_after,
		    flip.frame_before,
		    flip.frame_after,
		    flip.event_rc,
		    flip.update_errors,
		    k->vblank_sleeps - sleeps_before,
		    drv_i915_test_lcd_flip_name(flip.result));
		if (error != 0)
			return error;

		/* The old front is no longer read: the next draw target. */
		drv_i915_scanout_end(&i915_test_lcdd_buf[front]);
		front = back;
		k->flips_done++;
		kern_logf("i915: LCD-D shown %u: buffer %c variant %u (surf 0x%08x) -- take the photograph\n",
		    round,
		    'A' + (int)front,
		    variant,
		    (uint32_t)i915_test_lcdd_buf[front].surf);
		drv_i915_test_lcd_sleep_ms(k, I915_TEST_LCDD_HOLD_MS);
	}

	/* The evasion probe from the buffer now on the display. */
	error = i915_test_lcdd_evasion_probe(k, front);
	if (error != 0)
		return error;

	/* Succeeded: every round drew and flipped. */
	return 0;
}

/* Keeps both LCD-D buffers for ever: the display or the GPU may still use them. */
static void
i915_test_lcdd_abandon_both(void)
{
	unsigned index;

	/* Abandons every buffer that holds pages. */
	for (index = 0U; index < 2U; index++) {
		if (i915_test_lcdd_buf[index].state >= I915_SCANOUT_PINNED && i915_test_lcdd_buf[index].state != I915_SCANOUT_ABANDONED)
			drv_i915_scanout_abandon(&i915_test_lcdd_buf[index]);
	}
}

/*
 * Reclaims the LCD-D buffers once the display provably reads neither:
 * the pixels checked, the render-target mappings taken down (TLB
 * included), then the buffers unpinned and destroyed.  A draw not shown
 * done, or a mapping not shown gone, keeps both.  Returns 1 when both
 * buffers were released.
 */
static int
i915_test_lcdd_reclaim(
	struct i915_lcd_kernel *k,
	uint32_t *bad,
	int *unmapped)
{
	const struct i915_lcd_kernel_deps *d;
	struct i915_display *display;
	int unmap_error[2];
	unsigned index;
	int error;
	int released;

	d = k->d;
	display = container_of(k, struct i915_display, lk);
	unmap_error[0] = -1;
	unmap_error[1] = -1;
	*unmapped = 0;

	/* The display provably reads neither buffer. */
	for (index = 0U; index < 2U; index++) {
		if (i915_test_lcdd_buf[index].state == I915_SCANOUT_IN_USE)
			drv_i915_scanout_end(&i915_test_lcdd_buf[index]);
	}

	/* A draw was not shown done, or its release failed: the targets, their mappings and the draw stay. */
	if (i915_test_lcdd_gpu_kept) {
		i915_test_lcdd_abandon_both();
		return 0;
	}

	/* The images the buffers hold. */
	for (index = 0U; index < 2U; index++) {
		if (i915_test_lcdd_buf[index].obj != NULL && i915_test_lcdd_buf[index].state == I915_SCANOUT_PINNED)
			bad[index] = i915_test_lcdd_verify(NULL, &i915_test_lcdd_buf[index]);
	}

	/* The run-long target mappings, back to scratch with the TLB invalidated. */
	*unmapped = 1;
	for (index = 0U; index < 2U; index++) {
		unmap_error[index] = 0;
		if (i915_test_lcdd_map[index].rt != NULL || i915_test_lcdd_map[index].mapped != 0U)
			unmap_error[index] = drv_i915_test_fhd_rt_unmap(&i915_test_lcdd_map[index], d->vm, &i915_test_lcdd_tlb, d->es, d->mmio, d->uncore_lock);
		if (unmap_error[index] != 0)
			*unmapped = 0;
	}

	kern_logf("i915: LCD-D release: A target PTEs back to scratch %u/%u rc=%d | B %u/%u rc=%d | TLB invalidations %u (timeouts %u)\n",
	    i915_test_lcdd_map[0].scratch,
	    i915_test_lcdd_map[0].mapped,
	    unmap_error[0],
	    i915_test_lcdd_map[1].scratch,
	    i915_test_lcdd_map[1].mapped,
	    unmap_error[1],
	    i915_test_lcdd_tlb.invalidations,
	    i915_test_lcdd_tlb.timeouts);

	/* A mapping not shown gone: the pages may still be translated. */
	if (!*unmapped) {
		i915_test_lcdd_abandon_both();
		drv_i915_lcd_show_retain_gpu(display, d->gm, "an LCD-D render target could not be shown unmapped");
		return 0;
	}

	/* Unpins and destroys both buffers. */
	released = 1;
	for (index = 0U; index < 2U; index++) {
		if (i915_test_lcdd_buf[index].state == I915_SCANOUT_NONE)
			continue;

		/* Unpins, then destroys what was unpinned. */
		error = drv_i915_scanout_unpin(&i915_test_lcdd_buf[index]);
		if (error == 0)
			error = drv_i915_scanout_destroy(&i915_test_lcdd_buf[index]);
		if (error != 0)
			released = 0;
	}

	/* Reports whether both went. */
	return released;
}

/* Compares every variant's image across the two buffers; returns how many matched. */
static unsigned
i915_test_lcdd_cross(void)
{
	unsigned variant;
	unsigned cross;
	int same;

	/* One line per variant. */
	cross = 0U;
	for (variant = 0U; variant < I915_TEST_LCDD_VARIANTS; variant++) {
		same = 0;
		if (i915_test_lcdd_hash_set[0][variant] != 0U &&
		    i915_test_lcdd_hash_set[1][variant] != 0U &&
		    i915_test_lcdd_hash[0][variant] == i915_test_lcdd_hash[1][variant])
			same = 1;

		if (same)
			cross++;

		kern_logf("i915: LCD-D cross-buffer variant %u: A hash %016llx (%u draw(s)) | B hash %016llx (%u draw(s)) | %s\n",
		    variant,
		    (unsigned long long)i915_test_lcdd_hash[0][variant],
		    i915_test_lcdd_hash_set[0][variant],
		    (unsigned long long)i915_test_lcdd_hash[1][variant],
		    i915_test_lcdd_hash_set[1][variant],
		    same ? "SAME" : "DIFFERENT / MISSING");
	}

	/* Reports the count. */
	return cross;
}
