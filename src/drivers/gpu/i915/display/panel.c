/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The eDP panel power sequencer (PPS).
 *
 * The functions are the Linux v6.8.12 drivers/gpu/drm/i915/display/
 * intel_pps.c (sha256 97f12f42d1169d6e2666596ed190800bc116d5bf52c1c47c04dd99b45257af83)
 * rewritten in this tree's style: the PPS lock, the VDD on and off paths and
 * their delayed worker, the panel-status waits, the delay selection (BIOS,
 * VBT, eDP-spec fallback), the register programming, panel power on and off
 * and the backlight-enable bit.  Their register accesses, their order, the
 * waits and the power references are the Linux text's.  The VLV/CHV
 * power-sequencer stealing, the device-wide walks and the pre-DDI paths are
 * not ported; the VLV/CHV and ILK branches that remain are never taken here.
 *
 * The messages of the Linux text are reported through the DP environment's
 * message macros, which show the format text only and never evaluate the
 * arguments (dp-internal.h).  Those arguments therefore stay inside the
 * message macros: a register read written there does not happen, exactly as
 * in the text this file was ported from.
 *
 * The Linux original is under the MIT licence:
 *
 * Copyright (C) 2020 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "dp-internal.h"
#include "dp-sink.h"
#include "panel.h"
#include <kern/kcrt.h>

/*
 * The PP_STATUS values the panel-status waits look for.
 *
 * Each pair is a mask and the value the masked status must reach: the panel
 * on and idle, the panel off, and the power cycle over.
 */
#define IDLE_ON_MASK		(PP_ON | PP_SEQUENCE_MASK | 0                     | PP_SEQUENCE_STATE_MASK)
#define IDLE_ON_VALUE		(PP_ON | PP_SEQUENCE_NONE | 0                     | PP_SEQUENCE_STATE_ON_IDLE)

#define IDLE_OFF_MASK		(PP_ON | PP_SEQUENCE_MASK | 0                     | 0)
#define IDLE_OFF_VALUE		(0     | PP_SEQUENCE_NONE | 0                     | 0)

#define IDLE_CYCLE_MASK		(PP_ON | PP_SEQUENCE_MASK | PP_CYCLE_DELAY_ACTIVE | PP_SEQUENCE_STATE_MASK)
#define IDLE_CYCLE_VALUE	(0     | PP_SEQUENCE_NONE | 0                     | PP_SEQUENCE_STATE_OFF_IDLE)

/*
 * A test the initial power-sequencer choice applies to one sequencer.
 *
 * The choice walks the sequencers with one such test after another (panel
 * on, VDD on, any) and takes the first sequencer that passes.
 */
typedef bool (*i915_pps_check)(struct drm_i915_private *dev_priv, int pps_idx);

/*
 * The registers of the power sequencer an eDP uses.
 *
 * One instance is filled on the stack for each access; pp_div is invalid
 * where the power-cycle delay lives in PP_CONTROL (every PCH from CNP on).
 */
struct i915_pps_registers {
	/* The panel power control register. */
	i915_reg_t pp_ctrl;

	/* The panel power status register. */
	i915_reg_t pp_stat;

	/* The power-on and backlight-on delays. */
	i915_reg_t pp_on;

	/* The power-off and backlight-off delays. */
	i915_reg_t pp_off;

	/* The reference divider and power-cycle delay, or an invalid register. */
	i915_reg_t pp_div;
};

static const char *i915_pps_name(struct drm_i915_private *i915, struct intel_pps *pps);
static int i915_bxt_power_sequencer_idx(struct intel_dp *intel_dp);
static bool i915_pps_has_pp_on(struct drm_i915_private *dev_priv, int pps_idx);
static bool i915_pps_has_vdd_on(struct drm_i915_private *dev_priv, int pps_idx);
static bool i915_pps_any(struct drm_i915_private *dev_priv, int pps_idx);
static int i915_num_pps(struct drm_i915_private *i915);
static bool i915_pps_is_valid(struct intel_dp *intel_dp);
static int i915_bxt_initial_pps_idx(struct drm_i915_private *i915, i915_pps_check check);
static bool i915_pps_initial_setup(struct intel_dp *intel_dp);
static void i915_pps_get_registers(struct intel_dp *intel_dp, struct i915_pps_registers *regs);
static i915_reg_t i915_pp_ctrl_reg(struct intel_dp *intel_dp);
static i915_reg_t i915_pp_stat_reg(struct intel_dp *intel_dp);
static bool i915_edp_have_panel_power(struct intel_dp *intel_dp);
static bool i915_edp_have_panel_vdd(struct intel_dp *intel_dp);
static void i915_wait_panel_status(struct intel_dp *intel_dp, u32 mask, u32 value);
static void i915_wait_panel_on(struct intel_dp *intel_dp);
static void i915_wait_panel_off(struct intel_dp *intel_dp);
static void i915_wait_panel_power_cycle(struct intel_dp *intel_dp);
static void i915_wait_backlight_on(struct intel_dp *intel_dp);
static void i915_edp_wait_backlight_off(struct intel_dp *intel_dp);
static u32 i915_ilk_get_pp_control(struct intel_dp *intel_dp);
static void i915_pps_vdd_off_sync_unlocked(struct intel_dp *intel_dp);
static void i915_edp_panel_vdd_work(struct work_struct *work);
static void i915_edp_panel_vdd_schedule_off(struct intel_dp *intel_dp);
static void i915_pps_vdd_init(struct intel_dp *intel_dp);
static void i915_pps_init_timestamps(struct intel_dp *intel_dp);
static void i915_pps_readout_hw_state(struct intel_dp *intel_dp, struct edp_power_seq *seq);
static void i915_pps_dump_state(struct intel_dp *intel_dp, const char *state_name, const struct edp_power_seq *seq);
static void i915_pps_verify_state(struct intel_dp *intel_dp);
static bool i915_pps_delays_valid(struct edp_power_seq *delays);
static void i915_pps_init_delays_bios(struct intel_dp *intel_dp, struct edp_power_seq *bios);
static void i915_pps_init_delays_vbt(struct intel_dp *intel_dp, struct edp_power_seq *vbt);
static void i915_pps_init_delays_spec(struct intel_dp *intel_dp, struct edp_power_seq *spec);
static u16 i915_pps_pick_delay(u16 cur, u16 vbt, u16 spec);
static void i915_pps_init_delays(struct intel_dp *intel_dp);
static void i915_pps_init_registers(struct intel_dp *intel_dp, bool force_disable_vdd);
static void i915_pps_init_late(struct intel_dp *intel_dp);

/*
 * Takes the PPS lock (the Linux intel_pps_lock()).
 *
 * The DISPLAY_CORE power reference comes first, then the PPS mutex; the
 * reference is returned for drv_i915_pps_unlock().  The environment never
 * reports a reference of 0, so the body a Linux with_intel_pps_lock() block
 * guards always runs once between the lock and the unlock.
 */
intel_wakeref_t
drv_i915_pps_lock(
	struct intel_dp *intel_dp)
{
	struct drm_i915_private *dev_priv;
	intel_wakeref_t wakeref;

	/* Finds the device the port belongs to. */
	dev_priv = i915_dp_dp_to_i915(intel_dp);

	/*
	 * Holds the display core powered while the sequencer is used (see the
	 * Linux intel_pps_reset_all() for why), then serializes the sequencer.
	 */
	wakeref = i915_dp_intel_display_power_get(dev_priv, POWER_DOMAIN_DISPLAY_CORE);
	I915_DP_MUTEX_LOCK(&dev_priv->display.pps.mutex);

	/* Succeeded: the caller holds the lock and the reference. */
	return wakeref;
}

/*
 * Drops the PPS lock (the Linux intel_pps_unlock()).
 *
 * The mutex goes first, then the DISPLAY_CORE reference the lock took.  It
 * returns 0, which ends a Linux with_intel_pps_lock() block.
 */
intel_wakeref_t
drv_i915_pps_unlock(
	struct intel_dp *intel_dp,
	intel_wakeref_t wakeref)
{
	struct drm_i915_private *dev_priv;

	/* Finds the device the port belongs to. */
	dev_priv = i915_dp_dp_to_i915(intel_dp);

	/* Releases the sequencer, then the display core reference. */
	I915_DP_MUTEX_UNLOCK(&dev_priv->display.pps.mutex);
	i915_dp_intel_display_power_put(dev_priv, POWER_DOMAIN_DISPLAY_CORE, wakeref);

	/* Succeeded: no reference is left to the caller. */
	return 0;
}

/*
 * Warns when an eDP panel has neither power nor VDD during an AUX transfer
 * (the Linux intel_pps_check_power_unlocked()).
 *
 * The caller holds the PPS lock.
 */
void
drv_i915_pps_check_power_unlocked(
	struct intel_dp *intel_dp)
{
	struct drm_i915_private *dev_priv;
	struct intel_digital_port *dig_port;
	bool powered;
	bool vdd;
	bool is_edp;

	/* Finds the device and the port. */
	dev_priv = i915_dp_dp_to_i915(intel_dp);
	dig_port = i915_dp_dp_to_dig_port(intel_dp);

	/* Only an eDP panel has a power sequencer. */
	is_edp = i915_dp_is_edp(intel_dp);
	if (!is_edp)
		return;

	/* A panel with power needs nothing more. */
	powered = i915_edp_have_panel_power(intel_dp);
	if (powered)
		return;

	/* A panel with VDD forced on can talk AUX too. */
	vdd = i915_edp_have_panel_vdd(intel_dp);
	if (vdd)
		return;

	/* Reports the powered-off panel and the sequencer's state. */
	(void)I915_DP_DRM_WARN(&dev_priv->drm, 1,
			       "[ENCODER:%d:%s] %s powered off while attempting AUX CH communication.\n",
			       dig_port->base.base.base.id, dig_port->base.base.name,
			       i915_pps_name(dev_priv, &intel_dp->pps));
	I915_DP_DRM_DBG_KMS(&dev_priv->drm,
			    "[ENCODER:%d:%s] %s PP_STATUS: 0x%08x PP_CONTROL: 0x%08x\n",
			    dig_port->base.base.base.id, dig_port->base.base.name,
			    i915_pps_name(dev_priv, &intel_dp->pps),
			    i915_dp_intel_de_read(dev_priv, i915_pp_stat_reg(intel_dp)),
			    i915_dp_intel_de_read(dev_priv, i915_pp_ctrl_reg(intel_dp)));
}

/*
 * Waits out the panel power cycle under the PPS lock (the Linux
 * intel_pps_wait_power_cycle()).
 */
void
drv_i915_pps_wait_power_cycle(
	struct intel_dp *intel_dp)
{
	intel_wakeref_t wakeref;
	bool is_edp;

	/* Only an eDP panel has a power sequencer. */
	is_edp = i915_dp_is_edp(intel_dp);
	if (!is_edp)
		return;

	/* Waits for the cycle with the sequencer held. */
	wakeref = drv_i915_pps_lock(intel_dp);

	i915_wait_panel_power_cycle(intel_dp);

	(void)drv_i915_pps_unlock(intel_dp, wakeref);
}

/*
 * Forces VDD on for an access to the sink (the Linux
 * intel_pps_vdd_on_unlocked()).
 *
 * It must be paired with drv_i915_pps_vdd_off_unlocked() and the caller
 * holds the PPS lock around the whole on/off sequence; it nests with
 * drv_i915_pps_vdd_on() and drv_i915_pps_vdd_off_sync().  It reports whether
 * this call is the one that must turn VDD off again.
 */
bool
drv_i915_pps_vdd_on_unlocked(
	struct intel_dp *intel_dp)
{
	struct drm_i915_private *dev_priv;
	struct intel_digital_port *dig_port;
	u32 pp;
	i915_reg_t pp_stat_reg;
	i915_reg_t pp_ctrl_reg;
	bool need_to_disable;
	bool vdd;
	bool powered;
	bool is_edp;

	/* Finds the device and the port, and whether nobody wanted VDD yet. */
	dev_priv = i915_dp_dp_to_i915(intel_dp);
	dig_port = i915_dp_dp_to_dig_port(intel_dp);
	need_to_disable = !intel_dp->pps.want_panel_vdd;

	I915_DP_LOCKDEP_ASSERT_HELD(&dev_priv->display.pps.mutex);

	/* Only an eDP panel has a power sequencer. */
	is_edp = i915_dp_is_edp(intel_dp);
	if (!is_edp)
		return false;

	/*
	 * A pending delayed VDD-off must not undo this request; want_panel_vdd
	 * tells the delayed worker and the other paths that VDD is in use.
	 */
	(void)cancel_delayed_work(&intel_dp->pps.panel_vdd_work);
	intel_dp->pps.want_panel_vdd = true;

	/* VDD already forced on: only the bookkeeping changes. */
	vdd = i915_edp_have_panel_vdd(intel_dp);
	if (vdd)
		return need_to_disable;

	/* The forced VDD holds the AUX power domain for as long as it is on. */
	(void)I915_DP_DRM_WARN_ON(&dev_priv->drm, intel_dp->pps.vdd_wakeref);
	intel_dp->pps.vdd_wakeref = i915_dp_intel_display_power_get(dev_priv, i915_aux_power_domain(dig_port));

	/* Picks the sequencer's registers. */
	pp_stat_reg = i915_pp_stat_reg(intel_dp);
	pp_ctrl_reg = i915_pp_ctrl_reg(intel_dp);

	I915_DP_DRM_DBG_KMS(&dev_priv->drm, "[ENCODER:%d:%s] %s turning VDD on\n",
			    dig_port->base.base.base.id, dig_port->base.base.name,
			    i915_pps_name(dev_priv, &intel_dp->pps));

	/* An unpowered panel must finish its power cycle before VDD comes on. */
	powered = i915_edp_have_panel_power(intel_dp);
	if (!powered)
		i915_wait_panel_power_cycle(intel_dp);

	/* Forces VDD on and posts the write. */
	pp = i915_ilk_get_pp_control(intel_dp);
	pp |= EDP_FORCE_VDD;

	i915_dp_intel_de_write(dev_priv, pp_ctrl_reg, pp);
	i915_dp_intel_de_posting_read(dev_priv, pp_ctrl_reg);
	I915_DP_DRM_DBG_KMS(&dev_priv->drm, "[ENCODER:%d:%s] %s PP_STATUS: 0x%08x PP_CONTROL: 0x%08x\n",
			    dig_port->base.base.base.id, dig_port->base.base.name,
			    i915_pps_name(dev_priv, &intel_dp->pps),
			    i915_dp_intel_de_read(dev_priv, pp_stat_reg),
			    i915_dp_intel_de_read(dev_priv, pp_ctrl_reg));

	/* If the panel was not on, gives it the power-up delay before the AUX channel is used. */
	powered = i915_edp_have_panel_power(intel_dp);
	if (!powered) {
		I915_DP_DRM_DBG_KMS(&dev_priv->drm,
				    "[ENCODER:%d:%s] %s panel power wasn't enabled\n",
				    dig_port->base.base.base.id, dig_port->base.base.name,
				    i915_pps_name(dev_priv, &intel_dp->pps));
		i915_dp_msleep(intel_dp->pps.panel_power_up_delay);
	}

	/* Succeeded: reports whether this call must turn VDD off again. */
	return need_to_disable;
}

/*
 * Forces VDD on (the Linux intel_pps_vdd_on()).
 *
 * It must be paired with drv_i915_pps_off().  Nested calls are not allowed,
 * because the lock is dropped in between; the caller prevents them.
 */
void
drv_i915_pps_vdd_on(
	struct intel_dp *intel_dp)
{
	struct drm_i915_private *i915;
	intel_wakeref_t wakeref;
	bool vdd;
	bool is_edp;

	/* Finds the device the port belongs to. */
	i915 = i915_dp_dp_to_i915(intel_dp);

	/* Only an eDP panel has a power sequencer. */
	is_edp = i915_dp_is_edp(intel_dp);
	if (!is_edp)
		return;

	/* Forces VDD on with the sequencer held. */
	vdd = false;
	wakeref = drv_i915_pps_lock(intel_dp);

	vdd = drv_i915_pps_vdd_on_unlocked(intel_dp);

	(void)drv_i915_pps_unlock(intel_dp, wakeref);

	/* Reports a VDD that someone had already requested. */
	(void)I915_STATE_WARN(i915, !vdd, "[ENCODER:%d:%s] %s VDD already requested on\n",
			      i915_dp_dp_to_dig_port(intel_dp)->base.base.base.id,
			      i915_dp_dp_to_dig_port(intel_dp)->base.base.name,
			      i915_pps_name(i915, &intel_dp->pps));
}

/*
 * Turns VDD off at once, cancelling a delayed VDD-off (the Linux
 * intel_pps_vdd_off_sync()).
 *
 * The delayed worker is cancelled before the PPS lock is taken, because the
 * worker takes that lock itself.
 */
void
drv_i915_pps_vdd_off_sync(
	struct intel_dp *intel_dp)
{
	intel_wakeref_t wakeref;
	bool is_edp;

	/* Only an eDP panel has a power sequencer. */
	is_edp = i915_dp_is_edp(intel_dp);
	if (!is_edp)
		return;

	/* Stops the delayed VDD-off and waits for a running one. */
	(void)i915_dp_cancel_delayed_work_sync(&intel_dp->pps.panel_vdd_work);

	/* VDD may still be on because of the delayed VDD-off: turns it off here. */
	wakeref = drv_i915_pps_lock(intel_dp);

	i915_pps_vdd_off_sync_unlocked(intel_dp);

	(void)drv_i915_pps_unlock(intel_dp, wakeref);
}

/*
 * Ends a VDD request, turning VDD off now or later (the Linux
 * intel_pps_vdd_off_unlocked()).
 *
 * It must be paired with drv_i915_pps_vdd_on_unlocked() and the caller holds
 * the PPS lock around the whole on/off sequence.  With sync clear, VDD stays
 * on for a while so that a following access finds it on.
 */
void
drv_i915_pps_vdd_off_unlocked(
	struct intel_dp *intel_dp,
	bool sync)
{
	struct drm_i915_private *dev_priv;
	bool is_edp;

	/* Finds the device the port belongs to. */
	dev_priv = i915_dp_dp_to_i915(intel_dp);

	I915_DP_LOCKDEP_ASSERT_HELD(&dev_priv->display.pps.mutex);

	/* Only an eDP panel has a power sequencer. */
	is_edp = i915_dp_is_edp(intel_dp);
	if (!is_edp)
		return;

	/* Reports a VDD-off without a matching VDD-on. */
	(void)I915_STATE_WARN(dev_priv, !intel_dp->pps.want_panel_vdd,
			      "[ENCODER:%d:%s] %s VDD not forced on",
			      i915_dp_dp_to_dig_port(intel_dp)->base.base.base.id,
			      i915_dp_dp_to_dig_port(intel_dp)->base.base.name,
			      i915_pps_name(dev_priv, &intel_dp->pps));

	/* Nobody wants VDD any more: the delayed worker may turn it off. */
	intel_dp->pps.want_panel_vdd = false;

	/* Turns VDD off now, or schedules the delayed VDD-off. */
	if (sync) {
		i915_pps_vdd_off_sync_unlocked(intel_dp);
	} else {
		i915_edp_panel_vdd_schedule_off(intel_dp);
	}
}

/*
 * Turns the panel power on (the Linux intel_pps_on_unlocked()).
 *
 * The caller holds the PPS lock.
 */
void
drv_i915_pps_on_unlocked(
	struct intel_dp *intel_dp)
{
	struct drm_i915_private *dev_priv;
	u32 pp;
	i915_reg_t pp_ctrl_reg;
	bool powered;
	int warned;
	int dpls_wa;
	bool is_edp;

	/* Finds the device the port belongs to. */
	dev_priv = i915_dp_dp_to_i915(intel_dp);

	I915_DP_LOCKDEP_ASSERT_HELD(&dev_priv->display.pps.mutex);

	/* Only an eDP panel has a power sequencer. */
	is_edp = i915_dp_is_edp(intel_dp);
	if (!is_edp)
		return;

	I915_DP_DRM_DBG_KMS(&dev_priv->drm, "[ENCODER:%d:%s] %s turn panel power on\n",
			    i915_dp_dp_to_dig_port(intel_dp)->base.base.base.id,
			    i915_dp_dp_to_dig_port(intel_dp)->base.base.name,
			    i915_pps_name(dev_priv, &intel_dp->pps));

	/* A panel that already has power is left alone, with a warning. */
	powered = i915_edp_have_panel_power(intel_dp);
	warned = I915_DP_DRM_WARN(&dev_priv->drm, powered,
				  "[ENCODER:%d:%s] %s panel power already on\n",
				  i915_dp_dp_to_dig_port(intel_dp)->base.base.base.id,
				  i915_dp_dp_to_dig_port(intel_dp)->base.base.name,
				  i915_pps_name(dev_priv, &intel_dp->pps));
	if (warned)
		return;

	/* The panel must have finished its last power cycle. */
	i915_wait_panel_power_cycle(intel_dp);

	/* Reads the control word. */
	pp_ctrl_reg = i915_pp_ctrl_reg(intel_dp);
	pp = i915_ilk_get_pp_control(intel_dp);

	/* ILK workaround: disables the reset around the power sequence. */
	if (IS_IRONLAKE(dev_priv)) {
		pp &= ~PANEL_POWER_RESET;
		i915_dp_intel_de_write(dev_priv, pp_ctrl_reg, pp);
		i915_dp_intel_de_posting_read(dev_priv, pp_ctrl_reg);
	}

	/*
	 * WA 22019252566: disables DPLS clock gating around the power sequence.
	 *
	 * XXX: the display version is the DP environment's fixed 13, so a
	 * display 12 (Tiger Lake) device takes this workaround too.
	 */
	dpls_wa = i915_dp_is_display_ver(dev_priv, 13, 14);
	if (dpls_wa)
		(void)i915_dp_intel_de_rmw(dev_priv, SOUTH_DSPCLK_GATE_D, 0, PCH_DPLSUNIT_CLOCK_GATE_DISABLE);

	/* Starts the power-up sequence and posts the write. */
	pp |= PANEL_POWER_ON;
	if (!IS_IRONLAKE(dev_priv))
		pp |= PANEL_POWER_RESET;

	i915_dp_intel_de_write(dev_priv, pp_ctrl_reg, pp);
	i915_dp_intel_de_posting_read(dev_priv, pp_ctrl_reg);

	/* Waits for the panel to be on; the backlight-on delay counts from here. */
	i915_wait_panel_on(intel_dp);
	intel_dp->pps.last_power_on = i915_dp_jiffies();

	/* Ends the DPLS clock-gating workaround. */
	if (dpls_wa)
		(void)i915_dp_intel_de_rmw(dev_priv, SOUTH_DSPCLK_GATE_D, PCH_DPLSUNIT_CLOCK_GATE_DISABLE, 0);

	/* ILK workaround: restores the panel reset bit. */
	if (IS_IRONLAKE(dev_priv)) {
		pp |= PANEL_POWER_RESET;
		i915_dp_intel_de_write(dev_priv, pp_ctrl_reg, pp);
		i915_dp_intel_de_posting_read(dev_priv, pp_ctrl_reg);
	}
}

/*
 * Turns the panel power on under the PPS lock (the Linux intel_pps_on()).
 */
void
drv_i915_pps_on(
	struct intel_dp *intel_dp)
{
	intel_wakeref_t wakeref;
	bool is_edp;

	/* Only an eDP panel has a power sequencer. */
	is_edp = i915_dp_is_edp(intel_dp);
	if (!is_edp)
		return;

	/* Powers the panel with the sequencer held. */
	wakeref = drv_i915_pps_lock(intel_dp);

	drv_i915_pps_on_unlocked(intel_dp);

	(void)drv_i915_pps_unlock(intel_dp, wakeref);
}

/*
 * Turns the panel power and the forced VDD off (the Linux
 * intel_pps_off_unlocked()).
 *
 * The caller holds the PPS lock and had forced VDD on; the AUX power
 * reference that VDD held is returned.
 */
void
drv_i915_pps_off_unlocked(
	struct intel_dp *intel_dp)
{
	struct drm_i915_private *dev_priv;
	struct intel_digital_port *dig_port;
	u32 pp;
	i915_reg_t pp_ctrl_reg;
	intel_wakeref_t vdd_wakeref;
	bool is_edp;

	/* Finds the device and the port. */
	dev_priv = i915_dp_dp_to_i915(intel_dp);
	dig_port = i915_dp_dp_to_dig_port(intel_dp);

	I915_DP_LOCKDEP_ASSERT_HELD(&dev_priv->display.pps.mutex);

	/* Only an eDP panel has a power sequencer. */
	is_edp = i915_dp_is_edp(intel_dp);
	if (!is_edp)
		return;

	I915_DP_DRM_DBG_KMS(&dev_priv->drm, "[ENCODER:%d:%s] %s turn panel power off\n",
			    dig_port->base.base.base.id, dig_port->base.base.name,
			    i915_pps_name(dev_priv, &intel_dp->pps));

	/* Reports a power-off without VDD forced on. */
	(void)I915_DP_DRM_WARN(&dev_priv->drm, !intel_dp->pps.want_panel_vdd,
			       "[ENCODER:%d:%s] %s need VDD to turn off panel\n",
			       dig_port->base.base.base.id, dig_port->base.base.name,
			       i915_pps_name(dev_priv, &intel_dp->pps));

	/*
	 * Switches off panel power and forced VDD together, for otherwise some
	 * panels get very unhappy and cease to work; the backlight bit goes
	 * too.
	 */
	pp = i915_ilk_get_pp_control(intel_dp);
	pp &= ~(PANEL_POWER_ON | PANEL_POWER_RESET | EDP_FORCE_VDD | EDP_BLC_ENABLE);

	pp_ctrl_reg = i915_pp_ctrl_reg(intel_dp);

	/* Nobody wants VDD any more. */
	intel_dp->pps.want_panel_vdd = false;

	/* Writes the control word and posts it. */
	i915_dp_intel_de_write(dev_priv, pp_ctrl_reg, pp);
	i915_dp_intel_de_posting_read(dev_priv, pp_ctrl_reg);

	/* Waits for the panel to be off; the power cycle counts from here. */
	i915_wait_panel_off(intel_dp);
	intel_dp->pps.panel_power_off_time = ktime_get_boottime();

	/* Returns the reference taken when VDD was enabled. */
	vdd_wakeref = intel_dp->pps.vdd_wakeref;
	intel_dp->pps.vdd_wakeref = 0;
	i915_dp_intel_display_power_put(dev_priv, i915_aux_power_domain(dig_port), vdd_wakeref);
}

/*
 * Turns the panel power off under the PPS lock (the Linux intel_pps_off()).
 */
void
drv_i915_pps_off(
	struct intel_dp *intel_dp)
{
	intel_wakeref_t wakeref;
	bool is_edp;

	/* Only an eDP panel has a power sequencer. */
	is_edp = i915_dp_is_edp(intel_dp);
	if (!is_edp)
		return;

	/* Powers the panel off with the sequencer held. */
	wakeref = drv_i915_pps_lock(intel_dp);

	drv_i915_pps_off_unlocked(intel_dp);

	(void)drv_i915_pps_unlock(intel_dp, wakeref);
}

/*
 * Sets the backlight-enable bit of the panel power control (the Linux
 * intel_pps_backlight_on()).
 *
 * The backlight-on delay since the panel power came on is waited out first:
 * a backlight enabled right away may flicker while the panel syncs with the
 * eDP link.
 */
void
drv_i915_pps_backlight_on(
	struct intel_dp *intel_dp)
{
	struct drm_i915_private *dev_priv;
	intel_wakeref_t wakeref;
	i915_reg_t pp_ctrl_reg;
	u32 pp;

	/* Finds the device the port belongs to. */
	dev_priv = i915_dp_dp_to_i915(intel_dp);

	/* Lets the image become solid before the backlight shows it. */
	i915_wait_backlight_on(intel_dp);

	/* Sets the backlight-enable bit with the sequencer held, and posts it. */
	wakeref = drv_i915_pps_lock(intel_dp);

	pp_ctrl_reg = i915_pp_ctrl_reg(intel_dp);
	pp = i915_ilk_get_pp_control(intel_dp);
	pp |= EDP_BLC_ENABLE;

	i915_dp_intel_de_write(dev_priv, pp_ctrl_reg, pp);
	i915_dp_intel_de_posting_read(dev_priv, pp_ctrl_reg);

	(void)drv_i915_pps_unlock(intel_dp, wakeref);
}

/*
 * Clears the backlight-enable bit of the panel power control (the Linux
 * intel_pps_backlight_off()).
 *
 * The backlight-off delay is waited out afterwards.
 */
void
drv_i915_pps_backlight_off(
	struct intel_dp *intel_dp)
{
	struct drm_i915_private *dev_priv;
	intel_wakeref_t wakeref;
	i915_reg_t pp_ctrl_reg;
	u32 pp;
	bool is_edp;

	/* Finds the device the port belongs to. */
	dev_priv = i915_dp_dp_to_i915(intel_dp);

	/* Only an eDP panel has a power sequencer. */
	is_edp = i915_dp_is_edp(intel_dp);
	if (!is_edp)
		return;

	/* Clears the backlight-enable bit with the sequencer held, and posts it. */
	wakeref = drv_i915_pps_lock(intel_dp);

	pp_ctrl_reg = i915_pp_ctrl_reg(intel_dp);
	pp = i915_ilk_get_pp_control(intel_dp);
	pp &= ~EDP_BLC_ENABLE;

	i915_dp_intel_de_write(dev_priv, pp_ctrl_reg, pp);
	i915_dp_intel_de_posting_read(dev_priv, pp_ctrl_reg);

	(void)drv_i915_pps_unlock(intel_dp, wakeref);

	/* Waits out the backlight-off delay from now. */
	intel_dp->pps.last_backlight_off = i915_dp_jiffies();
	i915_edp_wait_backlight_off(intel_dp);
}

/*
 * Tells whether the panel has power or forced VDD (the Linux
 * intel_pps_have_panel_power_or_vdd()).
 */
bool
drv_i915_pps_have_panel_power_or_vdd(
	struct intel_dp *intel_dp)
{
	intel_wakeref_t wakeref;
	bool have_power;
	bool vdd;

	/* Samples the power and, only without power, the VDD state with the sequencer held. */
	have_power = false;
	wakeref = drv_i915_pps_lock(intel_dp);

	have_power = i915_edp_have_panel_power(intel_dp);
	if (!have_power) {
		vdd = i915_edp_have_panel_vdd(intel_dp);
		if (vdd)
			have_power = true;
	}

	(void)drv_i915_pps_unlock(intel_dp, wakeref);

	/* Reports a panel with neither. */
	if (!have_power)
		return false;

	/* Succeeded: the panel has power or VDD. */
	return true;
}

/*
 * Reprograms the power sequencer, as on resume (the Linux
 * intel_pps_encoder_reset()).
 *
 * A VDD found on is adopted and scheduled off.
 */
void
drv_i915_pps_encoder_reset(
	struct intel_dp *intel_dp)
{
	intel_wakeref_t wakeref;
	bool vdd;
	bool is_edp;

	/* Only an eDP panel has a power sequencer. */
	is_edp = i915_dp_is_edp(intel_dp);
	if (!is_edp)
		return;

	/* Reinitializes the sequencer with it held, in case the firmware changed it. */
	wakeref = drv_i915_pps_lock(intel_dp);

	/* VLV/CHV set their sequencer up again first (never here: the platform test is constant). */
	if (IS_VALLEYVIEW(NULL) || IS_CHERRYVIEW(NULL))
		vlv_initial_power_sequencer_setup(intel_dp);

	i915_pps_init_delays(intel_dp);
	i915_pps_init_registers(intel_dp, false);
	i915_pps_vdd_init(intel_dp);

	vdd = i915_edp_have_panel_vdd(intel_dp);
	if (vdd)
		i915_edp_panel_vdd_schedule_off(intel_dp);

	(void)drv_i915_pps_unlock(intel_dp, wakeref);
}

/*
 * Sets up the power sequencer of an eDP (the Linux intel_pps_init()).
 *
 * It picks the sequencer, selects the delays, programs the registers and
 * adopts a VDD the firmware left on.  VDD stays on while the connector is
 * initializing.  It reports whether the sequencer picked is usable.
 */
bool
drv_i915_pps_init(
	struct intel_dp *intel_dp)
{
	intel_wakeref_t wakeref;
	bool valid;

	/*
	 * Keeps VDD on until the real delays are known, and prepares the
	 * delayed VDD-off.
	 */
	intel_dp->pps.initializing = true;
	i915_dp_init_delayed_work(&intel_dp->pps.panel_vdd_work, i915_edp_panel_vdd_work);

	/* Starts the power timestamps. */
	i915_pps_init_timestamps(intel_dp);

	/* Picks and programs the sequencer with it held. */
	wakeref = drv_i915_pps_lock(intel_dp);

	valid = i915_pps_initial_setup(intel_dp);

	i915_pps_init_delays(intel_dp);
	i915_pps_init_registers(intel_dp, false);
	i915_pps_vdd_init(intel_dp);

	(void)drv_i915_pps_unlock(intel_dp, wakeref);

	/* Reports an unusable sequencer. */
	if (!valid)
		return false;

	/* Succeeded: the sequencer is usable. */
	return true;
}

/*
 * Finishes the power sequencer set-up once the panel's VBT data is known
 * (the Linux intel_pps_init_late()).
 *
 * The delays are selected again, the registers reprogrammed, and the
 * delayed VDD-off is scheduled when VDD is on.
 */
void
drv_i915_pps_init_late(
	struct intel_dp *intel_dp)
{
	intel_wakeref_t wakeref;
	bool vdd;

	/* Reinitializes the delays with the sequencer held. */
	wakeref = drv_i915_pps_lock(intel_dp);

	i915_pps_init_late(intel_dp);

	kern_memset(&intel_dp->pps.pps_delays, 0, sizeof(intel_dp->pps.pps_delays));
	i915_pps_init_delays(intel_dp);
	i915_pps_init_registers(intel_dp, false);

	/* The delays are final: VDD may now be turned off when unused. */
	intel_dp->pps.initializing = false;

	vdd = i915_edp_have_panel_vdd(intel_dp);
	if (vdd)
		i915_edp_panel_vdd_schedule_off(intel_dp);

	(void)drv_i915_pps_unlock(intel_dp, wakeref);
}

/*
 * Runs one panel power operation of the modeset hooks on the live eDP.
 *
 * It is the panel hook of the modeset's register and panel hooks (struct
 * i915_lcd_emit): ctx is the panel run (struct i915_lcd_kernel), and op an
 * enum i915_lcd_panel_op.  It returns 0, or -I915_EDP_EINVAL without a live
 * eDP or for an unknown operation.
 */
int
drv_i915_edp_emit_panel(
	void *ctx,
	int op)
{
	struct i915_lcd_kernel *kernel;
	struct i915_dp_world *world;
	int done;

	/* Finds the eDP world of the run's panel. */
	kernel = ctx;
	world = drv_i915_edp_world_of(kernel->d->edp);

	/* Runs the operation on the live eDP. */
	done = drv_i915_edp_panel_op(world, op);
	if (done != 0)
		return done;

	/* Succeeded: the operation ran. */
	return 0;
}

/* Names a power sequencer for the messages. */
static const char *
i915_pps_name(
	struct drm_i915_private *i915,
	struct intel_pps *pps)
{
	UNUSED_PARAMETER(i915);

	/* VLV/CHV name the pipe their sequencer is locked to, BXT and later its index. */
	if (IS_VALLEYVIEW(i915) || IS_CHERRYVIEW(i915)) {
		/* Names the pipe the sequencer is locked to. */
		switch (pps->pps_pipe) {
		case INVALID_PIPE:
			return "PPS <none>";
		case PIPE_A:
			return "PPS A";
		case PIPE_B:
			return "PPS B";
		default:
			I915_DP_MISSING_CASE(pps->pps_pipe);
			break;
		}
	} else {
		/* Names the sequencer's index. */
		switch (pps->pps_idx) {
		case 0:
			return "PPS 0";
		case 1:
			return "PPS 1";
		default:
			I915_DP_MISSING_CASE(pps->pps_idx);
			break;
		}
	}

	/* Reports a sequencer that has no name. */
	return "PPS <invalid>";
}

/* Reports the sequencer index, reprogramming a sequencer that was reset (BXT/GLK). */
static int
i915_bxt_power_sequencer_idx(
	struct intel_dp *intel_dp)
{
	struct drm_i915_private *dev_priv;
	int pps_idx;
	bool is_edp;

	/* Finds the device and the sequencer index. */
	dev_priv = i915_dp_dp_to_i915(intel_dp);
	pps_idx = intel_dp->pps.pps_idx;

	I915_DP_LOCKDEP_ASSERT_HELD(&dev_priv->display.pps.mutex);

	/* A regular DP port never lands here. */
	is_edp = i915_dp_is_edp(intel_dp);
	(void)I915_DP_DRM_WARN_ON(&dev_priv->drm, !is_edp);

	/* A sequencer that was not reset keeps its programming. */
	if (!intel_dp->pps.pps_reset)
		return pps_idx;

	/*
	 * Only the hardware needs to be reprogrammed; the software state was
	 * fixed when the connector was initialized.
	 */
	intel_dp->pps.pps_reset = false;
	i915_pps_init_registers(intel_dp, false);

	/* Succeeded: reports the reprogrammed sequencer. */
	return pps_idx;
}

/* Tells whether a sequencer has the panel powered. */
static bool
i915_pps_has_pp_on(
	struct drm_i915_private *dev_priv,
	int pps_idx)
{
	u32 status;

	/* Reads the sequencer's status. */
	status = i915_dp_intel_de_read(dev_priv, PP_STATUS(pps_idx));

	/* Reports a panel without power. */
	if ((status & PP_ON) == 0)
		return false;

	/* Succeeded: the panel has power. */
	return true;
}

/* Tells whether a sequencer has VDD forced on. */
static bool
i915_pps_has_vdd_on(
	struct drm_i915_private *dev_priv,
	int pps_idx)
{
	u32 control;

	/* Reads the sequencer's control word. */
	control = i915_dp_intel_de_read(dev_priv, PP_CONTROL(pps_idx));

	/* Reports a sequencer without forced VDD. */
	if ((control & EDP_FORCE_VDD) == 0)
		return false;

	/* Succeeded: VDD is forced on. */
	return true;
}

/* Accepts any sequencer. */
static bool
i915_pps_any(
	struct drm_i915_private *dev_priv,
	int pps_idx)
{
	UNUSED_PARAMETER(dev_priv);
	UNUSED_PARAMETER(pps_idx);

	/* Reports that every sequencer qualifies. */
	return true;
}

/* Reports how many power sequencers the platform has. */
static int
i915_num_pps(
	struct drm_i915_private *i915)
{
	UNUSED_PARAMETER(i915);

	/* VLV/CHV have one per pipe of the two. */
	if (IS_VALLEYVIEW(i915) || IS_CHERRYVIEW(i915))
		return 2;

	/* BXT/GLK have two. */
	if (IS_GEMINILAKE(i915) || IS_BROXTON(i915))
		return 2;

	/* MTL and later PCHs have two. */
	if (INTEL_PCH_TYPE(i915) >= PCH_MTL)
		return 2;

	/* DG1 and DG2 have one. */
	if (INTEL_PCH_TYPE(i915) >= PCH_DG1)
		return 1;

	/* ICP up to ADP have two. */
	if (INTEL_PCH_TYPE(i915) >= PCH_ICP)
		return 2;

	/* Older PCHs have one. */
	return 1;
}

/* Tells whether the chosen sequencer can drive the panel. */
static bool
i915_pps_is_valid(
	struct intel_dp *intel_dp)
{
	struct drm_i915_private *i915;
	u32 chicken;

	/* Finds the device the port belongs to. */
	i915 = i915_dp_dp_to_i915(intel_dp);

	/* The second sequencer of an ICP..ADP PCH works only when its I/O is selected. */
	if (intel_dp->pps.pps_idx == 1 &&
	    INTEL_PCH_TYPE(i915) >= PCH_ICP &&
	    INTEL_PCH_TYPE(i915) <= PCH_ADP) {
		chicken = i915_dp_intel_de_read(i915, SOUTH_CHICKEN1);
		if ((chicken & ICP_SECOND_PPS_IO_SELECT) == 0)
			return false;

		return true;
	}

	/* Succeeded: any other sequencer is usable. */
	return true;
}

/* Reports the first sequencer that passes a check, or -1 when none does. */
static int
i915_bxt_initial_pps_idx(
	struct drm_i915_private *i915,
	i915_pps_check check)
{
	int pps_idx;
	int pps_num;
	bool passes;

	/* Walks the sequencers in index order. */
	pps_num = i915_num_pps(i915);
	for (pps_idx = 0; pps_idx < pps_num; pps_idx++) {
		passes = check(i915, pps_idx);
		if (passes)
			return pps_idx;
	}

	/* Reports that no sequencer passes. */
	return -1;
}

/* Picks the eDP's initial power sequencer and tells whether it is usable. */
static bool
i915_pps_initial_setup(
	struct intel_dp *intel_dp)
{
	struct intel_encoder *encoder;
	struct intel_connector *connector;
	struct drm_i915_private *i915;
	int pps_num;
	int too_large;
	bool valid;

	/* Finds the encoder, the connector and the device. */
	encoder = &i915_dp_dp_to_dig_port(intel_dp)->base;
	connector = intel_dp->attached_connector;
	i915 = i915_dp_to_i915(encoder->base.dev);

	I915_DP_LOCKDEP_ASSERT_HELD(&i915->display.pps.mutex);

	/* VLV/CHV have a sequencer set-up of their own. */
	if (IS_VALLEYVIEW(i915) || IS_CHERRYVIEW(i915)) {
		vlv_initial_power_sequencer_setup(intel_dp);
		return true;
	}

	/* Asks the VBT first. */
	pps_num = i915_num_pps(i915);
	if (pps_num > 1) {
		intel_dp->pps.pps_idx = connector->panel.vbt.backlight.controller;
	} else {
		intel_dp->pps.pps_idx = 0;
	}

	/* A controller index the platform does not have means no answer. */
	pps_num = i915_num_pps(i915);
	too_large = I915_DP_DRM_WARN_ON(&i915->drm, intel_dp->pps.pps_idx >= pps_num);
	if (too_large)
		intel_dp->pps.pps_idx = -1;

	/* The VBT was not parsed yet: picks one where the panel is on. */
	if (intel_dp->pps.pps_idx < 0)
		intel_dp->pps.pps_idx = i915_bxt_initial_pps_idx(i915, i915_pps_has_pp_on);

	/* None found: picks one where VDD is on. */
	if (intel_dp->pps.pps_idx < 0)
		intel_dp->pps.pps_idx = i915_bxt_initial_pps_idx(i915, i915_pps_has_vdd_on);

	/* None found: picks any, and says which sequencer is assumed. */
	if (intel_dp->pps.pps_idx < 0) {
		intel_dp->pps.pps_idx = i915_bxt_initial_pps_idx(i915, i915_pps_any);

		I915_DP_DRM_DBG_KMS(&i915->drm,
				    "[ENCODER:%d:%s] no initial power sequencer, assuming %s\n",
				    encoder->base.base.id, encoder->base.name,
				    i915_pps_name(i915, &intel_dp->pps));
	} else {
		I915_DP_DRM_DBG_KMS(&i915->drm,
				    "[ENCODER:%d:%s] initial power sequencer: %s\n",
				    encoder->base.base.id, encoder->base.name,
				    i915_pps_name(i915, &intel_dp->pps));
	}

	/* Reports a sequencer that cannot drive the panel. */
	valid = i915_pps_is_valid(intel_dp);
	if (!valid)
		return false;

	/* Succeeded: the chosen sequencer is usable. */
	return true;
}

/* Fills the registers of the eDP's power sequencer. */
static void
i915_pps_get_registers(
	struct intel_dp *intel_dp,
	struct i915_pps_registers *regs)
{
	struct drm_i915_private *dev_priv;
	int pps_idx;

	/* Finds the device the port belongs to. */
	dev_priv = i915_dp_dp_to_i915(intel_dp);

	/* Starts from no registers. */
	kern_memset(regs, 0, sizeof(*regs));

	/* Finds the sequencer, reprogramming a reset one on BXT/GLK. */
	if (IS_VALLEYVIEW(dev_priv) || IS_CHERRYVIEW(dev_priv)) {
		pps_idx = vlv_power_sequencer_pipe(intel_dp);
	} else if (IS_GEMINILAKE(dev_priv) || IS_BROXTON(dev_priv)) {
		pps_idx = i915_bxt_power_sequencer_idx(intel_dp);
	} else {
		pps_idx = intel_dp->pps.pps_idx;
	}

	/* The four registers every sequencer has. */
	regs->pp_ctrl = PP_CONTROL(pps_idx);
	regs->pp_stat = PP_STATUS(pps_idx);
	regs->pp_on = PP_ON_DELAYS(pps_idx);
	regs->pp_off = PP_OFF_DELAYS(pps_idx);

	/* The cycle delay moved from PP_DIVISOR to PP_CONTROL on BXT/GLK and CNP+. */
	if (IS_GEMINILAKE(dev_priv) ||
	    IS_BROXTON(dev_priv) ||
	    INTEL_PCH_TYPE(dev_priv) >= PCH_CNP) {
		regs->pp_div = INVALID_MMIO_REG;
	} else {
		regs->pp_div = PP_DIVISOR(pps_idx);
	}
}

/* Reports the eDP's panel power control register. */
static i915_reg_t
i915_pp_ctrl_reg(
	struct intel_dp *intel_dp)
{
	struct i915_pps_registers regs;

	/* Looks up the sequencer's registers. */
	i915_pps_get_registers(intel_dp, &regs);

	/* Succeeded: reports the control register. */
	return regs.pp_ctrl;
}

/* Reports the eDP's panel power status register. */
static i915_reg_t
i915_pp_stat_reg(
	struct intel_dp *intel_dp)
{
	struct i915_pps_registers regs;

	/* Looks up the sequencer's registers. */
	i915_pps_get_registers(intel_dp, &regs);

	/* Succeeded: reports the status register. */
	return regs.pp_stat;
}

/* Tells whether the panel has power. */
static bool
i915_edp_have_panel_power(
	struct intel_dp *intel_dp)
{
	struct drm_i915_private *dev_priv;
	u32 status;

	/* Finds the device the port belongs to. */
	dev_priv = i915_dp_dp_to_i915(intel_dp);

	I915_DP_LOCKDEP_ASSERT_HELD(&dev_priv->display.pps.mutex);

	/* A VLV/CHV port without a sequencer has no power. */
	if ((IS_VALLEYVIEW(dev_priv) || IS_CHERRYVIEW(dev_priv)) &&
	    intel_dp->pps.pps_pipe == INVALID_PIPE)
		return false;

	/* Reads the panel status. */
	status = i915_dp_intel_de_read(dev_priv, i915_pp_stat_reg(intel_dp));

	/* Reports a panel without power. */
	if ((status & PP_ON) == 0)
		return false;

	/* Succeeded: the panel has power. */
	return true;
}

/* Tells whether the panel has VDD forced on. */
static bool
i915_edp_have_panel_vdd(
	struct intel_dp *intel_dp)
{
	struct drm_i915_private *dev_priv;
	u32 control;

	/* Finds the device the port belongs to. */
	dev_priv = i915_dp_dp_to_i915(intel_dp);

	I915_DP_LOCKDEP_ASSERT_HELD(&dev_priv->display.pps.mutex);

	/* A VLV/CHV port without a sequencer has no VDD. */
	if ((IS_VALLEYVIEW(dev_priv) || IS_CHERRYVIEW(dev_priv)) &&
	    intel_dp->pps.pps_pipe == INVALID_PIPE)
		return false;

	/* Reads the panel control word. */
	control = i915_dp_intel_de_read(dev_priv, i915_pp_ctrl_reg(intel_dp));

	/* Reports a panel without forced VDD. */
	if ((control & EDP_FORCE_VDD) == 0)
		return false;

	/* Succeeded: VDD is forced on. */
	return true;
}

/* Waits up to 5 s for the masked panel status to reach a value. */
static void
i915_wait_panel_status(
	struct intel_dp *intel_dp,
	u32 mask,
	u32 value)
{
	struct drm_i915_private *dev_priv;
	struct intel_digital_port *dig_port;
	i915_reg_t pp_stat_reg;
	i915_reg_t pp_ctrl_reg;
	int waited;

	/* Finds the device and the port. */
	dev_priv = i915_dp_dp_to_i915(intel_dp);
	dig_port = i915_dp_dp_to_dig_port(intel_dp);

	I915_DP_LOCKDEP_ASSERT_HELD(&dev_priv->display.pps.mutex);

	/* Compares the delays the hardware holds with the ones selected. */
	i915_pps_verify_state(intel_dp);

	/* Picks the sequencer's registers. */
	pp_stat_reg = i915_pp_stat_reg(intel_dp);
	pp_ctrl_reg = i915_pp_ctrl_reg(intel_dp);

	I915_DP_DRM_DBG_KMS(&dev_priv->drm,
			    "[ENCODER:%d:%s] %s mask: 0x%08x value: 0x%08x PP_STATUS: 0x%08x PP_CONTROL: 0x%08x\n",
			    dig_port->base.base.base.id, dig_port->base.base.name,
			    i915_pps_name(dev_priv, &intel_dp->pps),
			    mask, value,
			    i915_dp_intel_de_read(dev_priv, pp_stat_reg),
			    i915_dp_intel_de_read(dev_priv, pp_ctrl_reg));

	/* Waits for the status; a timeout is reported and the sequence goes on. */
	waited = i915_dp_intel_de_wait_for_register(dev_priv, pp_stat_reg, mask, value, 5000);
	if (waited != 0) {
		I915_DP_DRM_ERR(&dev_priv->drm,
				"[ENCODER:%d:%s] %s panel status timeout: PP_STATUS: 0x%08x PP_CONTROL: 0x%08x\n",
				dig_port->base.base.base.id, dig_port->base.base.name,
				i915_pps_name(dev_priv, &intel_dp->pps),
				i915_dp_intel_de_read(dev_priv, pp_stat_reg),
				i915_dp_intel_de_read(dev_priv, pp_ctrl_reg));
	}

	I915_DP_DRM_DBG_KMS(&dev_priv->drm, "Wait complete\n");
}

/* Waits for the panel to be on and idle. */
static void
i915_wait_panel_on(
	struct intel_dp *intel_dp)
{
	struct drm_i915_private *i915;
	struct intel_digital_port *dig_port;

	/* Finds the device and the port. */
	i915 = i915_dp_dp_to_i915(intel_dp);
	dig_port = i915_dp_dp_to_dig_port(intel_dp);

	I915_DP_DRM_DBG_KMS(&i915->drm, "[ENCODER:%d:%s] %s wait for panel power on\n",
			    dig_port->base.base.base.id, dig_port->base.base.name,
			    i915_pps_name(i915, &intel_dp->pps));

	/* Waits for the on-and-idle status. */
	i915_wait_panel_status(intel_dp, IDLE_ON_MASK, IDLE_ON_VALUE);
}

/* Waits for the panel to be off. */
static void
i915_wait_panel_off(
	struct intel_dp *intel_dp)
{
	struct drm_i915_private *i915;
	struct intel_digital_port *dig_port;

	/* Finds the device and the port. */
	i915 = i915_dp_dp_to_i915(intel_dp);
	dig_port = i915_dp_dp_to_dig_port(intel_dp);

	I915_DP_DRM_DBG_KMS(&i915->drm, "[ENCODER:%d:%s] %s wait for panel power off time\n",
			    dig_port->base.base.base.id, dig_port->base.base.name,
			    i915_pps_name(i915, &intel_dp->pps));

	/* Waits for the off status. */
	i915_wait_panel_status(intel_dp, IDLE_OFF_MASK, IDLE_OFF_VALUE);
}

/*
 * Waits for the panel power cycle (T11/T12) to finish.
 *
 * When VDD was the last thing turned off, the sequencer did not time the
 * cycle, so the remaining time since the power went off is slept here
 * before the status is waited for.
 */
static void
i915_wait_panel_power_cycle(
	struct intel_dp *intel_dp)
{
	struct drm_i915_private *i915;
	struct intel_digital_port *dig_port;
	ktime_t panel_power_on_time;
	s64 panel_power_off_duration;
	unsigned long now;

	/* Finds the device and the port. */
	i915 = i915_dp_dp_to_i915(intel_dp);
	dig_port = i915_dp_dp_to_dig_port(intel_dp);

	I915_DP_DRM_DBG_KMS(&i915->drm, "[ENCODER:%d:%s] %s wait for panel power cycle\n",
			    dig_port->base.base.base.id, dig_port->base.base.name,
			    i915_pps_name(i915, &intel_dp->pps));

	/* Measures how long the panel has been off. */
	panel_power_on_time = ktime_get_boottime();
	panel_power_off_duration = ktime_ms_delta(panel_power_on_time, intel_dp->pps.panel_power_off_time);

	/* Sleeps for what remains of the power-cycle delay. */
	if (panel_power_off_duration < (s64)intel_dp->pps.panel_power_cycle_delay) {
		now = i915_dp_jiffies();
		i915_wait_remaining_ms_from_jiffies(now, (int)(intel_dp->pps.panel_power_cycle_delay - panel_power_off_duration));
	}

	/* Waits for the sequencer to report the cycle over. */
	i915_wait_panel_status(intel_dp, IDLE_CYCLE_MASK, IDLE_CYCLE_VALUE);
}

/* Waits out the backlight-on delay since the panel power came on. */
static void
i915_wait_backlight_on(
	struct intel_dp *intel_dp)
{
	/* Sleeps for what remains of the delay. */
	i915_wait_remaining_ms_from_jiffies(intel_dp->pps.last_power_on, intel_dp->pps.backlight_on_delay);
}

/* Waits out the backlight-off delay since the backlight went off. */
static void
i915_edp_wait_backlight_off(
	struct intel_dp *intel_dp)
{
	/* Sleeps for what remains of the delay. */
	i915_wait_remaining_ms_from_jiffies(intel_dp->pps.last_backlight_off, intel_dp->pps.backlight_off_delay);
}

/* Reads the panel control word, with the unlock key where the register is locked. */
static u32
i915_ilk_get_pp_control(
	struct intel_dp *intel_dp)
{
	struct drm_i915_private *dev_priv;
	u32 control;
	int has_ddi;
	int locked;
	int warned;

	/* Finds the device the port belongs to. */
	dev_priv = i915_dp_dp_to_i915(intel_dp);

	I915_DP_LOCKDEP_ASSERT_HELD(&dev_priv->display.pps.mutex);

	/* Reads the control word. */
	control = i915_dp_intel_de_read(dev_priv, i915_pp_ctrl_reg(intel_dp));

	/* A pre-DDI register without the unlock key is locked. */
	has_ddi = i915_vbt_has_ddi(dev_priv);
	locked = 0;
	if (!has_ddi && (control & PANEL_UNLOCK_MASK) != PANEL_UNLOCK_REGS)
		locked = 1;

	/* Adds the unlock key to a locked control word, with a warning. */
	warned = I915_DP_DRM_WARN_ON(&dev_priv->drm, locked);
	if (warned) {
		control &= ~PANEL_UNLOCK_MASK;
		control |= PANEL_UNLOCK_REGS;
	}

	/* Succeeded: reports the control word. */
	return control;
}

/* Turns forced VDD off now and returns the AUX reference it held; the PPS lock is held. */
static void
i915_pps_vdd_off_sync_unlocked(
	struct intel_dp *intel_dp)
{
	struct drm_i915_private *dev_priv;
	struct intel_digital_port *dig_port;
	u32 pp;
	i915_reg_t pp_stat_reg;
	i915_reg_t pp_ctrl_reg;
	intel_wakeref_t vdd_wakeref;
	bool vdd;

	/* Finds the device and the port. */
	dev_priv = i915_dp_dp_to_i915(intel_dp);
	dig_port = i915_dp_dp_to_dig_port(intel_dp);

	I915_DP_LOCKDEP_ASSERT_HELD(&dev_priv->display.pps.mutex);

	/* Reports a VDD-off while someone still wants VDD. */
	(void)I915_DP_DRM_WARN_ON(&dev_priv->drm, intel_dp->pps.want_panel_vdd);

	/* Nothing to do when VDD is not forced on. */
	vdd = i915_edp_have_panel_vdd(intel_dp);
	if (!vdd)
		return;

	I915_DP_DRM_DBG_KMS(&dev_priv->drm, "[ENCODER:%d:%s] %s turning VDD off\n",
			    dig_port->base.base.base.id, dig_port->base.base.name,
			    i915_pps_name(dev_priv, &intel_dp->pps));

	/* Clears the forced VDD. */
	pp = i915_ilk_get_pp_control(intel_dp);
	pp &= ~EDP_FORCE_VDD;

	pp_ctrl_reg = i915_pp_ctrl_reg(intel_dp);
	pp_stat_reg = i915_pp_stat_reg(intel_dp);

	i915_dp_intel_de_write(dev_priv, pp_ctrl_reg, pp);
	i915_dp_intel_de_posting_read(dev_priv, pp_ctrl_reg);

	/* Makes sure the sequencer is idle before allowing subsequent activity. */
	I915_DP_DRM_DBG_KMS(&dev_priv->drm, "[ENCODER:%d:%s] %s PP_STATUS: 0x%08x PP_CONTROL: 0x%08x\n",
			    dig_port->base.base.base.id, dig_port->base.base.name,
			    i915_pps_name(dev_priv, &intel_dp->pps),
			    i915_dp_intel_de_read(dev_priv, pp_stat_reg),
			    i915_dp_intel_de_read(dev_priv, pp_ctrl_reg));

	/* Without panel power, the power cycle counts from now. */
	if ((pp & PANEL_POWER_ON) == 0)
		intel_dp->pps.panel_power_off_time = ktime_get_boottime();

	/* Returns the AUX reference the forced VDD held. */
	vdd_wakeref = intel_dp->pps.vdd_wakeref;
	intel_dp->pps.vdd_wakeref = 0;
	i915_dp_intel_display_power_put(dev_priv, i915_aux_power_domain(dig_port), vdd_wakeref);
}

/* Runs the delayed VDD-off: turns VDD off unless someone wants it again. */
static void
i915_edp_panel_vdd_work(
	struct work_struct *work)
{
	struct intel_pps *pps;
	struct intel_dp *intel_dp;
	intel_wakeref_t wakeref;

	/* Finds the sequencer state and the port the work belongs to. */
	pps = container_of(to_delayed_work(work), struct intel_pps, panel_vdd_work);
	intel_dp = container_of(pps, struct intel_dp, pps);

	/* Turns VDD off with the sequencer held, unless it is wanted again. */
	wakeref = drv_i915_pps_lock(intel_dp);

	if (!intel_dp->pps.want_panel_vdd)
		i915_pps_vdd_off_sync_unlocked(intel_dp);

	(void)drv_i915_pps_unlock(intel_dp, wakeref);
}

/*
 * Schedules the delayed VDD-off.
 *
 * The delay is five power-cycle delays, long enough to keep the panel
 * powered across a sequence of operations.  Nothing is scheduled while the
 * connector is initializing, because the real delays are not known yet.
 */
static void
i915_edp_panel_vdd_schedule_off(
	struct intel_dp *intel_dp)
{
	struct drm_i915_private *i915;
	unsigned long delay;

	/* Finds the device the port belongs to. */
	i915 = i915_dp_dp_to_i915(intel_dp);

	/* Keeps VDD on until the initialization is done. */
	if (intel_dp->pps.initializing)
		return;

	/* Queues the VDD-off a long time from now, relative to the power-down delay. */
	delay = i915_dp_msecs_to_jiffies(intel_dp->pps.panel_power_cycle_delay * 5);
	(void)i915_dp_queue_delayed_work(i915->unordered_wq, &intel_dp->pps.panel_vdd_work, delay);
}

/*
 * Adopts a VDD the firmware left on.
 *
 * The forced VDD needs a power domain reference, so the reference is taken
 * here; the caller schedules the VDD-off so that it is not held forever.
 */
static void
i915_pps_vdd_init(
	struct intel_dp *intel_dp)
{
	struct drm_i915_private *dev_priv;
	struct intel_digital_port *dig_port;
	bool vdd;

	/* Finds the device and the port. */
	dev_priv = i915_dp_dp_to_i915(intel_dp);
	dig_port = i915_dp_dp_to_dig_port(intel_dp);

	I915_DP_LOCKDEP_ASSERT_HELD(&dev_priv->display.pps.mutex);

	/* Nothing to adopt when VDD is off. */
	vdd = i915_edp_have_panel_vdd(intel_dp);
	if (!vdd)
		return;

	I915_DP_DRM_DBG_KMS(&dev_priv->drm,
			    "[ENCODER:%d:%s] %s VDD left on by BIOS, adjusting state tracking\n",
			    dig_port->base.base.base.id, dig_port->base.base.name,
			    i915_pps_name(dev_priv, &intel_dp->pps));

	/* Takes the AUX reference the forced VDD holds. */
	(void)I915_DP_DRM_WARN_ON(&dev_priv->drm, intel_dp->pps.vdd_wakeref);
	intel_dp->pps.vdd_wakeref = i915_dp_intel_display_power_get(dev_priv, i915_aux_power_domain(dig_port));
}

/*
 * Starts the power timestamps.
 *
 * The panel power-off time starts at 0, assuming that only a previously
 * loaded driver could have toggled the panel power since boot, and that it
 * waited out the power-off delay itself.
 */
static void
i915_pps_init_timestamps(
	struct intel_dp *intel_dp)
{
	/* Counts the power cycle as long over and the other delays from now. */
	intel_dp->pps.panel_power_off_time = 0;
	intel_dp->pps.last_power_on = i915_dp_jiffies();
	intel_dp->pps.last_backlight_off = i915_dp_jiffies();
}

/* Reads the delays the sequencer holds. */
static void
i915_pps_readout_hw_state(
	struct intel_dp *intel_dp,
	struct edp_power_seq *seq)
{
	struct drm_i915_private *dev_priv;
	u32 pp_on;
	u32 pp_off;
	u32 pp_ctl;
	u32 pp_div;
	struct i915_pps_registers regs;
	int has_ddi;
	bool has_div;

	/* Finds the device and the sequencer's registers. */
	dev_priv = i915_dp_dp_to_i915(intel_dp);
	i915_pps_get_registers(intel_dp, &regs);

	/* Reads the control word, which carries the cycle delay on newer PCHs. */
	pp_ctl = i915_ilk_get_pp_control(intel_dp);

	/* Unlocks a pre-DDI sequencer. */
	has_ddi = i915_vbt_has_ddi(dev_priv);
	if (!has_ddi)
		i915_dp_intel_de_write(dev_priv, regs.pp_ctrl, pp_ctl);

	/* Reads the delay registers. */
	pp_on = i915_dp_intel_de_read(dev_priv, regs.pp_on);
	pp_off = i915_dp_intel_de_read(dev_priv, regs.pp_off);

	/* Pulls the timing values out of the registers. */
	seq->t1_t3 = REG_FIELD_GET(PANEL_POWER_UP_DELAY_MASK, pp_on);
	seq->t8 = REG_FIELD_GET(PANEL_LIGHT_ON_DELAY_MASK, pp_on);
	seq->t9 = REG_FIELD_GET(PANEL_LIGHT_OFF_DELAY_MASK, pp_off);
	seq->t10 = REG_FIELD_GET(PANEL_POWER_DOWN_DELAY_MASK, pp_off);

	/* The cycle delay, in 100 ms units, from PP_DIVISOR or from the control word. */
	has_div = i915_mmio_reg_valid(regs.pp_div);
	if (has_div) {
		pp_div = i915_dp_intel_de_read(dev_priv, regs.pp_div);

		seq->t11_t12 = REG_FIELD_GET(PANEL_POWER_CYCLE_DELAY_MASK, pp_div) * 1000;
	} else {
		seq->t11_t12 = REG_FIELD_GET(BXT_POWER_CYCLE_DELAY_MASK, pp_ctl) * 1000;
	}
}

/* Reports one set of delays in a debug message. */
static void
i915_pps_dump_state(
	struct intel_dp *intel_dp,
	const char *state_name,
	const struct edp_power_seq *seq)
{
	UNUSED_PARAMETER(intel_dp);

	/* The message macros never read the device, so none is looked up. */
	I915_DP_DRM_DBG_KMS(NULL, "%s t1_t3 %d t8 %d t9 %d t10 %d t11_t12 %d\n",
			    state_name,
			    seq->t1_t3, seq->t8, seq->t9, seq->t10, seq->t11_t12);
}

/* Reports delays in the hardware that differ from the ones selected. */
static void
i915_pps_verify_state(
	struct intel_dp *intel_dp)
{
	struct edp_power_seq hw;
	struct edp_power_seq *sw;

	/* Finds the delays selected. */
	sw = &intel_dp->pps.pps_delays;

	/* Reads the delays the hardware holds. */
	i915_pps_readout_hw_state(intel_dp, &hw);

	/* Reports any delay that differs, with both sets. */
	if (hw.t1_t3 != sw->t1_t3 ||
	    hw.t8 != sw->t8 ||
	    hw.t9 != sw->t9 ||
	    hw.t10 != sw->t10 ||
	    hw.t11_t12 != sw->t11_t12) {
		I915_DP_DRM_ERR(NULL, "PPS state mismatch\n");
		i915_pps_dump_state(intel_dp, "sw", sw);
		i915_pps_dump_state(intel_dp, "hw", &hw);
	}
}

/* Tells whether a set of delays has any delay set. */
static bool
i915_pps_delays_valid(
	struct edp_power_seq *delays)
{
	/* Any delay set makes the set valid. */
	if (delays->t1_t3 != 0)
		return true;
	if (delays->t8 != 0)
		return true;
	if (delays->t9 != 0)
		return true;
	if (delays->t10 != 0)
		return true;
	if (delays->t11_t12 != 0)
		return true;

	/* Reports a set with every delay unset. */
	return false;
}

/* Reports the delays the firmware programmed, read once and kept. */
static void
i915_pps_init_delays_bios(
	struct intel_dp *intel_dp,
	struct edp_power_seq *bios)
{
	struct drm_i915_private *dev_priv;
	bool known;

	/* Finds the device the port belongs to. */
	dev_priv = i915_dp_dp_to_i915(intel_dp);

	I915_DP_LOCKDEP_ASSERT_HELD(&dev_priv->display.pps.mutex);

	/* Reads the firmware's delays the first time only. */
	known = i915_pps_delays_valid(&intel_dp->pps.bios_pps_delays);
	if (!known)
		i915_pps_readout_hw_state(intel_dp, &intel_dp->pps.bios_pps_delays);

	/* Hands the firmware's delays to the caller. */
	*bios = intel_dp->pps.bios_pps_delays;

	i915_pps_dump_state(intel_dp, "bios", bios);
}

/* Reports the delays the VBT gives for the panel. */
static void
i915_pps_init_delays_vbt(
	struct intel_dp *intel_dp,
	struct edp_power_seq *vbt)
{
	struct drm_i915_private *dev_priv;
	struct intel_connector *connector;
	bool valid;
	bool quirk;

	/* Finds the device and the connector. */
	dev_priv = i915_dp_dp_to_i915(intel_dp);
	connector = intel_dp->attached_connector;

	/* Takes the VBT's power sequence. */
	*vbt = connector->panel.vbt.edp.pps;

	/* A VBT without delays gives nothing. */
	valid = i915_pps_delays_valid(vbt);
	if (!valid)
		return;

	/*
	 * On the Toshiba Satellite P50-C-18C the VBT's T12 of 500 ms is too
	 * short and the panel occasionally fails to power back on; 800 ms is
	 * enough.
	 */
	quirk = i915_dp_intel_has_quirk(dev_priv, QUIRK_INCREASE_T12_DELAY);
	if (quirk) {
		vbt->t11_t12 = max_t(u16, vbt->t11_t12, 1300 * 10);
		I915_DP_DRM_DBG_KMS(&dev_priv->drm,
				    "Increasing T12 panel delay as per the quirk to %d\n",
				    vbt->t11_t12);
	}

	/*
	 * T11_T12 is really in units of 100 ms and zero based in the hardware
	 * (100 ms is added), but the VBT table multiplies it by 1000 so that it
	 * is in units of 100 us too.
	 */
	vbt->t11_t12 += 100 * 10;

	i915_pps_dump_state(intel_dp, "vbt", vbt);
}

/* Reports the upper delay limits of the eDP 1.3 specification. */
static void
i915_pps_init_delays_spec(
	struct intel_dp *intel_dp,
	struct edp_power_seq *spec)
{
	struct drm_i915_private *dev_priv;

	/* Finds the device the port belongs to. */
	dev_priv = i915_dp_dp_to_i915(intel_dp);

	I915_DP_LOCKDEP_ASSERT_HELD(&dev_priv->display.pps.mutex);

	/* The eDP 1.3 limits, in the hardware's 100 us units; T8 and T9 have none and take T7's. */
	spec->t1_t3 = 210 * 10;
	spec->t8 = 50 * 10;
	spec->t9 = 50 * 10;
	spec->t10 = 500 * 10;

	/* T11_T12 is in 100 ms units, zero based in the hardware, multiplied to 100 us like the VBT's. */
	spec->t11_t12 = (510 + 100) * 10;

	i915_pps_dump_state(intel_dp, "spec", spec);
}

/*
 * Picks one delay: the larger of the register and VBT values, or the spec
 * limit when both are unset.
 */
static u16
i915_pps_pick_delay(
	u16 cur,
	u16 vbt,
	u16 spec)
{
	u16 larger;

	/* Takes the larger of the register and VBT values. */
	larger = max(cur, vbt);

	/* Both unset: falls back to the spec limit. */
	if (larger == 0)
		return spec;

	/* Succeeded: reports the larger value. */
	return larger;
}

/*
 * Selects the panel delays once: the larger of the register and VBT values,
 * or the spec limits, and the millisecond delays the waits use.
 */
static void
i915_pps_init_delays(
	struct intel_dp *intel_dp)
{
	struct drm_i915_private *dev_priv;
	struct edp_power_seq cur;
	struct edp_power_seq vbt;
	struct edp_power_seq spec;
	struct edp_power_seq *final;
	bool initialized;

	/* Finds the device and the delays to fill. */
	dev_priv = i915_dp_dp_to_i915(intel_dp);
	final = &intel_dp->pps.pps_delays;

	I915_DP_LOCKDEP_ASSERT_HELD(&dev_priv->display.pps.mutex);

	/* Delays already selected are kept. */
	initialized = i915_pps_delays_valid(final);
	if (initialized)
		return;

	/* Collects the three candidate sets. */
	i915_pps_init_delays_bios(intel_dp, &cur);
	i915_pps_init_delays_vbt(intel_dp, &vbt);
	i915_pps_init_delays_spec(intel_dp, &spec);

	/* The power-up delay (T1+T3). */
	final->t1_t3 = i915_pps_pick_delay(cur.t1_t3, vbt.t1_t3, spec.t1_t3);

	/* The backlight-on delay (T8). */
	final->t8 = i915_pps_pick_delay(cur.t8, vbt.t8, spec.t8);

	/* The backlight-off delay (T9). */
	final->t9 = i915_pps_pick_delay(cur.t9, vbt.t9, spec.t9);

	/* The power-down delay (T10). */
	final->t10 = i915_pps_pick_delay(cur.t10, vbt.t10, spec.t10);

	/* The power-cycle delay (T11+T12). */
	final->t11_t12 = i915_pps_pick_delay(cur.t11_t12, vbt.t11_t12, spec.t11_t12);

	/* Converts each delay from 100 us units to the milliseconds the waits use. */
	intel_dp->pps.panel_power_up_delay = DIV_ROUND_UP(final->t1_t3, 10);
	intel_dp->pps.backlight_on_delay = DIV_ROUND_UP(final->t8, 10);
	intel_dp->pps.backlight_off_delay = DIV_ROUND_UP(final->t9, 10);
	intel_dp->pps.panel_power_down_delay = DIV_ROUND_UP(final->t10, 10);
	intel_dp->pps.panel_power_cycle_delay = DIV_ROUND_UP(final->t11_t12, 10);

	I915_DP_DRM_DBG_KMS(&dev_priv->drm,
			    "panel power up delay %d, power down delay %d, power cycle delay %d\n",
			    intel_dp->pps.panel_power_up_delay,
			    intel_dp->pps.panel_power_down_delay,
			    intel_dp->pps.panel_power_cycle_delay);

	I915_DP_DRM_DBG_KMS(&dev_priv->drm, "backlight on delay %d, off delay %d\n",
			    intel_dp->pps.backlight_on_delay,
			    intel_dp->pps.backlight_off_delay);

	/*
	 * Overrides the hardware backlight delays to 1, because the waits on
	 * them are done by hand.  BSpec recommends it for T8; for T9 the
	 * backlight-off delay would otherwise be waited twice, once by hand and
	 * once for PP_STATUS when the panel is disabled.
	 */
	final->t8 = 1;
	final->t9 = 1;

	/* The hardware has only 100 ms granularity for T11_T12: rounds it up. */
	final->t11_t12 = roundup(final->t11_t12, 100 * 10);
}

/* Programs the selected delays into the sequencer. */
static void
i915_pps_init_registers(
	struct intel_dp *intel_dp,
	bool force_disable_vdd)
{
	struct drm_i915_private *dev_priv;
	u32 pp_on;
	u32 pp_off;
	u32 port_sel;
	u32 pp;
	int div;
	struct i915_pps_registers regs;
	enum port port;
	const struct edp_power_seq *seq;
	bool has_div;

	/* Finds the device, the raw-clock divider, the port and the delays. */
	dev_priv = i915_dp_dp_to_i915(intel_dp);
	port_sel = 0;
	div = i915_dp_display_runtime_info(dev_priv)->rawclk_freq / 1000;
	port = i915_dp_dp_to_dig_port(intel_dp)->base.port;
	seq = &intel_dp->pps.pps_delays;

	I915_DP_LOCKDEP_ASSERT_HELD(&dev_priv->display.pps.mutex);

	/* Looks up the sequencer's registers. */
	i915_pps_get_registers(intel_dp, &regs);

	/*
	 * Disables VDD first when asked: some VLV firmware leaves VDD on in
	 * sequencers no port uses, which would upset the power-domain tracking
	 * the first time such a sequencer is picked.
	 */
	if (force_disable_vdd) {
		pp = i915_ilk_get_pp_control(intel_dp);

		(void)I915_DP_DRM_WARN(&dev_priv->drm, pp & PANEL_POWER_ON,
				       "Panel power already on\n");

		if (pp & EDP_FORCE_VDD) {
			I915_DP_DRM_DBG_KMS(&dev_priv->drm,
					    "VDD already on, disabling first\n");
		}

		pp &= ~EDP_FORCE_VDD;

		i915_dp_intel_de_write(dev_priv, regs.pp_ctrl, pp);
	}

	/* Packs the delays into the two delay registers. */
	pp_on = REG_FIELD_PREP(PANEL_POWER_UP_DELAY_MASK, seq->t1_t3) |
		REG_FIELD_PREP(PANEL_LIGHT_ON_DELAY_MASK, seq->t8);
	pp_off = REG_FIELD_PREP(PANEL_LIGHT_OFF_DELAY_MASK, seq->t9) |
		 REG_FIELD_PREP(PANEL_POWER_DOWN_DELAY_MASK, seq->t10);

	/* Only VLV/CHV and IBX/CPT select the port in the sequencer; Haswell and later have no port bits. */
	if (IS_VALLEYVIEW(dev_priv) || IS_CHERRYVIEW(dev_priv)) {
		port_sel = PANEL_PORT_SELECT_VLV(port);
	} else if (HAS_PCH_IBX(dev_priv) || HAS_PCH_CPT(dev_priv)) {
		/* Selects the port the panel is on. */
		switch (port) {
		case PORT_A:
			port_sel = PANEL_PORT_SELECT_DPA;
			break;
		case PORT_C:
			port_sel = PANEL_PORT_SELECT_DPC;
			break;
		case PORT_D:
			port_sel = PANEL_PORT_SELECT_DPD;
			break;
		default:
			I915_DP_MISSING_CASE(port);
			break;
		}
	}

	pp_on |= port_sel;

	/* Writes the delay registers. */
	i915_dp_intel_de_write(dev_priv, regs.pp_on, pp_on);
	i915_dp_intel_de_write(dev_priv, regs.pp_off, pp_off);

	/* Writes the pp clock divisor by the Bspec formula and the cycle delay, or the cycle delay in PP_CONTROL. */
	has_div = i915_mmio_reg_valid(regs.pp_div);
	if (has_div) {
		i915_dp_intel_de_write(dev_priv,
				       regs.pp_div,
				       REG_FIELD_PREP(PP_REFERENCE_DIVIDER_MASK, (100 * div) / 2 - 1) |
				       REG_FIELD_PREP(PANEL_POWER_CYCLE_DELAY_MASK, DIV_ROUND_UP(seq->t11_t12, 1000)));
	} else {
		(void)i915_dp_intel_de_rmw(dev_priv,
					   regs.pp_ctrl,
					   BXT_POWER_CYCLE_DELAY_MASK,
					   REG_FIELD_PREP(BXT_POWER_CYCLE_DELAY_MASK, DIV_ROUND_UP(seq->t11_t12, 1000)));
	}

	I915_DP_DRM_DBG_KMS(&dev_priv->drm,
			    "panel power sequencer register settings: PP_ON %#x, PP_OFF %#x, PP_DIV %#x\n",
			    i915_dp_intel_de_read(dev_priv, regs.pp_on),
			    i915_dp_intel_de_read(dev_priv, regs.pp_off),
			    i915_mmio_reg_valid(regs.pp_div) ?
			    i915_dp_intel_de_read(dev_priv, regs.pp_div) :
			    (i915_dp_intel_de_read(dev_priv, regs.pp_ctrl) & BXT_POWER_CYCLE_DELAY_MASK));
}

/* Switches to the sequencer the VBT names once the panel's VBT data is known. */
static void
i915_pps_init_late(
	struct intel_dp *intel_dp)
{
	struct drm_i915_private *i915;
	struct intel_encoder *encoder;
	struct intel_connector *connector;
	int pps_num;
	int controller;
	int mismatch;

	/* Finds the device, the encoder and the connector. */
	i915 = i915_dp_dp_to_i915(intel_dp);
	encoder = &i915_dp_dp_to_dig_port(intel_dp)->base;
	connector = intel_dp->attached_connector;

	/* VLV/CHV have a sequencer set-up of their own. */
	if (IS_VALLEYVIEW(i915) || IS_CHERRYVIEW(i915))
		return;

	/* With one sequencer there is nothing to switch to. */
	pps_num = i915_num_pps(i915);
	if (pps_num < 2)
		return;

	/* Reports a VBT controller that differs from the sequencer chosen at first. */
	controller = connector->panel.vbt.backlight.controller;
	mismatch = 0;
	if (controller >= 0 && intel_dp->pps.pps_idx != controller)
		mismatch = 1;

	(void)I915_DP_DRM_WARN(&i915->drm, mismatch,
			       "[ENCODER:%d:%s] power sequencer mismatch: %d (initial) vs. %d (VBT)\n",
			       encoder->base.base.id, encoder->base.name,
			       intel_dp->pps.pps_idx, connector->panel.vbt.backlight.controller);

	/* Takes the VBT's controller when it names one. */
	if (controller >= 0)
		intel_dp->pps.pps_idx = controller;
}
