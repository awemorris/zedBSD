/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the xd transcoders and pipes (display/xd-pipe.c) on
 * the register model (xd-fake-regs.c): the 5330's panel mode on
 * transcoder A against the registers the firmware left (the timing, the
 * M/N values, the DDI function word, the source size, the MSA word), the
 * order of a transcoder's setup, an HDMI (DVI) mode on transcoder B of
 * Tiger Lake, the start and the stop of a transcoder with the wait for it
 * to go off, the pipe's miscellaneous, chicken and colour words, the
 * function's disable, and the first plane of the 5330's panel and of a
 * second pipe.
 */

#include "host-test.h"
#include "xd-fake-regs.h"

#include "../../display/xd-dp.h"
#include "../../display/xd-io.h"
#include "../../display/xd-pipe.h"
#include "../../display/xd-platform.h"
#include "../../intel/display-regs-xd.h"

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The devices the checks describe: the 5330's Alder Lake-P and a Tiger Lake. */
#define I915_XD_PIPE_TEST_ADLP		0x46a6U
#define I915_XD_PIPE_TEST_ADLP_REVID	0x0cU
#define I915_XD_PIPE_TEST_TGL		0x9a49U
#define I915_XD_PIPE_TEST_TGL_REVID	0x01U
#define I915_XD_PIPE_TEST_ICL		0x8a52U
#define I915_XD_PIPE_TEST_ICL_REVID	0x07U

/* The window the checks write through. */
static struct i915_xd_io i915_xd_pipe_test_io;

static const struct i915_xd_platform *i915_xd_pipe_test_setup(uint16_t device, uint8_t revid);
static void i915_xd_pipe_test_panel(struct i915_xd_trans_setup *setup);
static void i915_xd_pipe_test_edp(void);
static void i915_xd_pipe_test_order(void);
static void i915_xd_pipe_test_hdmi(void);
static void i915_xd_pipe_test_start_stop(void);
static void i915_xd_pipe_test_pipe_words(void);
static void i915_xd_pipe_test_plane(void);

/*
 * Runs every check of the transcoders and pipes and reports.
 */
int
main(void)
{
	int failures;

	/* Runs each group of checks on a fresh model. */
	i915_xd_pipe_test_edp();
	i915_xd_pipe_test_order();
	i915_xd_pipe_test_hdmi();
	i915_xd_pipe_test_start_stop();
	i915_xd_pipe_test_pipe_words();
	i915_xd_pipe_test_plane();

	/* Reports the result. */
	failures = i915_host_report("host_xd_pipe_test");
	if (failures != 0)
		return 1;

	/* Succeeded: every check passed. */
	return 0;
}

/* Starts a fresh model and a window on it; returns the device's description. */
static const struct i915_xd_platform *
i915_xd_pipe_test_setup(
	uint16_t device,
	uint8_t revid)
{
	struct i915_xd_io_hooks hooks;
	struct i915_xd_choice choice;
	int error;

	/* A fresh model and a window that reaches it. */
	i915_xd_fake_reset();
	i915_xd_fake_hooks(&hooks);
	drv_i915_xd_io_init(&i915_xd_pipe_test_io, NULL, NULL);
	drv_i915_xd_io_set_hooks(&i915_xd_pipe_test_io, &hooks);

	/* The description. */
	error = drv_i915_xd_platform_choose(device, revid, 0, 0U, &choice);
	if (error != 0)
		return NULL;

	/* Succeeded: the description. */
	return choice.platform;
}

/* Describes the 5330's panel mode on transcoder A: 1920x1080 at 140.8 MHz, 18 bpp on HBR x2. */
static void
i915_xd_pipe_test_panel(
	struct i915_xd_trans_setup *setup)
{
	/* The mode's timing. */
	memset(setup, 0, sizeof(*setup));
	setup->trans = 0U;
	setup->timing.hdisplay = 1920U;
	setup->timing.hblank_start = 1920U;
	setup->timing.hblank_end = 2080U;
	setup->timing.hsync_start = 1936U;
	setup->timing.hsync_end = 1952U;
	setup->timing.htotal = 2080U;
	setup->timing.vdisplay = 1080U;
	setup->timing.vblank_start = 1080U;
	setup->timing.vblank_end = 1128U;
	setup->timing.vsync_start = 1083U;
	setup->timing.vsync_end = 1097U;
	setup->timing.vtotal = 1128U;

	/* The link's M/N values, a fixed refresh, no multiplier, one line of frame start delay. */
	setup->dp = 1;
	drv_i915_xd_dp_m_n(18U * 16U, 2U, 140800U, 270000U, 1000000U, &setup->m_n);
	setup->pixel_multiplier = 1U;
	setup->frame_start_delay = 1U;
}

/*
 * The 5330's panel on transcoder A: the words the firmware left
 * (regs-selected.txt of the display reference), but the configuration
 * word, which a modeset writes without its enable.
 */
static void
i915_xd_pipe_test_edp(void)
{
	const struct i915_xd_platform *platform;
	struct i915_xd_trans_setup setup;
	struct i915_xd_trans_func func;
	struct i915_xd_msa msa;

	/* Alder Lake-P. */
	platform = i915_xd_pipe_test_setup(I915_XD_PIPE_TEST_ADLP, I915_XD_PIPE_TEST_ADLP_REVID);
	if (platform == NULL) {
		i915_host_check(0, "pipe: the Alder Lake-P description");
		return;
	}

	/* The setup, the source size, the function and the MSA. */
	i915_xd_pipe_test_panel(&setup);
	drv_i915_xd_pipe_configure(&i915_xd_pipe_test_io, platform, &setup);
	drv_i915_xd_pipe_source(&i915_xd_pipe_test_io, 0U, 1920U, 1080U);
	memset(&func, 0, sizeof(func));
	func.ddi = 0U;
	func.mode = I915_XD_FUNC_DP_SST;
	func.bpp = 18U;
	func.hsync_high = 1;
	func.lanes = 2U;
	func.sync_master = I915_XD_NO_TRANSCODER;
	drv_i915_xd_pipe_func_enable(&i915_xd_pipe_test_io, 0U, &func);
	memset(&msa, 0, sizeof(msa));
	msa.bpp = 18U;
	drv_i915_xd_pipe_msa(&i915_xd_pipe_test_io, 0U, &msa);

	/* The timing against the firmware's. */
	i915_host_check(i915_xd_fake_get(0x60000U) == 0x081f077fU && i915_xd_fake_get(0x60004U) == 0x081f077fU &&
			i915_xd_fake_get(0x60008U) == 0x079f078fU && i915_xd_fake_get(0x6000cU) == 0x04670437U &&
			i915_xd_fake_get(0x60010U) == 0x04670000U && i915_xd_fake_get(0x60014U) == 0x0448043aU &&
			i915_xd_fake_get(0x60028U) == 0U && i915_xd_fake_get(0x6007cU) == 0U,
			"pipe: the 5330's panel timing (the blank start in the context latency, written as 1)");

	/* The M/N values, the source size, the function word. */
	i915_host_check(i915_xd_fake_get(0x60030U) == 0x7e4b17e4U && i915_xd_fake_get(0x60034U) == 0x00800000U &&
			i915_xd_fake_get(0x60040U) == 0x00042bfeU && i915_xd_fake_get(0x60044U) == 0x00080000U,
			"pipe: the 5330's panel M/N values (TU 64)");
	i915_host_check(i915_xd_fake_get(0x6001cU) == 0x077f0437U, "pipe: the source size 1920x1080");
	i915_host_check(i915_xd_fake_get(0x60400U) == 0x8a210002U && i915_xd_fake_get(0x60404U) == 0U,
			"pipe: the function word 0x8a210002 (DDI A, DP SST, 6 bpc, +HSync, x2), no port sync");
	i915_host_check(i915_xd_fake_get(0x60410U) == 0x00000001U, "pipe: the MSA of 6 bpc RGB with a synchronous clock");

	/* The configuration word: not enabled by the setup, progressive; the refresh fixed, the delays. */
	i915_host_check(i915_xd_fake_get(0x70008U) == 0U && i915_xd_fake_get(0x60420U) == 0U &&
			i915_xd_fake_get(0x6002cU) == 0U && i915_xd_fake_get(0x420c0U) == 0x80000000U,
			"pipe: the configuration off, the refresh fixed, the multiplier 1, the vblank delay set and the frame start delay 1");
}

/* The order of a transcoder's setup: the M/N values, the timing, the refresh, the delays, the configuration. */
static void
i915_xd_pipe_test_order(void)
{
	static const uint32_t expected[] = {
		0x60030U, 0x60034U, 0x60040U, 0x60044U,
		0x6007cU, 0x60028U, 0x60000U, 0x60004U, 0x60008U, 0x6000cU, 0x60010U, 0x60014U,
		0x420c0U, 0x60420U, 0x6002cU, 0x420c0U, 0x70008U
	};
	const struct i915_xd_platform *platform;
	struct i915_xd_trans_setup setup;
	struct i915_xd_io_record records[32];
	unsigned count;
	unsigned writes;
	unsigned index;
	int same;

	/* Alder Lake-P, the panel's setup. */
	platform = i915_xd_pipe_test_setup(I915_XD_PIPE_TEST_ADLP, I915_XD_PIPE_TEST_ADLP_REVID);
	if (platform == NULL) {
		i915_host_check(0, "pipe: the Alder Lake-P description");
		return;
	}

	/* The 5330's panel, configured. */
	i915_xd_pipe_test_panel(&setup);
	drv_i915_xd_pipe_configure(&i915_xd_pipe_test_io, platform, &setup);

	/* The writes in order, after the step. */
	count = drv_i915_xd_io_records(&i915_xd_pipe_test_io, records, 32U);
	same = count > 0U && records[0].kind == I915_XD_IO_RECORD_STEP;
	writes = 0U;
	for (index = 1U; same && index < count; index++) {
		if (records[index].kind != I915_XD_IO_RECORD_WRITE)
			continue;
		if (writes >= sizeof(expected) / sizeof(expected[0]) || records[index].offset != expected[writes])
			same = 0;
		writes++;
	}

	/* Checks the setup writes M/N, the timing, the refresh, the delays, then the configuration. */
	i915_host_check(same && writes == sizeof(expected) / sizeof(expected[0]),
			"pipe: the setup writes M/N, the timing, the refresh, the delays, then the configuration");

	/* A variable refresh on display 12 and 13: the range, the control word with the guardband, the flip line. */
	setup.vrr.vmin = 1128U;
	setup.vrr.vmax = 2256U;
	setup.vrr.flipline = 1129U;
	setup.vrr.guardband = 40U;
	drv_i915_xd_pipe_configure(&i915_xd_pipe_test_io, platform, &setup);
	i915_host_check(i915_xd_fake_get(0x60434U) == 1127U && i915_xd_fake_get(0x60424U) == 2255U &&
			i915_xd_fake_get(0x60420U) == (0x60000000U | 40U) && i915_xd_fake_get(0x60438U) == 1128U,
			"pipe: a variable refresh on display 13 (the guardband)");

	/* A transcoder that runs keeps its enable. */
	setup.running = 1;
	drv_i915_xd_pipe_configure(&i915_xd_pipe_test_io, platform, &setup);
	i915_host_check(i915_xd_fake_get(0x70008U) == 0x80000000U, "pipe: a fastset keeps the configuration enabled");
}

/*
 * An HDMI mode without an HDMI sink (DVI) on transcoder B of Tiger Lake:
 * 1920x1080 at 148.5 MHz, interlace off; the vertical blank start is a
 * timing on display 12, and the function word is DVI on DDI B.
 */
static void
i915_xd_pipe_test_hdmi(void)
{
	const struct i915_xd_platform *platform;
	struct i915_xd_trans_setup setup;
	struct i915_xd_trans_func func;

	/* Tiger Lake. */
	platform = i915_xd_pipe_test_setup(I915_XD_PIPE_TEST_TGL, I915_XD_PIPE_TEST_TGL_REVID);
	if (platform == NULL) {
		i915_host_check(0, "pipe: the Tiger Lake description");
		return;
	}

	/* The CEA 1080p60 mode, no M/N. */
	memset(&setup, 0, sizeof(setup));
	setup.trans = 1U;
	setup.timing.hdisplay = 1920U;
	setup.timing.hblank_start = 1920U;
	setup.timing.hblank_end = 2200U;
	setup.timing.hsync_start = 2008U;
	setup.timing.hsync_end = 2052U;
	setup.timing.htotal = 2200U;
	setup.timing.vdisplay = 1080U;
	setup.timing.vblank_start = 1080U;
	setup.timing.vblank_end = 1125U;
	setup.timing.vsync_start = 1084U;
	setup.timing.vsync_end = 1089U;
	setup.timing.vtotal = 1125U;
	setup.pixel_multiplier = 1U;
	setup.frame_start_delay = 1U;
	drv_i915_xd_pipe_configure(&i915_xd_pipe_test_io, platform, &setup);

	/* Display 12 writes the blank start as a timing and no context latency; no M/N. */
	i915_host_check(i915_xd_fake_get(0x61000U) == 0x0897077fU && i915_xd_fake_get(0x61010U) == 0x04640437U &&
			i915_xd_fake_get(0x61008U) == 0x080307d7U && i915_xd_fake_get(0x6107cU) == 0U &&
			i915_xd_fake_get(0x61030U) == 0U,
			"pipe: the HDMI mode on transcoder B of display 12 (the blank start a timing, no M/N)");

	/* DVI on DDI B at 8 bpc with both syncs high. */
	memset(&func, 0, sizeof(func));
	func.ddi = 1U;
	func.mode = I915_XD_FUNC_DVI;
	func.bpp = 24U;
	func.vsync_high = 1;
	func.hsync_high = 1;
	func.sync_master = I915_XD_NO_TRANSCODER;
	drv_i915_xd_pipe_func_enable(&i915_xd_pipe_test_io, 1U, &func);
	i915_host_check(i915_xd_fake_get(0x61400U) == 0x91030000U, "pipe: the DVI function word on DDI B (0x91030000)");

	/* An interlaced mode: the vertical total and blank end lose a line, the sync shifts by half a line. */
	setup.timing.interlaced = 1;
	drv_i915_xd_pipe_configure(&i915_xd_pipe_test_io, platform, &setup);
	i915_host_check(i915_xd_fake_get(0x61028U) == 2008U - 1100U && i915_xd_fake_get(0x6100cU) == 0x04630437U &&
			i915_xd_fake_get(0x71008U) == 0x00600000U,
			"pipe: an interlaced mode's sync shift, its shorter total and its configuration");
}

/*
 * The start and the stop of a transcoder: the arbiter slots on display
 * 13, a running transcoder left alone, the stop's FEC stall release and
 * the wait for the transcoder to go off, and a transcoder that does not.
 */
static void
i915_xd_pipe_test_start_stop(void)
{
	const struct i915_xd_platform *platform;
	int ran;
	int was_running;
	unsigned late;
	uint64_t before;

	/* Alder Lake-P: the start sets the arbiter slots and the enable. */
	platform = i915_xd_pipe_test_setup(I915_XD_PIPE_TEST_ADLP, I915_XD_PIPE_TEST_ADLP_REVID);
	if (platform == NULL) {
		i915_host_check(0, "pipe: the Alder Lake-P description");
		return;
	}

	/* Starts pipe A. */
	ran = drv_i915_xd_pipe_start(&i915_xd_pipe_test_io, platform, 0U);
	i915_host_check(ran == 0 && i915_xd_fake_get(0x70028U) == 0x2000U && i915_xd_fake_get(0x70008U) == 0x80000000U,
			"pipe: the start takes the arbiter slots and enables the transcoder");

	/* A running transcoder is left as it is. */
	ran = drv_i915_xd_pipe_start(&i915_xd_pipe_test_io, platform, 0U);
	i915_host_check(ran == 1, "pipe: a running transcoder is not started again");

	/* The stop: the enable cleared, the FEC stall released, the state followed at once. */
	i915_xd_fake_rule_mirror(0x70008U, 0x80000000U, 0x70008U, -1, 0U);
	i915_xd_fake_set(0x420c0U, 0x00800000U);
	late = drv_i915_xd_pipe_stop(&i915_xd_pipe_test_io, platform, 0U, &was_running);
	i915_host_check(late == 0U && was_running == 1 && i915_xd_fake_get(0x70008U) == 0U && i915_xd_fake_get(0x420c0U) == 0U,
			"pipe: the stop clears the enable and the FEC stall, the transcoder goes off");

	/* A stopped transcoder is left as it is. */
	late = drv_i915_xd_pipe_stop(&i915_xd_pipe_test_io, platform, 0U, &was_running);
	i915_host_check(late == 0U && was_running == 0, "pipe: a stopped transcoder is not stopped again");

	/* A transcoder that stays on: the wait runs out after 100 ms. */
	platform = i915_xd_pipe_test_setup(I915_XD_PIPE_TEST_ADLP, I915_XD_PIPE_TEST_ADLP_REVID);
	i915_xd_fake_set(0x70008U, 0xc0000000U);
	before = i915_xd_fake_now();
	late = drv_i915_xd_pipe_stop(&i915_xd_pipe_test_io, platform, 0U, &was_running);
	i915_host_check(late == I915_XD_PIPE_LATE_OFF && i915_xd_fake_now() - before >= 100000U,
			"pipe: a transcoder that stays on is reported after 100 ms");

	/* Display 11 has no FEC stall bit: the stop leaves the chicken register alone. */
	platform = i915_xd_pipe_test_setup(I915_XD_PIPE_TEST_ICL, I915_XD_PIPE_TEST_ICL_REVID);
	i915_xd_fake_rule_mirror(0x70008U, 0x80000000U, 0x70008U, -1, 0U);
	i915_xd_fake_set(0x70008U, 0xc0000000U);
	i915_xd_fake_set(0x420c0U, 0x00800000U);
	late = drv_i915_xd_pipe_stop(&i915_xd_pipe_test_io, platform, 0U, &was_running);
	i915_host_check(late == 0U && was_running == 1 && i915_xd_fake_get(0x420c0U) == 0x00800000U,
			"pipe: the display 11 stop leaves the chicken register as it was");

	/* The frame counter. */
	i915_xd_fake_set(0x71040U, 1234U);
	i915_host_check(drv_i915_xd_pipe_frame(&i915_xd_pipe_test_io, 1U) == 1234U, "pipe: pipe B's frame counter");
}

/* The pipe's own words: miscellaneous, chicken, colours; the function's disable. */
static void
i915_xd_pipe_test_pipe_words(void)
{
	const struct i915_xd_platform *platform;
	struct i915_xd_pipe_misc misc;
	uint32_t read;

	/* Alder Lake-P. */
	platform = i915_xd_pipe_test_setup(I915_XD_PIPE_TEST_ADLP, I915_XD_PIPE_TEST_ADLP_REVID);
	if (platform == NULL) {
		i915_host_check(0, "pipe: the Alder Lake-P description");
		return;
	}

	/* 18 bpp with dither: 6 bpc, dither, rounding. */
	memset(&misc, 0, sizeof(misc));
	misc.bpp = 18U;
	misc.dither = 1;
	drv_i915_xd_pipe_misc(&i915_xd_pipe_test_io, platform, 0U, &misc);
	i915_host_check(i915_xd_fake_get(0x70030U) == 0x00000150U, "pipe: the miscellaneous word of 18 bpp with dither (0x150)");

	/* 36 bpp YCbCr 4:2:0 on display 13. */
	misc.bpp = 36U;
	misc.dither = 0;
	misc.output = I915_XD_OUTPUT_YCBCR420;
	drv_i915_xd_pipe_misc(&i915_xd_pipe_test_io, platform, 0U, &misc);
	i915_host_check(i915_xd_fake_get(0x70030U) == (0x0c000000U | 0x800U | 0x80U | 0x100U),
			"pipe: 12 bpc 4:2:0 in full blend on display 13");

	/* The chicken word keeps what it had and adds the pass-through and no underrun recovery. */
	i915_xd_fake_set(0x70038U, 0x00000001U);
	drv_i915_xd_pipe_chicken(&i915_xd_pipe_test_io, platform, 0U);
	i915_host_check(i915_xd_fake_get(0x70038U) == (0x40000000U | 0x8000U | 0x80U | 0x1U), "pipe: the chicken word");

	/* The colour words. */
	i915_xd_fake_set(0x70034U, 0x12345678U);
	drv_i915_xd_pipe_colors(&i915_xd_pipe_test_io, 0U, 0U, 0U);
	i915_host_check(i915_xd_fake_get(0x70034U) == 0U && i915_xd_fake_get(0x4a480U) == 0U && i915_xd_fake_get(0x49028U) == 0U,
			"pipe: a black bottom colour, 8-bit gamma, no CSC");

	/* The function's disable keeps nothing but what is not the DDI, the mode or the enable. */
	i915_xd_fake_set(0x60400U, 0x8a210002U);
	i915_xd_fake_set(0x60404U, 0x00000011U);
	read = drv_i915_xd_pipe_func_disable(&i915_xd_pipe_test_io, 0U, 0);
	i915_host_check(read == 0x8a210002U && i915_xd_fake_get(0x60400U) == 0x00210002U && i915_xd_fake_get(0x60404U) == 0U,
			"pipe: the function's disable clears the enable, the DDI and the mode, and the port sync");
}

/*
 * The first plane: the buffers it refuses, its control words on display
 * 13 and 12, the 5330's panel buffer (the words its firmware left: the
 * control 0x94000000, stride 0x78, size 0x0437077f, colour control
 * 0x2000) in the order an update writes them, the plane on pipe B, the
 * plane off and the live surface.
 */
static void
i915_xd_pipe_test_plane(void)
{
	static const uint32_t expected[] = {
		0x70188U, 0x7018cU, 0x70190U, 0x70194U, 0x70198U, 0x701a0U, 0x701a4U, 0x701c0U, 0x701c8U, 0x701ccU,
		0x70180U, 0x7019cU
	};
	const struct i915_xd_platform *platform;
	const struct i915_xd_platform *tgl;
	struct i915_xd_plane plane;
	struct i915_xd_io_record records[32];
	unsigned count;
	unsigned writes;
	unsigned index;
	int same;
	int error;

	/* The buffers the plane refuses. */
	error = drv_i915_xd_plane_check(I915_XD_FOURCC_XRGB8888, 0U, 1920U, 1080U, 7680U, 0x180000U);
	i915_host_check(error == 0, "plane: the 5330's panel buffer is taken");
	error = drv_i915_xd_plane_check(0x34324241U, 0U, 1920U, 1080U, 7680U, 0x180000U);
	i915_host_check(error == EINVAL, "plane: another format is refused");
	error = drv_i915_xd_plane_check(I915_XD_FOURCC_XRGB8888, 2U, 1920U, 1080U, 7680U, 0x180000U);
	i915_host_check(error == EINVAL, "plane: a tiled buffer is refused");
	error = drv_i915_xd_plane_check(I915_XD_FOURCC_XRGB8888, 0U, 8193U, 1080U, 40000U, 0x180000U);
	i915_host_check(error == EINVAL, "plane: a side above 8192 is refused");
	error = drv_i915_xd_plane_check(I915_XD_FOURCC_XRGB8888, 0U, 1920U, 1080U, 7000U, 0x180000U);
	i915_host_check(error == EINVAL, "plane: a pitch shorter than a line is refused");
	error = drv_i915_xd_plane_check(I915_XD_FOURCC_XRGB8888, 0U, 1920U, 1080U, 7700U, 0x180000U);
	i915_host_check(error == EINVAL, "plane: a pitch not of 64 bytes is refused");
	error = drv_i915_xd_plane_check(I915_XD_FOURCC_XRGB8888, 0U, 1920U, 1080U, 7680U, 0x180800U);
	i915_host_check(error == EINVAL, "plane: a surface off a page is refused");

	/* Tiger Lake: no arbiter slots. */
	tgl = i915_xd_pipe_test_setup(I915_XD_PIPE_TEST_TGL, I915_XD_PIPE_TEST_TGL_REVID);
	memset(&plane, 0, sizeof(plane));
	if (tgl != NULL)
		drv_i915_xd_plane_words(tgl, &plane);
	i915_host_check(tgl != NULL && plane.ctl == 0x84000000U && plane.color_ctl == 0x2000U, "plane: the control word of display 12");

	/* Alder Lake-P: the panel's buffer at 0x180000. */
	platform = i915_xd_pipe_test_setup(I915_XD_PIPE_TEST_ADLP, I915_XD_PIPE_TEST_ADLP_REVID);
	if (platform == NULL) {
		i915_host_check(0, "plane: the Alder Lake-P description");
		return;
	}

	/* Plane 1 of pipe A with a 1920x1080 buffer. */
	memset(&plane, 0, sizeof(plane));
	plane.pipe = 0U;
	plane.width = 1920U;
	plane.height = 1080U;
	plane.pitch = 7680U;
	plane.surf = 0x180000U;
	plane.alpha = 0xffU;
	drv_i915_xd_plane_words(platform, &plane);
	drv_i915_xd_plane_noarm(&i915_xd_pipe_test_io, &plane);
	drv_i915_xd_plane_arm(&i915_xd_pipe_test_io, &plane);
	i915_host_check(i915_xd_fake_get(0x70180U) == 0x94000000U && i915_xd_fake_get(0x70188U) == 0x78U &&
			i915_xd_fake_get(0x70190U) == 0x0437077fU && i915_xd_fake_get(0x7019cU) == 0x180000U &&
			i915_xd_fake_get(0x701ccU) == 0x2000U && i915_xd_fake_get(0x7018cU) == 0U &&
			i915_xd_fake_get(0x701a0U) == 0xff000000U && i915_xd_fake_get(0x70198U) == 0U,
			"plane: the 5330's panel plane (control 0x94000000, stride 0x78, size 0x0437077f)");

	/* The order: the unarmed words, then the control word and the arming surface. */
	count = drv_i915_xd_io_records(&i915_xd_pipe_test_io, records, 32U);
	same = 1;
	writes = 0U;
	for (index = 0U; index < count; index++) {
		if (records[index].kind != I915_XD_IO_RECORD_WRITE)
			continue;
		if (writes >= sizeof(expected) / sizeof(expected[0]) || records[index].offset != expected[writes])
			same = 0;
		writes++;
	}

	/* Checks the unarmed words, then the control word and the surface. */
	i915_host_check(same && writes == sizeof(expected) / sizeof(expected[0]), "plane: the unarmed words, then the control word and the surface");

	/* A translucent plane on pipe B at an offset. */
	plane.pipe = 1U;
	plane.x = 100U;
	plane.y = 50U;
	plane.alpha = 0x80U;
	drv_i915_xd_plane_noarm(&i915_xd_pipe_test_io, &plane);
	i915_host_check(i915_xd_fake_get(0x7118cU) == 0x00320064U && i915_xd_fake_get(0x71198U) == 0x80000000U &&
			i915_xd_fake_get(0x711a0U) == 0x80000000U,
			"plane: pipe B's plane at (100, 50) with the constant alpha 0x80");

	/* Off, and the live surface. */
	drv_i915_xd_plane_off_noarm(&i915_xd_pipe_test_io, 0U);
	drv_i915_xd_plane_off_arm(&i915_xd_pipe_test_io, 0U);
	i915_xd_fake_set(0x701acU, 0x180000U);
	i915_host_check(i915_xd_fake_get(0x70180U) == 0U && i915_xd_fake_get(0x7019cU) == 0U &&
			drv_i915_xd_plane_live(&i915_xd_pipe_test_io, 0U) == 0x180000U,
			"plane: the plane off, and the live surface");
}
