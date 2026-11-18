/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the display's power (display/display-power.c) on the
 * register model (xd-fake-regs.c), with the Alder Lake-P description: the
 * start's DC states and display buffer slices; the AUX channels' power
 * functions of display 12 and 13, directly and through Thunderbolt; a
 * pipe's wells taken and given back; the DC off well with and without the
 * DMC's program; a deferred give-back through the bound work and through
 * a flush; a pipe drain that fails and keeps every later well on; and the
 * turning off of a well the firmware left on.
 */

#include "host-test.h"
#include "xd-fake-regs.h"

#include "../../display/internal.h"
#include "../../display/display-power.h"
#include "../../display/xd-io.h"
#include "../../display/xd-platform.h"
#include "../../display/xd-power.h"
#include "../../intel/display-regs-xd.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* The Latitude 5330's GPU (Alder Lake-P) and its revision. */
#define I915_DISPLAY_POWER_TEST_DEVICE	0x46a6U
#define I915_DISPLAY_POWER_TEST_REVID	0x0cU

/* The pipe whose wells the checks take: pipe B, whose well holds its interrupts. */
#define I915_DISPLAY_POWER_TEST_PIPE	I915_XD_POWERS_PIPE(1U)

/*
 * What the hooks of the test saw: the interrupt layer's restores and
 * drains (and whether a drain fails), the VGA client's port writes, and
 * the delayed work's armings and cancels.
 */
struct display_power_test_seen {
	unsigned pipes_up;
	unsigned pipes_down;
	int drain_fails;
	unsigned vga_writes;
	unsigned queued;
	int queued_delay;
	unsigned cancelled;
	int cancel_sync;
};

static void display_power_test_start(void);
static void display_power_test_aux(void);
static void display_power_test_pipe(void);
static void display_power_test_dc_off(void);
static void display_power_test_later(void);
static void display_power_test_drain(void);
static void display_power_test_sanitize(void);
static struct i915_display_power *display_power_test_setup(void);
static int display_power_test_owned(struct i915_display_power *power, uint64_t functions);
static void display_power_test_post_enable(void *ctx, unsigned pipe_mask);
static int display_power_test_pre_disable(void *ctx, unsigned pipe_mask);
static int display_power_test_vga_get(void *ctx, int rsrc);
static unsigned char display_power_test_vga_in8(void *ctx, unsigned short port);
static void display_power_test_vga_out8(void *ctx, unsigned short port, unsigned char val);
static void display_power_test_vga_put(void *ctx, int rsrc);
static int display_power_test_queue(void *context, int delay_ms);
static int display_power_test_cancel(void *context, int sync);

/*
 * The interrupt layer's hooks of the test.  They never change.
 */
static const struct i915_pw_irq_ops display_power_test_irq_ops = {
	display_power_test_post_enable,
	display_power_test_pre_disable
};

/*
 * The display the checks run on.  Each check prepares it afresh; it is
 * too large for the stack and lives for the whole test.
 */
static struct i915_display *display_power_test_display;

/*
 * What the hooks saw since the display was prepared.
 */
static struct display_power_test_seen display_power_test_seen;

/*
 * The VGA client's port operations: the legacy I/O is always granted and
 * the writes are counted, so that no real port is touched.
 */
static struct i915_vga_io_ops display_power_test_vga_io;

/*
 * Runs every check of the display's power and reports.
 */
int
main(void)
{
	int failures;

	/* The display the checks run on. */
	display_power_test_display = calloc(1U, sizeof(*display_power_test_display));
	if (display_power_test_display == NULL)
		return 1;

	/* Runs each group of checks. */
	display_power_test_start();
	display_power_test_aux();
	display_power_test_pipe();
	display_power_test_dc_off();
	display_power_test_later();
	display_power_test_drain();
	display_power_test_sanitize();

	/* Reports the result. */
	free(display_power_test_display);
	failures = i915_host_report("host_display_power_test");
	if (failures != 0)
		return 1;

	/* Succeeded: every check passed. */
	return 0;
}

/* Checks what the start sets: the DC states and the display buffer's slices of Alder Lake-P. */
static void
display_power_test_start(void)
{
	struct i915_display_power *power;

	/* A prepared display. */
	power = display_power_test_setup();
	i915_host_check(power != NULL, "display power: the start finds the Alder Lake-P description");
	if (power == NULL)
		return;

	/* DC9, DC3CO and up to DC6 allowed, DC6 entered when idle, four slices. */
	i915_host_check(power->dc_allowed == 0x4000000aU && power->dc_target == 0x2U,
			"display power: DC9, DC3CO and DC6 are allowed and DC6 is the idle state");
	i915_host_check(power->dbuf_mask == 0xfU && power->dbuf_slices == 0U && power->start_held == 0,
			"display power: four display buffer slices, none powered yet, no start reference");
}

/* Checks the AUX channels' power functions, directly and through Thunderbolt. */
static void
display_power_test_aux(void)
{
	/* A channel used directly takes its own AUX well, an unknown one channel A's. */
	i915_host_check(drv_i915_display_power_aux(13U, 0, 0) == I915_XD_POWERS_AUX(0U) &&
			drv_i915_display_power_aux(13U, 3, 0) == I915_XD_POWERS_AUX(3U) &&
			drv_i915_display_power_aux(13U, 7, 0) == I915_XD_POWERS_AUX(7U) &&
			drv_i915_display_power_aux(12U, 8, 0) == I915_XD_POWERS_AUX(8U) &&
			drv_i915_display_power_aux(13U, 9, 0) == I915_XD_POWERS_AUX(0U) &&
			drv_i915_display_power_aux(13U, -1, 0) == I915_XD_POWERS_AUX(0U),
			"display power: an AUX channel takes its own well, an unknown one channel A's");

	/* Through Thunderbolt only a Type-C channel has a well: four on display 13, six on display 12. */
	i915_host_check(drv_i915_display_power_aux(13U, 3, 1) == I915_XD_POWERS_TBT_AUX(0U) &&
			drv_i915_display_power_aux(13U, 6, 1) == I915_XD_POWERS_TBT_AUX(3U) &&
			drv_i915_display_power_aux(13U, 7, 1) == 0ULL &&
			drv_i915_display_power_aux(13U, 2, 1) == 0ULL &&
			drv_i915_display_power_aux(12U, 8, 1) == I915_XD_POWERS_TBT_AUX(5U),
			"display power: a Thunderbolt AUX well only for a Type-C channel");
}

/* Checks a pipe's wells taken and given back. */
static void
display_power_test_pipe(void)
{
	struct i915_display_power *power;
	int error;
	int on;
	int owned;

	/* A prepared display. */
	power = display_power_test_setup();
	if (power == NULL)
		return;

	/* Takes pipe B's wells: they are requested, on and recorded. */
	error = drv_i915_display_power_get(power, I915_DISPLAY_POWER_TEST_PIPE);
	on = drv_i915_display_power_is_on(power, I915_DISPLAY_POWER_TEST_PIPE);
	owned = display_power_test_owned(power, I915_DISPLAY_POWER_TEST_PIPE);
	i915_host_check(error == 0 && on == 1 && owned == 1, "display power: pipe B's wells come up for its first user");

	/* A second user changes nothing; the first give-back keeps them on. */
	error = drv_i915_display_power_get(power, I915_DISPLAY_POWER_TEST_PIPE);
	drv_i915_display_power_put(power, I915_DISPLAY_POWER_TEST_PIPE);
	on = drv_i915_display_power_is_on(power, I915_DISPLAY_POWER_TEST_PIPE);
	i915_host_check(error == 0 && on == 1, "display power: the wells stay on while a user is left");

	/* The last give-back takes them down. */
	drv_i915_display_power_put(power, I915_DISPLAY_POWER_TEST_PIPE);
	on = drv_i915_display_power_is_on(power, I915_DISPLAY_POWER_TEST_PIPE);
	owned = display_power_test_owned(power, I915_DISPLAY_POWER_TEST_PIPE);
	i915_host_check(on == 0 && owned == 0, "display power: the wells go down after the last user");
}

/* Checks the DC off well: the DC states left, and entered again only with the DMC's program. */
static void
display_power_test_dc_off(void)
{
	struct i915_display_power *power;
	uint32_t dc_state;
	int error;

	/* A prepared display the firmware left in DC6. */
	power = display_power_test_setup();
	if (power == NULL)
		return;
	i915_xd_fake_set(I915_XD_DC_STATE_EN, 0x2U);

	/* Taking DC off leaves the DC states. */
	error = drv_i915_display_power_get(power, I915_XD_POWERS_DC_OFF);
	dc_state = i915_xd_fake_get(I915_XD_DC_STATE_EN);
	i915_host_check(error == 0 && (dc_state & 0x3U) == 0U, "display power: DC off leaves the DC states");

	/* Without the DMC's program its give-back enters no DC state. */
	drv_i915_display_power_put(power, I915_XD_POWERS_DC_OFF);
	dc_state = i915_xd_fake_get(I915_XD_DC_STATE_EN);
	i915_host_check((dc_state & 0x3U) == 0U && power->dc_off_off == 1U,
			"display power: without the DMC's program the display stays out of the DC states");

	/* With it the give-back enters DC6. */
	power->dmc_loaded = 1;
	(void)drv_i915_display_power_get(power, I915_XD_POWERS_DC_OFF);
	drv_i915_display_power_put(power, I915_XD_POWERS_DC_OFF);
	dc_state = i915_xd_fake_get(I915_XD_DC_STATE_EN);
	i915_host_check((dc_state & 0x3U) == 0x2U, "display power: with the DMC's program the give-back of DC off enters DC6");
}

/* Checks a deferred give-back: run by the bound work, and by a flush. */
static void
display_power_test_later(void)
{
	struct i915_display_power *power;
	struct i915_xd_power_work work;
	int on;

	/* A prepared display with the test's work bound. */
	power = display_power_test_setup();
	if (power == NULL)
		return;
	work.queue = display_power_test_queue;
	work.cancel = display_power_test_cancel;
	work.context = &display_power_test_seen;
	drv_i915_display_power_bind_work(power, &work);

	/* A deferred give-back keeps the wells on and arms the work with the delay. */
	(void)drv_i915_display_power_get(power, I915_DISPLAY_POWER_TEST_PIPE);
	drv_i915_display_power_put_later(power, I915_DISPLAY_POWER_TEST_PIPE, 50);
	on = drv_i915_display_power_is_on(power, I915_DISPLAY_POWER_TEST_PIPE);
	i915_host_check(on == 1 && display_power_test_seen.queued == 1U && display_power_test_seen.queued_delay == 50,
			"display power: a deferred give-back keeps the wells on and arms the work");

	/* The work's run takes them down. */
	drv_i915_display_power_work(power);
	on = drv_i915_display_power_is_on(power, I915_DISPLAY_POWER_TEST_PIPE);
	i915_host_check(on == 0, "display power: the work's run gives the wells back");

	/* A default delay, then a flush: the wells go down and the work is cancelled synchronously. */
	(void)drv_i915_display_power_get(power, I915_DISPLAY_POWER_TEST_PIPE);
	drv_i915_display_power_put_later(power, I915_DISPLAY_POWER_TEST_PIPE, -1);
	i915_host_check(display_power_test_seen.queued_delay == I915_DISPLAY_POWER_LATER_MS, "display power: the default delay is 100 ms");
	drv_i915_display_power_flush(power);
	on = drv_i915_display_power_is_on(power, I915_DISPLAY_POWER_TEST_PIPE);
	i915_host_check(on == 0 && display_power_test_seen.cancel_sync == 1 && power->xd->power.stats.errors == 0U,
			"display power: a flush gives the wells back and cancels the work synchronously");

	/* Unbound, a deferred give-back is immediate. */
	drv_i915_display_power_bind_work(power, NULL);
	(void)drv_i915_display_power_get(power, I915_DISPLAY_POWER_TEST_PIPE);
	drv_i915_display_power_put_later(power, I915_DISPLAY_POWER_TEST_PIPE, 50);
	on = drv_i915_display_power_is_on(power, I915_DISPLAY_POWER_TEST_PIPE);
	i915_host_check(on == 0, "display power: without a bound work the give-back is immediate");
}

/* Checks a pipe drain that fails: the well stays on and every later well off is refused. */
static void
display_power_test_drain(void)
{
	struct i915_display_power *power;
	int owned;

	/* A prepared display with interrupts on. */
	power = display_power_test_setup();
	if (power == NULL)
		return;
	power->irqs_enabled = 1;

	/* Taking pipe B's wells restores the pipe's interrupts. */
	(void)drv_i915_display_power_get(power, I915_DISPLAY_POWER_TEST_PIPE);
	i915_host_check(display_power_test_seen.pipes_up != 0U, "display power: a pipe well that comes up restores its interrupts");

	/* A drain that fails keeps the wells on and refuses the later offs. */
	display_power_test_seen.drain_fails = 1;
	drv_i915_display_power_put(power, I915_DISPLAY_POWER_TEST_PIPE);
	owned = display_power_test_owned(power, I915_DISPLAY_POWER_TEST_PIPE);
	i915_host_check(display_power_test_seen.pipes_down == 1U &&
			power->irq_sync_failed == 1 &&
			power->refusals != 0U &&
			power->kept_wells != 0U &&
			owned == 1,
			"display power: a failed drain keeps the well on and refuses every later well off");
}

/* Checks the turning off of a well the firmware left on and nothing uses. */
static void
display_power_test_sanitize(void)
{
	const struct i915_xd_platform *platform;
	struct i915_display_power *power;
	uint64_t wells;
	uint32_t control;
	unsigned well;
	unsigned turned_off;
	int owned;

	/* A prepared display. */
	power = display_power_test_setup();
	if (power == NULL)
		return;
	platform = power->xd->choice.platform;

	/* The firmware left pipe B's main wells requested. */
	wells = drv_i915_xd_power_wells_of(platform, I915_DISPLAY_POWER_TEST_PIPE);
	control = 0U;
	for (well = 0U; well < platform->nwells; well++) {
		/* Only pipe B's wells of the main control. */
		if ((wells & (1ULL << well)) == 0ULL)
			continue;
		if (platform->wells[well].control != I915_XD_WELL_CONTROL_MAIN)
			continue;

		/* The request and the state bits of the well. */
		control |= 3U << (2U * platform->wells[well].index);
	}

	/* Writes the requests and states the firmware left. */
	i915_xd_fake_set(I915_XD_PWR_WELL_CTL, control);

	/* The sanitize turns them off. */
	turned_off = drv_i915_display_power_sanitize(power);
	owned = display_power_test_owned(power, I915_DISPLAY_POWER_TEST_PIPE);
	i915_host_check(control != 0U && turned_off != 0U && owned == 0,
			"display power: the wells the firmware left on and nothing uses are turned off");
}

/*
 * Prepares the display on a fresh model: every main, AUX and DDI well's
 * state follows its request at once and every power gate's fuses are in;
 * the Alder Lake-P description, the display's power started with the
 * test's interrupt and VGA hooks.  Returns the display's power, or NULL
 * when the description is not found.
 */
static struct i915_display_power *
display_power_test_setup(void)
{
	struct i915_xd_io_hooks hooks;
	struct i915_display *display;
	struct i915_display_power *power;
	int error;

	/* A fresh model, a fresh display and nothing seen. */
	display = display_power_test_display;
	memset(display, 0, sizeof(*display));
	memset(&display_power_test_seen, 0, sizeof(display_power_test_seen));
	i915_xd_fake_reset();
	i915_xd_fake_hooks(&hooks);
	drv_i915_xd_io_init(&display->xd.io, NULL, NULL);
	drv_i915_xd_io_set_hooks(&display->xd.io, &hooks);

	/* The states of the three controls follow their requests; the fuses are in. */
	i915_xd_fake_rule_mirror(I915_XD_PWR_WELL_CTL, 0xaaaaaaaaU, I915_XD_PWR_WELL_CTL, -1, 0U);
	i915_xd_fake_rule_mirror(I915_XD_PWR_WELL_CTL_AUX, 0xaaaaaaaaU, I915_XD_PWR_WELL_CTL_AUX, -1, 0U);
	i915_xd_fake_rule_mirror(I915_XD_PWR_WELL_CTL_DDI, 0xaaaaaaaaU, I915_XD_PWR_WELL_CTL_DDI, -1, 0U);
	i915_xd_fake_set(I915_XD_FUSE_STATUS, 0xffffffffU);

	/* The description of the Latitude 5330's GPU. */
	error = drv_i915_xd_platform_choose(I915_DISPLAY_POWER_TEST_DEVICE, I915_DISPLAY_POWER_TEST_REVID, 0, 0U, &display->xd.choice);
	if (error != 0)
		return NULL;

	/* The description is chosen. */
	display->xd.chosen = 1;

	/* The VGA client's ports are the test's. */
	display_power_test_vga_io.get = display_power_test_vga_get;
	display_power_test_vga_io.in8 = display_power_test_vga_in8;
	display_power_test_vga_io.out8 = display_power_test_vga_out8;
	display_power_test_vga_io.put = display_power_test_vga_put;
	display_power_test_vga_io.ctx = &display_power_test_seen;
	display->g_vga_io = &display_power_test_vga_io;

	/* The display's power with the test's hooks; no CD clock, so DC off checks nothing beside the DC states. */
	power = &display->power;
	error = drv_i915_display_power_start(power, &display->xd, &display->dc_trace);
	if (error != 0)
		return NULL;

	/* The VGA client, the trace ring and the interrupt hooks the wells' changes use. */
	power->vga = &display->vga_client;
	power->trace = &display->dc_trace;
	power->irq_ops = &display_power_test_irq_ops;
	power->irq_ctx = &display_power_test_seen;

	/* Succeeded: the display's power. */
	return power;
}

/* Tells whether the driver owns every main, AUX and DDI well of the functions on (1 or 0). */
static int
display_power_test_owned(
	struct i915_display_power *power,
	uint64_t functions)
{
	const struct i915_xd_platform *platform;
	uint64_t wells;
	unsigned well;
	int owned;

	/* Asks each requested well of the functions. */
	platform = power->xd->choice.platform;
	wells = drv_i915_xd_power_wells_of(platform, functions);
	for (well = 0U; well < platform->nwells; well++) {
		/* Only the functions' wells that have a request bit. */
		if ((wells & (1ULL << well)) == 0ULL)
			continue;
		if (platform->wells[well].control == I915_XD_WELL_CONTROL_DC_OFF)
			continue;

		/* A well the driver does not own on answers no. */
		owned = drv_i915_xd_power_well_owned(&power->xd->io, platform, well);
		if (owned == 0)
			return 0;
	}

	/* Succeeded: every well is owned on. */
	return 1;
}

/* Records a restore of the pipes' interrupts. */
static void
display_power_test_post_enable(
	void *ctx,
	unsigned pipe_mask)
{
	struct display_power_test_seen *seen;

	UNUSED_PARAMETER(pipe_mask);

	/* Counts it. */
	seen = ctx;
	seen->pipes_up++;
}

/* Records a drain of the pipes' interrupts and answers whether it fails. */
static int
display_power_test_pre_disable(
	void *ctx,
	unsigned pipe_mask)
{
	struct display_power_test_seen *seen;

	UNUSED_PARAMETER(pipe_mask);

	/* Counts it. */
	seen = ctx;
	seen->pipes_down++;

	/* A drain set to fail reports it. */
	if (seen->drain_fails)
		return 1;

	/* Succeeded: the pipes are drained. */
	return 0;
}

/* Grants the legacy VGA I/O. */
static int
display_power_test_vga_get(
	void *ctx,
	int rsrc)
{
	UNUSED_PARAMETER(ctx);
	UNUSED_PARAMETER(rsrc);

	/* Succeeded: granted. */
	return 1;
}

/* Reads a VGA port: always 0. */
static unsigned char
display_power_test_vga_in8(
	void *ctx,
	unsigned short port)
{
	UNUSED_PARAMETER(ctx);
	UNUSED_PARAMETER(port);

	/* Succeeded: the port reads 0. */
	return 0U;
}

/* Counts a VGA port write. */
static void
display_power_test_vga_out8(
	void *ctx,
	unsigned short port,
	unsigned char val)
{
	struct display_power_test_seen *seen;

	UNUSED_PARAMETER(port);
	UNUSED_PARAMETER(val);

	/* Counts it. */
	seen = ctx;
	seen->vga_writes++;
}

/* Gives the legacy VGA I/O back. */
static void
display_power_test_vga_put(
	void *ctx,
	int rsrc)
{
	UNUSED_PARAMETER(ctx);
	UNUSED_PARAMETER(rsrc);
}

/* Records an arming of the delayed work: 1, newly armed. */
static int
display_power_test_queue(
	void *context,
	int delay_ms)
{
	struct display_power_test_seen *seen;

	/* Counts it and keeps the delay. */
	seen = context;
	seen->queued++;
	seen->queued_delay = delay_ms;

	/* Succeeded: newly armed. */
	return 1;
}

/* Records a cancel of the delayed work: 0, it was not pending. */
static int
display_power_test_cancel(
	void *context,
	int sync)
{
	struct display_power_test_seen *seen;

	/* Counts it and keeps whether it waited. */
	seen = context;
	seen->cancelled++;
	seen->cancel_sync = sync;

	/* Succeeded: nothing was pending. */
	return 0;
}
