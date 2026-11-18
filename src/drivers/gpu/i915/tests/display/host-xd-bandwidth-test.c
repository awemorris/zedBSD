/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the memory's bandwidth for the display
 * (display/xd-bandwidth.c) on the register model, with a firmware model
 * answering the mailbox: the memory's description, the points, the
 * display's share of each point, the group a plane count takes on
 * display 11 and on display 12 and later, and the restriction that keeps
 * SAGV off.
 */

#include "host-test.h"
#include "xd-fake-regs.h"

#include "../../display/xd-bandwidth.h"
#include "../../display/xd-io.h"
#include "../../display/xd-platform.h"
#include "../../intel/display-regs-xd.h"

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The 5330's Alder Lake-P. */
#define I915_XD_BW_TEST_ADLP		0x46a6U
#define I915_XD_BW_TEST_ADLP_REVID	0x0cU

/*
 * The memory the firmware model describes: DDR4 on two channels with
 * four QGV points and three PSF points, each QGV point at 1000 units
 * (16.668 GHz in the decode's rounding) with tRP 15, tRCD 0, tRDPRE 4 and
 * tRAS 8, and PSF points at 16, 32 and 48.
 */
#define I915_XD_BW_TEST_GLOBAL		0x00003420U
#define I915_XD_BW_TEST_QGV_FIRST	0x000f03e8U
#define I915_XD_BW_TEST_QGV_SECOND	0x00000804U
#define I915_XD_BW_TEST_PSF		0x00302010U

/*
 * The firmware model: the words it answers with, the commands it
 * refuses, how many restriction requests it answers "not safe" before it
 * answers "safe" (or always, when not_safe is ~0), and what it saw.
 */
struct i915_xd_bw_test_firmware {
	uint32_t global;
	uint32_t qgv_first;
	uint32_t qgv_second;
	uint32_t psf;
	int refuse_global;
	int refuse_qgv;
	int refuse_psf;
	unsigned not_safe;
	unsigned restrict_requests;
	uint32_t restrict_mask;
	unsigned commands;
};

/* The firmware the mailbox rule answers from; every check sets it up again. */
static struct i915_xd_bw_test_firmware i915_xd_bw_test_firmware;

static const struct i915_xd_platform *i915_xd_bw_test_setup(struct i915_xd_io *io);
static void i915_xd_bw_test_answer(uint32_t offset, uint32_t value, void *context);
static void i915_xd_bw_test_table(void);
static void i915_xd_bw_test_hold(void);
static void i915_xd_bw_test_refusals(void);
static void i915_xd_bw_test_groups(void);

/*
 * Runs every check of the memory's bandwidth.
 */
int
main(void)
{
	int failures;

	/* Runs each group of checks on a fresh model. */
	i915_xd_bw_test_table();
	i915_xd_bw_test_hold();
	i915_xd_bw_test_refusals();
	i915_xd_bw_test_groups();

	/* Reports the result. */
	failures = i915_host_report("host_xd_bandwidth_test");
	if (failures != 0)
		return 1;

	/* Succeeded: every check passed. */
	return 0;
}

/* Prepares a window that reaches a fresh register model and the firmware model; returns the 5330's description. */
static const struct i915_xd_platform *
i915_xd_bw_test_setup(
	struct i915_xd_io *io)
{
	struct i915_xd_io_hooks hooks;
	struct i915_xd_choice choice;
	int error;

	/* A fresh model, a window that reaches it, and the firmware answering the mailbox. */
	i915_xd_fake_reset();
	drv_i915_xd_io_init(io, NULL, NULL);
	i915_xd_fake_hooks(&hooks);
	drv_i915_xd_io_set_hooks(io, &hooks);
	memset(&i915_xd_bw_test_firmware, 0, sizeof(i915_xd_bw_test_firmware));
	i915_xd_bw_test_firmware.global = I915_XD_BW_TEST_GLOBAL;
	i915_xd_bw_test_firmware.qgv_first = I915_XD_BW_TEST_QGV_FIRST;
	i915_xd_bw_test_firmware.qgv_second = I915_XD_BW_TEST_QGV_SECOND;
	i915_xd_bw_test_firmware.psf = I915_XD_BW_TEST_PSF;
	i915_xd_fake_rule_call(I915_XD_GTDRIVER_MAILBOX, i915_xd_bw_test_answer, &i915_xd_bw_test_firmware);

	/* The description. */
	error = drv_i915_xd_platform_choose(I915_XD_BW_TEST_ADLP, I915_XD_BW_TEST_ADLP_REVID, 0, 0U, &choice);
	if (error != 0)
		return NULL;

	/* Succeeded: the description. */
	return choice.platform;
}

/* Answers a mailbox command: the memory's description, a point, the PSF points, or a restriction. */
static void
i915_xd_bw_test_answer(
	uint32_t offset,
	uint32_t value,
	void *context)
{
	struct i915_xd_bw_test_firmware *firmware;
	uint32_t command;
	uint32_t status;

	UNUSED_PARAMETER(offset);

	/* Only a command handed over is answered. */
	firmware = context;
	if ((value & I915_XD_GTDRIVER_MAILBOX_BUSY) == 0U)
		return;

	/* Counts the command and answers it. */
	firmware->commands++;
	command = value & ~I915_XD_GTDRIVER_MAILBOX_BUSY;
	status = I915_XD_GTDRIVER_STATUS_OK;
	if (command == (I915_XD_MAILBOX_MEMORY_INFO | I915_XD_MAILBOX_MEMORY_INFO_GLOBAL)) {
		/* The memory's description. */
		i915_xd_fake_set(I915_XD_GTDRIVER_DATA0, firmware->global);
		if (firmware->refuse_global)
			status = I915_XD_GTDRIVER_STATUS_REJECTED;
	} else if (command == (I915_XD_MAILBOX_MEMORY_INFO | I915_XD_MAILBOX_MEMORY_INFO_PSF)) {
		/* The PSF points' clocks. */
		i915_xd_fake_set(I915_XD_GTDRIVER_DATA0, firmware->psf);
		if (firmware->refuse_psf)
			status = I915_XD_GTDRIVER_STATUS_REJECTED;
	} else if ((command & 0xffffU) == (I915_XD_MAILBOX_MEMORY_INFO | 0x100U)) {
		/* A QGV point (every point the same). */
		i915_xd_fake_set(I915_XD_GTDRIVER_DATA0, firmware->qgv_first);
		i915_xd_fake_set(I915_XD_GTDRIVER_DATA1, firmware->qgv_second);
		if (firmware->refuse_qgv)
			status = I915_XD_GTDRIVER_STATUS_REJECTED;
	} else if (command == I915_XD_MAILBOX_SAGV_CONFIG) {
		/* The restriction: "not safe" for the first requests, then "safe". */
		firmware->restrict_requests++;
		firmware->restrict_mask = i915_xd_fake_get(I915_XD_GTDRIVER_DATA0);
		i915_xd_fake_set(I915_XD_GTDRIVER_DATA0, 0U);
		if (firmware->restrict_requests <= firmware->not_safe)
			i915_xd_fake_set(I915_XD_GTDRIVER_DATA0, I915_XD_MAILBOX_SAGV_REPLY_QGV);
	}

	/* The answer: no busy bit, the status. */
	i915_xd_fake_set(I915_XD_GTDRIVER_MAILBOX, status);
}

/* The memory the firmware describes and the display's share of its points. */
static void
i915_xd_bw_test_table(void)
{
	const struct i915_xd_platform *platform;
	struct i915_xd_bandwidth bw;
	struct i915_xd_io io;
	unsigned index;
	int same;
	int error;

	/* Reads the memory. */
	platform = i915_xd_bw_test_setup(&io);
	i915_host_check(platform != NULL, "table: the 5330's description is found");
	error = drv_i915_xd_bw_read(&io, platform, &bw);
	i915_host_check(error == 0 && bw.dram_read, "table: the memory is read");
	i915_host_check(bw.dram_type == I915_XD_DRAM_DDR4 && bw.channels == 2U && bw.qgv_points == 4U && bw.psf_points == 3U,
	    "table: DDR4, two channels, four QGV points, three PSF points");

	/* Two planes share group 1; the other groups serve the rest. */
	i915_host_check(bw.group_planes[0] == 0U && bw.group_planes[1] == 2U && bw.group_planes[2] == 0U,
	    "table: group 1 serves up to two planes");

	/* Every point's share is capped at the system agent's 38 GB/s; the peak is two 64-bit channels at the clock. */
	same = 1;
	for (index = 0U; index < 4U; index++) {
		if (bw.derated_mbps[0][index] != 38000U || bw.peak_mbps[0][index] != 266688U)
			same = 0;
	}

	/* Checks each point's share is the 38000 MB/s cap, its peak 266688 MB/s. */
	i915_host_check(same, "table: each point's share is the 38000 MB/s cap, its peak 266688 MB/s");
	i915_host_check(bw.psf_mbps[0] == 17067U && bw.psf_mbps[1] == 34133U && bw.psf_mbps[2] == 51200U,
	    "table: the PSF points' bandwidth");
	i915_host_check(bw.table_valid && bw.sagv == I915_XD_SAGV_ON, "table: valid, SAGV switching");
	i915_host_check(drv_i915_xd_bw_point_mbps(&bw, platform, 1U, 0U) == 38000U, "table: one plane gets the point's share");
	i915_host_check(drv_i915_xd_bw_point_mbps(&bw, platform, 1U, 4U) == 0U, "table: a point not advertised has nothing");
	i915_host_check(drv_i915_xd_bw_allowed_points(&bw) == 0xfU, "table: before a restriction every point is allowed");
	i915_host_check(i915_xd_bw_test_firmware.commands == 6U,
	    "table: six mailbox commands (the memory, four points, the PSF points)");
}

/* The restriction that keeps SAGV off: the mask, the firmware's patience, a firmware that never agrees. */
static void
i915_xd_bw_test_hold(void)
{
	const struct i915_xd_platform *platform;
	struct i915_xd_bandwidth bw;
	struct i915_xd_io io;
	uint64_t before;
	int error;

	/* The firmware agrees on the third request. */
	platform = i915_xd_bw_test_setup(&io);
	(void)drv_i915_xd_bw_read(&io, platform, &bw);
	i915_xd_bw_test_firmware.not_safe = 2U;
	error = drv_i915_xd_bw_hold_sagv_off(&io, platform, &bw);
	i915_host_check(error == 0 && bw.restrict_error == 0, "hold: the restriction is accepted");
	i915_host_check(bw.kept_qgv == 0x1U && bw.kept_psf == 0x4U, "hold: the first QGV point and the fastest PSF point are kept");
	i915_host_check(bw.restriction == 0x30eU && i915_xd_bw_test_firmware.restrict_mask == 0x30eU,
	    "hold: the mask leaves every other advertised point");
	i915_host_check(i915_xd_bw_test_firmware.restrict_requests == 3U, "hold: asked again until safe");
	i915_host_check(bw.sagv == I915_XD_SAGV_OFF && drv_i915_xd_bw_allowed_points(&bw) == 0x1U, "hold: one QGV point is left: SAGV is off");

	/* A firmware that never agrees: the time is bounded and the answer reported. */
	platform = i915_xd_bw_test_setup(&io);
	(void)drv_i915_xd_bw_read(&io, platform, &bw);
	i915_xd_bw_test_firmware.not_safe = ~0U;
	before = i915_xd_fake_now();
	error = drv_i915_xd_bw_hold_sagv_off(&io, platform, &bw);
	i915_host_check(error == ETIMEDOUT && bw.restrict_error == ETIMEDOUT && bw.sagv == I915_XD_SAGV_ON,
	    "hold: a firmware that never agrees times out and SAGV stays as it was");
	i915_host_check(i915_xd_fake_now() - before >= 51000U && i915_xd_fake_now() - before < 60000U, "hold: the wait is 51 ms");

	/* A single QGV point: SAGV is the firmware's, nothing is asked. */
	platform = i915_xd_bw_test_setup(&io);
	i915_xd_bw_test_firmware.global = 0x00000120U;
	(void)drv_i915_xd_bw_read(&io, platform, &bw);
	error = drv_i915_xd_bw_hold_sagv_off(&io, platform, &bw);
	i915_host_check(error == 0 && bw.sagv == I915_XD_SAGV_NOT_CONTROLLED && i915_xd_bw_test_firmware.restrict_requests == 0U,
	    "hold: one QGV point is the firmware's: nothing asked");
}

/* Refused or unknown answers: a memory not described, a point refused, the PSF points refused. */
static void
i915_xd_bw_test_refusals(void)
{
	const struct i915_xd_platform *platform;
	struct i915_xd_bandwidth bw;
	struct i915_xd_io io;
	int error;

	/* A memory type the decode does not know: no points, SAGV taken as switching. */
	platform = i915_xd_bw_test_setup(&io);
	i915_xd_bw_test_firmware.global = 0x0000342fU;
	error = drv_i915_xd_bw_read(&io, platform, &bw);
	i915_host_check(error == 0 && !bw.dram_read && bw.qgv_points == 0U && bw.sagv == I915_XD_SAGV_ON,
	    "refusals: an unknown memory type leaves no points");

	/* A refused description: the same. */
	platform = i915_xd_bw_test_setup(&io);
	i915_xd_bw_test_firmware.refuse_global = 1;
	error = drv_i915_xd_bw_read(&io, platform, &bw);
	i915_host_check(error == 0 && !bw.dram_read && bw.qgv_points == 0U, "refusals: a refused description leaves no points");

	/* A refused point: the bandwidth is not known. */
	platform = i915_xd_bw_test_setup(&io);
	i915_xd_bw_test_firmware.refuse_qgv = 1;
	error = drv_i915_xd_bw_read(&io, platform, &bw);
	i915_host_check(error != 0 && bw.sagv == I915_XD_SAGV_UNKNOWN && !bw.table_valid, "refusals: a refused point leaves the bandwidth unknown");

	/* Refused PSF points are left out. */
	platform = i915_xd_bw_test_setup(&io);
	i915_xd_bw_test_firmware.refuse_psf = 1;
	error = drv_i915_xd_bw_read(&io, platform, &bw);
	i915_host_check(error == 0 && bw.psf_points == 0U && bw.qgv_points == 4U, "refusals: refused PSF points are left out");
}

/* The group a plane count takes: display 12 and later, and display 11. */
static void
i915_xd_bw_test_groups(void)
{
	const struct i915_xd_platform *adlp;
	struct i915_xd_platform icl;
	struct i915_xd_bandwidth bw;

	/* A made-up table: group 1 serves up to two planes at 200, group 0 the rest at 100. */
	memset(&bw, 0, sizeof(bw));
	bw.qgv_points = 1U;
	bw.group_planes[1] = 2U;
	bw.derated_mbps[0][0] = 100U;
	bw.derated_mbps[1][0] = 200U;
	adlp = drv_i915_xd_platform_row(0U);
	i915_host_check(adlp != NULL, "groups: a description is found");
	if (adlp == NULL)
		return;

	/* Display 12 and later: the fewest planes' group that covers the count, group 0 for more. */
	icl = *adlp;
	icl.display_ver = 13U;
	i915_host_check(drv_i915_xd_bw_point_mbps(&bw, &icl, 1U, 0U) == 200U, "groups: display 13, one plane takes group 1");
	i915_host_check(drv_i915_xd_bw_point_mbps(&bw, &icl, 0U, 0U) == 200U, "groups: display 13, no planes count as one");
	i915_host_check(drv_i915_xd_bw_point_mbps(&bw, &icl, 3U, 0U) == 100U, "groups: display 13, three planes take group 0");

	/* Display 11: the first group whose count the planes reach. */
	icl.display_ver = 11U;
	i915_host_check(drv_i915_xd_bw_point_mbps(&bw, &icl, 1U, 0U) == 100U, "groups: display 11, one plane takes group 0");
}
