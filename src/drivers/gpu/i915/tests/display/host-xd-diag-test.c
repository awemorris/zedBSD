/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the display's diagnostics (display/xd-diag.c) on the
 * register model: the stop of what is still scanned out when the device
 * goes away, and the log of what the firmware left, which writes nothing.
 */

#include "host-test.h"
#include "xd-fake-regs.h"

#include "../../display/xd-diag.h"
#include "../../display/xd-io.h"
#include "../../display/xd-platform.h"
#include "../../intel/display-regs-xd.h"

#include <stddef.h>
#include <stdint.h>

/* The 5330's Alder Lake-P. */
#define I915_XD_DIAG_TEST_ADLP		0x46a6U
#define I915_XD_DIAG_TEST_ADLP_REVID	0x0cU

/* A primary plane's word with its enable and an XRGB format. */
#define I915_XD_DIAG_TEST_PLANE_CTL	(I915_XD_PLANE_CTL_ENABLE | 0x04000000U)

static void i915_xd_diag_test_setup(struct i915_xd_io *io);
static void i915_xd_diag_test_stop_lit(void);
static void i915_xd_diag_test_stop_stuck(void);
static void i915_xd_diag_test_stop_idle(void);
static void i915_xd_diag_test_firmware_left(void);

/*
 * Runs every check of the display's diagnostics.
 */
int
main(void)
{
	int failures;

	/* Runs each group of checks on a fresh model. */
	i915_xd_diag_test_stop_lit();
	i915_xd_diag_test_stop_stuck();
	i915_xd_diag_test_stop_idle();
	i915_xd_diag_test_firmware_left();

	/* Reports the result. */
	failures = i915_host_report("host_xd_diag_test");
	if (failures != 0)
		return 1;

	/* Succeeded: every check passed. */
	return 0;
}

/* Prepares a window that reaches a fresh register model. */
static void
i915_xd_diag_test_setup(
	struct i915_xd_io *io)
{
	struct i915_xd_io_hooks hooks;

	/* Empties the model and points the window at it. */
	i915_xd_fake_reset();
	drv_i915_xd_io_init(io, NULL, NULL);
	i915_xd_fake_hooks(&hooks);
	drv_i915_xd_io_set_hooks(io, &hooks);
}

/* A lit pipe A on DDI A is stopped: plane off and armed, transcoder off and seen off, buffer off. */
static void
i915_xd_diag_test_stop_lit(void)
{
	struct i915_xd_io io;
	unsigned stopped;

	/* Pipe A scans out on DDI A; its transcoder reports its state as the model's hardware does. */
	i915_xd_diag_test_setup(&io);
	i915_xd_fake_set(I915_XD_TRANS_CONF(0U), I915_XD_TRANS_CONF_ENABLE | I915_XD_TRANS_CONF_STATE);
	i915_xd_fake_set(I915_XD_PLANE_CTL(0U), I915_XD_DIAG_TEST_PLANE_CTL);
	i915_xd_fake_set(I915_XD_PLANE_SURF(0U), 0x00100000U);
	i915_xd_fake_set(I915_XD_DDI_BUF_CTL(0U), I915_XD_DDI_BUF_ENABLE | 0x10U);
	i915_xd_fake_rule_mirror(I915_XD_TRANS_CONF(0U), I915_XD_TRANS_CONF_ENABLE, I915_XD_TRANS_CONF(0U), -1, 0U);

	/* Stops it. */
	stopped = drv_i915_xd_diag_stop_scanout(&io);

	/* The pipe and the buffer were counted and are off; the plane's surface was written to arm the plane's disable. */
	i915_host_check(stopped == 2U, "stop: the pipe and the buffer are counted");
	i915_host_check((i915_xd_fake_get(I915_XD_PLANE_CTL(0U)) & I915_XD_PLANE_CTL_ENABLE) == 0U, "stop: the plane is off");
	i915_host_check(i915_xd_fake_get(I915_XD_PLANE_SURF(0U)) == 0U, "stop: the plane's surface armed the disable");
	i915_host_check((i915_xd_fake_get(I915_XD_TRANS_CONF(0U)) & (I915_XD_TRANS_CONF_ENABLE | I915_XD_TRANS_CONF_STATE)) == 0U, "stop: the transcoder is off and reports it");
	i915_host_check(i915_xd_fake_get(I915_XD_DDI_BUF_CTL(0U)) == 0x10U, "stop: the buffer is off, its other bits kept");
	i915_host_check(i915_xd_fake_get(I915_XD_TRANS_CONF(1U)) == 0U, "stop: pipe B is left alone");
}

/* A transcoder that never reports itself off still ends the stop after the bounded wait. */
static void
i915_xd_diag_test_stop_stuck(void)
{
	struct i915_xd_io io;
	unsigned stopped;
	uint64_t before;

	/* Pipe B's transcoder keeps its state bit whatever is written. */
	i915_xd_diag_test_setup(&io);
	i915_xd_fake_set(I915_XD_TRANS_CONF(1U), I915_XD_TRANS_CONF_ENABLE | I915_XD_TRANS_CONF_STATE);
	before = i915_xd_fake_now();

	/* Stops it. */
	stopped = drv_i915_xd_diag_stop_scanout(&io);

	/* The pipe is counted, its enable is cleared, and the wait ended at its bound. */
	i915_host_check(stopped == 1U, "stuck: the pipe is counted");
	i915_host_check((i915_xd_fake_get(I915_XD_TRANS_CONF(1U)) & I915_XD_TRANS_CONF_ENABLE) == 0U, "stuck: the transcoder's enable is cleared");
	i915_host_check(i915_xd_fake_now() - before >= 200000U, "stuck: the wait ran to its bound");
	i915_host_check(i915_xd_fake_now() - before < 400000U, "stuck: the wait ended near its bound");
}

/* Nothing on: nothing is written and nothing is counted. */
static void
i915_xd_diag_test_stop_idle(void)
{
	struct i915_xd_io io;
	unsigned stopped;

	/* An idle display. */
	i915_xd_diag_test_setup(&io);

	/* Stops nothing. */
	stopped = drv_i915_xd_diag_stop_scanout(&io);
	i915_host_check(stopped == 0U, "idle: nothing is counted");
	i915_host_check(i915_xd_fake_writes() == 0U, "idle: nothing is written");
}

/* The log of what the firmware left reads and never writes, with the wells off or on, and reads nothing without a description. */
static void
i915_xd_diag_test_firmware_left(void)
{
	struct i915_xd_choice choice;
	struct i915_xd_io io;
	unsigned reads_off;
	unsigned reads_on;
	int error;

	/* The 5330's description. */
	error = drv_i915_xd_platform_choose(I915_XD_DIAG_TEST_ADLP, I915_XD_DIAG_TEST_ADLP_REVID, 0, 0U, &choice);
	i915_host_check(error == 0, "left: the 5330's description is found");
	if (error != 0)
		return;

	/* Every well off: the pipes are not read. */
	i915_xd_diag_test_setup(&io);
	drv_i915_xd_diag_firmware_left(&io, choice.platform);
	reads_off = i915_xd_fake_reads();
	i915_host_check(i915_xd_fake_writes() == 0U, "left: nothing is written with the wells off");

	/* Every well on: the lit pipes' words are read too. */
	i915_xd_diag_test_setup(&io);
	i915_xd_fake_set(I915_XD_PWR_WELL_CTL, 0xffffffffU);
	i915_xd_fake_set(I915_XD_PWR_WELL_CTL_AUX, 0xffffffffU);
	i915_xd_fake_set(I915_XD_PWR_WELL_CTL_DDI, 0xffffffffU);
	drv_i915_xd_diag_firmware_left(&io, choice.platform);
	reads_on = i915_xd_fake_reads();
	i915_host_check(i915_xd_fake_writes() == 0U, "left: nothing is written with the wells on");
	i915_host_check(reads_on >= reads_off + 3U * choice.platform->pipes, "left: each powered pipe's three words are read");

	/* No description: nothing is read. */
	i915_xd_diag_test_setup(&io);
	drv_i915_xd_diag_firmware_left(&io, NULL);
	i915_host_check(i915_xd_fake_reads() == 0U, "left: nothing is read without a description");
}
