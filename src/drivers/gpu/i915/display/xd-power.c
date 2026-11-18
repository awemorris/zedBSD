/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The power wells of the display 12 and 13 code (see xd-power.h).
 */

#include "xd-io.h"
#include "xd-platform.h"
#include "xd-power.h"

#include "../intel/display-regs-xd.h"
#include "../intel/display-regs-xd9.h"
#include "../intel/display-regs-xd-south.h"

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>

/*
 * How long the hardware may take: a well's state after its request when
 * the table gives no time, a power gate's fuses, and a well's state
 * after its request is withdrawn.
 *
 * Values referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_display_power_well.c
 * (hsw_wait_for_power_well_enable, gen9_wait_for_power_well_fuses,
 * hsw_wait_for_power_well_disable).
 */
#define I915_XD_POWER_ENABLE_US		1000U
#define I915_XD_POWER_FUSE_US		1000U
#define I915_XD_POWER_DISABLE_US	1000U

/* The power gate whose fuses come before power well 1, and power well 1's own. */
#define I915_XD_POWER_GATE_0		0U
#define I915_XD_POWER_GATE_1		1U

/* The distance from a well control to its BIOS copy, the first of the four. */
#define I915_XD_POWER_BIOS_OFFSET	4U

/* The Type-C ports a Thunderbolt AUX function may name. */
#define I915_XD_POWER_TBT_PORTS		6U

/* The common lane word of a combo PHY that holds its AUX lane enable. */
#define I915_XD_POWER_CL_DW12		12U

/*
 * The TC cold block of Tiger Lake through the GT driver mailbox: how long
 * one request may take, how many requests are made, and the pause
 * between them.
 *
 * Values referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_display_power_well.c
 * (tgl_tc_cold_request: 3 tries, 1 ms apart), and
 * drivers/gpu/drm/i915/intel_pcode.c (snb_pcode_read: 500 us).
 */
#define I915_XD_POWER_TCCOLD_US		500U
#define I915_XD_POWER_TCCOLD_TRIES	3U
#define I915_XD_POWER_TCCOLD_PAUSE_US	1000U

/*
 * The TC cold exit of Ice Lake through the GT driver mailbox: how long
 * one request may take (250 us, then 1 ms more), how many requests are
 * made while the mailbox is busy, the pause between them, and how long
 * the exit takes once accepted.
 *
 * Values referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_display_power_well.c
 * (icl_tc_cold_exit: 250 us and 1 ms, 3 tries, 1 ms apart, 1 ms after).
 */
#define I915_XD_POWER_TC_EXIT_US	1250U
#define I915_XD_POWER_TC_EXIT_TRIES	3U
#define I915_XD_POWER_TC_EXIT_PAUSE_US	1000U
#define I915_XD_POWER_TC_EXIT_SETTLE_US	1000U

/*
 * How often a DC state is written again while it reads back otherwise,
 * and how many reads in a row must show it before it counts as taken.
 *
 * Values referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_display_power_well.c
 * (gen9_write_dc_state).
 */
#define I915_XD_DC_REWRITES		100U
#define I915_XD_DC_STEADY_READS		5U

static const struct i915_xd_well_desc *i915_xd_power_desc(const struct i915_xd_platform *platform, unsigned well);
static uint32_t i915_xd_power_control(unsigned control);
static int i915_xd_power_tc_port(const struct i915_xd_platform *platform, const struct i915_xd_well_desc *desc, unsigned *aux, unsigned *phy_kind);
static const struct i915_xd_port_desc *i915_xd_power_combo_aux_port(const struct i915_xd_platform *platform, const struct i915_xd_well_desc *desc);
static uint32_t i915_xd_power_combo_base(unsigned phy);
static int i915_xd_power_tc_exit(struct i915_xd_io *io, struct i915_xd_well_report *report);
static int i915_xd_power_wait_fuses(struct i915_xd_io *io, unsigned gate, struct i915_xd_well_report *report);
static void i915_xd_power_release(struct i915_xd_power *power, uint64_t wells, const struct i915_xd_power_switch *sw);
static void i915_xd_power_give_owed(struct i915_xd_power *power, unsigned set, const struct i915_xd_power_switch *sw);
static void i915_xd_power_down(struct i915_xd_power *power, unsigned well, const struct i915_xd_power_switch *sw);
static int i915_xd_power_take_owed(struct i915_xd_power *power, unsigned well);
static void i915_xd_power_cancel_work(struct i915_xd_power *power);
static uint32_t i915_xd_power_dc_mask(const struct i915_xd_platform *platform);
static uint32_t i915_xd_power_dc_held_mask(const struct i915_xd_power *power);
static int i915_xd_power_dc_store(struct i915_xd_power *power, struct i915_xd_io *io, uint32_t state, struct i915_xd_dc_report *report);
static int i915_xd_power_pw2_on(const struct i915_xd_power *power, struct i915_xd_io *io);
static void i915_xd_power_dc_report_clear(struct i915_xd_dc_report *report);
static int i915_xd_power_tccold(struct i915_xd_io *io, int block, struct i915_xd_well_report *report);

/*
 * Finds the table entry of a well given by its control and its index
 * there.  Returns the entry, or -1 when the platform has no such well.
 */
int
drv_i915_xd_power_find(
	const struct i915_xd_platform *platform,
	unsigned control,
	unsigned index)
{
	const struct i915_xd_well_desc *desc;
	unsigned well;

	/* Looks at each well of the table. */
	for (well = 0U; well < platform->nwells; well++) {
		desc = &platform->wells[well];
		if (desc->control != control)
			continue;
		if (desc->index != index)
			continue;

		/* Succeeded: the well's entry. */
		return (int)well;
	}

	/* The platform has no such well. */
	return -1;
}

/*
 * Brings up a well that has a request and a state bit.
 *
 * Power well 1 waits for the fuses of power gate 0 first (and on the
 * platforms that need it sets the FLR source chicken bit); a Type-C AUX
 * well routes its channel through the Thunderbolt IO or the PHY's own;
 * the request is set (on Ice Lake a combo PHY's AUX well also turns its
 * AUX lane on, and a legacy Type-C connector's asks the firmware to
 * leave TC cold) and the state waited for (a state that does not come
 * is noted in the report and the enable goes on, as a Thunderbolt AUX
 * well with its tunnel down never reports); the DisplayPort AUX well of
 * a Type-C port on a Dekel PHY waits for its PHY's microcontroller; Ice
 * Lake's AUX A and B of a port that is not eDP take the analog override
 * (display WA #1178); a well with fuses waits for them (late fuses are
 * noted); then the owner rewrites the VGA registers and restores the
 * pipe interrupts inside the well.  Tiger Lake's TC cold well asks the
 * firmware to block TC cold instead.  Returns 0, EINVAL for a well that
 * is not switched by a request, or EIO when the time base failed (the
 * caller stops).
 *
 * Values referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_display_power_well.c
 * (icl_combo_phy_aux_power_well_enable: AUX A and B of display WA #1178).
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_display_power_well.c
 * (hsw_power_well_enable, icl_combo_phy_aux_power_well_enable,
 * icl_tc_phy_aux_power_well_enable, hsw_wait_for_power_well_enable,
 * hsw_power_well_post_enable).
 */
int
drv_i915_xd_power_well_enable(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform,
	unsigned well,
	const struct i915_xd_power_hooks *hooks,
	struct i915_xd_well_report *report)
{
	const struct i915_xd_well_desc *desc;
	const struct i915_xd_port_desc *combo;
	uint32_t control;
	uint32_t state;
	uint32_t base;
	unsigned timeout_us;
	unsigned aux;
	unsigned phy_kind;
	int tc_port;
	int legacy;
	int edp;
	int error;

	/* Starts with nothing to report. */
	report->state_late = 0;
	report->fuses_late = 0U;

	/* Refuses a well the table does not have. */
	desc = i915_xd_power_desc(platform, well);
	if (desc == NULL)
		return EINVAL;

	/* The TC cold well of Tiger Lake is a request to the firmware: block TC cold. */
	if (desc->control == I915_XD_WELL_CONTROL_MAILBOX) {
		error = i915_xd_power_tccold(io, 1, report);
		if (error != 0)
			return error;

		/* Succeeded: TC cold is blocked (or the failure is in the report). */
		return 0;
	}

	/* Refuses a well the table does not switch by a request. */
	control = i915_xd_power_control(desc->control);
	if (control == 0U)
		return EINVAL;

	/* Power well 1 needs the FLR source chicken bit where the platform asks for it, and power gate 0's fuses. */
	if (desc->fuse_gate == I915_XD_POWER_GATE_1) {
		if ((platform->wa & I915_XD_WA_PW1_FLR_SOURCE) != 0U)
			(void)drv_i915_xd_io_rmw(io, I915_XD_CHICKEN_DCPR_1, 0U, I915_XD_DCPR_1_DISABLE_FLR_SRC);
		error = i915_xd_power_wait_fuses(io, I915_XD_POWER_GATE_0, report);
		if (error != 0)
			return error;
	}

	/* A Type-C AUX well routes its channel through the Thunderbolt IO or the PHY's own. */
	tc_port = i915_xd_power_tc_port(platform, desc, &aux, &phy_kind);
	if (tc_port >= 0) {
		if ((desc->flags & I915_XD_WELL_TBT_AUX) != 0U) {
			(void)drv_i915_xd_io_rmw(io, I915_XD_DDI_AUX_CTL(aux), 0U, I915_XD_AUX_CTL_TBT_IO);
		} else {
			(void)drv_i915_xd_io_rmw(io, I915_XD_DDI_AUX_CTL(aux), I915_XD_AUX_CTL_TBT_IO, 0U);
		}
	}

	/* Requests the well. */
	(void)drv_i915_xd_io_rmw(io, control, 0U, I915_XD_PWR_WELL_REQUEST(desc->index));

	/* On Ice Lake a combo PHY's AUX lane comes on with its AUX well. */
	combo = NULL;
	if ((platform->wa & I915_XD_WA_COMBO_AUX_LANE) != 0U)
		combo = i915_xd_power_combo_aux_port(platform, desc);
	if (combo != NULL) {
		base = i915_xd_power_combo_base(combo->phy);
		(void)drv_i915_xd_io_rmw(io, I915_XD_PORT_CL_DW(base, I915_XD_POWER_CL_DW12), 0U, I915_XD_CL_DW12_AUX_LANE_ENABLE);
	}

	/* On Ice Lake the DisplayPort AUX well of a legacy Type-C connector asks the firmware to leave TC cold. */
	legacy = 0;
	if (platform->tccold == I915_XD_TCCOLD_AUX_WELL_EXIT &&
	    tc_port >= 0 &&
	    (desc->flags & I915_XD_WELL_TBT_AUX) == 0U &&
	    hooks != NULL &&
	    hooks->tc_legacy != NULL)
		legacy = hooks->tc_legacy(hooks->context, (unsigned)tc_port);
	if (legacy) {
		error = i915_xd_power_tc_exit(io, report);
		if (error != 0)
			return EIO;
	}

	/* Waits for the state; a state that does not come is noted and the enable goes on. */
	timeout_us = (unsigned)desc->enable_timeout_ms * 1000U;
	if (timeout_us == 0U)
		timeout_us = I915_XD_POWER_ENABLE_US;
	state = I915_XD_PWR_WELL_STATE(desc->index);
	error = drv_i915_xd_io_poll(io, control, state, state, timeout_us, NULL);
	if (error == EIO)
		return EIO;
	if (error != 0)
		report->state_late = 1;

	/* The DisplayPort AUX well of a Type-C port on a Dekel PHY waits for the PHY's microcontroller. */
	if (tc_port >= 0 &&
	    phy_kind == I915_XD_PHY_DEKEL &&
	    (desc->flags & I915_XD_WELL_TBT_AUX) == 0U &&
	    hooks != NULL &&
	    hooks->tc_health != NULL) {
		error = hooks->tc_health(hooks->context, (unsigned)tc_port);
		if (error == EIO)
			return EIO;
	}

	/* Ice Lake's AUX A and B of a port that is not eDP take the analog override (display WA #1178). */
	edp = 0;
	if (combo != NULL &&
	    hooks != NULL &&
	    hooks->port_edp != NULL)
		edp = hooks->port_edp(hooks->context, combo->ddi);
	if (combo != NULL &&
	    (platform->wa & I915_XD_WA_AUX_ANAOVRD_1178) != 0U &&
	    desc->index <= I915_XD_WELL_AUX(1U) &&
	    !edp) {
		base = i915_xd_power_combo_base(combo->phy);
		(void)drv_i915_xd_io_rmw(io, I915_XD_AUX_ANAOVRD1(base), 0U, I915_XD_AUX_ANAOVRD1_ENABLE | I915_XD_AUX_ANAOVRD1_LDO_BYPASS);
	}

	/* A well with fuses waits for its power gate's. */
	if (desc->fuse_gate != I915_XD_WELL_NO_FUSE) {
		error = i915_xd_power_wait_fuses(io, desc->fuse_gate, report);
		if (error != 0)
			return error;
	}

	/* The VGA registers inside the well are rewritten. */
	if ((desc->flags & I915_XD_WELL_VGA) != 0U &&
	    hooks != NULL &&
	    hooks->vga_reset != NULL)
		hooks->vga_reset(hooks->context);

	/* The pipe interrupts inside the well are restored. */
	if (desc->irq_pipes != 0U &&
	    hooks != NULL &&
	    hooks->pipes_up != NULL)
		hooks->pipes_up(hooks->context, desc->irq_pipes);

	/* Succeeded: the well is requested and up. */
	return 0;
}

/*
 * Takes down a well that has a request and a state bit.
 *
 * The owner drains the pipe interrupts inside the well first (a failed
 * drain keeps the well on); on Ice Lake a combo PHY's AUX lane goes off
 * with its AUX well; then the driver's request is withdrawn, and
 * when the BIOS does not request the well too its state is waited for (a
 * state that stays is noted).  Tiger Lake's TC cold well asks the
 * firmware to allow TC cold again instead.  Returns 0, EINVAL for a well
 * that is not switched by a request, EBUSY when the drain failed, or EIO
 * when the time base failed.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_display_power_well.c
 * (hsw_power_well_disable, hsw_power_well_pre_disable,
 * icl_combo_phy_aux_power_well_disable, hsw_wait_for_power_well_disable).
 */
int
drv_i915_xd_power_well_disable(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform,
	unsigned well,
	const struct i915_xd_power_hooks *hooks,
	struct i915_xd_well_report *report)
{
	const struct i915_xd_well_desc *desc;
	const struct i915_xd_port_desc *combo;
	uint32_t control;
	uint32_t request;
	uint32_t bios;
	uint32_t base;
	int drained;
	int error;

	/* Starts with nothing to report. */
	report->state_late = 0;
	report->fuses_late = 0U;

	/* Refuses a well the table does not have. */
	desc = i915_xd_power_desc(platform, well);
	if (desc == NULL)
		return EINVAL;

	/* The TC cold well of Tiger Lake is a request to the firmware: unblock TC cold. */
	if (desc->control == I915_XD_WELL_CONTROL_MAILBOX) {
		error = i915_xd_power_tccold(io, 0, report);
		if (error != 0)
			return error;

		/* Succeeded: TC cold is allowed again (or the failure is in the report). */
		return 0;
	}

	/* Refuses a well the table does not switch by a request. */
	control = i915_xd_power_control(desc->control);
	if (control == 0U)
		return EINVAL;

	/* The owner drains the pipe interrupts inside the well; a failed drain keeps it on. */
	if (desc->irq_pipes != 0U &&
	    hooks != NULL &&
	    hooks->pipes_down != NULL) {
		drained = hooks->pipes_down(hooks->context, desc->irq_pipes);
		if (drained != 0)
			return EBUSY;
	}

	/* On Ice Lake a combo PHY's AUX lane goes off before its AUX well. */
	combo = NULL;
	if ((platform->wa & I915_XD_WA_COMBO_AUX_LANE) != 0U)
		combo = i915_xd_power_combo_aux_port(platform, desc);
	if (combo != NULL) {
		base = i915_xd_power_combo_base(combo->phy);
		(void)drv_i915_xd_io_rmw(io, I915_XD_PORT_CL_DW(base, I915_XD_POWER_CL_DW12), I915_XD_CL_DW12_AUX_LANE_ENABLE, 0U);
	}

	/* Withdraws the driver's request. */
	request = I915_XD_PWR_WELL_REQUEST(desc->index);
	(void)drv_i915_xd_io_rmw(io, control, request, 0U);

	/* While the BIOS still requests the well its state stays, as expected. */
	bios = drv_i915_xd_io_read(io, control - I915_XD_POWER_BIOS_OFFSET);
	if ((bios & request) != 0U)
		return 0;

	/* Waits for the state to clear; a state that stays is noted. */
	error = drv_i915_xd_io_poll(io, control, I915_XD_PWR_WELL_STATE(desc->index), 0U, I915_XD_POWER_DISABLE_US, NULL);
	if (error == EIO)
		return EIO;
	if (error != 0)
		report->state_late = 1;

	/* Succeeded: the driver's request is gone. */
	return 0;
}

/*
 * Tells whether the driver owns a well that is on: its driver request
 * and its state are both set.  A well not switched by a request reads
 * as off.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_display_power_well.c
 * (hsw_power_well_enabled).
 */
int
drv_i915_xd_power_well_owned(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform,
	unsigned well)
{
	const struct i915_xd_well_desc *desc;
	uint32_t control;
	uint32_t bits;
	uint32_t value;

	/* A well the table does not switch by a request is not owned. */
	desc = i915_xd_power_desc(platform, well);
	if (desc == NULL)
		return 0;
	control = i915_xd_power_control(desc->control);
	if (control == 0U)
		return 0;

	/* Both the driver's request and the state. */
	bits = I915_XD_PWR_WELL_REQUEST(desc->index) | I915_XD_PWR_WELL_STATE(desc->index);
	value = drv_i915_xd_io_read(io, control);
	if ((value & bits) != bits)
		return 0;

	/* Succeeded: the driver owns the well and it is on. */
	return 1;
}

/*
 * Tells whether a well is on, whoever asked for it: its state bit is set.
 * A well not switched through a request register (DC off, a mailbox
 * well) gates no register and reads as on.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_display_power.c
 * (__intel_display_power_is_enabled, the readout's gate).
 */
int
drv_i915_xd_power_well_powered(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform,
	unsigned well)
{
	const struct i915_xd_well_desc *desc;
	uint32_t control;
	uint32_t value;

	/* A well the table does not have powers nothing. */
	desc = i915_xd_power_desc(platform, well);
	if (desc == NULL)
		return 0;

	/* A well without a request register gates no register. */
	control = i915_xd_power_control(desc->control);
	if (control == 0U)
		return 1;

	/* The state alone. */
	value = drv_i915_xd_io_read(io, control);
	if ((value & I915_XD_PWR_WELL_STATE(desc->index)) == 0U)
		return 0;

	/* Succeeded: the well is on. */
	return 1;
}

/*
 * Takes a well's request over from the BIOS: when the BIOS requests the
 * well, the driver's request is set first (when it is not set already)
 * and the BIOS's cleared after it, so the well never goes down between.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_display_power_well.c
 * (hsw_power_well_sync_hw).
 */
void
drv_i915_xd_power_well_adopt(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform,
	unsigned well)
{
	const struct i915_xd_well_desc *desc;
	uint32_t control;
	uint32_t request;
	uint32_t bios;
	uint32_t driver;

	/* A well the table does not switch by a request has nothing to adopt. */
	desc = i915_xd_power_desc(platform, well);
	if (desc == NULL)
		return;
	control = i915_xd_power_control(desc->control);
	if (control == 0U)
		return;

	/* Nothing to take over when the BIOS does not request the well. */
	request = I915_XD_PWR_WELL_REQUEST(desc->index);
	bios = drv_i915_xd_io_read(io, control - I915_XD_POWER_BIOS_OFFSET);
	if ((bios & request) == 0U)
		return;

	/* The driver's request first, then the BIOS's withdrawn. */
	driver = drv_i915_xd_io_read(io, control);
	if ((driver & request) == 0U)
		drv_i915_xd_io_write(io, control, driver | request);
	drv_i915_xd_io_write(io, control - I915_XD_POWER_BIOS_OFFSET, bios & ~request);
}

/*
 * Prepares the counts of a display: no well has a user or owes one to
 * the work, none is known to be on, and no work is bound.  Nothing is
 * written to the hardware.
 */
void
drv_i915_xd_power_init(
	struct i915_xd_power *power,
	const struct i915_xd_platform *platform)
{
	unsigned well;

	/* Records the platform whose wells are counted. */
	power->platform = platform;

	/* Starts every well with no user and nothing owed to the work. */
	for (well = 0U; well < I915_XD_POWER_MAX_WELLS; well++) {
		power->refs[well] = 0U;
		power->owed[0][well] = 0U;
		power->owed[1][well] = 0U;
	}

	/* Starts with no work bound and none armed. */
	power->work_armed = 0;
	power->next_delay = 0;
	power->work.queue = NULL;
	power->work.cancel = NULL;
	power->work.context = NULL;

	/* No well is known to be on until the owner records one. */
	__atomic_store_n(&power->on, 0ULL, __ATOMIC_RELEASE);

	/* Starts the diagnostics at zero. */
	power->stats.deferred = 0U;
	power->stats.taken_over = 0U;
	power->stats.work_runs = 0U;
	power->stats.work_empty = 0U;
	power->stats.released = 0U;
	power->stats.requeues = 0U;
	power->stats.flushes = 0U;
	power->stats.errors = 0U;

	/* No DC state is allowed or known to be written until the owner says. */
	power->dc_allowed = 0U;
	power->dc_target = 0U;
	power->dc_firmware = 0;
	power->dc_state = 0U;
	power->dc_writes = 0U;
	power->dc_rewrites = 0U;
}

/*
 * Binds the owner's delayed work behind deferred give-backs, or unbinds
 * it when work is NULL or has no queue (a deferred give-back is then
 * immediate).
 */
void
drv_i915_xd_power_bind_work(
	struct i915_xd_power *power,
	const struct i915_xd_power_work *work)
{
	/* Unbinds the work when none is given. */
	if (work == NULL || work->queue == NULL) {
		power->work.queue = NULL;
		power->work.cancel = NULL;
		power->work.context = NULL;
		return;
	}

	/* Takes the owner's work. */
	power->work = *work;
}

/* Reports the set of table entries (a bit each) of the wells that power one of the functions. */
uint64_t
drv_i915_xd_power_wells_of(
	const struct i915_xd_platform *platform,
	uint64_t functions)
{
	uint64_t wells;
	unsigned well;

	/* Gathers the wells whose set holds one of the functions. */
	wells = 0ULL;
	for (well = 0U; well < platform->nwells; well++) {
		/* A table larger than a set can name ends here. */
		if (well >= I915_XD_POWER_MAX_WELLS)
			break;

		/* A well that powers none of the functions is passed over. */
		if ((platform->wells[well].powers & functions) == 0ULL)
			continue;

		/* Adds the well. */
		wells |= 1ULL << well;
	}

	/* Succeeded: the wells. */
	return wells;
}

/*
 * Counts a user on every well that powers one of the functions, in the
 * table's order, bringing up each well nobody used.  A user owed to the
 * delayed work is taken over instead: that well stays as it is, and the
 * work, when nothing is owed any more, is disarmed.  When a well fails
 * to come up, the wells this call counted are given back.  Returns 0,
 * EINVAL for a table larger than the counts, or the error of the well.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_display_power.c
 * (__intel_display_power_get_domain, intel_display_power_grab_async_put_ref).
 */
int
drv_i915_xd_power_get(
	struct i915_xd_power *power,
	uint64_t functions,
	const struct i915_xd_power_switch *sw)
{
	const struct i915_xd_platform *platform;
	uint64_t wells;
	uint64_t taken;
	uint64_t bit;
	unsigned well;
	int took_over;
	int owed;
	int error;

	/* A table with more wells than the counts can hold is not used. */
	platform = power->platform;
	if (platform->nwells > I915_XD_POWER_MAX_WELLS)
		return EINVAL;

	/* Counts the user on each well of the functions, parents first. */
	wells = drv_i915_xd_power_wells_of(platform, functions);
	taken = 0ULL;
	took_over = 0;
	for (well = 0U; well < platform->nwells; well++) {
		/* A well outside the set is passed over. */
		bit = 1ULL << well;
		if ((wells & bit) == 0ULL)
			continue;

		/* A user owed to the work becomes this caller's; the well stays as it is. */
		owed = i915_xd_power_take_owed(power, well);
		if (owed) {
			took_over = 1;
			taken |= bit;
			continue;
		}

		/* The first user brings the well up; a well that fails gives back what this call took. */
		if (power->refs[well] == 0U) {
			error = sw->on(sw->context, well);
			if (error != 0) {
				i915_xd_power_release(power, taken, sw);
				return error;
			}
		}

		/* The count is the number of users of the well. */
		power->refs[well]++;
		taken |= bit;
	}

	/* Disarms the work once a taken-over user leaves it nothing owed. */
	if (took_over) {
		power->stats.taken_over++;
		owed = drv_i915_xd_power_owes(power);
		if (!owed && power->work_armed) {
			i915_xd_power_cancel_work(power);
			power->work_armed = 0;
		}
	}

	/* Succeeded: every well of the functions is up and counts the caller. */
	return 0;
}

/*
 * Gives back the caller's user of every well that powers one of the
 * functions, in the reverse of the table's order; a well whose last
 * user leaves goes down.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_display_power.c
 * (__intel_display_power_put_domain).
 */
void
drv_i915_xd_power_put(
	struct i915_xd_power *power,
	uint64_t functions,
	const struct i915_xd_power_switch *sw)
{
	uint64_t wells;

	/* Gives back one user of each well of the functions. */
	wells = drv_i915_xd_power_wells_of(power->platform, functions);
	i915_xd_power_release(power, wells, sw);
}

/*
 * Gives back the caller's user of the wells of the functions later: the
 * users are owed to the delayed work, which gives them back after the
 * delay unless a new user takes them over first.  A give-back deferred
 * while the work is armed waits for the run after, with the longest
 * delay asked for.  Without a bound work the users are given back now.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_display_power.c
 * (__intel_display_power_put_async, queue_async_put_domains_work).
 */
void
drv_i915_xd_power_put_later(
	struct i915_xd_power *power,
	uint64_t functions,
	int delay_ms,
	const struct i915_xd_power_switch *sw)
{
	uint64_t wells;
	unsigned well;
	unsigned set;
	int queued;

	/* Without a work the users are given back now. */
	if (power->work.queue == NULL) {
		drv_i915_xd_power_put(power, functions, sw);
		return;
	}

	/* Owes the users to the run of the work that is due: the armed one, or the one after. */
	power->stats.deferred++;
	wells = drv_i915_xd_power_wells_of(power->platform, functions);
	set = 0U;
	if (power->work_armed)
		set = 1U;
	for (well = 0U; well < power->platform->nwells && well < I915_XD_POWER_MAX_WELLS; well++) {
		/* A well outside the set is passed over. */
		if ((wells & (1ULL << well)) == 0ULL)
			continue;

		/* A well with no user of the caller's to owe is an inconsistency, not a count. */
		if (power->refs[well] <= (unsigned)power->owed[0][well] + power->owed[1][well]) {
			power->stats.errors++;
			continue;
		}

		/* Owes the user to the run. */
		power->owed[set][well]++;
	}

	/* An armed work takes the longest delay asked for into its next arming. */
	if (power->work_armed) {
		if (delay_ms > power->next_delay)
			power->next_delay = delay_ms;
		return;
	}

	/* Arms the work; one that was armed already means the counts lost track of it. */
	power->work_armed = 1;
	queued = power->work.queue(power->work.context, delay_ms);
	if (queued == 0)
		power->stats.errors++;
}

/*
 * Runs the delayed work: gives back the users owed to this run, then
 * arms the work again for the ones deferred meanwhile, or disarms it.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_display_power.c
 * (intel_display_power_put_async_work).
 */
void
drv_i915_xd_power_work(
	struct i915_xd_power *power,
	const struct i915_xd_power_switch *sw)
{
	unsigned well;
	int later;
	int queued;

	/* A run that finds nothing owed (taken over or flushed) has nothing to do. */
	power->stats.work_runs++;
	if (!power->work_armed) {
		power->stats.work_empty++;
		return;
	}

	/* Gives back the users owed to this run. */
	power->work_armed = 0;
	i915_xd_power_give_owed(power, 0U, sw);

	/* Moves the users deferred meanwhile up to the next run. */
	later = 0;
	for (well = 0U; well < I915_XD_POWER_MAX_WELLS; well++) {
		if (power->owed[1][well] != 0U)
			later = 1;
		power->owed[0][well] = power->owed[1][well];
		power->owed[1][well] = 0U;
	}

	/* Arms the work again for them, or disarms one armed after this run began. */
	if (later) {
		power->stats.requeues++;
		power->work_armed = 1;
		queued = power->work.queue(power->work.context, power->next_delay);
		if (queued == 0)
			power->stats.errors++;
		power->next_delay = 0;
	} else {
		i915_xd_power_cancel_work(power);
	}
}

/*
 * Gives back every user owed to the work now and disarms it.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_display_power.c
 * (intel_display_power_flush_work).
 */
void
drv_i915_xd_power_flush(
	struct i915_xd_power *power,
	const struct i915_xd_power_switch *sw)
{
	unsigned well;

	/* Nothing is owed while the work is not armed. */
	power->stats.flushes++;
	if (!power->work_armed)
		return;

	/* Joins the users deferred for the run after to the ones due now. */
	power->work_armed = 0;
	for (well = 0U; well < I915_XD_POWER_MAX_WELLS; well++) {
		power->owed[0][well] = (uint16_t)(power->owed[0][well] + power->owed[1][well]);
		power->owed[1][well] = 0U;
	}

	/* Gives them back and disarms the work. */
	i915_xd_power_give_owed(power, 0U, sw);
	i915_xd_power_cancel_work(power);
}

/* Tells whether any user is owed to the work (1 or 0). */
int
drv_i915_xd_power_owes(
	const struct i915_xd_power *power)
{
	unsigned well;

	/* Looks at both runs of every well. */
	for (well = 0U; well < I915_XD_POWER_MAX_WELLS; well++) {
		if (power->owed[0][well] != 0U)
			return 1;
		if (power->owed[1][well] != 0U)
			return 1;
	}

	/* Succeeded: nothing is owed. */
	return 0;
}

/*
 * Tells whether every well that powers one of the functions is known to
 * be on, so that their registers can be read (1 or 0).  It takes no lock
 * and writes nothing: an interrupt handler may ask.
 */
int
drv_i915_xd_power_has(
	const struct i915_xd_power *power,
	uint64_t functions)
{
	uint64_t wells;
	uint64_t on;

	/* Every well of the functions must be recorded as on. */
	wells = drv_i915_xd_power_wells_of(power->platform, functions);
	on = __atomic_load_n(&power->on, __ATOMIC_ACQUIRE);
	if ((on & wells) != wells)
		return 0;

	/* Succeeded: the functions are powered. */
	return 1;
}

/* Records whether a well of the table is on, for the readers that take no lock. */
void
drv_i915_xd_power_record(
	struct i915_xd_power *power,
	unsigned well,
	int on)
{
	uint64_t bit;

	/* A well past what a set can name is not recorded. */
	if (well >= I915_XD_POWER_MAX_WELLS)
		return;

	/* Sets or clears the well's bit. */
	bit = 1ULL << well;
	if (on) {
		(void)__atomic_fetch_or(&power->on, bit, __ATOMIC_RELEASE);
	} else {
		(void)__atomic_fetch_and(&power->on, ~bit, __ATOMIC_RELEASE);
	}
}

/*
 * Sets the DC states the options allow, the one entered when DC off
 * loses its last user, and whether the DMC firmware is loaded.  Nothing
 * is written.
 */
void
drv_i915_xd_power_dc_policy(
	struct i915_xd_power *power,
	uint32_t allowed,
	uint32_t target,
	int firmware)
{
	/* Records the policy the DC state changes follow. */
	power->dc_allowed = allowed;
	power->dc_target = target;
	power->dc_firmware = firmware;
}

/*
 * Writes a DC state and waits for it to read back: a state outside the
 * allowed ones is reduced to them, the DC bits found before the write are
 * compared with the ones last written (a DMC that ignored a request is
 * reported), the other bits of the register are kept, and the state is
 * written again while it reads back otherwise.  Returns 0, or EIO when
 * the state never read back (the report holds it).
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_display_power_well.c
 * (gen9_set_dc_state, gen9_write_dc_state).
 */
int
drv_i915_xd_power_dc_write(
	struct i915_xd_power *power,
	struct i915_xd_io *io,
	uint32_t state,
	struct i915_xd_dc_report *report)
{
	int error;

	/* Writes the state with nothing reported yet. */
	i915_xd_power_dc_report_clear(report);
	error = i915_xd_power_dc_store(power, io, state, report);
	if (error != 0)
		return error;

	/* Succeeded: the state reads back. */
	return 0;
}

/*
 * Keeps the display out of the DC states (DC off comes on): no DC5 or
 * DC6 is left set.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_display_power_well.c
 * (gen9_disable_dc_states).
 */
void
drv_i915_xd_power_dc_leave(
	struct i915_xd_power *power,
	struct i915_xd_io *io,
	struct i915_xd_dc_report *report)
{
	/* Writes no DC state; a state that does not read back is in the report. */
	(void)drv_i915_xd_power_dc_write(power, io, I915_XD_DC_STATE_OFF, report);
}

/*
 * Lets the DMC take the display into the DC state the policy allows once
 * DC off lost its last user: DC6 (noting a utility pin in PWM mode and a
 * DC6 already requested) or DC5.  Without the DMC firmware nothing is
 * written.  Returns 0, or EIO when the state never read back.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_display_power_well.c
 * (gen9_dc_off_power_well_disable, skl_enable_dc6, assert_can_enable_dc6,
 * gen9_enable_dc5).
 */
int
drv_i915_xd_power_dc_enter(
	struct i915_xd_power *power,
	struct i915_xd_io *io,
	struct i915_xd_dc_report *report)
{
	uint32_t util_pin;
	uint32_t dc_state;
	int error;

	/* Without the DMC firmware the display stays out of DC states. */
	i915_xd_power_dc_report_clear(report);
	if (!power->dc_firmware)
		return 0;

	/* Enters the target state. */
	error = 0;
	switch (power->dc_target) {
	case I915_XD_DC_STATE_DC6:
		/* A utility pin driving a PWM keeps the display out of DC6. */
		util_pin = drv_i915_xd_io_read(io, I915_XD_UTIL_PIN_CTL);
		if ((util_pin & (I915_XD_UTIL_PIN_ENABLE | I915_XD_UTIL_PIN_MODE_MASK)) == (I915_XD_UTIL_PIN_ENABLE | I915_XD_UTIL_PIN_MODE_PWM))
			report->pwm_pin = 1;

		/* DC6 should not be requested already. */
		dc_state = drv_i915_xd_io_read(io, I915_XD_DC_STATE_EN);
		if ((dc_state & I915_XD_DC_STATE_DC6) != 0U)
			report->dc6_already = 1;

		/* Writes DC6. */
		error = i915_xd_power_dc_store(power, io, I915_XD_DC_STATE_DC6, report);
		break;
	case I915_XD_DC_STATE_DC5:
		error = i915_xd_power_dc_store(power, io, I915_XD_DC_STATE_DC5, report);
		break;
	default:
		break;
	}

	/* Reports a state that never read back. */
	if (error != 0)
		return error;

	/* Succeeded: the DMC may take the state. */
	return 0;
}

/*
 * Tells whether the display is held out of DC5 and DC6 (and DC3CO when
 * it is allowed): the state the DC off well reads as on (1 or 0).
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_display_power_well.c
 * (gen9_dc_off_power_well_enabled).
 */
int
drv_i915_xd_power_dc_off_held(
	const struct i915_xd_power *power,
	struct i915_xd_io *io)
{
	uint32_t dc_state;
	uint32_t held_mask;

	/* Any DC state bit set lets the DMC power the display down. */
	dc_state = drv_i915_xd_io_read(io, I915_XD_DC_STATE_EN);
	held_mask = i915_xd_power_dc_held_mask(power);
	if ((dc_state & held_mask) != 0U)
		return 0;

	/* Succeeded: the display is held out of the DC states. */
	return 1;
}

/*
 * Clears DC5 and DC6 (and DC3CO when allowed) at the display core's
 * start, before anything else is written; the state last written is not
 * changed.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_display_power.c
 * (icl_display_core_init: gen9_set_dc_state(DC_STATE_DISABLE)).
 */
void
drv_i915_xd_power_dc_clear(
	struct i915_xd_power *power,
	struct i915_xd_io *io)
{
	/* Clears the bits, keeping the rest of the register. */
	(void)drv_i915_xd_io_rmw(io, I915_XD_DC_STATE_EN, i915_xd_power_dc_held_mask(power), 0U);
}

/*
 * Enters DC9 before suspend: refused (EALREADY) when DC9 is set already,
 * (EBUSY) while DC5 or DC6 is set or power well 2 is on, and (EINVAL)
 * while the interrupts are enabled.  Then DC9 is written and the PCH is
 * let stop the display's reference clock.  Returns 0 or the refusal;
 * the report holds what the check read and a state that did not read
 * back.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_display_power_well.c
 * (bxt_enable_dc9, assert_can_enable_dc9), and
 * drivers/gpu/drm/i915/display/intel_display_power.c
 * (icl_display_core_uninit: the reference clock's chicken bit).
 */
int
drv_i915_xd_power_dc9_enter(
	struct i915_xd_power *power,
	struct i915_xd_io *io,
	int interrupts_enabled,
	struct i915_xd_dc_report *report)
{
	uint32_t dc_state;

	/* Reads what the checks look at. */
	i915_xd_power_dc_report_clear(report);
	dc_state = drv_i915_xd_io_read(io, I915_XD_DC_STATE_EN);
	report->checked = dc_state;
	report->pw2_on = i915_xd_power_pw2_on(power, io);

	/* Refuses a DC9 that is set already. */
	if ((dc_state & I915_XD_DC_STATE_DC9) != 0U)
		return EALREADY;

	/* Refuses while the DMC may still take DC5 or DC6. */
	if ((dc_state & I915_XD_DC_STATE_DC5_DC6_MASK) != 0U)
		return EBUSY;

	/* Refuses while power well 2 is on. */
	if (report->pw2_on)
		return EBUSY;

	/* Refuses while the interrupts are enabled. */
	if (interrupts_enabled)
		return EINVAL;

	/* Writes DC9; a state that does not read back is in the report. */
	(void)i915_xd_power_dc_store(power, io, I915_XD_DC_STATE_DC9, report);

	/* Lets the PCH stop the display's reference clock while the system idles. */
	(void)drv_i915_xd_io_rmw(io, I915_XD_SOUTH_CHICKEN1, 0U, I915_XD_SOUTH_CHICKEN1_REFCLK_STOP);

	/* Succeeded: the display is in DC9. */
	return 0;
}

/*
 * Leaves DC9 after resume: the PCH's reference clock first, then, unless
 * power well 2 is on or DC5 or DC6 is set (EBUSY, which DC9 cannot have
 * left behind), no DC state is written.  Returns 0 or the refusal.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_display_power_well.c
 * (bxt_disable_dc9, assert_can_disable_dc9).
 */
int
drv_i915_xd_power_dc9_leave(
	struct i915_xd_power *power,
	struct i915_xd_io *io,
	struct i915_xd_dc_report *report)
{
	uint32_t dc_state;

	/* Gives the PCH's reference clock back. */
	i915_xd_power_dc_report_clear(report);
	(void)drv_i915_xd_io_rmw(io, I915_XD_SOUTH_CHICKEN1, I915_XD_SOUTH_CHICKEN1_REFCLK_STOP, 0U);

	/* Reads what the checks look at. */
	dc_state = drv_i915_xd_io_read(io, I915_XD_DC_STATE_EN);
	report->checked = dc_state;
	report->pw2_on = i915_xd_power_pw2_on(power, io);

	/* Refuses while power well 2 is on. */
	if (report->pw2_on)
		return EBUSY;

	/* Refuses DC5 or DC6 set alongside DC9. */
	if ((dc_state & I915_XD_DC_STATE_DC5_DC6_MASK) != 0U)
		return EBUSY;

	/* Writes no DC state; a state that does not read back is in the report. */
	(void)i915_xd_power_dc_store(power, io, I915_XD_DC_STATE_OFF, report);

	/* Succeeded: the display left DC9. */
	return 0;
}

/*
 * Turns display 9's power well 2 on: the DMC is first kept out of DC5 and
 * DC6 (it may not take the display down while the well is in use), then
 * a well not yet up is requested and waited for, with power gate 2's
 * fuses.  Returns 0, EINVAL for a platform without the well, or the
 * error of the wait.
 */
int
drv_i915_xd9_power_pw2_on(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform)
{
	struct i915_xd_well_report report;
	uint32_t dc_state;
	uint32_t power;
	int well;
	int error;

	/* No DC5 or DC6 while the well is used. */
	dc_state = drv_i915_xd_io_read(io, I915_XD_DC_STATE_EN);
	if ((dc_state & (I915_XD_DC_STATE_DC5 | I915_XD_DC_STATE_DC6)) != 0U) {
		drv_i915_xd_io_write(io, I915_XD_DC_STATE_EN, dc_state & ~(I915_XD_DC_STATE_DC5 | I915_XD_DC_STATE_DC6));
		drv_i915_xd_io_posting_read(io, I915_XD_DC_STATE_EN);
	}

	/* The well of the platform. */
	well = drv_i915_xd_power_find(platform, I915_XD_WELL_CONTROL_MAIN, I915_XD9_WELL_PW_2);
	if (well < 0)
		return EINVAL;

	/* A well already up needs nothing. */
	power = drv_i915_xd_io_read(io, I915_XD_PWR_WELL_CTL);
	if ((power & I915_XD_PWR_WELL_STATE(I915_XD9_WELL_PW_2)) != 0U)
		return 0;

	/* The request, its state and the fuses. */
	error = drv_i915_xd_power_well_enable(io, platform, (unsigned)well, NULL, &report);
	if (error != 0)
		return error;

	/* A state or fuses that never came. */
	if (report.state_late != 0 || report.fuses_late != 0U)
		return ETIMEDOUT;

	/* Succeeded: pipes B and C and the AUX channels B to D have power. */
	return 0;
}

/*
 * Turns a display 9 DDI's IO well on (the request and its state) or off
 * (the driver's request withdrawn; the state may stay for another
 * requester).  Returns 0, EINVAL for a DDI without a well, or ETIMEDOUT.
 */
int
drv_i915_xd9_power_ddi_io(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform,
	unsigned ddi,
	int on)
{
	struct i915_xd_well_report report;
	int well;
	int error;

	/* The DDI's IO well: DDI A's for A and E, the DDI's own for B to D. */
	well = -1;
	if (ddi == 0U || ddi == 4U) {
		well = drv_i915_xd_power_find(platform, I915_XD_WELL_CONTROL_MAIN, I915_XD9_WELL_DDI_IO(0U));
	} else if (ddi <= 3U) {
		well = drv_i915_xd_power_find(platform, I915_XD_WELL_CONTROL_MAIN, I915_XD9_WELL_DDI_IO(ddi));
	}
	if (well < 0)
		return EINVAL;

	/* Off: the request withdrawn. */
	if (!on) {
		error = drv_i915_xd_power_well_disable(io, platform, (unsigned)well, NULL, &report);
		if (error != 0)
			return error;
		return 0;
	}

	/* On: the request and its state. */
	error = drv_i915_xd_power_well_enable(io, platform, (unsigned)well, NULL, &report);
	if (error != 0)
		return error;
	if (report.state_late != 0)
		return ETIMEDOUT;

	/* Succeeded: the DDI's lanes have power. */
	return 0;
}

/*
 * Gives back one user of each well in a set, in the reverse of the
 * table's order; a well whose last user leaves goes down, unless the
 * owner has to keep it on (it then keeps that user for good).
 */
static void
i915_xd_power_release(
	struct i915_xd_power *power,
	uint64_t wells,
	const struct i915_xd_power_switch *sw)
{
	unsigned well;

	/* Walks the table backwards, the wells inside a parent first. */
	for (well = power->platform->nwells; well > 0U; well--) {
		/* A well outside the set is passed over. */
		if (well > I915_XD_POWER_MAX_WELLS)
			continue;
		if ((wells & (1ULL << (well - 1U))) == 0ULL)
			continue;

		/* A give-back of a user the well does not have (other than the work's) is an inconsistency, not a count. */
		if (power->refs[well - 1U] <= (unsigned)power->owed[0][well - 1U] + power->owed[1][well - 1U]) {
			power->stats.errors++;
			continue;
		}

		/* The count is the number of users; the last one takes the well down. */
		power->refs[well - 1U]--;
		if (power->refs[well - 1U] == 0U)
			i915_xd_power_down(power, well - 1U, sw);
	}
}

/*
 * Gives back every user one run of the work owes, in the reverse of the
 * table's order; a well whose last user leaves goes down.
 */
static void
i915_xd_power_give_owed(
	struct i915_xd_power *power,
	unsigned set,
	const struct i915_xd_power_switch *sw)
{
	unsigned well;
	unsigned owed;

	/* Walks the table backwards, the wells inside a parent first. */
	for (well = I915_XD_POWER_MAX_WELLS; well > 0U; well--) {
		/* A well that owes this run nothing is passed over. */
		owed = power->owed[set][well - 1U];
		if (owed == 0U)
			continue;
		power->owed[set][well - 1U] = 0U;
		power->stats.released += owed;

		/* A well with fewer users than it owes is an inconsistency: it loses only what it has. */
		if (power->refs[well - 1U] < owed) {
			power->stats.errors++;
			owed = power->refs[well - 1U];
		}

		/* The owed users leave; the last one takes the well down. */
		if (owed == 0U)
			continue;
		power->refs[well - 1U] = (uint16_t)(power->refs[well - 1U] - owed);
		if (power->refs[well - 1U] == 0U)
			i915_xd_power_down(power, well - 1U, sw);
	}
}

/*
 * Takes a well down through the owner after its last user left; a well
 * the owner has to keep on keeps one user for good, so it is not brought
 * up again by a later user.
 */
static void
i915_xd_power_down(
	struct i915_xd_power *power,
	unsigned well,
	const struct i915_xd_power_switch *sw)
{
	int error;

	/* Asks the owner to take the well down. */
	error = sw->off(sw->context, well);

	/* A well that had to stay on keeps one user. */
	if (error == EBUSY)
		power->refs[well] = 1U;
}

/*
 * Takes over one user a well owes to the work, from the run after first.
 * Reports 1 when there was one, 0 when the well owes nothing.
 */
static int
i915_xd_power_take_owed(
	struct i915_xd_power *power,
	unsigned well)
{
	/* The run after gives back last, so its user is taken first. */
	if (power->owed[1][well] != 0U) {
		power->owed[1][well]--;
		return 1;
	}

	/* Then the run that is due. */
	if (power->owed[0][well] != 0U) {
		power->owed[0][well]--;
		return 1;
	}

	/* The well owes nothing. */
	return 0;
}

/* Disarms the owner's work, and forgets the delay of a next arming. */
static void
i915_xd_power_cancel_work(
	struct i915_xd_power *power)
{
	/* Asks the owner to disarm the work. */
	if (power->work.cancel != NULL)
		(void)power->work.cancel(power->work.context, 0);

	/* No arming is due any more. */
	power->next_delay = 0;
}

/* Reports the DC state bits the platform's display has. */
static uint32_t
i915_xd_power_dc_mask(
	const struct i915_xd_platform *platform)
{
	uint32_t mask;

	/* DC5, DC6 and DC9 on every display the table describes. */
	mask = I915_XD_DC_STATE_DC5 | I915_XD_DC_STATE_DC6 | I915_XD_DC_STATE_DC9;

	/* DC3CO from display 12 on. */
	if (platform->display_ver >= 12U)
		mask |= I915_XD_DC_STATE_DC3CO;

	/* Succeeded: the bits. */
	return mask;
}

/* Reports the DC state bits that let the DMC power the display down: DC5, DC6, and DC3CO when allowed. */
static uint32_t
i915_xd_power_dc_held_mask(
	const struct i915_xd_power *power)
{
	uint32_t mask;

	/* DC5 and DC6 always. */
	mask = I915_XD_DC_STATE_DC5_DC6_MASK;

	/* DC3CO only when the options allow it. */
	if ((power->dc_allowed & I915_XD_DC_STATE_DC3CO) != 0U)
		mask |= I915_XD_DC_STATE_DC3CO;

	/* Succeeded: the bits. */
	return mask;
}

/*
 * Writes a DC state into the report already started: see
 * drv_i915_xd_power_dc_write().
 */
static int
i915_xd_power_dc_store(
	struct i915_xd_power *power,
	struct i915_xd_io *io,
	uint32_t state,
	struct i915_xd_dc_report *report)
{
	uint32_t mask;
	uint32_t value;
	uint32_t written;
	unsigned rewrites;
	unsigned steady;

	/* Keeps only the allowed states. */
	state &= power->dc_allowed;

	/* Notes DC bits that are not the ones last written: the DMC ignored a request. */
	mask = i915_xd_power_dc_mask(power->platform);
	value = drv_i915_xd_io_read(io, I915_XD_DC_STATE_EN);
	if ((value & mask) != power->dc_state) {
		report->mismatch = 1;
		report->expected = power->dc_state;
		report->found = value & mask;
	}

	/* Writes the state into the DC bits, keeping the others, and records it as the one last written. */
	written = (value & ~mask) | state;
	drv_i915_xd_io_write(io, I915_XD_DC_STATE_EN, written);
	power->dc_writes++;
	power->dc_state = written & mask;

	/* Writes it again while it reads back otherwise, until it reads back steadily. */
	rewrites = 0U;
	steady = 0U;
	while (rewrites < I915_XD_DC_REWRITES) {
		/* Reads the register back. */
		value = drv_i915_xd_io_read(io, I915_XD_DC_STATE_EN);

		/* A state that did not stick is written again. */
		if (value != written) {
			drv_i915_xd_io_write(io, I915_XD_DC_STATE_EN, written);
			rewrites++;
			steady = 0U;
			continue;
		}

		/* The state read back; enough reads in a row end the wait. */
		if (steady > I915_XD_DC_STEADY_READS)
			break;
		steady++;
	}

	/* Counts the rewrites the state needed. */
	power->dc_rewrites += rewrites;

	/* Reports a state that never read back. */
	if (value != written) {
		report->not_taken = 1;
		report->wanted = written;
		report->now = value;
		return EIO;
	}

	/* Succeeded: the state reads back. */
	return 0;
}

/* Tells whether power well 2 is on and requested by the driver (1 or 0). */
static int
i915_xd_power_pw2_on(
	const struct i915_xd_power *power,
	struct i915_xd_io *io)
{
	int well;
	int owned;

	/* A platform without power well 2 has it off. */
	well = drv_i915_xd_power_find(power->platform, I915_XD_WELL_CONTROL_MAIN, I915_XD_WELL_PW_2);
	if (well < 0)
		return 0;

	/* Asks the well's request and state. */
	owned = drv_i915_xd_power_well_owned(io, power->platform, (unsigned)well);
	if (owned)
		return 1;

	/* The well is off. */
	return 0;
}

/* Starts a DC state change's report with nothing in it. */
static void
i915_xd_power_dc_report_clear(
	struct i915_xd_dc_report *report)
{
	/* Nothing seen yet. */
	report->mismatch = 0;
	report->expected = 0U;
	report->found = 0U;
	report->not_taken = 0;
	report->wanted = 0U;
	report->now = 0U;
	report->checked = 0U;
	report->pw2_on = 0;
	report->pwm_pin = 0;
	report->dc6_already = 0;
}

/* Returns a well's entry in the platform's table, or NULL past its end. */
static const struct i915_xd_well_desc *
i915_xd_power_desc(
	const struct i915_xd_platform *platform,
	unsigned well)
{
	/* An entry past the table has no description. */
	if (well >= platform->nwells)
		return NULL;

	/* Succeeded: the entry. */
	return &platform->wells[well];
}

/* Returns the driver's request register of a kind of control, or 0 for a well without one. */
static uint32_t
i915_xd_power_control(
	unsigned control)
{
	/* Names the register of the control. */
	switch (control) {
	case I915_XD_WELL_CONTROL_MAIN:
		return I915_XD_PWR_WELL_CTL;
	case I915_XD_WELL_CONTROL_AUX:
		return I915_XD_PWR_WELL_CTL_AUX;
	case I915_XD_WELL_CONTROL_DDI:
		return I915_XD_PWR_WELL_CTL_DDI;
	default:
		break;
	}

	/* DC off and the mailbox wells have no request bit. */
	return 0U;
}

/*
 * Returns the Type-C port (0 for the first) whose AUX well this is, in
 * *aux that port's AUX channel and in *phy_kind its PHY (Dekel or MG);
 * -1 for any other well.  A Thunderbolt AUX well serves the port whose
 * Thunderbolt AUX it powers, a DisplayPort AUX well the Type-C port
 * whose AUX channel it powers.
 */
static int
i915_xd_power_tc_port(
	const struct i915_xd_platform *platform,
	const struct i915_xd_well_desc *desc,
	unsigned *aux,
	unsigned *phy_kind)
{
	const struct i915_xd_port_desc *port;
	unsigned tc;
	unsigned index;

	/* Only an AUX well can be a Type-C port's. */
	*phy_kind = I915_XD_PHY_NONE;
	if (desc->control != I915_XD_WELL_CONTROL_AUX)
		return -1;

	/* The Type-C number a Thunderbolt AUX well serves (1 for the first), from what it powers. */
	tc = 0U;
	if ((desc->flags & I915_XD_WELL_TBT_AUX) != 0U) {
		for (index = 0U; index < I915_XD_POWER_TBT_PORTS; index++) {
			if ((desc->powers & I915_XD_POWERS_TBT_AUX(index)) != 0U) {
				tc = index + 1U;
				break;
			}
		}
	}

	/* Finds the port: by its Type-C number, or by the AUX channel the well powers. */
	for (index = 0U; index < platform->nports; index++) {
		port = &platform->ports[index];
		if (port->tc == 0U)
			continue;
		if (tc != 0U && port->tc != tc)
			continue;
		if (tc == 0U && port->aux != desc->index)
			continue;

		/* Succeeded: the port's Type-C number, its AUX channel and its PHY. */
		*aux = port->aux;
		*phy_kind = port->phy_kind;
		return (int)port->tc - 1;
	}

	/* A combo PHY's AUX well, or a Thunderbolt well of a port the platform does not have. */
	return -1;
}

/*
 * Waits for a power gate's fuses to be distributed.  Late fuses are
 * noted in the report and the caller goes on.  Returns 0, or EIO when
 * the time base failed.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_display_power_well.c
 * (gen9_wait_for_power_well_fuses).
 */
static int
i915_xd_power_wait_fuses(
	struct i915_xd_io *io,
	unsigned gate,
	struct i915_xd_well_report *report)
{
	int error;

	/* Waits for the gate's bit. */
	error = drv_i915_xd_io_poll(io, I915_XD_FUSE_STATUS, I915_XD_FUSE_STATUS_PG(gate), I915_XD_FUSE_STATUS_PG(gate), I915_XD_POWER_FUSE_US, NULL);
	if (error == EIO)
		return EIO;
	if (error != 0)
		report->fuses_late |= 1U << gate;

	/* Succeeded: the wait is over. */
	return 0;
}

/*
 * Asks the firmware to block TC cold (block nonzero) or to allow it
 * again, up to three times 1 ms apart: a block the firmware answers with
 * the exit failed bit counts as failed.  A request that never succeeds
 * is noted in the report (state_late) and the well's change goes on.
 * Returns 0, or EIO when the time base failed.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (TGL_PCODE_TCCOLD, TGL_PCODE_EXIT_TCCOLD_DATA_L_BLOCK_REQ,
 * TGL_PCODE_EXIT_TCCOLD_DATA_L_UNBLOCK_REQ,
 * TGL_PCODE_EXIT_TCCOLD_DATA_L_EXIT_FAILED).
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_display_power_well.c
 * (tgl_tc_cold_request, tgl_tc_cold_off_power_well_enable,
 * tgl_tc_cold_off_power_well_disable).
 */
static int
i915_xd_power_tccold(
	struct i915_xd_io *io,
	int block,
	struct i915_xd_well_report *report)
{
	uint32_t data0;
	uint32_t data1;
	unsigned tries;
	int error;

	/* Asks until the firmware agrees or the tries run out. */
	for (tries = 1U; ; tries++) {
		/* One request: block or unblock. */
		data0 = I915_XD_MAILBOX_TCCOLD_UNBLOCK;
		if (block)
			data0 = I915_XD_MAILBOX_TCCOLD_BLOCK;
		data1 = 0U;
		error = drv_i915_xd_io_mailbox(io, I915_XD_MAILBOX_TCCOLD, &data0, &data1, I915_XD_POWER_TCCOLD_US);

		/* A block whose exit failed is a failed request. */
		if (error == 0 &&
		    block &&
		    (data0 & I915_XD_MAILBOX_TCCOLD_EXIT_FAILED) != 0U)
			error = EIO;
		if (error == 0)
			break;

		/* The last try failed: noted, the change goes on. */
		if (tries == I915_XD_POWER_TCCOLD_TRIES) {
			report->state_late = 1;
			drv_i915_xd_io_step(io, "power: TC cold request failed");
			break;
		}

		/* Waits before the next try. */
		error = drv_i915_xd_io_delay(io, I915_XD_POWER_TCCOLD_PAUSE_US);
		if (error != 0)
			return EIO;
	}

	/* Succeeded: the request was made. */
	return 0;
}

/*
 * Returns the combo port whose AUX channel a well powers, or NULL for a
 * well that is not the AUX well of a combo port.
 */
static const struct i915_xd_port_desc *
i915_xd_power_combo_aux_port(
	const struct i915_xd_platform *platform,
	const struct i915_xd_well_desc *desc)
{
	const struct i915_xd_port_desc *port;
	unsigned index;

	/* Only a DisplayPort AUX well. */
	if (desc->control != I915_XD_WELL_CONTROL_AUX)
		return NULL;
	if ((desc->flags & I915_XD_WELL_TBT_AUX) != 0U)
		return NULL;

	/* The combo port of the well's channel. */
	for (index = 0U; index < platform->nports; index++) {
		port = &platform->ports[index];
		if (port->phy_kind != I915_XD_PHY_COMBO)
			continue;
		if (port->aux != desc->index)
			continue;

		/* Succeeded: the port. */
		return port;
	}

	/* No combo port has the channel. */
	return NULL;
}

/*
 * Reports the register block of combo PHY A or B (Ice Lake has only
 * those two).
 *
 * Values referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_combo_phy_regs.h (_ICL_COMBOPHY_A,
 * _ICL_COMBOPHY_B).
 */
static uint32_t
i915_xd_power_combo_base(
	unsigned phy)
{
	/* PHY B's block. */
	if (phy == 1U)
		return I915_XD_COMBO_PHY_B;

	/* Succeeded: PHY A's block. */
	return I915_XD_COMBO_PHY_A;
}

/*
 * Asks the firmware to take Ice Lake's Type-C PHYs out of TC cold: the
 * request is made again, up to three times 1 ms apart, while the mailbox
 * is busy with another command, and an accepted one is given 1 ms to
 * finish.  A request that fails is noted in the report (state_late) and
 * the well's enable goes on: the PHY is used as the firmware left it.
 * Returns 0, or EIO when the time base failed.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_display_power_well.c
 * (icl_tc_cold_exit).
 */
static int
i915_xd_power_tc_exit(
	struct i915_xd_io *io,
	struct i915_xd_well_report *report)
{
	uint32_t data0;
	unsigned tries;
	int error;

	/* Asks while the mailbox is busy, up to the last try. */
	for (tries = 1U; ; tries++) {
		/* One request, without data. */
		data0 = 0U;
		error = drv_i915_xd_io_mailbox(io, I915_XD_MAILBOX_EXIT_TCCOLD, &data0, NULL, I915_XD_POWER_TC_EXIT_US);
		if (error != EAGAIN)
			break;
		if (tries == I915_XD_POWER_TC_EXIT_TRIES)
			break;

		/* Waits before the next try. */
		error = drv_i915_xd_io_delay(io, I915_XD_POWER_TC_EXIT_PAUSE_US);
		if (error != 0)
			return EIO;
	}

	/* A request that failed is noted; the enable goes on. */
	if (error != 0) {
		report->state_late = 1;
		drv_i915_xd_io_step(io, "power: TC cold exit failed");
		return 0;
	}

	/* The exit takes up to 1 ms to finish. */
	error = drv_i915_xd_io_delay(io, I915_XD_POWER_TC_EXIT_SETTLE_US);
	if (error != 0)
		return EIO;

	/* Succeeded: the PHYs are out of TC cold. */
	return 0;
}
