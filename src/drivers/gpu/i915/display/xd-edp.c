/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The built-in panel's sink of the display 12 and 13 code: its power
 * sequencer as the driver keeps it, its DPCD and its EDID.
 */

#include "xd-edp.h"
#include "xd-aux.h"
#include "xd-edid.h"
#include "xd-io.h"
#include "xd-panel.h"
#include "xd-platform.h"

#include "../intel/display-dpcd-xd.h"
#include "../intel/display-regs-xd-south.h"

#include <kern/kcrt.h>

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The power cycle a panel of the quirk's list needs at least, in the
 * sequencer's 100 us units (1.3 s): its VBT's is too short for the panel
 * to come back on every time.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_pps.c
 * (pps_init_delays_vbt: QUIRK_INCREASE_T12_DELAY's 1300 ms).
 */
#define I915_XD_EDP_LONG_CYCLE		13000U

/*
 * How long VDD stays on after its last use, in power cycles: long enough
 * for a run of accesses to find it on.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_pps.c
 * (edp_panel_vdd_schedule_off: panel_power_cycle_delay * 5).
 */
#define I915_XD_EDP_VDD_CYCLES		5U

/*
 * The I2C addresses of the EDID on the DDC: the EEPROM, and the segment
 * pointer that selects which 256 bytes of it the EEPROM shows.
 *
 * Values referenced: VESA E-DDC Standard, version 1.2 (the 0xA0/0xA1
 * EDID address and the 0x60 segment pointer, as 7-bit addresses).
 */
#define I915_XD_EDP_DDC_EDID		0x50U
#define I915_XD_EDP_DDC_SEGMENT		0x30U

/* The AUX channels of the combo PHYs (A to C), the only ones a built-in panel has here. */
#define I915_XD_EDP_COMBO_CHANNELS	3U

/*
 * One AUX access of the sink in progress: the references and the lock
 * it took, and whether it is the one that wanted VDD first.
 */
struct i915_xd_edp_access {
	int aux_cookie;
	int core_cookie;
	int vdd_first;
};

static int i915_xd_edp_lock(struct i915_xd_edp *edp);
static void i915_xd_edp_unlock(struct i915_xd_edp *edp, int cookie);
static void i915_xd_edp_note(const struct i915_xd_edp *edp, const char *text);
static uint64_t i915_xd_edp_now(const struct i915_xd_edp *edp);
static void i915_xd_edp_sleep_ms(struct i915_xd_edp *edp, unsigned ms);
static void i915_xd_edp_wait_since(struct i915_xd_edp *edp, uint64_t since_ms, unsigned delay_ms);
static void i915_xd_edp_wait_cycle(struct i915_xd_edp *edp);
static int i915_xd_edp_pick(struct i915_xd_edp *edp);
static void i915_xd_edp_choose(struct i915_xd_edp *edp);
static void i915_xd_edp_adopt_vdd(struct i915_xd_edp *edp);
static int i915_xd_edp_vdd_on_held(struct i915_xd_edp *edp);
static void i915_xd_edp_vdd_off_held(struct i915_xd_edp *edp);
static void i915_xd_edp_vdd_off_now(struct i915_xd_edp *edp);
static void i915_xd_edp_vdd_off_sync(struct i915_xd_edp *edp);
static void i915_xd_edp_schedule_off(struct i915_xd_edp *edp);
static void i915_xd_edp_access_begin(struct i915_xd_edp *edp, struct i915_xd_edp_access *access);
static void i915_xd_edp_access_end(struct i915_xd_edp *edp, struct i915_xd_edp_access *access);
static int i915_xd_edp_read(struct i915_xd_edp *edp, uint32_t address, uint8_t *buffer, unsigned size);
static int i915_xd_edp_caps(struct i915_xd_edp *edp, uint8_t *caps);
static int i915_xd_edp_edid_block(void *context, unsigned segment, unsigned offset, uint8_t *buffer, unsigned size);
static void i915_xd_edp_panel_on(struct i915_xd_edp *edp);
static void i915_xd_edp_panel_off(struct i915_xd_edp *edp);
static void i915_xd_edp_backlight(struct i915_xd_edp *edp, int on);

/*
 * Prepares a panel's sink: its register window, its platform and the
 * caller's environment.  Nothing is taken until drv_i915_xd_edp_start().
 */
void
drv_i915_xd_edp_init(
	struct i915_xd_edp *edp,
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform,
	const struct i915_xd_edp_env *env)
{
	/* Starts from a sink not taken. */
	kern_memset(edp, 0, sizeof(*edp));
	edp->io = io;
	edp->platform = platform;
	edp->env = *env;
}

/*
 * Takes the panel's sink: chooses the sequencer (the VBT's, else the one
 * the firmware left the panel or VDD on, else the first), programs the
 * delays it chose from the firmware's, the VBT's early ones and the
 * specification's, adopts a VDD the firmware left on, then reads the
 * receiver capabilities, the eDP display control capabilities (a sink
 * may have none) and the EDID.  VDD stays on afterwards until
 * drv_i915_xd_edp_late() knows the final delays.  Returns 0, EBUSY while
 * the sink is taken, EINVAL for a channel that is not a combo PHY's,
 * ENXIO for a sequencer that cannot drive the panel, or the error of the
 * capabilities or the EDID; a failed start lets VDD go at once.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dp.c
 * (intel_edp_init_connector, intel_edp_init_dpcd), intel_pps.c
 * (intel_pps_init, pps_init_timestamps).
 */
int
drv_i915_xd_edp_start(
	struct i915_xd_edp *edp,
	const struct i915_xd_edp_config *config)
{
	struct i915_xd_edid_ddc ddc;
	int cookie;
	int usable;
	int error;

	/* One taking at a time. */
	if (edp->live)
		return EBUSY;

	/* The panel's channel is a combo PHY's. */
	if (config->aux_channel >= I915_XD_EDP_COMBO_CHANNELS)
		return EINVAL;
	error = drv_i915_xd_aux_init(&edp->aux, config->aux_channel);
	if (error != 0)
		return EINVAL;

	/*
	 * The delays are the early ones until the late step, so VDD is kept;
	 * the power cycle counts as long over, the other delays from now.
	 */
	edp->config = *config;
	edp->initializing = 1;
	edp->vdd_wanted = 0;
	edp->firmware_known = 0;
	kern_memset(&edp->program, 0, sizeof(edp->program));
	edp->power_off_ms = 0U;
	edp->power_on_ms = i915_xd_edp_now(edp);
	edp->backlight_off_ms = edp->power_on_ms;
	edp->edp_caps_read = 0;
	edp->edid_blocks = 0U;
	edp->edid_extensions = 0U;
	edp->stage = I915_XD_EDP_STAGE_NONE;

	/* Chooses and programs the sequencer, and adopts the firmware's VDD. */
	cookie = i915_xd_edp_lock(edp);

	usable = i915_xd_edp_pick(edp);
	i915_xd_edp_choose(edp);
	drv_i915_xd_pps_write_delays(edp->io, edp->pps, &edp->program);
	i915_xd_edp_adopt_vdd(edp);

	i915_xd_edp_unlock(edp, cookie);

	/* A sequencer that cannot drive the panel ends the taking. */
	if (!usable) {
		i915_xd_edp_note(edp, "xd-edp: the panel's power sequencer is not usable\n");
		i915_xd_edp_vdd_off_sync(edp);
		return ENXIO;
	}

	/* The sequencer is chosen. */
	edp->stage = I915_XD_EDP_STAGE_PPS;

	/* The receiver capabilities: without them there is no link. */
	error = i915_xd_edp_caps(edp, edp->caps);
	if (error != 0) {
		i915_xd_edp_note(edp, "xd-edp: the panel's receiver capabilities could not be read\n");
		i915_xd_edp_vdd_off_sync(edp);
		return error;
	}

	/* The capabilities are read. */
	edp->stage = I915_XD_EDP_STAGE_CAPS;

	/* The eDP display control capabilities, which a sink may not have. */
	error = i915_xd_edp_read(edp, I915_XD_DPCD_EDP_REV, edp->edp_caps, I915_XD_EDP_CAPS_BYTES);
	if (error == 0)
		edp->edp_caps_read = 1;

	/* The EDID over the channel's I2C. */
	ddc.read_block = i915_xd_edp_edid_block;
	ddc.context = edp;
	error = drv_i915_xd_edid_read(&ddc, edp->edid, I915_XD_EDP_EDID_BLOCKS, &edp->edid_blocks, &edp->edid_extensions);
	if (error != 0) {
		i915_xd_edp_note(edp, "xd-edp: the panel's EDID could not be read\n");
		i915_xd_edp_vdd_off_sync(edp);
		return error;
	}

	/* The sink is taken, VDD still on. */
	edp->stage = I915_XD_EDP_STAGE_TAKEN;
	edp->live = 1;

	/* Succeeded: the sink's capabilities and EDID are read. */
	return 0;
}

/*
 * Finishes the sequencer once the panel's own VBT data is known: the
 * sequencer the VBT names (when the south display has two), the delays
 * chosen again with the VBT's final ones and programmed, and VDD let go
 * a while from now.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_pps.c
 * (intel_pps_init_late, pps_init_late).
 */
void
drv_i915_xd_edp_late(
	struct i915_xd_edp *edp,
	int controller,
	const struct i915_xd_pps_delays *vbt)
{
	int cookie;
	int forced;

	/* Nothing to finish without a taken sink. */
	if (!edp->live)
		return;

	/* Chooses under the sequencer's lock. */
	cookie = i915_xd_edp_lock(edp);

	/* The VBT's sequencer, when there is a choice and it names one. */
	if (edp->config.sequencers >= 2U && controller >= 0) {
		if ((unsigned)controller != edp->pps)
			i915_xd_edp_note(edp, "xd-edp: the VBT names another power sequencer than the first choice\n");
		edp->pps = (unsigned)controller;
	}

	/* The delays chosen again with the final VBT data, and programmed. */
	edp->config.controller = controller;
	edp->config.vbt = *vbt;
	kern_memset(&edp->program, 0, sizeof(edp->program));
	i915_xd_edp_choose(edp);
	drv_i915_xd_pps_write_delays(edp->io, edp->pps, &edp->program);

	/* The delays are final: VDD may go when unused. */
	edp->initializing = 0;
	forced = drv_i915_xd_pps_vdd_forced(edp->io, edp->pps);
	if (forced)
		i915_xd_edp_schedule_off(edp);

	i915_xd_edp_unlock(edp, cookie);
}

/*
 * Lets the sink go: the delayed VDD-off is called off (and waited for),
 * VDD goes at once with the AUX reference it held.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_pps.c
 * (intel_pps_vdd_off_sync).
 */
void
drv_i915_xd_edp_stop(
	struct i915_xd_edp *edp)
{
	/* Nothing to let go without a taken sink. */
	if (!edp->live)
		return;

	/* VDD off now. */
	i915_xd_edp_vdd_off_sync(edp);
	edp->live = 0;
}

/*
 * Runs one of the panel's steps a screen asks for (I915_XD_EDP_PANEL_*
 * and I915_XD_EDP_BACKLIGHT_*), with the waits the panel needs.  Returns
 * 0, or EINVAL without a taken sink or for an unknown step.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_pps.c
 * (intel_pps_on, intel_pps_off, intel_pps_vdd_on, intel_pps_vdd_off_sync,
 * intel_pps_backlight_on, intel_pps_backlight_off).
 */
int
drv_i915_xd_edp_panel(
	struct i915_xd_edp *edp,
	unsigned step)
{
	int cookie;
	int first;

	/* The steps run on a taken sink. */
	if (!edp->live)
		return EINVAL;

	/* The step. */
	switch (step) {
	case I915_XD_EDP_PANEL_ON:
		i915_xd_edp_panel_on(edp);
		break;
	case I915_XD_EDP_PANEL_OFF:
		i915_xd_edp_panel_off(edp);
		break;
	case I915_XD_EDP_PANEL_VDD_ON:
		/* VDD held on until the panel's power goes off with it. */
		cookie = i915_xd_edp_lock(edp);

		first = i915_xd_edp_vdd_on_held(edp);

		i915_xd_edp_unlock(edp, cookie);

		/* Notes a second request. */
		if (!first)
			i915_xd_edp_note(edp, "xd-edp: VDD was already asked for\n");
		break;
	case I915_XD_EDP_PANEL_VDD_OFF:
		i915_xd_edp_vdd_off_sync(edp);
		break;
	case I915_XD_EDP_BACKLIGHT_ON:
		i915_xd_edp_backlight(edp, 1);
		break;
	case I915_XD_EDP_BACKLIGHT_OFF:
		i915_xd_edp_backlight(edp, 0);
		break;
	default:
		return EINVAL;
	}

	/* Succeeded: the step ran. */
	return 0;
}

/*
 * Runs the delayed VDD-off: VDD goes unless somebody wants it again.
 * The caller's timer calls it from a context that may sleep.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_pps.c
 * (edp_panel_vdd_work).
 */
void
drv_i915_xd_edp_vdd_work(
	struct i915_xd_edp *edp)
{
	int cookie;

	/* VDD off with the sequencer held, unless it is wanted again. */
	cookie = i915_xd_edp_lock(edp);

	if (!edp->vdd_wanted)
		i915_xd_edp_vdd_off_now(edp);

	i915_xd_edp_unlock(edp, cookie);
}

/*
 * Reads size bytes of the panel's DPCD at address, with the channel's
 * power and VDD held for the access.  Returns 0, EINVAL without a taken
 * sink, or the channel's error.
 */
int
drv_i915_xd_edp_dpcd_read(
	struct i915_xd_edp *edp,
	uint32_t address,
	uint8_t *buffer,
	unsigned size)
{
	int error;

	/* The sink must be taken. */
	if (!edp->live)
		return EINVAL;

	/* The access. */
	error = i915_xd_edp_read(edp, address, buffer, size);
	if (error != 0)
		return error;

	/* Succeeded: the bytes are read. */
	return 0;
}

/*
 * Writes size bytes to the panel's DPCD at address, as
 * drv_i915_xd_edp_dpcd_read() reads.
 */
int
drv_i915_xd_edp_dpcd_write(
	struct i915_xd_edp *edp,
	uint32_t address,
	const uint8_t *buffer,
	unsigned size)
{
	struct i915_xd_edp_access access;
	int error;

	/* The sink must be taken. */
	if (!edp->live)
		return EINVAL;

	/* The access. */
	i915_xd_edp_access_begin(edp, &access);

	error = drv_i915_xd_dpcd_write(&edp->aux, edp->io, address, buffer, size);

	i915_xd_edp_access_end(edp, &access);

	/* Reports a write the sink did not take. */
	if (error != 0)
		return error;

	/* Succeeded: the bytes are written. */
	return 0;
}

/*
 * Reads the panel's receiver capabilities again (the extended ones in
 * their place when the sink has them) into caps (15 bytes).  Returns 0,
 * EINVAL without a taken sink, or the error of the read.
 */
int
drv_i915_xd_edp_read_caps(
	struct i915_xd_edp *edp,
	uint8_t *caps)
{
	int error;

	/* The sink must be taken. */
	if (!edp->live)
		return EINVAL;

	/* The capabilities. */
	error = i915_xd_edp_caps(edp, caps);
	if (error != 0)
		return error;

	/* Succeeded: the capabilities are read. */
	return 0;
}

/*
 * Takes the sequencer's lock with the display core's power, which the
 * sequencer's registers need; returns the power's cookie.
 */
static int
i915_xd_edp_lock(
	struct i915_xd_edp *edp)
{
	int cookie;

	/* The power, then the lock. */
	cookie = edp->env.power_get(edp->env.context, I915_XD_EDP_POWER_CORE);
	edp->env.lock(edp->env.context, I915_XD_EDP_LOCK_PPS, 1);

	/* Succeeded: the caller holds both. */
	return cookie;
}

/* Drops the sequencer's lock, then the display core's power it took. */
static void
i915_xd_edp_unlock(
	struct i915_xd_edp *edp,
	int cookie)
{
	/* The lock, then the power. */
	edp->env.lock(edp->env.context, I915_XD_EDP_LOCK_PPS, 0);
	if (cookie != 0)
		edp->env.power_put(edp->env.context, I915_XD_EDP_POWER_CORE, cookie);
}

/* Passes a note of something wrong to the caller. */
static void
i915_xd_edp_note(
	const struct i915_xd_edp *edp,
	const char *text)
{
	/* The caller's error hook, when it has one. */
	if (edp->env.error != NULL)
		edp->env.error(edp->env.context, text);
}

/* Reads the caller's clock in milliseconds. */
static uint64_t
i915_xd_edp_now(
	const struct i915_xd_edp *edp)
{
	uint64_t now;

	/* The caller's clock. */
	now = edp->env.now_ms(edp->env.context);

	/* Succeeded: the time. */
	return now;
}

/* Lets so many milliseconds pass. */
static void
i915_xd_edp_sleep_ms(
	struct i915_xd_edp *edp,
	unsigned ms)
{
	/* No time to pass. */
	if (ms == 0U)
		return;

	/* The window's delay, in microseconds. */
	(void)drv_i915_xd_io_delay(edp->io, ms * 1000U);
}

/*
 * Waits until delay_ms milliseconds have passed since since_ms.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_pps.c
 * (wait_remaining_ms_from_jiffies).
 */
static void
i915_xd_edp_wait_since(
	struct i915_xd_edp *edp,
	uint64_t since_ms,
	unsigned delay_ms)
{
	uint64_t now;
	uint64_t end;

	/* The time the wait ends. */
	end = since_ms + delay_ms;
	now = i915_xd_edp_now(edp);

	/* Already over. */
	if (now >= end)
		return;

	/* Sleeps the rest. */
	i915_xd_edp_sleep_ms(edp, (unsigned)(end - now));
}

/*
 * Waits for the panel's power cycle to end: the part the sequencer did
 * not time (when VDD alone went off last) is slept, then the sequencer
 * is waited for.  The caller holds the sequencer's lock.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_pps.c
 * (wait_panel_power_cycle).
 */
static void
i915_xd_edp_wait_cycle(
	struct i915_xd_edp *edp)
{
	uint64_t now;
	uint64_t off_for;
	int error;

	/* How long the panel has been off, and the rest of the cycle slept. */
	now = i915_xd_edp_now(edp);
	off_for = now - edp->power_off_ms;
	if (now < edp->power_off_ms)
		off_for = 0U;
	if (off_for < edp->waits.power_cycle_ms)
		i915_xd_edp_sleep_ms(edp, edp->waits.power_cycle_ms - (unsigned)off_for);

	/* The sequencer's own end of the cycle. */
	error = drv_i915_xd_pps_wait_cycle(edp->io, edp->pps);
	if (error != 0)
		i915_xd_edp_note(edp, "xd-edp: the panel's power cycle did not end in time\n");
}

/*
 * Chooses the panel's sequencer: the one the VBT names when the south
 * display has two (none names the first otherwise), else the first that
 * has the panel powered, else VDD on, else any.  Returns nonzero when
 * the chosen one can drive the panel.  The caller holds the sequencer's
 * lock.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_pps.c
 * (pps_initial_setup, intel_num_pps, intel_pps_is_valid).
 */
static int
i915_xd_edp_pick(
	struct i915_xd_edp *edp)
{
	int chosen;
	int usable;

	/* The VBT's, when there is a choice; the first when there is not. */
	chosen = 0;
	if (edp->config.sequencers > 1U)
		chosen = edp->config.controller;
	if (chosen >= 0 && (unsigned)chosen >= edp->config.sequencers)
		chosen = -1;

	/* The one the firmware left the panel on, then VDD on, then any. */
	if (chosen < 0)
		chosen = drv_i915_xd_pps_find(edp->io, edp->config.sequencers, I915_XD_PPS_FIND_POWERED);
	if (chosen < 0)
		chosen = drv_i915_xd_pps_find(edp->io, edp->config.sequencers, I915_XD_PPS_FIND_VDD);
	if (chosen < 0)
		chosen = drv_i915_xd_pps_find(edp->io, edp->config.sequencers, I915_XD_PPS_FIND_ANY);
	if (chosen < 0)
		chosen = 0;
	edp->pps = (unsigned)chosen;

	/* Whether it can drive the panel. */
	usable = drv_i915_xd_pps_usable(edp->io, edp->pps, edp->config.sequencers, edp->config.muxed);
	if (usable == 0)
		return 0;

	/* Succeeded: the sequencer drives the panel. */
	return 1;
}

/*
 * Chooses the panel's delays once (a set already chosen is kept): the
 * firmware's as first found, the VBT's (raised to the quirk's power
 * cycle where the machine needs it) and the specification's, and the
 * waits that go with them.  The caller holds the sequencer's lock.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_pps.c
 * (pps_init_delays, pps_init_delays_bios, pps_init_delays_vbt,
 * pps_init_delays_spec).
 */
static void
i915_xd_edp_choose(
	struct i915_xd_edp *edp)
{
	struct i915_xd_pps_delays vbt;
	int chosen;
	int vbt_set;

	/* A set already chosen is kept. */
	chosen = drv_i915_xd_pps_delays_set(&edp->program);
	if (chosen != 0)
		return;

	/* The firmware's, read the first time and kept. */
	if (!edp->firmware_known) {
		drv_i915_xd_pps_read_delays(edp->io, edp->pps, &edp->firmware);
		edp->firmware_known = 1;
	}

	/* The VBT's, with the quirk's longer power cycle, counted as the sequencer counts. */
	vbt = edp->config.vbt;
	vbt_set = drv_i915_xd_pps_delays_set(&vbt);
	if (edp->config.long_power_cycle &&
	    vbt_set != 0 &&
	    vbt.power_cycle < I915_XD_EDP_LONG_CYCLE)
		vbt.power_cycle = I915_XD_EDP_LONG_CYCLE;
	drv_i915_xd_pps_from_vbt(&vbt);

	/* Each delay and the waits. */
	drv_i915_xd_pps_choose(&edp->firmware, &vbt, &edp->program, &edp->waits);
}

/*
 * Adopts a VDD the firmware left on: it holds the AUX channel's power,
 * which the sink takes here.  The caller holds the sequencer's lock.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_pps.c
 * (pps_vdd_init).
 */
static void
i915_xd_edp_adopt_vdd(
	struct i915_xd_edp *edp)
{
	int forced;

	/* Nothing to adopt. */
	forced = drv_i915_xd_pps_vdd_forced(edp->io, edp->pps);
	if (!forced)
		return;

	/* The AUX power VDD holds, once. */
	if (edp->vdd_cookie == 0)
		edp->vdd_cookie = edp->env.power_get(edp->env.context, I915_XD_EDP_POWER_AUX);
}

/*
 * Asks for VDD: a VDD-off pending is called off, the AUX power is taken
 * for it, and a panel without power waits out its power cycle before VDD
 * comes on and its power-up delay after.  Returns nonzero when this is
 * the first ask (the asker lets VDD go again).  The caller holds the
 * sequencer's lock.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_pps.c
 * (intel_pps_vdd_on_unlocked).
 */
static int
i915_xd_edp_vdd_on_held(
	struct i915_xd_edp *edp)
{
	int first;
	int forced;
	int powered;

	/*
	 * A VDD-off pending must not undo this ask; vdd_wanted tells the
	 * delayed VDD-off and the other steps that VDD is in use.
	 */
	first = 0;
	if (!edp->vdd_wanted)
		first = 1;
	edp->env.vdd_cancel(edp->env.context, 0);
	edp->vdd_wanted = 1;

	/* VDD on already: only the asking changes. */
	forced = drv_i915_xd_pps_vdd_forced(edp->io, edp->pps);
	if (forced)
		return first;

	/* VDD holds the AUX channel's power while it is on. */
	if (edp->vdd_cookie != 0)
		i915_xd_edp_note(edp, "xd-edp: VDD was off with its power still held\n");
	if (edp->vdd_cookie == 0)
		edp->vdd_cookie = edp->env.power_get(edp->env.context, I915_XD_EDP_POWER_AUX);

	/* A panel without power ends its power cycle first. */
	powered = drv_i915_xd_pps_powered(edp->io, edp->pps);
	if (!powered)
		i915_xd_edp_wait_cycle(edp);

	/* VDD on; a panel that had no power gets its power-up delay. */
	powered = drv_i915_xd_pps_vdd_on(edp->io, edp->pps);
	if (!powered)
		i915_xd_edp_sleep_ms(edp, edp->waits.power_up_ms);

	/* Succeeded: VDD is on. */
	return first;
}

/*
 * Ends an ask for VDD: nobody wants it now, and it goes a while later.
 * The caller holds the sequencer's lock.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_pps.c
 * (intel_pps_vdd_off_unlocked).
 */
static void
i915_xd_edp_vdd_off_held(
	struct i915_xd_edp *edp)
{
	/* An end without an ask. */
	if (!edp->vdd_wanted)
		i915_xd_edp_note(edp, "xd-edp: VDD let go without being asked for\n");

	/* The delayed VDD-off may take it now. */
	edp->vdd_wanted = 0;
	i915_xd_edp_schedule_off(edp);
}

/*
 * Lets VDD go now, with the AUX power it held: a panel left without
 * power starts its power cycle.  The caller holds the sequencer's lock.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_pps.c
 * (intel_pps_vdd_off_sync_unlocked).
 */
static void
i915_xd_edp_vdd_off_now(
	struct i915_xd_edp *edp)
{
	uint32_t control;
	int forced;
	int cookie;

	/* Somebody still wants it. */
	if (edp->vdd_wanted)
		i915_xd_edp_note(edp, "xd-edp: VDD let go while asked for\n");

	/* Nothing on. */
	forced = drv_i915_xd_pps_vdd_forced(edp->io, edp->pps);
	if (!forced)
		return;

	/* VDD off; without the panel's power the power cycle starts now. */
	control = drv_i915_xd_pps_vdd_off(edp->io, edp->pps);
	if ((control & I915_XD_PP_CONTROL_POWER_ON) == 0U)
		edp->power_off_ms = i915_xd_edp_now(edp);

	/* The AUX power VDD held. */
	cookie = edp->vdd_cookie;
	edp->vdd_cookie = 0;
	if (cookie != 0)
		edp->env.power_put(edp->env.context, I915_XD_EDP_POWER_AUX, cookie);
}

/*
 * Lets VDD go now from outside the sequencer's lock: the delayed VDD-off
 * is called off and waited for first (it takes the lock itself).
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_pps.c
 * (intel_pps_vdd_off_sync).
 */
static void
i915_xd_edp_vdd_off_sync(
	struct i915_xd_edp *edp)
{
	int cookie;

	/* The delayed VDD-off, called off. */
	edp->env.vdd_cancel(edp->env.context, 1);

	/* VDD off with the sequencer held. */
	cookie = i915_xd_edp_lock(edp);

	i915_xd_edp_vdd_off_now(edp);

	i915_xd_edp_unlock(edp, cookie);
}

/*
 * Arms the delayed VDD-off five power cycles from now, long enough for a
 * run of accesses; while the delays are the early ones VDD is kept.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_pps.c
 * (edp_panel_vdd_schedule_off).
 */
static void
i915_xd_edp_schedule_off(
	struct i915_xd_edp *edp)
{
	/* Kept until the final delays are known. */
	if (edp->initializing)
		return;

	/* The caller's timer. */
	edp->env.vdd_later(edp->env.context, edp->waits.power_cycle_ms * I915_XD_EDP_VDD_CYCLES);
}

/*
 * Starts an AUX access of the sink: the channel's lock, its power, the
 * sequencer's lock and VDD.  A panel with neither its power nor VDD is
 * noted.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dp_aux.c
 * (intel_dp_aux_xfer), drivers/gpu/drm/i915/display/intel_pps.c
 * (intel_pps_check_power_unlocked).
 */
static void
i915_xd_edp_access_begin(
	struct i915_xd_edp *edp,
	struct i915_xd_edp_access *access)
{
	int powered;
	int forced;

	/* The channel, its power and the sequencer. */
	edp->env.lock(edp->env.context, I915_XD_EDP_LOCK_AUX, 1);
	access->aux_cookie = edp->env.power_get(edp->env.context, I915_XD_EDP_POWER_AUX);
	access->core_cookie = i915_xd_edp_lock(edp);

	/* VDD for the access. */
	access->vdd_first = i915_xd_edp_vdd_on_held(edp);

	/* The sink answers only with the panel's power or VDD. */
	powered = drv_i915_xd_pps_powered(edp->io, edp->pps);
	forced = drv_i915_xd_pps_vdd_forced(edp->io, edp->pps);
	if (!powered && !forced)
		i915_xd_edp_note(edp, "xd-edp: the panel is off during an AUX access\n");
}

/*
 * Ends an AUX access: VDD let go later (when this access asked first),
 * the sequencer's lock, the channel's power a little later, and the
 * channel's lock.
 */
static void
i915_xd_edp_access_end(
	struct i915_xd_edp *edp,
	struct i915_xd_edp_access *access)
{
	/* VDD, the sequencer, the power and the channel in reverse. */
	if (access->vdd_first)
		i915_xd_edp_vdd_off_held(edp);
	i915_xd_edp_unlock(edp, access->core_cookie);
	if (access->aux_cookie != 0)
		edp->env.power_put_later(edp->env.context, I915_XD_EDP_POWER_AUX, access->aux_cookie);
	edp->env.lock(edp->env.context, I915_XD_EDP_LOCK_AUX, 0);
}

/* Reads DPCD bytes in one access of the sink. */
static int
i915_xd_edp_read(
	struct i915_xd_edp *edp,
	uint32_t address,
	uint8_t *buffer,
	unsigned size)
{
	struct i915_xd_edp_access access;
	int error;

	/* The access. */
	i915_xd_edp_access_begin(edp, &access);

	error = drv_i915_xd_dpcd_read(&edp->aux, edp->io, address, buffer, size);

	i915_xd_edp_access_end(edp, &access);

	/* Reports a read the sink did not answer. */
	if (error != 0)
		return error;

	/* Succeeded: the bytes are read. */
	return 0;
}

/*
 * Reads the receiver capabilities: the fifteen bytes, which a revision
 * of 0 makes useless, then the extended ones in their place when the
 * sink says it has them and their revision is not lower.  Returns 0, EIO
 * for a revision of 0, or the error of a read.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/display/drm_dp_helper.c (drm_dp_read_dpcd_caps,
 * drm_dp_read_extended_dpcd_caps).
 */
static int
i915_xd_edp_caps(
	struct i915_xd_edp *edp,
	uint8_t *caps)
{
	uint8_t extended[I915_XD_DPCD_CAPS_SIZE];
	int error;

	/* The receiver capabilities. */
	error = i915_xd_edp_read(edp, I915_XD_DPCD_REV, caps, I915_XD_DPCD_CAPS_SIZE);
	if (error != 0)
		return error;
	if (caps[I915_XD_DPCD_REV] == 0U)
		return EIO;

	/* No extended capabilities. */
	if ((caps[I915_XD_DPCD_READ_INTERVAL] & I915_XD_DPCD_EXTENDED_PRESENT) == 0U)
		return 0;

	/* The extended ones, which replace the receiver's unless their revision is lower. */
	error = i915_xd_edp_read(edp, I915_XD_DPCD_EXTENDED_REV, extended, I915_XD_DPCD_CAPS_SIZE);
	if (error != 0)
		return error;
	if (extended[I915_XD_DPCD_REV] < caps[I915_XD_DPCD_REV])
		return 0;

	/* Takes the extended capabilities in place of the first ones. */
	kern_memcpy(caps, extended, I915_XD_DPCD_CAPS_SIZE);

	/* Succeeded: the extended capabilities are in their place. */
	return 0;
}

/*
 * Reads size bytes of an EDID block over the channel's I2C in one access
 * of the sink: the segment pointer for a segment past the first, the
 * offset within the segment, the read.  Returns 0 or the channel's error
 * (the DDC of struct i915_xd_edid_ddc).
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/drm_edid.c
 * (drm_do_probe_ddc_edid).
 */
static int
i915_xd_edp_edid_block(
	void *context,
	unsigned segment,
	unsigned offset,
	uint8_t *buffer,
	unsigned size)
{
	struct i915_xd_aux_i2c_message messages[3];
	struct i915_xd_edp_access access;
	struct i915_xd_edp *edp;
	uint8_t segment_byte;
	uint8_t offset_byte;
	unsigned first;
	int error;

	/* The panel's sink, and the bytes of the segment and the offset. */
	edp = context;
	segment_byte = (uint8_t)segment;
	offset_byte = (uint8_t)offset;

	/* The segment pointer, which only a segment past the first needs. */
	messages[0].address = I915_XD_EDP_DDC_SEGMENT;
	messages[0].read = 0;
	messages[0].bytes = &segment_byte;
	messages[0].length = 1U;
	first = 1U;
	if (segment != 0U)
		first = 0U;

	/* The offset, then the bytes. */
	messages[1].address = I915_XD_EDP_DDC_EDID;
	messages[1].read = 0;
	messages[1].bytes = &offset_byte;
	messages[1].length = 1U;
	messages[2].address = I915_XD_EDP_DDC_EDID;
	messages[2].read = 1;
	messages[2].bytes = buffer;
	messages[2].length = size;

	/* The transaction in one access. */
	i915_xd_edp_access_begin(edp, &access);

	error = drv_i915_xd_aux_i2c(&edp->aux, edp->io, &messages[first], 3U - first);

	i915_xd_edp_access_end(edp, &access);

	/* Reports a transaction that failed. */
	if (error != 0)
		return error;

	/* Succeeded: the bytes are read. */
	return 0;
}

/*
 * Powers the panel on: a panel already on is left alone and noted, the
 * power cycle is waited out, the sequencer powers the panel with the
 * delays and the backlight's wait counts from then.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_pps.c
 * (intel_pps_on_unlocked).
 */
static void
i915_xd_edp_panel_on(
	struct i915_xd_edp *edp)
{
	int cookie;
	int powered;
	int ungate;
	int error;

	/* The steps run under the sequencer's lock. */
	cookie = i915_xd_edp_lock(edp);

	/* A panel already on. */
	powered = drv_i915_xd_pps_powered(edp->io, edp->pps);
	if (powered) {
		i915_xd_edp_unlock(edp, cookie);
		i915_xd_edp_note(edp, "xd-edp: the panel's power is already on\n");
		return;
	}

	/* The cycle, then the power (display 13 ungates the DPLS clock around it). */
	i915_xd_edp_wait_cycle(edp);
	ungate = 0;
	if (edp->platform->display_ver >= 13U)
		ungate = 1;
	error = drv_i915_xd_pps_power_on(edp->io, edp->pps, &edp->program, ungate);
	if (error != 0)
		i915_xd_edp_note(edp, "xd-edp: the panel did not come on in time\n");
	edp->power_on_ms = i915_xd_edp_now(edp);

	i915_xd_edp_unlock(edp, cookie);
}

/*
 * Powers the panel off with VDD and the backlight enable, which the
 * screen held on for it; the power cycle starts and the AUX power VDD
 * held goes.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_pps.c
 * (intel_pps_off_unlocked).
 */
static void
i915_xd_edp_panel_off(
	struct i915_xd_edp *edp)
{
	int cookie;
	int vdd_cookie;
	int error;

	/* The steps run under the sequencer's lock. */
	cookie = i915_xd_edp_lock(edp);

	/* The screen asked for VDD before; nobody wants it after. */
	if (!edp->vdd_wanted)
		i915_xd_edp_note(edp, "xd-edp: the panel goes off without VDD asked for\n");
	edp->vdd_wanted = 0;

	/* Everything off together, and the cycle starts. */
	error = drv_i915_xd_pps_power_off(edp->io, edp->pps);
	if (error != 0)
		i915_xd_edp_note(edp, "xd-edp: the panel did not go off in time\n");
	edp->power_off_ms = i915_xd_edp_now(edp);

	/* The AUX power VDD held. */
	vdd_cookie = edp->vdd_cookie;
	edp->vdd_cookie = 0;
	if (vdd_cookie != 0)
		edp->env.power_put(edp->env.context, I915_XD_EDP_POWER_AUX, vdd_cookie);

	i915_xd_edp_unlock(edp, cookie);
}

/*
 * Sets or clears the backlight enable: on waits out the backlight-on
 * delay since the panel came on (so the image is solid before it shows),
 * off waits out the backlight-off delay after.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_pps.c
 * (intel_pps_backlight_on, intel_pps_backlight_off).
 */
static void
i915_xd_edp_backlight(
	struct i915_xd_edp *edp,
	int on)
{
	int cookie;

	/* The backlight-on delay since the panel came on. */
	if (on)
		i915_xd_edp_wait_since(edp, edp->power_on_ms, edp->waits.backlight_on_ms);

	/* The enable, with the sequencer held. */
	cookie = i915_xd_edp_lock(edp);

	drv_i915_xd_pps_backlight(edp->io, edp->pps, on);

	i915_xd_edp_unlock(edp, cookie);

	/* The backlight-off delay from now. */
	if (!on) {
		edp->backlight_off_ms = i915_xd_edp_now(edp);
		i915_xd_edp_wait_since(edp, edp->backlight_off_ms, edp->waits.backlight_off_ms);
	}
}
