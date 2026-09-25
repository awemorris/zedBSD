/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The external HDMI display scenarios: HDMI-B, DUAL and DUAL-SHARED.
 *
 * HDMI-B shows one picture on the HDMI display of DDI B (pipe B) and
 * stops it.  DUAL lights the panel (pipe A) and the HDMI display at once,
 * each with its own picture, then stops the HDMI display while the panel
 * keeps running: one screen's stop must not take the other's PLL, power
 * domains or buffer with it.  DUAL-SHARED feeds both pipes from one
 * buffer, which only the last of them may release.
 *
 * The HDMI mode is CEA-861 format 4 (1280x720p60, 74.25 MHz), which every
 * HDMI sink supports.  The reference would take the sink's preferred mode
 * from its EDID, which this machine's DDC does not answer with the port
 * off (Linux sees the same); the mode and `connected` are given here, an
 * adaptation of the scenario, not of the driver.
 */

#include "lcd-run.h"
#include "scenarios.h"
#include "../../display/clock.h"
#include "../../display/diagnostics.h"
#include "../../display/hdmi.h"
#include "../../display/hotplug.h"
#include "../../display/modeset.h"
#include "../../display/scanout.h"
#include "../../display/state.h"
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

/* How long the HDMI-B picture and the two DUAL pictures stay up. */
#ifndef I915_TEST_HDMIB_WINDOW_MS
#define I915_TEST_HDMIB_WINDOW_MS	20000U
#endif
#ifndef I915_TEST_DUAL_WINDOW_MS
#define I915_TEST_DUAL_WINDOW_MS	20000U
#endif

/* The reference clock the HDMI PLL is computed from (kHz, non-SSC). */
#define I915_TEST_HDMI_REF_KHZ		38400

/* The HDMI output: port B, pipe B, transcoder B. */
#define I915_TEST_HDMI_PORT		1
#define I915_TEST_HDMI_PIPE		1

/* The second picture of DUAL. */
#define I915_TEST_DUAL_HDMI_PATTERN	111U

/* PIPE_FRMCOUNT_G4X of pipe A; the pipes are 0x1000 apart. */
#define I915_TEST_PIPE_FRAME_A		0x70040U
#define I915_TEST_PIPE_STRIDE		0x1000U

/* How long the DUAL windows log, and how long the panel runs alone for the camera. */
#define I915_TEST_DUAL_LOG_MS		5000U
#define I915_TEST_DUAL_FRAME_MS		500U
#define I915_TEST_DUAL_ALONE_MS		3000U
#define I915_TEST_DUAL_SHARE_ROUNDS	3U

/*
 * One screen of the two-screen scenarios.
 *
 * It names the modeset object and pipe it drives, the buffer it reads
 * (its own, or one it shares with the other screen) and what its bring-up
 * and stop returned.  The scenarios keep both screens in file-scope
 * storage: an abandoned buffer outlives the run.
 */
struct i915_test_dual_screen {
	/* The modeset object (0 = the panel, 1 = the HDMI display). */
	unsigned idx;

	/* The name the screen is logged under, and its pipe. */
	const char *name;
	int pipe;

	/* The screen's own buffer, and the one it reads (its own or the shared one). */
	struct i915_scanout so;
	struct i915_scanout *sop;

	/* The modeset configuration and state of the screen. */
	struct i915_lcd_modeset_cfg cfg;
	const struct i915_lcd_state *state;

	/* The picture and its hash. */
	unsigned pattern_id;
	uint64_t pattern_hash;

	/* What the bring-up and the stop returned, and what the screen holds. */
	int prepare_rc, enable_rc, disable_rc, begun, armed, released;
	uint32_t frame_first, frame_last;
};

/*
 * The two screens of DUAL and DUAL-SHARED, and the one shared buffer.
 *
 * Rewritten by each run; a buffer whose stop was not confirmed stays in
 * them for the device's life.
 */
static struct i915_test_dual_screen i915_test_dual_a;
static struct i915_test_dual_screen i915_test_dual_b;
static struct i915_scanout i915_test_dual_shared;

/*
 * The HDMI state computed from the CEA mode.
 *
 * Filled by each scenario before its run; the modeset object keeps a
 * pointer to it while the screen runs.
 */
static struct i915_lcd_state i915_test_hdmi_state;

/* CEA-861 format 4: 1280x720 at 60 Hz, 74.25 MHz, positive syncs, 8 bits per colour. */
static const struct i915_lcd_mode i915_test_hdmi_cea4 = {
	.clock_khz = 74250,
	.hdisplay = 1280,
	.hsync_start = 1390,
	.hsync_end = 1430,
	.htotal = 1650,
	.vdisplay = 720,
	.vsync_start = 725,
	.vsync_end = 730,
	.vtotal = 750,
	.hsync_positive = 1,
	.vsync_positive = 1,
	.edid_bpc = 8,
};

static int i915_test_hdmib_window(void *ctx, struct i915_lcd_observer *observer);
static uint32_t i915_test_dual_frame(const struct i915_lcd_kernel_deps *d, int pipe);
static int i915_test_dual_begin(struct i915_display *display, const char *tag, struct i915_lcd_kernel **k_out);
static int i915_test_dual_configure(struct i915_lcd_kernel *k, const char *tag);
static int i915_test_dual_bring_up(struct i915_display *display, struct i915_lcd_kernel *k, struct i915_test_dual_screen *sc);
static int i915_test_dual_enable(struct i915_display *display, struct i915_lcd_kernel *k, struct i915_test_dual_screen *sc);
static void i915_test_dual_log_state(struct i915_display *display, const char *when, struct i915_test_dual_screen *sc);
static int i915_test_dual_stop(struct i915_display *display, struct i915_test_dual_screen *sc);
static void i915_test_dual_release(struct i915_test_dual_screen *sc);
static int i915_test_dual_panel_kept(struct i915_display *display);

/*
 * Runs HDMI-B: one picture on the external HDMI display of DDI B, the
 * sink's EDID read while it is lit, then the stop path.
 */
void
drv_i915_test_display_hdmib(
	struct i915_device *device)
{
	struct i915_lcd_run_params params;
	struct i915_display *display;
	int error;

	/* The display to show on. */
	display = drv_i915_test_lcd_display(device, "HDMI-B");
	if (display == NULL)
		return;

	/* The HDMI state of the CEA mode, with its WRPLL. */
	error = drv_i915_lcd_compute_hdmi(&i915_test_hdmi_cea4, I915_TEST_HDMI_REF_KHZ, &i915_test_hdmi_state);
	if (error != 0) {
		kern_logf("i915: HDMI-B verdict: FAIL (the WRPLL calculation refused the TMDS clock: rc=%d)\n", error);
		return;
	}

	/* A single-screen run on port B, pipe B: the PLL pool starts empty. */
	kern_memset(&params, 0, sizeof(params));
	params.output_hdmi = 1;
	params.port = I915_TEST_HDMI_PORT;
	params.pipe = I915_TEST_HDMI_PIPE;
	params.cpu_transcoder = I915_TEST_HDMI_PIPE;
	params.dpll_id = 0;
	params.reset_dplls = 1;
	params.state = &i915_test_hdmi_state;
	params.tag = "HDMI-B";

	/* The picture's hash is pinned at 1920x1080 only. */
	params.pattern_id = I915_TEST_LCDB_PATTERN_ID;
	params.pattern_fnv = 0ULL;
	params.window_ms = I915_TEST_HDMIB_WINDOW_MS;
	params.in_window = i915_test_hdmib_window;

	/* One picture on the HDMI display. */
	error = drv_i915_test_lcd_run_one(display, &params);
	kern_logf("i915: HDMI-B verdict: %s (the LCD-B lines above carry the detail; the photograph is separate evidence)\n",
	    error == 0 ? "PASS" : "FAIL");
}

/*
 * Runs DUAL: the panel and the external HDMI display at once, each with
 * its own picture, then the HDMI display stopped while the panel keeps
 * running with everything it owns.
 */
void
drv_i915_test_display_dual(
	struct i915_device *device)
{
	struct i915_test_dual_screen *a;
	struct i915_test_dual_screen *b;
	struct i915_display *display;
	struct i915_lcd_kernel *k;
	uint32_t frame_a0;
	uint32_t frame_b0;
	uint32_t frame_a1;
	uint32_t frame_b1;
	uint32_t frame_a2;
	uint32_t frame_a3;
	unsigned t;
	int error;
	int kept;
	int pass;

	a = &i915_test_dual_a;
	b = &i915_test_dual_b;
	pass = 1;

	/* The display to show on. */
	display = drv_i915_test_lcd_display(device, "DUAL");
	if (display == NULL)
		return;

	/* The run, the empty PLL pool and the display window. */
	error = i915_test_dual_begin(display, "DUAL", &k);
	if (error != 0)
		return;

	/* The panel and the HDMI screen, each with its own buffer. */
	a->pattern_id = I915_TEST_LCDB_PATTERN_ID;
	b->pattern_id = I915_TEST_DUAL_HDMI_PATTERN;
	a->sop = &a->so;
	b->sop = &b->so;
	error = i915_test_dual_configure(k, "DUAL");
	if (error != 0)
		return;

	kern_logf("i915: DUAL: LCD %ux%u pattern %u on pipe A | HDMI %ux%u pattern %u on pipe B (VBT level shift %d)\n",
	    a->state->mode.hdisplay,
	    a->state->mode.vdisplay,
	    a->pattern_id,
	    b->state->mode.hdisplay,
	    b->state->mode.vdisplay,
	    b->pattern_id,
	    b->cfg.vbt_hdmi_level_shift);

	/* Both screens up; a screen that did not come up ends the run. */
	error = i915_test_dual_bring_up(display, k, a);
	if (error == 0)
		error = i915_test_dual_bring_up(display, k, b);
	if (error != 0) {
		pass = 0;
		goto stop;
	}

	i915_test_dual_log_state(display, "both up", a);
	i915_test_dual_log_state(display, "both up", b);

	/* Both pipes must be scanning out. */
	frame_a0 = i915_test_dual_frame(k->d, 0);
	frame_b0 = i915_test_dual_frame(k->d, 1);
	drv_i915_test_lcd_sleep_ms(k, I915_TEST_DUAL_FRAME_MS);
	frame_a1 = i915_test_dual_frame(k->d, 0);
	frame_b1 = i915_test_dual_frame(k->d, 1);
	a->frame_first = frame_a0;
	b->frame_first = frame_b0;
	kern_logf("i915: DUAL frames after enable: pipe A %u -> %u, pipe B %u -> %u\n",
	    frame_a0,
	    frame_a1,
	    frame_b0,
	    frame_b1);
	if (frame_a1 == frame_a0 || frame_b1 == frame_b0) {
		kern_logf("i915: DUAL: a pipe's frame counter does not advance\n");
		pass = 0;
	}

	/* The window: both pictures are up (the photograph is separate evidence). */
	for (t = 0U; t < I915_TEST_DUAL_WINDOW_MS / I915_TEST_DUAL_LOG_MS; t++) {
		drv_i915_test_lcd_sleep_ms(k, I915_TEST_DUAL_LOG_MS);
		kern_logf("i915: DUAL window t=%us: pipe A frame %u, pipe B frame %u\n",
		    (t + 1U) * (I915_TEST_DUAL_LOG_MS / 1000U),
		    i915_test_dual_frame(k->d, 0),
		    i915_test_dual_frame(k->d, 1));
	}

	/* The external display stops; the panel must keep running. */
	error = i915_test_dual_stop(display, b);
	if (error != 0)
		pass = 0;

	frame_a2 = i915_test_dual_frame(k->d, 0);
	drv_i915_test_lcd_sleep_ms(k, I915_TEST_DUAL_FRAME_MS);
	frame_a3 = i915_test_dual_frame(k->d, 0);
	a->frame_last = frame_a3;
	b->frame_last = i915_test_dual_frame(k->d, 1);
	i915_test_dual_log_state(display, "after the HDMI stop", a);
	kern_logf("i915: DUAL after the HDMI stop: pipe A frame %u -> %u (keeps running), pipe B frame %u (stopped)\n",
	    frame_a2,
	    frame_a3,
	    b->frame_last);
	if (frame_a3 == frame_a2) {
		kern_logf("i915: DUAL: the panel stopped when the external display did\n");
		pass = 0;
	}

	/* The panel still owns its crtc, plane, PLL and power domains. */
	kept = i915_test_dual_panel_kept(display);
	if (!kept) {
		kern_logf("i915: DUAL: the panel lost its own resources when the external display stopped\n");
		pass = 0;
	}

	/* The panel alone, for the photograph. */
	drv_i915_test_lcd_sleep_ms(k, I915_TEST_DUAL_ALONE_MS);

stop:
	/* The panel's stop, then both buffers back. */
	if (a->armed) {
		error = i915_test_dual_stop(display, a);
		if (error != 0)
			pass = 0;
	}

	if (b->begun && b->disable_rc != I915_LCD_MS_OK)
		pass = 0;

	if (a->so.state != I915_SCANOUT_NONE)
		i915_test_dual_release(a);

	if (b->so.state != I915_SCANOUT_NONE)
		i915_test_dual_release(b);

	if (!a->released || !b->released)
		pass = 0;

	kern_logf("i915: DUAL verdict: %s (LCD: enable rc=%d disable rc=%d released=%d | HDMI: enable rc=%d disable rc=%d released=%d | the photograph is separate evidence)\n",
	    pass ? "PASS" : "FAIL",
	    a->enable_rc,
	    a->disable_rc,
	    a->released,
	    b->enable_rc,
	    b->disable_rc,
	    b->released);

	/* The panel's modeset object is the selected one again. */
	(void)drv_i915_lcd_modeset_select(display, 0U);
}

/*
 * Runs DUAL-SHARED: one buffer feeds both pipes.
 *
 * The panel shows all of it; the HDMI display reads the same address with
 * the same pitch, so it shows the top-left 1280x720 of the same rows -- a
 * partial view, not a scaled mirror (that needs a pipe scaler this path
 * does not program).  The buffer belongs to both screens at once, and only
 * the last of them releases it.
 */
void
drv_i915_test_display_dual_share(
	struct i915_device *device)
{
	struct i915_test_dual_screen *a;
	struct i915_test_dual_screen *b;
	struct i915_scanout *shared;
	struct i915_display *display;
	struct i915_lcd_kernel *k;
	uint32_t frame_a0;
	uint32_t frame_b0;
	uint32_t frame_a1;
	uint32_t frame_b1;
	unsigned t;
	int unpin_while_used;
	int unpin_error;
	int destroy_error;
	int error;
	int pass;

	a = &i915_test_dual_a;
	b = &i915_test_dual_b;
	shared = &i915_test_dual_shared;
	pass = 1;

	/* The display to show on. */
	display = drv_i915_test_lcd_display(device, "DUAL-SHARED");
	if (display == NULL)
		return;

	/* The shared storage must be free: an earlier buffer may have been abandoned in it. */
	if (shared->state != I915_SCANOUT_NONE) {
		kern_logf("i915: DUAL-SHARED verdict: FAIL (an earlier run's resources are retained)\n");
		return;
	}

	/* The run, the empty PLL pool and the display window. */
	error = i915_test_dual_begin(display, "DUAL-SHARED", &k);
	if (error != 0)
		return;

	/* Both screens read the same buffer. */
	a->pattern_id = I915_TEST_LCDB_PATTERN_ID;
	b->pattern_id = I915_TEST_LCDB_PATTERN_ID;
	a->sop = shared;
	b->sop = shared;
	error = i915_test_dual_configure(k, "DUAL-SHARED");
	if (error != 0)
		return;

	/* One buffer, the panel's size. */
	error = drv_i915_scanout_create(k->d->gm, (uint32_t)a->state->mode.hdisplay, (uint32_t)a->state->mode.vdisplay, I915_FOURCC_XRGB8888, I915_MOD_LINEAR, shared);
	if (error == 0) {
		/* Pins it; a buffer that cannot be pinned is destroyed again. */
		error = drv_i915_scanout_pin(shared, "dual-shared");
		if (error != 0)
			(void)drv_i915_scanout_destroy(shared);
	}

	if (error != 0) {
		kern_logf("i915: DUAL-SHARED verdict: FAIL (the shared buffer could not be made: rc=%d)\n", error);
		return;
	}

	/* The picture, published to the display. */
	(void)drv_i915_lcd_pattern_fill(shared->cpu, shared->pitch, shared->width, shared->height, a->pattern_id);
	drv_i915_scanout_publish(shared);
	kern_logf("i915: DUAL-SHARED: one buffer surf=0x%08x %ux%u pitch=%u pattern %u | LCD shows all of it, HDMI the top-left %ux%u of the same rows (partial view, not a scaled mirror)\n",
	    (unsigned)shared->surf,
	    shared->width,
	    shared->height,
	    shared->pitch,
	    a->pattern_id,
	    b->state->mode.hdisplay,
	    b->state->mode.vdisplay);

	/* The panel shows the whole buffer. */
	a->cfg.fb_fourcc = shared->format;
	a->cfg.fb_modifier = shared->modifier;
	a->cfg.fb_width = shared->width;
	a->cfg.fb_height = shared->height;
	a->cfg.fb_pitch = shared->pitch;
	a->cfg.fb_surf = (uint32_t)shared->surf;

	/* The HDMI display shows the part of its size, with the buffer's own pitch. */
	b->cfg.fb_fourcc = shared->format;
	b->cfg.fb_modifier = shared->modifier;
	b->cfg.fb_width = (uint32_t)b->state->mode.hdisplay;
	b->cfg.fb_height = (uint32_t)b->state->mode.vdisplay;
	b->cfg.fb_pitch = shared->pitch;
	b->cfg.fb_surf = (uint32_t)shared->surf;

	/* Both screens take the buffer: the count of its users is what keeps it alive. */
	(void)i915_test_dual_enable(display, k, a);
	(void)i915_test_dual_enable(display, k, b);
	kern_logf("i915: DUAL-SHARED: LCD prepare=%d enable=%d | HDMI prepare=%d enable=%d | buffer users=%u state=%d\n",
	    a->prepare_rc,
	    a->enable_rc,
	    b->prepare_rc,
	    b->enable_rc,
	    shared->users,
	    shared->state);
	if (!a->armed || !b->armed) {
		pass = 0;
		goto stop;
	}

	i915_test_dual_log_state(display, "shared, both up", a);
	i915_test_dual_log_state(display, "shared, both up", b);

	/* Both pipes read the buffer and scan out. */
	frame_a0 = i915_test_dual_frame(k->d, 0);
	frame_b0 = i915_test_dual_frame(k->d, 1);
	drv_i915_test_lcd_sleep_ms(k, I915_TEST_DUAL_FRAME_MS);
	frame_a1 = i915_test_dual_frame(k->d, 0);
	frame_b1 = i915_test_dual_frame(k->d, 1);
	kern_logf("i915: DUAL-SHARED frames: pipe A %u -> %u, pipe B %u -> %u (both read surf 0x%08x)\n",
	    frame_a0,
	    frame_a1,
	    frame_b0,
	    frame_b1,
	    (unsigned)shared->surf);
	if (frame_a1 == frame_a0 || frame_b1 == frame_b0)
		pass = 0;

	/* The window, for the photograph. */
	for (t = 0U; t < I915_TEST_DUAL_SHARE_ROUNDS; t++) {
		drv_i915_test_lcd_sleep_ms(k, I915_TEST_DUAL_LOG_MS);
		kern_logf("i915: DUAL-SHARED window t=%us: pipe A frame %u, pipe B frame %u\n",
		    (t + 1U) * (I915_TEST_DUAL_LOG_MS / 1000U),
		    i915_test_dual_frame(k->d, 0),
		    i915_test_dual_frame(k->d, 1));
	}

	/* The external display lets go: the buffer must stay, because the panel still reads it. */
	error = i915_test_dual_stop(display, b);
	if (error != 0)
		pass = 0;

	kern_logf("i915: DUAL-SHARED after the HDMI stop: buffer users=%u state=%d (IN_USE=3) | pipe A frame %u\n",
	    shared->users,
	    shared->state,
	    i915_test_dual_frame(k->d, 0));
	if (shared->users != 1U || shared->state != I915_SCANOUT_IN_USE) {
		kern_logf("i915: DUAL-SHARED: the buffer was given up while the panel still reads it\n");
		pass = 0;
	}

	/* An unpin while the panel reads it is refused. */
	unpin_while_used = drv_i915_scanout_unpin(shared);
	kern_logf("i915: DUAL-SHARED: an unpin while the panel reads it is refused: rc=%d (0 would be wrong)\n", unpin_while_used);
	if (unpin_while_used == 0)
		pass = 0;

	drv_i915_test_lcd_sleep_ms(k, I915_TEST_DUAL_ALONE_MS);

stop:
	/* The panel's stop, then the buffer back once nobody reads it. */
	if (a->armed) {
		error = i915_test_dual_stop(display, a);
		if (error != 0)
			pass = 0;
	}

	kern_logf("i915: DUAL-SHARED after both stops: buffer users=%u state=%d\n",
	    shared->users,
	    shared->state);

	/* Unpins and destroys the shared buffer. */
	destroy_error = EBUSY;
	unpin_error = drv_i915_scanout_unpin(shared);
	if (unpin_error == 0)
		destroy_error = drv_i915_scanout_destroy(shared);

	a->released = 0;
	if (unpin_error == 0 && destroy_error == 0)
		a->released = 1;

	b->released = a->released;
	kern_logf("i915: DUAL-SHARED: buffer unpin=%d destroy=%d released=%d\n",
	    unpin_error,
	    destroy_error,
	    a->released);
	if (!a->released)
		pass = 0;

	kern_logf("i915: DUAL-SHARED verdict: %s (one buffer: LCD enable rc=%d disable rc=%d | HDMI enable rc=%d disable rc=%d | released=%d; the photograph is separate evidence)\n",
	    pass ? "PASS" : "FAIL",
	    a->enable_rc,
	    a->disable_rc,
	    b->enable_rc,
	    b->disable_rc,
	    a->released);

	/* The panel's modeset object is the selected one again. */
	(void)drv_i915_lcd_modeset_select(display, 0U);
}

/*
 * Reads the sink's EDID once more while the HDMI picture is up.
 *
 * With the port off this DDC answers nothing (Linux behaves the same on
 * this machine); the question is whether driving the TMDS output brings
 * the sink's DDC up.  The value is logged either way; it does not change
 * the run's verdict.
 */
static int
i915_test_hdmib_window(
	void *ctx,
	struct i915_lcd_observer *observer)
{
	struct i915_hpd_edid_info info;
	struct i915_hpd_summary summary;
	struct i915_display *display;
	struct i915_lcd_kernel *k;
	int status;

	UNUSED_PARAMETER(observer);

	k = ctx;
	display = container_of(k, struct i915_display, lk);

	/* The hotplug path must be running and have an HDMI connector. */
	drv_i915_hpd_summary(display, &summary);
	if (!summary.started || summary.hdmi_connector < 0) {
		kern_logf("i915: HDMI-B EDID while lit: the hotplug path is not running\n");
		return 0;
	}

	/* Detects the connector and reads what its EDID read found. */
	status = drv_i915_hpd_probe_connector(display, (unsigned)summary.hdmi_connector);
	drv_i915_hpd_edid_info(display->hpd_world, &info);
	kern_logf("i915: HDMI-B EDID while lit: status %d (1 connected, 2 disconnected) | reads %u fails %u rc %d | %s product 0x%04x EDID %u.%u %s | DTD1 %ux%u %u kHz\n",
	    status,
	    info.reads,
	    info.fails,
	    info.rc,
	    info.mfg,
	    info.product,
	    info.version,
	    info.revision,
	    info.digital ? "digital" : "analog",
	    info.hactive,
	    info.vactive,
	    info.pixel_clock_khz);

	/* The read is information only. */
	return 0;
}

/* Reads the frame counter of a pipe (PIPE_FRMCOUNT_G4X of that pipe). */
static uint32_t
i915_test_dual_frame(
	const struct i915_lcd_kernel_deps *d,
	int pipe)
{
	uint32_t frame;

	/* Reads the pipe's counter. */
	frame = drv_i915_read32(d->mmio, I915_TEST_PIPE_FRAME_A + I915_TEST_PIPE_STRIDE * (uint32_t)pipe);

	/* Reports the frame. */
	return frame;
}

/*
 * Starts a two-screen run: refuses when an earlier run retained resources
 * or the panel is not live, then the run's hooks, an empty PLL pool and
 * DBUF state, and the display window of the GGTT.  0, or EBUSY.
 */
static int
i915_test_dual_begin(
	struct i915_display *display,
	const char *tag,
	struct i915_lcd_kernel **k_out)
{
	const struct i915_lcd_kernel_deps *d;
	struct i915_lcd_kernel *k;
	int retained;

	d = &display->rlcd;

	/* The GT memory must be up for the buffers. */
	if (!d->gm->inited) {
		kern_logf("i915: %s verdict: FAIL (a dependency is missing)\n", tag);
		return EBUSY;
	}

	/* An earlier run left resources the display may still read. */
	retained = drv_i915_test_lcd_retained(display);
	if (retained) {
		kern_logf("i915: %s verdict: FAIL (an earlier run's resources are retained)\n", tag);
		return EBUSY;
	}

	/* The resident eDP must be live. */
	if (!d->edp->connector_live || d->edp->lcd_rc != 0) {
		kern_logf("i915: %s verdict: FAIL (the resident eDP is not live)\n", tag);
		return EBUSY;
	}

	/* The run, and the screens cleared. */
	k = drv_i915_test_lcd_start(display, NULL);
	kern_memset(&i915_test_dual_a, 0, sizeof(i915_test_dual_a));
	kern_memset(&i915_test_dual_b, 0, sizeof(i915_test_dual_b));

	/* This run owns the device's PLL pool and DBUF state. */
	drv_i915_lcd_dplls_reset(display->lcd_world);
	drv_i915_lcd_dbuf_forget(display->wm_world);

	/* The display window of the GGTT; claimed earlier is fine. */
	(void)drv_i915_gt_display_window_init(d->gm, I915_GT_DISPLAY_PAGES);

	/* Succeeded: the run is ready for its screens. */
	*k_out = k;
	return 0;
}

/*
 * Configures both screens: the panel from the initialisation's objects
 * (pipe A, with pipe B also active), and the HDMI display of port B from
 * the same device-wide inputs, the CEA state and the VBT child of the
 * port (pipe B, with pipe A also active).  0, or EINVAL.
 */
static int
i915_test_dual_configure(
	struct i915_lcd_kernel *k,
	const char *tag)
{
	const struct i915_vbt_encoder *encoder;
	const struct i915_lcd_kernel_deps *d;
	struct i915_test_dual_screen *a;
	struct i915_test_dual_screen *b;
	int error;

	d = k->d;
	a = &i915_test_dual_a;
	b = &i915_test_dual_b;

	/* Screen A: the resident panel, as LCD-B drives it. */
	a->idx = 0U;
	a->name = "LCD";
	a->pipe = 0;
	a->state = &d->edp->lcd;
	error = drv_i915_lcd_kernel_fill_cfg(k, &a->cfg);
	if (error != 0) {
		kern_logf("i915: %s verdict: FAIL (the panel's configuration could not be built)\n", tag);
		return EINVAL;
	}

	/* Pipe B is part of the panel's configuration. */
	a->cfg.also_active_pipes = 1U << 1;

	/* Screen B: the external HDMI display in the CEA mode. */
	b->idx = 1U;
	b->name = "HDMI";
	b->pipe = 1;
	error = drv_i915_lcd_compute_hdmi(&i915_test_hdmi_cea4, I915_TEST_HDMI_REF_KHZ, &i915_test_hdmi_state);
	if (error != 0) {
		kern_logf("i915: %s verdict: FAIL (the WRPLL calculation refused the TMDS clock: rc=%d)\n", tag, error);
		return EINVAL;
	}

	/* The device-wide inputs are the panel's; the output is HDMI on port B, pipe B. */
	b->state = &i915_test_hdmi_state;
	b->cfg = a->cfg;
	b->cfg.output_hdmi = 1;
	b->cfg.port = I915_TEST_HDMI_PORT;
	b->cfg.pipe = I915_TEST_HDMI_PIPE;
	b->cfg.cpu_transcoder = I915_TEST_HDMI_PIPE;
	b->cfg.aux_ch = I915_TEST_HDMI_PORT;

	/* Only the caller's expectation: the PLL rule decides. */
	b->cfg.dpll_id = 0;
	b->cfg.saved_port_bits = drv_i915_read32(d->mmio, I915_TEST_HDMI_DDI_BUF_CTL_B) & I915_TEST_SAVED_PORT_BITS;
	b->cfg.vbt_backlight_present = 0;

	/* The VBT child of port B: its HDMI level shift, when it names one. */
	encoder = drv_i915_vbt_encoder_for_port(&d->edp->vbt->parsed, I915_TEST_HDMI_PORT);
	b->cfg.vbt_hdmi_level_shift = -1;
	if (encoder != NULL)
		b->cfg.vbt_hdmi_level_shift = encoder->hdmi_level_shift;

	/* Pipe A is part of the HDMI configuration. */
	b->cfg.also_active_pipes = 1U << 0;

	/* Succeeded: both screens are configured. */
	return 0;
}

/*
 * Brings one screen of DUAL up on a buffer of its own: creates, pins,
 * fills and publishes the buffer, then prepares and enables the screen.
 * 0, or EIO.
 */
static int
i915_test_dual_bring_up(
	struct i915_display *display,
	struct i915_lcd_kernel *k,
	struct i915_test_dual_screen *sc)
{
	int error;

	/* The screen's buffer at its mode's size. */
	error = drv_i915_scanout_create(k->d->gm, (uint32_t)sc->state->mode.hdisplay, (uint32_t)sc->state->mode.vdisplay, I915_FOURCC_XRGB8888, I915_MOD_LINEAR, &sc->so);
	if (error != 0) {
		kern_logf("i915: DUAL %s: scanout create rc=%d\n", sc->name, error);
		return EIO;
	}

	/* Pins it for the display. */
	error = drv_i915_scanout_pin(&sc->so, sc->name);
	if (error != 0) {
		kern_logf("i915: DUAL %s: scanout pin rc=%d\n", sc->name, error);
		(void)drv_i915_scanout_destroy(&sc->so);
		return EIO;
	}

	/* The screen's picture, published to the display. */
	sc->pattern_hash = drv_i915_lcd_pattern_fill(sc->so.cpu, sc->so.pitch, sc->so.width, sc->so.height, sc->pattern_id);
	drv_i915_scanout_publish(&sc->so);

	/* The framebuffer the screen's plane reads. */
	sc->cfg.fb_fourcc = sc->so.format;
	sc->cfg.fb_modifier = sc->so.modifier;
	sc->cfg.fb_width = sc->so.width;
	sc->cfg.fb_height = sc->so.height;
	sc->cfg.fb_pitch = sc->so.pitch;
	sc->cfg.fb_surf = (uint32_t)sc->so.surf;

	/* Prepares and enables the screen; the enable commit's result is logged once it ran. */
	error = i915_test_dual_enable(display, k, sc);
	if (sc->begun) {
		kern_logf("i915: DUAL %s: enabled rc=%d pattern=%u surf=0x%08x %ux%u pitch=%u\n",
		    sc->name,
		    sc->enable_rc,
		    sc->pattern_id,
		    (unsigned)sc->so.surf,
		    sc->so.width,
		    sc->so.height,
		    sc->so.pitch);
	}

	/* A screen that did not come up. */
	if (error != 0)
		return EIO;

	/* Succeeded: the screen scans its buffer out. */
	return 0;
}

/*
 * Prepares one screen's modeset object, hands its buffer to the display
 * and runs the enable commit.  0, or EIO.
 */
static int
i915_test_dual_enable(
	struct i915_display *display,
	struct i915_lcd_kernel *k,
	struct i915_test_dual_screen *sc)
{
	int begin_error;

	/* The check phase of the screen's new state. */
	sc->enable_rc = -99;
	(void)drv_i915_lcd_modeset_select(display, sc->idx);
	sc->prepare_rc = drv_i915_lcd_modeset_prepare(display, sc->state, &sc->cfg, &k->ops);
	if (sc->prepare_rc != 0) {
		kern_logf("i915: DUAL %s: prepare rc=%d\n", sc->name, sc->prepare_rc);
		return EIO;
	}

	/* The display takes the buffer. */
	begin_error = drv_i915_scanout_begin(sc->sop);
	if (begin_error != 0) {
		kern_logf("i915: DUAL %s: the display could not take the buffer\n", sc->name);
		return EIO;
	}

	sc->begun = 1;

	/* The enable commit. */
	sc->enable_rc = drv_i915_lcd_modeset_commit_enable(display);
	if (sc->enable_rc != I915_LCD_MS_OK)
		return EIO;

	/* Succeeded: the screen's plane is armed. */
	sc->armed = 1;
	return 0;
}

/* Logs what one screen's modeset object holds. */
static void
i915_test_dual_log_state(
	struct i915_display *display,
	const char *when,
	struct i915_test_dual_screen *sc)
{
	struct i915_lcd_modeset_status status;

	/* Reads the screen's status. */
	(void)drv_i915_lcd_modeset_select(display, sc->idx);
	drv_i915_lcd_modeset_status(display, &status);

	kern_logf("i915: DUAL %s [%s]: crtc_active=%d plane_armed=%d DPLL%d on=%d active_mask=0x%x pipe_mask=0x%x | io wakeref=%d crtc domains=%u | DBUF slices=0x%x mbus_joined=%d | ddb %u..%u | wm0 enable=%u blocks=%u lines=%u | errors=%u\n",
	    sc->name,
	    when,
	    status.crtc_active,
	    status.plane_armed,
	    status.pll_id,
	    status.pll_on,
	    status.pll_active_mask,
	    status.pll_pipe_mask,
	    status.ddi_io_wakeref,
	    status.crtc_domains_held,
	    status.dbuf_slices_now,
	    status.mbus_joined_now,
	    status.ddb_start,
	    status.ddb_end,
	    status.wm0_enable,
	    status.wm0_blocks,
	    status.wm0_lines,
	    status.errors);
}

/*
 * Stops one screen: the plane, then the disable commit; on success the
 * plane is confirmed off and the display lets go of the screen's buffer.
 * 0, or EIO with what the stop left logged.
 */
static int
i915_test_dual_stop(
	struct i915_display *display,
	struct i915_test_dual_screen *sc)
{
	struct i915_lcd_modeset_status status;
	int plane_error;
	int disable_error;

	/* The plane off, then the disable commit of the screen. */
	(void)drv_i915_lcd_modeset_select(display, sc->idx);
	plane_error = drv_i915_lcd_modeset_plane_disable(display);
	disable_error = drv_i915_lcd_modeset_commit_disable(display);
	sc->disable_rc = plane_error;
	if (disable_error != I915_LCD_MS_OK)
		sc->disable_rc = disable_error;

	/* The caller watched the pipe stand still: the plane is off and the buffer is let go. */
	if (sc->disable_rc == I915_LCD_MS_OK) {
		drv_i915_lcd_modeset_plane_released(display);
		drv_i915_scanout_end(sc->sop);
	}

	kern_logf("i915: DUAL %s: stopped rc=%d\n", sc->name, sc->disable_rc);

	/* Logs what a stop that failed left. */
	if (sc->disable_rc != I915_LCD_MS_OK) {
		drv_i915_lcd_modeset_status(display, &status);
		kern_logf("i915: DUAL %s: what the stop left: crtc_active=%d plane_armed=%d DPLL%d on=%d active_mask=0x%x pipe_mask=0x%x io wakeref=%d aux wakeref=%d crtc domains=%u dc_off_held=%d stop_unconfirmed=%d errors=%u first=%s\n",
		    sc->name,
		    status.crtc_active,
		    status.plane_armed,
		    status.pll_id,
		    status.pll_on,
		    status.pll_active_mask,
		    status.pll_pipe_mask,
		    status.ddi_io_wakeref,
		    status.aux_wakeref,
		    status.crtc_domains_held,
		    status.dc_off_held,
		    status.stop_unconfirmed,
		    status.errors,
		    status.first_error != NULL ? status.first_error : "-");
		return EIO;
	}

	/* Succeeded: the screen is off and its buffer let go. */
	return 0;
}

/* Unpins and destroys one screen's own buffer. */
static void
i915_test_dual_release(
	struct i915_test_dual_screen *sc)
{
	int unpin_error;
	int destroy_error;

	/* Unpins, then destroys what was unpinned. */
	destroy_error = EBUSY;
	unpin_error = drv_i915_scanout_unpin(&sc->so);
	if (unpin_error == 0)
		destroy_error = drv_i915_scanout_destroy(&sc->so);

	sc->released = 0;
	if (unpin_error == 0 && destroy_error == 0)
		sc->released = 1;

	kern_logf("i915: DUAL %s: buffer unpin=%d destroy=%d released=%d\n",
	    sc->name,
	    unpin_error,
	    destroy_error,
	    sc->released);
}

/* Reports whether the panel still owns its crtc, plane, PLL and power domains. */
static int
i915_test_dual_panel_kept(
	struct i915_display *display)
{
	struct i915_lcd_modeset_status status;

	/* Reads the panel's status. */
	(void)drv_i915_lcd_modeset_select(display, i915_test_dual_a.idx);
	drv_i915_lcd_modeset_status(display, &status);

	/* Every one of them must still be held. */
	if (!status.crtc_active || !status.plane_armed || !status.pll_on)
		return 0;
	if (status.crtc_domains_held == 0U)
		return 0;

	/* The panel kept everything. */
	return 1;
}
