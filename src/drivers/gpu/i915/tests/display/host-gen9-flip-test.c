/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the Gen9 flip display's arithmetic (display/xd9-flip.c):
 * the readout of the firmware's display registers (a Let's Note
 * SV7 as its firmware leaves it, the variants the flips accept and every
 * refusal), the port and pixel clocks and the refresh, and the display
 * window's size.
 *
 *   sh run-gen9-flip-host-test.sh
 *
 * The node operations need the GPU and are not reached; the link drops
 * them.
 */

#include "host-test.h"

#include "../../display/xd9-flip.h"
#include "../../display/xd-clock.h"
#include "../../display/xd-takeover.h"
#include "gen9-fake-regs.h"

#include <uapi/errno.h>

#include <stdio.h>
#include <string.h>

static void i915_host_skl_sv7(struct i915_xd9_fw_registers *registers);
static void i915_host_skl_accepted(void);
static void i915_host_skl_variants(void);
static void i915_host_skl_refused(void);
static void i915_host_skl_refusal(const struct i915_xd9_fw_registers *registers, const char *reason);
static void i915_host_skl_clocks(void);
static void i915_host_skl_window(void);

/*
 * Checks the Gen9 flip display's arithmetic.
 */
int
main(void)
{
	int status;

	/* The SV7 as its firmware leaves it, the other accepted forms, and the refusals. */
	i915_host_skl_accepted();
	i915_host_skl_variants();
	i915_host_skl_refused();

	/* The clocks and the display window. */
	i915_host_skl_clocks();
	i915_host_skl_window();

	/* The tally. */
	status = i915_host_report("gen9_flip_host_test");
	if (status != 0)
		return status;

	/* Succeeded: the arithmetic is the one xd9-flip.c describes. */
	return 0;
}

/*
 * Fills the registers of a Let's Note SV7 (Kaby Lake R) as its firmware
 * leaves them: the 1920x1200 eDP panel on transcoder EDP and pipe A, DDI A
 * clocked by DPLL0 at 2.7 GHz (HBR), the first plane on a linear XRGB8888
 * surface at GGTT 0, and a 154 MHz pixel clock (2080x1235 totals, 59.95 Hz).
 */
static void
i915_host_skl_sv7(
	struct i915_xd9_fw_registers *registers)
{
	memset(registers, 0, sizeof(*registers));

	/* Power wells 1 and 2 requested and up, no DC state, DPLL0 at link rate 1350 (port clock 270 MHz) for DDI A. */
	registers->power = 0xf0000000U;
	registers->dc_state = 0U;
	registers->cdclk = 0x000003ffU;
	registers->dpll_ctrl1 = (I915_SKL_LINK_RATE_1350 << 1) | 1U;
	registers->dpll_ctrl2 = 1U;

	/* The eDP transcoder: DP SST on DDI A from pipe A (on/off), running, with its totals and link M/N. */
	registers->transcoder_read[I915_SKL_TRANSCODER_EDP] = 1;
	registers->ddi_func[I915_SKL_TRANSCODER_EDP] = 0x80000000U | (2U << 24) | (4U << 12);
	registers->transconf[I915_SKL_TRANSCODER_EDP] = 0xc0000000U;
	registers->htotal[I915_SKL_TRANSCODER_EDP] = ((2080U - 1U) << 16) | (1920U - 1U);
	registers->vtotal[I915_SKL_TRANSCODER_EDP] = ((1235U - 1U) << 16) | (1200U - 1U);
	registers->link_m[I915_SKL_TRANSCODER_EDP] = 299038U;
	registers->link_n[I915_SKL_TRANSCODER_EDP] = 0x80000U;

	/* Transcoders A to C are off. */
	registers->transcoder_read[0] = 1;
	registers->transcoder_read[1] = 1;
	registers->transcoder_read[2] = 1;

	/* Pipe A's first plane: on, XRGB8888, linear, plane gamma off, 1920x1200, stride 120 units, surface 0. */
	registers->pipe_read[0] = 1;
	registers->pipe_src[0] = ((1920U - 1U) << 16) | (1200U - 1U);
	registers->plane_ctl[0] = 0x80000000U | (4U << 24) | (1U << 13);
	registers->plane_stride[0] = 120U;
	registers->plane_size[0] = ((1200U - 1U) << 16) | (1920U - 1U);
	registers->plane_surf[0] = 0U;
	registers->plane_surflive[0] = 0U;

	/* Pipes B and C are off. */
	registers->pipe_read[1] = 1;
	registers->pipe_read[2] = 1;
}

/* Checks the readout of the SV7 as its firmware leaves it. */
static void
i915_host_skl_accepted(void)
{
	struct i915_xd9_fw_registers registers;
	struct i915_xd9_fw_readout readout;
	int error;

	/* The SV7: pipe A on transcoder EDP, 1920x1200, 59.95 Hz measured. */
	i915_host_skl_sv7(&registers);
	error = drv_i915_xd9_fw_decode(&registers, &readout);
	i915_host_check(error == 0 && readout.reason == NULL, "sv7: flips accepted");
	i915_host_check(readout.lit_pipes == 1U && readout.pipe == 0U && readout.transcoder == I915_SKL_TRANSCODER_EDP, "sv7: pipe A from transcoder EDP");
	i915_host_check(readout.width == 1920U && readout.height == 1200U && readout.stride_bytes == 7680U, "sv7: 1920x1200, 7680 bytes a row");
	i915_host_check(readout.rgbx == 0 && readout.surf == 0U && readout.scaled == 0, "sv7: B G R X at GGTT 0, unscaled");
	i915_host_check(readout.pixel_khz == 154000U, "sv7: pixel clock 154000 kHz");
	i915_host_check(readout.refresh_measured == 1 && readout.refresh_millihz == 59950U, "sv7: refresh 59950 mHz measured");
}

/* Checks the accepted forms other than the SV7's: another pipe, the other byte order, a scaler, and assumed refreshes. */
static void
i915_host_skl_variants(void)
{
	struct i915_xd9_fw_registers registers;
	struct i915_xd9_fw_readout readout;
	int error;

	/* eDP input A always-on also names pipe A. */
	i915_host_skl_sv7(&registers);
	registers.ddi_func[I915_SKL_TRANSCODER_EDP] = 0x80000000U | (2U << 24) | (0U << 12);
	error = drv_i915_xd9_fw_decode(&registers, &readout);
	i915_host_check(error == 0 && readout.pipe == 0U, "edp input A always-on: pipe A");

	/* eDP input B moves the panel to pipe B, whose plane is then the one flipped. */
	i915_host_skl_sv7(&registers);
	registers.ddi_func[I915_SKL_TRANSCODER_EDP] = 0x80000000U | (2U << 24) | (5U << 12);
	registers.plane_ctl[1] = registers.plane_ctl[0];
	registers.plane_stride[1] = registers.plane_stride[0];
	registers.plane_size[1] = registers.plane_size[0];
	registers.plane_ctl[0] = 0U;
	error = drv_i915_xd9_fw_decode(&registers, &readout);
	i915_host_check(error == 0 && readout.pipe == 1U && readout.transcoder == I915_SKL_TRANSCODER_EDP, "edp input B: pipe B");

	/* An external display alone on transcoder B (HDMI): pipe B, refresh assumed. */
	i915_host_skl_sv7(&registers);
	registers.ddi_func[I915_SKL_TRANSCODER_EDP] = 0U;
	registers.transconf[I915_SKL_TRANSCODER_EDP] = 0U;
	registers.ddi_func[1] = 0x80000000U | (1U << 28) | (0U << 24);
	registers.transconf[1] = 0xc0000000U;
	registers.plane_ctl[1] = registers.plane_ctl[0];
	registers.plane_stride[1] = registers.plane_stride[0];
	registers.plane_size[1] = registers.plane_size[0];
	registers.plane_ctl[0] = 0U;
	error = drv_i915_xd9_fw_decode(&registers, &readout);
	i915_host_check(error == 0 && readout.pipe == 1U && readout.transcoder == 1U, "hdmi on transcoder B: pipe B");
	i915_host_check(readout.refresh_measured == 0 && readout.refresh_millihz == I915_XD9_FW_REFRESH_ASSUMED, "hdmi: refresh assumed");

	/* R G B X order. */
	i915_host_skl_sv7(&registers);
	registers.plane_ctl[0] |= I915_SKL_PLANE_CTL_ORDER_RGBX;
	error = drv_i915_xd9_fw_decode(&registers, &readout);
	i915_host_check(error == 0 && readout.rgbx == 1, "rgbx order accepted");

	/* A scaled firmware mode is flipped as it is. */
	i915_host_skl_sv7(&registers);
	registers.scaler[0][0] = I915_SKL_PS_SCALER_EN;
	error = drv_i915_xd9_fw_decode(&registers, &readout);
	i915_host_check(error == 0 && readout.scaled == 1, "scaler on: accepted, scaled");

	/* A padded stride and a surface higher up the GGTT. */
	i915_host_skl_sv7(&registers);
	registers.plane_stride[0] = 128U;
	registers.plane_surf[0] = 0x00400000U;
	registers.plane_surflive[0] = 0x00400000U;
	error = drv_i915_xd9_fw_decode(&registers, &readout);
	i915_host_check(error == 0 && readout.stride_bytes == 8192U && readout.surf == 0x00400000U, "padded stride, surface at 4 MiB");

	/* DDI A's clock not overridden in DPLL_CTRL2: refresh assumed. */
	i915_host_skl_sv7(&registers);
	registers.dpll_ctrl2 = 0U;
	error = drv_i915_xd9_fw_decode(&registers, &readout);
	i915_host_check(error == 0 && readout.refresh_measured == 0 && readout.refresh_millihz == I915_XD9_FW_REFRESH_ASSUMED, "no ddi clock override: refresh assumed");

	/* DPLL0 in HDMI mode: refresh assumed. */
	i915_host_skl_sv7(&registers);
	registers.dpll_ctrl1 |= 1U << 5;
	error = drv_i915_xd9_fw_decode(&registers, &readout);
	i915_host_check(error == 0 && readout.refresh_measured == 0, "hdmi-mode dpll: refresh assumed");

	/* A link N of zero: refresh assumed. */
	i915_host_skl_sv7(&registers);
	registers.link_n[I915_SKL_TRANSCODER_EDP] = 0U;
	error = drv_i915_xd9_fw_decode(&registers, &readout);
	i915_host_check(error == 0 && readout.refresh_measured == 0 && readout.pixel_khz == 0U, "link n 0: refresh assumed");

	/* A link M that gives an impossible refresh: assumed. */
	i915_host_skl_sv7(&registers);
	registers.link_m[I915_SKL_TRANSCODER_EDP] = 0x00ffffffU;
	error = drv_i915_xd9_fw_decode(&registers, &readout);
	i915_host_check(error == 0 && readout.refresh_measured == 0, "out-of-range refresh: assumed");

	/* Power well 2 down: only the eDP transcoder and pipe A were read, which is enough. */
	i915_host_skl_sv7(&registers);
	registers.power = 0x30000000U;
	registers.transcoder_read[0] = 0;
	registers.transcoder_read[1] = 0;
	registers.transcoder_read[2] = 0;
	registers.pipe_read[1] = 0;
	registers.pipe_read[2] = 0;
	error = drv_i915_xd9_fw_decode(&registers, &readout);
	i915_host_check(error == 0 && readout.pipe == 0U, "pw2 down: pipe A still flipped");
}

/* Checks every condition that keeps the flips from being used. */
static void
i915_host_skl_refused(void)
{
	struct i915_xd9_fw_registers registers;

	/* Power well 1 down. */
	i915_host_skl_sv7(&registers);
	registers.power = 0U;
	i915_host_skl_refusal(&registers, "pw1-off");

	/* DC5 or DC6 allowed. */
	i915_host_skl_sv7(&registers);
	registers.dc_state = 2U;
	i915_host_skl_refusal(&registers, "dc-state");

	/* The plane off. */
	i915_host_skl_sv7(&registers);
	registers.plane_ctl[0] = 0U;
	i915_host_skl_refusal(&registers, "no-lit-pipe");

	/* The transcoder enabled but not running. */
	i915_host_skl_sv7(&registers);
	registers.transconf[I915_SKL_TRANSCODER_EDP] = 0x80000000U;
	i915_host_skl_refusal(&registers, "no-lit-pipe");

	/* An eDP input select with no pipe. */
	i915_host_skl_sv7(&registers);
	registers.ddi_func[I915_SKL_TRANSCODER_EDP] = 0x80000000U | (2U << 24) | (7U << 12);
	i915_host_skl_refusal(&registers, "no-lit-pipe");

	/* The panel and an external display both lit. */
	i915_host_skl_sv7(&registers);
	registers.ddi_func[1] = 0x80000000U | (1U << 28);
	registers.transconf[1] = 0xc0000000U;
	registers.plane_ctl[1] = registers.plane_ctl[0];
	i915_host_skl_refusal(&registers, "several-lit-pipes");

	/* A 10-bit format. */
	i915_host_skl_sv7(&registers);
	registers.plane_ctl[0] = (registers.plane_ctl[0] & ~(0xfU << 24)) | (2U << 24);
	i915_host_skl_refusal(&registers, "format");

	/* An X-tiled surface. */
	i915_host_skl_sv7(&registers);
	registers.plane_ctl[0] |= 1U << 10;
	i915_host_skl_refusal(&registers, "tiled");

	/* A rotated plane. */
	i915_host_skl_sv7(&registers);
	registers.plane_ctl[0] |= 2U;
	i915_host_skl_refusal(&registers, "rotated");

	/* Asynchronous flips. */
	i915_host_skl_sv7(&registers);
	registers.plane_ctl[0] |= I915_SKL_PLANE_CTL_ASYNC_FLIP;
	i915_host_skl_refusal(&registers, "async");

	/* A panned plane. */
	i915_host_skl_sv7(&registers);
	registers.plane_offset[0] = (8U << 16) | 0U;
	i915_host_skl_refusal(&registers, "offset");

	/* A width beyond the scanout buffers' limit. */
	i915_host_skl_sv7(&registers);
	registers.plane_size[0] = ((1200U - 1U) << 16) | (8193U - 1U);
	registers.plane_stride[0] = 0xfffU;
	i915_host_skl_refusal(&registers, "size");

	/* A stride shorter than a row. */
	i915_host_skl_sv7(&registers);
	registers.plane_stride[0] = 119U;
	i915_host_skl_refusal(&registers, "stride");

	/* A surface off the 256 KiB alignment. */
	i915_host_skl_sv7(&registers);
	registers.plane_surf[0] = 0x00001000U;
	registers.plane_surflive[0] = 0x00001000U;
	i915_host_skl_refusal(&registers, "surf-align");

	/* A firmware flip that has not latched. */
	i915_host_skl_sv7(&registers);
	registers.plane_surflive[0] = 0x00040000U;
	i915_host_skl_refusal(&registers, "surf-pending");

	/* The frame buffer compressor on. */
	i915_host_skl_sv7(&registers);
	registers.fbc_ctl = I915_SKL_DPFC_CTL_EN;
	i915_host_skl_refusal(&registers, "fbc");

	/* PSR on the panel. */
	i915_host_skl_sv7(&registers);
	registers.psr_ctl = I915_SKL_EDP_PSR_ENABLE;
	i915_host_skl_refusal(&registers, "psr");
}

/* Checks that the registers are refused for the given reason. */
static void
i915_host_skl_refusal(
	const struct i915_xd9_fw_registers *registers,
	const char *reason)
{
	struct i915_xd9_fw_readout readout;
	char what[96];
	int same;
	int error;

	/* Decodes and compares the reason. */
	error = drv_i915_xd9_fw_decode(registers, &readout);
	snprintf(what, sizeof(what), "refused: %s", reason);
	same = 0;
	if (readout.reason != NULL)
		same = strcmp(readout.reason, reason) == 0;
	i915_host_check(error == EINVAL && same, what);
	if (readout.reason != NULL && !same)
		printf("  got %s\n", readout.reason);
}

/* Checks the port clocks of the link rates and the DP pixel clock. */
static void
i915_host_skl_clocks(void)
{
	/* Every link rate's port clock is twice its link clock; an unknown rate has none. */
	i915_host_check(drv_i915_xd9_dpll_rate_port_khz(I915_SKL_LINK_RATE_810) == 162000U, "port clock RBR 162 MHz");
	i915_host_check(drv_i915_xd9_dpll_rate_port_khz(I915_SKL_LINK_RATE_1350) == 270000U, "port clock HBR 270 MHz");
	i915_host_check(drv_i915_xd9_dpll_rate_port_khz(I915_SKL_LINK_RATE_2700) == 540000U, "port clock HBR2 540 MHz");
	i915_host_check(drv_i915_xd9_dpll_rate_port_khz(I915_SKL_LINK_RATE_1080) == 216000U, "port clock 216 MHz");
	i915_host_check(drv_i915_xd9_dpll_rate_port_khz(I915_SKL_LINK_RATE_1620) == 324000U, "port clock 324 MHz");
	i915_host_check(drv_i915_xd9_dpll_rate_port_khz(I915_SKL_LINK_RATE_2160) == 432000U, "port clock 432 MHz");
	i915_host_check(drv_i915_xd9_dpll_rate_port_khz(7U) == 0U, "unknown rate: no clock");

	/* The pixel clock rounds up, and a zero N gives none. */
	i915_host_check(drv_i915_xd9_dp_pixel_khz(299038U, 0x80000U, 270000U) == 154000U, "pixel 154000 kHz from m/n");
	i915_host_check(drv_i915_xd9_dp_pixel_khz(1U, 3U, 2U) == 1U, "pixel rounds up");
	i915_host_check(drv_i915_xd9_dp_pixel_khz(5U, 0U, 270000U) == 0U, "pixel with n 0: none");
}

/* Checks the display window two buffers need. */
static void
i915_host_skl_window(void)
{
	unsigned pages;

	/* 1920x1200 at 7680 bytes a row: 2250 pages, guards and slack, twice, in 32-page units. */
	pages = drv_i915_xd9_flip_window_pages(7680U * 1200U);
	i915_host_check(pages == 5312U, "window for 1920x1200: 5312 pages");
	i915_host_check((pages % 32U) == 0U, "window in 32-page units");

	/* A buffer of a page and a bit: two pages each, 804 pages in all, rounded up to 832. */
	pages = drv_i915_xd9_flip_window_pages(4097U);
	i915_host_check(pages == 832U, "window rounds pages and units up");
}
