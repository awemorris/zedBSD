/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the Gen9 external outputs (display/xd9-outputs.c)
 * on the model of the hardware (gen9-fake-hw.c): the
 * memory latencies, the probe of DP and HDMI sinks and their generations,
 * lighting two outputs beside the panel with the DDB moved in ascending
 * order, a flip, darkening one with the DDB moved back in descending
 * order, a sink that goes, and no pipe left.
 *
 *   sh run-gen9-outputs-host-test.sh
 */

#include "host-test.h"
#include "gen9-fake-hw.h"

#include "../../display/xd9-outputs.h"
#include "../../device-info.h"
#include "gen9-fake-regs.h"

#include <uapi/errno.h>

#include <stdio.h>
#include <string.h>

static void i915_host_latency(void);
static void i915_host_outputs(void);
static void i915_host_init(struct i915_xd9_outputs *outputs);

/* The latencies the outputs use. */
static const uint16_t i915_host_latencies[8] = { 2U, 4U, 10U, 18U, 28U, 40U, 60U, 90U };

/*
 * Checks the external outputs.
 */
int
main(
	int argc,
	char **argv)
{
	int status;
	int error;

	/* A path on the command line receives the golden record of the writes. */
	if (argc > 1) {
		error = i915_gen9_fake_golden_open(argv[1]);
		if (error != 0) {
			fprintf(stderr, "gen9_outputs_host_test: cannot write %s: %d\n", argv[1], error);
			return 2;
		}
	}

	/* Each part. */
	i915_host_latency();
	i915_host_outputs();

	/* The last scene goes to the record. */
	i915_gen9_fake_golden_close();

	/* The tally. */
	status = i915_host_report("gen9_outputs_host_test");
	if (status != 0)
		return status;

	/* Succeeded: the outputs behave as xd9-outputs.c describes. */
	return 0;
}

/* Checks the decode and the adjustments of the PCODE latencies. */
static void
i915_host_latency(void)
{
	uint16_t latency[8];
	uint32_t low;
	uint32_t high;

	/* Level 0 is 0: 2 us more everywhere, then 1 us more on level 0. */
	low = 0U | (4U << 8) | (10U << 16) | (18U << 24);
	high = 28U | (40U << 8) | (60U << 16) | (90U << 24);
	drv_i915_xd9_wm_latency(low, high, 0, latency);
	i915_host_check(latency[0] == 3U && latency[1] == 6U && latency[3] == 20U && latency[7] == 92U, "latency: read latency and level 0");

	/* IPC: 4 us more on every level. */
	drv_i915_xd9_wm_latency(low, high, 1, latency);
	i915_host_check(latency[0] == 7U && latency[1] == 10U && latency[7] == 96U, "latency: IPC");

	/* A zero above level 0 turns the rest off. */
	low = 5U | (8U << 8) | (0U << 16) | (18U << 24);
	drv_i915_xd9_wm_latency(low, high, 0, latency);
	i915_host_check(latency[0] == 6U && latency[1] == 8U && latency[2] == 0U && latency[3] == 0U && latency[7] == 0U, "latency: levels after a zero off");
}

/* Sets the model up: ports B to D on the board, a DP sink on B and an HDMI sink on C attached, and the outputs beside a 1080p panel. */
static void
i915_host_init(
	struct i915_xd9_outputs *outputs)
{
	struct i915_xd9_panel panel;

	/* The board and the sinks. */
	i915_gen9_fake_reset();
	i915_gen9_fake.frames_run = 1;
	i915_gen9_fake_set(I915_SKL_SFUSE_STRAP, I915_SKL_SFUSE_STRAP_DDIB_DETECTED | I915_SKL_SFUSE_STRAP_DDIC_DETECTED | I915_SKL_SFUSE_STRAP_DDID_DETECTED);
	i915_gen9_fake_add_dp_sink(I915_SKL_PORT_B, 1U);
	i915_gen9_fake_add_hdmi_sink(I915_SKL_PORT_C);

	/* The panel: eDP on port A, pipe A, DPLL0, 1080p. */
	memset(&panel, 0, sizeof(panel));
	panel.pipe = 0U;
	panel.ddi = I915_SKL_PORT_A;
	panel.dpll = 0U;
	panel.hdisplay = 1920U;
	panel.htotal = 2200U;
	panel.pixel_khz = 148500U;
	drv_i915_xd9_outputs_init(outputs, i915_gen9_fake_xd(), drv_i915_xd_platform_display9_class(I915_XD_KIND_KBL, I915_XD_POWER_CLASS_U), &panel, 540000U, i915_host_latencies);
}

/* Checks the probe, lighting, a flip, darkening, a sink that goes and no pipe left. */
static void
i915_host_outputs(void)
{
	static struct i915_xd9_outputs outputs;
	unsigned changed;
	int panel_shrunk;
	int b_enabled;
	int b_wm;
	int c_enabled;
	int c_wm;
	int panel_again;
	int error;

	/* Three ports, a DP and an HDMI sink, the outputs beside a 1080p panel. */
	i915_host_init(&outputs);
	i915_host_check(outputs.present == ((1U << 1) | (1U << 2) | (1U << 3)), "outputs: B to D present");
	i915_host_check((i915_gen9_fake_get(I915_SKL_PCH_PORT_HOTPLUG) & I915_SKL_PORTB_HOTPLUG_ENABLE) != 0U, "outputs: hotplug sense on");

	/* Nothing attached yet as far as the live state says. */
	changed = drv_i915_xd9_outputs_poll(&outputs);
	i915_host_check(changed == 0U && outputs.connected == 0U, "poll: nothing live");

	/* B and C come. */
	i915_gen9_fake_set(I915_SKL_SDEISR, I915_SKL_SDE_PORTB_HOTPLUG | I915_SKL_SDE_PORTC_HOTPLUG);
	changed = drv_i915_xd9_outputs_poll(&outputs);
	i915_host_check(changed == ((1U << 1) | (1U << 2)), "poll: B and C changed");
	i915_host_check(outputs.output[0].connected && outputs.output[0].kind == I915_XD9_OUTPUT_DP && outputs.output[0].generation == 1U, "poll: B is DP, generation 1");
	i915_host_check(outputs.output[1].connected && outputs.output[1].kind == I915_XD9_OUTPUT_HDMI && outputs.output[1].generation == 2U, "poll: C is HDMI, generation 2");
	i915_host_check(outputs.output[0].edid_bytes == 256U && outputs.output[0].mode.hdisplay == 1920U && outputs.output[0].mode.clock_khz == 148500, "poll: B's EDID and 1080p");
	i915_host_check(outputs.output[1].mode.hdisplay == 1920U && outputs.output[0].sink_lanes == 4U && outputs.output[0].sink_max_khz == 540000U, "poll: C's mode, B's link");
	i915_host_check(drv_i915_xd9_ext_refresh(&outputs.output[0]) == 60000U, "poll: 60 Hz");

	/* B lit on pipe B and DPLL1, the panel's DDB shrunk before B's transcoder starts. */
	i915_gen9_fake.log_count = 0U;
	error = drv_i915_xd9_outputs_light(&outputs, 0U, 0x01000000U, 120U, 0);
	i915_host_check(error == 0 && outputs.output[0].lit && outputs.output[0].out.pipe == 1U && outputs.output[0].out.dpll == 1U, "light B: pipe B, DPLL1");
	panel_shrunk = i915_gen9_fake_written_at(I915_SKL_PLANE_BUF_CFG_1_A, 0U, 0U);
	b_wm = i915_gen9_fake_written_at(I915_SKL_PLANE_BUF_CFG_1_A + 0x1000U, 0U, 0U);
	b_enabled = i915_gen9_fake_written_at(I915_SKL_TRANSCONF_A + 0x1000U, I915_SKL_TRANSCONF_ENABLE, I915_SKL_TRANSCONF_ENABLE);
	i915_host_check(panel_shrunk >= 0 && panel_shrunk < b_wm && b_wm < b_enabled, "light B: panel's DDB, then B's, then B's transcoder");
	i915_host_check(i915_gen9_fake_get(I915_SKL_PLANE_BUF_CFG_1_A) == (421U << 16), "light B: panel's plane [0, 422)");
	i915_host_check(i915_gen9_fake_written_at(I915_SKL_PLANE_SURF_1_A, 0U, 0U) > panel_shrunk, "light B: panel's surface armed after its DDB");

	/* C lit on pipe C and DPLL2: A then B shrink, ascending, before C starts. */
	i915_gen9_fake.log_count = 0U;
	error = drv_i915_xd9_outputs_light(&outputs, 1U, 0x02000000U, 120U, 1);
	i915_host_check(error == 0 && outputs.output[1].out.pipe == 2U && outputs.output[1].out.dpll == 2U, "light C: pipe C, DPLL2");
	panel_again = i915_gen9_fake_written_at(I915_SKL_PLANE_BUF_CFG_1_A, 0U, 0U);
	b_wm = i915_gen9_fake_written_at(I915_SKL_PLANE_BUF_CFG_1_A + 0x1000U, 0U, 0U);
	c_wm = i915_gen9_fake_written_at(I915_SKL_PLANE_BUF_CFG_1_A + 0x2000U, 0U, 0U);
	c_enabled = i915_gen9_fake_written_at(I915_SKL_TRANSCONF_A + 0x2000U, I915_SKL_TRANSCONF_ENABLE, I915_SKL_TRANSCONF_ENABLE);
	i915_host_check(panel_again >= 0 && panel_again < b_wm && b_wm < c_wm && c_wm < c_enabled, "light C: A, B, C in order, then C's transcoder");
	i915_host_check((i915_gen9_fake_get(I915_SKL_PLANE_CTL_1_A + 0x2000U) & I915_SKL_PLANE_CTL_ORDER_RGBX) != 0U, "light C: RGBX plane");

	/* A flip on B. */
	drv_i915_xd9_outputs_flip(&outputs, 0U, 0x03000000U);
	i915_host_check(drv_i915_xd9_outputs_live(&outputs, 0U) == 0x03000000U, "flip B: live surface");

	/* B dark: its DDB gone, then C and A grow, descending. */
	i915_gen9_fake.log_count = 0U;
	drv_i915_xd9_outputs_darken(&outputs, 0U);
	b_wm = i915_gen9_fake_written_at(I915_SKL_PLANE_BUF_CFG_1_A + 0x1000U, 0U, 0U);
	c_wm = i915_gen9_fake_written_at(I915_SKL_PLANE_BUF_CFG_1_A + 0x2000U, 0U, 0U);
	panel_again = i915_gen9_fake_written_at(I915_SKL_PLANE_BUF_CFG_1_A, 0U, 0U);
	i915_host_check(!outputs.output[0].lit && i915_gen9_fake_get(I915_SKL_TRANSCONF_A + 0x1000U) == 0U, "darken B: transcoder off");
	i915_host_check(b_wm >= 0 && b_wm < c_wm && c_wm < panel_again, "darken B: B's DDB freed, then C, then A");
	i915_host_check(i915_gen9_fake_get(I915_SKL_PLANE_BUF_CFG_1_A + 0x1000U) == 0U, "darken B: B's DDB empty");

	/* C's sink goes: reported, still lit for the caller to darken. */
	i915_gen9_fake_set(I915_SKL_SDEISR, I915_SKL_SDE_PORTB_HOTPLUG);
	changed = drv_i915_xd9_outputs_poll(&outputs);
	i915_host_check(changed == (1U << 2) && !outputs.output[1].connected && outputs.output[1].lit, "poll: C went, still lit");
	drv_i915_xd9_outputs_darken(&outputs, 1U);

	/* With B and D lit and pipes B and C taken, a third has none left. */
	i915_gen9_fake_add_dp_sink(I915_SKL_PORT_D, 1U);
	i915_gen9_fake_add_hdmi_sink(I915_SKL_PORT_C);
	i915_gen9_fake_set(I915_SKL_SDEISR, I915_SKL_SDE_PORTB_HOTPLUG | I915_SKL_SDE_PORTC_HOTPLUG | I915_SKL_SDE_PORTD_HOTPLUG);
	changed = drv_i915_xd9_outputs_poll(&outputs);
	i915_host_check(changed == ((1U << 2) | (1U << 3)) && outputs.output[1].generation == 3U && outputs.output[2].generation == 4U, "poll: C again (generation 3) and D (4)");
	error = drv_i915_xd9_outputs_light(&outputs, 0U, 0x01000000U, 120U, 0);
	i915_host_check(error == 0, "light B again");
	error = drv_i915_xd9_outputs_light(&outputs, 2U, 0x04000000U, 120U, 0);
	i915_host_check(error == 0 && outputs.output[2].out.pipe == 2U && outputs.output[2].out.dpll == 2U, "light D: pipe C, DPLL2");
	error = drv_i915_xd9_outputs_light(&outputs, 1U, 0x02000000U, 120U, 0);
	i915_host_check(error == ENOSPC && !outputs.output[1].lit, "light C: no pipe left");
	error = drv_i915_xd9_outputs_light(&outputs, 0U, 0x01000000U, 120U, 0);
	i915_host_check(error == EBUSY, "light B twice: busy");
}
