/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the display core's start and stop of the xd display
 * (display/xd-core.c) on the register model (xd-fake-regs.c): the order
 * of an Alder Lake-P start (the south clock gate of its ADP, the PCH
 * handshake, the PHYs, power well 1, the CD clock, the DBUF, the buddy
 * of its LPDDR4 at four channels, the chicken bits and the fatal error
 * mask), a Tiger Lake start (the slices' tracker service, the ABOX
 * credits, the buddy's TLB timer, no error mask) and one with an unknown
 * memory, the faults that stop a start, and the stop.
 */

#include "host-test.h"
#include "xd-fake-regs.h"

#include "../../display/xd-core.h"
#include "../../display/xd-io.h"
#include "../../display/xd-platform.h"
#include "../../display/display-internal.h"
#include "../../intel/display-regs-xd.h"
#include "../../intel/display-regs-xd-south.h"

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The devices the checks describe: the 5330's Alder Lake-P and a Tiger Lake. */
#define I915_XD_CORE_TEST_ADLP		0x46a6U
#define I915_XD_CORE_TEST_ADLP_REVID	0x0cU
#define I915_XD_CORE_TEST_TGL		0x9a49U
#define I915_XD_CORE_TEST_TGL_REVID	0x01U

/* The caller's steps the checks see, in order: well 1 on/off, the CD clock on/off, the DBUF. */
#define I915_XD_CORE_TEST_WELL_ON	1U
#define I915_XD_CORE_TEST_WELL_OFF	2U
#define I915_XD_CORE_TEST_CDCLK_ON	3U
#define I915_XD_CORE_TEST_CDCLK_OFF	4U
#define I915_XD_CORE_TEST_DBUF		5U

/*
 * The caller's side of a run: the steps it saw (with the register writes
 * made before each), the slices asked for, and the faults to report.
 */
struct i915_xd_core_test_run {
	unsigned steps[8];
	unsigned writes_before[8];
	unsigned count;
	uint8_t slices;
	int fault_after_phys;
	int well_fails;
	unsigned faulted_asks;
};

/* The window and the run the checks share. */
static struct i915_xd_io i915_xd_core_test_io;
static struct i915_xd_core_test_run i915_xd_core_test_run;

static const struct i915_xd_platform *i915_xd_core_test_setup(uint16_t device, uint8_t revid);
static void i915_xd_core_test_note(unsigned step);
static int i915_xd_core_test_well_on(void *context);
static void i915_xd_core_test_well_off(void *context);
static void i915_xd_core_test_cdclk_on(void *context);
static void i915_xd_core_test_cdclk_off(void *context);
static void i915_xd_core_test_dbuf(void *context, uint8_t slices);
static int i915_xd_core_test_faulted(void *context);
static void i915_xd_core_test_hooks(struct i915_xd_core_hooks *hooks);
static void i915_xd_core_test_adlp(void);
static void i915_xd_core_test_tgl(void);
static void i915_xd_core_test_faults(void);
static void i915_xd_core_test_stop(void);
static void i915_xd_core_test_wa(void);

/*
 * Runs every check of the display core's start and stop and reports.
 */
int
main(void)
{
	int failures;

	/* Runs each group of checks on a fresh model. */
	i915_xd_core_test_adlp();
	i915_xd_core_test_tgl();
	i915_xd_core_test_faults();
	i915_xd_core_test_stop();
	i915_xd_core_test_wa();

	/* Reports the result. */
	failures = i915_host_report("host_xd_core_test");
	if (failures != 0)
		return 1;

	/* Succeeded: every check passed. */
	return 0;
}

/* Starts a fresh model, a window on it and an empty run; returns the device's description. */
static const struct i915_xd_platform *
i915_xd_core_test_setup(
	uint16_t device,
	uint8_t revid)
{
	struct i915_xd_io_hooks hooks;
	struct i915_xd_choice choice;
	int error;

	/* A fresh model, a window that reaches it, and no steps yet. */
	i915_xd_fake_reset();
	i915_xd_fake_hooks(&hooks);
	drv_i915_xd_io_init(&i915_xd_core_test_io, NULL, NULL);
	drv_i915_xd_io_set_hooks(&i915_xd_core_test_io, &hooks);
	memset(&i915_xd_core_test_run, 0, sizeof(i915_xd_core_test_run));

	/* The description. */
	error = drv_i915_xd_platform_choose(device, revid, 0, 0U, &choice);
	if (error != 0)
		return NULL;

	/* Succeeded: the description. */
	return choice.platform;
}

/* Notes one step of the caller, with the writes made so far. */
static void
i915_xd_core_test_note(
	unsigned step)
{
	/* A full list keeps its first steps. */
	if (i915_xd_core_test_run.count == 8U)
		return;

	/* The step. */
	i915_xd_core_test_run.steps[i915_xd_core_test_run.count] = step;
	i915_xd_core_test_run.writes_before[i915_xd_core_test_run.count] = i915_xd_fake_writes();
	i915_xd_core_test_run.count++;
}

/* Power well 1 on: refused when the run asks for it. */
static int
i915_xd_core_test_well_on(
	void *context)
{
	/* Notes the step. */
	(void)context;
	i915_xd_core_test_note(I915_XD_CORE_TEST_WELL_ON);
	if (i915_xd_core_test_run.well_fails)
		return EIO;

	/* Succeeded: the well is on. */
	return 0;
}

/* Power well 1 off. */
static void
i915_xd_core_test_well_off(
	void *context)
{
	/* Notes the step. */
	(void)context;
	i915_xd_core_test_note(I915_XD_CORE_TEST_WELL_OFF);
}

/* The CD clock up. */
static void
i915_xd_core_test_cdclk_on(
	void *context)
{
	/* Notes the step. */
	(void)context;
	i915_xd_core_test_note(I915_XD_CORE_TEST_CDCLK_ON);
}

/* The CD clock down. */
static void
i915_xd_core_test_cdclk_off(
	void *context)
{
	/* Notes the step. */
	(void)context;
	i915_xd_core_test_note(I915_XD_CORE_TEST_CDCLK_OFF);
}

/* The DBUF slices' power. */
static void
i915_xd_core_test_dbuf(
	void *context,
	uint8_t slices)
{
	/* Notes the step and the slices. */
	(void)context;
	i915_xd_core_test_note(I915_XD_CORE_TEST_DBUF);
	i915_xd_core_test_run.slices = slices;
}

/* Reports a fault after the PHYs when the run asks for it. */
static int
i915_xd_core_test_faulted(
	void *context)
{
	/* The first ask follows the PHYs. */
	(void)context;
	i915_xd_core_test_run.faulted_asks++;
	if (i915_xd_core_test_run.fault_after_phys && i915_xd_core_test_run.faulted_asks == 1U)
		return 1;

	/* Succeeded: no fault. */
	return 0;
}

/* Fills the hooks with the run's side. */
static void
i915_xd_core_test_hooks(
	struct i915_xd_core_hooks *hooks)
{
	/* The run's steps. */
	hooks->well1_on = i915_xd_core_test_well_on;
	hooks->well1_off = i915_xd_core_test_well_off;
	hooks->cdclk_on = i915_xd_core_test_cdclk_on;
	hooks->cdclk_off = i915_xd_core_test_cdclk_off;
	hooks->dbuf = i915_xd_core_test_dbuf;
	hooks->faulted = i915_xd_core_test_faulted;
	hooks->context = NULL;
}

/*
 * The 5330's start: an ADP south display, LPDDR4 at four channels; slice
 * 2 left on by the firmware.
 */
static void
i915_xd_core_test_adlp(void)
{
	const struct i915_xd_platform *platform;
	struct i915_xd_core_hooks hooks;
	struct i915_xd_core_input input;
	unsigned stop;
	unsigned phys;
	int error;

	/* Alder Lake-P. */
	platform = i915_xd_core_test_setup(I915_XD_CORE_TEST_ADLP, I915_XD_CORE_TEST_ADLP_REVID);
	if (platform == NULL) {
		i915_host_check(0, "core: the Alder Lake-P description");
		return;
	}

	/* The start. */
	i915_xd_fake_set(I915_XD_DBUF_CTL_S2, I915_XD_DBUF_POWER_STATE);
	i915_xd_fake_set(I915_XD_CHICKEN_DCPR_2, 0x1U);
	input.pch_kind = I915_PCH_ADP;
	input.step = 0U;
	input.dram_type = I915_XD_DRAM_LPDDR4;
	input.dram_channels = 4U;
	i915_xd_core_test_hooks(&hooks);
	error = drv_i915_xd_core_start(&i915_xd_core_test_io, platform, NULL, &input, &hooks, &stop, &phys);

	/* The caller's steps in order: well 1, the CD clock, the DBUF (slices 1 and 2). */
	i915_host_check(error == 0 && stop == I915_XD_CORE_DONE && i915_xd_core_test_run.count == 3U &&
			i915_xd_core_test_run.steps[0] == I915_XD_CORE_TEST_WELL_ON &&
			i915_xd_core_test_run.steps[1] == I915_XD_CORE_TEST_CDCLK_ON &&
			i915_xd_core_test_run.steps[2] == I915_XD_CORE_TEST_DBUF && i915_xd_core_test_run.slices == 0x3U,
			"core: well 1, the CD clock, then the DBUF with the firmware's slice 2 and slice 1");

	/* The south clock gate, the handshake, the buddies 0 and 1 of LPDDR4 x4, the chickens, the error mask. */
	i915_host_check((i915_xd_fake_get(I915_XD_SOUTH_DSPCLK_GATE_D) & I915_XD_SOUTH_DSPCLK_DPMG_UNGATE) != 0U &&
			(i915_xd_fake_get(I915_XD_NDE_RSTWRN_OPT) & I915_XD_NDE_RSTWRN_PCH_HANDSHAKE) != 0U &&
			i915_xd_fake_get(I915_XD_BW_BUDDY_PAGE_MASK(0U)) == 0x38U && i915_xd_fake_get(I915_XD_BW_BUDDY_PAGE_MASK(1U)) == 0x38U &&
			i915_xd_fake_get(I915_XD_BW_BUDDY_PAGE_MASK(2U)) == 0U && i915_xd_fake_get(I915_XD_BW_BUDDY_CTL(0U)) == 0U &&
			i915_xd_fake_get(I915_XD_CHICKEN_DCPR_2) == 0x0f000001U && i915_xd_fake_get(I915_XD_DISPLAY_ERR_FATAL_MASK) == 0xffffffffU,
			"core: the ADP clock gate, the handshake, the LPDDR4 x4 buddies, the chicken bits, the error mask");

	/* Display 13 sets no tracker service and no ABOX credits. */
	i915_host_check((i915_xd_fake_get(I915_XD_DBUF_CTL_S1) & I915_XD_DBUF_TRACKER_STATE_SERVICE) == 0U &&
			i915_xd_fake_get(I915_XD_MBUS_ABOX0_CTL) == 0U && i915_xd_fake_get(I915_XD_MBUS_ABOX1_CTL) == 0U,
			"core: display 13 keeps the tracker service and the ABOX credits");
}

/*
 * A Tiger Lake start: an ICP south display (no clock gate), DDR4 at two
 * channels; then one with an unknown memory.
 */
static void
i915_xd_core_test_tgl(void)
{
	const struct i915_xd_platform *platform;
	struct i915_xd_core_hooks hooks;
	struct i915_xd_core_input input;
	unsigned stop;
	unsigned phys;
	int error;

	/* Tiger Lake. */
	platform = i915_xd_core_test_setup(I915_XD_CORE_TEST_TGL, I915_XD_CORE_TEST_TGL_REVID);
	if (platform == NULL) {
		i915_host_check(0, "core: the Tiger Lake description");
		return;
	}

	/* The start. */
	input.pch_kind = I915_PCH_ICP;
	input.step = 0U;
	input.dram_type = I915_XD_DRAM_DDR4;
	input.dram_channels = 2U;
	i915_xd_core_test_hooks(&hooks);
	error = drv_i915_xd_core_start(&i915_xd_core_test_io, platform, NULL, &input, &hooks, &stop, &phys);

	/* The tracker service of 8 on two slices, the three ABOX credits, buddies 1 and 2 with the TLB timer. */
	i915_host_check(error == 0 && i915_xd_fake_get(I915_XD_SOUTH_DSPCLK_GATE_D) == 0U &&
			(i915_xd_fake_get(I915_XD_DBUF_CTL_S1) & I915_XD_DBUF_TRACKER_STATE_SERVICE) == 0x00400000U &&
			(i915_xd_fake_get(I915_XD_DBUF_CTL_S2) & I915_XD_DBUF_TRACKER_STATE_SERVICE) == 0x00400000U &&
			i915_xd_fake_get(I915_XD_DBUF_CTL_S3) == 0U &&
			i915_xd_fake_get(I915_XD_MBUS_ABOX0_CTL) == 0x00111010U && i915_xd_fake_get(I915_XD_MBUS_ABOX2_CTL) == 0x00111010U &&
			i915_xd_fake_get(I915_XD_BW_BUDDY_PAGE_MASK(1U)) == 0x1fU && i915_xd_fake_get(I915_XD_BW_BUDDY_PAGE_MASK(2U)) == 0x1fU &&
			i915_xd_fake_get(I915_XD_BW_BUDDY_PAGE_MASK(0U)) == 0U && i915_xd_fake_get(I915_XD_BW_BUDDY_CTL(1U)) == 0x00080000U &&
			i915_xd_fake_get(I915_XD_DISPLAY_ERR_FATAL_MASK) == 0U,
			"core: Tiger Lake's tracker service, ABOX credits, buddies with the TLB timer, no error mask");

	/* An unknown memory turns the buddies off. */
	platform = i915_xd_core_test_setup(I915_XD_CORE_TEST_TGL, I915_XD_CORE_TEST_TGL_REVID);
	input.dram_type = 0U;
	error = drv_i915_xd_core_start(&i915_xd_core_test_io, platform, NULL, &input, &hooks, &stop, &phys);
	i915_host_check(error == 0 && i915_xd_fake_get(I915_XD_BW_BUDDY_CTL(1U)) == I915_XD_BW_BUDDY_DISABLE &&
			i915_xd_fake_get(I915_XD_BW_BUDDY_CTL(2U)) == I915_XD_BW_BUDDY_DISABLE && i915_xd_fake_get(I915_XD_BW_BUDDY_PAGE_MASK(1U)) == 0U,
			"core: an unknown memory turns the buddies off");
}

/* The faults that stop a start: after the PHYs, and power well 1's. */
static void
i915_xd_core_test_faults(void)
{
	const struct i915_xd_platform *platform;
	struct i915_xd_core_hooks hooks;
	struct i915_xd_core_input input;
	unsigned stop;
	unsigned phys;
	int error;

	/* A fault after the PHYs: nothing of the caller's runs. */
	platform = i915_xd_core_test_setup(I915_XD_CORE_TEST_ADLP, I915_XD_CORE_TEST_ADLP_REVID);
	if (platform == NULL) {
		i915_host_check(0, "core: the Alder Lake-P description");
		return;
	}

	/* The south display, the stepping and the memory of the start. */
	input.pch_kind = I915_PCH_ADP;
	input.step = 0U;
	input.dram_type = I915_XD_DRAM_LPDDR4;
	input.dram_channels = 4U;
	i915_xd_core_test_hooks(&hooks);
	i915_xd_core_test_run.fault_after_phys = 1;
	error = drv_i915_xd_core_start(&i915_xd_core_test_io, platform, NULL, &input, &hooks, &stop, &phys);
	i915_host_check(error == EIO && stop == I915_XD_CORE_STOP_PHYS && i915_xd_core_test_run.count == 0U,
			"core: a fault after the PHYs stops the start before power well 1");

	/* Power well 1 refused: the CD clock does not start. */
	platform = i915_xd_core_test_setup(I915_XD_CORE_TEST_ADLP, I915_XD_CORE_TEST_ADLP_REVID);
	i915_xd_core_test_run.well_fails = 1;
	error = drv_i915_xd_core_start(&i915_xd_core_test_io, platform, NULL, &input, &hooks, &stop, &phys);
	i915_host_check(error == EIO && stop == I915_XD_CORE_STOP_WELL1 && i915_xd_core_test_run.count == 1U &&
			i915_xd_fake_get(I915_XD_DISPLAY_ERR_FATAL_MASK) == 0U,
			"core: a refused power well 1 stops the start");
}

/* The stop: the DBUF off, the CD clock down, power well 1 off, in that order. */
static void
i915_xd_core_test_stop(void)
{
	const struct i915_xd_platform *platform;
	struct i915_xd_core_hooks hooks;
	struct i915_xd_dc_report report;
	unsigned a_down;

	/* Alder Lake-P. */
	platform = i915_xd_core_test_setup(I915_XD_CORE_TEST_ADLP, I915_XD_CORE_TEST_ADLP_REVID);
	if (platform == NULL) {
		i915_host_check(0, "core: the Alder Lake-P description");
		return;
	}

	/* The stop. */
	i915_xd_core_test_hooks(&hooks);
	i915_xd_core_test_run.slices = 0xffU;
	drv_i915_xd_core_stop(&i915_xd_core_test_io, platform, NULL, &hooks, &report, &a_down);
	i915_host_check(i915_xd_core_test_run.count == 3U && i915_xd_core_test_run.steps[0] == I915_XD_CORE_TEST_DBUF &&
			i915_xd_core_test_run.slices == 0U && i915_xd_core_test_run.steps[1] == I915_XD_CORE_TEST_CDCLK_OFF &&
			i915_xd_core_test_run.steps[2] == I915_XD_CORE_TEST_WELL_OFF,
			"core: the stop turns the DBUF off, the CD clock down, then power well 1 off");
}

/*
 * The start's workarounds: display 13 turns the DPCE clock gate off and
 * clears the DDI clock register access chicken, writing only what
 * changes; display 12 writes the compressor's dummy pixel chicken and
 * clears the clock request policy's memory-up override.
 */
static void
i915_xd_core_test_wa(void)
{
	const struct i915_xd_platform *platform;
	unsigned writes;

	/* Display 13, from chickens the firmware left. */
	platform = i915_xd_core_test_setup(I915_XD_CORE_TEST_ADLP, I915_XD_CORE_TEST_ADLP_REVID);
	i915_xd_fake_set(I915_XD_CLKGATE_DIS_5, 0x1U);
	i915_xd_fake_set(I915_XD_CHICKEN_DCPR_1, I915_XD_DCPR_1_DDI_CLOCK_REG_ACCESS | 0x2U);
	drv_i915_xd_display_wa(&i915_xd_core_test_io, platform);
	i915_host_check(i915_xd_fake_get(I915_XD_CLKGATE_DIS_5) == (0x1U | I915_XD_CLKGATE_DIS_5_DPCE) &&
	    i915_xd_fake_get(I915_XD_CHICKEN_DCPR_1) == 0x2U && i915_xd_fake_get(I915_XD_DPFC_CHICKEN_A) == 0U,
	    "wa: display 13 gates DPCE off and clears the DDI clock register access");

	/* Applied again, nothing changes and nothing is written. */
	writes = i915_xd_fake_writes();
	drv_i915_xd_display_wa(&i915_xd_core_test_io, platform);
	i915_host_check(i915_xd_fake_writes() == writes, "wa: display 13 writes nothing that holds its value already");

	/* Display 12. */
	platform = i915_xd_core_test_setup(I915_XD_CORE_TEST_TGL, I915_XD_CORE_TEST_TGL_REVID);
	i915_xd_fake_set(I915_XD_CLKREQ_POLICY, I915_XD_CLKREQ_POLICY_MEM_UP | 0x4U);
	drv_i915_xd_display_wa(&i915_xd_core_test_io, platform);
	i915_host_check(i915_xd_fake_get(I915_XD_DPFC_CHICKEN_A) == I915_XD_DPFC_CHICKEN_DUMMY_PIXEL &&
	    i915_xd_fake_get(I915_XD_CLKREQ_POLICY) == 0x4U && i915_xd_fake_get(I915_XD_CLKGATE_DIS_5) == 0U,
	    "wa: display 12 writes the dummy pixel chicken and clears the memory-up override");
}
