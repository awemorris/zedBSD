/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the Gen9 mode set's register sequences
 * (display/xd-aux.c, xd-gmbus.c, xd-ddi.c, xd-output.c, xd-pipe.c, xd-dbuf.c, xd-hpd.c) on a
 * model of the hardware (gen9-fake-hw.c): the AUX channel with a DP sink (DPCD, deferrals,
 * I2C-over-AUX EDID, link training), the GMBUS with an HDMI sink, the
 * power wells, the DPLLs, the transcoders, the DDI buffers and the frame
 * counters.
 *
 *   sh run-gen9-modeset-host-test.sh
 *
 * The model answers as the hardware's registers do; it checks what
 * the code writes and in which order, not the hardware's timing.
 */

#include "host-test.h"
#include "gen9-fake-hw.h"

#include "../../display/xd-aux.h"
#include "../../display/xd-clock.h"
#include "../../display/xd-dbuf.h"
#include "../../display/xd-ddi.h"
#include "../../display/xd-dp.h"
#include "../../display/xd-gmbus.h"
#include "../../display/xd-hpd.h"
#include "../../display/xd-output.h"
#include "../../display/xd-pipe.h"
#include "../../display/xd-power.h"
#include "../../display/xd9-outputs.h"
#include "gen9-fake-regs.h"

#include <uapi/errno.h>

#include <stdio.h>
#include <string.h>

static const struct i915_xd_platform *i915_host_platform(unsigned kind, unsigned power_class);
static void i915_host_dp_port(struct i915_xd9_dp_port *dp, unsigned port);
static void i915_host_check_ddc(void);
static void i915_host_check_power(void);
static void i915_host_check_dp_output(void);
static void i915_host_check_dp_failure(void);
static void i915_host_check_hdmi_output(void);
static void i915_host_check_watermarks(void);
static void i915_host_check_hotplug(void);

/*
 * Checks the Gen9 mode set's register sequences on the model.
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
			fprintf(stderr, "gen9_modeset_host_test: cannot write %s: %d\n", argv[1], error);
			return 2;
		}
	}

	/* Each part. */
	i915_host_check_ddc();
	i915_host_check_power();
	i915_host_check_dp_output();
	i915_host_check_dp_failure();
	i915_host_check_hdmi_output();
	i915_host_check_watermarks();
	i915_host_check_hotplug();

	/* The last scene goes to the record. */
	i915_gen9_fake_golden_close();

	/* The tally. */
	status = i915_host_report("gen9_modeset_host_test");
	if (status != 0)
		return status;

	/* Succeeded: the sequences are the ones the xd code describes. */
	return 0;
}

/* Checks DPCD reads and writes with deferrals, a missing sink, and both EDID paths. */
static void
i915_host_check_ddc(void)
{
	struct i915_xd_gmbus gmbus;
	struct i915_xd_aux aux;
	uint8_t bytes[16];
	uint8_t edid[256];
	int error;

	/* A DP sink on B and an HDMI sink on C. */
	i915_gen9_fake_reset();
	i915_gen9_fake_add_dp_sink(I915_SKL_PORT_B, 1U);
	i915_gen9_fake_add_hdmi_sink(I915_SKL_PORT_C);

	/* The receiver capabilities, after two deferrals of the wake-up read. */
	i915_gen9_fake.sinks[I915_SKL_PORT_B].defer_left = 2U;
	memset(bytes, 0, sizeof(bytes));
	(void)drv_i915_xd_aux_init(&aux, I915_SKL_PORT_B);
	error = drv_i915_xd_dpcd_read(&aux, i915_gen9_fake_xd(), 0U, bytes, I915_DPCD_RECEIVER_CAP_SIZE);
	i915_host_check(error == 0 && bytes[0] == 0x12U && bytes[1] == 0x14U && bytes[2] == 0xc4U, "dpcd read after deferrals");
	i915_host_check(i915_gen9_fake.aux_transfers == 4U, "dpcd read took the wake-up read's three transfers and its own");

	/* A write. */
	bytes[0] = 0x02U;
	error = drv_i915_xd_dpcd_write(&aux, i915_gen9_fake_xd(), I915_DPCD_SET_POWER, bytes, 1U);
	i915_host_check(error == 0 && i915_gen9_fake.sinks[I915_SKL_PORT_B].dpcd[I915_DPCD_SET_POWER] == 0x02U, "dpcd write");

	/* No sink on port D: the wake-up read's 32 messages of five timeouts each. */
	i915_gen9_fake.aux_transfers = 0U;
	(void)drv_i915_xd_aux_init(&aux, I915_SKL_PORT_D);
	error = drv_i915_xd_dpcd_read(&aux, i915_gen9_fake_xd(), 0U, bytes, 1U);
	i915_host_check(error == ETIMEDOUT && i915_gen9_fake.aux_transfers == 160U, "dpcd read times out 32 messages of five transfers on an empty port");

	/* The EDID over AUX, both blocks. */
	memset(edid, 0, sizeof(edid));
	(void)drv_i915_xd_aux_init(&aux, I915_SKL_PORT_B);
	error = drv_i915_xd_aux_edid_blocks(&aux, i915_gen9_fake_xd(), edid, 2U);
	i915_host_check(error == 0 && memcmp(edid, i915_gen9_fake.sinks[I915_SKL_PORT_B].edid, sizeof(edid)) == 0, "aux edid two blocks");

	/* The EDID over the GMBUS of port C, both blocks, the clock gating back. */
	memset(edid, 0, sizeof(edid));
	drv_i915_xd_gmbus_init(&gmbus);
	error = drv_i915_xd9_gmbus_edid_blocks(&gmbus, i915_gen9_fake_xd(), drv_i915_xd9_gmbus_pin(I915_SKL_PORT_C), edid, 2U);
	i915_host_check(error == 0 && memcmp(edid, i915_gen9_fake.sinks[I915_SKL_PORT_C].edid, sizeof(edid)) == 0, "gmbus edid two blocks");
	i915_host_check((i915_gen9_fake_get(I915_SKL_SOUTH_DSPCLK_GATE_D) & I915_SKL_PCH_GMBUSUNIT_CLOCK_GATE_DISABLE) == 0U, "gmbus clock gating restored");
	i915_host_check(i915_gen9_fake_written_at(I915_SKL_GMBUS1, I915_SKL_GMBUS_CYCLE_STOP, I915_SKL_GMBUS_CYCLE_STOP) >= 0, "gmbus ends with a stop");

	/* Nothing on port D's pin: a NAK, cleared. */
	i915_gen9_fake.log_count = 0U;
	drv_i915_xd_gmbus_init(&gmbus);
	error = drv_i915_xd9_gmbus_edid_blocks(&gmbus, i915_gen9_fake_xd(), drv_i915_xd9_gmbus_pin(I915_SKL_PORT_D), edid, 1U);
	i915_host_check(error == ENXIO, "gmbus NAK on an empty pin");
	i915_host_check(i915_gen9_fake_written_at(I915_SKL_GMBUS1, I915_SKL_GMBUS_SW_CLR_INT, I915_SKL_GMBUS_SW_CLR_INT) >= 0, "gmbus NAK cleared");
	i915_host_check(drv_i915_xd9_gmbus_pin(I915_SKL_PORT_B) == 5U && drv_i915_xd9_gmbus_pin(I915_SKL_PORT_A) == 0U, "gmbus pins");
}

/* Checks power well 2, a DDI IO well, and a DPLL's start and stop. */
static void
i915_host_check_power(void)
{
	const struct i915_xd_platform *platform;
	struct i915_xd9_dpll_words words;
	uint32_t ctrl1;
	unsigned writes;
	int error;

	/* Nothing attached. */
	i915_gen9_fake_reset();
	platform = i915_host_platform(I915_XD_KIND_KBL, I915_XD_POWER_CLASS_H);

	/* Power well 2, then again with nothing written. */
	error = drv_i915_xd9_power_pw2_on(i915_gen9_fake_xd(), platform);
	i915_host_check(error == 0 && (i915_gen9_fake_get(I915_SKL_PWR_WELL_CTL2) & I915_SKL_PW_2_STATE) != 0U, "pw2 on");
	writes = i915_gen9_fake.log_count;
	error = drv_i915_xd9_power_pw2_on(i915_gen9_fake_xd(), platform);
	i915_host_check(error == 0 && i915_gen9_fake.log_count == writes, "pw2 already on writes nothing");

	/* A DDI IO well. */
	error = drv_i915_xd9_power_ddi_io(i915_gen9_fake_xd(), platform, I915_SKL_PORT_C, 1);
	i915_host_check(error == 0 && (i915_gen9_fake_get(I915_SKL_PWR_WELL_CTL2) & I915_SKL_PW_DDI_STATE(I915_SKL_PORT_C)) != 0U, "ddi io well on");
	(void)drv_i915_xd9_power_ddi_io(i915_gen9_fake_xd(), platform, I915_SKL_PORT_C, 0);
	i915_host_check((i915_gen9_fake_get(I915_SKL_PWR_WELL_CTL2) & I915_SKL_PW_DDI_REQUEST(I915_SKL_PORT_C)) == 0U, "ddi io well request dropped");

	/* DPLL2 at 148.5 MHz HDMI, DPLL0's field kept. */
	i915_gen9_fake_set(I915_SKL_DPLL_CTRL1, 0x0bU);
	(void)drv_i915_xd9_dpll_hdmi_words(148500U, &words);
	error = drv_i915_xd9_dpll_enable(i915_gen9_fake_xd(), platform, 2U, &words);
	ctrl1 = i915_gen9_fake_get(I915_SKL_DPLL_CTRL1);
	i915_host_check(error == 0 && ctrl1 == (0x0bU | (0x21U << 12)), "dpll2 CTRL1 field");
	i915_host_check(i915_gen9_fake_get(0x6c048U) == 0x80400173U && i915_gen9_fake_get(0x6c04cU) == 0x3a5U, "dpll2 CFGCR words");
	i915_host_check((i915_gen9_fake_get(I915_SKL_DPLL2_CTL) & I915_SKL_DPLL_ENABLE) != 0U, "dpll2 enabled");
	drv_i915_xd9_dpll_disable(i915_gen9_fake_xd(), platform, 2U);
	i915_host_check((i915_gen9_fake_get(I915_SKL_DPLL2_CTL) & I915_SKL_DPLL_ENABLE) == 0U, "dpll2 stopped");

	/* DPLL0 is refused. */
	error = drv_i915_xd9_dpll_enable(i915_gen9_fake_xd(), platform, 0U, &words);
	i915_host_check(error == EINVAL, "dpll0 refused");
}

/* Checks a 1080p DP output on port B, pipe B, DPLL1, and its way back. */
static void
i915_host_check_dp_output(void)
{
	const struct i915_xd_platform *platform;
	struct i915_xd_dp_choice choice;
	struct i915_xd9_output config;
	struct i915_xd9_dp_port dp;
	const struct i915_gen9_fake_sink *sink;
	uint32_t value;
	int ctrl1_at;
	int ctrl2_at;
	int buffer_at;
	int function_at;
	int enable_at;
	int control_at;
	int surface_at;
	int error;

	/* A DP sink on B, the frames running. */
	i915_gen9_fake_reset();
	i915_gen9_fake_add_dp_sink(I915_SKL_PORT_B, 1U);
	i915_gen9_fake.frames_run = 1;
	sink = &i915_gen9_fake.sinks[I915_SKL_PORT_B];

	/* The configuration of 1080p on RBR x4. */
	platform = i915_host_platform(I915_XD_KIND_KBL, I915_XD_POWER_CLASS_U);
	memset(&config, 0, sizeof(config));
	config.ddi = I915_SKL_PORT_B;
	config.pipe = 1U;
	config.dpll = 1U;
	(void)drv_i915_xd9_pick_mode(sink->edid, 128U, 540000U, &config.mode);
	(void)drv_i915_xd9_dp_link(540000U, 4U, (uint32_t)config.mode.clock_khz, &choice, &config.m_n);
	config.rate_khz = choice.rate_khz;
	config.lanes = choice.lanes;
	(void)drv_i915_xd9_dpll_dp_words(config.rate_khz, &config.dpll_words);
	config.surf = 0x01000000U;
	config.stride_units = 1920U * 4U / 64U;
	i915_host_dp_port(&dp, I915_SKL_PORT_B);

	/* The output on. */
	error = drv_i915_xd9_output_on(i915_gen9_fake_xd(), platform, &config, &dp);
	i915_host_check(error == 0, "dp output on");

	/* The sink: RBR, four lanes with enhanced framing, training off, D0, 8b/10b. */
	i915_host_check(sink->dpcd[I915_DPCD_LINK_BW_SET] == I915_DPCD_LINK_BW_1_62 && sink->dpcd[I915_DPCD_LANE_COUNT_SET] == 0x84U, "dp sink link set");
	i915_host_check(sink->dpcd[I915_DPCD_TRAINING_PATTERN_SET] == 0U && sink->dpcd[I915_DPCD_SET_POWER] == I915_DPCD_SET_POWER_D0, "dp sink trained and on");
	i915_host_check(sink->dpcd[I915_DPCD_MAIN_LINK_CHANNEL_CODING_SET] == I915_DPCD_SET_ANSI_8B10B && sink->dpcd[I915_DPCD_TRAINING_LANE0_SET + 3U] == 0x01U, "dp sink coding and lane 3 swing 1");
	i915_host_check((dp.set & 0x3U) == 1U, "dp trained at the swing the sink asked for");

	/* The port: entry 4 (swing 1), on, four lanes; the transport sending pixels with enhanced framing. */
	value = i915_gen9_fake_get(I915_SKL_DDI_BUF_CTL_A + 0x100U);
	i915_host_check(value == (I915_SKL_DDI_BUF_ENABLE | (4U << 24) | (3U << 1)), "dp buffer control");
	value = i915_gen9_fake_get(I915_SKL_DP_TP_CTL_A + 0x100U);
	i915_host_check(value == (I915_SKL_DP_TP_ENABLE | I915_SKL_DP_TP_TRAIN_NORMAL | I915_SKL_DP_TP_ENHANCED_FRAME), "dp transport normal");

	/* The clocks: DPLL1 at RBR, port B on DPLL1 with the override, transcoder B from port B. */
	i915_host_check(((i915_gen9_fake_get(I915_SKL_DPLL_CTRL1) >> 6) & 0x3fU) == 0x5U, "dp dpll1 at RBR");
	value = i915_gen9_fake_get(I915_SKL_DPLL_CTRL2);
	i915_host_check((value & I915_SKL_DPLL_CTRL2_CLK_OFF(I915_SKL_PORT_B)) == 0U && ((value >> 3) & 0x7U) == ((1U << 1) | 1U), "dp port clock select");
	i915_host_check(i915_gen9_fake_get(I915_SKL_TRANS_CLK_SEL_A + 4U) == (2U << 29), "dp transcoder clock");

	/* The transcoder: M/N, timings, the DDI function, running. */
	i915_host_check(i915_gen9_fake_get(I915_SKL_DATA_M1_A + 0x1000U) == ((63U << 25) | 0x580000U), "dp data M with TU 64");
	i915_host_check(i915_gen9_fake_get(I915_SKL_LINK_N1_A + 0x1000U) == 0x80000U, "dp link N");
	i915_host_check(i915_gen9_fake_get(I915_SKL_TRANS_HTOTAL_A + 0x1000U) == ((2199U << 16) | 1919U), "dp htotal");
	i915_host_check(i915_gen9_fake_get(I915_SKL_TRANS_VSYNC_A + 0x1000U) == ((1088U << 16) | 1083U), "dp vsync");
	i915_host_check(i915_gen9_fake_get(I915_SKL_PIPESRC_A + 0x1000U) == ((1919U << 16) | 1079U), "dp pipe source");
	value = I915_SKL_TRANS_DDI_FUNC_ENABLE | (1U << 28) | (2U << 24) | I915_SKL_TRANS_DDI_PVSYNC | I915_SKL_TRANS_DDI_PHSYNC | (3U << 1);
	i915_host_check(i915_gen9_fake_get(I915_SKL_TRANS_DDI_FUNC_CTL_A + 0x1000U) == value, "dp DDI function");
	i915_host_check(i915_gen9_fake_get(I915_SKL_TRANSCONF_A + 0x1000U) == (I915_SKL_TRANSCONF_ENABLE | I915_SKL_TRANSCONF_STATE), "dp transcoder running");
	i915_host_check(i915_gen9_fake_get(I915_SKL_TRANS_MSA_MISC_A + 0x1000U) == 0x21U, "dp MSA");

	/* The plane: on, XRGB8888, plane gamma off, the stride and size, the surface. */
	i915_host_check(i915_gen9_fake_get(I915_SKL_PLANE_CTL_1_A + 0x1000U) == (I915_SKL_PLANE_CTL_ENABLE | (4U << 24) | (1U << 13)), "dp plane control");
	i915_host_check(i915_gen9_fake_get(I915_SKL_PLANE_STRIDE_1_A + 0x1000U) == 120U && i915_gen9_fake_get(I915_SKL_PLANE_SIZE_1_A + 0x1000U) == ((1079U << 16) | 1919U), "dp plane stride and size");
	i915_host_check(i915_gen9_fake_get(I915_SKL_PLANE_SURF_1_A + 0x1000U) == 0x01000000U, "dp plane surface");

	/* The order: DPLL, port clock, buffer, DDI function, transcoder, plane control, surface. */
	ctrl1_at = i915_gen9_fake_written_at(I915_SKL_DPLL_CTRL1, 0U, 0U);
	ctrl2_at = i915_gen9_fake_written_at(I915_SKL_DPLL_CTRL2, 0U, 0U);
	buffer_at = i915_gen9_fake_written_at(I915_SKL_DDI_BUF_CTL_A + 0x100U, I915_SKL_DDI_BUF_ENABLE, I915_SKL_DDI_BUF_ENABLE);
	function_at = i915_gen9_fake_written_at(I915_SKL_TRANS_DDI_FUNC_CTL_A + 0x1000U, 0U, 0U);
	enable_at = i915_gen9_fake_written_at(I915_SKL_TRANSCONF_A + 0x1000U, I915_SKL_TRANSCONF_ENABLE, I915_SKL_TRANSCONF_ENABLE);
	control_at = i915_gen9_fake_written_at(I915_SKL_PLANE_CTL_1_A + 0x1000U, 0U, 0U);
	surface_at = i915_gen9_fake_written_at(I915_SKL_PLANE_SURF_1_A + 0x1000U, 0U, 0U);
	i915_host_check(ctrl1_at >= 0 && ctrl1_at < ctrl2_at && ctrl2_at < buffer_at && buffer_at < function_at, "dp order up to the DDI function");
	i915_host_check(function_at < enable_at && enable_at < control_at && control_at < surface_at, "dp order up to the surface");

	/* The way back. */
	drv_i915_xd9_output_off(i915_gen9_fake_xd(), platform, &config, &dp);
	i915_host_check(i915_gen9_fake_get(I915_SKL_PLANE_CTL_1_A + 0x1000U) == 0U && i915_gen9_fake_get(I915_SKL_TRANSCONF_A + 0x1000U) == 0U, "dp off plane and transcoder");
	i915_host_check((i915_gen9_fake_get(I915_SKL_TRANS_DDI_FUNC_CTL_A + 0x1000U) & (I915_SKL_TRANS_DDI_FUNC_ENABLE | (7U << 28))) == 0U, "dp off DDI function");
	i915_host_check(sink->dpcd[I915_DPCD_SET_POWER] == I915_DPCD_SET_POWER_D3 && i915_gen9_fake_get(I915_SKL_TRANS_CLK_SEL_A + 4U) == 0U, "dp off sink D3 and clock");
	i915_host_check((i915_gen9_fake_get(I915_SKL_DDI_BUF_CTL_A + 0x100U) & I915_SKL_DDI_BUF_ENABLE) == 0U, "dp off buffer");
	i915_host_check((i915_gen9_fake_get(I915_SKL_DPLL_CTRL2) & I915_SKL_DPLL_CTRL2_CLK_OFF(I915_SKL_PORT_B)) != 0U, "dp off port clock");
	i915_host_check((i915_gen9_fake_get(I915_SKL_DPLL1_CTL) & I915_SKL_DPLL_ENABLE) == 0U, "dp off dpll1");
	i915_host_check((i915_gen9_fake_get(I915_SKL_PWR_WELL_CTL2) & I915_SKL_PW_DDI_REQUEST(I915_SKL_PORT_B)) == 0U, "dp off io well");
}

/* Checks that a sink whose clock never recovers leaves the port, the clock and the DPLL off. */
static void
i915_host_check_dp_failure(void)
{
	const struct i915_xd_platform *platform;
	struct i915_xd_dp_choice choice;
	struct i915_xd9_output config;
	struct i915_xd9_dp_port dp;
	int error;

	/* A DP sink on D that recovers its clock only at swing 4. */
	i915_gen9_fake_reset();
	i915_gen9_fake_add_dp_sink(I915_SKL_PORT_D, 4U);

	/* 1024x768 on RBR x1, which cannot train. */
	platform = i915_host_platform(I915_XD_KIND_SKL, I915_XD_POWER_CLASS_H);
	memset(&config, 0, sizeof(config));
	config.ddi = I915_SKL_PORT_D;
	config.pipe = 2U;
	config.dpll = 3U;
	(void)drv_i915_xd9_pick_mode(NULL, 0U, 540000U, &config.mode);
	(void)drv_i915_xd9_dp_link(162000U, 4U, (uint32_t)config.mode.clock_khz, &choice, &config.m_n);
	config.rate_khz = choice.rate_khz;
	config.lanes = choice.lanes;
	(void)drv_i915_xd9_dpll_dp_words(config.rate_khz, &config.dpll_words);
	i915_host_dp_port(&dp, I915_SKL_PORT_D);

	/* The training fails and everything is stopped again. */
	error = drv_i915_xd9_output_on(i915_gen9_fake_xd(), platform, &config, &dp);
	i915_host_check(error == EIO, "dp failed training reports EIO");
	i915_host_check((i915_gen9_fake_get(I915_SKL_DDI_BUF_CTL_A + 0x300U) & I915_SKL_DDI_BUF_ENABLE) == 0U, "dp failed buffer off");
	i915_host_check((i915_gen9_fake_get(I915_SKL_DPLL3_CTL) & I915_SKL_DPLL_ENABLE) == 0U, "dp failed dpll3 off");
	i915_host_check((i915_gen9_fake_get(I915_SKL_DPLL_CTRL2) & I915_SKL_DPLL_CTRL2_CLK_OFF(I915_SKL_PORT_D)) != 0U, "dp failed port clock off");
	i915_host_check(i915_gen9_fake_get(I915_SKL_TRANSCONF_A + 0x2000U) == 0U, "dp failed transcoder untouched");
}

/* Checks a 1080p HDMI output on port C, pipe C, DPLL2. */
static void
i915_host_check_hdmi_output(void)
{
	const struct i915_xd_platform *platform;
	const struct i915_xd9_buf_trans *table;
	struct i915_xd9_output config;
	unsigned count;
	uint32_t trans;
	uint32_t value;
	int error;

	/* An HDMI sink on C. */
	i915_gen9_fake_reset();
	i915_gen9_fake_add_hdmi_sink(I915_SKL_PORT_C);

	/* The configuration. */
	platform = i915_host_platform(I915_XD_KIND_KBL, I915_XD_POWER_CLASS_U);
	memset(&config, 0, sizeof(config));
	config.hdmi = 1;
	config.ddi = I915_SKL_PORT_C;
	config.pipe = 2U;
	config.dpll = 2U;
	(void)drv_i915_xd9_pick_mode(i915_gen9_fake.sinks[I915_SKL_PORT_C].edid, 128U, 300000U, &config.mode);
	(void)drv_i915_xd9_dpll_hdmi_words((uint32_t)config.mode.clock_khz, &config.dpll_words);
	table = drv_i915_xd9_ddi_buf_table(platform, I915_XD9_OUTPUT_HDMI, I915_SKL_PORT_C, &count);
	config.hdmi_row = &table[drv_i915_xd9_ddi_hdmi_row(platform)];
	config.surf = 0x02000000U;
	config.stride_units = 120U;
	config.rgbx = 1;

	/* The output on. */
	error = drv_i915_xd9_output_on(i915_gen9_fake_xd(), platform, &config, NULL);
	i915_host_check(error == 0, "hdmi output on");

	/* The buffer: entry 9 holds the table's entry 8, its boost, on. */
	trans = I915_SKL_DDI_BUF_TRANS_A + 2U * I915_SKL_DDI_BUF_TRANS_STRIDE + 9U * 8U;
	i915_host_check(i915_gen9_fake_get(trans) == 0x80003015U && i915_gen9_fake_get(trans + 4U) == 0xcdU, "hdmi translation entry 9");
	value = i915_gen9_fake_get(I915_SKL_DISPIO_CR_TX_BMU_CR0);
	i915_host_check((value & I915_SKL_BALANCE_LEG_MASK(I915_SKL_PORT_C)) == (1U << I915_SKL_BALANCE_LEG_SHIFT(I915_SKL_PORT_C)), "hdmi boost");
	i915_host_check((i915_gen9_fake_get(I915_SKL_DDI_BUF_CTL_A + 0x200U) & I915_SKL_DDI_BUF_ENABLE) != 0U, "hdmi buffer on");

	/* Display WA #1143: the override toggled on transcoder C's chicken register and off again. */
	i915_host_check(i915_gen9_fake_written_at(I915_SKL_CHICKEN_TRANS_A + 4U, 3U << 18, 3U << 18) >= 0, "hdmi WA 1143 override on");
	i915_host_check((i915_gen9_fake_get(I915_SKL_CHICKEN_TRANS_A + 4U) & (3U << 18)) == 0U, "hdmi WA 1143 override off");

	/* The transcoder: HDMI mode on port C, no M/N, running; the plane in RGBX order. */
	value = I915_SKL_TRANS_DDI_FUNC_ENABLE | (2U << 28) | I915_SKL_TRANS_DDI_PVSYNC | I915_SKL_TRANS_DDI_PHSYNC;
	i915_host_check(i915_gen9_fake_get(I915_SKL_TRANS_DDI_FUNC_CTL_A + 0x2000U) == value, "hdmi DDI function");
	i915_host_check(i915_gen9_fake_get(I915_SKL_DATA_M1_A + 0x2000U) == 0U, "hdmi no M/N");
	i915_host_check((i915_gen9_fake_get(I915_SKL_TRANSCONF_A + 0x2000U) & I915_SKL_TRANSCONF_STATE) != 0U, "hdmi transcoder running");
	i915_host_check((i915_gen9_fake_get(I915_SKL_PLANE_CTL_1_A + 0x2000U) & I915_SKL_PLANE_CTL_ORDER_RGBX) != 0U, "hdmi plane RGBX");
	i915_host_check(((i915_gen9_fake_get(I915_SKL_DPLL_CTRL1) >> 12) & 0x3fU) == 0x21U, "hdmi dpll2 in HDMI mode");
}

/* Checks the words a pipe's watermarks and DDB are written as. */
static void
i915_host_check_watermarks(void)
{
	static const uint16_t latency[8] = { 2U, 4U, 10U, 18U, 28U, 40U, 60U, 90U };
	struct i915_xd9_pipe_plan pipes[3];

	/* Nothing attached. */
	i915_gen9_fake_reset();

	/* One 1080p pipe B. */
	memset(pipes, 0, sizeof(pipes));
	pipes[1].active = 1;
	pipes[1].hdisplay = 1920U;
	pipes[1].htotal = 2200U;
	pipes[1].pixel_khz = 148500U;
	pipes[1].plane_width = 1920U;
	(void)drv_i915_xd9_dbuf_plan(i915_host_platform(I915_XD_KIND_KBL, I915_XD_POWER_CLASS_H), latency, pipes, 3U);
	drv_i915_xd9_wm_write_pipe(i915_gen9_fake_xd(), 1U, &pipes[1]);

	/* The plane's levels 0 and 7, the DDB, the cursor's DDB, the line time. */
	i915_host_check(i915_gen9_fake_get(I915_SKL_PLANE_WM_1_A_0 + 0x1000U) == (0x80000000U | 4U), "wm level 0 word");
	i915_host_check(i915_gen9_fake_get(I915_SKL_PLANE_WM_1_A_0 + 0x1000U + 28U) == (0x80000000U | (7U << 14) | 107U), "wm level 7 word");
	i915_host_check(i915_gen9_fake_get(I915_SKL_PLANE_BUF_CFG_1_A + 0x1000U) == (859U << 16), "wm plane DDB");
	i915_host_check(i915_gen9_fake_get(I915_SKL_CUR_BUF_CFG_A + 0x1000U) == ((891U << 16) | 860U), "wm cursor DDB");
	i915_host_check(i915_gen9_fake_get(I915_SKL_CUR_WM_A_0 + 0x1000U) == 0U, "wm cursor level 0 off");
	i915_host_check(i915_gen9_fake_get(I915_SKL_WM_LINETIME_A + 4U) == 119U, "wm line time");
}

/* Checks the hotplug enables, the present and attached ports, and the vertical blank wait. */
static void
i915_host_check_hotplug(void)
{
	int error;

	/* Nothing attached. */
	i915_gen9_fake_reset();

	/* The enables. */
	drv_i915_xd9_hpd_enable(i915_gen9_fake_xd());
	i915_host_check(i915_gen9_fake_get(I915_SKL_PCH_PORT_HOTPLUG) == ((1U << 4) | (1U << 12) | (1U << 20)), "hotplug enables");

	/* Ports B and D on the board, a sink on C. */
	i915_gen9_fake_set(I915_SKL_SFUSE_STRAP, I915_SKL_SFUSE_STRAP_DDIB_DETECTED | I915_SKL_SFUSE_STRAP_DDID_DETECTED);
	i915_gen9_fake_set(I915_SKL_SDEISR, I915_SKL_SDE_PORTC_HOTPLUG);
	i915_host_check(drv_i915_xd9_hpd_present(i915_gen9_fake_xd()) == ((1U << 1) | (1U << 3)), "ports present");
	i915_host_check(drv_i915_xd9_hpd_connected(i915_gen9_fake_xd()) == (1U << 2), "ports connected");

	/* A running pipe's blank comes; a stopped one's does not. */
	i915_gen9_fake.frames_run = 1;
	error = drv_i915_xd9_pipe_wait_vblank(i915_gen9_fake_xd(), 1U);
	i915_host_check(error == 0, "vblank of a running pipe");
	i915_gen9_fake.frames_run = 0;
	error = drv_i915_xd9_pipe_wait_vblank(i915_gen9_fake_xd(), 1U);
	i915_host_check(error == ETIMEDOUT, "vblank of a stopped pipe times out");
}


/* Reports the display 9 row of a kind and a power class. */
static const struct i915_xd_platform *
i915_host_platform(
	unsigned kind,
	unsigned power_class)
{
	/* Succeeded: the row. */
	return drv_i915_xd_platform_display9_class(kind, power_class);
}

/* Fills a DisplayPort port from its sink's capabilities and the Kaby Lake U table. */
static void
i915_host_dp_port(
	struct i915_xd9_dp_port *dp,
	unsigned port)
{
	memset(dp, 0, sizeof(*dp));
	(void)drv_i915_xd_aux_init(&dp->aux, port);
	dp->io = i915_gen9_fake_xd();
	dp->ddi = port;

	/* The capabilities, as the node reads them. */
	(void)drv_i915_xd_dpcd_read(&dp->aux, dp->io, 0U, dp->caps, sizeof(dp->caps));

	/* Succeeded: the table. */
	dp->table = drv_i915_xd9_ddi_buf_table(i915_host_platform(I915_XD_KIND_KBL, I915_XD_POWER_CLASS_U), I915_XD9_OUTPUT_DP, port, &dp->rows);
}
