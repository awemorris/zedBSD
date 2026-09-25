/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The synchronous flip scenario: LCD-C.
 *
 * Two buffers the CPU prepared, with separate backing and separate GGTT
 * placement, one modeset, and synchronous flips A -> B -> A -> B -> A
 * through the reference's update of a running crtc.  A flip is complete
 * only when its event completed and the pipe's live surface is the new
 * buffer; only then does the old buffer go back to its owner.  Then the
 * reference's stop path, and both buffers released.
 */

#include "lcd-run.h"
#include "scenarios.h"
#include "../../display/diagnostics.h"
#include "../../display/modeset.h"
#include "../../display/scanout.h"
#include "../../ggtt.h"
#include "../../i915.h"
#include "../../memory.h"
#include <kern/kcrt.h>

#include <kern/klog.h>

#include <uapi/errno.h>
#include <stddef.h>

/* The two pictures of LCD-C. */
#define I915_TEST_LCDC_PATTERN_A	121U
#define I915_TEST_LCDC_PATTERN_B	122U

/* How long each picture stays up for the camera. */
#define I915_TEST_LCDC_HOLD_MS		8000U

/* The flips of LCD-C: B, A, B, A after A was shown first. */
#define I915_TEST_LCDC_FLIPS		4U

/* The size of the two buffers. */
#define I915_TEST_LCDC_WIDTH		1920U
#define I915_TEST_LCDC_HEIGHT		1080U

/* The short window of the show body; the flips run inside it. */
#define I915_TEST_LCDC_WINDOW_MS	1000U

/*
 * The two buffers of LCD-C.
 *
 * They outlive a run whose stop was not confirmed (abandoned in place);
 * the next run finds them in use and refuses.
 */
static struct i915_scanout i915_test_lcdc_a;
static struct i915_scanout i915_test_lcdc_b;

/*
 * The show environment and report of LCD-C.
 *
 * Too large for the stack; rewritten by each run.
 */
static struct i915_lcd_show_env i915_test_lcdc_env;
static struct i915_lcd_show_report i915_test_lcdc_report;

static uint32_t i915_test_lcdc_verify_a(void *ctx, const struct i915_scanout *so);
static int i915_test_lcdc_flips(void *ctx, struct i915_lcd_observer *observer);
static int i915_test_lcdc_buffer(struct i915_gt_mem *gm, struct i915_scanout *so, const char *owner, unsigned pattern);
static int i915_test_lcdc_release(struct i915_scanout *so);

/*
 * Runs LCD-C: one modeset, four synchronous flips between two buffers,
 * the stop path, both buffers released.
 */
void
drv_i915_test_display_lcdc(
	struct i915_device *device)
{
	const struct i915_lcd_kernel_deps *d;
	struct i915_lcd_show_report *rep;
	struct i915_lcd_show_env *env;
	struct i915_display *display;
	struct i915_lcd_kernel *k;
	const char *first_anomaly;
	uint32_t bad_a;
	uint32_t bad_b;
	int retained;
	int preflight_error;
	int fill_error;
	int window_error;
	int show_error;
	int released;
	int overlap;
	int held;
	int pass;
	int error;

	env = &i915_test_lcdc_env;
	rep = &i915_test_lcdc_report;
	bad_a = 0U;
	bad_b = 0U;
	released = 0;

	/* The display to show on. */
	display = drv_i915_test_lcd_display(device, "LCD-C");
	if (display == NULL)
		return;

	/* An earlier run left resources the display may still read: refused. */
	retained = drv_i915_test_lcd_retained(display);
	if (retained ||
	    i915_test_lcdc_a.state != I915_SCANOUT_NONE ||
	    i915_test_lcdc_b.state != I915_SCANOUT_NONE) {
		kern_logf("i915: LCD-C verdict: FAIL (refused before any initialisation: retained resources)\n");
		return;
	}

	/* The run; the hardware must be idle and the inputs complete. */
	k = drv_i915_test_lcd_start(display, NULL);
	d = k->d;
	kern_memset(env, 0, sizeof(*env));
	preflight_error = drv_i915_lcd_kernel_preflight(k);
	fill_error = 0;
	if (preflight_error == 0)
		fill_error = drv_i915_lcd_kernel_fill_cfg(k, &env->cfg);
	if (preflight_error != 0 || fill_error != 0) {
		kern_logf("i915: LCD-C verdict: FAIL (preflight: nothing was written)\n");
		return;
	}

	/* The display window of the GGTT; claimed earlier is fine. */
	window_error = drv_i915_gt_display_window_init(d->gm, I915_GT_DISPLAY_PAGES);
	if (window_error != 0 && window_error != EBUSY)
		return;

	/* Two buffers: separate backing, separate GGTT placement (guards and alignment each). */
	error = i915_test_lcdc_buffer(d->gm, &i915_test_lcdc_a, "lcd-c A", I915_TEST_LCDC_PATTERN_A);
	if (error == 0)
		error = i915_test_lcdc_buffer(d->gm, &i915_test_lcdc_b, "lcd-c B", I915_TEST_LCDC_PATTERN_B);
	if (error != 0) {
		kern_logf("i915: LCD-C verdict: FAIL (buffers rc=%d)\n", error);
		return;
	}

	/* The two GGTT ranges, guards included, must not overlap. */
	overlap = 1;
	if (i915_test_lcdc_a.obj->ggtt_page + i915_test_lcdc_a.obj->pages + i915_test_lcdc_a.guard_pages <= i915_test_lcdc_b.obj->ggtt_page - i915_test_lcdc_b.guard_pages ||
	    i915_test_lcdc_b.obj->ggtt_page + i915_test_lcdc_b.obj->pages + i915_test_lcdc_b.guard_pages <= i915_test_lcdc_a.obj->ggtt_page - i915_test_lcdc_a.guard_pages)
		overlap = 0;

	kern_logf("i915: LCD-C buffers: A surf 0x%08llx ggtt page %u (+%u, guard %u) | B surf 0x%08llx ggtt page %u (+%u) | overlap=%d\n",
	    (unsigned long long)i915_test_lcdc_a.surf,
	    i915_test_lcdc_a.obj->ggtt_page,
	    i915_test_lcdc_a.obj->pages,
	    i915_test_lcdc_a.guard_pages,
	    (unsigned long long)i915_test_lcdc_b.surf,
	    i915_test_lcdc_b.obj->ggtt_page,
	    i915_test_lcdc_b.obj->pages,
	    overlap);
	k->flip_a = &i915_test_lcdc_a;
	k->flip_b = &i915_test_lcdc_b;

	/* The show environment: buffer A first, the flips in the window. */
	env->hw = &k->ops;
	env->gm = d->gm;
	env->lcd = &d->edp->lcd;
	env->pipe = 0;
	env->first_frames_ms = I915_TEST_LCD_FIRST_FRAMES_MS;
	env->window_ms = I915_TEST_LCDC_WINDOW_MS;
	env->in_window = i915_test_lcdc_flips;
	env->in_window_ctx = k;
	env->at_stage = drv_i915_lcd_kernel_at_stage;
	env->at_stage_ctx = k;
	k->pattern_id = I915_TEST_LCDC_PATTERN_A;
	k->window_ms = env->window_ms;

	/* Shows buffer A, flips in the window, and stops through the reference's path. */
	show_error = drv_i915_lcd_show_prepared(display, env, &i915_test_lcdc_a, i915_test_lcdc_verify_a, NULL, rep);
	drv_i915_lcd_log_trace(rep->trace);
	drv_i915_lcd_log_observer(&rep->obs);
	held = drv_i915_test_lcd_power_held(k);

	/* The display provably reads neither buffer: both go back; otherwise both are kept for ever. */
	if (rep->display_released || !rep->display_acquired) {
		drv_i915_scanout_end(&i915_test_lcdc_b);
		drv_i915_gt_clflush(i915_test_lcdc_b.cpu, i915_test_lcdc_b.size);
		bad_b = drv_i915_lcd_pattern_verify(i915_test_lcdc_b.cpu, i915_test_lcdc_b.pitch, i915_test_lcdc_b.width, i915_test_lcdc_b.height, I915_TEST_LCDC_PATTERN_B, NULL, NULL);
		bad_a = rep->readback_bad_after;

		/* Releases A, then B. */
		released = 0;
		error = i915_test_lcdc_release(&i915_test_lcdc_a);
		if (error == 0) {
			error = i915_test_lcdc_release(&i915_test_lcdc_b);
			if (error == 0)
				released = 1;
		}
	} else if (i915_test_lcdc_b.state >= I915_SCANOUT_PINNED && i915_test_lcdc_b.state != I915_SCANOUT_ABANDONED) {
		drv_i915_scanout_abandon(&i915_test_lcdc_b);
	}

	/* Judges the run. */
	pass = 0;
	if (show_error == 0 &&
	    k->flips_done == I915_TEST_LCDC_FLIPS &&
	    released &&
	    held == 0 &&
	    bad_a == 0U &&
	    bad_b == 0U &&
	    k->unresolved_steps == 0U &&
	    k->time_faults == 0U)
		pass = 1;

	first_anomaly = "none";
	if (rep->first_anomaly != NULL)
		first_anomaly = rep->first_anomaly;

	kern_logf("i915: LCD-C verdict: %s (flips done %u/4, one modeset, stop %s, both buffers released=%d, pixels after: A bad %u B bad %u, power refs held %d, first anomaly: %s)\n",
	    pass ? "PASS" : "FAIL",
	    k->flips_done,
	    rep->display_released ? "confirmed" : "NOT confirmed",
	    released,
	    bad_a,
	    bad_b,
	    held,
	    first_anomaly);
}

/* Counts the pixels of buffer A that differ from its picture, read from memory. */
static uint32_t
i915_test_lcdc_verify_a(
	void *ctx,
	const struct i915_scanout *so)
{
	uint32_t bad;

	UNUSED_PARAMETER(ctx);

	/* Reads what is in memory, not a stale CPU line. */
	drv_i915_gt_clflush(so->cpu, so->size);
	bad = drv_i915_lcd_pattern_verify(so->cpu, so->pitch, so->width, so->height, I915_TEST_LCDC_PATTERN_A, NULL, NULL);

	/* Reports the count. */
	return bad;
}

/*
 * Runs the flips of LCD-C inside the window: A is up first, then B, A,
 * B, A, each held for the camera.  A flip that is not complete leaves
 * both buffers in use and ends the window; the stop path decides.
 */
static int
i915_test_lcdc_flips(
	void *ctx,
	struct i915_lcd_observer *observer)
{
	struct i915_lcd_flip_result flip;
	struct i915_scanout *sequence[I915_TEST_LCDC_FLIPS];
	struct i915_lcd_kernel *k;
	struct i915_display *display;
	struct i915_scanout *to;
	struct i915_scanout *from;
	unsigned index;
	int begin_error;
	int error;

	UNUSED_PARAMETER(observer);

	k = ctx;
	display = container_of(k, struct i915_display, lk);

	/* B, A, B, A. */
	sequence[0] = k->flip_b;
	sequence[1] = k->flip_a;
	sequence[2] = k->flip_b;
	sequence[3] = k->flip_a;

	/* The first picture (A) for the camera. */
	kern_logf("i915: LCD-C shown 0: pattern %u (buffer A, surf 0x%08x) -- take the photograph\n",
	    I915_TEST_LCDC_PATTERN_A,
	    (uint32_t)k->flip_a->surf);
	drv_i915_test_lcd_sleep_ms(k, I915_TEST_LCDC_HOLD_MS);

	/* Flips to each buffer in turn. */
	for (index = 0U; index < I915_TEST_LCDC_FLIPS; index++) {
		to = sequence[index];
		from = k->flip_a;
		if (to == k->flip_a)
			from = k->flip_b;

		/* The display takes the new buffer before it is armed. */
		if (to->state == I915_SCANOUT_PINNED) {
			begin_error = drv_i915_scanout_begin(to);
			if (begin_error != 0)
				return EINVAL;
		}

		/* The reference's update of the running crtc. */
		error = drv_i915_lcd_modeset_flip(display, (uint32_t)to->surf, &flip);
		kern_logf("i915: LCD-C flip %u gen %u: surf 0x%08x -> 0x%08x | live 0x%08x -> 0x%08x | frame %u -> %u | event rc=%d | update errors %d | vblank sleeps %u | result %s\n",
		    index + 1U,
		    flip.gen,
		    flip.old_surf,
		    flip.new_surf,
		    flip.live_before,
		    flip.live_after,
		    flip.frame_before,
		    flip.frame_after,
		    flip.event_rc,
		    flip.update_errors,
		    k->vblank_sleeps,
		    drv_i915_test_lcd_flip_name(flip.result));
		if (error != I915_LCD_MS_OK || flip.result != I915_LCD_FLIP_DONE)
			return EIO;

		/* The old buffer is no longer read by the display: it goes back to its owner. */
		drv_i915_scanout_end(from);
		k->flips_done++;
		kern_logf("i915: LCD-C shown %u: pattern %u (buffer %c, surf 0x%08x) -- take the photograph\n",
		    index + 1U,
		    to == k->flip_a ? I915_TEST_LCDC_PATTERN_A : I915_TEST_LCDC_PATTERN_B,
		    to == k->flip_a ? 'A' : 'B',
		    (uint32_t)to->surf);
		drv_i915_test_lcd_sleep_ms(k, I915_TEST_LCDC_HOLD_MS);
	}

	/* Succeeded: every flip completed. */
	return 0;
}

/* Creates, pins, fills and publishes one LCD-C buffer: 0, or the failing step's error. */
static int
i915_test_lcdc_buffer(
	struct i915_gt_mem *gm,
	struct i915_scanout *so,
	const char *owner,
	unsigned pattern)
{
	int error;

	/* The buffer at the panel's size. */
	error = drv_i915_scanout_create(gm, I915_TEST_LCDC_WIDTH, I915_TEST_LCDC_HEIGHT, I915_FOURCC_XRGB8888, I915_MOD_LINEAR, so);
	if (error != 0)
		return error;

	/* Pins it for the display. */
	error = drv_i915_scanout_pin(so, owner);
	if (error != 0)
		return error;

	/* The picture, published to the display. */
	(void)drv_i915_lcd_pattern_fill(so->cpu, so->pitch, so->width, so->height, pattern);
	drv_i915_scanout_publish(so);

	/* Succeeded: the buffer is ready for the display. */
	return 0;
}

/* Unpins and destroys one LCD-C buffer: 0, or the failing step's error. */
static int
i915_test_lcdc_release(
	struct i915_scanout *so)
{
	int error;

	/* Unpins it. */
	error = drv_i915_scanout_unpin(so);
	if (error != 0)
		return error;

	/* Destroys it. */
	error = drv_i915_scanout_destroy(so);
	if (error != 0)
		return error;

	/* Succeeded: the buffer is gone. */
	return 0;
}
