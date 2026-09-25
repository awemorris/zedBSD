/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The OpRegion brightness scenario on the real panel: LCD-O.
 *
 * One picture is up while the OpRegion service runs on a SHADOW mailbox
 * (driver-owned RAM in the OpRegion format; the firmware region is never
 * mapped writable) and serves synthetic ASLE brightness requests entered
 * through the GSE entry.  Each request must reach the panel's PWM with the
 * duty of the reference's clamp_user_to_hw(), and the mailbox must carry
 * the reference's response.  Then the service is stopped, the brightness
 * restored through the normal user path, and the picture stopped.
 */

#include "lcd-run.h"
#include "scenarios.h"
#include "../../display/diagnostics.h"
#include "../../display/modeset.h"
#include "../../display/opregion.h"
#include "../../display/panel-backlight.h"
#include "../../display/scanout.h"
#include "../../ggtt.h"
#include "../../i915.h"
#include "../../memory.h"
#include "../../mmio.h"
#include "../../workqueue.h"
#include <kern/kcrt.h>

#include <kern/klog.h>
#include <kern/sched.h>

#include <uapi/errno.h>
#include <stddef.h>

/* The picture of LCD-O, and how long each brightness holds for the camera. */
#define I915_TEST_LCDO_PATTERN		131U
#define I915_TEST_LCDO_HOLD_MS		6000U

/* The size of the buffer, and the short window of the show body the requests run in. */
#define I915_TEST_LCDO_WIDTH		1920U
#define I915_TEST_LCDO_HEIGHT		1080U
#define I915_TEST_LCDO_WINDOW_MS	1000U

/* A mapping-table token, not a physical address: the shadow only. */
#define I915_TEST_LCDO_ASLS_TOKEN	0x6f000018U

/* The size of the shadow OpRegion. */
#define I915_TEST_LCDO_SHADOW_SIZE	8192U

/* The mailbox offsets the scenario reads and writes. */
#define I915_TEST_OPREGION_DRDY		0x100U
#define I915_TEST_OPREGION_ARDY		0x300U
#define I915_TEST_OPREGION_ASLC		0x304U
#define I915_TEST_OPREGION_TCHE		0x308U
#define I915_TEST_OPREGION_BCLP		0x310U
#define I915_TEST_OPREGION_CBLV		0x318U

/* The header fields of the shadow: size, version, mailboxes. */
#define I915_TEST_OPREGION_SIZE_KIB	0x10U
#define I915_TEST_OPREGION_VER_MINOR	0x16U
#define I915_TEST_OPREGION_VER_MAJOR	0x17U
#define I915_TEST_OPREGION_MBOX		0x58U

/* The mailboxes the shadow announces (ACPI, SWSCI, ASLE, VBT). */
#define I915_TEST_OPREGION_MBOXES	0x1dU

/* BCLP's valid bit, CBLV's valid bit and ASLC's SET_BACKLIGHT request. */
#define I915_TEST_BCLP_VALID		(1U << 31)
#define I915_TEST_CBLV_VALID		(1U << 31)
#define I915_TEST_ASLC_SET_BACKLIGHT	(1U << 1)

/* BXT_BLC_PWM_DUTY of controller 0. */
#define I915_TEST_PWM_DUTY		0xc8258U

/* The requests of LCD-O. */
#define I915_TEST_LCDO_STEPS		4U

/* How long the ASLE flush may wait for the worker, in ticks. */
#define I915_TEST_LCDO_FLUSH_TICKS	200U

/*
 * The buffer of LCD-O.
 *
 * It outlives a run whose stop was not confirmed; the next run refuses.
 */
static struct i915_scanout i915_test_lcdo_so;

/*
 * The show environment and report of LCD-O.
 *
 * Too large for the stack; rewritten by each run.
 */
static struct i915_lcd_show_env i915_test_lcdo_env;
static struct i915_lcd_show_report i915_test_lcdo_report;

/*
 * The shadow OpRegion the service runs on.
 *
 * Page aligned like the firmware region it stands for; rewritten at the
 * start of each run's window and kept mapped by the service until its
 * cleanup.
 */
static uint8_t i915_test_lcdo_shadow[I915_TEST_LCDO_SHADOW_SIZE] __attribute__((aligned(4096)));

/*
 * The worker queue of the OpRegion service.
 *
 * Created once and kept for the device's life (live says so): the
 * service's cleanup does not destroy the queue it was given.
 */
static struct i915_workqueue i915_test_lcdo_wq;
static int i915_test_lcdo_wq_live;

/*
 * What the run saw: the last backlight result, the requests that matched,
 * and whether the restore gave the starting duty back.  Reset by each run.
 */
static int i915_test_lcdo_backlight_rc;
static unsigned i915_test_lcdo_steps_ok;
static int i915_test_lcdo_restore_ok;

/* The OpRegion signature the service checks. */
static const char i915_test_opregion_signature[16] = {
	'I', 'n', 't', 'e', 'l', 'G', 'r', 'a', 'p', 'h', 'i', 'c', 's', 'M', 'e', 'm'
};

static uint32_t i915_test_lcdo_read(unsigned offset);
static void i915_test_lcdo_write(unsigned offset, uint32_t value);
static void i915_test_lcdo_backlight(void *ctx, uint32_t level, uint32_t max);
static uint32_t i915_test_lcdo_verify(void *ctx, const struct i915_scanout *so);
static uint32_t i915_test_lcdo_expected_duty(uint32_t level, uint32_t min, uint32_t max);
static int i915_test_lcdo_service_start(struct i915_display *display, struct i915_lcd_kernel *k);
static int i915_test_lcdo_steps(void *ctx, struct i915_lcd_observer *observer);

/*
 * Runs LCD-O: one picture, synthetic ASLE brightness requests served by
 * the OpRegion service on a shadow mailbox, the service stopped, the
 * brightness restored and the picture stopped.
 */
void
drv_i915_test_display_lcdo(
	struct i915_device *device)
{
	const struct i915_lcd_kernel_deps *d;
	struct i915_lcd_show_report *rep;
	struct i915_lcd_show_env *env;
	struct i915_display *display;
	struct i915_lcd_kernel *k;
	const char *first_anomaly;
	int retained;
	int preflight_error;
	int fill_error;
	int window_error;
	int show_error;
	int registered;
	int released;
	int held;
	int pass;
	int error;

	env = &i915_test_lcdo_env;
	rep = &i915_test_lcdo_report;
	released = 0;
	i915_test_lcdo_steps_ok = 0U;
	i915_test_lcdo_restore_ok = 0;

	/* The display to show on. */
	display = drv_i915_test_lcd_display(device, "LCD-O");
	if (display == NULL)
		return;

	/* An earlier run left resources the display may still read: refused. */
	retained = drv_i915_test_lcd_retained(display);
	if (retained || i915_test_lcdo_so.state != I915_SCANOUT_NONE) {
		kern_logf("i915: LCD-O verdict: FAIL (refused before any initialisation: retained resources)\n");
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
		kern_logf("i915: LCD-O verdict: FAIL (preflight: nothing was written)\n");
		return;
	}

	/* The display window of the GGTT; claimed earlier is fine. */
	window_error = drv_i915_gt_display_window_init(d->gm, I915_GT_DISPLAY_PAGES);
	if (window_error != 0 && window_error != EBUSY)
		return;

	/* The buffer at the panel's size. */
	error = drv_i915_scanout_create(d->gm, I915_TEST_LCDO_WIDTH, I915_TEST_LCDO_HEIGHT, I915_FOURCC_XRGB8888, I915_MOD_LINEAR, &i915_test_lcdo_so);
	if (error == 0)
		error = drv_i915_scanout_pin(&i915_test_lcdo_so, "lcd-o");
	if (error != 0) {
		kern_logf("i915: LCD-O verdict: FAIL (buffer rc=%d)\n", error);
		return;
	}

	/* The picture, published to the display. */
	(void)drv_i915_lcd_pattern_fill(i915_test_lcdo_so.cpu, i915_test_lcdo_so.pitch, i915_test_lcdo_so.width, i915_test_lcdo_so.height, I915_TEST_LCDO_PATTERN);
	drv_i915_scanout_publish(&i915_test_lcdo_so);

	/* The show environment: the requests run in the window. */
	env->hw = &k->ops;
	env->gm = d->gm;
	env->lcd = &d->edp->lcd;
	env->pipe = 0;
	env->first_frames_ms = I915_TEST_LCD_FIRST_FRAMES_MS;
	env->window_ms = I915_TEST_LCDO_WINDOW_MS;
	env->in_window = i915_test_lcdo_steps;
	env->in_window_ctx = k;
	env->at_stage = drv_i915_lcd_kernel_at_stage;
	env->at_stage_ctx = k;
	k->pattern_id = I915_TEST_LCDO_PATTERN;
	k->window_ms = env->window_ms;

	/* Shows the picture, serves the requests in the window, stops through the reference's path. */
	show_error = drv_i915_lcd_show_prepared(display, env, &i915_test_lcdo_so, i915_test_lcdo_verify, NULL, rep);
	drv_i915_lcd_log_trace(rep->trace);
	drv_i915_lcd_log_observer(&rep->obs);
	held = drv_i915_test_lcd_power_held(k);

	/* The display provably let go: the buffer goes back; otherwise it is kept for ever. */
	if (rep->display_released || !rep->display_acquired) {
		error = drv_i915_scanout_unpin(&i915_test_lcdo_so);
		if (error == 0) {
			error = drv_i915_scanout_destroy(&i915_test_lcdo_so);
			if (error == 0)
				released = 1;
		}
	} else if (i915_test_lcdo_so.state >= I915_SCANOUT_PINNED && i915_test_lcdo_so.state != I915_SCANOUT_ABANDONED) {
		drv_i915_scanout_abandon(&i915_test_lcdo_so);
	}

	/* Judges the run; the service must be unregistered again. */
	registered = drv_i915_opregion_notifier_registered(display);
	pass = 0;
	if (show_error == 0 &&
	    i915_test_lcdo_steps_ok == I915_TEST_LCDO_STEPS &&
	    i915_test_lcdo_restore_ok &&
	    released &&
	    held == 0 &&
	    rep->readback_bad_after == 0U &&
	    k->unresolved_steps == 0U &&
	    k->time_faults == 0U &&
	    !registered)
		pass = 1;

	first_anomaly = "none";
	if (rep->first_anomaly != NULL)
		first_anomaly = rep->first_anomaly;

	kern_logf("i915: LCD-O verdict: %s (ASLE steps %u/4 with the PWM duty = the reference clamp_user_to_hw, restore %s, the service unregistered, stop %s, buffer released=%d, power refs held %d, first anomaly: %s)\n",
	    pass ? "PASS" : "FAIL",
	    i915_test_lcdo_steps_ok,
	    i915_test_lcdo_restore_ok ? "OK" : "DIFFERS",
	    rep->display_released ? "confirmed" : "NOT confirmed",
	    released,
	    held,
	    first_anomaly);
}

/* Reads a 32-bit mailbox field of the shadow. */
static uint32_t
i915_test_lcdo_read(
	unsigned offset)
{
	uint32_t value;

	/* The shadow is bytes; the field may be unaligned for the compiler. */
	kern_memcpy(&value, i915_test_lcdo_shadow + offset, sizeof(value));

	/* Reports the field. */
	return value;
}

/* Writes a 32-bit mailbox field of the shadow, as the firmware would. */
static void
i915_test_lcdo_write(
	unsigned offset,
	uint32_t value)
{
	/* The shadow is bytes; the field may be unaligned for the compiler. */
	kern_memcpy(i915_test_lcdo_shadow + offset, &value, sizeof(value));
}

/* The backlight target of the service: the ACPI brightness on the running panel. */
static void
i915_test_lcdo_backlight(
	void *ctx,
	uint32_t level,
	uint32_t max)
{
	struct i915_lcd_kernel *k;
	struct i915_display *display;

	k = ctx;
	display = container_of(k, struct i915_display, lk);

	/* intel_backlight_set_acpi() on the selected screen; the result is judged by the step. */
	i915_test_lcdo_backlight_rc = drv_i915_lcd_modeset_backlight_acpi(display, level, max);
}

/* Counts the pixels of the buffer that differ from its picture, read from memory. */
static uint32_t
i915_test_lcdo_verify(
	void *ctx,
	const struct i915_scanout *so)
{
	uint32_t bad;

	UNUSED_PARAMETER(ctx);

	/* Reads what is in memory, not a stale CPU line. */
	drv_i915_gt_clflush(so->cpu, so->size);
	bad = drv_i915_lcd_pattern_verify(so->cpu, so->pitch, so->width, so->height, I915_TEST_LCDO_PATTERN, NULL, NULL);

	/* Reports the count. */
	return bad;
}

/*
 * Returns the reference's clamp_user_to_hw(): the level scaled from
 * [0, 255] to [0, max] (DIV_ROUND_CLOSEST), then clamped to [min, max].
 */
static uint32_t
i915_test_lcdo_expected_duty(
	uint32_t level,
	uint32_t min,
	uint32_t max)
{
	uint32_t hw;

	/* Scales to the hardware range. */
	hw = (uint32_t)(((uint64_t)level * (uint64_t)max + 127U) / 255U);

	/* Clamps to the panel's range. */
	if (hw < min)
		return min;
	if (hw > max)
		return max;

	/* The scaled level is inside the range. */
	return hw;
}

/*
 * Starts the OpRegion service on the shadow: a fresh header with the
 * signature, the size, the version and the mailboxes; the worker queue
 * once; setup, the video policy, the panel's backlight as target, and the
 * registration (DIDL / CADL, READY in the shadow only).  0, or EIO.
 */
static int
i915_test_lcdo_service_start(
	struct i915_display *display,
	struct i915_lcd_kernel *k)
{
	uint32_t mailboxes;
	int error;

	/* A fresh shadow in the OpRegion format. */
	mailboxes = I915_TEST_OPREGION_MBOXES;
	kern_memset(i915_test_lcdo_shadow, 0, sizeof(i915_test_lcdo_shadow));
	kern_memcpy(i915_test_lcdo_shadow, i915_test_opregion_signature, sizeof(i915_test_opregion_signature));
	i915_test_lcdo_shadow[I915_TEST_OPREGION_SIZE_KIB] = 8U;
	i915_test_lcdo_shadow[I915_TEST_OPREGION_VER_MINOR] = 1U;
	i915_test_lcdo_shadow[I915_TEST_OPREGION_VER_MAJOR] = 2U;
	kern_memcpy(i915_test_lcdo_shadow + I915_TEST_OPREGION_MBOX, &mailboxes, sizeof(mailboxes));

	/* The service's worker queue lives as long as the device. */
	if (!i915_test_lcdo_wq_live) {
		error = drv_i915_workqueue_create(&i915_test_lcdo_wq, "i915-lcdo-opregion");
		if (error != 0)
			return EIO;

		i915_test_lcdo_wq_live = 1;
	}

	/* Maps the shadow under the token. */
	error = drv_i915_opregion_shadow_map(display, I915_TEST_LCDO_ASLS_TOKEN, i915_test_lcdo_shadow, sizeof(i915_test_lcdo_shadow));
	if (error != 0)
		return EIO;

	/* intel_opregion_setup() on the shadow. */
	error = drv_i915_opregion_shadow_setup(display, I915_TEST_LCDO_ASLS_TOKEN);
	if (error != 0)
		return EIO;

	/* The ASLE service with the video policy: BCLP requests are served. */
	error = drv_i915_opregion_service_start(display, &i915_test_lcdo_wq, I915_OPREGION_POLICY_VIDEO);
	if (error != 0)
		return EIO;

	/* The panel's backlight is the target. */
	error = drv_i915_opregion_add_backlight(display, i915_test_lcdo_backlight, k);
	if (error != 0)
		return EIO;

	/* intel_opregion_register(). */
	drv_i915_opregion_register(display);

	/* Succeeded: the service runs on the shadow. */
	return 0;
}

/*
 * Runs the requests of LCD-O inside the window: the start brightness for
 * the camera, the service started on the shadow, four ASLE requests (10
 * is below the clamp), then the service stopped (gate, the reference's
 * unregister, cleanup) and the user brightness restored.
 */
static int
i915_test_lcdo_steps(
	void *ctx,
	struct i915_lcd_observer *observer)
{
	static const uint32_t levels[I915_TEST_LCDO_STEPS] = { 10U, 64U, 160U, 255U };
	struct i915_lcd_modeset_status status;
	struct i915_display *display;
	struct i915_lcd_kernel *k;
	uint32_t duty_start;
	uint32_t user_start;
	uint32_t user_max;
	uint32_t want;
	uint32_t want_cblv;
	uint32_t duty;
	unsigned started;
	unsigned finished;
	unsigned queued_new;
	unsigned queued_pending;
	unsigned step;
	int error;

	UNUSED_PARAMETER(observer);

	k = ctx;
	display = container_of(k, struct i915_display, lk);

	/* The start brightness, to restore afterwards. */
	drv_i915_lcd_modeset_status(display, &status);
	duty_start = drv_i915_read32(k->d->mmio, I915_TEST_PWM_DUTY);
	user_start = status.backlight_user;
	user_max = status.backlight_user_max;
	kern_logf("i915: LCD-O shown 0: pattern %u at the start brightness (user %u/%u, PWM duty 0x%x, min %u max %u) -- take the photograph\n",
	    I915_TEST_LCDO_PATTERN,
	    user_start,
	    user_max,
	    duty_start,
	    status.backlight_min,
	    status.backlight_max);
	drv_i915_test_lcd_sleep_ms(k, I915_TEST_LCDO_HOLD_MS);

	/* The service on the shadow mailbox. */
	error = i915_test_lcdo_service_start(display, k);
	if (error != 0)
		return error;

	kern_logf("i915: LCD-O service: mailbox_backend=%s service_epoch=%u event_source=SYNTHETIC(GSE entry) display_backend=HARDWARE | shadow ARDY %u DRDY %u TCHE %u | real_opregion_write_count=0 (the firmware region is never mapped writable)\n",
	    drv_i915_opregion_mailbox_backend(display),
	    drv_i915_opregion_service_epoch(display),
	    i915_test_lcdo_read(I915_TEST_OPREGION_ARDY),
	    i915_test_lcdo_read(I915_TEST_OPREGION_DRDY),
	    i915_test_lcdo_read(I915_TEST_OPREGION_TCHE));

	/* One synthetic request per level, each checked on the mailbox and the PWM. */
	for (step = 0U; step < I915_TEST_LCDO_STEPS; step++) {
		want = i915_test_lcdo_expected_duty(levels[step], status.backlight_min, status.backlight_max);
		want_cblv = ((levels[step] * 100U + 254U) / 255U) | I915_TEST_CBLV_VALID;

		/* The firmware's request: BCLP valid with the level, ASLC SET_BACKLIGHT, then the GSE. */
		i915_test_lcdo_backlight_rc = -1;
		i915_test_lcdo_write(I915_TEST_OPREGION_BCLP, I915_TEST_BCLP_VALID | levels[step]);
		i915_test_lcdo_write(I915_TEST_OPREGION_ASLC, I915_TEST_ASLC_SET_BACKLIGHT);
		drv_i915_opregion_gse_entry(display);
		(void)drv_i915_opregion_asle_flush(display, sched_ticks() + I915_TEST_LCDO_FLUSH_TICKS);

		/* What the service answered and what the PWM runs now. */
		duty = drv_i915_read32(k->d->mmio, I915_TEST_PWM_DUTY);
		drv_i915_opregion_worker_stats_get(display, &started, &finished, &queued_new, &queued_pending);
		kern_logf("i915: LCD-O step %u: request BCLP %u/255 | response ASLC 0x%x CBLV 0x%x (want 0x%x) | backlight rc %d | PWM duty 0x%x (%u) want %u (reference clamp_user_to_hw: scale to [0, %u], clamp at %u) | worker started %u finished %u\n",
		    step + 1U,
		    levels[step],
		    i915_test_lcdo_read(I915_TEST_OPREGION_ASLC),
		    i915_test_lcdo_read(I915_TEST_OPREGION_CBLV),
		    want_cblv,
		    i915_test_lcdo_backlight_rc,
		    duty,
		    duty,
		    want,
		    status.backlight_max,
		    status.backlight_min,
		    started,
		    finished);

		/* The request matched: served once, answered, and the reference's duty on the PWM. */
		if (i915_test_lcdo_backlight_rc == I915_LCD_MS_OK &&
		    i915_test_lcdo_read(I915_TEST_OPREGION_ASLC) == 0U &&
		    i915_test_lcdo_read(I915_TEST_OPREGION_CBLV) == want_cblv &&
		    duty == want &&
		    started == step + 1U &&
		    finished == step + 1U)
			i915_test_lcdo_steps_ok++;

		kern_logf("i915: LCD-O shown %u: brightness %u/255 via the synthetic ASLE request -- take the photograph\n",
		    step + 1U,
		    levels[step]);
		drv_i915_test_lcd_sleep_ms(k, I915_TEST_LCDO_HOLD_MS);
	}

	/* Stops the service (gate, the reference's unregister, cleanup), then restores through the user path. */
	drv_i915_opregion_unregister(display);
	(void)drv_i915_opregion_cleanup(display);
	(void)drv_i915_lcd_modeset_brightness(display, user_start, user_max);
	duty = drv_i915_read32(k->d->mmio, I915_TEST_PWM_DUTY);
	i915_test_lcdo_restore_ok = 0;
	if (duty == duty_start)
		i915_test_lcdo_restore_ok = 1;

	kern_logf("i915: LCD-O restore: user %u/%u -> PWM duty 0x%x (start 0x%x) %s | service: backend %s, notifier registered %d\n",
	    user_start,
	    user_max,
	    duty,
	    duty_start,
	    i915_test_lcdo_restore_ok ? "OK" : "DIFFERS",
	    drv_i915_opregion_mailbox_backend(display),
	    drv_i915_opregion_notifier_registered(display));

	/* The window ran; the steps are judged by the verdict. */
	return 0;
}
