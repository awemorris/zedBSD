/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of a screen's plan (display/xd-screen.c): the 5330's
 * panel against the numbers the older check phase reports for it (the
 * CD clock 179.2 MHz from a 70.4 MHz pixel need and an 11 MHz bandwidth
 * need, 564 MB/s, four slices joined, the plane's level 0 and its part
 * of the buffer), the watermarks (two pipes sharing their slices by
 * weight, a blank too short, a buffer too wide for the share, a memory
 * without levels), the refusals (another CD clock, more than the
 * highest, another voltage level, the QGV point, the sinks the plan does
 * not drive, a link too slow), an HDMI sink's link
 * and WRPLL on DDI B, an external DisplayPort sink's link on a Type-C
 * port with a branch's limits, and Tiger Lake's pixel rate rule; the
 * light's and the dark's order of steps, what a failed step skips, the
 * undo of a failed light, and a dark retained when its stop is not
 * confirmed; an update that must land in one frame (the evasion window,
 * the scan line read again at 0, a line outside the window, one inside it
 * slept out, a blank that never comes, an interrupt reference refused, an
 * update that crosses a blank); the flips (refusals, a flip that latches, one whose event
 * comes without the new buffer, one without an event, the poll and the
 * settle) and the verdicts of a resident run.
 */

#include "host-test.h"
#include "xd-fake-regs.h"

#include "../../display/xd-clock.h"
#include "../../display/xd-dp.h"
#include "../../display/xd-platform.h"
#include "../../display/xd-screen.h"
#include "../../display/display-internal.h"

#include "../../intel/display-regs-xd.h"

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The devices the checks describe: the 5330's Alder Lake-P and a Tiger Lake. */
#define I915_XD_SCREEN_TEST_ADLP	0x46a6U
#define I915_XD_SCREEN_TEST_ADLP_REVID	0x0cU
#define I915_XD_SCREEN_TEST_TGL		0x9a49U
#define I915_XD_SCREEN_TEST_TGL_REVID	0x01U

/*
 * The steps the sequencer checks see, in order, and the step that fails
 * (0 for none).
 */
struct i915_xd_screen_test_seen {
	unsigned steps[32];
	unsigned count;
	unsigned fail;
};

/* The steps seen by the sequencer checks. */
static struct i915_xd_screen_test_seen i915_xd_screen_test_seen;

/*
 * What the update checks' backend saw: whether interrupts are off, the
 * offs, the vertical blank references held and refused, the sleeps (and
 * those entered with interrupts off), the arming writes (and those with
 * interrupts on), the events, the errors; and what it does: refuse the
 * references, move the scan line out of the window at a sleep, move the
 * frame counter at the arming writes.
 */
struct i915_xd_screen_test_backend {
	int irq_off;
	unsigned offs;
	int refs;
	unsigned refusals;
	unsigned sleeps;
	unsigned sleeps_irq_off;
	unsigned arms;
	unsigned arms_irq_on;
	unsigned events;
	unsigned errors;
	int refuse;
	int sleep_moves;
	int arm_crosses;
};

/* The update checks' backend. */
static struct i915_xd_screen_test_backend i915_xd_screen_test_backend;

/* The scan line counter's answers of the zero check, in order. */
static uint32_t i915_xd_screen_test_lines[4];

/* The next answer of the zero check. */
static unsigned i915_xd_screen_test_line_next;

static int i915_xd_screen_test_note(unsigned step);
static int i915_xd_screen_test_s1(void *context);
static int i915_xd_screen_test_s2(void *context);
static int i915_xd_screen_test_s3(void *context);
static int i915_xd_screen_test_s4(void *context);
static int i915_xd_screen_test_s5(void *context);
static int i915_xd_screen_test_s6(void *context);
static int i915_xd_screen_test_s7(void *context);
static int i915_xd_screen_test_s8(void *context);
static int i915_xd_screen_test_s9(void *context);
static int i915_xd_screen_test_s10(void *context);
static int i915_xd_screen_test_s11(void *context);
static int i915_xd_screen_test_s12(void *context);
static int i915_xd_screen_test_d1(void *context);
static int i915_xd_screen_test_d2(void *context);
static int i915_xd_screen_test_d3(void *context);
static int i915_xd_screen_test_d4(void *context);
static int i915_xd_screen_test_d5(void *context);
static int i915_xd_screen_test_d6(void *context);
static int i915_xd_screen_test_d7(void *context);
static int i915_xd_screen_test_d8(void *context);
static int i915_xd_screen_test_d9(void *context);
static void i915_xd_screen_test_hooks(struct i915_xd_screen_hooks *hooks);
static int i915_xd_screen_test_order(const unsigned *want, unsigned count);
static void i915_xd_screen_test_sequencer(void);
static void i915_xd_screen_test_flips(void);
static void i915_xd_screen_test_update(void);
static void i915_xd_screen_test_update_hooks(struct i915_xd_update_hooks *hooks);
static int i915_xd_screen_test_vblank_get(void *context);
static void i915_xd_screen_test_vblank_put(void *context);
static long i915_xd_screen_test_vblank_sleep(void *context, long ticks);
static void i915_xd_screen_test_irq_off(void *context);
static void i915_xd_screen_test_irq_on(void *context);
static void i915_xd_screen_test_arm(void *context);
static void i915_xd_screen_test_arm_event(void *context);
static void i915_xd_screen_test_error(void *context, const char *what);
static uint32_t i915_xd_screen_test_line_rule(uint32_t offset, uint32_t value, void *context);
static void i915_xd_screen_test_runs(void);
static const struct i915_xd_platform *i915_xd_screen_test_platform(uint16_t device, uint8_t revid);
static void i915_xd_screen_test_mode(struct i915_display_mode *mode, int clock_khz);
static void i915_xd_screen_test_panel_input(struct i915_xd_plan_input *input, const struct i915_xd_platform *platform, const struct i915_display_mode *mode);
static void i915_xd_screen_test_panel(void);
static void i915_xd_screen_test_refusals(void);
static void i915_xd_screen_test_wm(void);
static void i915_xd_screen_test_sinks(void);
static void i915_xd_screen_test_hdmi(void);
static void i915_xd_screen_test_dp(void);
static void i915_xd_screen_test_tgl(void);

/*
 * Runs every check of the plan and reports.
 */
int
main(void)
{
	int failures;

	/* Each group of checks. */
	i915_xd_screen_test_panel();
	i915_xd_screen_test_refusals();
	i915_xd_screen_test_wm();
	i915_xd_screen_test_sinks();
	i915_xd_screen_test_hdmi();
	i915_xd_screen_test_dp();
	i915_xd_screen_test_tgl();
	i915_xd_screen_test_sequencer();
	i915_xd_screen_test_update();
	i915_xd_screen_test_flips();
	i915_xd_screen_test_runs();

	/* Reports the result. */
	failures = i915_host_report("host_xd_screen_test");
	if (failures != 0)
		return 1;

	/* Succeeded: every check passed. */
	return 0;
}

/* Returns a device's description, or NULL. */
static const struct i915_xd_platform *
i915_xd_screen_test_platform(
	uint16_t device,
	uint8_t revid)
{
	struct i915_xd_choice choice;
	int error;

	/* The description by the product. */
	error = drv_i915_xd_platform_choose(device, revid, 0, 0U, &choice);
	if (error != 0)
		return NULL;

	/* Succeeded: the description. */
	return choice.platform;
}

/* A 1920x1080 mode at a clock. */
static void
i915_xd_screen_test_mode(
	struct i915_display_mode *mode,
	int clock_khz)
{
	/* The timings of the 5330's panel. */
	memset(mode, 0, sizeof(*mode));
	mode->clock_khz = clock_khz;
	mode->hdisplay = 1920U;
	mode->hsync_start = 1968U;
	mode->hsync_end = 2000U;
	mode->htotal = 2080U;
	mode->vdisplay = 1080U;
	mode->vsync_start = 1083U;
	mode->vsync_end = 1088U;
	mode->vtotal = 1128U;
}

/*
 * The 5330's panel: HBR on four lanes, eDP 1.3, a 6 bpc EDID, the CD
 * clock 179.2 MHz from a 38.4 MHz reference; the memory's six watermark
 * levels as the 5330's start reads them, a 1920x1080 buffer, no other
 * pipe in the display buffer.
 */
static void
i915_xd_screen_test_panel_input(
	struct i915_xd_plan_input *input,
	const struct i915_xd_platform *platform,
	const struct i915_display_mode *mode)
{
	/* The output, its sink and the platform. */
	memset(input, 0, sizeof(*input));
	input->kind = I915_XD_SCREEN_PANEL;
	input->ddi = 0U;
	input->pipe = 0U;
	input->mode = mode;
	input->sink.rate_code = 0x0aU;
	input->sink.max_lanes = 4U;
	input->sink.edp_revision = 0x02U;
	input->platform = platform;
	input->ref_khz = 38400U;
	input->run_pipes = 0x1U;

	/* The CD clock the start left, its highest step, the QGV point, the plane's format. */
	input->cdclk.ref_khz = 38400U;
	input->cdclk.vco_khz = 537600U;
	input->cdclk.cdclk_khz = 179200U;
	input->cdclk.bypass_khz = 19200U;
	input->cdclk.voltage = 0U;
	input->cdclk_max_khz = 652800U;
	input->qgv_mbps = 20000U;
	input->cpp = 4U;

	/* The watermark levels, the buffer and the frame start delay. */
	input->wm.levels = 6U;
	input->wm.latency_us[0] = 3U;
	input->wm.latency_us[1] = 54U;
	input->wm.latency_us[2] = 83U;
	input->wm.latency_us[3] = 102U;
	input->wm.latency_us[4] = 147U;
	input->wm.latency_us[5] = 147U;
	input->wm.latency_us[6] = 144U;
	input->wm.latency_us[7] = 144U;
	input->wm.ipc = 1;
	input->wm.sagv_block_us = 35U;
	input->fb_width = 1920U;
	input->fb_height = 1080U;
	input->frame_start_delay = 1U;
}

/* The 5330's panel against the older check phase's numbers. */
static void
i915_xd_screen_test_panel(void)
{
	const struct i915_xd_platform *platform;
	struct i915_xd_plan_input input;
	struct i915_display_mode mode;
	struct i915_xd_dp_m_n m_n;
	struct i915_xd_plan plan;
	unsigned refused;

	/* The panel at 140.8 MHz, 6 bpc. */
	platform = i915_xd_screen_test_platform(I915_XD_SCREEN_TEST_ADLP, I915_XD_SCREEN_TEST_ADLP_REVID);
	i915_host_check(platform != NULL, "panel: the 5330's description");
	if (platform == NULL)
		return;
	i915_xd_screen_test_mode(&mode, 140800);
	mode.edid_bpc = 6;
	i915_xd_screen_test_panel_input(&input, platform, &mode);

	/* The plan. */
	refused = drv_i915_xd_screen_plan(&input, &plan);
	i915_host_check(refused == I915_XD_PLAN_OK && plan.refused == I915_XD_PLAN_OK, "panel: accepted");
	i915_host_check(plan.rate_khz == 270000U && plan.lanes == 4U && plan.bpp == 18U, "panel: HBR, four lanes, 18 bpp");
	drv_i915_xd_dp_m_n(18U * 16U, 4U, 140800U, 270000U, 1000000U, &m_n);
	i915_host_check(memcmp(&m_n, &plan.m_n, sizeof(m_n)) == 0 && plan.m_n.data_n != 0U, "panel: the link's M/N");
	i915_host_check(plan.pll_set && plan.pll.cfgcr0 != 0U, "panel: DPLL words for DDI A's combo PHY");
	i915_host_check(plan.min_cdclk_khz == 70400U && plan.cdclk_khz == 179200U && plan.vco_khz == 537600U && plan.voltage == 0U,
			"panel: needs 70.4 MHz, gets the running 179.2 MHz at VCO 537.6 MHz, level 0 (the older check's numbers)");
	i915_host_check(plan.data_mbps == 564U, "panel: 564 MB/s (the older check's data rate)");
	i915_host_check(plan.slices == 0xfU && plan.joined == 1U, "panel: four slices, the bus joined (pipe A alone)");
	i915_host_check(plan.crtc_min_cdclk_khz == 70400U && plan.bw_min_cdclk_khz == 11000U, "panel: the pipe needs 70.4 MHz, the read bandwidth 11 MHz");
	i915_host_check(plan.enabled_slices == 0xfU && plan.weight == 1920U && plan.pipe_ddb.start == 0U && plan.pipe_ddb.end == 4096U,
			"panel: all four slices powered, the pipe's weight its width, the whole joined buffer");
	i915_host_check(plan.plane_ddb.start == 0U && plan.plane_ddb.end == 4060U && plan.cursor_ddb.start == 4060U && plan.cursor_ddb.end == 4096U,
			"panel: the plane's blocks 0..4060, the cursor's the rest (the older check's numbers)");
	i915_host_check(plan.wm.level[0].enable == 1U && plan.wm.level[0].blocks == 16U && plan.wm.level[0].lines == 1U,
			"panel: level 0 on, 16 blocks, one line (the older check's numbers)");

	/* With pipe B too: the bus is not joined. */
	input.run_pipes = 0x3U;
	(void)drv_i915_xd_screen_plan(&input, &plan);
	i915_host_check(plan.joined == 0U && plan.slices != 0U && plan.slices != 0xfU, "panel: pipes A and B: not joined, part of the slices");

	/* A VBT cap and an EDID that names no depth. */
	mode.edid_bpc = 0;
	input.sink.vbt_bpp = 18U;
	input.run_pipes = 0x1U;
	(void)drv_i915_xd_screen_plan(&input, &plan);
	i915_host_check(plan.bpp == 18U, "panel: 24 bpp capped by the VBT's 18");
	input.sink.vbt_bpp = 0U;
	(void)drv_i915_xd_screen_plan(&input, &plan);
	i915_host_check(plan.bpp == 24U, "panel: 24 bpp when the EDID names no depth");
}

/* The check's refusals. */
static void
i915_xd_screen_test_refusals(void)
{
	const struct i915_xd_platform *platform;
	struct i915_xd_plan_input input;
	struct i915_display_mode mode;
	struct i915_xd_plan plan;
	unsigned refused;

	/* The Alder Lake-P description. */
	platform = i915_xd_screen_test_platform(I915_XD_SCREEN_TEST_ADLP, I915_XD_SCREEN_TEST_ADLP_REVID);
	if (platform == NULL)
		return;

	/* A mode that needs a higher CD clock (on four HBR3 lanes). */
	i915_xd_screen_test_mode(&mode, 600000);
	i915_xd_screen_test_panel_input(&input, platform, &mode);
	input.sink.rate_code = 0x1eU;
	refused = drv_i915_xd_screen_plan(&input, &plan);
	i915_host_check(refused == I915_XD_PLAN_CDCLK && plan.cdclk_khz == 307200U, "refusal: 600 MHz needs 307.2 MHz, not the running 179.2");

	/* More than the highest step. */
	i915_xd_screen_test_mode(&mode, 140800);
	i915_xd_screen_test_panel_input(&input, platform, &mode);
	input.cdclk_max_khz = 60000U;
	refused = drv_i915_xd_screen_plan(&input, &plan);
	i915_host_check(refused == I915_XD_PLAN_CDCLK && plan.cdclk_khz == 0U, "refusal: a need above the highest step");

	/* Another voltage level. */
	i915_xd_screen_test_panel_input(&input, platform, &mode);
	input.cdclk.voltage = 1U;
	refused = drv_i915_xd_screen_plan(&input, &plan);
	i915_host_check(refused == I915_XD_PLAN_VOLTAGE, "refusal: the running level is not the mode's");

	/* The QGV point. */
	i915_xd_screen_test_panel_input(&input, platform, &mode);
	input.qgv_mbps = 500U;
	refused = drv_i915_xd_screen_plan(&input, &plan);
	i915_host_check(refused == I915_XD_PLAN_QGV, "refusal: 564 MB/s against a 500 MB/s QGV point");
	input.qgv_mbps = 0U;
	refused = drv_i915_xd_screen_plan(&input, &plan);
	i915_host_check(refused == I915_XD_PLAN_QGV, "refusal: an unknown QGV point");

	/* A link refused is not checked. */
	i915_xd_screen_test_panel_input(&input, platform, &mode);
	input.sink.rate_code = 0x06U;
	input.sink.max_lanes = 1U;
	refused = drv_i915_xd_screen_plan(&input, &plan);
	i915_host_check(refused == I915_XD_PLAN_LINK && plan.cdclk_khz == 0U, "refusal: one RBR lane cannot carry 140.8 MHz at 24 bpp; no check");
}

/* The watermarks and the display buffer's shares. */
static void
i915_xd_screen_test_wm(void)
{
	const struct i915_xd_platform *platform;
	struct i915_xd_plan_input input;
	struct i915_display_mode mode;
	struct i915_xd_plan plan;
	unsigned refused;

	/* The Tiger Lake description. */
	platform = i915_xd_screen_test_platform(I915_XD_SCREEN_TEST_TGL, I915_XD_SCREEN_TEST_TGL_REVID);
	if (platform == NULL)
		return;

	/* Tiger Lake's pipe C beside a 1920-wide pipe B on the same slice: the second half of it. */
	i915_xd_screen_test_mode(&mode, 140800);
	i915_xd_screen_test_panel_input(&input, platform, &mode);
	input.pipe = 2U;
	input.run_pipes = 0x7U;
	input.pipe_slices[0] = 0x1U;
	input.pipe_weight[0] = 1920U;
	input.pipe_slices[1] = 0x2U;
	input.pipe_weight[1] = 1920U;
	(void)drv_i915_xd_screen_plan(&input, &plan);
	i915_host_check(plan.slices == 0x2U && plan.enabled_slices == 0x3U, "wm: pipes A, B and C: C on slice 2 with B, slices 1 and 2 powered");
	i915_host_check(plan.pipe_ddb.start == 1536U && plan.pipe_ddb.end == 2048U, "wm: C's half of slice 2 after B's");
	i915_host_check(plan.cursor_ddb.end == 2048U && plan.plane_ddb.start == 1536U && plan.plane_ddb.end == plan.cursor_ddb.start,
			"wm: the plane from the start of C's half, the cursor at its end");

	/* A buffer too wide for the share. */
	input.fb_width = 60000U;
	refused = drv_i915_xd_screen_plan(&input, &plan);
	i915_host_check(refused == I915_XD_PLAN_WM && plan.wm_error == EINVAL, "wm: a 60000-pixel line's level 0 does not fit half a slice");

	/* A blank of one line. */
	platform = i915_xd_screen_test_platform(I915_XD_SCREEN_TEST_ADLP, I915_XD_SCREEN_TEST_ADLP_REVID);
	if (platform == NULL)
		return;
	i915_xd_screen_test_panel_input(&input, platform, &mode);
	mode.vtotal = (uint16_t)(mode.vdisplay + 1U);
	refused = drv_i915_xd_screen_plan(&input, &plan);
	i915_host_check(refused == I915_XD_PLAN_WM && plan.wm_error == EINVAL && plan.cdclk_khz == 0U, "wm: a one-line blank holds no level; no CD clock is checked");

	/* A short blank turns the deeper levels off. */
	mode.vtotal = 1086U;
	refused = drv_i915_xd_screen_plan(&input, &plan);
	i915_host_check(refused == I915_XD_PLAN_OK && plan.wm_cut == 1 && plan.wm.level[1].enable == 1U && plan.wm.level[2].enable == 0U && plan.wm.level[5].enable == 0U,
			"wm: a 6-line blank keeps levels 0 and 1 (54 us) and turns 2 (83 us) and deeper off");

	/* A memory without levels. */
	i915_xd_screen_test_mode(&mode, 140800);
	i915_xd_screen_test_panel_input(&input, platform, &mode);
	input.wm.levels = 0U;
	refused = drv_i915_xd_screen_plan(&input, &plan);
	i915_host_check(refused == I915_XD_PLAN_WM && plan.wm_error == EINVAL, "wm: a memory that names no levels");
}

/* The panels the plan does not drive. */
static void
i915_xd_screen_test_sinks(void)
{
	const struct i915_xd_platform *platform;
	struct i915_xd_plan_input input;
	struct i915_display_mode mode;
	struct i915_xd_plan plan;
	unsigned refused;

	/* The Alder Lake-P description. */
	platform = i915_xd_screen_test_platform(I915_XD_SCREEN_TEST_ADLP, I915_XD_SCREEN_TEST_ADLP_REVID);
	if (platform == NULL)
		return;
	i915_xd_screen_test_mode(&mode, 140800);

	/* eDP 1.4, a rate code that is not one of the four, three lanes, a mode without a clock. */
	i915_xd_screen_test_panel_input(&input, platform, &mode);
	input.sink.edp_revision = 0x03U;
	refused = drv_i915_xd_screen_plan_link(&input, &plan);
	i915_host_check(refused == I915_XD_PLAN_SINK, "sinks: an eDP 1.4 panel's own rates are not taken");
	i915_xd_screen_test_panel_input(&input, platform, &mode);
	input.sink.rate_code = 0x0cU;
	refused = drv_i915_xd_screen_plan_link(&input, &plan);
	i915_host_check(refused == I915_XD_PLAN_SINK, "sinks: rate code 0x0c");
	i915_xd_screen_test_panel_input(&input, platform, &mode);
	input.sink.max_lanes = 3U;
	refused = drv_i915_xd_screen_plan_link(&input, &plan);
	i915_host_check(refused == I915_XD_PLAN_SINK, "sinks: three lanes");
	mode.clock_khz = 0;
	i915_xd_screen_test_panel_input(&input, platform, &mode);
	refused = drv_i915_xd_screen_plan_link(&input, &plan);
	i915_host_check(refused == I915_XD_PLAN_LINK, "sinks: a mode without a clock");
}

/* An HDMI sink on DDI B. */
static void
i915_xd_screen_test_hdmi(void)
{
	const struct i915_xd_platform *platform;
	struct i915_xd_plan_input input;
	struct i915_display_mode mode;
	struct i915_xd_dpll_words words;
	struct i915_xd_plan plan;
	unsigned refused;
	int error;

	/* The Alder Lake-P description. */
	platform = i915_xd_screen_test_platform(I915_XD_SCREEN_TEST_ADLP, I915_XD_SCREEN_TEST_ADLP_REVID);
	if (platform == NULL)
		return;

	/* 1080p60 on DDI B, pipe B. */
	i915_xd_screen_test_mode(&mode, 148500);
	i915_xd_screen_test_panel_input(&input, platform, &mode);
	input.kind = I915_XD_SCREEN_HDMI;
	input.ddi = 1U;
	input.pipe = 1U;
	input.run_pipes = 0x3U;
	refused = drv_i915_xd_screen_plan_link(&input, &plan);
	error = drv_i915_xd_dpll_hdmi_words(platform, 38400U, 148500U, I915_XD_DPLL_AFC_NONE, &words);
	i915_host_check(refused == I915_XD_PLAN_OK && plan.rate_khz == 148500U && plan.lanes == 4U && plan.bpp == 24U, "hdmi: the TMDS clock on four lanes, 24 bpp");
	i915_host_check(error == 0 && plan.pll_set && plan.pll.cfgcr0 == words.cfgcr0 && plan.pll.cfgcr1 == words.cfgcr1, "hdmi: the WRPLL's words");
	i915_host_check(plan.m_n.data_n == 0U, "hdmi: no M/N");
}

/* An external DisplayPort sink on a Type-C port. */
static void
i915_xd_screen_test_dp(void)
{
	static const uint32_t rates[] = { 162000U, 270000U, 540000U };
	const struct i915_xd_platform *platform;
	struct i915_xd_plan_input input;
	struct i915_display_mode mode;
	struct i915_xd_plan plan;
	unsigned refused;

	/* The Alder Lake-P description. */
	platform = i915_xd_screen_test_platform(I915_XD_SCREEN_TEST_ADLP, I915_XD_SCREEN_TEST_ADLP_REVID);
	if (platform == NULL)
		return;

	/* 1080p60 on TC1. */
	i915_xd_screen_test_mode(&mode, 148500);
	i915_xd_screen_test_panel_input(&input, platform, &mode);
	input.kind = I915_XD_SCREEN_DP;
	input.ddi = 3U;
	input.sink.rates = rates;
	input.sink.nrates = 3U;
	input.sink.max_lanes = 4U;
	refused = drv_i915_xd_screen_plan_link(&input, &plan);
	i915_host_check(refused == I915_XD_PLAN_OK && plan.bpp == 24U && plan.need_kbps <= plan.carry_kbps && plan.rate_khz == 162000U && plan.lanes == 4U,
			"dp: the lowest rate first: RBR on four lanes carries 1080p60 at 24 bpp");
	i915_host_check(plan.pll_set == 0 && plan.m_n.data_n != 0U, "dp: no combo PLL on a Type-C port, the M/N");

	/* Two lanes at most. */
	input.sink.lane_limit = 2U;
	refused = drv_i915_xd_screen_plan_link(&input, &plan);
	i915_host_check(refused == I915_XD_PLAN_OK && plan.lanes <= 2U && plan.rate_khz == 270000U, "dp: two lanes at most: HBR");

	/* A branch's limits, and a sink without rates. */
	input.sink.lane_limit = 0U;
	input.sink.max_dotclock_khz = 100000U;
	refused = drv_i915_xd_screen_plan_link(&input, &plan);
	i915_host_check(refused == I915_XD_PLAN_LINK, "dp: above a branch's dot clock");
	input.sink.max_dotclock_khz = 0U;
	input.sink.max_bpc = 6U;
	refused = drv_i915_xd_screen_plan_link(&input, &plan);
	i915_host_check(refused == I915_XD_PLAN_OK && plan.bpp == 18U, "dp: a branch's 6 bpc cap");
	input.sink.nrates = 0U;
	refused = drv_i915_xd_screen_plan_link(&input, &plan);
	i915_host_check(refused == I915_XD_PLAN_SINK, "dp: no shared rate");
}

/* Tiger Lake: the pixel rate itself is the need. */
static void
i915_xd_screen_test_tgl(void)
{
	const struct i915_xd_platform *platform;
	struct i915_xd_plan_input input;
	struct i915_display_mode mode;
	struct i915_xd_plan plan;

	/* The panel at 140.8 MHz on a Tiger Lake. */
	platform = i915_xd_screen_test_platform(I915_XD_SCREEN_TEST_TGL, I915_XD_SCREEN_TEST_TGL_REVID);
	i915_host_check(platform != NULL, "tgl: a Tiger Lake description");
	if (platform == NULL)
		return;
	i915_xd_screen_test_mode(&mode, 140800);
	i915_xd_screen_test_panel_input(&input, platform, &mode);
	(void)drv_i915_xd_screen_plan(&input, &plan);
	i915_host_check(plan.min_cdclk_khz == 140800U && plan.cdclk_khz >= 140800U, "tgl: needs the pixel rate itself");
	i915_host_check(plan.refused == I915_XD_PLAN_CDCLK, "tgl: the running 179.2 MHz is not a Tiger Lake step: another CD clock");
}

/* An update that must land in one frame, against the backend above. */
static void
i915_xd_screen_test_update(void)
{
	struct i915_xd_update_report report;
	struct i915_xd_update_hooks hooks;
	struct i915_xd_update update;
	struct i915_xd_io_hooks io_hooks;
	struct i915_xd_evade window;
	struct i915_xd_io io;
	unsigned line;

	/* The fake registers and the backend's hooks. */
	i915_xd_fake_reset();
	i915_xd_fake_hooks(&io_hooks);
	drv_i915_xd_io_init(&io, NULL, NULL);
	drv_i915_xd_io_set_hooks(&io, &io_hooks);
	i915_xd_screen_test_update_hooks(&hooks);

	/* The window of the 5330's panel: 100 us is 7 lines of 2080 pixels at 140.8 MHz; a mode without a line length. */
	drv_i915_xd_pipe_evade_window(140800U, 2080U, 1080U, &window);
	i915_host_check(window.min == 1073 && window.max == 1079 && window.vblank_start == 1080, "update: the window 1073..1079 before the blank at 1080");
	drv_i915_xd_pipe_evade_window(140800U, 0U, 1080U, &window);
	i915_host_check(window.min == 1079 && window.max == 1079, "update: a mode without a line length: one line");

	/* A counter read as 0 is read again until it moves; the offset of an HDMI output. */
	i915_xd_screen_test_lines[0] = 0U;
	i915_xd_screen_test_lines[1] = 0U;
	i915_xd_screen_test_lines[2] = 7U;
	i915_xd_screen_test_line_next = 0U;
	i915_xd_fake_rule_read(I915_XD_PIPE_SCANLINE(0U), i915_xd_screen_test_line_rule, NULL);
	line = drv_i915_xd_pipe_scanline(&io, 0U, 1125U, I915_XD_SCANLINE_OFFSET_HDMI);
	i915_host_check(line == 9U && i915_xd_screen_test_line_next == 3U, "update: a 0 read again until 7, plus HDMI's 2");
	i915_xd_fake_reset();
	i915_xd_fake_hooks(&io_hooks);
	drv_i915_xd_io_set_hooks(&io, &io_hooks);
	i915_xd_fake_set(I915_XD_PIPE_SCANLINE(0U), 1124U);
	line = drv_i915_xd_pipe_scanline(&io, 0U, 1125U, I915_XD_SCANLINE_OFFSET);
	i915_host_check(line == 0U, "update: the last line plus one wraps to 0");

	/* The update: pipe A, 1125 lines, the window above. */
	memset(&update, 0, sizeof(update));
	update.pipe = 0U;
	update.vtotal = 1125U;
	update.scanline_offset = I915_XD_SCANLINE_OFFSET;
	drv_i915_xd_pipe_evade_window(140800U, 2080U, 1080U, &update.window);

	/* A line outside the window: no sleep, the writes with interrupts off, the event's reference kept. */
	memset(&i915_xd_screen_test_backend, 0, sizeof(i915_xd_screen_test_backend));
	i915_xd_fake_set(I915_XD_PIPE_SCANLINE(0U), 499U);
	i915_xd_fake_set(I915_XD_PIPE_FRMCNT(0U), 40U);
	drv_i915_xd_screen_update(&io, &update, &hooks, &report);
	i915_host_check(report.sleeps == 0U && report.start_line == 500U && report.start_frame == 40U && report.end_frame == 40U && !report.crossed && !report.gave_up,
			"update: line 500 is outside the window: written at once");
	i915_host_check(i915_xd_screen_test_backend.arms == 1U && i915_xd_screen_test_backend.arms_irq_on == 0U && i915_xd_screen_test_backend.offs == 1U &&
			!i915_xd_screen_test_backend.irq_off && i915_xd_screen_test_backend.events == 1U && i915_xd_screen_test_backend.refs == 1 &&
			i915_xd_screen_test_backend.errors == 0U,
			"update: one section with interrupts off around the writes, interrupts back on, the event holds one reference");

	/* A line inside the window: one sleep with interrupts on, then the writes. */
	memset(&i915_xd_screen_test_backend, 0, sizeof(i915_xd_screen_test_backend));
	i915_xd_screen_test_backend.sleep_moves = 1;
	i915_xd_fake_set(I915_XD_PIPE_SCANLINE(0U), 1075U);
	drv_i915_xd_screen_update(&io, &update, &hooks, &report);
	i915_host_check(report.sleeps == 1U && i915_xd_screen_test_backend.sleeps_irq_off == 0U && report.start_line == 4U && report.start_frame == 41U &&
			i915_xd_screen_test_backend.arms == 1U && i915_xd_screen_test_backend.arms_irq_on == 0U && i915_xd_screen_test_backend.refs == 1 &&
			!i915_xd_screen_test_backend.irq_off && i915_xd_screen_test_backend.errors == 0U,
			"update: line 1076 is inside: slept to the next frame with interrupts on, written at line 4");

	/* A blank that never comes: the update gives up after its ticks and writes anyway. */
	memset(&i915_xd_screen_test_backend, 0, sizeof(i915_xd_screen_test_backend));
	i915_xd_fake_set(I915_XD_PIPE_SCANLINE(0U), 1075U);
	drv_i915_xd_screen_update(&io, &update, &hooks, &report);
	i915_host_check(report.gave_up && report.sleeps == 2U && i915_xd_screen_test_backend.errors == 1U && i915_xd_screen_test_backend.arms == 1U &&
			i915_xd_screen_test_backend.refs == 1 && !i915_xd_screen_test_backend.irq_off,
			"update: no blank within the two ticks: one error, written anyway, the references balanced");

	/* An interrupt reference refused: no wait, the writes with interrupts off, both refusals reported. */
	memset(&i915_xd_screen_test_backend, 0, sizeof(i915_xd_screen_test_backend));
	i915_xd_screen_test_backend.refuse = 1;
	i915_xd_fake_set(I915_XD_PIPE_SCANLINE(0U), 1075U);
	drv_i915_xd_screen_update(&io, &update, &hooks, &report);
	i915_host_check(report.sleeps == 0U && report.start_frame == 0U && i915_xd_screen_test_backend.refusals == 2U && i915_xd_screen_test_backend.errors == 2U &&
			i915_xd_screen_test_backend.arms == 1U && i915_xd_screen_test_backend.arms_irq_on == 0U && i915_xd_screen_test_backend.events == 1U &&
			!i915_xd_screen_test_backend.irq_off,
			"update: the references refused: written at once with interrupts off, two errors");

	/* Writes that cross a blank. */
	memset(&i915_xd_screen_test_backend, 0, sizeof(i915_xd_screen_test_backend));
	i915_xd_screen_test_backend.arm_crosses = 1;
	i915_xd_fake_set(I915_XD_PIPE_SCANLINE(0U), 499U);
	drv_i915_xd_screen_update(&io, &update, &hooks, &report);
	i915_host_check(report.crossed && report.end_frame == report.start_frame + 1U && i915_xd_screen_test_backend.errors == 1U,
			"update: the frame counter moved during the writes: reported");

	/* A window outside the frame: no wait at all. */
	memset(&i915_xd_screen_test_backend, 0, sizeof(i915_xd_screen_test_backend));
	update.window.min = 0;
	drv_i915_xd_screen_update(&io, &update, &hooks, &report);
	i915_host_check(report.start_frame == 0U && i915_xd_screen_test_backend.refs == 1 && i915_xd_screen_test_backend.offs == 1U &&
			i915_xd_screen_test_backend.arms_irq_on == 0U && i915_xd_screen_test_backend.errors == 0U,
			"update: an empty window: written with interrupts off, no wait, no check");
}

/* Fills the update checks' hooks. */
static void
i915_xd_screen_test_update_hooks(
	struct i915_xd_update_hooks *hooks)
{
	/* Each hook of the backend above. */
	memset(hooks, 0, sizeof(*hooks));
	hooks->vblank_get = i915_xd_screen_test_vblank_get;
	hooks->vblank_put = i915_xd_screen_test_vblank_put;
	hooks->vblank_sleep = i915_xd_screen_test_vblank_sleep;
	hooks->irq_off = i915_xd_screen_test_irq_off;
	hooks->irq_on = i915_xd_screen_test_irq_on;
	hooks->arm = i915_xd_screen_test_arm;
	hooks->arm_event = i915_xd_screen_test_arm_event;
	hooks->error = i915_xd_screen_test_error;
}

/* Takes a reference, or refuses one. */
static int
i915_xd_screen_test_vblank_get(
	void *context)
{
	UNUSED_PARAMETER(context);

	/* A refusing backend. */
	if (i915_xd_screen_test_backend.refuse) {
		i915_xd_screen_test_backend.refusals++;
		return EINVAL;
	}

	/* Succeeded: one reference more. */
	i915_xd_screen_test_backend.refs++;
	return 0;
}

/* Gives a reference back. */
static void
i915_xd_screen_test_vblank_put(
	void *context)
{
	UNUSED_PARAMETER(context);

	/* One reference less. */
	i915_xd_screen_test_backend.refs--;
}

/* Sleeps: the next frame comes (the line leaves the window) when the backend moves it; otherwise a tick runs out. */
static long
i915_xd_screen_test_vblank_sleep(
	void *context,
	long ticks)
{
	UNUSED_PARAMETER(context);

	/* Counts the sleep and one entered with interrupts off. */
	i915_xd_screen_test_backend.sleeps++;
	if (i915_xd_screen_test_backend.irq_off)
		i915_xd_screen_test_backend.sleeps_irq_off++;

	/* A blank that never comes spends a tick. */
	if (!i915_xd_screen_test_backend.sleep_moves)
		return ticks - 1L;

	/* The next frame: line 3, the counter one on. */
	i915_xd_fake_set(I915_XD_PIPE_SCANLINE(0U), 3U);
	i915_xd_fake_set(I915_XD_PIPE_FRMCNT(0U), i915_xd_fake_get(I915_XD_PIPE_FRMCNT(0U)) + 1U);

	/* Succeeded: the blank came with ticks left. */
	return ticks;
}

/* Interrupts off. */
static void
i915_xd_screen_test_irq_off(
	void *context)
{
	UNUSED_PARAMETER(context);

	/* Counts the off. */
	i915_xd_screen_test_backend.irq_off = 1;
	i915_xd_screen_test_backend.offs++;
}

/* Interrupts on. */
static void
i915_xd_screen_test_irq_on(
	void *context)
{
	UNUSED_PARAMETER(context);

	/* Back on. */
	i915_xd_screen_test_backend.irq_off = 0;
}

/* The arming writes: counted, and with interrupts on noted; a crossing backend moves the counter. */
static void
i915_xd_screen_test_arm(
	void *context)
{
	UNUSED_PARAMETER(context);

	/* Counts the writes and those outside the section. */
	i915_xd_screen_test_backend.arms++;
	if (!i915_xd_screen_test_backend.irq_off)
		i915_xd_screen_test_backend.arms_irq_on++;

	/* The blank comes during the writes. */
	if (i915_xd_screen_test_backend.arm_crosses)
		i915_xd_fake_set(I915_XD_PIPE_FRMCNT(0U), i915_xd_fake_get(I915_XD_PIPE_FRMCNT(0U)) + 1U);
}

/* Arms the event. */
static void
i915_xd_screen_test_arm_event(
	void *context)
{
	UNUSED_PARAMETER(context);

	/* Counts the event. */
	i915_xd_screen_test_backend.events++;
}

/* Counts an error. */
static void
i915_xd_screen_test_error(
	void *context,
	const char *what)
{
	UNUSED_PARAMETER(context);
	UNUSED_PARAMETER(what);

	/* One error more. */
	i915_xd_screen_test_backend.errors++;
}

/* Answers the scan line counter's reads of the zero check in order. */
static uint32_t
i915_xd_screen_test_line_rule(
	uint32_t offset,
	uint32_t value,
	void *context)
{
	uint32_t line;

	UNUSED_PARAMETER(offset);
	UNUSED_PARAMETER(value);
	UNUSED_PARAMETER(context);

	/* The next answer, the last one again when they run out. */
	line = i915_xd_screen_test_lines[i915_xd_screen_test_line_next];
	if (i915_xd_screen_test_line_next < 3U)
		i915_xd_screen_test_line_next++;

	/* Succeeded: the answer. */
	return line;
}

/* Notes a step; the step the run names fails with EIO. */
static int
i915_xd_screen_test_note(
	unsigned step)
{
	/* The step, when there is room. */
	if (i915_xd_screen_test_seen.count < 32U) {
		i915_xd_screen_test_seen.steps[i915_xd_screen_test_seen.count] = step;
		i915_xd_screen_test_seen.count++;
	}

	/* The failing step. */
	if (step == i915_xd_screen_test_seen.fail)
		return 5;

	/* Succeeded: the step ran. */
	return 0;
}

/* Light step 1. */
static int
i915_xd_screen_test_s1(
	void *context)
{
	/* Notes the step. */
	(void)context;
	return i915_xd_screen_test_note(1U);
}

/* Light step 2. */
static int
i915_xd_screen_test_s2(
	void *context)
{
	/* Notes the step. */
	(void)context;
	return i915_xd_screen_test_note(2U);
}

/* Light step 3. */
static int
i915_xd_screen_test_s3(
	void *context)
{
	/* Notes the step. */
	(void)context;
	return i915_xd_screen_test_note(3U);
}

/* Light step 4. */
static int
i915_xd_screen_test_s4(
	void *context)
{
	/* Notes the step. */
	(void)context;
	return i915_xd_screen_test_note(4U);
}

/* Light step 5. */
static int
i915_xd_screen_test_s5(
	void *context)
{
	/* Notes the step. */
	(void)context;
	return i915_xd_screen_test_note(5U);
}

/* Light step 6. */
static int
i915_xd_screen_test_s6(
	void *context)
{
	/* Notes the step. */
	(void)context;
	return i915_xd_screen_test_note(6U);
}

/* Light step 7. */
static int
i915_xd_screen_test_s7(
	void *context)
{
	/* Notes the step. */
	(void)context;
	return i915_xd_screen_test_note(7U);
}

/* Light step 8. */
static int
i915_xd_screen_test_s8(
	void *context)
{
	/* Notes the step. */
	(void)context;
	return i915_xd_screen_test_note(8U);
}

/* Light step 9. */
static int
i915_xd_screen_test_s9(
	void *context)
{
	/* Notes the step. */
	(void)context;
	return i915_xd_screen_test_note(9U);
}

/* Light step 10. */
static int
i915_xd_screen_test_s10(
	void *context)
{
	/* Notes the step. */
	(void)context;
	return i915_xd_screen_test_note(10U);
}

/* Light step 11. */
static int
i915_xd_screen_test_s11(
	void *context)
{
	/* Notes the step. */
	(void)context;
	return i915_xd_screen_test_note(11U);
}

/* Light step 12. */
static int
i915_xd_screen_test_s12(
	void *context)
{
	/* Notes the step. */
	(void)context;
	return i915_xd_screen_test_note(12U);
}

/* Dark step 1. */
static int
i915_xd_screen_test_d1(
	void *context)
{
	/* Notes the step. */
	(void)context;
	return i915_xd_screen_test_note(101U);
}

/* Dark step 2. */
static int
i915_xd_screen_test_d2(
	void *context)
{
	/* Notes the step. */
	(void)context;
	return i915_xd_screen_test_note(102U);
}

/* Dark step 3. */
static int
i915_xd_screen_test_d3(
	void *context)
{
	/* Notes the step. */
	(void)context;
	return i915_xd_screen_test_note(103U);
}

/* Dark step 4. */
static int
i915_xd_screen_test_d4(
	void *context)
{
	/* Notes the step. */
	(void)context;
	return i915_xd_screen_test_note(104U);
}

/* Dark step 5. */
static int
i915_xd_screen_test_d5(
	void *context)
{
	/* Notes the step. */
	(void)context;
	return i915_xd_screen_test_note(105U);
}

/* Dark step 6. */
static int
i915_xd_screen_test_d6(
	void *context)
{
	/* Notes the step. */
	(void)context;
	return i915_xd_screen_test_note(106U);
}

/* Dark step 7. */
static int
i915_xd_screen_test_d7(
	void *context)
{
	/* Notes the step. */
	(void)context;
	return i915_xd_screen_test_note(107U);
}

/* Dark step 8. */
static int
i915_xd_screen_test_d8(
	void *context)
{
	/* Notes the step. */
	(void)context;
	return i915_xd_screen_test_note(108U);
}

/* Dark step 9. */
static int
i915_xd_screen_test_d9(
	void *context)
{
	/* Notes the step. */
	(void)context;
	return i915_xd_screen_test_note(109U);
}

/* Fills every hook. */
static void
i915_xd_screen_test_hooks(
	struct i915_xd_screen_hooks *hooks)
{
	/* The light's and the dark's steps, numbered in their order. */
	memset(hooks, 0, sizeof(*hooks));
	hooks->power = i915_xd_screen_test_s1;
	hooks->dbuf = i915_xd_screen_test_s2;
	hooks->setup = i915_xd_screen_test_s3;
	hooks->dmc_on = i915_xd_screen_test_s4;
	hooks->link_power = i915_xd_screen_test_s5;
	hooks->pll = i915_xd_screen_test_s6;
	hooks->port = i915_xd_screen_test_s7;
	hooks->pipe = i915_xd_screen_test_s8;
	hooks->output = i915_xd_screen_test_s9;
	hooks->check = i915_xd_screen_test_s10;
	hooks->plane = i915_xd_screen_test_s11;
	hooks->settle = i915_xd_screen_test_s12;
	hooks->dark_power = i915_xd_screen_test_d1;
	hooks->plane_off = i915_xd_screen_test_d2;
	hooks->output_off = i915_xd_screen_test_d3;
	hooks->port_off = i915_xd_screen_test_d4;
	hooks->pll_off = i915_xd_screen_test_d5;
	hooks->link_power_off = i915_xd_screen_test_d6;
	hooks->dmc_off = i915_xd_screen_test_d7;
	hooks->confirm = i915_xd_screen_test_d8;
	hooks->dark_settle = i915_xd_screen_test_d9;
	hooks->context = NULL;
}

/* Tells whether the steps seen are exactly the wanted ones. */
static int
i915_xd_screen_test_order(
	const unsigned *want,
	unsigned count)
{
	unsigned index;

	/* The count, then each step. */
	if (i915_xd_screen_test_seen.count != count)
		return 0;
	for (index = 0U; index < count; index++) {
		if (i915_xd_screen_test_seen.steps[index] != want[index])
			return 0;
	}

	/* Succeeded: the same steps in the same order. */
	return 1;
}

/* The light's and the dark's order, failures, the undo and the retained dark. */
static void
i915_xd_screen_test_sequencer(void)
{
	static const unsigned light[] = { 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U, 10U, 11U, 12U };
	static const unsigned light_check_fails[] = { 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U, 10U, 12U };
	static const unsigned light_port_undone[] = { 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 104U, 105U, 106U, 107U, 12U };
	static const unsigned dark[] = { 101U, 102U, 103U, 104U, 105U, 106U, 107U, 108U, 109U };
	static const unsigned dark_confirm_fails[] = { 101U, 102U, 103U, 104U, 105U, 106U, 107U, 108U };
	struct i915_xd_screen_hooks hooks;
	struct i915_xd_screen screen;
	struct i915_xd_io_hooks io_hooks;
	struct i915_xd_io io;
	int error;

	/* A window on the register model, for the steps' trace. */
	i915_xd_fake_reset();
	i915_xd_fake_hooks(&io_hooks);
	drv_i915_xd_io_init(&io, NULL, NULL);
	drv_i915_xd_io_set_hooks(&io, &io_hooks);
	i915_xd_screen_test_hooks(&hooks);

	/* A light. */
	memset(&screen, 0, sizeof(screen));
	memset(&i915_xd_screen_test_seen, 0, sizeof(i915_xd_screen_test_seen));
	error = drv_i915_xd_screen_light(&screen, &io, &hooks, 0);
	i915_host_check(error == 0 && i915_xd_screen_test_order(light, 12U) && screen.phase == I915_XD_SCREEN_LIT && screen.failed == -1,
			"sequencer: the light's twelve steps in order, lit");

	/* A failed check: the plane is skipped, the settle runs. */
	memset(&screen, 0, sizeof(screen));
	memset(&i915_xd_screen_test_seen, 0, sizeof(i915_xd_screen_test_seen));
	i915_xd_screen_test_seen.fail = 10U;
	error = drv_i915_xd_screen_light(&screen, &io, &hooks, 0);
	i915_host_check(error == 5 && i915_xd_screen_test_order(light_check_fails, 11U) && screen.failed == 9 && screen.phase == I915_XD_SCREEN_LINKED,
			"sequencer: a failed check skips the plane, the settle still runs, the screen stays linked");

	/* A failed pipe with the undo: the dark from the port on, then the settle. */
	memset(&screen, 0, sizeof(screen));
	memset(&i915_xd_screen_test_seen, 0, sizeof(i915_xd_screen_test_seen));
	i915_xd_screen_test_seen.fail = 8U;
	error = drv_i915_xd_screen_light(&screen, &io, &hooks, 1);
	i915_host_check(error == 5 && screen.phase == I915_XD_SCREEN_OFF && i915_xd_screen_test_order(light_port_undone, 13U),
			"sequencer: a failed pipe with the undo runs the dark from the port off to the pipe DMC, then the settle; the screen is off");

	/* A dark. */
	memset(&screen, 0, sizeof(screen));
	memset(&i915_xd_screen_test_seen, 0, sizeof(i915_xd_screen_test_seen));
	screen.phase = I915_XD_SCREEN_LIT;
	error = drv_i915_xd_screen_dark(&screen, &io, &hooks);
	i915_host_check(error == 0 && i915_xd_screen_test_order(dark, 9U) && screen.phase == I915_XD_SCREEN_OFF, "sequencer: the dark's nine steps in order, off");

	/* A stop not confirmed: retained, nothing given back. */
	memset(&screen, 0, sizeof(screen));
	memset(&i915_xd_screen_test_seen, 0, sizeof(i915_xd_screen_test_seen));
	i915_xd_screen_test_seen.fail = 108U;
	error = drv_i915_xd_screen_dark(&screen, &io, &hooks);
	i915_host_check(error == 5 && i915_xd_screen_test_order(dark_confirm_fails, 8U) && screen.phase == I915_XD_SCREEN_RETAINED,
			"sequencer: a stop not confirmed is retained, its settle not run");

	/* A missing hook does nothing. */
	memset(&hooks, 0, sizeof(hooks));
	memset(&screen, 0, sizeof(screen));
	error = drv_i915_xd_screen_light(&screen, &io, &hooks, 0);
	i915_host_check(error == 0 && screen.ran == 12U && i915_xd_fake_writes() == 0U, "sequencer: without hooks every step runs and nothing is written");
}

/* The flips of a screen. */
static void
i915_xd_screen_test_flips(void)
{
	struct i915_xd_flip flip;
	unsigned verdict;
	int idle;
	int error;

	/* The refusals. */
	memset(&flip, 0, sizeof(flip));
	flip.current = 0x1000U;
	i915_host_check(drv_i915_xd_flip_refused(&flip, 0, 0, 0x2000U) == I915_XD_FLIP_NOT_LIT, "flips: not lit");
	i915_host_check(drv_i915_xd_flip_refused(&flip, 1, 1, 0x2000U) == I915_XD_FLIP_RETAINED, "flips: retained");
	i915_host_check(drv_i915_xd_flip_refused(&flip, 1, 0, 0x1000U) == I915_XD_FLIP_SAME, "flips: the buffer displayed");
	i915_host_check(drv_i915_xd_flip_refused(&flip, 1, 0, 0x2010U) == I915_XD_FLIP_UNALIGNED, "flips: not on a page");
	i915_host_check(drv_i915_xd_flip_refused(&flip, 1, 0, 0x2000U) == I915_XD_FLIP_OK, "flips: a flip may be armed");

	/* Armed: busy until it latches; the poll retires it. */
	drv_i915_xd_flip_arm(&flip, 0x2000U);
	i915_host_check(flip.armed && flip.pending == 0x2000U && flip.old == 0x1000U, "flips: armed, both buffers the display's");
	i915_host_check(drv_i915_xd_flip_refused(&flip, 1, 0, 0x3000U) == I915_XD_FLIP_BUSY, "flips: a second flip while one is armed is refused");
	idle = drv_i915_xd_flip_idle(&flip, 0x1000U);
	i915_host_check(idle == 0 && flip.armed, "flips: the pipe still scans the old buffer: not idle");
	idle = drv_i915_xd_flip_idle(&flip, 0x2000U);
	i915_host_check(idle == 1 && !flip.armed && flip.current == 0x2000U, "flips: latched: retired, the new buffer is the current one");

	/* A waited flip: done, not latched, no event. */
	drv_i915_xd_flip_arm(&flip, 0x1000U);
	verdict = drv_i915_xd_flip_judge(&flip, 0, 0x1000U);
	i915_host_check(verdict == I915_XD_FLIP_DONE && flip.current == 0x1000U && !flip.stuck, "flips: the event and the new buffer: done");
	drv_i915_xd_flip_arm(&flip, 0x2000U);
	verdict = drv_i915_xd_flip_judge(&flip, 0, 0x1000U);
	i915_host_check(verdict == I915_XD_FLIP_NOT_LATCHED && flip.stuck && flip.current == 0x1000U, "flips: the event without the new buffer: stuck");
	i915_host_check(drv_i915_xd_flip_refused(&flip, 1, 0, 0x3000U) == I915_XD_FLIP_BUSY, "flips: a stuck screen refuses flips");
	memset(&flip, 0, sizeof(flip));
	flip.current = 0x1000U;
	drv_i915_xd_flip_arm(&flip, 0x2000U);
	verdict = drv_i915_xd_flip_judge(&flip, 5, 0x1000U);
	i915_host_check(verdict == I915_XD_FLIP_TIMEOUT && flip.stuck && flip.armed, "flips: no event: stuck, still armed");
	idle = drv_i915_xd_flip_idle(&flip, 0x2000U);
	i915_host_check(idle == 1 && flip.armed, "flips: a stuck flip counts as idle for the poll, and is not retired");

	/* The settle. */
	memset(&flip, 0, sizeof(flip));
	flip.current = 0x1000U;
	drv_i915_xd_flip_arm(&flip, 0x2000U);
	error = drv_i915_xd_flip_settled(&flip, 0, 0x2000U);
	i915_host_check(error == 0 && flip.current == 0x2000U && !flip.armed, "flips: settled");
	drv_i915_xd_flip_arm(&flip, 0x3000U);
	error = drv_i915_xd_flip_settled(&flip, 0, 0x2000U);
	i915_host_check(error == EIO && flip.stuck, "flips: not settled: stuck");
}

/* The verdicts of a resident run. */
static void
i915_xd_screen_test_runs(void)
{
	struct i915_xd_run_outcome outcome;
	struct i915_xd_run_result result;
	int error;

	/* Served and given back. */
	memset(&outcome, 0, sizeof(outcome));
	outcome.passed = 1;
	outcome.released = 1;
	outcome.acquired = 1;
	error = drv_i915_xd_screen_run_verdict(&outcome, &result);
	i915_host_check(error == 0 && result.reason == I915_XD_RUN_SERVED && result.acquired, "runs: served");

	/* A link lowered after a clean failure: again. */
	outcome.passed = 0;
	outcome.lowered = 1;
	error = drv_i915_xd_screen_run_verdict(&outcome, &result);
	i915_host_check(error == EAGAIN && result.reason == I915_XD_RUN_FAILED, "runs: an external link lowered: EAGAIN");

	/* Lowered but power still held: failed. */
	outcome.held = 1;
	error = drv_i915_xd_screen_run_verdict(&outcome, &result);
	i915_host_check(error == EIO, "runs: power still held: EIO even with a lowered link");

	/* Unplugged: failed, the reason says why. */
	memset(&outcome, 0, sizeof(outcome));
	outcome.unplugged = 1;
	outcome.released = 1;
	error = drv_i915_xd_screen_run_verdict(&outcome, &result);
	i915_host_check(error == EIO && result.reason == I915_XD_RUN_UNPLUGGED && !result.acquired, "runs: unplugged");

	/* Passed but the buffers not released. */
	memset(&outcome, 0, sizeof(outcome));
	outcome.passed = 1;
	error = drv_i915_xd_screen_run_verdict(&outcome, &result);
	i915_host_check(error == EIO, "runs: buffers kept by the display: EIO");
}
