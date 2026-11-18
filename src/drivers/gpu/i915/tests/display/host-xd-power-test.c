/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the xd power wells (display/xd-power.c) on the register
 * model (xd-fake-regs.c): the wells of the Alder Lake-P description
 * against the Latitude 5330's own list and the Linux table's facts; the
 * exact writes of power well 1 (power gate 0 before, power gate 1 after),
 * a pipe well brought up without power well 2, a Type-C AUX well's
 * routing and its PHY health hook, a Thunderbolt AUX well that does not
 * report; a drain that fails and keeps the well on; a disable while the
 * BIOS still requests the well; and the take-over of a BIOS request.
 * Above the switching, the counts: which wells power which functions,
 * the order wells come up and go down in, a well that fails to come up,
 * a well that has to stay on, the deferred give-back with its delayed
 * work, and the record of the wells that are on.  Then the DC states:
 * the verified write, DC off held and released with and without the
 * DMC firmware, and DC9's checks and writes.
 */

#include "host-test.h"
#include "xd-fake-regs.h"

#include "../../display/xd-io.h"
#include "../../display/xd-platform.h"
#include "../../display/xd-power.h"
#include "../../intel/display-regs-xd.h"
#include "../../intel/display-regs-xd-south.h"

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The Latitude 5330's GPU (Alder Lake-P) and its revision. */
#define I915_XD_POWER_TEST_DEVICE	0x46a6U
#define I915_XD_POWER_TEST_REVID	0x0cU

/*
 * What the owner's hooks saw: the pipe masks restored and drained, the
 * VGA rewrites, the Type-C ports whose PHY health was waited for, and
 * whether the next drain fails.
 */
struct i915_xd_power_test_seen {
	unsigned pipes_up;
	unsigned pipes_down;
	unsigned vga;
	unsigned health_calls;
	unsigned health_port;
	int drain_fails;
};

/*
 * What the switch of the counts saw: the wells turned on and off in
 * order (the table entry, plus 100 for an off), the well whose on fails,
 * the well whose off is refused, the work's armings with their delay,
 * and its disarmings.
 */
struct i915_xd_power_test_switch {
	unsigned order[32];
	unsigned count;
	int fail_on;
	int keep_on;
	unsigned queued;
	int last_delay;
	unsigned cancelled;
};

/* The window the checks share, reaching a fresh model each time. */
static struct i915_xd_io i915_xd_power_test_io;

/*
 * The power wells of the Latitude 5330 as its Linux debugfs lists them
 * (i915_power_domain_info of the reference capture), the always-on
 * well left out: the description must have the same wells in the same
 * order.
 */
static const char *const i915_xd_power_test_5330[] = {
	"PW_1", "DC_off", "PW_2", "PW_A", "PW_B", "PW_C", "PW_D",
	"DDI_IO_A", "DDI_IO_B", "DDI_IO_C", "DDI_IO_D", "DDI_IO_E",
	"DDI_IO_TC1", "DDI_IO_TC2", "DDI_IO_TC3", "DDI_IO_TC4",
	"AUX_A", "AUX_B", "AUX_C", "AUX_D", "AUX_E",
	"AUX_USBC1", "AUX_USBC2", "AUX_USBC3", "AUX_USBC4",
	"AUX_TBT1", "AUX_TBT2", "AUX_TBT3", "AUX_TBT4"
};

static const struct i915_xd_platform *i915_xd_power_test_setup(void);
static void i915_xd_power_test_bare(void);
static unsigned i915_xd_power_test_writes(uint32_t *offsets, uint32_t *values, unsigned max);
static void i915_xd_power_test_hooks(struct i915_xd_power_hooks *hooks, struct i915_xd_power_test_seen *seen);
static void i915_xd_power_test_pipes_up(void *context, unsigned pipe_mask);
static int i915_xd_power_test_pipes_down(void *context, unsigned pipe_mask);
static void i915_xd_power_test_vga(void *context);
static int i915_xd_power_test_health(void *context, unsigned tc_port);
static int i915_xd_power_test_well(const struct i915_xd_platform *platform, const char *name);
static void i915_xd_power_test_table(void);
static void i915_xd_power_test_main_wells(void);
static void i915_xd_power_test_aux_wells(void);
static void i915_xd_power_test_disable(void);
static void i915_xd_power_test_counts(void);
static void i915_xd_power_test_deferred(void);
static void i915_xd_power_test_switch(struct i915_xd_power_switch *sw, struct i915_xd_power_work *work, struct i915_xd_power_test_switch *seen);
static int i915_xd_power_test_on(void *context, unsigned well);
static int i915_xd_power_test_off(void *context, unsigned well);
static int i915_xd_power_test_queue(void *context, int delay_ms);
static int i915_xd_power_test_cancel(void *context, int sync);
static uint64_t i915_xd_power_test_set(const struct i915_xd_platform *platform, const char *const *names, unsigned count);
static void i915_xd_power_test_dc(void);
static void i915_xd_power_test_dc9(void);
static const struct i915_xd_platform *i915_xd_power_test_dc_setup(struct i915_xd_power *power, int firmware);
static uint32_t i915_xd_power_test_stuck(uint32_t offset, uint32_t value, void *context);

/*
 * Runs every check of the xd power wells and reports.
 */
int
main(void)
{
	int failures;

	/* Runs each group of checks. */
	i915_xd_power_test_table();
	i915_xd_power_test_main_wells();
	i915_xd_power_test_aux_wells();
	i915_xd_power_test_disable();
	i915_xd_power_test_counts();
	i915_xd_power_test_deferred();
	i915_xd_power_test_dc();
	i915_xd_power_test_dc9();

	/* Reports the result. */
	failures = i915_host_report("host_xd_power_test");
	if (failures != 0)
		return 1;

	/* Succeeded: every check passed. */
	return 0;
}

/*
 * Starts a fresh model in which every well's state follows its driver
 * request at once and every power gate's fuses are distributed, and
 * returns the Alder Lake-P description.
 */
static const struct i915_xd_platform *
i915_xd_power_test_setup(void)
{
	struct i915_xd_io_hooks hooks;
	struct i915_xd_choice choice;
	int error;

	/* A fresh model and a window that reaches it. */
	i915_xd_fake_reset();
	i915_xd_fake_hooks(&hooks);
	drv_i915_xd_io_init(&i915_xd_power_test_io, NULL, NULL);
	drv_i915_xd_io_set_hooks(&i915_xd_power_test_io, &hooks);

	/* The states of the three controls follow their requests; the fuses are in. */
	i915_xd_fake_rule_mirror(I915_XD_PWR_WELL_CTL, 0xaaaaaaaaU, I915_XD_PWR_WELL_CTL, -1, 0U);
	i915_xd_fake_rule_mirror(I915_XD_PWR_WELL_CTL_AUX, 0xaaaaaaaaU, I915_XD_PWR_WELL_CTL_AUX, -1, 0U);
	i915_xd_fake_rule_mirror(I915_XD_PWR_WELL_CTL_DDI, 0xaaaaaaaaU, I915_XD_PWR_WELL_CTL_DDI, -1, 0U);
	i915_xd_fake_set(I915_XD_FUSE_STATUS, 0xffffffffU);

	/* The description of the Latitude 5330's GPU. */
	error = drv_i915_xd_platform_choose(I915_XD_POWER_TEST_DEVICE, I915_XD_POWER_TEST_REVID, 0, 0U, &choice);
	if (error != 0)
		return NULL;

	/* Succeeded: the description. */
	return choice.platform;
}

/* Starts a fresh model whose wells never report their state. */
static void
i915_xd_power_test_bare(void)
{
	struct i915_xd_io_hooks hooks;

	/* A fresh model without rules, and the window on it. */
	i915_xd_fake_reset();
	i915_xd_fake_hooks(&hooks);
	drv_i915_xd_io_init(&i915_xd_power_test_io, NULL, NULL);
	drv_i915_xd_io_set_hooks(&i915_xd_power_test_io, &hooks);
}

/* Copies the writes of the window, oldest first; returns how many. */
static unsigned
i915_xd_power_test_writes(
	uint32_t *offsets,
	uint32_t *values,
	unsigned max)
{
	struct i915_xd_io_record records[I915_XD_IO_RECORDS];
	unsigned count;
	unsigned index;
	unsigned kept;

	/* Takes the record and keeps the writes. */
	count = drv_i915_xd_io_records(&i915_xd_power_test_io, records, I915_XD_IO_RECORDS);
	kept = 0U;
	for (index = 0U; index < count && kept < max; index++) {
		if (records[index].kind != I915_XD_IO_RECORD_WRITE)
			continue;

		/* Keeps the write. */
		offsets[kept] = records[index].offset;
		values[kept] = records[index].value;
		kept++;
	}

	/* Succeeded: the writes copied. */
	return kept;
}

/* Fills the hooks with the test's recorders. */
static void
i915_xd_power_test_hooks(
	struct i915_xd_power_hooks *hooks,
	struct i915_xd_power_test_seen *seen)
{
	/* Nothing seen yet. */
	memset(seen, 0, sizeof(*seen));

	/* The recorders. */
	hooks->pipes_up = i915_xd_power_test_pipes_up;
	hooks->pipes_down = i915_xd_power_test_pipes_down;
	hooks->vga_reset = i915_xd_power_test_vga;
	hooks->tc_health = i915_xd_power_test_health;
	hooks->tc_legacy = NULL;
	hooks->port_edp = NULL;
	hooks->context = seen;
}

/* Records the pipes whose interrupts are restored. */
static void
i915_xd_power_test_pipes_up(
	void *context,
	unsigned pipe_mask)
{
	struct i915_xd_power_test_seen *seen;

	/* Adds the pipes. */
	seen = context;
	seen->pipes_up |= pipe_mask;
}

/* Records the pipes drained, and fails the drain when asked to. */
static int
i915_xd_power_test_pipes_down(
	void *context,
	unsigned pipe_mask)
{
	struct i915_xd_power_test_seen *seen;

	/* Adds the pipes. */
	seen = context;
	seen->pipes_down |= pipe_mask;

	/* A drain the test made fail. */
	if (seen->drain_fails != 0)
		return 1;

	/* Succeeded: drained. */
	return 0;
}

/* Counts the VGA rewrites. */
static void
i915_xd_power_test_vga(
	void *context)
{
	struct i915_xd_power_test_seen *seen;

	/* Counts one. */
	seen = context;
	seen->vga++;
}

/* Records a wait for a Type-C PHY's health. */
static int
i915_xd_power_test_health(
	void *context,
	unsigned tc_port)
{
	struct i915_xd_power_test_seen *seen;

	/* Records the port. */
	seen = context;
	seen->health_calls++;
	seen->health_port = tc_port;

	/* Succeeded: the PHY is healthy. */
	return 0;
}

/* Returns the table entry of a well by its name, or -1. */
static int
i915_xd_power_test_well(
	const struct i915_xd_platform *platform,
	const char *name)
{
	unsigned well;
	int differs;

	/* Looks for the name. */
	for (well = 0U; well < platform->nwells; well++) {
		differs = strcmp(platform->wells[well].name, name);
		if (differs == 0)
			return (int)well;
	}

	/* No such well. */
	return -1;
}

/*
 * The Alder Lake-P wells: the 5330's list in order, and the facts of the
 * main wells as the Linux table has them (control index, power gate,
 * VGA, interrupt pipes).
 */
static void
i915_xd_power_test_table(void)
{
	const struct i915_xd_platform *platform;
	const struct i915_xd_well_desc *desc;
	unsigned count;
	unsigned well;
	unsigned same;
	int found;
	int differs;

	/* The wells, in the 5330's order. */
	platform = i915_xd_power_test_setup();
	i915_host_check(platform != NULL, "power: the Latitude 5330's GPU has a description");
	if (platform == NULL)
		return;
	count = sizeof(i915_xd_power_test_5330) / sizeof(i915_xd_power_test_5330[0]);
	same = 0U;
	for (well = 0U; well < count && well < platform->nwells; well++) {
		differs = strcmp(platform->wells[well].name, i915_xd_power_test_5330[well]);
		if (differs == 0)
			same++;
	}

	/* Checks the description's wells are the 5330's, in its order. */
	i915_host_check(platform->nwells == count && same == count, "power: the description's wells are the 5330's, in its order");

	/* Power well 1: index 0, power gate 1, nothing else. */
	desc = &platform->wells[0];
	i915_host_check(desc->control == I915_XD_WELL_CONTROL_MAIN && desc->index == 0U && desc->fuse_gate == 1U &&
			(desc->flags & I915_XD_WELL_VGA) == 0U && desc->irq_pipes == 0U,
			"power: PW_1 is index 0 with power gate 1");

	/* Power well 2: index 1, power gate 2, the VGA plane. */
	desc = &platform->wells[2];
	i915_host_check(desc->index == 1U && desc->fuse_gate == 2U && (desc->flags & I915_XD_WELL_VGA) != 0U &&
			desc->irq_pipes == 0U,
			"power: PW_2 is index 1 with power gate 2 and the VGA plane");

	/* The pipe wells A..D: indexes 5..8, power gates 6..9, each its pipe's interrupts. */
	same = 0U;
	for (well = 3U; well < 7U; well++) {
		desc = &platform->wells[well];
		if (desc->index == well + 2U &&
		    desc->fuse_gate == well + 3U &&
		    desc->irq_pipes == (1U << (well - 3U)))
			same++;
	}

	/* Checks PW_A..PW_D are indexes 5..8, power gates 6..9, pipe interrupts A..D. */
	i915_host_check(same == 4U, "power: PW_A..PW_D are indexes 5..8, power gates 6..9, pipe interrupts A..D");

	/* The AUX_USBC wells take 500 ms; the Thunderbolt ones are marked. */
	found = i915_xd_power_test_well(platform, "AUX_USBC1");
	i915_host_check(found >= 0 && platform->wells[found].enable_timeout_ms == 500U, "power: AUX_USBC1 may take 500 ms");
	found = i915_xd_power_test_well(platform, "AUX_TBT2");
	i915_host_check(found >= 0 && (platform->wells[found].flags & I915_XD_WELL_TBT_AUX) != 0U &&
			platform->wells[found].index == 10U,
			"power: AUX_TBT2 is the Thunderbolt AUX well at index 10");

	/* Finding by the control and the index. */
	found = drv_i915_xd_power_find(platform, I915_XD_WELL_CONTROL_MAIN, I915_XD_WELL_PW_A);
	i915_host_check(found == 3, "power: PW_A is found by its control and index");
	found = drv_i915_xd_power_find(platform, I915_XD_WELL_CONTROL_MAIN, I915_XD_WELL_PW_5);
	i915_host_check(found == -1, "power: display 13 has no PW_5");
}

/* Power well 1, power well 2 and a pipe well. */
static void
i915_xd_power_test_main_wells(void)
{
	const struct i915_xd_platform *platform;
	struct i915_xd_power_hooks hooks;
	struct i915_xd_power_test_seen seen;
	struct i915_xd_well_report report;
	uint32_t offsets[8];
	uint32_t values[8];
	unsigned count;
	int owned;
	int error;

	/* Power well 1: the FLR source bit and power gate 0 first, then the request, then power gate 1. */
	platform = i915_xd_power_test_setup();
	if (platform == NULL)
		return;
	i915_xd_power_test_hooks(&hooks, &seen);
	error = drv_i915_xd_power_well_enable(&i915_xd_power_test_io, platform, 0U, &hooks, &report);
	count = i915_xd_power_test_writes(offsets, values, 8U);
	i915_host_check(error == 0 && count == 2U &&
			offsets[0] == I915_XD_CHICKEN_DCPR_1 && values[0] == I915_XD_DCPR_1_DISABLE_FLR_SRC &&
			offsets[1] == I915_XD_PWR_WELL_CTL && values[1] == 0x2U &&
			report.state_late == 0 && report.fuses_late == 0U,
			"power: PW_1 sets the FLR source bit, then its request");
	owned = drv_i915_xd_power_well_owned(&i915_xd_power_test_io, platform, 0U);
	i915_host_check(owned == 1, "power: PW_1 is owned and on");

	/* Late fuses of power gates 0 and 1 are reported; the enable goes on. */
	platform = i915_xd_power_test_setup();
	i915_xd_fake_set(I915_XD_FUSE_STATUS, 0U);
	error = drv_i915_xd_power_well_enable(&i915_xd_power_test_io, platform, 0U, &hooks, &report);
	i915_host_check(error == 0 && report.fuses_late == 0x3U && report.state_late == 0,
			"power: late fuses of power gates 0 and 1 are reported, the well still comes up");

	/* Power well 2 rewrites the VGA registers. */
	platform = i915_xd_power_test_setup();
	i915_xd_power_test_hooks(&hooks, &seen);
	error = drv_i915_xd_power_well_enable(&i915_xd_power_test_io, platform, 2U, &hooks, &report);
	i915_host_check(error == 0 && seen.vga == 1U && seen.pipes_up == 0U, "power: PW_2 rewrites the VGA registers");

	/* Pipe A's well comes up without power well 2 and restores pipe A's interrupts. */
	platform = i915_xd_power_test_setup();
	i915_xd_power_test_hooks(&hooks, &seen);
	error = drv_i915_xd_power_well_enable(&i915_xd_power_test_io, platform, 3U, &hooks, &report);
	count = i915_xd_power_test_writes(offsets, values, 8U);
	i915_host_check(error == 0 && count == 1U && offsets[0] == I915_XD_PWR_WELL_CTL &&
			values[0] == I915_XD_PWR_WELL_REQUEST(I915_XD_WELL_PW_A) && seen.pipes_up == 0x1U && seen.vga == 0U,
			"power: PW_A comes up alone and restores pipe A's interrupts");

	/* A well whose state never comes is reported, not unwound. */
	platform = i915_xd_power_test_setup();
	i915_xd_power_test_bare();
	i915_xd_fake_set(I915_XD_FUSE_STATUS, 0xffffffffU);
	error = drv_i915_xd_power_well_enable(&i915_xd_power_test_io, platform, 3U, &hooks, &report);
	i915_host_check(error == 0 && report.state_late == 1 &&
			(i915_xd_fake_get(I915_XD_PWR_WELL_CTL) & I915_XD_PWR_WELL_REQUEST(I915_XD_WELL_PW_A)) != 0U,
			"power: a state that does not come is reported and the request stays");

	/* DC off has no request bit: refused. */
	platform = i915_xd_power_test_setup();
	error = drv_i915_xd_power_well_enable(&i915_xd_power_test_io, platform, 1U, &hooks, &report);
	i915_host_check(error == EINVAL, "power: DC_off is not switched by a request");
}

/* The AUX wells of a combo port, a Type-C port and a Thunderbolt tunnel. */
static void
i915_xd_power_test_aux_wells(void)
{
	const struct i915_xd_platform *platform;
	struct i915_xd_power_hooks hooks;
	struct i915_xd_power_test_seen seen;
	struct i915_xd_well_report report;
	uint32_t offsets[8];
	uint32_t values[8];
	unsigned count;
	int well;
	int error;

	/* AUX A: only the request. */
	platform = i915_xd_power_test_setup();
	if (platform == NULL)
		return;
	i915_xd_power_test_hooks(&hooks, &seen);
	well = i915_xd_power_test_well(platform, "AUX_A");
	error = drv_i915_xd_power_well_enable(&i915_xd_power_test_io, platform, (unsigned)well, &hooks, &report);
	count = i915_xd_power_test_writes(offsets, values, 8U);
	i915_host_check(error == 0 && count == 1U && offsets[0] == I915_XD_PWR_WELL_CTL_AUX && values[0] == 0x2U &&
			seen.health_calls == 0U,
			"power: AUX_A is only requested");

	/* AUX USBC2: the channel routed to the PHY, the request, then the PHY's health of TC2. */
	platform = i915_xd_power_test_setup();
	i915_xd_power_test_hooks(&hooks, &seen);
	i915_xd_fake_set(I915_XD_DDI_AUX_CTL(4U), 0x123U | I915_XD_AUX_CTL_TBT_IO);
	well = i915_xd_power_test_well(platform, "AUX_USBC2");
	error = drv_i915_xd_power_well_enable(&i915_xd_power_test_io, platform, (unsigned)well, &hooks, &report);
	count = i915_xd_power_test_writes(offsets, values, 8U);
	i915_host_check(error == 0 && count == 2U &&
			offsets[0] == I915_XD_DDI_AUX_CTL(4U) && values[0] == 0x123U &&
			offsets[1] == I915_XD_PWR_WELL_CTL_AUX && values[1] == I915_XD_PWR_WELL_REQUEST(4U) &&
			seen.health_calls == 1U && seen.health_port == 1U,
			"power: AUX_USBC2 routes its channel to the PHY and waits for TC2's health");

	/* AUX TBT3: the channel routed through the Thunderbolt IO; no health; a missing state is only reported. */
	platform = i915_xd_power_test_setup();
	i915_xd_power_test_bare();
	i915_xd_power_test_hooks(&hooks, &seen);
	well = i915_xd_power_test_well(platform, "AUX_TBT3");
	error = drv_i915_xd_power_well_enable(&i915_xd_power_test_io, platform, (unsigned)well, &hooks, &report);
	count = i915_xd_power_test_writes(offsets, values, 8U);
	i915_host_check(error == 0 && count == 2U &&
			offsets[0] == I915_XD_DDI_AUX_CTL(5U) && values[0] == I915_XD_AUX_CTL_TBT_IO &&
			offsets[1] == I915_XD_PWR_WELL_CTL_AUX && values[1] == I915_XD_PWR_WELL_REQUEST(11U) &&
			report.state_late == 1 && seen.health_calls == 0U,
			"power: AUX_TBT3 routes TC3's channel through the Thunderbolt IO and may not report");
}

/* Taking wells down, a failed drain, a BIOS request, and taking a BIOS request over. */
static void
i915_xd_power_test_disable(void)
{
	const struct i915_xd_platform *platform;
	struct i915_xd_power_hooks hooks;
	struct i915_xd_power_test_seen seen;
	struct i915_xd_well_report report;
	uint32_t offsets[8];
	uint32_t values[8];
	unsigned count;
	int owned;
	int error;

	/* PW_B down: pipe B drained, the request withdrawn, the state gone. */
	platform = i915_xd_power_test_setup();
	if (platform == NULL)
		return;
	i915_xd_power_test_hooks(&hooks, &seen);
	(void)drv_i915_xd_power_well_enable(&i915_xd_power_test_io, platform, 4U, &hooks, &report);
	error = drv_i915_xd_power_well_disable(&i915_xd_power_test_io, platform, 4U, &hooks, &report);
	owned = drv_i915_xd_power_well_owned(&i915_xd_power_test_io, platform, 4U);
	i915_host_check(error == 0 && seen.pipes_down == 0x2U && owned == 0 && report.state_late == 0 &&
			(i915_xd_fake_get(I915_XD_PWR_WELL_CTL) & I915_XD_PWR_WELL_STATE(I915_XD_WELL_PW_B)) == 0U,
			"power: PW_B goes down after pipe B is drained");

	/* A failed drain keeps PW_C on and its request untouched. */
	platform = i915_xd_power_test_setup();
	i915_xd_power_test_hooks(&hooks, &seen);
	(void)drv_i915_xd_power_well_enable(&i915_xd_power_test_io, platform, 5U, &hooks, &report);
	seen.drain_fails = 1;
	error = drv_i915_xd_power_well_disable(&i915_xd_power_test_io, platform, 5U, &hooks, &report);
	owned = drv_i915_xd_power_well_owned(&i915_xd_power_test_io, platform, 5U);
	i915_host_check(error == EBUSY && owned == 1, "power: a failed drain keeps PW_C on");

	/* A well the BIOS still requests: the driver's request goes, no wait for the state. */
	platform = i915_xd_power_test_setup();
	i915_xd_power_test_hooks(&hooks, &seen);
	(void)drv_i915_xd_power_well_enable(&i915_xd_power_test_io, platform, 2U, &hooks, &report);
	i915_xd_fake_set(I915_XD_PWR_WELL_CTL_BIOS, I915_XD_PWR_WELL_REQUEST(I915_XD_WELL_PW_2));
	i915_xd_fake_set(I915_XD_PWR_WELL_CTL, I915_XD_PWR_WELL_REQUEST(I915_XD_WELL_PW_2) | I915_XD_PWR_WELL_STATE(I915_XD_WELL_PW_2));
	error = drv_i915_xd_power_well_disable(&i915_xd_power_test_io, platform, 2U, &hooks, &report);
	i915_host_check(error == 0 && report.state_late == 0 &&
			(i915_xd_fake_get(I915_XD_PWR_WELL_CTL) & I915_XD_PWR_WELL_REQUEST(I915_XD_WELL_PW_2)) == 0U,
			"power: a well the BIOS requests loses only the driver's request");

	/* Taking power well 2 over from the BIOS: the driver's request first, then the BIOS's withdrawn. */
	platform = i915_xd_power_test_setup();
	i915_xd_fake_set(I915_XD_PWR_WELL_CTL_BIOS, I915_XD_PWR_WELL_REQUEST(I915_XD_WELL_PW_2) | 0x8000U);
	drv_i915_xd_power_well_adopt(&i915_xd_power_test_io, platform, 2U);
	count = i915_xd_power_test_writes(offsets, values, 8U);
	i915_host_check(count == 2U &&
			offsets[0] == I915_XD_PWR_WELL_CTL && values[0] == I915_XD_PWR_WELL_REQUEST(I915_XD_WELL_PW_2) &&
			offsets[1] == I915_XD_PWR_WELL_CTL_BIOS && values[1] == 0x8000U,
			"power: a BIOS request is taken over without a gap");

	/* Nothing to take over when the BIOS does not request the well. */
	platform = i915_xd_power_test_setup();
	drv_i915_xd_power_well_adopt(&i915_xd_power_test_io, platform, 3U);
	count = i915_xd_power_test_writes(offsets, values, 8U);
	i915_host_check(count == 0U, "power: no BIOS request, nothing written");
}

/* Checks which wells power which functions, and how the counts bring them up and down. */
static void
i915_xd_power_test_counts(void)
{
	static const char *const pipe_b[] = { "PW_2", "PW_B" };
	static const char *const pipe_c[] = { "DC_off", "PW_2", "PW_C" };
	static const char *const usbc1[] = { "DC_off", "PW_2", "AUX_USBC1" };
	static const char *const init[] = { "DC_off", "PW_2", "PW_A", "PW_B", "PW_C", "PW_D" };
	static const char *const ddi_b[] = { "DDI_IO_B" };
	static const char *const pipe_a[] = { "PW_A" };
	static struct i915_xd_power power;
	const struct i915_xd_platform *platform;
	struct i915_xd_power_switch sw;
	struct i915_xd_power_work work;
	struct i915_xd_power_test_switch seen;
	uint64_t init_functions;
	int dc_off;
	int pw_2;
	int pw_c;
	int has;
	int error;

	/* The Alder Lake-P description. */
	platform = i915_xd_power_test_setup();
	if (platform == NULL)
		return;
	dc_off = i915_xd_power_test_well(platform, "DC_off");
	pw_2 = i915_xd_power_test_well(platform, "PW_2");
	pw_c = i915_xd_power_test_well(platform, "PW_C");

	/* Which wells power what, as Linux's map and the 5330's debugfs have it. */
	init_functions = I915_XD_POWERS_PIPE(0U) | I915_XD_POWERS_PIPE(1U) | I915_XD_POWERS_PIPE(2U) | I915_XD_POWERS_PIPE(3U) | I915_XD_POWERS_DC_OFF;
	i915_host_check(drv_i915_xd_power_wells_of(platform, I915_XD_POWERS_PIPE(0U)) == i915_xd_power_test_set(platform, pipe_a, 1U),
			"power: pipe A needs PW_A alone, not PW_2");
	i915_host_check(drv_i915_xd_power_wells_of(platform, I915_XD_POWERS_PIPE(1U) | I915_XD_POWERS_TRANS(1U)) == i915_xd_power_test_set(platform, pipe_b, 2U),
			"power: pipe B needs PW_2 and PW_B");
	i915_host_check(drv_i915_xd_power_wells_of(platform, I915_XD_POWERS_PIPE(2U)) == i915_xd_power_test_set(platform, pipe_c, 3U),
			"power: pipe C needs DC off, PW_2 and PW_C");
	i915_host_check(drv_i915_xd_power_wells_of(platform, I915_XD_POWERS_DDI_IO(1U)) == i915_xd_power_test_set(platform, ddi_b, 1U),
			"power: the IO of DDI B needs its IO well alone, not PW_2");
	i915_host_check(drv_i915_xd_power_wells_of(platform, I915_XD_POWERS_AUX(3U)) == i915_xd_power_test_set(platform, usbc1, 3U),
			"power: the AUX channel of Type-C port 1 needs PW_2");
	i915_host_check(drv_i915_xd_power_wells_of(platform, init_functions) == i915_xd_power_test_set(platform, init, 6U),
			"power: the init set is DC off, PW_2 and PW_A to PW_D");
	i915_host_check(drv_i915_xd_power_wells_of(platform, I915_XD_POWERS_LANES(0U) | I915_XD_POWERS_LANES(1U)) == 0ULL,
			"power: the lanes of DDI A and B need no switched well");

	/* Pipe C: the parents first, then back in the reverse order. */
	drv_i915_xd_power_init(&power, platform);
	i915_xd_power_test_switch(&sw, &work, &seen);
	error = drv_i915_xd_power_get(&power, I915_XD_POWERS_PIPE(2U), &sw);
	i915_host_check(error == 0 && seen.count == 3U &&
			seen.order[0] == (unsigned)dc_off && seen.order[1] == (unsigned)pw_2 && seen.order[2] == (unsigned)pw_c &&
			power.refs[pw_2] == 1U,
			"power: pipe C brings up DC off, PW_2 and PW_C in that order");
	drv_i915_xd_power_put(&power, I915_XD_POWERS_PIPE(2U), &sw);
	i915_host_check(seen.count == 6U &&
			seen.order[3] == 100U + (unsigned)pw_c && seen.order[4] == 100U + (unsigned)pw_2 && seen.order[5] == 100U + (unsigned)dc_off &&
			power.refs[pw_2] == 0U,
			"power: pipe C takes them down in the reverse order");

	/* A well shared by two users stays up for the second. */
	i915_xd_power_test_switch(&sw, &work, &seen);
	(void)drv_i915_xd_power_get(&power, I915_XD_POWERS_PIPE(1U), &sw);
	(void)drv_i915_xd_power_get(&power, I915_XD_POWERS_PIPE(2U), &sw);
	drv_i915_xd_power_put(&power, I915_XD_POWERS_PIPE(2U), &sw);
	i915_host_check(power.refs[pw_2] == 1U && seen.count == 6U && seen.order[4] == 100U + (unsigned)pw_c && seen.order[5] == 100U + (unsigned)dc_off,
			"power: PW_2 stays up for pipe B when pipe C leaves");
	drv_i915_xd_power_put(&power, I915_XD_POWERS_PIPE(1U), &sw);

	/* A well that fails to come up gives back what the call took, and the error comes back. */
	i915_xd_power_test_switch(&sw, &work, &seen);
	seen.fail_on = pw_c;
	error = drv_i915_xd_power_get(&power, I915_XD_POWERS_PIPE(2U), &sw);
	i915_host_check(error == EIO && power.refs[dc_off] == 0U && power.refs[pw_2] == 0U && power.refs[pw_c] == 0U &&
			seen.count == 4U && seen.order[2] == 100U + (unsigned)pw_2 && seen.order[3] == 100U + (unsigned)dc_off,
			"power: a well that fails to come up gives back DC off and PW_2");

	/* A well that has to stay on keeps a user and is not brought up again. */
	i915_xd_power_test_switch(&sw, &work, &seen);
	(void)drv_i915_xd_power_get(&power, I915_XD_POWERS_PIPE(2U), &sw);
	seen.keep_on = pw_c;
	drv_i915_xd_power_put(&power, I915_XD_POWERS_PIPE(2U), &sw);
	seen.keep_on = -1;
	(void)drv_i915_xd_power_get(&power, I915_XD_POWERS_PIPE(2U), &sw);
	i915_host_check(power.refs[pw_c] == 2U && seen.count == 7U && seen.order[5] == (unsigned)dc_off && seen.order[6] == (unsigned)pw_2,
			"power: a well kept on keeps its user and is not switched again");

	/* A give-back of a user the well does not have is counted as an error and switches nothing. */
	drv_i915_xd_power_init(&power, platform);
	i915_xd_power_test_switch(&sw, &work, &seen);
	drv_i915_xd_power_put(&power, I915_XD_POWERS_PIPE(0U), &sw);
	i915_host_check(power.stats.errors == 1U && seen.count == 0U, "power: a give-back without a user is an error, not a switch");

	/* The record of the wells that are on answers without the counts. */
	drv_i915_xd_power_init(&power, platform);
	has = drv_i915_xd_power_has(&power, I915_XD_POWERS_PIPE(1U));
	i915_host_check(has == 0, "power: pipe B is not powered before its wells are recorded");
	drv_i915_xd_power_record(&power, (unsigned)pw_2, 1);
	has = drv_i915_xd_power_has(&power, I915_XD_POWERS_PIPE(1U));
	i915_host_check(has == 0, "power: pipe B is not powered with PW_2 alone");
	drv_i915_xd_power_record(&power, (unsigned)i915_xd_power_test_well(platform, "PW_B"), 1);
	has = drv_i915_xd_power_has(&power, I915_XD_POWERS_PIPE(1U));
	i915_host_check(has == 1, "power: pipe B is powered once PW_2 and PW_B are recorded on");
	drv_i915_xd_power_record(&power, (unsigned)pw_2, 0);
	has = drv_i915_xd_power_has(&power, I915_XD_POWERS_PIPE(1U));
	i915_host_check(has == 0, "power: pipe B is not powered once PW_2 is recorded off");
	has = drv_i915_xd_power_has(&power, 0ULL);
	i915_host_check(has == 1, "power: a function no well powers is always powered");
}

/* Checks the give-back deferred to the delayed work. */
static void
i915_xd_power_test_deferred(void)
{
	static struct i915_xd_power power;
	const struct i915_xd_platform *platform;
	struct i915_xd_power_switch sw;
	struct i915_xd_power_work work;
	struct i915_xd_power_test_switch seen;
	int usbc1;
	int pw_b;
	int owes;

	/* The Alder Lake-P description, with the work bound. */
	platform = i915_xd_power_test_setup();
	if (platform == NULL)
		return;
	usbc1 = i915_xd_power_test_well(platform, "AUX_USBC1");
	pw_b = i915_xd_power_test_well(platform, "PW_B");
	drv_i915_xd_power_init(&power, platform);
	i915_xd_power_test_switch(&sw, &work, &seen);
	drv_i915_xd_power_bind_work(&power, &work);

	/* A deferred give-back keeps the wells up and arms the work with its delay. */
	(void)drv_i915_xd_power_get(&power, I915_XD_POWERS_AUX(3U), &sw);
	drv_i915_xd_power_put_later(&power, I915_XD_POWERS_AUX(3U), 100, &sw);
	i915_host_check(power.refs[usbc1] == 1U && seen.count == 3U && seen.queued == 1U && seen.last_delay == 100 && power.work_armed == 1,
			"power: a deferred give-back keeps the wells up and arms the work");

	/* A new user takes the owed one over; the work is disarmed. */
	(void)drv_i915_xd_power_get(&power, I915_XD_POWERS_AUX(3U), &sw);
	owes = drv_i915_xd_power_owes(&power);
	i915_host_check(power.refs[usbc1] == 1U && seen.count == 3U && seen.cancelled == 1U && power.work_armed == 0 && owes == 0 &&
			power.stats.taken_over == 1U,
			"power: a get takes the owed user over without switching");

	/* Deferred while armed: the run after gives it back, with the longest delay. */
	(void)drv_i915_xd_power_get(&power, I915_XD_POWERS_PIPE(1U), &sw);
	drv_i915_xd_power_put_later(&power, I915_XD_POWERS_AUX(3U), 100, &sw);
	drv_i915_xd_power_put_later(&power, I915_XD_POWERS_PIPE(1U), 300, &sw);
	drv_i915_xd_power_work(&power, &sw);
	i915_host_check(power.refs[usbc1] == 0U && power.refs[pw_b] == 1U && seen.queued == 3U && seen.last_delay == 300 &&
			power.stats.requeues == 1U && power.work_armed == 1,
			"power: the work gives back its run and arms itself for the run after");
	drv_i915_xd_power_work(&power, &sw);
	owes = drv_i915_xd_power_owes(&power);
	i915_host_check(power.refs[pw_b] == 0U && owes == 0 && power.work_armed == 0 && seen.cancelled == 2U &&
			power.stats.released == 5U && power.stats.errors == 0U,
			"power: the run after gives the rest back and disarms the work");

	/* A run that finds nothing owed does nothing. */
	drv_i915_xd_power_work(&power, &sw);
	i915_host_check(power.stats.work_empty == 1U, "power: a run with nothing owed does nothing");

	/* A flush gives back both runs at once. */
	(void)drv_i915_xd_power_get(&power, I915_XD_POWERS_AUX(3U), &sw);
	(void)drv_i915_xd_power_get(&power, I915_XD_POWERS_PIPE(1U), &sw);
	drv_i915_xd_power_put_later(&power, I915_XD_POWERS_AUX(3U), 100, &sw);
	drv_i915_xd_power_put_later(&power, I915_XD_POWERS_PIPE(1U), 100, &sw);
	drv_i915_xd_power_flush(&power, &sw);
	owes = drv_i915_xd_power_owes(&power);
	i915_host_check(power.refs[usbc1] == 0U && power.refs[pw_b] == 0U && owes == 0 && power.work_armed == 0,
			"power: a flush gives back everything owed");

	/* Without a work a deferred give-back is immediate. */
	drv_i915_xd_power_bind_work(&power, NULL);
	(void)drv_i915_xd_power_get(&power, I915_XD_POWERS_AUX(3U), &sw);
	drv_i915_xd_power_put_later(&power, I915_XD_POWERS_AUX(3U), 100, &sw);
	i915_host_check(power.refs[usbc1] == 0U && power.work_armed == 0, "power: without a work the give-back is immediate");
}

/* Fills the switch and the work of the counts with the recording steps. */
static void
i915_xd_power_test_switch(
	struct i915_xd_power_switch *sw,
	struct i915_xd_power_work *work,
	struct i915_xd_power_test_switch *seen)
{
	/* Nothing seen yet; nothing fails or is refused. */
	memset(seen, 0, sizeof(*seen));
	seen->fail_on = -1;
	seen->keep_on = -1;

	/* The steps. */
	sw->on = i915_xd_power_test_on;
	sw->off = i915_xd_power_test_off;
	sw->context = seen;
	work->queue = i915_xd_power_test_queue;
	work->cancel = i915_xd_power_test_cancel;
	work->context = seen;
}

/* Records a well turned on; fails the one asked to fail. */
static int
i915_xd_power_test_on(
	void *context,
	unsigned well)
{
	struct i915_xd_power_test_switch *seen;

	/* Fails the well asked to fail. */
	seen = context;
	if ((int)well == seen->fail_on)
		return EIO;

	/* Records the well. */
	if (seen->count < 32U)
		seen->order[seen->count] = well;
	seen->count++;

	/* Succeeded: the well is on. */
	return 0;
}

/* Records a well turned off; refuses the one asked to stay on. */
static int
i915_xd_power_test_off(
	void *context,
	unsigned well)
{
	struct i915_xd_power_test_switch *seen;

	/* Refuses the well asked to stay on. */
	seen = context;
	if ((int)well == seen->keep_on)
		return EBUSY;

	/* Records the well. */
	if (seen->count < 32U)
		seen->order[seen->count] = 100U + well;
	seen->count++;

	/* Succeeded: the well is off. */
	return 0;
}

/* Records an arming of the work and its delay. */
static int
i915_xd_power_test_queue(
	void *context,
	int delay_ms)
{
	struct i915_xd_power_test_switch *seen;

	/* Records the arming. */
	seen = context;
	seen->queued++;
	seen->last_delay = delay_ms;

	/* Succeeded: the work was not armed. */
	return 1;
}

/* Records a disarming of the work. */
static int
i915_xd_power_test_cancel(
	void *context,
	int sync)
{
	struct i915_xd_power_test_switch *seen;

	UNUSED_PARAMETER(sync);

	/* Records the disarming. */
	seen = context;
	seen->cancelled++;

	/* Succeeded: the work was armed. */
	return 1;
}

/* Reports the set of table entries of the named wells. */
static uint64_t
i915_xd_power_test_set(
	const struct i915_xd_platform *platform,
	const char *const *names,
	unsigned count)
{
	uint64_t set;
	unsigned index;
	int well;

	/* Adds each named well. */
	set = 0ULL;
	for (index = 0U; index < count; index++) {
		well = i915_xd_power_test_well(platform, names[index]);
		if (well >= 0)
			set |= 1ULL << well;
	}

	/* Succeeded: the set. */
	return set;
}

/* Checks the DC state writes and DC off. */
static void
i915_xd_power_test_dc(void)
{
	static struct i915_xd_power power;
	struct i915_xd_dc_report report;
	uint32_t offsets[8];
	uint32_t values[8];
	unsigned count;
	int held;
	int error;
	const struct i915_xd_platform *platform;

	/* Leaving the DC states writes no DC state, keeps the other bits, and records it. */
	platform = i915_xd_power_test_dc_setup(&power, 1);
	if (platform == NULL)
		return;
	i915_xd_fake_set(I915_XD_DC_STATE_EN, 0x00100002U);
	power.dc_state = I915_XD_DC_STATE_DC6;
	drv_i915_xd_power_dc_leave(&power, &i915_xd_power_test_io, &report);
	count = i915_xd_power_test_writes(offsets, values, 8U);
	i915_host_check(count == 1U && offsets[0] == I915_XD_DC_STATE_EN && values[0] == 0x00100000U &&
			power.dc_state == 0U && power.dc_writes == 1U && report.mismatch == 0 && report.not_taken == 0,
			"power: leaving the DC states clears DC6 and keeps the other bits");

	/* DC bits that are not the ones last written are reported as a DMC that ignored a request. */
	i915_xd_fake_set(I915_XD_DC_STATE_EN, I915_XD_DC_STATE_DC5);
	drv_i915_xd_power_dc_leave(&power, &i915_xd_power_test_io, &report);
	i915_host_check(report.mismatch == 1 && report.expected == 0U && report.found == I915_XD_DC_STATE_DC5,
			"power: DC bits that were not written are reported");

	/* A state that never reads back is written again a hundred times and reported. */
	i915_xd_fake_rule_read(I915_XD_DC_STATE_EN, i915_xd_power_test_stuck, NULL);
	error = drv_i915_xd_power_dc_write(&power, &i915_xd_power_test_io, I915_XD_DC_STATE_DC6, &report);
	i915_host_check(error == EIO && report.not_taken == 1 && report.wanted == I915_XD_DC_STATE_DC6 && report.now == I915_XD_DC_STATE_DC5 &&
			power.dc_rewrites == 100U,
			"power: a DC state that never reads back is rewritten 100 times and reported");

	/* A state outside the allowed ones is reduced to them. */
	platform = i915_xd_power_test_dc_setup(&power, 1);
	if (platform == NULL)
		return;
	drv_i915_xd_power_dc_policy(&power, I915_XD_DC_STATE_DC9, I915_XD_DC_STATE_DC6, 1);
	(void)drv_i915_xd_power_dc_write(&power, &i915_xd_power_test_io, I915_XD_DC_STATE_DC6, &report);
	i915_host_check(i915_xd_fake_get(I915_XD_DC_STATE_EN) == 0U, "power: a DC state the options do not allow is not written");

	/* Without the DMC firmware DC off's release writes nothing. */
	platform = i915_xd_power_test_dc_setup(&power, 0);
	if (platform == NULL)
		return;
	error = drv_i915_xd_power_dc_enter(&power, &i915_xd_power_test_io, &report);
	count = i915_xd_power_test_writes(offsets, values, 8U);
	held = drv_i915_xd_power_dc_off_held(&power, &i915_xd_power_test_io);
	i915_host_check(error == 0 && count == 0U && held == 1, "power: without the DMC firmware the display stays out of DC states");

	/* With it DC6 is written, and DC off no longer reads as held. */
	platform = i915_xd_power_test_dc_setup(&power, 1);
	if (platform == NULL)
		return;
	error = drv_i915_xd_power_dc_enter(&power, &i915_xd_power_test_io, &report);
	count = i915_xd_power_test_writes(offsets, values, 8U);
	held = drv_i915_xd_power_dc_off_held(&power, &i915_xd_power_test_io);
	i915_host_check(error == 0 && count == 1U && values[0] == I915_XD_DC_STATE_DC6 && power.dc_state == I915_XD_DC_STATE_DC6 &&
			held == 0 && report.pwm_pin == 0 && report.dc6_already == 0,
			"power: releasing DC off with the firmware enters DC6");

	/* A utility pin driving a PWM, and DC6 requested already, are reported. */
	platform = i915_xd_power_test_dc_setup(&power, 1);
	if (platform == NULL)
		return;
	i915_xd_fake_set(I915_XD_UTIL_PIN_CTL, I915_XD_UTIL_PIN_ENABLE | I915_XD_UTIL_PIN_MODE_PWM);
	i915_xd_fake_set(I915_XD_DC_STATE_EN, I915_XD_DC_STATE_DC6);
	(void)drv_i915_xd_power_dc_enter(&power, &i915_xd_power_test_io, &report);
	i915_host_check(report.pwm_pin == 1 && report.dc6_already == 1, "power: a PWM utility pin and a DC6 already requested are reported");

	/* DC5 as the target, and the clear at the core's start that keeps the other bits and the record. */
	platform = i915_xd_power_test_dc_setup(&power, 1);
	if (platform == NULL)
		return;
	drv_i915_xd_power_dc_policy(&power, 0x4000000bU, I915_XD_DC_STATE_DC5, 1);
	(void)drv_i915_xd_power_dc_enter(&power, &i915_xd_power_test_io, &report);
	i915_host_check(i915_xd_fake_get(I915_XD_DC_STATE_EN) == I915_XD_DC_STATE_DC5, "power: a DC5 target enters DC5");
	i915_xd_fake_set(I915_XD_DC_STATE_EN, 0x40000103U);
	drv_i915_xd_power_dc_clear(&power, &i915_xd_power_test_io);
	i915_host_check(i915_xd_fake_get(I915_XD_DC_STATE_EN) == 0x00000100U && power.dc_state == I915_XD_DC_STATE_DC5,
			"power: the core's clear drops DC5, DC6 and an allowed DC3CO only");
}

/* Checks DC9's checks and writes. */
static void
i915_xd_power_test_dc9(void)
{
	static struct i915_xd_power power;
	struct i915_xd_dc_report report;
	uint32_t offsets[8];
	uint32_t values[8];
	unsigned count;
	int error;
	const struct i915_xd_platform *platform;

	/* Entering: DC9 first, then the PCH's reference clock let go. */
	platform = i915_xd_power_test_dc_setup(&power, 1);
	if (platform == NULL)
		return;
	i915_xd_fake_set(I915_XD_SOUTH_CHICKEN1, I915_XD_SOUTH_CHICKEN1_SECOND_PPS_IO);
	error = drv_i915_xd_power_dc9_enter(&power, &i915_xd_power_test_io, 0, &report);
	count = i915_xd_power_test_writes(offsets, values, 8U);
	i915_host_check(error == 0 && count == 2U &&
			offsets[0] == I915_XD_DC_STATE_EN && values[0] == I915_XD_DC_STATE_DC9 &&
			offsets[1] == I915_XD_SOUTH_CHICKEN1 && values[1] == (I915_XD_SOUTH_CHICKEN1_SECOND_PPS_IO | I915_XD_SOUTH_CHICKEN1_REFCLK_STOP),
			"power: DC9 is written, then the PCH may stop the reference clock");

	/* Entering again is refused as done already. */
	error = drv_i915_xd_power_dc9_enter(&power, &i915_xd_power_test_io, 0, &report);
	i915_host_check(error == EALREADY, "power: DC9 set already is refused");

	/* Leaving: the reference clock first, then no DC state. */
	platform = i915_xd_power_test_dc_setup(&power, 1);
	if (platform == NULL)
		return;
	i915_xd_fake_set(I915_XD_DC_STATE_EN, I915_XD_DC_STATE_DC9);
	i915_xd_fake_set(I915_XD_SOUTH_CHICKEN1, I915_XD_SOUTH_CHICKEN1_REFCLK_STOP);
	power.dc_state = I915_XD_DC_STATE_DC9;
	error = drv_i915_xd_power_dc9_leave(&power, &i915_xd_power_test_io, &report);
	count = i915_xd_power_test_writes(offsets, values, 8U);
	i915_host_check(error == 0 && count == 2U &&
			offsets[0] == I915_XD_SOUTH_CHICKEN1 && values[0] == 0U &&
			offsets[1] == I915_XD_DC_STATE_EN && values[1] == 0U && report.mismatch == 0,
			"power: leaving DC9 gives the reference clock back, then clears the state");

	/* The refusals: DC6 set, power well 2 on, the interrupts on. */
	platform = i915_xd_power_test_dc_setup(&power, 1);
	if (platform == NULL)
		return;
	i915_xd_fake_set(I915_XD_DC_STATE_EN, I915_XD_DC_STATE_DC6);
	error = drv_i915_xd_power_dc9_enter(&power, &i915_xd_power_test_io, 0, &report);
	i915_host_check(error == EBUSY && report.checked == I915_XD_DC_STATE_DC6, "power: DC9 is refused while DC6 is set");
	i915_xd_fake_set(I915_XD_DC_STATE_EN, 0U);
	i915_xd_fake_set(I915_XD_PWR_WELL_CTL, I915_XD_PWR_WELL_REQUEST(I915_XD_WELL_PW_2) | I915_XD_PWR_WELL_STATE(I915_XD_WELL_PW_2));
	error = drv_i915_xd_power_dc9_enter(&power, &i915_xd_power_test_io, 0, &report);
	i915_host_check(error == EBUSY && report.pw2_on == 1, "power: DC9 is refused while power well 2 is on");
	error = drv_i915_xd_power_dc9_leave(&power, &i915_xd_power_test_io, &report);
	i915_host_check(error == EBUSY, "power: leaving DC9 is refused while power well 2 is on");
	i915_xd_fake_set(I915_XD_PWR_WELL_CTL, 0U);
	error = drv_i915_xd_power_dc9_enter(&power, &i915_xd_power_test_io, 1, &report);
	count = i915_xd_power_test_writes(offsets, values, 8U);
	i915_host_check(error == EINVAL && count == 1U, "power: DC9 is refused while the interrupts are on, nothing more is written");
}

/*
 * Starts a fresh model and the counts of the Alder Lake-P description
 * with the policy of the 5330: DC9, DC3CO and up to DC6 allowed, DC6 the
 * target, and the DMC firmware loaded or not.
 */
static const struct i915_xd_platform *
i915_xd_power_test_dc_setup(
	struct i915_xd_power *power,
	int firmware)
{
	const struct i915_xd_platform *platform;

	/* A fresh model and the description. */
	platform = i915_xd_power_test_setup();
	if (platform == NULL)
		return NULL;

	/* The counts and the policy. */
	drv_i915_xd_power_init(power, platform);
	drv_i915_xd_power_dc_policy(power, 0x4000000aU, I915_XD_DC_STATE_DC6, firmware);

	/* Succeeded: the description. */
	return platform;
}

/* Reads DC_STATE_EN as DC5 whatever was written: a DMC that keeps its own state. */
static uint32_t
i915_xd_power_test_stuck(
	uint32_t offset,
	uint32_t value,
	void *context)
{
	UNUSED_PARAMETER(offset);
	UNUSED_PARAMETER(value);
	UNUSED_PARAMETER(context);

	/* Succeeded: the state the DMC keeps. */
	return I915_XD_DC_STATE_DC5;
}
