/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the platform descriptions over every display 11 to
 * 13 row (display/xd-platform.c and the parts that read the rows): each
 * row is found by its own PCI id; on each row the links of an eDP panel
 * on DDI A, of HDMI on a combo port and of DisplayPort on the first
 * Type-C connector are planned (display/xd-screen.c), with the combo PLL,
 * Dekel TC PLL or MG PLL words that give the link back
 * (display/xd-clock.c), and a CD clock step for them; the firmware's
 * output on each row is classified (display/xd-takeover.c: the desktops'
 * Type-C connectors are combo ports); and a PCI id no row has is
 * refused.
 */

#include "host-test.h"

#include "../../display/xd-clock.h"
#include "../../display/xd-platform.h"
#include "../../display/xd-screen.h"
#include "../../display/xd-takeover.h"
#include "../../display/display-internal.h"
#include "../../intel/display-regs-xd.h"

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The reference clock every check plans on, and the eDP and DisplayPort rates offered. */
#define I915_XD_COVER_TEST_REF		38400U

static const uint32_t i915_xd_cover_test_rates[] = { 162000U, 270000U, 540000U };

static void i915_xd_cover_test_mode(struct i915_display_mode *mode);
static void i915_xd_cover_test_input(struct i915_xd_plan_input *input, const struct i915_xd_platform *platform, const struct i915_display_mode *mode);
static const struct i915_xd_port_desc *i915_xd_cover_test_combo(const struct i915_xd_platform *platform);
static const struct i915_xd_port_desc *i915_xd_cover_test_type_c(const struct i915_xd_platform *platform);
static void i915_xd_cover_test_check(int passed, const struct i915_xd_platform *platform, const char *what);
static void i915_xd_cover_test_row(const struct i915_xd_platform *platform);
static uint32_t i915_xd_cover_test_select(const struct i915_xd_platform *platform, unsigned ddi);
static void i915_xd_cover_test_classify(const struct i915_xd_platform *platform);
static void i915_xd_cover_test_unknown(void);

/*
 * Runs the checks of every row and reports.
 */
int
main(void)
{
	const struct i915_xd_platform *platform;
	unsigned index;
	int failures;
	unsigned rows;

	/* Each row. */
	rows = drv_i915_xd_platform_count();
	for (index = 0U; index < rows; index++) {
		platform = drv_i915_xd_platform_row(index);
		i915_xd_cover_test_row(platform);
		i915_xd_cover_test_classify(platform);
	}

	/* The ids no row has. */
	i915_xd_cover_test_unknown();

	/* Reports the result. */
	failures = i915_host_report("host_xd_cover_test");
	if (failures != 0)
		return 1;

	/* Succeeded: every check passed. */
	return 0;
}

/* Fills 1080p60 (148.5 MHz). */
static void
i915_xd_cover_test_mode(
	struct i915_display_mode *mode)
{
	/* The CEA timings of 1920x1080 at 60 Hz. */
	memset(mode, 0, sizeof(*mode));
	mode->clock_khz = 148500;
	mode->hdisplay = 1920U;
	mode->hsync_start = 2008U;
	mode->hsync_end = 2052U;
	mode->htotal = 2200U;
	mode->vdisplay = 1080U;
	mode->vsync_start = 1084U;
	mode->vsync_end = 1089U;
	mode->vtotal = 1125U;
	mode->hsync_positive = 1;
	mode->vsync_positive = 1;
}

/* Fills the plan input of an eDP panel on DDI A, pipe A, on a row. */
static void
i915_xd_cover_test_input(
	struct i915_xd_plan_input *input,
	const struct i915_xd_platform *platform,
	const struct i915_display_mode *mode)
{
	/* The output, its sink and the row. */
	memset(input, 0, sizeof(*input));
	input->kind = I915_XD_SCREEN_PANEL;
	input->ddi = 0U;
	input->pipe = 0U;
	input->mode = mode;
	input->sink.rate_code = 0x14U;
	input->sink.max_lanes = 4U;
	input->sink.edp_revision = 0x02U;
	input->platform = platform;
	input->ref_khz = I915_XD_COVER_TEST_REF;
	input->run_pipes = 0x1U;
	input->cdclk_max_khz = 652800U;
	input->qgv_mbps = 20000U;
	input->cpp = 4U;
}

/* The row's first combo port other than DDI A, or NULL. */
static const struct i915_xd_port_desc *
i915_xd_cover_test_combo(
	const struct i915_xd_platform *platform)
{
	unsigned index;

	/* In the row's order. */
	for (index = 0U; index < platform->nports; index++) {
		if (platform->ports[index].phy_kind == I915_XD_PHY_COMBO && platform->ports[index].ddi != 0U)
			return &platform->ports[index];
	}

	/* Succeeded: none. */
	return NULL;
}

/* The row's first Type-C connector (a Dekel or MG port, or a combo port on a Type-C pin), or NULL. */
static const struct i915_xd_port_desc *
i915_xd_cover_test_type_c(
	const struct i915_xd_platform *platform)
{
	unsigned index;

	/* In the row's order. */
	for (index = 0U; index < platform->nports; index++) {
		if (platform->ports[index].tc != 0U)
			return &platform->ports[index];
		if (platform->ports[index].south_pin_kind == I915_XD_SOUTH_PIN_TC)
			return &platform->ports[index];
	}

	/* Succeeded: none. */
	return NULL;
}

/* Records a check of a row, named after it. */
static void
i915_xd_cover_test_check(
	int passed,
	const struct i915_xd_platform *platform,
	const char *what)
{
	char name[160];

	/* The row's name before the check's. */
	(void)snprintf(name, sizeof(name), "%s: %s", platform->name, what);
	i915_host_check(passed, name);
}

/*
 * One row: found by its first PCI id; the eDP panel on DDI A, HDMI on a
 * combo port and DisplayPort on the first Type-C connector planned, with
 * PLL words that give each link back; a CD clock step for 1080p60; the
 * Type-C flow where the row has Dekel or MG ports.
 */
static void
i915_xd_cover_test_row(
	const struct i915_xd_platform *platform)
{
	const struct i915_xd_port_desc *port;
	struct i915_xd_tc_pll_words tc_words;
	struct i915_xd_mg_pll_words mg_words;
	struct i915_xd_dpll_words words;
	struct i915_xd_plan_input input;
	struct i915_display_mode mode;
	struct i915_xd_choice choice;
	struct i915_xd_plan plan;
	uint32_t port_khz;
	unsigned refused;
	int error;

	/* The row's own PCI id finds it. */
	error = drv_i915_xd_platform_choose(platform->pci_ids[0], 0x0U, 0, 0U, &choice);
	i915_xd_cover_test_check(error == 0 && choice.platform == platform, platform, "its first PCI id finds the row");

	/* eDP on DDI A at HBR2 at most: a combo PLL whose words give the rate. */
	i915_xd_cover_test_mode(&mode);
	i915_xd_cover_test_input(&input, platform, &mode);
	refused = drv_i915_xd_screen_plan_link(&input, &plan);
	port_khz = drv_i915_xd_dpll_port_khz(platform, I915_XD_COVER_TEST_REF, &plan.pll);
	i915_xd_cover_test_check(refused == I915_XD_PLAN_OK && plan.pll_set && port_khz == plan.rate_khz && plan.lanes == 4U,
		platform, "eDP on DDI A: the link and a combo PLL that gives it");

	/* HDMI 1080p60 on a combo port: the TMDS clock and the WRPLL's words. */
	port = i915_xd_cover_test_combo(platform);
	i915_xd_cover_test_check(port != NULL, platform, "a combo port besides DDI A");
	if (port != NULL) {
		input.kind = I915_XD_SCREEN_HDMI;
		input.ddi = port->ddi;
		input.pipe = 1U;
		input.run_pipes = 0x3U;
		refused = drv_i915_xd_screen_plan_link(&input, &plan);
		error = drv_i915_xd_dpll_hdmi_words(platform, I915_XD_COVER_TEST_REF, 148500U, I915_XD_DPLL_AFC_NONE, &words);
		i915_xd_cover_test_check(refused == I915_XD_PLAN_OK && error == 0 && plan.rate_khz == 148500U && plan.pll_set &&
			plan.pll.cfgcr0 == words.cfgcr0 && plan.pll.cfgcr1 == words.cfgcr1, platform, "HDMI on a combo port: the WRPLL");
	}

	/* DisplayPort on the first Type-C connector: a Dekel port's TC PLL, or a combo port's DPLL. */
	port = i915_xd_cover_test_type_c(platform);
	i915_xd_cover_test_check(port != NULL, platform, "a Type-C connector");
	if (port != NULL) {
		i915_xd_cover_test_input(&input, platform, &mode);
		input.kind = I915_XD_SCREEN_DP;
		input.ddi = port->ddi;
		input.pipe = 1U;
		input.run_pipes = 0x3U;
		input.sink.rates = i915_xd_cover_test_rates;
		input.sink.nrates = sizeof(i915_xd_cover_test_rates) / sizeof(i915_xd_cover_test_rates[0]);
		refused = drv_i915_xd_screen_plan_link(&input, &plan);
		i915_xd_cover_test_check(refused == I915_XD_PLAN_OK && plan.rate_khz == 162000U && plan.lanes == 4U, platform,
			"DisplayPort on the first Type-C connector: RBR on four lanes");
		if (port->phy_kind == I915_XD_PHY_DEKEL) {
			error = drv_i915_xd_tc_pll_words(I915_XD_COVER_TEST_REF, plan.rate_khz, 0, I915_XD_DPLL_AFC_NONE, &tc_words);
			port_khz = drv_i915_xd_tc_pll_port_khz(I915_XD_COVER_TEST_REF, &tc_words);
			i915_xd_cover_test_check(plan.pll_set == 0 && error == 0 && port_khz == plan.rate_khz && platform->tc_flow != I915_XD_TC_FLOW_NONE,
				platform, "a Dekel port: the TC PLL gives the rate, the row has a Type-C flow");
		} else if (port->phy_kind == I915_XD_PHY_MG) {
			error = drv_i915_xd_mg_pll_words(I915_XD_COVER_TEST_REF, plan.rate_khz, 0, &mg_words);
			port_khz = drv_i915_xd_mg_pll_port_khz(I915_XD_COVER_TEST_REF, &mg_words);
			i915_xd_cover_test_check(plan.pll_set == 0 && error == 0 && port_khz == plan.rate_khz && platform->tc_flow != I915_XD_TC_FLOW_NONE,
				platform, "an MG port: the MG PLL gives the rate, the row has a Type-C flow");
		} else {
			port_khz = drv_i915_xd_dpll_port_khz(platform, I915_XD_COVER_TEST_REF, &plan.pll);
			i915_xd_cover_test_check(plan.pll_set && port_khz == plan.rate_khz && platform->tc_flow == I915_XD_TC_FLOW_NONE,
				platform, "a combo Type-C connector: a DPLL gives the rate, the row has no Type-C flow");
		}
	}

	/* A CD clock step for 1080p60's two pixels a clock. */
	i915_xd_cover_test_check(drv_i915_xd_cdclk_lowest(platform, I915_XD_COVER_TEST_REF, 74250U) != 0U, platform, "a CD clock step for 1080p60");
}

/* A DDI in a transcoder's DDI function: display 11 names the DDI itself, later displays its index plus one. */
static uint32_t
i915_xd_cover_test_select(
	const struct i915_xd_platform *platform,
	unsigned ddi)
{
	/* Display 11's field. */
	if (platform->display_ver < 12U)
		return i915_xd_field_put(I915_XD_DDI_FUNC_SELECT_11, ddi);

	/* Succeeded: the later field. */
	return i915_xd_field_put(I915_XD_DDI_FUNC_SELECT, ddi + 1U);
}

/*
 * The firmware's output on a row: HDMI on a combo port; DisplayPort on
 * the first Type-C connector is the Type-C DisplayPort on a Dekel port
 * and the combo DisplayPort on a desktop's combo connector.
 */
static void
i915_xd_cover_test_classify(
	const struct i915_xd_platform *platform)
{
	const struct i915_xd_port_desc *port;
	struct i915_xd_firmware firmware;

	/* HDMI on a combo port, lit on pipe B. */
	port = i915_xd_cover_test_combo(platform);
	if (port != NULL) {
		memset(&firmware, 0, sizeof(firmware));
		firmware.lit_pipes = 0x2U;
		firmware.pipe[1].ddi_function = I915_XD_DDI_FUNC_ENABLE | i915_xd_cover_test_select(platform, port->ddi) |
			i915_xd_field_put(I915_XD_DDI_FUNC_MODE, I915_XD_DDI_FUNC_MODE_HDMI);
		drv_i915_xd_firmware_classify(platform, 0, &firmware);
		i915_xd_cover_test_check(firmware.output == I915_XD_FW_OUTPUT_HDMI && firmware.output_ddi == (int)port->ddi, platform,
			"the firmware's HDMI on a combo port");
	}

	/* DisplayPort SST on the first Type-C connector. */
	port = i915_xd_cover_test_type_c(platform);
	if (port == NULL)
		return;
	memset(&firmware, 0, sizeof(firmware));
	firmware.lit_pipes = 0x2U;
	firmware.pipe[1].ddi_function = I915_XD_DDI_FUNC_ENABLE | i915_xd_cover_test_select(platform, port->ddi) |
		i915_xd_field_put(I915_XD_DDI_FUNC_MODE, I915_XD_DDI_FUNC_MODE_DP_SST);
	drv_i915_xd_firmware_classify(platform, 0, &firmware);
	if (port->tc != 0U) {
		i915_xd_cover_test_check(firmware.output == I915_XD_FW_OUTPUT_DP_TC, platform, "the firmware's DisplayPort on a Dekel or MG port");
	} else {
		i915_xd_cover_test_check(firmware.output == I915_XD_FW_OUTPUT_DP_COMBO, platform,
			"the firmware's DisplayPort on a combo Type-C connector");
	}
}

/* PCI ids no row has: a Kaby Lake, a Coffee Lake, a Meteor Lake. */
static void
i915_xd_cover_test_unknown(void)
{
	static const uint16_t ids[] = { 0x5917U, 0x3e92U, 0x7d55U };
	struct i915_xd_choice choice;
	unsigned index;
	unsigned refused;
	int error;

	/* Each is refused. */
	refused = 0U;
	for (index = 0U; index < sizeof(ids) / sizeof(ids[0]); index++) {
		error = drv_i915_xd_platform_choose(ids[index], 0x0U, 0, 0U, &choice);
		if (error == ENODEV && choice.platform == NULL)
			refused++;
	}

	/* Checks Kaby Lake, Coffee Lake and Meteor Lake ids have no row. */
	i915_host_check(refused == sizeof(ids) / sizeof(ids[0]), "unknown: Kaby Lake, Coffee Lake and Meteor Lake ids have no row");
}
