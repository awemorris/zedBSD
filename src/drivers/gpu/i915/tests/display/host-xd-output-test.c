/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of a screen's output (display/xd-output.c) on the
 * register model: the swing table rows of the sink's level bytes; the
 * light of the 5330's panel on pipe A (the power uses in order, the
 * display buffer, the backlight's setup from the VBT, the pipe's DMC,
 * the AUX power before the PLL, the PLL's words, the port and its link
 * trained against a model sink, the transcoder's words, the start with
 * the backlight); the dark that gives everything back in reverse; the
 * backlight's levels (the user's range, the firmware's request, on and
 * off, a panel the other way round); the faults: a PLL that does not
 * lock, a link that does not train, a pipe found running; an HDMI sink
 * on DDI B; an external DisplayPort sink on TC1 in DisplayPort
 * alternate and in Thunderbolt mode; and on an Ice Lake (display 11) the
 * panel on the eDP transcoder and an external sink on TC1's MG PHY; and
 * on every row of the descriptions the panel, an HDMI sink on the first
 * combo port after DDI A, and on a row with Type-C PHYs a DisplayPort
 * sink on TC1 in DisplayPort alternate and in Thunderbolt mode; and the
 * 5330's two screens (the panel on pipe A, a monitor on the USB-C port
 * TC2 on pipe B) lit one after the other, whose registers must end as
 * the machine's did under Linux; and on display 11, 12 and 13 the order
 * of the steps that differ between the versions (the PLL, the clocks,
 * the transcoder's function, the link's training and transport, the
 * buffer, the transcoder) in the panel's, HDMI's, a Type-C DisplayPort
 * sink's and a Thunderbolt sink's light and dark.
 */

#include "host-test.h"
#include "xd-fake-regs.h"

#include "../../display/xd-ddi.h"
#include "../../display/xd-dmc.h"
#include "../../display/xd-io.h"
#include "../../display/xd-output.h"
#include "../../display/xd-platform.h"
#include "../../display/xd-screen.h"
#include "../../intel/display-dpcd-xd.h"
#include "../../intel/display-regs-xd.h"
#include "../../intel/display-regs-xd-south.h"

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The 5330's Alder Lake-P, and an Ice Lake with port F (D0). */
#define I915_XD_OT_TEST_ADLP		0x46a6U
#define I915_XD_OT_TEST_ADLP_REVID	0x0cU
#define I915_XD_OT_TEST_ICL		0x8a52U
#define I915_XD_OT_TEST_ICL_REVID	0x07U

/* The events the log keeps: the environment's calls and the writes of the registers the test watches. */
#define I915_XD_OT_GET			1U
#define I915_XD_OT_PUT			2U
#define I915_XD_OT_PUT_LATER		3U
#define I915_XD_OT_PANEL		4U
#define I915_XD_OT_SLICES		5U
#define I915_XD_OT_VBLANK_OFF		6U
#define I915_XD_OT_LINK_FAILED		7U
#define I915_XD_OT_OBSERVE		8U
#define I915_XD_OT_DPCD_WRITE		9U
#define I915_XD_OT_WRITE		10U
#define I915_XD_OT_TC			11U

/* The Type-C hooks' events: the link taken and given back, the lanes, the Thunderbolt PLL on and off. */
#define I915_XD_OT_TC_GET		1U
#define I915_XD_OT_TC_PUT		2U
#define I915_XD_OT_TC_LANES		3U
#define I915_XD_OT_TC_PLL_ON		6U
#define I915_XD_OT_TC_PLL_OFF		7U

/* The id of the first Type-C port's own PLL (TC PLL 1). */
#define I915_XD_OT_TC_PLL_1		3U

/* The most events the log keeps. */
#define I915_XD_OT_LOG			512U

/* One event: its kind and two values (the use and the cookie, the register and the value, the address and its first byte). */
struct i915_xd_ot_event {
	unsigned kind;
	uint32_t a;
	uint32_t b;
};

/*
 * The model of the panel's sink: the pattern and levels it was given,
 * the lanes, whether it never recovers its clock, and its power state.
 */
struct i915_xd_ot_sink {
	uint8_t caps[I915_XD_DPCD_CAPS_SIZE];
	uint8_t pattern;
	uint8_t set[4];
	unsigned lanes;
	int cr_never;
	uint8_t power;
	uint8_t oui[3];
};

/*
 * The log of a run: its events in order, the errors the output noted
 * (and the last one's text), and the locks it holds.
 */
struct i915_xd_ot_run {
	struct i915_xd_ot_event log[I915_XD_OT_LOG];
	unsigned count;
	unsigned errors;
	const char *last_error;
	int locks;
	int cookie_next;
	int failing_get;
	unsigned tc_mode;
};

/* The window, the Dekel PHYs' window, the run's log and the sink the checks share; each group starts them afresh. */
static struct i915_xd_io i915_xd_ot_io;
static struct i915_xd_dkl i915_xd_ot_dkl;
static struct i915_xd_ot_run i915_xd_ot_log;
static struct i915_xd_ot_sink i915_xd_ot_sink;

static const struct i915_xd_platform *i915_xd_ot_setup(void);
static void i915_xd_ot_env(struct i915_xd_output_env *env, const struct i915_xd_platform *platform);
static void i915_xd_ot_panel_output(struct i915_xd_output *out);
static void i915_xd_ot_record(unsigned kind, uint32_t a, uint32_t b);
static int i915_xd_ot_find(unsigned kind, uint32_t a, unsigned from);
static int i915_xd_ot_find_value(unsigned kind, uint32_t a, uint32_t b, unsigned from);
static void i915_xd_ot_watch(uint32_t offset, uint32_t value, void *context);
static void i915_xd_ot_buffer(uint32_t offset, uint32_t value, void *context);
static int i915_xd_ot_power_get(void *context, unsigned use, unsigned index);
static void i915_xd_ot_power_put(void *context, unsigned use, unsigned index, int cookie);
static void i915_xd_ot_power_put_later(void *context, unsigned use, unsigned index, int cookie, int delay_ms);
static int i915_xd_ot_panel(void *context, unsigned step);
static void i915_xd_ot_slices(void *context, uint8_t slices);
static void i915_xd_ot_lock(void *context, unsigned lock, int taken);
static void i915_xd_ot_vblank_off(void *context);
static void i915_xd_ot_link_failed(void *context);
static void i915_xd_ot_observe(void *context, unsigned point);
static void i915_xd_ot_error(void *context, const char *text);
static int i915_xd_ot_sink_read(void *context, uint32_t address, uint8_t *buffer, unsigned size);
static int i915_xd_ot_sink_write(void *context, uint32_t address, const uint8_t *buffer, unsigned size);
static int i915_xd_ot_sink_probe(void *context, uint32_t address);
static int i915_xd_ot_sink_caps(void *context, uint8_t *caps);
static void i915_xd_ot_tc_get_link(void *context, unsigned lanes);
static void i915_xd_ot_tc_put_link(void *context);
static unsigned i915_xd_ot_tc_mode(void *context);
static void i915_xd_ot_tc_lanes(void *context, unsigned lanes);
static unsigned i915_xd_ot_tc_pin(void *context);
static int i915_xd_ot_tc_pll(void *context, int on);
static uint32_t i915_xd_ot_dkl_address(unsigned tc, uint32_t phy_offset);
static int i915_xd_ot_light(struct i915_xd_output *out, const struct i915_xd_output_env *env);
static void i915_xd_ot_dark(struct i915_xd_output *out, const struct i915_xd_output_env *env);
static void i915_xd_ot_test_rows(void);
static void i915_xd_ot_test_light(void);
static void i915_xd_ot_test_dark(void);
static void i915_xd_ot_test_backlight(void);
static void i915_xd_ot_test_faults(void);
static void i915_xd_ot_test_hdmi(void);
static void i915_xd_ot_test_type_c(void);
static const struct i915_xd_platform *i915_xd_ot_setup_icl(void);
static void i915_xd_ot_test_icl(void);
static void i915_xd_ot_test_cover(void);
static const struct i915_xd_platform *i915_xd_ot_cover_setup(const struct i915_xd_platform *platform);
static void i915_xd_ot_cover_output(const struct i915_xd_platform *platform, struct i915_xd_output *out);
static void i915_xd_ot_cover_run(const struct i915_xd_platform *platform, struct i915_xd_output *out, unsigned tc_mode, const char *kind);
static void i915_xd_ot_cover_hdmi(const struct i915_xd_platform *platform);
static int i915_xd_ot_cover_tc1(const struct i915_xd_platform *platform, struct i915_xd_output *out);
static void i915_xd_ot_cover_type_c(const struct i915_xd_platform *platform, unsigned tc_mode, const char *kind);
static void i915_xd_ot_test_cover_ports(void);
static void i915_xd_ot_test_end_state(void);
static int i915_xd_ot_find_masked(unsigned kind, uint32_t a, uint32_t mask, uint32_t value, unsigned from);
static void i915_xd_ot_order_step(const struct i915_xd_platform *platform, const char *kind, const char *step, unsigned event, uint32_t a, uint32_t mask, uint32_t value, int *at);
static void i915_xd_ot_order_absent(const struct i915_xd_platform *platform, const char *kind, const char *step, unsigned event, uint32_t a, uint32_t mask, uint32_t value, unsigned from, int to);
static void i915_xd_ot_order_watch(const struct i915_xd_output *out, uint32_t pll_enable, uint32_t transport);
static void i915_xd_ot_order_run(const struct i915_xd_platform *platform, struct i915_xd_output *out, unsigned tc_mode, const char *kind);
static const struct i915_xd_platform *i915_xd_ot_order_row(const char *name);
static void i915_xd_ot_test_order(void);

/* The Type-C hooks of the test: each one logged. */
static const struct i915_xd_output_tc i915_xd_ot_tc = {
	i915_xd_ot_tc_get_link,
	i915_xd_ot_tc_put_link,
	i915_xd_ot_tc_mode,
	i915_xd_ot_tc_lanes,
	i915_xd_ot_tc_pin,
	i915_xd_ot_tc_pll,
};

/*
 * Runs every check of the output and reports.
 */
int
main(void)
{
	int failures;

	/* Runs each group of checks. */
	i915_xd_ot_test_rows();
	i915_xd_ot_test_light();
	i915_xd_ot_test_dark();
	i915_xd_ot_test_backlight();
	i915_xd_ot_test_faults();
	i915_xd_ot_test_hdmi();
	i915_xd_ot_test_type_c();
	i915_xd_ot_test_icl();
	i915_xd_ot_test_cover();
	i915_xd_ot_test_cover_ports();
	i915_xd_ot_test_end_state();
	i915_xd_ot_test_order();

	/* Reports the result. */
	failures = i915_host_report("host_xd_output_test");
	if (failures != 0)
		return 1;

	/* Succeeded: every check passed. */
	return 0;
}

/*
 * Starts a fresh model, log and sink, with the hardware's answers the
 * output waits for: the PLLs' power and lock follow their requests, the
 * DDI buffers leave idle while enabled, the transcoders' state follows
 * their enable, the idle patterns are sent; returns the 5330's
 * description.
 */
static const struct i915_xd_platform *
i915_xd_ot_setup(void)
{
	static const uint32_t watched[] = {
		0x46010U, 0x164280U, 0x164284U, 0x164288U, 0x46140U, 0x60400U, 0x60410U, 0x6001cU, 0x70030U,
		0x60030U, 0x70008U, 0x70034U, 0x45270U, 0x7003cU, 0x45250U, 0x60200U, 0x68180U, 0x68280U,
		0xc8250U, 0xc8254U, 0xc8258U, 0x4438cU, 0x78000U, 0x60540U, 0x46144U, 0x61400U, 0x61030U, 0x61200U,
		0x4610cU, 0x61540U, 0x71008U,
	};
	struct i915_xd_io_hooks hooks;
	struct i915_xd_choice choice;
	unsigned index;
	int error;

	/* A fresh model and a window that reaches it. */
	i915_xd_fake_reset();
	i915_xd_fake_hooks(&hooks);
	drv_i915_xd_io_init(&i915_xd_ot_io, NULL, NULL);
	drv_i915_xd_io_set_hooks(&i915_xd_ot_io, &hooks);
	drv_i915_xd_dkl_init(&i915_xd_ot_dkl, &i915_xd_ot_io);
	memset(&i915_xd_ot_log, 0, sizeof(i915_xd_ot_log));
	i915_xd_ot_log.cookie_next = 1;

	/* The panel's sink: DPCD 1.1, HBR, two lanes, enhanced framing, asleep. */
	memset(&i915_xd_ot_sink, 0, sizeof(i915_xd_ot_sink));
	i915_xd_ot_sink.caps[0] = 0x11U;
	i915_xd_ot_sink.caps[1] = 0x0aU;
	i915_xd_ot_sink.caps[2] = 0x82U;
	i915_xd_ot_sink.power = I915_XD_DPCD_POWER_D3;

	/* The hardware's answers. */
	i915_xd_fake_rule_mirror(0x46010U, 0x08000000U, 0x46010U, -1, 0U);
	i915_xd_fake_rule_mirror(0x46010U, 0x80000000U, 0x46010U, -1, 0U);
	i915_xd_fake_rule_mirror(0x46014U, 0x08000000U, 0x46014U, -1, 0U);
	i915_xd_fake_rule_mirror(0x46014U, 0x80000000U, 0x46014U, -1, 0U);
	i915_xd_fake_rule_mirror(0x70008U, 0x80000000U, 0x70008U, -1, 0U);
	i915_xd_fake_rule_mirror(0x71008U, 0x80000000U, 0x71008U, -1, 0U);
	for (index = 0U; index < 4U; index++) {
		i915_xd_fake_rule_call(I915_XD_DDI_BUF_CTL(index), i915_xd_ot_buffer, NULL);
		i915_xd_fake_set(I915_XD_DDI_BUF_CTL(index), I915_XD_DDI_BUF_IDLE);
	}

	/* The pipes' frame counters are running. */
	i915_xd_fake_set(0x60544U, 0x02000000U);
	i915_xd_fake_set(0x61544U, 0x02000000U);

	/* The registers whose writes the log keeps. */
	for (index = 0U; index < sizeof(watched) / sizeof(watched[0]); index++)
		i915_xd_fake_rule_call(watched[index], i915_xd_ot_watch, NULL);

	/* The description. */
	error = drv_i915_xd_platform_choose(I915_XD_OT_TEST_ADLP, I915_XD_OT_TEST_ADLP_REVID, 0, 0U, &choice);
	if (error != 0)
		return NULL;

	/* Succeeded: the description. */
	return choice.platform;
}

/* Fills the environment the output runs in: the model's window and the test's hooks. */
static void
i915_xd_ot_env(
	struct i915_xd_output_env *env,
	const struct i915_xd_platform *platform)
{
	/* The window, the platform and every hook. */
	memset(env, 0, sizeof(*env));
	env->io = &i915_xd_ot_io;
	env->platform = platform;
	env->power_get = i915_xd_ot_power_get;
	env->power_put = i915_xd_ot_power_put;
	env->power_put_later = i915_xd_ot_power_put_later;
	env->panel = i915_xd_ot_panel;
	env->sink.read = i915_xd_ot_sink_read;
	env->sink.write = i915_xd_ot_sink_write;
	env->sink.probe = i915_xd_ot_sink_probe;
	env->sink.read_caps = i915_xd_ot_sink_caps;
	env->sink.context = &i915_xd_ot_sink;
	env->dbuf_slices = i915_xd_ot_slices;
	env->lock = i915_xd_ot_lock;
	env->vblank_off = i915_xd_ot_vblank_off;
	env->link_failed = i915_xd_ot_link_failed;
	env->observe = i915_xd_ot_observe;
	env->error = i915_xd_ot_error;
	env->dkl = &i915_xd_ot_dkl;
	env->context = &i915_xd_ot_log;
}

/*
 * Describes the 5330's panel: DDI A, AUX A, pipe and transcoder A,
 * 1920x1080 (positive horizontal sync) at 18 bpp with dither on HBR x2, DPLL 0 with its words, the
 * pipe's DMC program loaded, the VBT's 200 Hz PWM with a lowest
 * brightness of 6, the display buffer going from slice 1 to all four
 * joined.
 */
static void
i915_xd_ot_panel_output(
	struct i915_xd_output *out)
{
	/* The port, the pipe and the mode. */
	memset(out, 0, sizeof(*out));
	out->kind = I915_XD_SCREEN_PANEL;
	out->timing.hdisplay = 1920U;
	out->timing.hblank_start = 1920U;
	out->timing.hblank_end = 2080U;
	out->timing.hsync_start = 1936U;
	out->timing.hsync_end = 1952U;
	out->timing.htotal = 2080U;
	out->timing.vdisplay = 1080U;
	out->timing.vblank_start = 1080U;
	out->timing.vblank_end = 1128U;
	out->timing.vsync_start = 1083U;
	out->timing.vsync_end = 1097U;
	out->timing.vtotal = 1128U;
	out->hsync_high = 1;
	out->src_width = 1920U;
	out->src_height = 1080U;
	out->bpp = 18U;
	out->dither = 1;
	out->frame_start_delay = 1U;
	out->pixel_multiplier = 1U;

	/* The link. */
	out->rate_khz = 270000U;
	out->lanes = 2U;
	out->enhanced = 1;
	out->rate_index = -1;
	drv_i915_xd_dp_m_n(18U * 16U, 2U, 140800U, 270000U, 1000000U, &out->m_n);

	/* The PLL, the colours, the line time and the DMC programs. */
	out->pll = 0U;
	out->pll_words.cfgcr0 = 0x00e001a5U;
	out->pll_words.cfgcr1 = 0x00000088U;
	out->gamma_mode = 0U;
	out->csc_mode = 0U;
	out->linetime = 0x78U;
	out->dmc_programs = 1U << I915_XD_DMC_PIPE(0U);

	/* The sink's capabilities. */
	memcpy(out->caps, i915_xd_ot_sink.caps, sizeof(out->caps));

	/* The display buffer of a light from nothing. */
	out->dbuf.slices_before = 0x1U;
	out->dbuf.slices_after = 0xfU;
	out->dbuf.joined_before = 0U;
	out->dbuf.joined_after = 1U;
	out->dbuf.pipes_before = 0U;
	out->dbuf.pipes_after = 0x1U;

	/* The VBT's backlight and the south display's. */
	out->backlight.vbt_present = 1;
	out->backlight.vbt_controller = 0U;
	out->backlight.vbt_hz = 200U;
	out->backlight.vbt_min = 6U;
	out->backlight.rawclk_khz = 19200U;
	out->backlight.controllers = 2U;
	out->backlight.muxed = 1;
}

/* Adds an event to the run's log. */
static void
i915_xd_ot_record(
	unsigned kind,
	uint32_t a,
	uint32_t b)
{
	struct i915_xd_ot_event *event;

	/* A full log keeps its first events. */
	if (i915_xd_ot_log.count == I915_XD_OT_LOG)
		return;

	/* The next event. */
	event = &i915_xd_ot_log.log[i915_xd_ot_log.count];
	event->kind = kind;
	event->a = a;
	event->b = b;
	i915_xd_ot_log.count++;
}

/* Finds the first event of a kind and first value from a position; -1 when none. */
static int
i915_xd_ot_find(
	unsigned kind,
	uint32_t a,
	unsigned from)
{
	unsigned index;

	/* The events in order. */
	for (index = from; index < i915_xd_ot_log.count; index++) {
		if (i915_xd_ot_log.log[index].kind == kind && i915_xd_ot_log.log[index].a == a)
			return (int)index;
	}

	/* None. */
	return -1;
}

/* Finds the first event of a kind and both values from a position; -1 when none. */
static int
i915_xd_ot_find_value(
	unsigned kind,
	uint32_t a,
	uint32_t b,
	unsigned from)
{
	unsigned index;

	/* The events in order. */
	for (index = from; index < i915_xd_ot_log.count; index++) {
		if (i915_xd_ot_log.log[index].kind == kind &&
		    i915_xd_ot_log.log[index].a == a &&
		    i915_xd_ot_log.log[index].b == b)
			return (int)index;
	}

	/* None. */
	return -1;
}

/* Logs a write of a watched register. */
static void
i915_xd_ot_watch(
	uint32_t offset,
	uint32_t value,
	void *context)
{
	(void)context;

	/* The register and its value. */
	i915_xd_ot_record(I915_XD_OT_WRITE, offset, value);
}

/* Answers a write of DDI A's buffer: idle while it is not enabled. */
static void
i915_xd_ot_buffer(
	uint32_t offset,
	uint32_t value,
	void *context)
{
	(void)context;

	/* The idle bit follows the enable. */
	if ((value & I915_XD_DDI_BUF_ENABLE) != 0U) {
		i915_xd_fake_set(offset, value & ~I915_XD_DDI_BUF_IDLE);
	} else {
		i915_xd_fake_set(offset, value | I915_XD_DDI_BUF_IDLE);
	}

	/* Records the write. */
	i915_xd_ot_record(I915_XD_OT_WRITE, offset, value);
}

/* Takes a reference: a new cookie, or 0 when the test makes the get fail. */
static int
i915_xd_ot_power_get(
	void *context,
	unsigned use,
	unsigned index)
{
	struct i915_xd_ot_run *run;
	int cookie;

	UNUSED_PARAMETER(index);

	/* The test's refusal. */
	run = context;
	if (run->failing_get)
		return 0;

	/* Succeeded: the next cookie. */
	cookie = run->cookie_next;
	run->cookie_next++;
	i915_xd_ot_record(I915_XD_OT_GET, use, (uint32_t)cookie);
	return cookie;
}

/* Gives a reference back. */
static void
i915_xd_ot_power_put(
	void *context,
	unsigned use,
	unsigned index,
	int cookie)
{
	(void)context;
	(void)index;

	/* The use and its cookie. */
	i915_xd_ot_record(I915_XD_OT_PUT, use, (uint32_t)cookie);
}

/* Gives a reference back later; the delay is kept in place of the cookie. */
static void
i915_xd_ot_power_put_later(
	void *context,
	unsigned use,
	unsigned index,
	int cookie,
	int delay_ms)
{
	(void)context;
	(void)index;
	(void)cookie;

	/* The use and the delay. */
	i915_xd_ot_record(I915_XD_OT_PUT_LATER, use, (uint32_t)delay_ms);
}

/* Takes a step of the panel's power sequencer. */
static int
i915_xd_ot_panel(
	void *context,
	unsigned step)
{
	(void)context;

	/* The step. */
	i915_xd_ot_record(I915_XD_OT_PANEL, step, 0U);

	/* Succeeded: the step ran. */
	return 0;
}

/* Takes a request for display buffer slices. */
static void
i915_xd_ot_slices(
	void *context,
	uint8_t slices)
{
	(void)context;

	/* The slices. */
	i915_xd_ot_record(I915_XD_OT_SLICES, slices, 0U);
}

/* Counts the locks the output holds. */
static void
i915_xd_ot_lock(
	void *context,
	unsigned lock,
	int taken)
{
	struct i915_xd_ot_run *run;

	UNUSED_PARAMETER(lock);

	/* One more or one less. */
	run = context;
	if (taken) {
		run->locks++;
	} else {
		run->locks--;
	}
}

/* Notes the settling of the pipe's event. */
static void
i915_xd_ot_vblank_off(
	void *context)
{
	(void)context;

	/* The event. */
	i915_xd_ot_record(I915_XD_OT_VBLANK_OFF, 0U, 0U);
}

/* Notes a link that did not train. */
static void
i915_xd_ot_link_failed(
	void *context)
{
	(void)context;

	/* The event. */
	i915_xd_ot_record(I915_XD_OT_LINK_FAILED, 0U, 0U);
}

/* Notes a point of the run. */
static void
i915_xd_ot_observe(
	void *context,
	unsigned point)
{
	(void)context;

	/* The point. */
	i915_xd_ot_record(I915_XD_OT_OBSERVE, point, 0U);
}

/* Counts an error and keeps its text. */
static void
i915_xd_ot_error(
	void *context,
	const char *text)
{
	struct i915_xd_ot_run *run;

	/* The error. */
	run = context;
	run->errors++;
	run->last_error = text;
}

/*
 * Answers a DPCD read of the sink: the link status reports the clock
 * recovered at every level (never with cr_never) and the channel
 * equalized outside pattern 1, with no level requests.
 */
static int
i915_xd_ot_sink_read(
	void *context,
	uint32_t address,
	uint8_t *buffer,
	unsigned size)
{
	struct i915_xd_ot_sink *sink;
	uint8_t status[I915_XD_DPCD_STATUS_SIZE];
	unsigned lane;
	uint8_t nibble;

	/* Everything else reads as zeros. */
	sink = context;
	memset(buffer, 0, size);
	if (address != I915_XD_DPCD_LANE_STATUS)
		return (int)size;

	/* The status of each lane. */
	memset(status, 0, sizeof(status));
	for (lane = 0U; lane < sink->lanes && lane < 4U; lane++) {
		nibble = 0U;
		if (!sink->cr_never)
			nibble |= I915_XD_DPCD_LANE_CR;
		if (!sink->cr_never && (sink->pattern & 0x0fU) != I915_XD_DPCD_PATTERN_1)
			nibble |= I915_XD_DPCD_LANE_EQ | I915_XD_DPCD_LANE_LOCKED;
		status[lane >> 1] |= (uint8_t)(nibble << (4U * (lane & 1U)));
	}

	/* The lanes are aligned. */
	status[2] = I915_XD_DPCD_ALIGN_DONE;
	memcpy(buffer, status, size < sizeof(status) ? size : sizeof(status));

	/* Succeeded: the status is read. */
	return (int)size;
}

/* Takes a DPCD write of the sink: its power, the source's identifier, the pattern and levels, the lanes. */
static int
i915_xd_ot_sink_write(
	void *context,
	uint32_t address,
	const uint8_t *buffer,
	unsigned size)
{
	struct i915_xd_ot_sink *sink;
	unsigned lane;

	/* Every write is logged by its first byte. */
	sink = context;
	i915_xd_ot_record(I915_XD_OT_DPCD_WRITE, address, buffer[0]);

	/* The power state and the source's identifier. */
	if (address == I915_XD_DPCD_SET_POWER)
		sink->power = buffer[0];
	if (address == 0x300U && size == 3U)
		memcpy(sink->oui, buffer, 3U);

	/* The pattern with the levels after it, and the levels alone. */
	if (address == I915_XD_DPCD_PATTERN_SET) {
		sink->pattern = buffer[0];
		for (lane = 0U; lane + 1U < size && lane < 4U; lane++)
			sink->set[lane] = buffer[1U + lane];
	}

	/* The levels the source set. */
	if (address == I915_XD_DPCD_LEVELS_SET) {
		for (lane = 0U; lane < size && lane < 4U; lane++)
			sink->set[lane] = buffer[lane];
	}

	/* The rate and the lanes. */
	if (address == I915_XD_DPCD_BW_SET && size == 2U)
		sink->lanes = buffer[1] & 0x1fU;
	if (address == I915_XD_DPCD_LANES_SET)
		sink->lanes = buffer[0] & 0x1fU;

	/* Succeeded: the bytes are taken. */
	return (int)size;
}

/* Answers that the sink is awake. */
static int
i915_xd_ot_sink_probe(
	void *context,
	uint32_t address)
{
	(void)context;
	(void)address;

	/* Succeeded: the sink answers. */
	return 0;
}

/* Answers the sink's capabilities. */
static int
i915_xd_ot_sink_caps(
	void *context,
	uint8_t *caps)
{
	struct i915_xd_ot_sink *sink;

	/* The model's. */
	sink = context;
	memcpy(caps, sink->caps, I915_XD_DPCD_CAPS_SIZE);

	/* Succeeded: the capabilities are read. */
	return 0;
}

/* Logs the Type-C link taken for a link of so many lanes. */
static void
i915_xd_ot_tc_get_link(
	void *context,
	unsigned lanes)
{
	(void)context;

	/* The event. */
	i915_xd_ot_record(I915_XD_OT_TC, I915_XD_OT_TC_GET, lanes);
}

/* Logs the Type-C link given back. */
static void
i915_xd_ot_tc_put_link(
	void *context)
{
	(void)context;

	/* The event. */
	i915_xd_ot_record(I915_XD_OT_TC, I915_XD_OT_TC_PUT, 0U);
}

/* Answers the mode the test holds the Type-C port in. */
static unsigned
i915_xd_ot_tc_mode(
	void *context)
{
	struct i915_xd_ot_run *run;

	/* The test's mode. */
	run = context;
	return run->tc_mode;
}

/* Logs the PHY's lanes given to the display. */
static void
i915_xd_ot_tc_lanes(
	void *context,
	unsigned lanes)
{
	(void)context;

	/* The event. */
	i915_xd_ot_record(I915_XD_OT_TC, I915_XD_OT_TC_LANES, lanes);
}

/* Answers pin assignment C, which a DisplayPort alternate mode partner of four lanes uses. */
static unsigned
i915_xd_ot_tc_pin(
	void *context)
{
	(void)context;

	/* The test's assignment. */
	return I915_XD_TC_PIN_C;
}

/* Logs the Thunderbolt PLL on or off: 0. */
static int
i915_xd_ot_tc_pll(
	void *context,
	int on)
{
	(void)context;

	/* The event. */
	i915_xd_ot_record(I915_XD_OT_TC, on ? I915_XD_OT_TC_PLL_ON : I915_XD_OT_TC_PLL_OFF, 0U);

	/* Succeeded: the PLL is switched. */
	return 0;
}

/* Returns the address a Type-C port's PHY register is reached at through the Dekel window. */
static uint32_t
i915_xd_ot_dkl_address(
	unsigned tc,
	uint32_t phy_offset)
{
	uint32_t index_reg;
	uint32_t index_value;
	uint32_t address;

	/* The window's address of the register. */
	address = drv_i915_xd_dkl_window(tc, phy_offset, &index_reg, &index_value);
	return address;
}

/* Runs a light's steps of the output as the screen orders them; the first failed step's error, or 0. */
static int
i915_xd_ot_light(
	struct i915_xd_output *out,
	const struct i915_xd_output_env *env)
{
	int first;
	int error;

	/* Each step, the first failure kept. */
	first = 0;
	error = drv_i915_xd_output_dc_hold(out, env);
	if (error != 0 && first == 0)
		first = error;
	error = drv_i915_xd_output_power(out, env);
	if (error != 0 && first == 0)
		first = error;
	error = drv_i915_xd_output_dbuf(out, env);
	if (error != 0 && first == 0)
		first = error;
	error = drv_i915_xd_output_backlight_setup(out, env);
	if (error != 0 && first == 0)
		first = error;
	error = drv_i915_xd_output_dmc(out, env, 1);
	if (error != 0 && first == 0)
		first = error;
	error = drv_i915_xd_output_link_power(out, env);
	if (error != 0 && first == 0)
		first = error;
	error = drv_i915_xd_output_pll(out, env);
	if (error != 0 && first == 0)
		first = error;
	error = drv_i915_xd_output_port(out, env);
	if (error != 0 && first == 0)
		first = error;
	error = drv_i915_xd_output_pipe(out, env);
	if (error != 0 && first == 0)
		first = error;
	error = drv_i915_xd_output_start(out, env);
	if (error != 0 && first == 0)
		first = error;
	error = drv_i915_xd_output_settle(out, env);
	if (error != 0 && first == 0)
		first = error;
	drv_i915_xd_output_dc_allow(out, env);

	/* Reports the first failure, or none. */
	return first;
}

/* Runs a dark's steps of the output as the screen orders them, for the display buffer going back to slice 1. */
static void
i915_xd_ot_dark(
	struct i915_xd_output *out,
	const struct i915_xd_output_env *env)
{
	/* The buffer of a dark that leaves nothing running. */
	out->dbuf.slices_before = 0xfU;
	out->dbuf.slices_after = 0x1U;
	out->dbuf.joined_before = 1U;
	out->dbuf.joined_after = 0U;
	out->dbuf.pipes_before = 0x1U;
	out->dbuf.pipes_after = 0U;

	/* Each step. */
	(void)drv_i915_xd_output_dc_hold(out, env);
	(void)drv_i915_xd_output_dark_power(out, env);
	(void)drv_i915_xd_output_stop(out, env);
	(void)drv_i915_xd_output_port_off(out, env);
	(void)drv_i915_xd_output_pll_off(out, env);
	(void)drv_i915_xd_output_link_power_off(out, env);
	(void)drv_i915_xd_output_dmc(out, env, 0);
	(void)drv_i915_xd_output_dark_settle(out, env);
	drv_i915_xd_output_dc_allow(out, env);
}

/* Checks the swing table rows of the sink's level bytes and the highest swing of a table. */
static void
i915_xd_ot_test_rows(void)
{
	static const uint8_t levels[10] = { 0x00U, 0x08U, 0x10U, 0x18U, 0x01U, 0x09U, 0x11U, 0x02U, 0x0aU, 0x03U };
	struct i915_xd_combo_swing_table table;
	unsigned index;
	unsigned row;
	int ordered;
	int error;

	/* Each pair a sink may ask for takes its row in order. */
	ordered = 1;
	for (index = 0U; index < 10U; index++) {
		error = drv_i915_xd_combo_level_row(levels[index], &row);
		if (error != 0 || row != index)
			ordered = 0;
	}

	/* Checks the ten swing and pre-emphasis pairs take rows 0 to 9 in order. */
	i915_host_check(ordered, "output: the ten swing and pre-emphasis pairs take rows 0 to 9 in order");

	/* A pair beyond the tables, and the bits a level byte carries besides. */
	error = drv_i915_xd_combo_level_row(0x1aU, &row);
	i915_host_check(error == EINVAL && row == 0U, "output: swing 2 with pre-emphasis 3 has no row (row 0)");
	error = drv_i915_xd_combo_level_row(0x0aU | I915_XD_DPCD_SWING_TOP | I915_XD_DPCD_PREEMPH_TOP, &row);
	i915_host_check(error == 0 && row == 8U, "output: the highest-level flags of a level byte do not move its row");

	/* The highest swing a table reaches: that of its last row. */
	memset(&table, 0, sizeof(table));
	table.count = 10U;
	i915_host_check(drv_i915_xd_dp_swing_max(table.count) == 3U, "output: a table of ten rows reaches swing 3");
	table.count = 9U;
	i915_host_check(drv_i915_xd_dp_swing_max(table.count) == 2U, "output: a table of nine rows reaches swing 2");
	table.count = 0U;
	i915_host_check(drv_i915_xd_dp_swing_max(table.count) == 0U, "output: an empty table is taken as its first row");
}

/* Checks the light of the 5330's panel. */
static void
i915_xd_ot_test_light(void)
{
	const struct i915_xd_platform *platform;
	struct i915_xd_output_env env;
	struct i915_xd_output out;
	int dc;
	int core;
	int pipe;
	int trans;
	int lanes;
	int aux;
	int pll;
	int map;
	int io;
	int panel_on;
	int wake;
	int start;
	int pwm;
	int backlight;
	int error;

	/* A light of the panel. */
	platform = i915_xd_ot_setup();
	i915_host_check(platform != NULL, "output: the 5330's description");
	if (platform == NULL)
		return;
	i915_xd_ot_env(&env, platform);
	i915_xd_ot_panel_output(&out);
	error = i915_xd_ot_light(&out, &env);
	i915_host_check(error == 0 && i915_xd_ot_log.errors == 0U && i915_xd_ot_log.locks == 0,
			"output: the panel's light runs through, without errors, every lock given back");

	/* The power uses: DC states off first, then the core, the pipe, the transcoder and the lanes. */
	dc = i915_xd_ot_find(I915_XD_OT_GET, I915_XD_OUTPUT_POWER_DC_OFF, 0U);
	core = i915_xd_ot_find(I915_XD_OT_GET, I915_XD_OUTPUT_POWER_CORE, 0U);
	pipe = i915_xd_ot_find(I915_XD_OT_GET, I915_XD_OUTPUT_POWER_PIPE, 0U);
	trans = i915_xd_ot_find(I915_XD_OT_GET, I915_XD_OUTPUT_POWER_TRANS, 0U);
	lanes = i915_xd_ot_find(I915_XD_OT_GET, I915_XD_OUTPUT_POWER_LANES, 0U);
	i915_host_check(dc == 0 && core > dc && pipe > core && trans > pipe && lanes > trans,
			"output: DC states held off, then the display core, the pipe, the transcoder and the lanes");

	/* The display buffer: the bus joined and the slices of both layouts, then the pipe's credits. */
	i915_host_check(i915_xd_ot_find(I915_XD_OT_SLICES, 0xfU, 0U) > lanes &&
			(i915_xd_fake_get(0x4438cU) & 0x80000000U) != 0U &&
			i915_xd_ot_find(I915_XD_OT_WRITE, 0x7003cU, 0U) > i915_xd_ot_find(I915_XD_OT_SLICES, 0xfU, 0U),
			"output: the bus joined and slices 1 to 4 asked for, then the pipe's data box credits");

	/* The backlight from the VBT: 19.2 MHz over 200 Hz, the lowest duty of 6/255, the level at the highest. */
	i915_host_check(out.backlight.present && out.backlight.max == 96000U && out.backlight.min == 2259U &&
			out.backlight.level == 96000U && out.backlight.enabled,
			"output: the backlight's range from the VBT (96000, lowest 2259), the level at the highest, on");

	/* The pipe's DMC, the AUX power before the PLL, the PLL's words, power and lock. */
	aux = i915_xd_ot_find(I915_XD_OT_GET, I915_XD_OUTPUT_POWER_AUX, 0U);
	pll = i915_xd_ot_find_value(I915_XD_OT_WRITE, 0x46010U, 0x88000000U, 0U);
	if (pll < 0)
		pll = i915_xd_ot_find(I915_XD_OT_WRITE, 0x164284U, 0U);
	i915_host_check((i915_xd_fake_get(0x45250U) & 1U) != 0U && aux > lanes && pll > aux,
			"output: the pipe's DMC switched on, the AUX power taken before the PLL");
	i915_host_check(i915_xd_fake_get(0x164284U) == 0x00e001a5U && i915_xd_fake_get(0x164288U) == 0x00000088U &&
			(i915_xd_fake_get(0x46010U) & 0xcc000000U) == 0xcc000000U && out.pll_on && out.pll_pipes == 1U,
			"output: DPLL 0 with the plan's words, powered and locked, for pipe A");

	/* The port: the panel's power before the clock map and the IO power, the clock select, the function. */
	panel_on = i915_xd_ot_find(I915_XD_OT_PANEL, I915_XD_OUTPUT_PANEL_ON, 0U);
	map = i915_xd_ot_find(I915_XD_OT_WRITE, 0x164280U, 0U);
	io = i915_xd_ot_find(I915_XD_OT_GET, I915_XD_OUTPUT_POWER_DDI_IO, 0U);
	i915_host_check(panel_on > pll && map > panel_on && io > map,
			"output: the panel's power, then DDI A's clock from DPLL 0, then the port's IO power");
	i915_host_check((i915_xd_fake_get(0x164280U) & 0x403U) == 0U && i915_xd_fake_get(0x46140U) == 0x10000000U,
			"output: DDI A's clock ungated on DPLL 0, transcoder A on PHY A's clock");
	i915_host_check(i915_xd_fake_get(0x60400U) == 0x8a210002U,
			"output: transcoder A's function: enabled, DDI A, DP SST, 6 bpc, positive horizontal sync, two lanes");

	/* The sink: the source's identifier, then woken, before the training. */
	wake = i915_xd_ot_find_value(I915_XD_OT_DPCD_WRITE, I915_XD_DPCD_SET_POWER, I915_XD_DPCD_POWER_D0, 0U);
	i915_host_check(i915_xd_ot_find(I915_XD_OT_DPCD_WRITE, 0x300U, 0U) > io && wake > i915_xd_ot_find(I915_XD_OT_DPCD_WRITE, 0x300U, 0U) &&
			i915_xd_ot_sink.oui[1] == 0xaaU && i915_xd_ot_find(I915_XD_OT_DPCD_WRITE, I915_XD_DPCD_PATTERN_SET, 0U) > wake,
			"output: the source's identifier written, the sink woken, then the training");

	/* The link trained and carrying the stream. */
	i915_host_check(out.trained && i915_xd_ot_sink.lanes == 2U && (i915_xd_ot_sink.pattern & 0x0fU) == 0U &&
			(i915_xd_fake_get(I915_XD_DDI_BUF_CTL(0U)) & 0x80000080U) == 0x80000000U &&
			(i915_xd_fake_get(0x60540U) & 0x80000700U) == 0x80000300U,
			"output: the link trained on two lanes, the sink out of the pattern, the buffer on, the transport normal");

	/* The pipe's words: the source, the depth with dither, the stream's attributes, the M/N. */
	i915_host_check(i915_xd_fake_get(0x6001cU) == ((1919U << 16) | 1079U) &&
			(i915_xd_fake_get(0x70030U) & 0xf0U) == 0x50U &&
			i915_xd_ot_find(I915_XD_OT_WRITE, 0x60410U, 0U) >= 0 &&
			i915_xd_fake_get(0x60030U) == ((out.m_n.tu - 1U) << 25 | out.m_n.data_m),
			"output: the pipe's source 1920x1080, 6 bpc dithered, the stream's attributes, the data M/N");

	/* The output: the transcoder running, then the PWM, then the sequencer's backlight enable. */
	start = i915_xd_ot_find_value(I915_XD_OT_WRITE, 0x70008U, 0x80000000U, 0U);
	pwm = i915_xd_ot_find_value(I915_XD_OT_WRITE, 0xc8250U, 0x80000000U, 0U);
	backlight = i915_xd_ot_find(I915_XD_OT_PANEL, I915_XD_OUTPUT_PANEL_BACKLIGHT_ON, 0U);
	i915_host_check(out.lit && (i915_xd_fake_get(0x70008U) & 0xc0000000U) == 0xc0000000U &&
			start > wake && pwm > start && backlight > pwm,
			"output: the transcoder runs, then the PWM starts, then the sequencer's backlight enable");
	i915_host_check(i915_xd_fake_get(0xc8254U) == 96000U && i915_xd_fake_get(0xc8258U) == 96000U,
			"output: the PWM at the VBT's period and the highest duty");

	/* The settle: the new slices alone, DC states back 17 ms later, the uses kept. */
	i915_host_check(i915_xd_ot_find(I915_XD_OT_SLICES, 0xfU, (unsigned)backlight) > backlight &&
			i915_xd_ot_find_value(I915_XD_OT_PUT_LATER, I915_XD_OUTPUT_POWER_DC_OFF, 17U, 0U) >= 0 &&
			i915_xd_ot_find(I915_XD_OT_PUT, I915_XD_OUTPUT_POWER_PIPE, 0U) < 0 &&
			out.held == ((1U << I915_XD_OUTPUT_POWER_CORE) | (1U << I915_XD_OUTPUT_POWER_PIPE) |
				     (1U << I915_XD_OUTPUT_POWER_TRANS) | (1U << I915_XD_OUTPUT_POWER_LANES) |
				     (1U << I915_XD_OUTPUT_POWER_DDI_IO) | (1U << I915_XD_OUTPUT_POWER_AUX)),
			"output: the settle keeps the new slices, lets DC states back after 17 ms, keeps the pipe's power");
}

/* Checks the dark of the panel after its light. */
static void
i915_xd_ot_test_dark(void)
{
	const struct i915_xd_platform *platform;
	struct i915_xd_output_env env;
	struct i915_xd_output out;
	unsigned from;
	int backlight;
	int pwm;
	int vblank;
	int stop;
	int sleep;
	int vdd;
	int off;
	int io;
	int aux;
	int put;

	/* A light, then a dark, of the panel. */
	platform = i915_xd_ot_setup();
	if (platform == NULL)
		return;
	i915_xd_ot_env(&env, platform);
	i915_xd_ot_panel_output(&out);
	(void)i915_xd_ot_light(&out, &env);
	from = i915_xd_ot_log.count;
	i915_xd_ot_dark(&out, &env);
	i915_host_check(i915_xd_ot_log.errors == 0U && i915_xd_ot_log.locks == 0, "output: the panel's dark runs without errors, every lock given back");

	/* The backlight: the sequencer's enable, then the PWM at duty 0 and stopped. */
	backlight = i915_xd_ot_find(I915_XD_OT_PANEL, I915_XD_OUTPUT_PANEL_BACKLIGHT_OFF, from);
	pwm = i915_xd_ot_find_value(I915_XD_OT_WRITE, 0xc8258U, 0U, from);
	i915_host_check(backlight > (int)from && pwm > backlight &&
			(i915_xd_fake_get(0xc8250U) & 0x80000000U) == 0U && !out.backlight.enabled && out.backlight.level == 96000U,
			"output: the sequencer's backlight enable off, then the PWM at duty 0 and stopped, the level kept");

	/* The transcoder: the pipe's event settled first, then stopped and its function cleared, the scalers off. */
	vblank = i915_xd_ot_find(I915_XD_OT_VBLANK_OFF, 0U, from);
	stop = i915_xd_ot_find_value(I915_XD_OT_WRITE, 0x70008U, 0x40000000U, from);
	if (stop < 0)
		stop = i915_xd_ot_find_value(I915_XD_OT_WRITE, 0x70008U, 0x00000000U, from);
	i915_host_check(vblank > pwm && stop > vblank && (i915_xd_fake_get(0x70008U) & 0xc0000000U) == 0U &&
			(i915_xd_fake_get(0x60400U) & 0x80000000U) == 0U &&
			i915_xd_ot_find_value(I915_XD_OT_WRITE, 0x68180U, 0U, from) > stop &&
			i915_xd_ot_find_value(I915_XD_OT_WRITE, 0x68280U, 0U, from) > stop,
			"output: the pipe's event settled, the transcoder stopped, its function off, both scalers off");

	/* The sink to sleep, then the port: buffer off and idle, no clock to the transcoder. */
	sleep = i915_xd_ot_find_value(I915_XD_OT_DPCD_WRITE, I915_XD_DPCD_SET_POWER, I915_XD_DPCD_POWER_D3, from);
	i915_host_check(sleep > stop && i915_xd_ot_sink.power == I915_XD_DPCD_POWER_D3 &&
			(i915_xd_fake_get(I915_XD_DDI_BUF_CTL(0U)) & 0x80000080U) == 0x80U &&
			i915_xd_fake_get(0x46140U) == 0U &&
			i915_xd_ot_find_value(I915_XD_OT_WRITE, I915_XD_DDI_BUF_CTL(0U), 0x00000002U, from) > sleep,
			"output: the sink put to sleep, then the DDI buffer off and idle, no clock to the transcoder");

	/* The panel's power with VDD held, then the port's IO power, then the clock gated. */
	vdd = i915_xd_ot_find(I915_XD_OT_PANEL, I915_XD_OUTPUT_PANEL_VDD_ON, from);
	off = i915_xd_ot_find(I915_XD_OT_PANEL, I915_XD_OUTPUT_PANEL_OFF, from);
	io = i915_xd_ot_find(I915_XD_OT_PUT, I915_XD_OUTPUT_POWER_DDI_IO, from);
	i915_host_check(vdd > sleep && off > vdd && io > off &&
			i915_xd_ot_find(I915_XD_OT_WRITE, 0x164280U, (unsigned)io) > io &&
			(i915_xd_fake_get(0x164280U) & 0x400U) == 0x400U,
			"output: VDD held, the panel off, the port's IO power given back, then DDI A's clock gated");

	/* The PLL off, then the AUX power, then the pipe's DMC. */
	aux = i915_xd_ot_find(I915_XD_OT_PUT, I915_XD_OUTPUT_POWER_AUX, from);
	i915_host_check((i915_xd_fake_get(0x46010U) & 0xcc000000U) == 0U && !out.pll_on && out.pll_pipes == 0U &&
			aux > io && (i915_xd_fake_get(0x45250U) & 1U) == 0U && !out.lit,
			"output: DPLL 0 off, the AUX power given back, the pipe's DMC off, the pipe no longer runs");

	/* The display buffer back to slice 1 split, the pipe's power given back, DC states let back. */
	put = i915_xd_ot_find(I915_XD_OT_PUT, I915_XD_OUTPUT_POWER_LANES, from);
	i915_host_check((i915_xd_fake_get(0x4438cU) & 0x80000000U) == 0U &&
			i915_xd_ot_find(I915_XD_OT_SLICES, 0xfU, from) > aux &&
			i915_xd_ot_find(I915_XD_OT_SLICES, 0x1U, from) > i915_xd_ot_find(I915_XD_OT_SLICES, 0xfU, from) &&
			put > i915_xd_ot_find(I915_XD_OT_SLICES, 0x1U, from) &&
			i915_xd_ot_find(I915_XD_OT_PUT, I915_XD_OUTPUT_POWER_CORE, from) > i915_xd_ot_find(I915_XD_OT_SLICES, 0x1U, from) &&
			i915_xd_ot_find_value(I915_XD_OT_PUT_LATER, I915_XD_OUTPUT_POWER_DC_OFF, 17U, from) > put &&
			out.held == 0U,
			"output: the bus split, slices 1 to 4 then slice 1 alone, the pipe's power given back, DC states back later: nothing held");
}

/* Checks the backlight's levels. */
static void
i915_xd_ot_test_backlight(void)
{
	const struct i915_xd_platform *platform;
	struct i915_xd_output_env env;
	struct i915_xd_output out;
	int error;

	/* A panel whose firmware left the PWM running at its own period and a duty. */
	platform = i915_xd_ot_setup();
	if (platform == NULL)
		return;
	i915_xd_ot_env(&env, platform);
	i915_xd_ot_panel_output(&out);
	i915_xd_fake_set(0xc8250U, 0x80000000U);
	i915_xd_fake_set(0xc8254U, 50000U);
	i915_xd_fake_set(0xc8258U, 30000U);
	error = drv_i915_xd_output_backlight_setup(&out, &env);
	i915_host_check(error == 0 && out.backlight.max == 50000U && out.backlight.level == 30000U && out.backlight.enabled,
			"output: the firmware's period is the range, its duty the level, the PWM found on");

	/* The user's half lies half way from the lowest level, rounded to the closest. */
	drv_i915_xd_output_brightness(&out, &env, 50U, 100U);
	i915_host_check(out.backlight.level == out.backlight.min + (50000U - out.backlight.min + 1U) / 2U &&
			i915_xd_fake_get(0xc8258U) == out.backlight.level,
			"output: the user's half lies half way from the lowest level, shown at once");

	/* The user's 0 is the lowest level, never 0; the user's level comes back from it. */
	drv_i915_xd_output_brightness(&out, &env, 0U, 100U);
	i915_host_check(out.backlight.level == out.backlight.min && out.backlight.level != 0U &&
			drv_i915_xd_output_user_level(&out, 100U) == 0U,
			"output: the user's 0 is the lowest level (not 0), and reads back as 0");

	/* The firmware's request on the whole range, raised to the lowest level. */
	drv_i915_xd_output_brightness_acpi(&out, &env, 1U, 255U);
	i915_host_check(out.backlight.level == out.backlight.min, "output: a firmware request below the lowest level is raised to it");
	drv_i915_xd_output_brightness_acpi(&out, &env, 255U, 255U);
	i915_host_check(out.backlight.level == 50000U, "output: the firmware's highest is the period");

	/* Off keeps the level; on from the lowest comes back at the highest. */
	drv_i915_xd_output_brightness(&out, &env, 0U, 100U);
	drv_i915_xd_output_backlight_off(&out, &env);
	i915_host_check(!out.backlight.enabled && out.backlight.level == out.backlight.min &&
			(i915_xd_fake_get(0xc8250U) & 0x80000000U) == 0U,
			"output: off stops the PWM and keeps the level");
	drv_i915_xd_output_backlight_on(&out, &env);
	i915_host_check(out.backlight.enabled && out.backlight.level == 50000U && i915_xd_fake_get(0xc8258U) == 50000U,
			"output: on from the lowest level comes back at the highest");

	/* A level set while off is shown at the next on. */
	drv_i915_xd_output_backlight_off(&out, &env);
	drv_i915_xd_output_brightness(&out, &env, 100U, 100U);
	i915_host_check(i915_xd_fake_get(0xc8258U) == 0U, "output: a level set while off is not shown");
	drv_i915_xd_output_backlight_on(&out, &env);
	i915_host_check(i915_xd_fake_get(0xc8258U) == 50000U, "output: ... and is shown at the next on");

	/* A panel the other way round takes the range read backwards. */
	out.backlight.inverted = 1;
	drv_i915_xd_output_brightness(&out, &env, 100U, 100U);
	i915_host_check(i915_xd_fake_get(0xc8258U) == out.backlight.min, "output: an inverted panel's highest level is the lowest duty");

	/* A panel without a VBT backlight has none. */
	i915_xd_ot_panel_output(&out);
	out.backlight.vbt_present = 0;
	error = drv_i915_xd_output_backlight_setup(&out, &env);
	i915_host_check(error == 0 && !out.backlight.present, "output: a panel the VBT gives no backlight has none");

	/* No period from the firmware nor the VBT: no backlight can be driven. */
	i915_xd_ot_panel_output(&out);
	out.backlight.rawclk_khz = 0U;
	i915_xd_fake_set(0xc8254U, 0U);
	error = drv_i915_xd_output_backlight_setup(&out, &env);
	i915_host_check(error == ENODEV && !out.backlight.present && i915_xd_ot_log.locks == 0,
			"output: no period from the firmware or the VBT is refused, the lock given back");
}

/* Checks the faults of a light. */
static void
i915_xd_ot_test_faults(void)
{
	struct i915_xd_io_hooks hooks;
	const struct i915_xd_platform *platform;
	struct i915_xd_output_env env;
	struct i915_xd_output out;
	int error;

	/* A PLL that does not lock: the step reports it, the PLL counts as on (the dark turns it off). */
	platform = i915_xd_ot_setup();
	if (platform == NULL)
		return;
	i915_xd_fake_reset();
	i915_xd_fake_hooks(&hooks);
	drv_i915_xd_io_set_hooks(&i915_xd_ot_io, &hooks);
	i915_xd_fake_rule_mirror(0x46010U, 0x08000000U, 0x46010U, -1, 0U);
	i915_xd_ot_env(&env, platform);
	i915_xd_ot_panel_output(&out);
	(void)drv_i915_xd_output_link_power(&out, &env);
	error = drv_i915_xd_output_pll(&out, &env);
	i915_host_check(error == EIO && i915_xd_ot_log.errors == 1U && strstr(i915_xd_ot_log.last_error, "not locked") != NULL &&
			out.pll_on && i915_xd_ot_log.locks == 0,
			"output: a PLL that does not lock is reported, counts as on, the lock given back");
	error = drv_i915_xd_output_pll_off(&out, &env);
	i915_host_check(error == 0 && !out.pll_on && out.pll_pipes == 0U, "output: ... and the dark turns it off");

	/* A link that does not train: the caller hears of it, the link still ends its training. */
	platform = i915_xd_ot_setup();
	if (platform == NULL)
		return;
	i915_xd_ot_env(&env, platform);
	i915_xd_ot_panel_output(&out);
	i915_xd_ot_sink.cr_never = 1;
	(void)i915_xd_ot_light(&out, &env);
	i915_host_check(i915_xd_ot_find(I915_XD_OT_LINK_FAILED, 0U, 0U) >= 0 && out.trained && out.lit,
			"output: a link that does not train is the caller's to lower; the pipe still lights (black)");

	/* A pipe found running is not lit again. */
	error = drv_i915_xd_output_dmc(&out, &env, 1);
	i915_host_check(error == EBUSY, "output: a pipe that runs already is refused");

	/* A failed reference fails its step. */
	platform = i915_xd_ot_setup();
	if (platform == NULL)
		return;
	i915_xd_ot_env(&env, platform);
	i915_xd_ot_panel_output(&out);
	i915_xd_ot_log.failing_get = 1;
	error = drv_i915_xd_output_power(&out, &env);
	i915_host_check(error == EIO && out.held == 0U && i915_xd_ot_log.errors == 1U,
			"output: a power reference that could not be taken fails the step, nothing held");

	/* A pipe without a DMC program has nothing to switch. */
	platform = i915_xd_ot_setup();
	if (platform == NULL)
		return;
	i915_xd_ot_env(&env, platform);
	i915_xd_ot_panel_output(&out);
	out.dmc_programs = 0U;
	(void)drv_i915_xd_output_dmc(&out, &env, 1);
	i915_host_check(i915_xd_ot_find(I915_XD_OT_WRITE, 0x45250U, 0U) < 0, "output: a pipe without a DMC program has nothing switched");
}

/*
 * Checks an HDMI sink on DDI B, pipe B: no AUX power, no panel, the
 * transcoder without M/N in DVI mode, the signal levels and the buffer
 * after the transcoder, and a dark without a sink to put to sleep.
 */
static void
i915_xd_ot_test_hdmi(void)
{
	const struct i915_xd_platform *platform;
	struct i915_xd_output_env env;
	struct i915_xd_output out;
	unsigned from;
	int start;
	int buffer;
	int error;

	/* An HDMI output on DDI B with DPLL 1. */
	platform = i915_xd_ot_setup();
	if (platform == NULL)
		return;
	i915_xd_ot_env(&env, platform);
	i915_xd_ot_panel_output(&out);
	out.kind = I915_XD_SCREEN_HDMI;
	out.ddi = 1U;
	out.phy = 1U;
	out.aux = 1U;
	out.pipe = 1U;
	out.trans = 1U;
	out.lanes = 4U;
	out.rate_khz = 74250U;
	out.bpp = 24U;
	out.dither = 0;
	out.pll = 1U;
	out.hdmi_level = -1;
	out.dmc_programs = 0U;
	out.backlight.vbt_present = 0;
	out.dbuf.pipes_after = 0x2U;
	error = i915_xd_ot_light(&out, &env);
	i915_host_check(error == 0 && i915_xd_ot_log.errors == 0U, "output: an HDMI sink's light runs through");

	/* No AUX power, no panel, no DPCD. */
	i915_host_check(i915_xd_ot_find(I915_XD_OT_GET, I915_XD_OUTPUT_POWER_AUX, 0U) < 0 &&
			i915_xd_ot_find(I915_XD_OT_PANEL, I915_XD_OUTPUT_PANEL_ON, 0U) < 0 &&
			i915_xd_ot_find(I915_XD_OT_DPCD_WRITE, I915_XD_DPCD_SET_POWER, 0U) < 0,
			"output: HDMI takes no AUX power, runs no panel sequence, writes no DPCD");

	/* The transcoder in DVI mode on PHY B's clock, without M/N. */
	i915_host_check(i915_xd_fake_get(0x46144U) == 0x20000000U &&
			(i915_xd_fake_get(0x61400U) & 0xff000000U) == 0x91000000U &&
			i915_xd_ot_find(I915_XD_OT_WRITE, 0x61030U, 0U) < 0,
			"output: transcoder B on PHY B's clock, its function DVI on DDI B, no M/N");

	/* The buffer after the transcoder runs; the packets cleared before. */
	start = i915_xd_ot_find_value(I915_XD_OT_WRITE, 0x71008U, 0x80000000U, 0U);
	buffer = i915_xd_ot_find(I915_XD_OT_WRITE, I915_XD_DDI_BUF_CTL(1U), 0U);
	i915_host_check(i915_xd_ot_find(I915_XD_OT_WRITE, 0x61200U, 0U) < start && buffer > start &&
			(i915_xd_fake_get(I915_XD_DDI_BUF_CTL(1U)) & 0x80000080U) == 0x80000000U,
			"output: the packets cleared before the pipe, DDI B's buffer started after the transcoder");

	/* The dark: no sink to put to sleep, the buffer off, the clock gated. */
	from = i915_xd_ot_log.count;
	i915_xd_ot_dark(&out, &env);
	i915_host_check(i915_xd_ot_find(I915_XD_OT_DPCD_WRITE, I915_XD_DPCD_SET_POWER, from) < 0 &&
			(i915_xd_fake_get(I915_XD_DDI_BUF_CTL(1U)) & 0x80000080U) == 0x80U &&
			(i915_xd_fake_get(0x164280U) & 0x800U) == 0x800U && out.held == 0U,
			"output: HDMI's dark: no DPCD, DDI B's buffer off and idle, its clock gated, nothing held");
}

/*
 * Checks an external DisplayPort sink on TC1, pipe B: the Type-C link
 * before the AUX power, the lanes for the display, the port's own PLL
 * with its words, the clock select, the PHY's DP mode and levels, the
 * PHY ownership in the buffer's word and the reset released before it
 * (DisplayPort alternate mode); in Thunderbolt mode none of the PHY's
 * parts and the Thunderbolt PLL through the caller; and the dark that
 * gives the link back last.
 */
static void
i915_xd_ot_test_type_c(void)
{
	const struct i915_xd_platform *platform;
	const struct i915_xd_pll_desc *pll;
	struct i915_xd_output_env env;
	struct i915_xd_output out;
	uint32_t dp_mode;
	uint32_t dpcntl;
	uint32_t reset;
	unsigned from;
	int get;
	int aux;
	int lanes;
	int buffer;
	int wa;
	int error;

	/* An external sink on TC1 in DisplayPort alternate mode, its PLL answering and its PHY's registers watched. */
	platform = i915_xd_ot_setup();
	if (platform == NULL)
		return;
	pll = drv_i915_xd_dpll_desc(platform, I915_XD_OT_TC_PLL_1);
	if (pll == NULL) {
		i915_host_check(0, "output: the platform has TC PLL 1");
		return;
	}

	/* The PLL's power and lock follow the request. */
	i915_xd_fake_rule_mirror(pll->enable, 0x08000000U, pll->enable, -1, 0U);
	i915_xd_fake_rule_mirror(pll->enable, 0x80000000U, pll->enable, -1, 0U);
	dp_mode = i915_xd_ot_dkl_address(0U, I915_XD_DKL_DP_MODE + I915_XD_DKL_LANE_STRIDE);
	dpcntl = i915_xd_ot_dkl_address(0U, I915_XD_DKL_TX_DPCNTL0);
	reset = i915_xd_ot_dkl_address(0U, I915_XD_DKL_PCS_DW5 + I915_XD_DKL_LANE_STRIDE);
	i915_xd_fake_rule_call(dp_mode, i915_xd_ot_watch, NULL);
	i915_xd_fake_rule_call(dpcntl, i915_xd_ot_watch, NULL);
	i915_xd_fake_rule_call(reset, i915_xd_ot_watch, NULL);
	i915_xd_ot_env(&env, platform);
	env.tc = &i915_xd_ot_tc;
	i915_xd_ot_log.tc_mode = I915_XD_OUTPUT_TC_DP_ALT;
	i915_xd_ot_panel_output(&out);
	out.kind = I915_XD_SCREEN_DP;
	out.ddi = 3U;
	out.phy = 5U;
	out.aux = 3U;
	out.pipe = 1U;
	out.trans = 1U;
	out.tc = 1;
	out.tc_port = 0U;
	out.tc_pll = I915_XD_OT_TC_PLL_1;
	out.tc_pll_words.refclkin = 0x100U;
	out.tc_pll_words.coreclk = 0xa00U;
	out.tc_pll_words.hsclk = 0x6200U;
	out.tc_pll_words.div0 = 0x84269U;
	out.tc_pll_words.div1 = 0x1c0027U;
	out.tc_pll_words.ssc = 0x40002000U;
	out.tc_pll_words.bias = 0x5e000000U;
	out.tc_pll_words.tdc = 0x52U;
	out.rate_khz = 162000U;
	out.lanes = 4U;
	out.dmc_programs = 0U;
	out.backlight.vbt_present = 0;
	out.dbuf.pipes_after = 0x2U;
	error = i915_xd_ot_light(&out, &env);
	i915_host_check(error == 0 && i915_xd_ot_log.errors == 0U && out.trained, "output: a Type-C sink's light runs through and trains");

	/* The link, then the AUX power, then the lanes; the port's own PLL locked, no combo or Thunderbolt PLL. */
	get = i915_xd_ot_find(I915_XD_OT_TC, I915_XD_OT_TC_GET, 0U);
	aux = i915_xd_ot_find(I915_XD_OT_GET, I915_XD_OUTPUT_POWER_AUX, 0U);
	lanes = i915_xd_ot_find(I915_XD_OT_TC, I915_XD_OT_TC_LANES, 0U);
	i915_host_check(get >= 0 && aux > get && lanes > aux &&
			(i915_xd_fake_get(pll->enable) & 0xc0000000U) == 0xc0000000U &&
			i915_xd_ot_find(I915_XD_OT_TC, I915_XD_OT_TC_PLL_ON, 0U) < 0 &&
			(i915_xd_fake_get(0x46010U) & 0x80000000U) == 0U,
			"output: the Type-C link, the AUX power, the PHY's lanes, then TC PLL 1 enabled and locked (no combo or Thunderbolt PLL)");

	/* The clock from the port's own PLL, the TC1 gate open, the PHY's mode (both lanes doubled) and levels. */
	i915_host_check(i915_xd_fake_get(0x4610cU) == 0x80000000U &&
			(i915_xd_fake_get(0x164280U) & (1U << 12)) == 0U &&
			(i915_xd_fake_get(dp_mode) & (I915_XD_DKL_DP_MODE_X1 | I915_XD_DKL_DP_MODE_X2)) == I915_XD_DKL_DP_MODE_X2 &&
			i915_xd_ot_find(I915_XD_OT_WRITE, dpcntl, 0U) > 0 &&
			i915_xd_fake_get(0x46144U) == 0x60000000U,
			"output: TC1's clock from its own PLL, its gate open, the PHY's DP mode and levels, transcoder B on PHY F's clock");

	/* The PHY's reset released before the buffer, which shows the display owns the PHY. */
	wa = i915_xd_ot_find(I915_XD_OT_WRITE, reset, 0U);
	buffer = i915_xd_ot_find(I915_XD_OT_WRITE, I915_XD_DDI_BUF_CTL(3U), 0U);
	i915_host_check(wa > 0 && buffer > wa && (out.buf_word & I915_XD_DDI_BUF_TC_OWNED) != 0U &&
			i915_xd_ot_find(I915_XD_OT_DPCD_WRITE, 0x300U, 0U) < 0,
			"output: the PHY's reset released before TC1's buffer starts, the buffer owns the PHY, no source identifier");

	/* The dark: the clock gated and deselected, the PLL off, then the AUX power, then the link. */
	from = i915_xd_ot_log.count;
	i915_xd_ot_dark(&out, &env);
	i915_host_check(i915_xd_fake_get(0x4610cU) == 0U && (i915_xd_fake_get(0x164280U) & (1U << 12)) != 0U &&
			(i915_xd_fake_get(pll->enable) & 0xc8000000U) == 0U &&
			i915_xd_ot_find(I915_XD_OT_TC, I915_XD_OT_TC_PUT, from) >
			i915_xd_ot_find(I915_XD_OT_PUT, I915_XD_OUTPUT_POWER_AUX, from) &&
			out.held == 0U && !out.tc_linked,
			"output: TC1's clock gated and deselected, TC PLL 1 off, the AUX power, then the link given back");

	/* In Thunderbolt mode: no IO power, no lanes, nothing of the PHY's, the Thunderbolt PLL by the caller, no ownership. */
	platform = i915_xd_ot_setup();
	if (platform == NULL)
		return;
	i915_xd_fake_rule_call(dp_mode, i915_xd_ot_watch, NULL);
	i915_xd_fake_rule_call(dpcntl, i915_xd_ot_watch, NULL);
	i915_xd_fake_rule_call(reset, i915_xd_ot_watch, NULL);
	i915_xd_ot_env(&env, platform);
	env.tc = &i915_xd_ot_tc;
	i915_xd_ot_log.tc_mode = I915_XD_OUTPUT_TC_TBT;
	out.held = 0U;
	out.lit = 0;
	out.pll_on = 0;
	out.pll_pipes = 0U;
	error = i915_xd_ot_light(&out, &env);
	i915_host_check(error == 0 && i915_xd_ot_find(I915_XD_OT_GET, I915_XD_OUTPUT_POWER_DDI_IO, 0U) < 0 &&
			i915_xd_ot_find(I915_XD_OT_TC, I915_XD_OT_TC_LANES, 0U) < 0 &&
			i915_xd_ot_find(I915_XD_OT_TC, I915_XD_OT_TC_PLL_ON, 0U) >= 0 &&
			i915_xd_ot_find(I915_XD_OT_WRITE, dp_mode, 0U) < 0 &&
			i915_xd_ot_find(I915_XD_OT_WRITE, dpcntl, 0U) < 0 &&
			i915_xd_ot_find(I915_XD_OT_WRITE, reset, 0U) < 0 &&
			i915_xd_fake_get(0x4610cU) == 0xc0000000U && (out.buf_word & I915_XD_DDI_BUF_TC_OWNED) == 0U,
			"output: in Thunderbolt mode no IO power, no PHY lanes, mode, levels or reset, the Thunderbolt PLL by the caller at 1.62 GHz, no ownership");
}

/*
 * Starts the model of the other checks on an Ice Lake: the eDP
 * transcoder's state follows its enable, the ports' own transports
 * report their idle patterns sent, and the eDP transcoder's function and
 * clock select and the transports are watched.
 */
static const struct i915_xd_platform *
i915_xd_ot_setup_icl(void)
{
	static const uint32_t watched[] = {
		I915_XD_TRANS_DDI_FUNC_CTL(I915_XD_TRANS_EDP), I915_XD_TRANS_CLK_SEL(I915_XD_TRANS_EDP),
		I915_XD_DP_TP_CTL(I915_XD_DP_TP_PORT(0U)), I915_XD_DP_TP_CTL(I915_XD_DP_TP_PORT(2U)),
	};
	struct i915_xd_choice choice;
	unsigned index;
	int error;
	const struct i915_xd_platform *model;

	/* The model of the other checks. */
	model = i915_xd_ot_setup();
	if (model == NULL)
		return NULL;

	/* The eDP transcoder's state, the ports' transports, the watched registers. */
	i915_xd_fake_rule_mirror(I915_XD_TRANS_CONF(I915_XD_TRANS_EDP), 0x80000000U, I915_XD_TRANS_CONF(I915_XD_TRANS_EDP), -1, 0U);
	i915_xd_fake_set(I915_XD_DP_TP_STATUS(I915_XD_DP_TP_PORT(0U)), 0x02000000U);
	i915_xd_fake_set(I915_XD_DP_TP_STATUS(I915_XD_DP_TP_PORT(2U)), 0x02000000U);
	for (index = 0U; index < sizeof(watched) / sizeof(watched[0]); index++)
		i915_xd_fake_rule_call(watched[index], i915_xd_ot_watch, NULL);

	/* The description. */
	error = drv_i915_xd_platform_choose(I915_XD_OT_TEST_ICL, I915_XD_OT_TEST_ICL_REVID, 0, 0U, &choice);
	if (error != 0)
		return NULL;

	/* Succeeded: the Ice Lake's description. */
	return choice.platform;
}

/* Ice Lake: the panel on the eDP transcoder, then an external sink on TC1's MG PHY. */
static void
i915_xd_ot_test_icl(void)
{
	const struct i915_xd_platform *platform;
	const struct i915_xd_pll_desc *pll;
	struct i915_xd_output_env env;
	struct i915_xd_output out;
	uint32_t function;
	uint32_t func_ctl;
	uint32_t transport;
	uint32_t dp_mode;
	uint32_t swing;
	unsigned from;
	int first_transport;
	int enable;
	int clock;
	int last_train;
	int buffer;
	int error;
	int next;

	/* The panel on DDI A and the eDP transcoder (pipe A), its buffer in one slice. */
	platform = i915_xd_ot_setup_icl();
	i915_host_check(platform != NULL && platform->display_ver == 11U, "icl: the Ice Lake's description");
	if (platform == NULL)
		return;
	i915_xd_ot_env(&env, platform);
	i915_xd_ot_panel_output(&out);
	out.trans = I915_XD_TRANS_EDP;
	out.dmc_programs = 0U;
	out.dbuf.slices_after = 0x1U;
	out.dbuf.joined_after = 0U;
	error = i915_xd_ot_light(&out, &env);
	i915_host_check(error == 0 && i915_xd_ot_log.errors == 0U && out.trained, "icl: the panel's light on the eDP transcoder runs through and trains");

	/* The eDP transcoder's function: DDI A in display 11's select, pipe A's input kept on, enabled; no clock select. */
	func_ctl = I915_XD_TRANS_DDI_FUNC_CTL(I915_XD_TRANS_EDP);
	function = i915_xd_fake_get(func_ctl);
	i915_host_check((function & I915_XD_DDI_FUNC_ENABLE) != 0U && (function & I915_XD_DDI_FUNC_SELECT_11) == 0U &&
			(function & I915_XD_DDI_FUNC_EDP_INPUT) == I915_XD_DDI_FUNC_EDP_INPUT_A_ON &&
			i915_xd_ot_find(I915_XD_OT_WRITE, I915_XD_TRANS_CLK_SEL(I915_XD_TRANS_EDP), 0U) < 0,
			"icl: the eDP transcoder's function selects DDI A with pipe A's input, enabled; its clock is not selected");

	/* The training on port A's own transport, before the function; nothing on transcoder A's. */
	transport = I915_XD_DP_TP_CTL(I915_XD_DP_TP_PORT(0U));
	first_transport = i915_xd_ot_find(I915_XD_OT_WRITE, transport, 0U);
	enable = i915_xd_ot_find(I915_XD_OT_WRITE, func_ctl, 0U);
	i915_host_check(first_transport >= 0 && enable > first_transport && i915_xd_ot_find(I915_XD_OT_WRITE, I915_XD_DP_TP_CTL(0U), 0U) < 0,
			"icl: the link trains on port A's transport, the function is written once after it, transcoder A's transport is untouched");

	/* The dark: the eDP transcoder stopped, its function cleared, still no clock select. */
	from = i915_xd_ot_log.count;
	i915_xd_ot_dark(&out, &env);
	i915_host_check((i915_xd_fake_get(I915_XD_TRANS_CONF(I915_XD_TRANS_EDP)) & 0x80000000U) == 0U &&
			(i915_xd_fake_get(func_ctl) & I915_XD_DDI_FUNC_ENABLE) == 0U &&
			i915_xd_ot_find(I915_XD_OT_WRITE, I915_XD_TRANS_CLK_SEL(I915_XD_TRANS_EDP), from) < 0 && out.held == 0U,
			"icl: the dark stops the eDP transcoder and clears its function, selects no clock, gives everything back");

	/* An external sink on TC1 (DDI C, the MG PHY C) in DisplayPort alternate mode, on pipe and transcoder B. */
	platform = i915_xd_ot_setup_icl();
	if (platform == NULL)
		return;
	pll = drv_i915_xd_dpll_desc(platform, I915_XD_OT_TC_PLL_1);
	if (pll == NULL) {
		i915_host_check(0, "icl: the platform has MG PLL 1");
		return;
	}

	/* The PLL's power and lock follow the request. */
	i915_xd_fake_rule_mirror(pll->enable, 0x08000000U, pll->enable, -1, 0U);
	i915_xd_fake_rule_mirror(pll->enable, 0x80000000U, pll->enable, -1, 0U);
	dp_mode = I915_XD_MG_PHY(0U) + I915_XD_MG_DP_MODE;
	swing = I915_XD_MG_PHY(0U) + I915_XD_MG_TX1_SWINGCTRL;
	i915_xd_fake_rule_call(dp_mode, i915_xd_ot_watch, NULL);
	i915_xd_fake_rule_call(swing, i915_xd_ot_watch, NULL);
	i915_xd_ot_env(&env, platform);
	env.tc = &i915_xd_ot_tc;
	i915_xd_ot_log.tc_mode = I915_XD_OUTPUT_TC_DP_ALT;
	i915_xd_ot_panel_output(&out);
	out.kind = I915_XD_SCREEN_DP;
	out.ddi = 2U;
	out.phy = 2U;
	out.aux = 2U;
	out.pipe = 1U;
	out.trans = 1U;
	out.tc = 1;
	out.tc_port = 0U;
	out.tc_pll = I915_XD_OT_TC_PLL_1;
	error = drv_i915_xd_mg_pll_words(38400U, 162000U, 0, &out.mg_pll_words);
	out.rate_khz = 162000U;
	out.lanes = 4U;
	out.dmc_programs = 0U;
	out.backlight.vbt_present = 0;
	out.dbuf.slices_after = 0x1U;
	out.dbuf.joined_after = 0U;
	out.dbuf.pipes_after = 0x2U;
	if (error == 0)
		error = i915_xd_ot_light(&out, &env);
	i915_host_check(error == 0 && i915_xd_ot_log.errors == 0U && out.trained, "icl: a TC1 sink's light on the MG PHY runs through and trains");

	/* The MG PLL locked, the MG PHY's DP mode and swing written. */
	i915_host_check((i915_xd_fake_get(pll->enable) & 0xc0000000U) == 0xc0000000U &&
			i915_xd_fake_get(I915_XD_MG_PHY(0U) + I915_XD_MG_PLL_DIV0) == out.mg_pll_words.div0 &&
			i915_xd_ot_find(I915_XD_OT_WRITE, dp_mode, 0U) >= 0 && i915_xd_ot_find(I915_XD_OT_WRITE, swing, 0U) >= 0,
			"icl: MG PLL 1 enabled and locked with its words, the MG PHY's DP mode and swing written");

	/* Transcoder B on DDI C's clock in display 11's field, once the link trained on port C's transport. */
	transport = I915_XD_DP_TP_CTL(I915_XD_DP_TP_PORT(2U));
	clock = i915_xd_ot_find_value(I915_XD_OT_WRITE, I915_XD_TRANS_CLK_SEL(1U), 0x60000000U, 0U);
	last_train = i915_xd_ot_find(I915_XD_OT_WRITE, transport, 0U);
	next = last_train;
	while (next >= 0) {
		/* The next write of the transport after the last one found. */
		last_train = next;
		next = i915_xd_ot_find(I915_XD_OT_WRITE, transport, (unsigned)last_train + 1U);
	}

	/* Checks transcoder B's clock select (DDI C, 0x60000000) written once, after the training on port C's transport. */
	i915_host_check(clock >= 0 && last_train >= 0 && clock > last_train && i915_xd_ot_find(I915_XD_OT_WRITE, I915_XD_TRANS_CLK_SEL(1U), 0U) == clock,
			"icl: transcoder B's clock select (DDI C, 0x60000000) written once, after the training on port C's transport");

	/* The dark: the clock off the transcoder before the buffer stops. */
	from = i915_xd_ot_log.count;
	i915_xd_ot_dark(&out, &env);
	clock = i915_xd_ot_find_value(I915_XD_OT_WRITE, I915_XD_TRANS_CLK_SEL(1U), I915_XD_TRANS_CLK_SEL_NONE, from);
	buffer = i915_xd_ot_find(I915_XD_OT_WRITE, I915_XD_DDI_BUF_CTL(2U), from);
	i915_host_check(clock >= 0 && buffer > clock && (i915_xd_fake_get(pll->enable) & 0xc8000000U) == 0U && out.held == 0U,
			"icl: the dark takes the clock off transcoder B before DDI C's buffer stops, MG PLL 1 off, everything given back");
}

/*
 * Checks the panel's light and dark on every row of the descriptions:
 * the same panel on DDI A with DPLL 0 (on display 11 through the eDP
 * transcoder, in one slice, without the pipe's DMC) runs through on
 * each, leaves DDI A's buffer and the transcoder's function on, and the
 * dark turns both off and gives every power reference back.
 */
static void
i915_xd_ot_test_cover(void)
{
	const struct i915_xd_platform *platform;
	struct i915_xd_output_env env;
	struct i915_xd_output out;
	uint32_t func_ctl;
	uint32_t buffer;
	uint32_t function;
	unsigned rows;
	unsigned row;
	unsigned index;
	unsigned kind;
	unsigned gets;
	unsigned puts;
	int lit;
	int dark;
	int error;

	/* Each row, from a fresh model. */
	rows = drv_i915_xd_platform_count();
	for (row = 0U; row < rows; row++) {
		/* The panel of the row's display version. */
		platform = drv_i915_xd_platform_row(row);
		i915_xd_ot_panel_output(&out);
		func_ctl = I915_XD_TRANS_DDI_FUNC_CTL(0U);
		if (platform->display_ver == 11U) {
			(void)i915_xd_ot_setup_icl();
			out.trans = I915_XD_TRANS_EDP;
			out.dmc_programs = 0U;
			out.dbuf.slices_after = 0x1U;
			out.dbuf.joined_after = 0U;
			func_ctl = I915_XD_TRANS_DDI_FUNC_CTL(I915_XD_TRANS_EDP);
		} else {
			(void)i915_xd_ot_setup();
		}

		/* The light on the row. */
		i915_xd_ot_env(&env, platform);
		error = i915_xd_ot_light(&out, &env);
		buffer = i915_xd_fake_get(I915_XD_DDI_BUF_CTL(0U));
		function = i915_xd_fake_get(func_ctl);

		/* The light ran through, the buffer and the function on. */
		lit = 1;
		if (error != 0 || i915_xd_ot_log.errors != 0U)
			lit = 0;
		if (i915_xd_ot_log.locks != 0 || !out.lit)
			lit = 0;
		if ((buffer & 0x80000000U) == 0U || (function & I915_XD_DDI_FUNC_ENABLE) == 0U)
			lit = 0;

		/* Checks the row's light. */
		i915_host_check(lit, platform->name);

		/* The dark on the row. */
		i915_xd_ot_dark(&out, &env);
		buffer = i915_xd_fake_get(I915_XD_DDI_BUF_CTL(0U));
		function = i915_xd_fake_get(func_ctl);

		/* The references the light and the dark took and gave back, at once or later. */
		gets = 0U;
		puts = 0U;
		for (index = 0U; index < i915_xd_ot_log.count; index++) {
			kind = i915_xd_ot_log.log[index].kind;
			if (kind == I915_XD_OT_GET)
				gets++;
			if (kind == I915_XD_OT_PUT || kind == I915_XD_OT_PUT_LATER)
				puts++;
		}

		/* The dark ran through, the buffer and the function off, every reference back. */
		dark = 1;
		if (i915_xd_ot_log.errors != 0U || i915_xd_ot_log.locks != 0)
			dark = 0;
		if ((buffer & 0x80000000U) != 0U || (function & I915_XD_DDI_FUNC_ENABLE) != 0U)
			dark = 0;
		if (gets == 0U || gets != puts)
			dark = 0;
		if (out.held != 0U)
			dark = 0;

		/* Checks the row's dark. */
		i915_host_check(dark, platform->name);
	}
}

/* Starts the model for a row: the Ice Lake's for display 11, the 5330's otherwise; returns the row. */
static const struct i915_xd_platform *
i915_xd_ot_cover_setup(
	const struct i915_xd_platform *platform)
{
	const struct i915_xd_platform *model;

	/* The model of the row's display version. */
	if (platform->display_ver == 11U) {
		model = i915_xd_ot_setup_icl();
	} else {
		model = i915_xd_ot_setup();
	}
	if (model == NULL)
		return NULL;

	/* Succeeded: the row itself, on that model. */
	return platform;
}

/* Describes an external output on pipe and transcoder B for a row, its display buffer in one slice on display 11. */
static void
i915_xd_ot_cover_output(
	const struct i915_xd_platform *platform,
	struct i915_xd_output *out)
{
	/* The panel's mode on pipe B, with nothing of the panel's. */
	i915_xd_ot_panel_output(out);
	out->pipe = 1U;
	out->trans = 1U;
	out->dmc_programs = 0U;
	out->backlight.vbt_present = 0;
	out->dbuf.pipes_after = 0x2U;

	/* Display 11's buffer has one slice. */
	if (platform->display_ver == 11U) {
		out->dbuf.slices_after = 0x1U;
		out->dbuf.joined_after = 0U;
	}
}

/*
 * Lights and darkens one output on a row and checks both: the light runs
 * through (a DisplayPort link trains), the DDI buffer and the
 * transcoder's function are on; the dark turns both off, gives every
 * power reference back and, on a Type-C port, the link too.
 */
static void
i915_xd_ot_cover_run(
	const struct i915_xd_platform *platform,
	struct i915_xd_output *out,
	unsigned tc_mode,
	const char *kind)
{
	static char what[128];
	struct i915_xd_output_env env;
	uint32_t buffer;
	uint32_t function;
	unsigned index;
	unsigned event;
	unsigned gets;
	unsigned puts;
	int lit;
	int dark;
	int error;

	/* The environment, with the Type-C hooks in the mode asked. */
	i915_xd_ot_env(&env, platform);
	env.tc = &i915_xd_ot_tc;
	i915_xd_ot_log.tc_mode = tc_mode;

	/* The light. */
	error = i915_xd_ot_light(out, &env);
	buffer = i915_xd_fake_get(I915_XD_DDI_BUF_CTL(out->ddi));
	function = i915_xd_fake_get(I915_XD_TRANS_DDI_FUNC_CTL(out->trans));
	lit = 1;
	if (error != 0 || i915_xd_ot_log.errors != 0U || i915_xd_ot_log.locks != 0 || !out->lit)
		lit = 0;
	if (out->kind != I915_XD_SCREEN_HDMI && !out->trained)
		lit = 0;
	if ((buffer & I915_XD_DDI_BUF_ENABLE) == 0U || (function & I915_XD_DDI_FUNC_ENABLE) == 0U)
		lit = 0;

	/* Checks the light. */
	(void)snprintf(what, sizeof(what), "cover: %s: %s light", platform->name, kind);
	i915_host_check(lit, what);

	/* The dark. */
	i915_xd_ot_dark(out, &env);
	buffer = i915_xd_fake_get(I915_XD_DDI_BUF_CTL(out->ddi));
	function = i915_xd_fake_get(I915_XD_TRANS_DDI_FUNC_CTL(out->trans));

	/* The references the light and the dark took and gave back, at once or later. */
	gets = 0U;
	puts = 0U;
	for (index = 0U; index < i915_xd_ot_log.count; index++) {
		event = i915_xd_ot_log.log[index].kind;
		if (event == I915_XD_OT_GET)
			gets++;
		if (event == I915_XD_OT_PUT || event == I915_XD_OT_PUT_LATER)
			puts++;
	}

	/* The dark ran through, the buffer and the function off, every reference and the link back. */
	dark = 1;
	if (i915_xd_ot_log.errors != 0U || i915_xd_ot_log.locks != 0)
		dark = 0;
	if ((buffer & I915_XD_DDI_BUF_ENABLE) != 0U || (function & I915_XD_DDI_FUNC_ENABLE) != 0U)
		dark = 0;
	if (gets == 0U || gets != puts || out->held != 0U || out->tc_linked)
		dark = 0;

	/* Checks the dark. */
	(void)snprintf(what, sizeof(what), "cover: %s: %s dark", platform->name, kind);
	i915_host_check(dark, what);
}

/* An HDMI sink on a row's first combo port after DDI A, on DPLL 1. */
static void
i915_xd_ot_cover_hdmi(
	const struct i915_xd_platform *platform)
{
	const struct i915_xd_port_desc *port;
	struct i915_xd_output out;
	unsigned index;

	/* The first combo port that is not DDI A. */
	port = NULL;
	for (index = 0U; index < platform->nports; index++) {
		if (platform->ports[index].phy_kind != I915_XD_PHY_COMBO || platform->ports[index].ddi == 0U)
			continue;
		port = &platform->ports[index];
		break;
	}
	if (port == NULL) {
		i915_host_check(0, "cover: a row without a second combo port");
		return;
	}

	/* The sink at 1080p's 74.25 MHz, eight bits a colour. */
	if (i915_xd_ot_cover_setup(platform) == NULL)
		return;
	i915_xd_ot_cover_output(platform, &out);
	out.kind = I915_XD_SCREEN_HDMI;
	out.ddi = port->ddi;
	out.phy = port->phy;
	out.aux = port->aux;
	out.lanes = 4U;
	out.rate_khz = 74250U;
	out.bpp = 24U;
	out.dither = 0;
	out.pll = 1U;
	out.hdmi_level = -1;

	/* Lights and darkens it. */
	i915_xd_ot_cover_run(platform, &out, I915_XD_OUTPUT_TC_DP_ALT, "HDMI");
}

/* Describes a DisplayPort sink on a row's TC1 (a Dekel or an MG PHY) at 1.62 GHz on four lanes; returns 0, or ENODEV without TC1, its PLL or its words. */
static int
i915_xd_ot_cover_tc1(
	const struct i915_xd_platform *platform,
	struct i915_xd_output *out)
{
	const struct i915_xd_port_desc *port;
	const struct i915_xd_pll_desc *pll;
	unsigned index;
	int error;

	/* TC1, and its own PLL. */
	port = NULL;
	for (index = 0U; index < platform->nports; index++) {
		if (platform->ports[index].tc != 1U)
			continue;
		port = &platform->ports[index];
		break;
	}
	pll = NULL;
	for (index = 0U; index < platform->nplls; index++) {
		if (platform->plls[index].kind != I915_XD_PLL_TC || platform->plls[index].tc != 1U)
			continue;
		pll = &platform->plls[index];
		break;
	}
	if (port == NULL || pll == NULL)
		return ENODEV;

	/* The sink on TC1. */
	i915_xd_ot_cover_output(platform, out);
	out->kind = I915_XD_SCREEN_DP;
	out->ddi = port->ddi;
	out->phy = port->phy;
	out->aux = port->aux;
	out->tc = 1;
	out->tc_port = 0U;
	out->tc_pll = pll->select;
	out->rate_khz = 162000U;
	out->lanes = 4U;

	/* The PLL's words: the MG PLL's from the 38.4 MHz reference, the Dekel PLL's of the 5330's RBR. */
	if (port->phy_kind == I915_XD_PHY_MG) {
		error = drv_i915_xd_mg_pll_words(38400U, 162000U, 0, &out->mg_pll_words);
		if (error != 0)
			return ENODEV;
		return 0;
	}
	out->tc_pll_words.refclkin = 0x100U;
	out->tc_pll_words.coreclk = 0xa00U;
	out->tc_pll_words.hsclk = 0x6200U;
	out->tc_pll_words.div0 = 0x84269U;
	out->tc_pll_words.div1 = 0x1c0027U;
	out->tc_pll_words.ssc = 0x40002000U;
	out->tc_pll_words.bias = 0x5e000000U;
	out->tc_pll_words.tdc = 0x52U;

	/* Succeeded: the sink. */
	return 0;
}

/* A DisplayPort sink on a row's TC1 in one Type-C mode, lit and darkened. */
static void
i915_xd_ot_cover_type_c(
	const struct i915_xd_platform *platform,
	unsigned tc_mode,
	const char *kind)
{
	const struct i915_xd_pll_desc *pll;
	struct i915_xd_output out;
	int error;

	/* The model and the sink. */
	if (i915_xd_ot_cover_setup(platform) == NULL)
		return;
	error = i915_xd_ot_cover_tc1(platform, &out);
	pll = drv_i915_xd_dpll_desc(platform, out.tc_pll);
	if (error != 0 || pll == NULL) {
		i915_host_check(0, "cover: a Type-C row without TC1, its PLL or its words");
		return;
	}

	/* The PLL's power and lock follow the request. */
	i915_xd_fake_rule_mirror(pll->enable, 0x08000000U, pll->enable, -1, 0U);
	i915_xd_fake_rule_mirror(pll->enable, 0x80000000U, pll->enable, -1, 0U);

	/* Lights and darkens it. */
	i915_xd_ot_cover_run(platform, &out, tc_mode, kind);
}

/*
 * Checks every row's external outputs: HDMI on the first combo port
 * after DDI A, and on a row whose TC1 is a Type-C PHY a DisplayPort sink
 * there in DisplayPort alternate and in Thunderbolt mode (a row whose
 * Type-C connectors are combo ports has no Type-C mode).
 */
static void
i915_xd_ot_test_cover_ports(void)
{
	const struct i915_xd_platform *platform;
	const struct i915_xd_port_desc *port;
	unsigned rows;
	unsigned row;
	unsigned index;
	int type_c;

	/* Each row. */
	rows = drv_i915_xd_platform_count();
	for (row = 0U; row < rows; row++) {
		/* HDMI on every row. */
		platform = drv_i915_xd_platform_row(row);
		i915_xd_ot_cover_hdmi(platform);

		/* A row with a Type-C PHY. */
		type_c = 0;
		for (index = 0U; index < platform->nports; index++) {
			port = &platform->ports[index];
			if (port->tc == 1U && (port->phy_kind == I915_XD_PHY_DEKEL || port->phy_kind == I915_XD_PHY_MG))
				type_c = 1;
		}
		if (!type_c)
			continue;

		/* DisplayPort on TC1 in both modes. */
		i915_xd_ot_cover_type_c(platform, I915_XD_OUTPUT_TC_DP_ALT, "DP-TC");
		i915_xd_ot_cover_type_c(platform, I915_XD_OUTPUT_TC_TBT, "TBT");
	}
}

/*
 * One register of the 5330's two screens as the machine read under
 * Linux: its offset, the bits compared, the value and its name.
 */
struct i915_xd_ot_end_reg {
	uint32_t offset;
	uint32_t mask;
	uint32_t value;
	const char *name;
};

/*
 * The registers of the 5330 with its panel on pipe A (DDI A, DPLL 0, HBR
 * on two lanes, 18 bits a pixel with dither) and a 1920x1280 monitor on
 * its USB-C port TC2 on pipe B (DisplayPort alternate mode, TC PLL 2,
 * RBR on four lanes, 30 bits a pixel), read on the machine while Linux
 * drove both.  Only the bits the output writes are compared: of TC2's
 * buffer not bits 12 and 8, which have no field in the Linux v6.8.12
 * definitions (the machine ran a later Linux); of the DDI clock map only
 * the two ports' own bits (DDI A's gate and PLL select, TC2's gate), as
 * the other ports' gates are what the firmware and the sanitize left.
 */
static const struct i915_xd_ot_end_reg i915_xd_ot_end_regs[] = {
	{ 0x60000U, 0xffffffffU, 0x081f077fU, "transcoder A HTOTAL" },
	{ 0x60004U, 0xffffffffU, 0x081f077fU, "transcoder A HBLANK" },
	{ 0x60008U, 0xffffffffU, 0x079f078fU, "transcoder A HSYNC" },
	{ 0x6000cU, 0xffffffffU, 0x04670437U, "transcoder A VTOTAL" },
	{ 0x60010U, 0xffffffffU, 0x04670000U, "transcoder A VBLANK" },
	{ 0x60014U, 0xffffffffU, 0x0448043aU, "transcoder A VSYNC" },
	{ 0x6001cU, 0xffffffffU, 0x077f0437U, "pipe A source size" },
	{ 0x60030U, 0xffffffffU, 0x7e4b17e4U, "transcoder A DATA_M1" },
	{ 0x60034U, 0xffffffffU, 0x00800000U, "transcoder A DATA_N1" },
	{ 0x60040U, 0xffffffffU, 0x00042bfeU, "transcoder A LINK_M1" },
	{ 0x60044U, 0xffffffffU, 0x00080000U, "transcoder A LINK_N1" },
	{ 0x60400U, 0xffffffffU, 0x8a210002U, "transcoder A DDI function" },
	{ 0x60410U, 0xffffffffU, 0x00000001U, "transcoder A MSA misc" },
	{ 0x60540U, 0xffffffffU, 0x80000300U, "transcoder A DP transport" },
	{ 0x70008U, 0xffffffffU, 0xc0000000U, "transcoder A configuration" },
	{ 0x70030U, 0xffffffffU, 0x00800150U, "pipe A misc" },
	{ 0x46140U, 0xffffffffU, 0x10000000U, "transcoder A clock select" },
	{ 0x64000U, 0xffffffffU, 0x80000002U, "DDI A buffer" },
	{ 0x46010U, 0xffffffffU, 0xcc000000U, "DPLL 0 enable" },
	{ 0x164284U, 0xffffffffU, 0x00e001a5U, "DPLL 0 CFGCR0" },
	{ 0x164288U, 0xffffffffU, 0x00000088U, "DPLL 0 CFGCR1" },
	{ 0x61000U, 0xffffffffU, 0x081f077fU, "transcoder B HTOTAL" },
	{ 0x61004U, 0xffffffffU, 0x081f077fU, "transcoder B HBLANK" },
	{ 0x61008U, 0xffffffffU, 0x07cf07afU, "transcoder B HSYNC" },
	{ 0x6100cU, 0xffffffffU, 0x052404ffU, "transcoder B VTOTAL" },
	{ 0x61010U, 0xffffffffU, 0x05240000U, "transcoder B VBLANK" },
	{ 0x61014U, 0xffffffffU, 0x050c0502U, "transcoder B VSYNC" },
	{ 0x6101cU, 0xffffffffU, 0x077f04ffU, "pipe B source size" },
	{ 0x61030U, 0xffffffffU, 0x7e79bf86U, "transcoder B DATA_M1" },
	{ 0x61034U, 0xffffffffU, 0x00800000U, "transcoder B DATA_N1" },
	{ 0x61040U, 0xffffffffU, 0x00081dd5U, "transcoder B LINK_M1" },
	{ 0x61044U, 0xffffffffU, 0x00080000U, "transcoder B LINK_N1" },
	{ 0x61400U, 0xffffffffU, 0xaa110006U, "transcoder B DDI function" },
	{ 0x61410U, 0xffffffffU, 0x00000041U, "transcoder B MSA misc" },
	{ 0x61540U, 0xffffffffU, 0x80040300U, "transcoder B DP transport" },
	{ 0x71008U, 0xffffffffU, 0xc0000000U, "transcoder B configuration" },
	{ 0x71030U, 0xffffffffU, 0x00800120U, "pipe B misc" },
	{ 0x46144U, 0xffffffffU, 0x70000000U, "transcoder B clock select" },
	{ 0x64400U, 0xffffeeffU, 0x80001146U, "DDI TC2 buffer" },
	{ 0x46110U, 0xffffffffU, 0x80000000U, "DDI TC2 clock select" },
	{ 0x46040U, 0xffffffffU, 0xcc000000U, "TC PLL 2 enable" },
	{ 0x164280U, 0x00002403U, 0x01e05800U, "DDI clock map" },
};

/*
 * Lights the 5330's two screens one after the other on the model, the
 * panel on pipe A and the USB-C monitor on TC2 and pipe B, and compares
 * the registers both leave with the machine's under Linux.
 */
static void
i915_xd_ot_test_end_state(void)
{
	static char what[160];
	const struct i915_xd_platform *platform;
	const struct i915_xd_pll_desc *pll;
	const struct i915_xd_ot_end_reg *reg;
	struct i915_xd_output_env env;
	struct i915_xd_output panel;
	struct i915_xd_output monitor;
	uint32_t value;
	unsigned index;
	int error;

	/* The model, TC2's buffer (its PHY owned, as the Type-C connection leaves it) and its PLL answering. */
	platform = i915_xd_ot_setup();
	if (platform == NULL)
		return;
	pll = drv_i915_xd_dpll_desc(platform, 4U);
	if (pll == NULL) {
		i915_host_check(0, "end state: the platform has TC PLL 2");
		return;
	}
	i915_xd_fake_rule_mirror(pll->enable, 0x08000000U, pll->enable, -1, 0U);
	i915_xd_fake_rule_mirror(pll->enable, 0x80000000U, pll->enable, -1, 0U);
	i915_xd_fake_rule_call(I915_XD_DDI_BUF_CTL(4U), i915_xd_ot_buffer, NULL);
	i915_xd_fake_set(I915_XD_DDI_BUF_CTL(4U), I915_XD_DDI_BUF_IDLE | I915_XD_DDI_BUF_TC_OWNED);
	i915_xd_fake_rule_call(0x61540U, i915_xd_ot_watch, NULL);
	i915_xd_ot_env(&env, platform);
	env.tc = &i915_xd_ot_tc;
	i915_xd_ot_log.tc_mode = I915_XD_OUTPUT_TC_DP_ALT;

	/* The panel on pipe A; its sink has no enhanced framing (its DPCD's third byte is 0x02). */
	i915_xd_ot_sink.caps[2] = 0x02U;
	i915_xd_ot_panel_output(&panel);
	panel.enhanced = 0;
	error = i915_xd_ot_light(&panel, &env);
	i915_host_check(error == 0 && i915_xd_ot_log.errors == 0U && panel.trained, "end state: the panel lights on pipe A");

	/* The monitor's sink: DPCD 1.4, RBR to HBR2, four lanes, enhanced framing, asleep. */
	memset(&i915_xd_ot_sink, 0, sizeof(i915_xd_ot_sink));
	i915_xd_ot_sink.caps[0] = 0x14U;
	i915_xd_ot_sink.caps[1] = 0x14U;
	i915_xd_ot_sink.caps[2] = 0x84U;
	i915_xd_ot_sink.power = I915_XD_DPCD_POWER_D3;

	/* The monitor on TC2 and pipe B: 1920x1280 at 164.36 MHz, 30 bits a pixel, RBR on four lanes, TC PLL 2. */
	i915_xd_ot_panel_output(&monitor);
	monitor.kind = I915_XD_SCREEN_DP;
	monitor.ddi = 4U;
	monitor.phy = 6U;
	monitor.aux = 4U;
	monitor.pipe = 1U;
	monitor.trans = 1U;
	monitor.tc = 1;
	monitor.tc_port = 1U;
	monitor.tc_pll = 4U;
	monitor.tc_pll_words.refclkin = 0x100U;
	monitor.tc_pll_words.coreclk = 0xa00U;
	monitor.tc_pll_words.hsclk = 0x6200U;
	monitor.tc_pll_words.div0 = 0x84269U;
	monitor.tc_pll_words.div1 = 0x1c0027U;
	monitor.tc_pll_words.ssc = 0x40002000U;
	monitor.tc_pll_words.bias = 0x5e000000U;
	monitor.tc_pll_words.tdc = 0x52U;
	monitor.timing.hdisplay = 1920U;
	monitor.timing.hblank_start = 1920U;
	monitor.timing.hblank_end = 2080U;
	monitor.timing.hsync_start = 1968U;
	monitor.timing.hsync_end = 2000U;
	monitor.timing.htotal = 2080U;
	monitor.timing.vdisplay = 1280U;
	monitor.timing.vblank_start = 1280U;
	monitor.timing.vblank_end = 1317U;
	monitor.timing.vsync_start = 1283U;
	monitor.timing.vsync_end = 1293U;
	monitor.timing.vtotal = 1317U;
	monitor.src_width = 1920U;
	monitor.src_height = 1280U;
	monitor.bpp = 30U;
	monitor.dither = 0;
	monitor.rate_khz = 162000U;
	monitor.lanes = 4U;
	drv_i915_xd_dp_m_n(30U * 16U, 4U, 164360U, 162000U, 1000000U, &monitor.m_n);
	monitor.linetime = 0x66U;
	monitor.dmc_programs = 0U;
	monitor.backlight.vbt_present = 0;
	monitor.dbuf.slices_before = 0xfU;
	monitor.dbuf.joined_before = 1U;
	monitor.dbuf.joined_after = 0U;
	monitor.dbuf.pipes_before = 0x1U;
	monitor.dbuf.pipes_after = 0x3U;
	error = i915_xd_ot_light(&monitor, &env);
	i915_host_check(error == 0 && i915_xd_ot_log.errors == 0U && monitor.trained, "end state: the monitor lights on TC2 and pipe B");

	/* Each register against the machine's. */
	for (index = 0U; index < sizeof(i915_xd_ot_end_regs) / sizeof(i915_xd_ot_end_regs[0]); index++) {
		reg = &i915_xd_ot_end_regs[index];
		value = i915_xd_fake_get(reg->offset);
		(void)snprintf(what, sizeof(what), "end state: %s (0x%06x) 0x%08x, the machine's 0x%08x", reg->name, reg->offset, value, reg->value);
		i915_host_check((value & reg->mask) == (reg->value & reg->mask), what);
	}
}

/* Finds the first event of a kind on a register (or step) whose value has the bits asked, from an index; -1 for none. */
static int
i915_xd_ot_find_masked(
	unsigned kind,
	uint32_t a,
	uint32_t mask,
	uint32_t value,
	unsigned from)
{
	const struct i915_xd_ot_event *event;
	unsigned index;

	/* The first matching event at or after the index. */
	for (index = from; index < i915_xd_ot_log.count; index++) {
		event = &i915_xd_ot_log.log[index];
		if (event->kind != kind || event->a != a)
			continue;
		if ((event->b & mask) != value)
			continue;

		/* Succeeded: its index. */
		return (int)index;
	}

	/* No such event. */
	return -1;
}

/*
 * Checks that a step comes after the one before it in the run (at holds
 * the index of the step before, and then of this one; once a step is
 * missing the rest are not looked for).
 */
static void
i915_xd_ot_order_step(
	const struct i915_xd_platform *platform,
	const char *kind,
	const char *step,
	unsigned event,
	uint32_t a,
	uint32_t mask,
	uint32_t value,
	int *at)
{
	static char what[160];
	int found;

	/* The step at or after the one before. */
	found = -1;
	if (*at >= 0)
		found = i915_xd_ot_find_masked(event, a, mask, value, (unsigned)*at);

	/* Checks the step came after the one before. */
	(void)snprintf(what, sizeof(what), "order: %s: %s: %s", platform->name, kind, step);
	i915_host_check(found >= 0, what);
	*at = found;
}

/* Checks that a step does not come between two indices of the run (to -1: the end of the run). */
static void
i915_xd_ot_order_absent(
	const struct i915_xd_platform *platform,
	const char *kind,
	const char *step,
	unsigned event,
	uint32_t a,
	uint32_t mask,
	uint32_t value,
	unsigned from,
	int to)
{
	static char what[160];
	int found;
	int absent;

	/* The step anywhere in the range. */
	found = i915_xd_ot_find_masked(event, a, mask, value, from);
	absent = found < 0;
	if (found >= 0 && to >= 0 && found >= to)
		absent = 1;

	/* Checks the step was not in the range. */
	(void)snprintf(what, sizeof(what), "order: %s: %s: %s", platform->name, kind, step);
	i915_host_check(absent, what);
}

/* Logs the writes of the registers whose order the run checks. */
static void
i915_xd_ot_order_watch(
	const struct i915_xd_output *out,
	uint32_t pll_enable,
	uint32_t transport)
{
	/* The transcoder's timing, function, configuration and clock, the transport, the DDI's clock and the PLL. */
	i915_xd_fake_rule_call(0x60000U + 0x1000U * out->trans, i915_xd_ot_watch, NULL);
	i915_xd_fake_rule_call(I915_XD_TRANS_DDI_FUNC_CTL(out->trans), i915_xd_ot_watch, NULL);
	i915_xd_fake_rule_call(I915_XD_TRANS_CONF(out->trans), i915_xd_ot_watch, NULL);
	i915_xd_fake_rule_call(I915_XD_TRANS_CLK_SEL(out->trans), i915_xd_ot_watch, NULL);
	i915_xd_fake_rule_call(I915_XD_DP_TP_CTL(transport), i915_xd_ot_watch, NULL);
	i915_xd_fake_rule_call(I915_XD_DDI_CLK_SEL(out->ddi), i915_xd_ot_watch, NULL);
	i915_xd_fake_rule_call(I915_XD_DPCLKA_CFGCR0, i915_xd_ot_watch, NULL);
	if (pll_enable != 0U)
		i915_xd_fake_rule_call(pll_enable, i915_xd_ot_watch, NULL);
}

/*
 * Lights and darkens one output and checks the order of its steps
 * against the order of the version's sequence.
 *
 * Light: the PLL, the DDI's clock (the panel's power before it), on
 * display 12 and later the transcoder's clock and its function without
 * the enable, then for DisplayPort the sink woken, the training on the
 * transport, the buffer and the transport's normal pattern, on display
 * 11 the transcoder's clock after the training; then the transcoder's
 * timing, its function enabled and the transcoder started.  HDMI takes
 * the transcoder's clock before the timing and starts the buffer after
 * the transcoder.  Dark: the transcoder stopped, its function off, a
 * DisplayPort sink put to sleep, on display 11 the transcoder's clock
 * off before the buffer, the buffer stopped, from display 12 the clock
 * off after it, the panel's power, the DDI's clock and the PLL off.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_display.c (hsw_crtc_enable,
 * hsw_crtc_disable), intel_ddi.c (tgl_ddi_pre_enable_dp,
 * hsw_ddi_pre_enable_dp, intel_ddi_pre_enable_hdmi, intel_enable_ddi,
 * intel_enable_ddi_hdmi, intel_ddi_post_disable,
 * intel_ddi_post_disable_dp, intel_ddi_post_disable_hdmi).
 */
static void
i915_xd_ot_order_run(
	const struct i915_xd_platform *platform,
	struct i915_xd_output *out,
	unsigned tc_mode,
	const char *kind)
{
	const struct i915_xd_pll_desc *pll;
	struct i915_xd_output_env env;
	uint32_t pll_enable;
	uint32_t transport;
	uint32_t clock_mask;
	uint32_t clock_select;
	unsigned from;
	int tbt;
	int dp;
	int at;

	/* The PLL's enable (none in Thunderbolt mode, where the caller runs it) and the transport. */
	tbt = out->tc && tc_mode == I915_XD_OUTPUT_TC_TBT;
	dp = out->kind != I915_XD_SCREEN_HDMI;
	pll_enable = 0U;
	pll = drv_i915_xd_dpll_desc(platform, out->tc ? out->tc_pll : out->pll);
	if (pll != NULL && !tbt)
		pll_enable = pll->enable;
	transport = out->trans;
	if (platform->display_ver < 12U)
		transport = I915_XD_DP_TP_PORT(out->ddi);

	/* The transcoder's clock select: the PHY's index plus one, in the version's field. */
	clock_mask = I915_XD_TRANS_CLK_SEL_FIELD;
	if (platform->display_ver < 12U)
		clock_mask = I915_XD_TRANS_CLK_SEL_FIELD_11;
	clock_select = i915_xd_field_put(clock_mask, out->phy + 1U);
	if (out->tc && pll != NULL) {
		i915_xd_fake_rule_mirror(pll->enable, 0x08000000U, pll->enable, -1, 0U);
		i915_xd_fake_rule_mirror(pll->enable, 0x80000000U, pll->enable, -1, 0U);
	}
	i915_xd_ot_order_watch(out, pll_enable, transport);

	/* The environment, with the Type-C hooks in the mode asked. */
	i915_xd_ot_env(&env, platform);
	env.tc = &i915_xd_ot_tc;
	i915_xd_ot_log.tc_mode = tc_mode;

	/* The light. */
	(void)i915_xd_ot_light(out, &env);
	at = 0;
	if (tbt) {
		i915_xd_ot_order_step(platform, kind, "light: the Thunderbolt PLL", I915_XD_OT_TC, I915_XD_OT_TC_PLL_ON, 0U, 0U, &at);
	} else {
		i915_xd_ot_order_step(platform, kind, "light: the PLL", I915_XD_OT_WRITE, pll_enable, 0x80000000U, 0x80000000U, &at);
	}
	if (out->kind == I915_XD_SCREEN_PANEL)
		i915_xd_ot_order_step(platform, kind, "light: the panel's power", I915_XD_OT_PANEL, I915_XD_OUTPUT_PANEL_ON, 0U, 0U, &at);
	if (out->tc) {
		i915_xd_ot_order_step(platform, kind, "light: the DDI's clock select", I915_XD_OT_WRITE, I915_XD_DDI_CLK_SEL(out->ddi), 0xf0000000U, tbt ? 0xc0000000U : 0x80000000U, &at);
	} else {
		i915_xd_ot_order_step(platform, kind, "light: the DDI's clock map", I915_XD_OT_WRITE, I915_XD_DPCLKA_CFGCR0, 0U, 0U, &at);
	}
	if (!dp || platform->display_ver >= 12U) {
		if (out->trans != I915_XD_TRANS_EDP)
			i915_xd_ot_order_step(platform, kind, "light: the transcoder's clock", I915_XD_OT_WRITE, I915_XD_TRANS_CLK_SEL(out->trans), clock_mask, clock_select, &at);
	}
	if (dp && platform->display_ver >= 12U)
		i915_xd_ot_order_step(platform, kind, "light: the function without its enable", I915_XD_OT_WRITE, I915_XD_TRANS_DDI_FUNC_CTL(out->trans), I915_XD_DDI_FUNC_ENABLE, 0U, &at);
	if (dp) {
		i915_xd_ot_order_step(platform, kind, "light: the sink woken", I915_XD_OT_DPCD_WRITE, I915_XD_DPCD_SET_POWER, 0xffU, I915_XD_DPCD_POWER_D0, &at);
		i915_xd_ot_order_step(platform, kind, "light: the training on the transport", I915_XD_OT_WRITE, I915_XD_DP_TP_CTL(transport), I915_XD_DP_TP_ENABLE, I915_XD_DP_TP_ENABLE, &at);
		if (platform->display_ver < 12U)
			i915_xd_ot_order_absent(platform, kind, "light: no function before the training", I915_XD_OT_WRITE, I915_XD_TRANS_DDI_FUNC_CTL(out->trans), 0U, 0U, 0U, at);
		i915_xd_ot_order_step(platform, kind, "light: the buffer", I915_XD_OT_WRITE, I915_XD_DDI_BUF_CTL(out->ddi), I915_XD_DDI_BUF_ENABLE, I915_XD_DDI_BUF_ENABLE, &at);
		i915_xd_ot_order_step(platform, kind, "light: the normal pattern", I915_XD_OT_WRITE, I915_XD_DP_TP_CTL(transport), I915_XD_DP_TP_PATTERN, 0x300U, &at);
		if (platform->display_ver < 12U)
			i915_xd_ot_order_absent(platform, kind, "light: no transcoder clock before the training ends", I915_XD_OT_WRITE, I915_XD_TRANS_CLK_SEL(out->trans), 0U, 0U, 0U, at);
		if (platform->display_ver < 12U && out->trans != I915_XD_TRANS_EDP)
			i915_xd_ot_order_step(platform, kind, "light: the transcoder's clock after the training", I915_XD_OT_WRITE, I915_XD_TRANS_CLK_SEL(out->trans), clock_mask, clock_select, &at);
	}
	i915_xd_ot_order_step(platform, kind, "light: the timing", I915_XD_OT_WRITE, 0x60000U + 0x1000U * out->trans, 0U, 0U, &at);
	i915_xd_ot_order_step(platform, kind, "light: the function enabled", I915_XD_OT_WRITE, I915_XD_TRANS_DDI_FUNC_CTL(out->trans), I915_XD_DDI_FUNC_ENABLE, I915_XD_DDI_FUNC_ENABLE, &at);
	i915_xd_ot_order_step(platform, kind, "light: the transcoder started", I915_XD_OT_WRITE, I915_XD_TRANS_CONF(out->trans), I915_XD_TRANS_CONF_ENABLE, I915_XD_TRANS_CONF_ENABLE, &at);
	if (!dp)
		i915_xd_ot_order_absent(platform, kind, "light: no buffer before the transcoder", I915_XD_OT_WRITE, I915_XD_DDI_BUF_CTL(out->ddi), I915_XD_DDI_BUF_ENABLE, I915_XD_DDI_BUF_ENABLE, 0U, at);
	if (!dp)
		i915_xd_ot_order_step(platform, kind, "light: the buffer after the transcoder", I915_XD_OT_WRITE, I915_XD_DDI_BUF_CTL(out->ddi), I915_XD_DDI_BUF_ENABLE, I915_XD_DDI_BUF_ENABLE, &at);

	/* The dark. */
	from = i915_xd_ot_log.count;
	i915_xd_ot_dark(out, &env);
	at = (int)from;
	i915_xd_ot_order_step(platform, kind, "dark: the transcoder stopped", I915_XD_OT_WRITE, I915_XD_TRANS_CONF(out->trans), I915_XD_TRANS_CONF_ENABLE, 0U, &at);
	i915_xd_ot_order_step(platform, kind, "dark: the function off", I915_XD_OT_WRITE, I915_XD_TRANS_DDI_FUNC_CTL(out->trans), I915_XD_DDI_FUNC_ENABLE, 0U, &at);
	if (dp)
		i915_xd_ot_order_step(platform, kind, "dark: the sink asleep", I915_XD_OT_DPCD_WRITE, I915_XD_DPCD_SET_POWER, 0xffU, I915_XD_DPCD_POWER_D3, &at);
	if (platform->display_ver < 12U && out->trans != I915_XD_TRANS_EDP)
		i915_xd_ot_order_step(platform, kind, "dark: the transcoder's clock before the buffer", I915_XD_OT_WRITE, I915_XD_TRANS_CLK_SEL(out->trans), 0xffffffffU, 0U, &at);
	i915_xd_ot_order_step(platform, kind, "dark: the buffer stopped", I915_XD_OT_WRITE, I915_XD_DDI_BUF_CTL(out->ddi), I915_XD_DDI_BUF_ENABLE, 0U, &at);
	if (platform->display_ver >= 12U)
		i915_xd_ot_order_absent(platform, kind, "dark: no transcoder clock off before the buffer", I915_XD_OT_WRITE, I915_XD_TRANS_CLK_SEL(out->trans), 0xffffffffU, 0U, from, at);
	if (platform->display_ver >= 12U)
		i915_xd_ot_order_step(platform, kind, "dark: the transcoder's clock after the buffer", I915_XD_OT_WRITE, I915_XD_TRANS_CLK_SEL(out->trans), 0xffffffffU, 0U, &at);
	if (out->kind == I915_XD_SCREEN_PANEL)
		i915_xd_ot_order_step(platform, kind, "dark: the panel's power", I915_XD_OT_PANEL, I915_XD_OUTPUT_PANEL_OFF, 0U, 0U, &at);
	if (out->tc) {
		i915_xd_ot_order_step(platform, kind, "dark: the DDI's clock deselected", I915_XD_OT_WRITE, I915_XD_DDI_CLK_SEL(out->ddi), 0xf0000000U, 0U, &at);
	} else {
		i915_xd_ot_order_step(platform, kind, "dark: the DDI's clock gated", I915_XD_OT_WRITE, I915_XD_DPCLKA_CFGCR0, 0U, 0U, &at);
	}
	if (tbt) {
		i915_xd_ot_order_step(platform, kind, "dark: the Thunderbolt PLL", I915_XD_OT_TC, I915_XD_OT_TC_PLL_OFF, 0U, 0U, &at);
	} else {
		i915_xd_ot_order_step(platform, kind, "dark: the PLL off", I915_XD_OT_WRITE, pll_enable, 0x80000000U, 0U, &at);
	}
}

/* Finds the row of a description by its name; NULL for none. */
static const struct i915_xd_platform *
i915_xd_ot_order_row(
	const char *name)
{
	const struct i915_xd_platform *platform;
	unsigned rows;
	unsigned row;

	/* The row of that name. */
	rows = drv_i915_xd_platform_count();
	for (row = 0U; row < rows; row++) {
		platform = drv_i915_xd_platform_row(row);
		if (strcmp(platform->name, name) == 0)
			return platform;
	}

	/* No such row. */
	return NULL;
}

/*
 * Checks the order of the light's and the dark's steps on display 11
 * (Ice Lake), 12 (Tiger Lake) and 13 (Alder Lake-P): the panel on DDI A
 * (on display 11 through the eDP transcoder), HDMI on the first combo
 * port after DDI A, and DisplayPort on TC1 in DisplayPort alternate and
 * in Thunderbolt mode.
 */
static void
i915_xd_ot_test_order(void)
{
	static const char *const names[] = { "Ice Lake", "Tiger Lake", "Alder Lake-P" };
	const struct i915_xd_platform *platform;
	const struct i915_xd_port_desc *port;
	struct i915_xd_output out;
	unsigned version;
	unsigned index;

	/* Each version's row. */
	for (version = 0U; version < sizeof(names) / sizeof(names[0]); version++) {
		platform = i915_xd_ot_order_row(names[version]);
		if (platform == NULL) {
			i915_host_check(0, "order: a row of the version");
			continue;
		}

		/* The panel on DDI A and DPLL 0, through the eDP transcoder on display 11. */
		if (i915_xd_ot_cover_setup(platform) == NULL)
			return;
		i915_xd_ot_panel_output(&out);
		if (platform->display_ver == 11U) {
			out.trans = I915_XD_TRANS_EDP;
			out.dmc_programs = 0U;
			out.dbuf.slices_after = 0x1U;
			out.dbuf.joined_after = 0U;
		}
		i915_xd_ot_order_run(platform, &out, I915_XD_OUTPUT_TC_DP_ALT, "eDP");

		/* HDMI on the first combo port after DDI A, on DPLL 1. */
		port = NULL;
		for (index = 0U; index < platform->nports; index++) {
			if (platform->ports[index].phy_kind != I915_XD_PHY_COMBO || platform->ports[index].ddi == 0U)
				continue;
			port = &platform->ports[index];
			break;
		}
		if (port == NULL) {
			i915_host_check(0, "order: a second combo port");
			continue;
		}
		if (i915_xd_ot_cover_setup(platform) == NULL)
			return;
		i915_xd_ot_cover_output(platform, &out);
		out.kind = I915_XD_SCREEN_HDMI;
		out.ddi = port->ddi;
		out.phy = port->phy;
		out.aux = port->aux;
		out.lanes = 4U;
		out.rate_khz = 74250U;
		out.bpp = 24U;
		out.dither = 0;
		out.pll = 1U;
		out.hdmi_level = -1;
		i915_xd_ot_order_run(platform, &out, I915_XD_OUTPUT_TC_DP_ALT, "HDMI");

		/* DisplayPort on TC1, in both Type-C modes. */
		for (index = 0U; index < 2U; index++) {
			if (i915_xd_ot_cover_setup(platform) == NULL)
				return;
			if (i915_xd_ot_cover_tc1(platform, &out) != 0) {
				i915_host_check(0, "order: TC1, its PLL and its words");
				continue;
			}
			if (index == 0U) {
				i915_xd_ot_order_run(platform, &out, I915_XD_OUTPUT_TC_DP_ALT, "DP-TC");
			} else {
				i915_xd_ot_order_run(platform, &out, I915_XD_OUTPUT_TC_TBT, "TBT");
			}
		}
	}
}
