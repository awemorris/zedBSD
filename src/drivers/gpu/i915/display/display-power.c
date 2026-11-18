/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The display's power (see display-power.h).
 *
 * The counts of the xd power wells decide which wells come up and go down;
 * this file is the owner that switches them, under the display's power
 * lock.  A well of the main, AUX or DDI controls is switched by its
 * request bit through xd-power.c, with the steps of this display around
 * it.  The DC off well is not a request bit: it is on while the display
 * is held out of the DC states, and its release lets the DMC enter the
 * state the options and the loaded program allow.  The Type-C cold block
 * of Tiger Lake is a firmware mailbox request that the counts keep without
 * a switch here.
 */

#include "internal.h"
#include "display-power.h"
#include "clock.h"
#include "vga.h"
#include "xd-core.h"
#include "xd-dbuf.h"
#include "xd-ddi.h"
#include "xd-power.h"
#include "xd-tc.h"
#include "xd.h"

#include "../intel/display-regs-xd.h"
#include "../mmio.h"
#include "../sync.h"
#include "../trace.h"

#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/lock.h>

#include <uapi/errno.h>

/* The trace stage of the display's start, which the display power and the combo PHYs record in. */
#define I915_DISPLAY_POWER_TRACE_STAGE	4U

/* The most AUX channels a display has: A to C and six Type-C channels on display 12. */
#define I915_DISPLAY_POWER_AUX_CHANNELS	9

/* The first Type-C AUX channel, and the last one of display 13, which has four. */
#define I915_DISPLAY_POWER_AUX_TC1	3
#define I915_DISPLAY_POWER_AUX_TC4_D13	6

/*
 * The DC states every display 11 to 13 may enter, and the one it enters
 * when nothing holds it out of them: DC9 around a suspend, DC3CO, and up
 * to DC6 while idle.
 *
 * Values referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_display_power.c
 * (get_allowed_dc_mask, sanitize_target_dc_state).
 */
#define I915_DISPLAY_POWER_DC_ALLOWED	(I915_XD_DC_STATE_DC9 | I915_XD_DC_STATE_DC3CO | I915_XD_DC_STATE_DC6)
#define I915_DISPLAY_POWER_DC_TARGET	I915_XD_DC_STATE_DC6

/*
 * What the hooks of one well's change act for: the display's power and
 * the well's name for the messages.  One lives on the stack of each
 * change.
 */
struct display_power_hook {
	struct i915_display_power *power;
	const char *name;
};

/*
 * The display buffer slices' control registers, slice 1 first.  The DC off
 * well reads them to check the slices the DMC left.  It never changes.
 */
static const uint32_t display_power_dbuf_controls[4] = {
	I915_XD_DBUF_CTL_S1,
	I915_XD_DBUF_CTL_S2,
	I915_XD_DBUF_CTL_S3,
	I915_XD_DBUF_CTL_S4
};

static const struct i915_xd_well_desc *display_power_desc(const struct i915_display_power *power, unsigned well);
static int display_power_well_on(struct i915_display_power *power, unsigned well);
static int display_power_well_off(struct i915_display_power *power, unsigned well);
static int display_power_well_lit(struct i915_display_power *power, unsigned well);
static int display_power_always_on(const struct i915_xd_well_desc *desc);
static void display_power_hooks(struct i915_display_power *power, const struct i915_xd_well_desc *desc, struct display_power_hook *hook, struct i915_xd_power_hooks *hooks);
static void display_power_pipes_up(void *context, unsigned pipe_mask);
static int display_power_pipes_down(void *context, unsigned pipe_mask);
static void display_power_vga_reset(void *context);
static int display_power_tc_health(void *context, unsigned tc_port);
static void display_power_switch(struct i915_display_power *power, struct i915_xd_power_switch *sw);
static int display_power_switch_on(void *context, unsigned well);
static int display_power_switch_off(void *context, unsigned well);
static struct i915_xd_power *display_power_dc_policy(struct i915_display_power *power);
static void display_power_dc_log(const struct i915_xd_dc_report *report);
static void display_power_dc_off_on(struct i915_display_power *power);
static void display_power_dc_off_off(struct i915_display_power *power);
static void display_power_combo_up(struct i915_display_power *power);
static void display_power_sync(struct i915_display_power *power);
static void display_power_dbuf_apply(struct i915_display_power *power, uint8_t slices);
static void display_power_core_hooks(struct i915_display_power *power, struct i915_xd_core_hooks *hooks);
static int display_power_core_well1_on(void *context);
static void display_power_core_well1_off(void *context);
static void display_power_core_cdclk_on(void *context);
static void display_power_core_cdclk_off(void *context);
static void display_power_core_dbuf(void *context, uint8_t slices);
static int display_power_core_faulted(void *context);
static void display_power_core_fault(struct i915_display_power *power, const char *where);

/*
 * Prepares the power of a display: the lock, the DC states it may enter,
 * the counts of its platform's wells (no user, none known to be on) and
 * the display buffer's slices, and records it in the device's trace.
 *
 * Nothing is written to the hardware.  Returns 0, or EINVAL when the
 * display has no platform description or its description has more wells
 * than the counts hold.
 */
int
drv_i915_display_power_start(
	struct i915_display_power *power,
	struct i915_xd *xd,
	struct i915_trace *trace)
{
	const struct i915_xd_platform *platform;

	/* A display without a description has no wells to count. */
	if (xd == NULL || !xd->chosen) {
		kern_logf("i915: display power: no platform description\n");
		return EINVAL;
	}

	/* Refuses a table larger than the counts hold. */
	platform = xd->choice.platform;
	if (platform->nwells > I915_XD_POWER_MAX_WELLS) {
		kern_logf("i915: display power: %u wells in the %s description\n", platform->nwells, platform->name);
		return EINVAL;
	}

	/* The lock of the counts and the wells' changes. */
	(void)mutex_init(&power->lock, LOCK_RANK_DEVICE, "i915-display-power");

	/* The DC states the display may enter and the one it enters when idle. */
	power->xd = xd;
	power->dc_allowed = I915_DISPLAY_POWER_DC_ALLOWED;
	power->dc_target = I915_DISPLAY_POWER_DC_TARGET;
	power->dmc_loaded = 0;

	/* The display buffer's slices, slice 1 first; none is known to be powered yet. */
	power->dbuf_mask = (uint8_t)((1U << platform->dbuf_slices) - 1U);
	power->dbuf_slices = 0U;

	/* No well has a user or is known to be on; no delayed work is bound. */
	drv_i915_xd_power_init(&xd->power, platform);
	power->work.queue = NULL;
	power->work.cancel = NULL;
	power->work.context = NULL;
	power->ready = 1;

	/* Records and logs what was prepared. */
	drv_i915_trace_record(trace, I915_DISPLAY_POWER_TRACE_STAGE, I915_TRACE_ACQUIRE, "display power", (uint64_t)platform->nwells, (uint64_t)power->dc_allowed);
	kern_logf("i915: display power: %u wells (%s), DC states allowed 0x%x, entered when idle 0x%x\n",
		platform->nwells,
		platform->name,
		power->dc_allowed,
		power->dc_target);

	/* Succeeded: the wells can be counted and switched. */
	return 0;
}

/*
 * Forgets the counts of a display's power.  No register is touched; the
 * wells stay as they are.
 */
void
drv_i915_display_power_stop(
	struct i915_display_power *power)
{
	/* Nothing to forget before the start. */
	if (!power->ready)
		return;

	/* Forgets the counts; the wells stay as they are. */
	power->ready = 0;
}

/*
 * Takes one user of every well that powers one of the functions: the
 * first user of a well brings it up.
 *
 * Returns 0, or the error of the well that did not come up (the wells
 * this call took are given back).
 */
int
drv_i915_display_power_get(
	struct i915_display_power *power,
	uint64_t functions)
{
	struct i915_xd_power_switch sw;
	int error;

	/* Counts the user under the lock. */
	display_power_switch(power, &sw);
	mutex_lock(&power->lock);

	error = drv_i915_xd_power_get(&power->xd->power, functions, &sw);

	mutex_unlock(&power->lock);

	/* Reports a well that did not come up. */
	if (error != 0)
		return error;

	/* Succeeded: the caller holds the functions' wells. */
	return 0;
}

/*
 * Gives one user of every well that powers one of the functions back: a
 * well whose last user leaves goes down.
 */
void
drv_i915_display_power_put(
	struct i915_display_power *power,
	uint64_t functions)
{
	struct i915_xd_power_switch sw;

	/* Gives the user back under the lock. */
	display_power_switch(power, &sw);
	mutex_lock(&power->lock);

	drv_i915_xd_power_put(&power->xd->power, functions, &sw);

	mutex_unlock(&power->lock);
}

/*
 * Gives one user of every well that powers one of the functions back
 * later: the wells stay up, owed to the delayed work, until it runs after
 * the delay or a new user takes them over.  A negative delay is the
 * default.  Without a bound work the users go back now.
 */
void
drv_i915_display_power_put_later(
	struct i915_display_power *power,
	uint64_t functions,
	int delay_ms)
{
	struct i915_xd_power_switch sw;

	/* The default delay when none is named. */
	if (delay_ms < 0)
		delay_ms = I915_DISPLAY_POWER_LATER_MS;

	/* Owes the user to the work under the lock. */
	display_power_switch(power, &sw);
	mutex_lock(&power->lock);

	power->later_puts++;
	drv_i915_xd_power_put_later(&power->xd->power, functions, delay_ms, &sw);

	mutex_unlock(&power->lock);
}

/*
 * Tells whether every well that powers one of the functions is recorded
 * as on (1 or 0).
 *
 * It answers from the record, not a fresh read, and takes no lock: the
 * interrupt handler asks before it reads a pipe's registers.
 */
int
drv_i915_display_power_is_on(
	struct i915_display_power *power,
	uint64_t functions)
{
	int powered;

	/* A display whose power never started has no well on. */
	if (power->xd == NULL)
		return 0;
	if (!power->ready)
		return 0;

	/* Asks the record of the wells whether every well of the functions is on. */
	powered = drv_i915_xd_power_has(&power->xd->power, functions);
	if (!powered)
		return 0;

	/* Succeeded: the functions are powered. */
	return 1;
}

/*
 * Binds the delayed work behind a deferred give-back; a work without a
 * queue unbinds it, and every later give-back is then immediate.
 */
void
drv_i915_display_power_bind_work(
	struct i915_display_power *power,
	const struct i915_xd_power_work *work)
{
	/* Publishes the work to the counts under the lock. */
	mutex_lock(&power->lock);

	power->work.queue = NULL;
	power->work.cancel = NULL;
	power->work.context = NULL;
	if (work != NULL)
		power->work = *work;
	drv_i915_xd_power_bind_work(&power->xd->power, &power->work);

	mutex_unlock(&power->lock);
}

/* Runs the delayed work: gives back the users owed to this run. */
void
drv_i915_display_power_work(
	struct i915_display_power *power)
{
	struct i915_xd_power_switch sw;

	/* Gives back what is owed under the lock. */
	display_power_switch(power, &sw);
	mutex_lock(&power->lock);

	drv_i915_xd_power_work(&power->xd->power, &sw);

	mutex_unlock(&power->lock);
}

/*
 * Gives back every user owed to the delayed work now and waits out a run
 * in progress; anything still owed afterwards is counted as an error of
 * the counts.
 */
void
drv_i915_display_power_flush(
	struct i915_display_power *power)
{
	struct i915_xd_power_switch sw;
	int owes;

	/* Gives back what is owed under the lock. */
	display_power_switch(power, &sw);
	mutex_lock(&power->lock);

	drv_i915_xd_power_flush(&power->xd->power, &sw);

	mutex_unlock(&power->lock);

	/* Cancels the work synchronously, outside the lock its run takes. */
	if (power->work.cancel != NULL)
		(void)power->work.cancel(power->work.context, 1);

	/* Checks that nothing is owed any more. */
	mutex_lock(&power->lock);

	owes = drv_i915_xd_power_owes(&power->xd->power);
	if (owes || power->xd->power.work_armed)
		power->xd->power.stats.errors++;

	mutex_unlock(&power->lock);
}

/*
 * Names the functions an AUX channel needs (I915_XD_POWERS_*).
 *
 * The channel numbers are those of the platform's ports: A to C, then the
 * Type-C channels from 3; display 13 gives 7 and 8 to its combo ports D
 * and E, display 12 to its Type-C ports 5 and 6, and in both the channel's
 * AUX well is the one of the same number.  Used through Thunderbolt only a
 * Type-C channel has a well (its Thunderbolt AUX well), and any other
 * channel answers 0, which the caller refuses.  An unknown channel takes
 * channel A's well.
 *
 * Values referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_display_power.c
 * (intel_aux_power_domain, intel_legacy_aux_to_power_domain,
 * intel_tbt_aux_to_power_domain).
 */
uint64_t
drv_i915_display_power_aux(
	unsigned display_ver,
	int aux_ch,
	int thunderbolt)
{
	int last_tc;

	/* A channel used directly takes its own AUX well; an unknown one takes channel A's. */
	if (!thunderbolt) {
		/* Channel A for a number outside the display's channels. */
		if (aux_ch < 0 || aux_ch >= I915_DISPLAY_POWER_AUX_CHANNELS)
			return I915_XD_POWERS_AUX(0U);

		/* Succeeded: the channel's AUX well. */
		return I915_XD_POWERS_AUX((unsigned)aux_ch);
	}

	/* Display 13 has four Type-C channels, display 12 six. */
	last_tc = I915_DISPLAY_POWER_AUX_CHANNELS - 1;
	if (display_ver >= 13U)
		last_tc = I915_DISPLAY_POWER_AUX_TC4_D13;

	/* A channel that is not Type-C has no Thunderbolt well. */
	if (aux_ch < I915_DISPLAY_POWER_AUX_TC1 || aux_ch > last_tc)
		return 0ULL;

	/* Succeeded: the Type-C port's Thunderbolt AUX well. */
	return I915_XD_POWERS_TBT_AUX((unsigned)(aux_ch - I915_DISPLAY_POWER_AUX_TC1));
}

/*
 * Turns off every well the firmware left on that nothing uses, the last
 * well of the table first, each read afresh.  Power well 1, which the
 * display core holds, and the firmware's Type-C cold block are left.
 * Returns how many wells were turned off.
 */
unsigned
drv_i915_display_power_sanitize(
	struct i915_display_power *power)
{
	const struct i915_xd_platform *platform;
	const struct i915_xd_well_desc *desc;
	unsigned well;
	unsigned turned_off;
	int always;
	int lit;

	/* Walks the wells from the last. */
	platform = power->xd->choice.platform;
	turned_off = 0U;
	well = platform->nwells;
	while (well > 0U) {
		/* Power well 1 and the mailbox block stay as they are. */
		well--;
		desc = display_power_desc(power, well);
		always = display_power_always_on(desc);
		if (desc->control == I915_XD_WELL_CONTROL_MAILBOX)
			continue;
		if (always)
			continue;

		/* A well with a user stays on. */
		if (power->xd->power.refs[well] != 0U)
			continue;

		/* A well that is off needs nothing. */
		lit = display_power_well_lit(power, well);
		if (!lit)
			continue;

		/* Turns the unused well off. */
		kern_logf("i915: display power: well %s was left on by the firmware and is unused: turned off\n", desc->name);
		(void)display_power_well_off(power, well);
		turned_off++;
	}

	/* Succeeded: the wells turned off. */
	return turned_off;
}

/* Counts the wells that are on as the driver owns them (the mailbox block is not counted). */
unsigned
drv_i915_display_power_wells_on(
	struct i915_display_power *power)
{
	const struct i915_xd_platform *platform;
	const struct i915_xd_well_desc *desc;
	unsigned well;
	unsigned count;
	int lit;

	/* Asks every switched well. */
	platform = power->xd->choice.platform;
	count = 0U;
	for (well = 0U; well < platform->nwells; well++) {
		/* The mailbox block is not a well the driver reads. */
		desc = display_power_desc(power, well);
		if (desc->control == I915_XD_WELL_CONTROL_MAILBOX)
			continue;

		/* Counts a well the driver owns on. */
		lit = display_power_well_lit(power, well);
		if (lit)
			count++;
	}

	/* Succeeded: the wells on. */
	return count;
}

/*
 * Compares each well's users with its state in the hardware: a well with
 * a user (and power well 1, which the core holds) is expected on, any
 * other off.  Logs each well that disagrees and returns how many did.
 */
unsigned
drv_i915_display_power_check(
	struct i915_display_power *power)
{
	const struct i915_xd_platform *platform;
	const struct i915_xd_well_desc *desc;
	unsigned well;
	unsigned mismatches;
	unsigned users;
	int expect;
	int always;
	int lit;

	/* Compares every switched well. */
	platform = power->xd->choice.platform;
	mismatches = 0U;
	for (well = 0U; well < platform->nwells; well++) {
		/* The mailbox block has no state to read. */
		desc = display_power_desc(power, well);
		if (desc->control == I915_XD_WELL_CONTROL_MAILBOX)
			continue;

		/* A well with users, or power well 1, is expected on. */
		users = power->xd->power.refs[well];
		always = display_power_always_on(desc);
		expect = 0;
		if (users != 0U)
			expect = 1;
		if (always)
			expect = 1;

		/* Reports a well whose state does not follow its users. */
		lit = display_power_well_lit(power, well);
		if (expect != lit) {
			kern_logf("i915: display power: well %s reads %s with %u users\n", desc->name, lit ? "on" : "off", users);
			mismatches++;
		}
	}

	/* Succeeded: how many wells disagreed. */
	return mismatches;
}

/* Logs every well that has users or is on, and the DC state. */
void
drv_i915_display_power_log(
	struct i915_display_power *power)
{
	const struct i915_xd_platform *platform;
	const struct i915_xd_well_desc *desc;
	unsigned well;
	unsigned users;
	uint32_t dc_state;
	int lit;

	/* Names every switched well that has users or is on. */
	platform = power->xd->choice.platform;
	for (well = 0U; well < platform->nwells; well++) {
		/* The mailbox block is not read. */
		desc = display_power_desc(power, well);
		if (desc->control == I915_XD_WELL_CONTROL_MAILBOX)
			continue;

		/* A well with neither is not named. */
		users = power->xd->power.refs[well];
		lit = display_power_well_lit(power, well);
		if (users != 0U || lit)
			kern_logf("i915: display power: well %s: %u users, %s\n", desc->name, users, lit ? "on" : "off");
	}

	/* The DC state and what its changes met. */
	dc_state = drv_i915_xd_io_read(&power->xd->io, I915_XD_DC_STATE_EN);
	kern_logf("i915: display power: DC_STATE_EN=0x%08x, DC off released %u times, DC state writes %u (rewrites %u), DMC program %s\n",
		dc_state,
		power->dc_off_off,
		power->xd->power.dc_writes,
		power->xd->power.dc_rewrites,
		power->dmc_loaded ? "loaded" : "not loaded");
}

/* Powers the display buffer slices asked for, within the display's, and the others off. */
void
drv_i915_display_power_dbuf(
	struct i915_display_power *power,
	uint8_t slices)
{
	/* Keeps the request within the display's slices and applies it. */
	display_power_dbuf_apply(power, (uint8_t)(slices & power->dbuf_mask));
}

/*
 * Starts the display core through xd-core.c (the DC states the firmware
 * left cleared, the PCH handshake, the combo PHYs, power well 1, the CD
 * clock, the display buffer, the memory bus credits, the bandwidth
 * buddies, the DC request bits), then takes the start's reference and
 * reads every well's state, taking a well the firmware requested over.
 *
 * The CD clock (power->cdclk) and the trace ring of the combo PHYs
 * (power->trace) are set by the caller.  Returns 0, or EIO
 * when a fault of the register access stopped the start: core_fault then
 * names the step, and no reference is taken.
 */
int
drv_i915_display_power_core_start(
	struct i915_display_power *power,
	unsigned dram_type,
	unsigned dram_channels)
{
	struct i915_xd_core_hooks hooks;
	struct i915_xd_core_input input;
	struct i915_xd_power *policy;
	struct i915_xd *xd;
	unsigned stop;
	unsigned phys;
	unsigned count;
	int error;

	/* The trace ring of the combo PHYs' start begins empty. */
	xd = power->xd;
	drv_i915_trace_init(power->trace);

	/* The south display, the stepping and the memory. */
	input.pch_kind = xd->choice.pch_kind;
	input.step = xd->choice.step;
	input.dram_type = dram_type;
	input.dram_channels = dram_channels;

	/* The core's start, with power well 1, the CD clock and the display buffer's slices here. */
	display_power_core_hooks(power, &hooks);
	policy = display_power_dc_policy(power);
	error = drv_i915_xd_core_start(&xd->io, xd->choice.platform, policy, &input, &hooks, &stop, &phys);

	/* Records and logs the combo PHYs brought up. */
	count = (unsigned)__builtin_popcount(phys);
	drv_i915_trace_record(power->trace, I915_DISPLAY_POWER_TRACE_STAGE, I915_TRACE_ACQUIRE, "combo PHYs", (uint64_t)count, 0U);
	kern_logf("i915: display power: combo PHYs up: %u (mask 0x%x)\n", count, phys);

	/* Reports where a fault stopped the start. */
	if (error != 0) {
		if (stop == I915_XD_CORE_STOP_PHYS) {
			display_power_core_fault(power, "combo PHYs");
		} else if (stop == I915_XD_CORE_STOP_WELL1) {
			display_power_core_fault(power, "power well 1");
		} else {
			display_power_core_fault(power, "CD clock");
		}

		/* Reports the fault; no reference is taken. */
		return EIO;
	}

	/* Keeps every pipe's wells and the exit from the DC states until the display is served. */
	(void)drv_i915_display_power_get(power, I915_DISPLAY_POWER_START);
	power->start_held = 1;

	/* Reads every well's state, taking over what the firmware requested. */
	mutex_lock(&power->lock);

	display_power_sync(power);

	mutex_unlock(&power->lock);

	/* Succeeded: the display core is up. */
	return 0;
}

/*
 * Takes the display core down for a suspend.
 *
 * The caller holds the start's reference again and has stopped every
 * output.  The start's reference goes back, so that every well nothing
 * else uses turns off; the deferred give-backs are finished and the wells
 * checked against their users; then the core goes down through xd-core.c
 * (the DC states left, the display buffer off, the CD clock down, power
 * well 1 off, the combo PHYs down).  The resume's core start brings it up
 * again.
 */
void
drv_i915_display_power_core_stop(
	struct i915_display_power *power)
{
	struct i915_xd_core_hooks hooks;
	struct i915_xd_dc_report report;
	struct i915_xd_power *policy;
	unsigned phy_a_down;

	/* Gives the start's reference back. */
	drv_i915_display_power_release(power);

	/* Finishes the deferred give-backs and checks every well against its users. */
	drv_i915_display_power_flush(power);
	(void)drv_i915_display_power_check(power);

	/* Takes the core down. */
	display_power_core_hooks(power, &hooks);
	policy = display_power_dc_policy(power);
	drv_i915_xd_core_stop(&power->xd->io, power->xd->choice.platform, policy, &hooks, &report, &phy_a_down);

	/* Logs the DC states left and the PHYs down. */
	display_power_dc_log(&report);
	if (phy_a_down != 0U)
		kern_logf("i915: display power: combo PHY A was not up before the stop\n");
	kern_logf("i915: display power: combo PHYs down\n");

	/* The resume starts the core again. */
	power->core_suspended = 1;
}

/*
 * Takes the start's reference again: every pipe's wells and the exit
 * from the DC states stay up from here (a suspend, the display's stop).
 * Then checks the wells against their users.
 */
void
drv_i915_display_power_hold(
	struct i915_display_power *power)
{
	/* A held reference is not taken twice. */
	if (!power->start_held) {
		(void)drv_i915_display_power_get(power, I915_DISPLAY_POWER_START);
		power->start_held = 1;
	}

	/* Checks the wells against their users. */
	(void)drv_i915_display_power_check(power);
}

/*
 * Gives the start's reference back, so that every well nothing else uses
 * turns off, the DC off well last, and the display may enter DC6.
 */
void
drv_i915_display_power_release(
	struct i915_display_power *power)
{
	/* Nothing is held. */
	if (!power->start_held)
		return;

	/* Gives the reference back. */
	power->start_held = 0;
	drv_i915_display_power_put(power, I915_DISPLAY_POWER_START);
}

/*
 * Gives the start's reference back when the display is served, and
 * records in the probe state the wells on before and after, the DC state
 * last written and the wells that disagree with their users.
 */
void
drv_i915_display_power_serve(
	struct i915_display_power *power,
	struct i915_driver_probe *probe)
{
	/* Counts the wells on before the give-back. */
	probe->wells_on_before = drv_i915_display_power_wells_on(power);

	/* Gives the start's reference back. */
	drv_i915_display_power_release(power);

	/* Records what the give-back left. */
	probe->wells_on_after = drv_i915_display_power_wells_on(power);
	probe->dc_state_after = (int)power->xd->power.dc_state;
	probe->verify_mismatches = drv_i915_display_power_check(power);
}

/*
 * Puts the display in DC9 for a suspend, with the PCH let stop the
 * display's reference clock.
 *
 * The display core must be down and the interrupts off.  Returns 0, or
 * the refusal of xd-power.c's check, nothing written.
 */
int
drv_i915_display_power_dc9_enter(
	struct i915_display_power *power,
	int irqs_enabled)
{
	struct i915_xd_dc_report report;
	struct i915_xd_power *policy;
	int error;

	/* Enters DC9 when the checks allow it. */
	policy = display_power_dc_policy(power);
	error = drv_i915_xd_power_dc9_enter(policy, &power->xd->io, irqs_enabled, &report);
	display_power_dc_log(&report);
	if (error != 0) {
		kern_logf("i915: display power: DC9 not entered (%d; DC_STATE_EN 0x%x, power well 2 %d, interrupts %d)\n",
			error,
			report.checked,
			report.pw2_on,
			irqs_enabled);
		return error;
	}

	/* Succeeded: the display is in DC9. */
	kern_logf("i915: display power: DC9 entered\n");
	return 0;
}

/*
 * Leaves DC9 after a suspend: the PCH's reference clock first, then the
 * DC state.  Returns 0, or the refusal of xd-power.c's check.
 */
int
drv_i915_display_power_dc9_leave(
	struct i915_display_power *power)
{
	struct i915_xd_dc_report report;
	struct i915_xd_power *policy;
	int error;

	/* Leaves DC9 when the checks allow it. */
	policy = display_power_dc_policy(power);
	error = drv_i915_xd_power_dc9_leave(policy, &power->xd->io, &report);
	display_power_dc_log(&report);
	if (error != 0) {
		kern_logf("i915: display power: DC9 not left (%d; DC_STATE_EN 0x%x, power well 2 %d)\n",
			error,
			report.checked,
			report.pw2_on);
		return error;
	}

	/* Succeeded: the display left DC9. */
	kern_logf("i915: display power: DC9 left\n");
	return 0;
}

/* Reports one well's row of the platform's table. */
static const struct i915_xd_well_desc *
display_power_desc(
	const struct i915_display_power *power,
	unsigned well)
{
	/* The row of the table. */
	return &power->xd->choice.platform->wells[well];
}

/* Tells whether a well is power well 1, which belongs to the display core and the DMC and no user turns off. */
static int
display_power_always_on(
	const struct i915_xd_well_desc *desc)
{
	/* Only the main control's power well 1. */
	if (desc->control != I915_XD_WELL_CONTROL_MAIN)
		return 0;
	if (desc->index != I915_XD_WELL_PW_1)
		return 0;

	/* Succeeded: power well 1. */
	return 1;
}

/*
 * Turns a well on and records it on.
 *
 * The DC off well leaves the DC states.  Any other well comes up by its
 * request through xd-power.c with this display's hooks; fuses or a state
 * that come late are logged and the well counts as on.  Returns 0, or EIO
 * when the time base failed.
 */
static int
display_power_well_on(
	struct i915_display_power *power,
	unsigned well)
{
	const struct i915_xd_well_desc *desc;
	struct display_power_hook hook;
	struct i915_xd_power_hooks hooks;
	struct i915_xd_well_report report;
	unsigned gate;
	int error;

	/* The DC off well leaves the DC states. */
	desc = display_power_desc(power, well);
	if (desc->control == I915_XD_WELL_CONTROL_DC_OFF) {
		display_power_dc_off_on(power);
		drv_i915_xd_power_record(&power->xd->power, well, 1);
		return 0;
	}

	/* Brings the well up. */
	display_power_hooks(power, desc, &hook, &hooks);
	error = drv_i915_xd_power_well_enable(&power->xd->io, power->xd->choice.platform, well, &hooks, &report);
	if (error != 0)
		return EIO;

	/* Logs the power gates whose fuses came late; the well still comes up. */
	for (gate = 0U; gate < 32U; gate++) {
		if ((report.fuses_late & (1U << gate)) != 0U)
			kern_logf("i915: display power: well %s: the fuses of power gate %u came late\n", desc->name, gate);
	}

	/*
	 * A state that comes late is logged and the well counts as on: a
	 * Type-C AUX well reports late when its Thunderbolt tunnel is down, or
	 * while the TC cold exit it starts is still running.
	 */
	if (report.state_late) {
		power->late_states++;
		kern_logf("i915: display power: well %s: its state came late\n", desc->name);
	}

	/* Succeeded: the well is on. */
	drv_i915_xd_power_record(&power->xd->power, well, 1);
	return 0;
}

/*
 * Turns a well off and records what it reads afterwards.
 *
 * Once a pipe's interrupts failed to drain, every well off is refused.
 * The DC off well lets the DMC enter the target DC state.  Returns 0,
 * EBUSY when the well stays on (a refusal, or its pipes' interrupts did
 * not drain now), or EIO when the time base failed.
 */
static int
display_power_well_off(
	struct i915_display_power *power,
	unsigned well)
{
	const struct i915_xd_well_desc *desc;
	struct display_power_hook hook;
	struct i915_xd_power_hooks hooks;
	struct i915_xd_well_report report;
	int lit;
	int error;

	/* A failed drain refuses every later well off. */
	if (power->irq_sync_failed) {
		power->refusals++;
		return EBUSY;
	}

	/* The DC off well lets the DMC take the display into its DC state. */
	desc = display_power_desc(power, well);
	if (desc->control == I915_XD_WELL_CONTROL_DC_OFF) {
		display_power_dc_off_off(power);

		/* Records whether the display is still held out of the DC states. */
		lit = display_power_well_lit(power, well);
		drv_i915_xd_power_record(&power->xd->power, well, lit);
		return 0;
	}

	/* Takes the well down; the pipes' interrupts drain first, and a failed drain keeps it on. */
	display_power_hooks(power, desc, &hook, &hooks);
	error = drv_i915_xd_power_well_disable(&power->xd->io, power->xd->choice.platform, well, &hooks, &report);
	if (error == EBUSY) {
		power->refusals++;
		return EBUSY;
	}

	/* Reports a fault of the time base. */
	if (error != 0)
		return EIO;

	/* Records whether the driver still owns the well on. */
	lit = display_power_well_lit(power, well);
	drv_i915_xd_power_record(&power->xd->power, well, lit);

	/* Succeeded: the driver's request is gone. */
	return 0;
}

/*
 * Tells whether a well is on as the driver owns it (1 or 0): the DC off
 * well while the display is held out of the DC states, any other well
 * while both the driver's request and the state are set.
 */
static int
display_power_well_lit(
	struct i915_display_power *power,
	unsigned well)
{
	const struct i915_xd_well_desc *desc;
	struct i915_xd_power *policy;
	int held;
	int owned;

	/* The DC off well is on while no DC state is set. */
	desc = display_power_desc(power, well);
	if (desc->control == I915_XD_WELL_CONTROL_DC_OFF) {
		policy = display_power_dc_policy(power);
		held = drv_i915_xd_power_dc_off_held(policy, &power->xd->io);
		if (!held)
			return 0;

		/* Succeeded: the display is held out of the DC states. */
		return 1;
	}

	/* Any other well needs the driver's request and the state. */
	owned = drv_i915_xd_power_well_owned(&power->xd->io, power->xd->choice.platform, well);
	if (owned == 0)
		return 0;

	/* Succeeded: the driver owns the well on. */
	return 1;
}

/* Fills the xd power wells' hooks with this display's interrupt, VGA and Type-C PHY steps. */
static void
display_power_hooks(
	struct i915_display_power *power,
	const struct i915_xd_well_desc *desc,
	struct display_power_hook *hook,
	struct i915_xd_power_hooks *hooks)
{
	/* The power and the well the hooks act for. */
	hook->power = power;
	hook->name = desc->name;

	/* The steps; Ice Lake's legacy and eDP answers are not given here (no display 11 port is driven). */
	hooks->pipes_up = display_power_pipes_up;
	hooks->pipes_down = display_power_pipes_down;
	hooks->vga_reset = display_power_vga_reset;
	hooks->tc_health = display_power_tc_health;
	hooks->tc_legacy = NULL;
	hooks->port_edp = NULL;
	hooks->context = hook;
}

/*
 * Restores the interrupts of a well's pipes after it came up, once the
 * interrupts are installed; before that there is nothing to restore.
 */
static void
display_power_pipes_up(
	void *context,
	unsigned pipe_mask)
{
	struct display_power_hook *hook;
	struct i915_display_power *power;

	/* The power the hook acts for. */
	hook = context;
	power = hook->power;

	/* The interrupt layer rewrites the pipes' interrupt registers, which live in the well. */
	if (power->irqs_enabled && power->irq_ops != NULL)
		power->irq_ops->post_enable(power->irq_ctx, pipe_mask);
}

/*
 * Drains the interrupts of a well's pipes before it goes off: the pipes'
 * sources stop and a handler already running is waited for, because it
 * would read a register without power.  Reports nonzero when the drain
 * failed now or before: the well then stays on.
 */
static int
display_power_pipes_down(
	void *context,
	unsigned pipe_mask)
{
	struct display_power_hook *hook;
	struct i915_display_power *power;
	int drained;

	/* The power the hook acts for. */
	hook = context;
	power = hook->power;

	/* The interrupt layer drains the pipes once the interrupts are installed. */
	if (power->irqs_enabled &&
	    !power->irq_sync_failed &&
	    power->irq_ops != NULL) {
		drained = power->irq_ops->pre_disable(power->irq_ctx, pipe_mask);

		/* A handler that was not seen to leave keeps this well, and every later one, on. */
		if (drained != 0) {
			power->irq_sync_failed = 1;
			kern_logf("i915: display power: well %s: the pipes' interrupts did not drain; it stays on and every later well off is refused\n", hook->name);
		}
	}

	/* A failed drain keeps the well on. */
	if (power->irq_sync_failed)
		return 1;

	/* Succeeded: the pipes are drained. */
	return 0;
}

/* Rewrites the VGA registers after the well that holds the VGA plane came up, so that the VGA console stays sane. */
static void
display_power_vga_reset(
	void *context)
{
	struct display_power_hook *hook;

	/* Rewrites them through the VGA client. */
	hook = context;
	drv_i915_vga_reset_io_mem(hook->power->vga);
	hook->power->vga_resets++;
}

/*
 * Waits up to a millisecond for a Type-C port's PHY microcontroller to
 * report itself healthy after its AUX well came up.  One that does not is
 * logged and the well still counts as on.  Returns 0, or EIO when the time
 * base failed.
 */
static int
display_power_tc_health(
	void *context,
	unsigned tc_port)
{
	struct display_power_hook *hook;
	struct i915_display_power *power;
	int waited;

	/* The power the hook acts for. */
	hook = context;
	power = hook->power;

	/* Without the PHYs' windows (the GPU-free tests) there is nothing to read. */
	if (power->dkl == NULL)
		return 0;

	/* Polls the microcontroller's health through the Dekel window. */
	waited = drv_i915_xd_tc_wait_health(power->dkl, tc_port, I915_XD_DKL_HEALTH_US);
	if (waited == EIO)
		return EIO;

	/* A PHY that stayed unhealthy is logged; the well goes on. */
	if (waited != 0) {
		power->late_health++;
		kern_logf("i915: display power: well %s: the Type-C PHY of TC%u did not report itself healthy in time\n", hook->name, tc_port + 1U);
	}

	/* Succeeded: the wait is over. */
	return 0;
}

/* Fills the switch the xd counts turn wells on and off through. */
static void
display_power_switch(
	struct i915_display_power *power,
	struct i915_xd_power_switch *sw)
{
	/* The steps and the power they act for. */
	sw->on = display_power_switch_on;
	sw->off = display_power_switch_off;
	sw->context = power;
}

/*
 * Turns a well on for the counts.  The firmware's mailbox block has no
 * switch here: it counts as on while it has users.  Returns 0, or the
 * error that stops the caller.
 */
static int
display_power_switch_on(
	void *context,
	unsigned well)
{
	struct i915_display_power *power;
	const struct i915_xd_well_desc *desc;
	int error;

	/* The mailbox block is only recorded. */
	power = context;
	desc = display_power_desc(power, well);
	if (desc->control == I915_XD_WELL_CONTROL_MAILBOX) {
		drv_i915_xd_power_record(&power->xd->power, well, 1);
		return 0;
	}

	/* Turns the well on. */
	error = display_power_well_on(power, well);
	if (error != 0)
		return error;

	/* Succeeded: the well is on. */
	return 0;
}

/*
 * Turns a well off for the counts.  Returns 0, or EBUSY when the well
 * stays on (it then keeps one user for good).
 */
static int
display_power_switch_off(
	void *context,
	unsigned well)
{
	struct i915_display_power *power;
	const struct i915_xd_well_desc *desc;
	int error;

	/* The mailbox block is only recorded. */
	power = context;
	desc = display_power_desc(power, well);
	if (desc->control == I915_XD_WELL_CONTROL_MAILBOX) {
		drv_i915_xd_power_record(&power->xd->power, well, 0);
		return 0;
	}

	/* Turns the well off; a refused off keeps it on. */
	error = display_power_well_off(power, well);
	if (error == EBUSY) {
		power->kept_wells++;
		return EBUSY;
	}

	/* Succeeded: the well is off, or its off failed in a way that leaves nothing to keep. */
	return 0;
}

/*
 * Hands the display's DC policy (the states allowed, the one entered when
 * idle, whether the DMC's program is loaded) to xd-power.c and returns
 * its counts.
 */
static struct i915_xd_power *
display_power_dc_policy(
	struct i915_display_power *power)
{
	struct i915_xd_power *policy;

	/* The policy follows the options and the DMC's load. */
	policy = &power->xd->power;
	drv_i915_xd_power_dc_policy(policy, power->dc_allowed, power->dc_target, power->dmc_loaded);

	/* Succeeded: the counts whose DC state the caller changes. */
	return policy;
}

/* Logs what a DC state change met. */
static void
display_power_dc_log(
	const struct i915_xd_dc_report *report)
{
	/* A utility pin driving a PWM keeps the display out of DC6. */
	if (report->pwm_pin)
		kern_logf("i915: display power: DC6 is not entered: the utility pin drives a PWM\n");

	/* DC6 was requested already. */
	if (report->dc6_already)
		kern_logf("i915: display power: DC6 was requested already\n");

	/* The DMC did not keep the state last written. */
	if (report->mismatch)
		kern_logf("i915: display power: DC state 0x%x found where 0x%x was written\n", report->found, report->expected);

	/* The state never read back as written. */
	if (report->not_taken)
		kern_logf("i915: display power: DC state 0x%x did not read back (0x%x)\n", report->wanted, report->now);
}

/*
 * Leaves the DC states when the DC off well comes on.
 *
 * Once the core has started, the CD clock and the display buffer's slices
 * the hardware reads are compared with what the driver set (never set
 * again here), and the combo PHYs, whose state the DMC does not keep
 * across the DC states, are brought up again.
 */
static void
display_power_dc_off_on(
	struct i915_display_power *power)
{
	struct i915_xd_dc_report report;
	struct i915_cdclk_config readout;
	struct i915_xd_power *policy;
	unsigned slice;
	uint8_t slices;
	uint32_t control;

	/* Leaves the DC states through the write that is read back. */
	power->dc_off_on++;
	policy = display_power_dc_policy(power);
	drv_i915_xd_power_dc_leave(policy, &power->xd->io, &report);
	display_power_dc_log(&report);

	/* Before the core's start there is no clock or buffer to compare. */
	if (power->cdclk == NULL)
		return;

	/* Reads the CD clock and compares it with what the driver set. */
	kern_memset(&readout, 0, sizeof(readout));
	drv_i915_cdclk_read(power->cdclk, &readout);
	if (readout.cdclk != power->cdclk->hw.cdclk || readout.vco != power->cdclk->hw.vco) {
		kern_logf("i915: display power: DC off: the CD clock reads %u/%u kHz where %u/%u was set\n",
			readout.cdclk,
			readout.vco,
			power->cdclk->hw.cdclk,
			power->cdclk->hw.vco);
	}

	/* Reads which display buffer slices are powered. */
	slices = 0U;
	for (slice = 0U; slice < 4U; slice++) {
		control = drv_i915_xd_io_read(&power->xd->io, display_power_dbuf_controls[slice]);
		if ((control & I915_XD_DBUF_POWER_STATE) != 0U)
			slices |= (uint8_t)(1U << slice);
	}

	/* Logs slices that do not follow what the driver set. */
	if (slices != power->dbuf_slices)
		kern_logf("i915: display power: DC off: display buffer slices 0x%x are powered where 0x%x were set\n", slices, power->dbuf_slices);

	/* Brings the combo PHYs up again. */
	drv_i915_trace_init(power->trace);
	display_power_combo_up(power);
}

/*
 * Lets the DMC take the display into the target DC state when the DC off
 * well goes off; without the DMC's program nothing is written.
 */
static void
display_power_dc_off_off(
	struct i915_display_power *power)
{
	struct i915_xd_dc_report report;
	struct i915_xd_power *policy;

	/* Enters the target state; a write that does not read back is logged. */
	power->dc_off_off++;
	policy = display_power_dc_policy(power);
	(void)drv_i915_xd_power_dc_enter(policy, &power->xd->io, &report);
	display_power_dc_log(&report);
}

/* Brings up the combo PHYs that do not read as up, and records and logs how many. */
static void
display_power_combo_up(
	struct i915_display_power *power)
{
	unsigned written;
	unsigned count;

	/* Brings them up through the xd DDI code. */
	written = drv_i915_xd_combo_phy_init(&power->xd->io, power->xd->choice.platform);
	count = (unsigned)__builtin_popcount(written);

	/* Records and logs what was brought up. */
	drv_i915_trace_record(power->trace, I915_DISPLAY_POWER_TRACE_STAGE, I915_TRACE_ACQUIRE, "combo PHYs", (uint64_t)count, 0U);
	kern_logf("i915: display power: combo PHYs up: %u (mask 0x%x)\n", count, written);
}

/*
 * Reads every switched well's state into the record, taking a well the
 * firmware requested over: the driver's request is set before the
 * firmware's is withdrawn.  The users are not touched.  The caller holds
 * the lock.
 */
static void
display_power_sync(
	struct i915_display_power *power)
{
	const struct i915_xd_platform *platform;
	const struct i915_xd_well_desc *desc;
	unsigned well;
	int lit;

	/* Walks every well of the table. */
	platform = power->xd->choice.platform;
	for (well = 0U; well < platform->nwells; well++) {
		/* The mailbox block has no state to read. */
		desc = display_power_desc(power, well);
		if (desc->control == I915_XD_WELL_CONTROL_MAILBOX)
			continue;

		/* Takes the firmware's request of a requested well over. */
		if (desc->control != I915_XD_WELL_CONTROL_DC_OFF)
			drv_i915_xd_power_well_adopt(&power->xd->io, platform, well);

		/* Records what the hardware now says. */
		lit = display_power_well_lit(power, well);
		drv_i915_xd_power_record(&power->xd->power, well, lit);
	}
}

/* Powers exactly the slices asked for through the xd display buffer code, under the lock, and logs late slices. */
static void
display_power_dbuf_apply(
	struct i915_display_power *power,
	uint8_t slices)
{
	uint8_t late;
	unsigned slice;

	/* Powers the slices under the lock. */
	mutex_lock(&power->lock);

	late = drv_i915_xd_dbuf_power(&power->xd->io, power->xd->choice.platform, slices);
	power->dbuf_slices = slices;

	mutex_unlock(&power->lock);

	/* Logs a slice that did not follow the request in time. */
	for (slice = 0U; slice < 4U; slice++) {
		if ((late & (1U << slice)) == 0U)
			continue;

		/* Logs the slice that was late. */
		kern_logf("i915: display power: display buffer slice %u did not turn %s in time\n",
			slice,
			(slices & (1U << slice)) != 0U ? "on" : "off");
	}
}

/* Fills xd-core.c's hooks with this display's power well 1, CD clock and display buffer. */
static void
display_power_core_hooks(
	struct i915_display_power *power,
	struct i915_xd_core_hooks *hooks)
{
	/* The display's power is the hooks' context. */
	hooks->well1_on = display_power_core_well1_on;
	hooks->well1_off = display_power_core_well1_off;
	hooks->cdclk_on = display_power_core_cdclk_on;
	hooks->cdclk_off = display_power_core_cdclk_off;
	hooks->dbuf = display_power_core_dbuf;
	hooks->faulted = display_power_core_faulted;
	hooks->context = power;
}

/* Turns power well 1 on under the lock: 0, or EIO when the table has none or the time base failed. */
static int
display_power_core_well1_on(
	void *context)
{
	struct i915_display_power *power;
	int well;
	int error;

	/* The display's power the hook acts for, and power well 1's row of its table. */
	power = context;
	well = drv_i915_xd_power_find(power->xd->choice.platform, I915_XD_WELL_CONTROL_MAIN, I915_XD_WELL_PW_1);
	if (well < 0)
		return EIO;

	/* Turns it on under the lock. */
	mutex_lock(&power->lock);

	error = display_power_well_on(power, (unsigned)well);

	mutex_unlock(&power->lock);

	/* Reports a time base fault. */
	if (error != 0)
		return EIO;

	/* Succeeded: power well 1 is on. */
	return 0;
}

/* Turns power well 1 off under the lock. */
static void
display_power_core_well1_off(
	void *context)
{
	struct i915_display_power *power;
	int well;

	/* The display's power the hook acts for, and power well 1's row of its table. */
	power = context;
	well = drv_i915_xd_power_find(power->xd->choice.platform, I915_XD_WELL_CONTROL_MAIN, I915_XD_WELL_PW_1);
	if (well < 0)
		return;

	/* Turns it off under the lock. */
	mutex_lock(&power->lock);

	(void)display_power_well_off(power, (unsigned)well);

	mutex_unlock(&power->lock);
}

/* Brings the CD clock up. */
static void
display_power_core_cdclk_on(
	void *context)
{
	struct i915_display_power *power;

	/* Starts the display's CD clock. */
	power = context;
	drv_i915_cdclk_start(power->cdclk);
}

/* Takes the CD clock down. */
static void
display_power_core_cdclk_off(
	void *context)
{
	struct i915_display_power *power;

	/* Stops the display's CD clock. */
	power = context;
	drv_i915_cdclk_stop(power->cdclk);
}

/* Powers the display buffer slices the core asks for, within the display's. */
static void
display_power_core_dbuf(
	void *context,
	uint8_t slices)
{
	struct i915_display_power *power;

	/* Powers the slices asked for that the display has. */
	power = context;
	display_power_dbuf_apply(power, (uint8_t)(slices & power->dbuf_mask));
}

/* Tells whether the register access met a time base fault. */
static int
display_power_core_faulted(
	void *context)
{
	int faulted;

	UNUSED_PARAMETER(context);

	/* The access layer's fault state. */
	faulted = drv_i915_time_base_faulted();
	if (faulted != 0)
		return 1;

	/* Succeeded: no fault. */
	return 0;
}

/* Records the first fault of the core's start and the step it stopped at, and logs it. */
static void
display_power_core_fault(
	struct i915_display_power *power,
	const char *where)
{
	/* Keeps the first fault and its step. */
	if (!power->core_faulted) {
		power->core_faulted = 1;
		power->core_fault = where;
	}

	/* Logs every stop. */
	kern_logf("i915: display power: the core's start stopped at %s (a fault of the register access)\n", where);
}
