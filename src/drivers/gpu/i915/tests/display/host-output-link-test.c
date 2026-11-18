/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of an output's planned link (display/output.c's
 * drv_i915_output_link_dp() and drv_i915_output_link_hdmi()) and of the
 * firmware's output:
 *
 *   - the 5330's USB-C monitor (RBR, HBR and HBR2 shared, 4 lanes,
 *     1920x1280 at 164.36 MHz) takes RBR x4 at 24 bpp, the link Linux
 *     trained on the machine, with its M/N and no combo PLL words (the
 *     Type-C port's PLL is the picture's own);
 *   - the fallback's limits move the link (2 lanes: HBR x2), and limits no
 *     link fits under refuse with ENOSPC;
 *   - a branch's downstream bits per colour lower the depth, its dot clock
 *     limit refuses the mode, and a sink without a shared rate is invalid;
 *   - an HDMI mode runs four lanes at its pixel clock on combo PLL words,
 *     and a mode without a clock or a device without a description is
 *     refused.
 *   - the firmware's output by the platform's ports
 *     (drv_i915_gop_output_read()): the panel on DDI A, HDMI on a combo
 *     port, external DisplayPort on another port (Tiger Lake's TC5,
 *     Rocket Lake's combo TC1), and what is not lit.
 */

#include "host-test.h"

#include "../../display/output.h"
#include "../../display/xd-platform.h"

#include <stdio.h>
#include <string.h>

#include <uapi/errno.h>

/* The 5330's Alder Lake-P and the reference clock of its port PLLs (kHz). */
#define OUTPUT_LINK_TEST_ADLP		0x46a6U
#define OUTPUT_LINK_TEST_ADLP_REVID	0x0cU
#define OUTPUT_LINK_TEST_REF_KHZ	38400U

static const struct i915_xd_platform *output_link_test_platform(void);
static void output_link_test_sink(struct i915_xd_dp_probe *sink);
static void output_link_test_monitor_mode(struct i915_display_mode *mode);
static void output_link_test_monitor(const struct i915_xd_platform *platform);
static void output_link_test_limits(const struct i915_xd_platform *platform);
static void output_link_test_branch(const struct i915_xd_platform *platform);
static void output_link_test_hdmi(const struct i915_xd_platform *platform);
static unsigned output_link_test_gop(const struct i915_xd_platform *platform, unsigned pipe, unsigned ddi, unsigned mode);
static void output_link_test_gops(const struct i915_xd_platform *platform);

/*
 * Runs the checks and reports the tally.
 */
int
main(
	int argc,
	char **argv)
{
	const struct i915_xd_platform *platform;
	int verbose_asked;
	int report;

	/* -v prints every check. */
	verbose_asked = 1;
	if (argc > 1)
		verbose_asked = strcmp(argv[1], "-v");
	if (verbose_asked == 0)
		i915_host_verbose = 1;

	/* The 5330's description. */
	platform = output_link_test_platform();
	i915_host_check(platform != NULL, "the 5330's Alder Lake-P is described");
	if (platform == NULL) {
		report = i915_host_report("host-output-link-test");
		return report;
	}

	/* Runs each part. */
	output_link_test_monitor(platform);
	output_link_test_limits(platform);
	output_link_test_branch(platform);
	output_link_test_hdmi(platform);
	output_link_test_gops(platform);

	/* Reports the tally. */
	report = i915_host_report("host-output-link-test");
	return report;
}

/* Returns the description of the 5330's Alder Lake-P, or NULL. */
static const struct i915_xd_platform *
output_link_test_platform(void)
{
	struct i915_xd_choice choice;
	int error;

	/* The description by the product and its stepping. */
	error = drv_i915_xd_platform_choose(OUTPUT_LINK_TEST_ADLP, OUTPUT_LINK_TEST_ADLP_REVID, 0, 0U, &choice);
	if (error != 0)
		return NULL;

	/* Succeeded: the description. */
	return choice.platform;
}

/* The 5330's USB-C monitor as the probe found it: RBR, HBR, HBR2 shared, 4 lanes, not a branch. */
static void
output_link_test_sink(
	struct i915_xd_dp_probe *sink)
{
	/* A connected DPCD 1.4 sink. */
	memset(sink, 0, sizeof(*sink));
	sink->status = I915_XD_DP_PROBE_CONNECTED;
	sink->dpcd[0] = 0x14;
	sink->dpcd_valid = 1;

	/* The shared rates and lanes. */
	sink->common_rates[0] = 162000;
	sink->common_rates[1] = 270000;
	sink->common_rates[2] = 540000;
	sink->num_common_rates = 3;
	sink->max_rate = 540000;
	sink->max_sink_lanes = 4;
	sink->max_lanes = 4;
}

/* The monitor's preferred mode: 1920x1280 at 164.36 MHz. */
static void
output_link_test_monitor_mode(
	struct i915_display_mode *mode)
{
	/* The timing of the EDID's first descriptor. */
	memset(mode, 0, sizeof(*mode));
	mode->clock_khz = 164360;
	mode->hdisplay = 1920;
	mode->hsync_start = 1968;
	mode->hsync_end = 2000;
	mode->htotal = 2080;
	mode->vdisplay = 1280;
	mode->vsync_start = 1283;
	mode->vsync_end = 1293;
	mode->vtotal = 1317;
	mode->edid_bpc = 8;
}

/* The monitor's link is the one Linux trained. */
static void
output_link_test_monitor(
	const struct i915_xd_platform *platform)
{
	struct i915_xd_dp_probe sink;
	struct i915_display_mode mode;
	struct i915_display_link link;
	int error;

	/* Plans the link without limits. */
	output_link_test_sink(&sink);
	output_link_test_monitor_mode(&mode);
	error = drv_i915_output_link_dp(platform, 3U, &mode, &sink, 0, 0, OUTPUT_LINK_TEST_REF_KHZ, &link);
	i915_host_check(error == 0, "monitor: the link is planned");
	i915_host_check(link.rate_khz == 162000U && link.lanes == 4U, "monitor: RBR x4 (the link Linux trained)");
	i915_host_check(link.bpp == 24U, "monitor: 24 bpp");
	i915_host_check(link.data_n != 0U && link.link_n != 0U && link.tu == 64U, "monitor: M/N computed");
	i915_host_check(link.cfgcr0 == 0U && link.cfgcr1 == 0U, "monitor: no combo PLL words on a Type-C port");
	i915_host_check(link.ref_khz == OUTPUT_LINK_TEST_REF_KHZ, "monitor: the reference clock is kept");
}

/* The fallback's limits move the link, and limits no link fits under refuse. */
static void
output_link_test_limits(
	const struct i915_xd_platform *platform)
{
	struct i915_xd_dp_probe sink;
	struct i915_display_mode mode;
	struct i915_display_link link;
	int error;

	/* Two lanes: HBR x2 is the first that carries the mode. */
	output_link_test_sink(&sink);
	output_link_test_monitor_mode(&mode);
	error = drv_i915_output_link_dp(platform, 3U, &mode, &sink, 540000, 2, OUTPUT_LINK_TEST_REF_KHZ, &link);
	i915_host_check(error == 0 && link.rate_khz == 270000U && link.lanes == 2U, "limits: two lanes give HBR x2");

	/* RBR at two lanes carries no depth of the mode, not even 18 bpp. */
	error = drv_i915_output_link_dp(platform, 3U, &mode, &sink, 162000, 2, OUTPUT_LINK_TEST_REF_KHZ, &link);
	i915_host_check(error == ENOSPC, "limits: RBR x2 carries no depth of the mode (ENOSPC)");

	/* RBR at four lanes is the link when HBR is excluded. */
	error = drv_i915_output_link_dp(platform, 3U, &mode, &sink, 162000, 4, OUTPUT_LINK_TEST_REF_KHZ, &link);
	i915_host_check(error == 0 && link.rate_khz == 162000U && link.lanes == 4U, "limits: RBR x4 under the RBR limit");

	/* A sink that shares no rate is not a sink to drive. */
	sink.num_common_rates = 0;
	error = drv_i915_output_link_dp(platform, 3U, &mode, &sink, 0, 0, OUTPUT_LINK_TEST_REF_KHZ, &link);
	i915_host_check(error == EINVAL, "limits: no shared rate is EINVAL");
}

/* A branch's downstream limits lower the depth or refuse the mode. */
static void
output_link_test_branch(
	const struct i915_xd_platform *platform)
{
	struct i915_xd_dp_probe sink;
	struct i915_display_mode mode;
	struct i915_display_link link;
	int error;

	/* A branch that passes 6 bits per colour: 18 bpp. */
	output_link_test_sink(&sink);
	output_link_test_monitor_mode(&mode);
	sink.branch = 1;
	sink.max_bpc = 6;
	error = drv_i915_output_link_dp(platform, 3U, &mode, &sink, 0, 0, OUTPUT_LINK_TEST_REF_KHZ, &link);
	i915_host_check(error == 0 && link.bpp == 18U, "branch: 6 bpc downstream gives 18 bpp");

	/* A deep-colour EDID is still driven at 8 bpc. */
	sink.max_bpc = 0;
	mode.edid_bpc = 10;
	error = drv_i915_output_link_dp(platform, 3U, &mode, &sink, 0, 0, OUTPUT_LINK_TEST_REF_KHZ, &link);
	i915_host_check(error == 0 && link.bpp == 24U, "branch: a 10 bpc display is driven at 24 bpp");

	/* A dot clock limit below the mode refuses it. */
	sink.max_dotclock_khz = 148500;
	error = drv_i915_output_link_dp(platform, 3U, &mode, &sink, 0, 0, OUTPUT_LINK_TEST_REF_KHZ, &link);
	i915_host_check(error == ENOSPC, "branch: a mode above the downstream dot clock is ENOSPC");
}

/* An HDMI mode runs four lanes at its pixel clock on the combo PLL's words. */
static void
output_link_test_hdmi(
	const struct i915_xd_platform *platform)
{
	struct i915_display_mode mode;
	struct i915_display_link link;
	int error;

	/* CEA format 4: 1280x720 at 74.25 MHz. */
	memset(&mode, 0, sizeof(mode));
	mode.clock_khz = 74250;
	mode.hdisplay = 1280;
	mode.hsync_start = 1390;
	mode.hsync_end = 1430;
	mode.htotal = 1650;
	mode.vdisplay = 720;
	mode.vsync_start = 725;
	mode.vsync_end = 730;
	mode.vtotal = 750;
	mode.hsync_positive = 1;
	mode.vsync_positive = 1;
	mode.edid_bpc = 8;

	/* The link and the PLL of the mode. */
	error = drv_i915_output_link_hdmi(platform, 1U, &mode, OUTPUT_LINK_TEST_REF_KHZ, &link);
	i915_host_check(error == 0, "hdmi: the link is planned");
	i915_host_check(link.lanes == 4U && link.bpp == 24U, "hdmi: four lanes at 24 bpp");
	i915_host_check(link.rate_khz == 74250U, "hdmi: the link runs at the pixel clock");
	i915_host_check(link.cfgcr0 != 0U, "hdmi: the combo PLL's words are planned");
	i915_host_check(link.ref_khz == OUTPUT_LINK_TEST_REF_KHZ, "hdmi: the reference clock is kept");

	/* A mode without a clock, and a device without a description, are refused. */
	mode.clock_khz = 0;
	error = drv_i915_output_link_hdmi(platform, 1U, &mode, OUTPUT_LINK_TEST_REF_KHZ, &link);
	i915_host_check(error == EINVAL, "hdmi: a mode without a clock is EINVAL");
	mode.clock_khz = 74250;
	error = drv_i915_output_link_hdmi(NULL, 1U, &mode, OUTPUT_LINK_TEST_REF_KHZ, &link);
	i915_host_check(error == EINVAL, "hdmi: a device without a description is EINVAL");
}

/*
 * Classifies the firmware's output of a record with one lit pipe whose
 * transcoder drives a DDI in a mode (TRANS_DDI_FUNC_CTL: enable, the
 * port select DDI + 1, the mode select), and reports its kind.
 */
static unsigned
output_link_test_gop(
	const struct i915_xd_platform *platform,
	unsigned pipe,
	unsigned ddi,
	unsigned mode)
{
	struct i915_xd_firmware firmware;
	struct i915_gop_output gop;

	/* The record: one lit pipe. */
	memset(&firmware, 0, sizeof(firmware));
	firmware.pipe[pipe].state = I915_XD_FW_PIPE_LIT;
	firmware.pipe[pipe].ddi_function = 0x80000000U | ((ddi + 1U) << 27) | (mode << 24);

	/* Succeeded: the kind it reads as. */
	drv_i915_gop_output_read(platform, &firmware, &gop);
	return gop.kind;
}

/*
 * Checks the firmware's output by the platform's ports: the panel on DDI
 * A, HDMI on the combo ports, external DisplayPort on the other ports
 * (Tiger Lake's TC5 and TC6, Rocket Lake's combo TC1), and what is not
 * lit (a port the platform does not drive, HDMI on a Type-C port, DP MST).
 */
static void
output_link_test_gops(
	const struct i915_xd_platform *platform)
{
	struct i915_xd_firmware firmware;
	struct i915_xd_choice choice;
	struct i915_gop_output gop;
	unsigned kind;
	int error;

	/* Alder Lake-P: DP SST on A is the panel, HDMI or DVI on A or B the HDMI path, DP SST on TC1 external. */
	kind = output_link_test_gop(platform, 0U, 0U, 2U);
	i915_host_check(kind == I915_GOP_EDP, "gop: DP SST on DDI A is the panel");
	kind = output_link_test_gop(platform, 1U, 1U, 1U);
	i915_host_check(kind == I915_GOP_HDMI, "gop: DVI on DDI B is the HDMI path");
	kind = output_link_test_gop(platform, 1U, 0U, 0U);
	i915_host_check(kind == I915_GOP_HDMI, "gop: HDMI on DDI A, a combo port, is the HDMI path");
	kind = output_link_test_gop(platform, 1U, 3U, 2U);
	i915_host_check(kind == I915_GOP_DP_TC, "gop: DP SST on TC1 is the external DisplayPort path");

	/* What is not lit: DDI C (Alder Lake-P has none), HDMI on TC1, DP MST on TC2. */
	kind = output_link_test_gop(platform, 1U, 2U, 2U);
	i915_host_check(kind == I915_GOP_OTHER, "gop: a port the platform does not drive is not lit");
	kind = output_link_test_gop(platform, 1U, 3U, 0U);
	i915_host_check(kind == I915_GOP_OTHER, "gop: HDMI on a Type-C port is not lit");
	kind = output_link_test_gop(platform, 1U, 4U, 3U);
	i915_host_check(kind == I915_GOP_OTHER, "gop: DP MST is not lit");

	/* Tiger Lake's TC5 (DDI 7) in DP SST mode is external DisplayPort. */
	error = drv_i915_xd_platform_choose(0x9a49U, 0x0U, 0, 0U, &choice);
	kind = I915_GOP_NONE;
	if (error == 0)
		kind = output_link_test_gop(choice.platform, 1U, 7U, 2U);
	i915_host_check(kind == I915_GOP_DP_TC, "gop: Tiger Lake's TC5 in DP SST mode is external DisplayPort");

	/* Rocket Lake's TC1 (a combo port, DDI 3) in DP SST mode is external DisplayPort, in HDMI mode HDMI. */
	error = drv_i915_xd_platform_choose(0x4c8aU, 0x0U, 0, 0U, &choice);
	kind = I915_GOP_NONE;
	if (error == 0)
		kind = output_link_test_gop(choice.platform, 1U, 3U, 2U);
	i915_host_check(kind == I915_GOP_DP_TC, "gop: Rocket Lake's TC1 in DP SST mode is external DisplayPort");
	if (error == 0)
		kind = output_link_test_gop(choice.platform, 1U, 3U, 0U);
	i915_host_check(kind == I915_GOP_HDMI, "gop: Rocket Lake's combo TC1 in HDMI mode is the HDMI path");

	/* A lit pipe whose transcoder drives no port is not an output. */
	memset(&firmware, 0, sizeof(firmware));
	firmware.pipe[0].state = I915_XD_FW_PIPE_LIT;
	drv_i915_gop_output_read(platform, &firmware, &gop);
	i915_host_check(gop.kind == I915_GOP_NONE && gop.pipes == 1U, "gop: a lit pipe that drives no port is no output");
}
