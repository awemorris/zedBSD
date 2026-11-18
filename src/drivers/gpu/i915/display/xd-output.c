/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * One output of a screen of the display 12 and 13 code (see xd-output.h).
 */

#include "xd-output.h"
#include "xd-clock.h"
#include "xd-dbuf.h"
#include "xd-ddi.h"
#include "xd-dmc.h"
#include "xd-dp.h"
#include "xd-io.h"
#include "xd-panel.h"
#include "xd-pipe.h"
#include "xd-platform.h"
#include "xd-power.h"
#include "xd-screen.h"

#include "../intel/display-dpcd-xd.h"
#include "../intel/display-regs-xd9.h"
#include "../intel/display-regs-xd.h"

#include <kern/kcrt.h>

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The delay before the DC states may come back after a light or a dark:
 * long enough that a commit following at 60 frames a second does not turn
 * them off and on again.
 *
 * Values referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_display.c (intel_atomic_commit_tail,
 * the delay of the DC off put).
 */
#define I915_XD_OUTPUT_DC_DELAY_MS	17

/*
 * The PWM frequency of a panel whose VBT names none.
 *
 * Values referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_backlight.c (get_vbt_pwm_freq).
 */
#define I915_XD_OUTPUT_PWM_HZ		200U

/*
 * The DPCD revision from which a sink has a power state, the tries a
 * sink is given to wake up and the wait between them.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dp.c
 * (intel_dp_set_power).
 */
#define I915_XD_OUTPUT_DPCD_POWER_REV	0x11U
#define I915_XD_OUTPUT_WAKE_TRIES	3U
#define I915_XD_OUTPUT_WAKE_US		1000U

/*
 * The source's identifier an eDP panel is given before it is woken (the
 * vendor's OUI the panel may tune itself to), and where it is written.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dp.c
 * (intel_edp_init_source_oui), include/drm/display/drm_dp.h
 * (DP_SOURCE_OUI).
 */
#define I915_XD_OUTPUT_SOURCE_OUI	0x300U
#define I915_XD_OUTPUT_OUI_SIZE		3U

/*
 * The DPCD revision of a sink that may be a branch device whose ports
 * signal hotplug only while it is awake, the branch flag of its
 * capabilities and the hotplug flag of its first port.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dp.c
 * (downstream_hpd_needs_d0), include/drm/display/drm_dp.h
 * (DP_DOWNSTREAMPORT_PRESENT, DP_DWN_STRM_PORT_PRESENT, DP_DS_PORT_HPD).
 */
#define I915_XD_OUTPUT_DPCD_REV_1_1	0x11U
#define I915_XD_OUTPUT_DPCD_PORTS	0x005U
#define I915_XD_OUTPUT_DPCD_BRANCH	0x01U

/*
 * The training patterns a sink is told of, as the DPCD writes them (with
 * the scrambling bit masked off).
 *
 * Values referenced: Linux v6.8.12 include/drm/display/drm_dp.h
 * (DP_TRAINING_PATTERN_MASK_1_4).
 */
#define I915_XD_OUTPUT_PATTERN_MASK	0x0fU

/* The power uses a pipe's crtc takes for the whole time it runs. */
#define I915_XD_OUTPUT_CRTC_USES	((1U << I915_XD_OUTPUT_POWER_CORE) | (1U << I915_XD_OUTPUT_POWER_PIPE) | \
					 (1U << I915_XD_OUTPUT_POWER_TRANS) | (1U << I915_XD_OUTPUT_POWER_LANES))

/*
 * The run of a port step: the output and its environment, which the
 * training's hooks reach through their context.
 */
struct i915_xd_output_run {
	struct i915_xd_output *out;
	const struct i915_xd_output_env *env;
};

static unsigned i915_xd_output_index(const struct i915_xd_output *out, unsigned use);
static int i915_xd_output_take(struct i915_xd_output *out, const struct i915_xd_output_env *env, unsigned use);
static void i915_xd_output_give(struct i915_xd_output *out, const struct i915_xd_output_env *env, unsigned use);
static void i915_xd_output_error(const struct i915_xd_output_env *env, const char *text);
static void i915_xd_output_debug(const struct i915_xd_output_env *env, const char *text);
static void i915_xd_output_lock(const struct i915_xd_output_env *env, unsigned lock, int taken);
static int i915_xd_output_panel(const struct i915_xd_output_env *env, unsigned step);
static void i915_xd_output_observe(const struct i915_xd_output_env *env, unsigned point);
static void i915_xd_output_dbuf_before(struct i915_xd_output *out, const struct i915_xd_output_env *env);
static void i915_xd_output_dbuf_after(struct i915_xd_output *out, const struct i915_xd_output_env *env);
static void i915_xd_output_give_crtc(struct i915_xd_output *out, const struct i915_xd_output_env *env);
static int i915_xd_output_port_hdmi(struct i915_xd_output *out, const struct i915_xd_output_env *env);
static void i915_xd_output_clock_on(struct i915_xd_output *out, const struct i915_xd_output_env *env);
static void i915_xd_output_clock_off(struct i915_xd_output *out, const struct i915_xd_output_env *env);
static void i915_xd_output_trans_clock(struct i915_xd_output *out, const struct i915_xd_output_env *env);
static void i915_xd_output_func(const struct i915_xd_output *out, unsigned mode, struct i915_xd_trans_func *func);
static void i915_xd_output_hdmi_levels(struct i915_xd_output *out, const struct i915_xd_output_env *env);
static void i915_xd_output_ddi_link(const struct i915_xd_output *out, const struct i915_xd_output_env *env, struct i915_xd_ddi_link *link);
static unsigned i915_xd_output_transport(const struct i915_xd_output *out, const struct i915_xd_output_env *env);
static int i915_xd_output_mg(const struct i915_xd_output *out, const struct i915_xd_output_env *env);
static const struct i915_xd_combo_swing_table *i915_xd_output_swing_table(const struct i915_xd_output *out, const struct i915_xd_output_env *env);
static void i915_xd_output_dp_link(struct i915_xd_output_run *run, struct i915_xd_dp_link *link);
static void i915_xd_output_sink_power(struct i915_xd_output *out, const struct i915_xd_output_env *env, uint8_t state);
static void i915_xd_output_islands(struct i915_xd_output *out, const struct i915_xd_output_env *env, int on);
static void i915_xd_output_levels(void *context, const uint8_t set[4]);
static void i915_xd_output_pattern(void *context, uint8_t pattern);
static void i915_xd_output_prepare(void *context);
static void i915_xd_output_phy_wa(void *context);
static int i915_xd_output_tc_own_pll(const struct i915_xd_output *out);
static void i915_xd_output_tc_dp_mode(struct i915_xd_output *out, const struct i915_xd_output_env *env);
static void i915_xd_output_tc_levels(struct i915_xd_output_run *run, const uint8_t set[4]);
static void i915_xd_output_idle(void *context);
static void i915_xd_output_sleep(void *context, unsigned min_us, unsigned max_us);
static void i915_xd_output_note(void *context, unsigned kind, const char *text);
static void i915_xd_output_ddi_late(const struct i915_xd_output_env *env, unsigned late);
static void i915_xd_output_pll_late(const struct i915_xd_output_env *env, unsigned late);
static uint32_t i915_xd_output_invert(const struct i915_xd_output *out, uint32_t level);
static int i915_xd_output_scale(const struct i915_xd_output_env *env, uint32_t value, uint32_t from_min, uint32_t from_max, uint32_t to_min, uint32_t to_max, uint32_t *scaled);

/*
 * Keeps the display out of DC states for the light or the dark that
 * starts: the DMC firmware must not power anything down while registers
 * are written and PLLs are waited for.  Returns 0, or EIO when the
 * reference could not be taken.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_display.c (intel_atomic_commit_tail).
 */
int
drv_i915_xd_output_dc_hold(
	struct i915_xd_output *out,
	const struct i915_xd_output_env *env)
{
	int error;

	/* The exit from DC states. */
	error = i915_xd_output_take(out, env, I915_XD_OUTPUT_POWER_DC_OFF);
	if (error != 0)
		return error;

	/* Succeeded: DC states stay off until the end. */
	return 0;
}

/*
 * Takes the power a lit pipe needs for the whole time it runs: the
 * display core (for its PLL), the pipe, the transcoder and the port's
 * lanes; a use held already is not taken twice.  Nothing is to be given
 * back at the end of a light.  Returns 0, or EIO.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_display.c
 * (intel_modeset_get_crtc_power_domains, get_crtc_power_domains).
 */
int
drv_i915_xd_output_power(
	struct i915_xd_output *out,
	const struct i915_xd_output_env *env)
{
	int error;

	/* The light gives nothing back. */
	out->put_later = 0U;

	/* The display core, which the port's PLL needs. */
	error = i915_xd_output_take(out, env, I915_XD_OUTPUT_POWER_CORE);
	if (error != 0)
		return error;

	/* The pipe. */
	error = i915_xd_output_take(out, env, I915_XD_OUTPUT_POWER_PIPE);
	if (error != 0)
		return error;

	/* The transcoder. */
	error = i915_xd_output_take(out, env, I915_XD_OUTPUT_POWER_TRANS);
	if (error != 0)
		return error;

	/* The port's lanes. */
	error = i915_xd_output_take(out, env, I915_XD_OUTPUT_POWER_LANES);
	if (error != 0)
		return error;

	/* Succeeded: the pipe's power is held. */
	return 0;
}

/*
 * Marks every power use of the running pipe to be given back at the end
 * of the dark; nothing is taken.
 */
int
drv_i915_xd_output_dark_power(
	struct i915_xd_output *out,
	const struct i915_xd_output_env *env)
{
	(void)env;

	/* The pipe's uses that are held. */
	out->put_later = out->held & I915_XD_OUTPUT_CRTC_USES;

	/* Succeeded: nothing to take. */
	return 0;
}

/*
 * Brings the display buffer to what the light needs before the pipe
 * runs: the bus joined or split and the slices of both the old and the
 * new layout on (the planes move to the new one later), then the data
 * box credits of the pipe.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/skl_watermark.c
 * (intel_dbuf_pre_plane_update, intel_mbus_dbox_update).
 */
int
drv_i915_xd_output_dbuf(
	struct i915_xd_output *out,
	const struct i915_xd_output_env *env)
{
	/* The slices and the bus, then the credits. */
	i915_xd_output_dbuf_before(out, env);

	/* Succeeded: the buffer is ready for the pipe. */
	return 0;
}

/*
 * Reads how the firmware left the panel's backlight PWM and derives the
 * backlight's range and level: the VBT's controller (or the first, when
 * the south display cannot use it), the period the firmware programmed
 * (or the VBT's frequency in raw clock ticks), the lowest duty from the
 * VBT's minimum, the level from the duty and whether it runs.  A panel
 * without a VBT backlight has none.  Returns 0, or ENODEV when no period
 * is known.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_backlight.c (intel_backlight_setup,
 * intel_pwm_setup_backlight, cnp_setup_backlight, get_vbt_pwm_freq,
 * intel_pwm_get_backlight).
 */
int
drv_i915_xd_output_backlight_setup(
	struct i915_xd_output *out,
	const struct i915_xd_output_env *env)
{
	struct i915_xd_output_backlight *backlight;
	unsigned controller;
	unsigned hz;
	uint32_t duty;
	int usable;

	/* The panel's backlight. */
	backlight = &out->backlight;

	/* A panel the VBT gives no backlight has none to set up. */
	if (!backlight->vbt_present)
		return 0;

	/* The setup reads the PWM under the backlight's lock. */
	i915_xd_output_lock(env, I915_XD_OUTPUT_LOCK_BACKLIGHT, 1);

	/* The VBT's controller, or the first when the south display cannot use it. */
	controller = backlight->vbt_controller;
	usable = drv_i915_xd_backlight_controller_usable(env->io, controller, backlight->controllers, backlight->muxed);
	if (!usable) {
		i915_xd_output_debug(env, "xd-output: the VBT's backlight controller is not usable, the first is\n");
		controller = 0U;
	}

	/* The polarity, the enable and the period the firmware left. */
	drv_i915_xd_backlight_read(&backlight->pwm, env->io, controller);

	/* Without a programmed period, the VBT's frequency (or the default) gives it. */
	if (backlight->pwm.period == 0U) {
		hz = backlight->vbt_hz;
		if (hz == 0U)
			hz = I915_XD_OUTPUT_PWM_HZ;
		backlight->pwm.period = drv_i915_xd_backlight_period(backlight->rawclk_khz, hz);
	}

	/* Neither gives one: the panel's backlight cannot be driven. */
	if (backlight->pwm.period == 0U) {
		i915_xd_output_lock(env, I915_XD_OUTPUT_LOCK_BACKLIGHT, 0);
		return ENODEV;
	}

	/* The range of levels is the PWM's: the lowest duty up to the period. */
	backlight->max = backlight->pwm.period;
	backlight->min = drv_i915_xd_backlight_min_duty(backlight->max, backlight->vbt_min);

	/* The level the duty shows now, and whether the PWM runs. */
	duty = drv_i915_xd_backlight_duty(&backlight->pwm, env->io);
	backlight->level = i915_xd_output_invert(out, duty);
	backlight->enabled = backlight->pwm.running;

	i915_xd_output_lock(env, I915_XD_OUTPUT_LOCK_BACKLIGHT, 0);

	/* Succeeded: the panel has a backlight from here. */
	backlight->present = 1;
	return 0;
}

/*
 * Switches the pipe's DMC on before the pipe is set up, or off after it
 * stopped (the pipe no longer runs from here), when the firmware has a
 * program for it.  Returns 0, or EBUSY when a light finds the pipe
 * running.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_display.c (hsw_crtc_enable,
 * hsw_crtc_disable), intel_dmc.c (intel_dmc_enable_pipe,
 * intel_dmc_disable_pipe).
 */
int
drv_i915_xd_output_dmc(
	struct i915_xd_output *out,
	const struct i915_xd_output_env *env,
	int on)
{
	unsigned program;

	/* A pipe that runs already is not lit again. */
	if (on && out->lit) {
		i915_xd_output_error(env, "xd-output: the pipe runs already\n");
		return EBUSY;
	}

	/* The pipe's program, when the firmware has one. */
	program = I915_XD_DMC_PIPE(out->pipe);
	if (((out->dmc_programs >> program) & 1U) != 0U)
		drv_i915_xd_dmc_pipe_switch(env->io, out->pipe, on);

	/* After the dark's switch the pipe no longer runs. */
	if (!on)
		out->lit = 0;

	/* Succeeded: the pipe's DMC is switched. */
	return 0;
}

/*
 * Takes the power the port's link needs before its PLL is enabled: a
 * Type-C port's link to its PHY first (and with it the mode the port is
 * held in), then the AUX channel's power of a DisplayPort link, which
 * must stay powered while the link runs, then the Type-C PHY's lanes for
 * the display.  An HDMI port on a combo PHY needs nothing here.  Returns
 * 0, or EIO.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi.c
 * (intel_ddi_pre_pll_enable, main_link_aux_power_domain_get,
 * intel_ddi_main_link_aux_domain), intel_tc.c (intel_tc_port_get_link,
 * intel_tc_port_set_fia_lane_count).
 */
int
drv_i915_xd_output_link_power(
	struct i915_xd_output *out,
	const struct i915_xd_output_env *env)
{
	int error;

	/* A Type-C port's link, and the mode the port is held in. */
	if (out->tc) {
		env->tc->get_link(env->context, out->lanes);
		out->tc_linked = 1;
		out->tc_mode = env->tc->mode(env->context);
	}

	/* An HDMI port on a combo PHY has no AUX channel to keep powered. */
	if (out->kind == I915_XD_SCREEN_HDMI && !out->tc)
		return 0;

	/* The AUX channel's power. */
	error = i915_xd_output_take(out, env, I915_XD_OUTPUT_POWER_AUX);
	if (error != 0)
		return error;

	/* The Type-C PHY's lanes for the display, but in Thunderbolt mode. */
	if (out->tc && out->tc_mode != I915_XD_OUTPUT_TC_TBT)
		env->tc->lanes(env->context, out->lanes);

	/* Succeeded: the link's power is held. */
	return 0;
}

/*
 * Turns the port's PLL on for the pipe, under the PLLs' lock: the first
 * pipe on it turns it on with its words, a later one only joins it.  The
 * PLL is a combo port's DPLL, or a Type-C port's own PLL in DisplayPort
 * alternate or legacy mode (its words go to its PHY); the Thunderbolt
 * PLL the Type-C ports share otherwise is the caller's.  Returns 0,
 * EBUSY when the pipe is on the PLL already, ENODEV for a PLL the
 * platform does not have, or EIO when the PLL did not power up or lock.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_dpll_mgr.c (intel_enable_shared_dpll,
 * icl_pll_enable, combo_pll_enable, mg_pll_enable).
 */
int
drv_i915_xd_output_pll(
	struct i915_xd_output *out,
	const struct i915_xd_output_env *env)
{
	const struct i915_xd_pll_desc *pll;
	uint8_t pipe_bit;
	uint8_t before;
	unsigned pll_id;
	unsigned late;
	int own;
	int error;
	int mg;

	/* The pipe's bit among the PLL's users. */
	pipe_bit = (uint8_t)(1U << out->pipe);

	/* The Thunderbolt PLL the Type-C ports share is the caller's. */
	own = i915_xd_output_tc_own_pll(out);
	if (out->tc && !own) {
		error = env->tc->tbt_pll(env->context, 1);
		if (error != 0)
			return error;
		out->pll_pipes |= pipe_bit;
		out->pll_on = 1;
		return 0;
	}

	/* Joins the PLL or turns it on, under the PLLs' lock. */
	i915_xd_output_lock(env, I915_XD_OUTPUT_LOCK_PLL, 1);

	/* The pipe is on the PLL already. */
	if ((out->pll_pipes & pipe_bit) != 0U) {
		i915_xd_output_lock(env, I915_XD_OUTPUT_LOCK_PLL, 0);
		i915_xd_output_error(env, "xd-output: the pipe is on the PLL already\n");
		return EBUSY;
	}

	/* Another pipe runs the PLL: it is on, and the pipe joins it. */
	before = out->pll_pipes;
	out->pll_pipes |= pipe_bit;
	if (before != 0U) {
		if (!out->pll_on)
			i915_xd_output_error(env, "xd-output: the shared PLL is off\n");
		i915_xd_output_lock(env, I915_XD_OUTPUT_LOCK_PLL, 0);
		return 0;
	}

	/* The combo PLL, or the Type-C port's own, as the platform knows it. */
	pll_id = out->pll;
	if (out->tc)
		pll_id = out->tc_pll;
	pll = drv_i915_xd_dpll_desc(env->platform, pll_id);
	if (pll == NULL) {
		i915_xd_output_lock(env, I915_XD_OUTPUT_LOCK_PLL, 0);
		i915_xd_output_error(env, "xd-output: the port PLL is not the platform's\n");
		return ENODEV;
	}

	/* Turns it on with the plan's words: a combo PLL's own, or a Type-C PLL's in its PHY (an MG PHY's on display 11). */
	mg = 0;
	if (out->tc)
		mg = i915_xd_output_mg(out, env);
	if (mg) {
		late = drv_i915_xd_mg_pll_enable(env->io, pll, &out->mg_pll_words);
	} else if (out->tc) {
		late = drv_i915_xd_tc_pll_enable(env->io, env->dkl, pll, &out->tc_pll_words);
	} else {
		late = drv_i915_xd_dpll_enable(env->io, env->platform, pll, &out->pll_words);
	}

	/* The PLL is the output's from here on; the PLL lock goes. */
	out->pll_on = 1;
	i915_xd_output_lock(env, I915_XD_OUTPUT_LOCK_PLL, 0);

	/* Reports the waits that ran out. */
	i915_xd_output_pll_late(env, late);
	if (late != 0U)
		return EIO;

	/* Succeeded: the PLL runs for the pipe. */
	return 0;
}

/*
 * Brings the port up.  A DisplayPort port (the panel's or a Type-C
 * one): the pipe's underruns armed, the link's levels and the DDI
 * buffer's word, the panel's power, the DDI clock from the PLL, the
 * port's IO power (a Type-C port in Thunderbolt mode has none), the
 * Type-C PHY's DisplayPort mode, the transcoder's clock and function,
 * the signal levels and lanes, the splitter off, the sink woken (and a
 * branch's converter set up), the link trained and ended, then the
 * stream's attributes.  A link that does not train is reported to the
 * caller and the step goes on (the pipe still lights, black).  An HDMI
 * port: the DDI clock, the IO power, the transcoder's clock and the
 * data island packets off; its buffer starts with the output.  Returns
 * 0, or EIO when the port's IO power could not be taken.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi.c
 * (intel_ddi_pre_enable, intel_ddi_pre_enable_dp, tgl_ddi_pre_enable_dp,
 * intel_ddi_pre_enable_hdmi, intel_ddi_init_dp_buf_reg,
 * intel_ddi_enable_clock, intel_ddi_enable_transcoder_clock,
 * intel_ddi_config_transcoder_func, intel_ddi_power_up_lanes,
 * intel_ddi_mso_configure, intel_ddi_set_dp_msa), intel_dp.c
 * (intel_dp_set_link_params, intel_dp_set_power,
 * intel_dp_configure_protocol_converter), intel_dp_link_training.c
 * (intel_dp_start_link_train, intel_dp_stop_link_train).
 */
int
drv_i915_xd_output_port(
	struct i915_xd_output *out,
	const struct i915_xd_output_env *env)
{
	struct i915_xd_output_run run;
	struct i915_xd_ddi_link ddi;
	struct i915_xd_dp_link link;
	struct i915_xd_trans_func func;
	struct i915_xd_msa msa;
	int error;

	/* Where the pipe's underruns are armed. */
	i915_xd_output_observe(env, I915_XD_OUTPUT_SEE_UNDERRUN);

	/* An HDMI port. */
	if (out->kind == I915_XD_SCREEN_HDMI) {
		error = i915_xd_output_port_hdmi(out, env);
		if (error != 0)
			return error;
		return 0;
	}

	/* The link starts untrained from the lowest levels. */
	kern_memset(out->set, 0, sizeof(out->set));
	out->trained = 0;

	/* The DDI buffer's word; its enable is set when the link starts.  A Type-C PHY of display 13 the display owns says so in it. */
	i915_xd_output_ddi_link(out, env, &ddi);
	out->buf_word = drv_i915_xd_ddi_dp_word(env->platform, &ddi);
	if (out->tc &&
	    env->platform->display_ver >= 13U &&
	    out->tc_mode != I915_XD_OUTPUT_TC_TBT)
		out->buf_word |= I915_XD_DDI_BUF_TC_OWNED;

	/* The panel's power. */
	if (out->kind == I915_XD_SCREEN_PANEL)
		(void)i915_xd_output_panel(env, I915_XD_OUTPUT_PANEL_ON);

	/* The DDI clock from the port's PLL. */
	i915_xd_output_clock_on(out, env);

	/* The port's IO power, but on a Type-C port in Thunderbolt mode. */
	if (!out->tc || out->tc_mode != I915_XD_OUTPUT_TC_TBT) {
		error = i915_xd_output_take(out, env, I915_XD_OUTPUT_POWER_DDI_IO);
		if (error != 0)
			return error;
	}

	/* The Type-C PHY's DisplayPort mode. */
	if (out->tc)
		i915_xd_output_tc_dp_mode(out, env);

	/* From display 12 the transcoder runs on the port's clock and has its function, not yet enabled, before the training. */
	if (env->platform->display_ver >= 12U) {
		i915_xd_output_trans_clock(out, env);
		i915_xd_output_func(out, I915_XD_FUNC_DP_SST, &func);
		drv_i915_xd_pipe_func_config(env->io, out->trans, &func);
	}

	/* The signal levels of the lowest levels, then the lanes a combo port's link uses. */
	run.out = out;
	run.env = env;
	i915_xd_output_levels(&run, out->set);
	if (!out->tc)
		drv_i915_xd_combo_lanes(env->io, out->phy, out->lanes, (out->kept & I915_XD_DDI_BUF_REVERSAL) != 0U ? 1 : 0);

	/* No splitter: the stream is one (display 11 has none). */
	if (env->platform->display_ver >= 12U)
		(void)drv_i915_xd_io_rmw(env->io, I915_XD_PIPE_DSS_CTL1(out->pipe), I915_XD_PIPE_DSS_SPLITTER, 0U);

	/* The sink wakes up, and a branch's converter is set up. */
	i915_xd_output_sink_power(out, env, I915_XD_DPCD_POWER_D0);
	if (env->converter != NULL)
		env->converter(env->context);

	/* Trains the link; one that did not train is the caller's to lower. */
	i915_xd_output_dp_link(&run, &link);
	error = drv_i915_xd_dp_train(&link);
	kern_memcpy(out->set, link.set, sizeof(out->set));
	if (error != 0) {
		if (out->hobl_active)
			out->hobl_failed = 1;
		if (env->link_failed != NULL)
			env->link_failed(env->context);
	}

	/* The link leaves the training pattern and carries the stream. */
	out->trained = 1;
	i915_xd_output_dp_link(&run, &link);
	drv_i915_xd_dp_train_end(&link);

	/* Display 11's transcoder runs on the port's clock once the link is up. */
	if (env->platform->display_ver < 12U)
		i915_xd_output_trans_clock(out, env);

	/* The stream's attributes the sink reads: depth and colour range. */
	kern_memset(&msa, 0, sizeof(msa));
	msa.bpp = out->bpp;
	msa.cea_rgb = out->limited_range;
	drv_i915_xd_pipe_msa(env->io, out->trans, &msa);

	/* Succeeded: the port runs its link. */
	return 0;
}

/*
 * Sets the pipe up: its source size and word, the transcoder (a
 * DisplayPort link's M/N, the timing, frame start, the transcoder's
 * configuration), then (the pipe
 * runs from here) the bottom colour and colour modes, the line time and
 * the chicken bits.  The output scans out the first plane alone, which
 * has the HDR path.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_display.c (hsw_crtc_enable,
 * intel_set_pipe_src_size, bdw_set_pipe_misc, hsw_configure_cpu_transcoder,
 * hsw_set_linetime_wm, icl_set_pipe_chicken), intel_color.c
 * (icl_color_commit_arm).
 */
int
drv_i915_xd_output_pipe(
	struct i915_xd_output *out,
	const struct i915_xd_output_env *env)
{
	struct i915_xd_trans_setup setup;
	struct i915_xd_pipe_misc misc;

	/* The source size. */
	drv_i915_xd_pipe_source(env->io, out->pipe, out->src_width, out->src_height);

	/* The pipe's word: depth, dither, RGB, HDR precision. */
	kern_memset(&misc, 0, sizeof(misc));
	misc.bpp = out->bpp;
	misc.dither = out->dither;
	misc.output = I915_XD_OUTPUT_RGB;
	misc.hdr = 1;
	drv_i915_xd_pipe_misc(env->io, env->platform, out->pipe, &misc);

	/* The transcoder: a DisplayPort link's M/N, the timing, the frame start. */
	kern_memset(&setup, 0, sizeof(setup));
	setup.trans = out->trans;
	setup.timing = out->timing;
	if (out->kind != I915_XD_SCREEN_HDMI) {
		setup.dp = 1;
		setup.m_n = out->m_n;
	}

	/* The pixel multiplier and the frame start delay of the timing. */
	setup.pixel_multiplier = out->pixel_multiplier;
	setup.frame_start_delay = out->frame_start_delay;
	drv_i915_xd_pipe_configure(env->io, env->platform, &setup);

	/* The pipe counts as running from here. */
	out->lit = 1;

	/* The colours, the line time and the chicken bits. */
	drv_i915_xd_pipe_colors(env->io, out->pipe, out->gamma_mode, out->csc_mode);
	drv_i915_xd_io_write(env->io, I915_XD_WM_LINETIME(out->pipe), out->linetime);
	drv_i915_xd_pipe_chicken(env->io, env->platform, out->pipe);

	/* Succeeded: the pipe is set up. */
	return 0;
}

/*
 * Starts the output: the transcoder's function enabled and the
 * transcoder started; then the panel's backlight on (the PWM, then the
 * sequencer's enable) and the packets it does not use off, or an HDMI
 * port's signal levels, lanes and DDI buffer (its sink in DVI mode, which
 * needs no scrambling).  Returns 0, or EIO when an HDMI buffer did not
 * leave idle.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi.c
 * (intel_enable_ddi, intel_enable_ddi_dp, intel_enable_ddi_hdmi),
 * intel_display.c (intel_enable_transcoder), intel_dp.c
 * (intel_edp_backlight_on, intel_dp_set_infoframes).
 */
int
drv_i915_xd_output_start(
	struct i915_xd_output *out,
	const struct i915_xd_output_env *env)
{
	struct i915_xd_trans_func func;
	struct i915_xd_ddi_link ddi;
	unsigned mode;
	unsigned late;
	int ran;

	/* The transcoder's function with its enable: DVI for an HDMI port, single stream DisplayPort otherwise. */
	mode = I915_XD_FUNC_DP_SST;
	if (out->kind == I915_XD_SCREEN_HDMI)
		mode = I915_XD_FUNC_DVI;
	i915_xd_output_func(out, mode, &func);
	if (env->platform->display_ver < 12U) {
		drv_i915_xd_pipe_func_enable_11(env->io, out->trans, out->pipe, &func);
	} else {
		drv_i915_xd_pipe_func_enable(env->io, out->trans, &func);
	}

	/* The transcoder starts; one found running is left as it is. */
	ran = drv_i915_xd_pipe_start(env->io, env->platform, out->trans);
	if (ran)
		i915_xd_output_error(env, "xd-output: the transcoder ran already\n");

	/* An HDMI port: its levels, its lanes, then its buffer. */
	if (out->kind == I915_XD_SCREEN_HDMI) {
		i915_xd_output_hdmi_levels(out, env);
		drv_i915_xd_combo_lanes(env->io, out->phy, out->lanes, (out->kept & I915_XD_DDI_BUF_REVERSAL) != 0U ? 1 : 0);
		i915_xd_output_ddi_link(out, env, &ddi);
		late = drv_i915_xd_ddi_hdmi_start(env->io, env->platform, &ddi);
		i915_xd_output_ddi_late(env, late);
		if (late != 0U)
			return EIO;
		return 0;
	}

	/* The panel's backlight. */
	if (out->kind == I915_XD_SCREEN_PANEL)
		drv_i915_xd_output_backlight_on(out, env);

	/* The packets the output does not use. */
	i915_xd_output_islands(out, env, 1);

	/* Succeeded: the output shows the pipe. */
	return 0;
}

/*
 * Ends a light after the plane: the display buffer's slices that only
 * the old layout used go off, and the uses no longer needed are given
 * back.  The caller lets DC states back (drv_i915_xd_output_dc_allow).
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_display.c (intel_atomic_commit_tail),
 * skl_watermark.c (intel_dbuf_post_plane_update).
 */
int
drv_i915_xd_output_settle(
	struct i915_xd_output *out,
	const struct i915_xd_output_env *env)
{
	/* The new layout's slices alone. */
	i915_xd_output_dbuf_after(out, env);

	/* The uses the light no longer needs. */
	i915_xd_output_give_crtc(out, env);

	/* Succeeded: the light is settled. */
	return 0;
}

/*
 * Stops the output's picture before the transcoder: the link no longer
 * counts as trained, and the panel's backlight goes off (the sequencer's
 * enable, then the PWM).  An HDMI sink in DVI mode has nothing to reset.
 * Returns 0.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi.c
 * (intel_disable_ddi, intel_disable_ddi_dp, intel_disable_ddi_hdmi),
 * intel_dp.c (intel_edp_backlight_off).
 */
int
drv_i915_xd_output_stop(
	struct i915_xd_output *out,
	const struct i915_xd_output_env *env)
{
	/* The link is no longer up. */
	out->trained = 0;

	/* The panel's backlight. */
	if (out->kind == I915_XD_SCREEN_PANEL)
		drv_i915_xd_output_backlight_off(out, env);

	/* Succeeded: the picture is stopped. */
	return 0;
}

/*
 * Stops the transcoder and the port: the pipe's pending vertical blank
 * event settled, the transcoder stopped and its function cleared, the
 * scalers off, the packets off, a DisplayPort sink put to sleep, the DDI
 * buffer stopped, no clock to the transcoder, the panel's power off (VDD
 * held for the sequence), the port's IO power given back and its clock
 * gated.  Returns 0, or EIO when the transcoder did not stop or the
 * buffer did not go idle (the stop is then not confirmed).
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi.c
 * (intel_ddi_post_disable, intel_ddi_post_disable_dp,
 * intel_ddi_post_disable_hdmi, intel_ddi_disable_transcoder_func,
 * intel_disable_ddi_buf, intel_ddi_disable_transcoder_clock,
 * intel_ddi_disable_clock), intel_display.c (intel_disable_transcoder),
 * intel_dp.c (intel_dp_set_infoframes, intel_dp_set_power), intel_pps.c
 * (intel_pps_vdd_on, intel_pps_off).
 */
int
drv_i915_xd_output_port_off(
	struct i915_xd_output *out,
	const struct i915_xd_output_env *env)
{
	struct i915_xd_ddi_link ddi;
	unsigned late;
	int was_running;
	int error;
	int dp;

	/* No error yet; the output is DisplayPort unless it is HDMI. */
	error = 0;
	dp = out->kind != I915_XD_SCREEN_HDMI;

	/* The pipe's pending vertical blank event will not come. */
	if (env->vblank_off != NULL)
		env->vblank_off(env->context);

	/* The transcoder, then its function. */
	late = drv_i915_xd_pipe_stop(env->io, env->platform, out->trans, &was_running);
	if ((late & I915_XD_PIPE_LATE_OFF) != 0U) {
		i915_xd_output_error(env, "xd-output: the transcoder did not stop\n");
		error = EIO;
	}

	/* Turns the transcoder's DDI function off. */
	(void)drv_i915_xd_pipe_func_disable(env->io, out->trans, 0);

	/* The scalers and the packets. */
	drv_i915_xd_pipe_scalers_off(env->io, out->pipe);
	i915_xd_output_islands(out, env, 0);

	/* A DisplayPort sink goes to sleep before its link goes away. */
	if (dp)
		i915_xd_output_sink_power(out, env, I915_XD_DPCD_POWER_D3);

	/* Display 11 takes the clock off the transcoder before the buffer stops. */
	if (env->platform->display_ver < 12U)
		drv_i915_xd_pipe_clock_off(env->io, out->trans);

	/* The DDI buffer (and a DisplayPort link's transport). */
	i915_xd_output_ddi_link(out, env, &ddi);
	late = drv_i915_xd_ddi_stop(env->io, &ddi, dp);
	i915_xd_output_ddi_late(env, late);
	if ((late & I915_XD_DDI_LATE_IDLE) != 0U)
		error = EIO;
	out->buf_word &= ~I915_XD_DDI_BUF_ENABLE;

	/* From display 12 the transcoder's clock goes after the buffer. */
	if (env->platform->display_ver >= 12U)
		drv_i915_xd_pipe_clock_off(env->io, out->trans);

	/* The panel's power, with VDD held for the sequence. */
	if (out->kind == I915_XD_SCREEN_PANEL) {
		(void)i915_xd_output_panel(env, I915_XD_OUTPUT_PANEL_VDD_ON);
		(void)i915_xd_output_panel(env, I915_XD_OUTPUT_PANEL_OFF);
	}

	/* The port's IO power, then its clock. */
	i915_xd_output_give(out, env, I915_XD_OUTPUT_POWER_DDI_IO);
	i915_xd_output_clock_off(out, env);

	/* Reports a stop that was not confirmed. */
	if (error != 0)
		return error;

	/* Succeeded: the port is off. */
	return 0;
}

/*
 * Takes the pipe off the port's PLL, under the PLLs' lock; the last pipe
 * turns it off (the Thunderbolt PLL is the caller's).  Returns 0,
 * EINVAL when the pipe is not on the PLL, or EIO when the PLL did not
 * unlock or power down.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_dpll_mgr.c (intel_disable_shared_dpll,
 * icl_pll_disable).
 */
int
drv_i915_xd_output_pll_off(
	struct i915_xd_output *out,
	const struct i915_xd_output_env *env)
{
	const struct i915_xd_pll_desc *pll;
	uint8_t pipe_bit;
	unsigned pll_id;
	unsigned late;
	int own;
	int error;

	/* The pipe's bit among the PLL's users. */
	pipe_bit = (uint8_t)(1U << out->pipe);

	/* The Thunderbolt PLL the Type-C ports share is the caller's. */
	own = i915_xd_output_tc_own_pll(out);
	if (out->tc && !own) {
		error = env->tc->tbt_pll(env->context, 0);
		out->pll_pipes &= (uint8_t)~pipe_bit;
		out->pll_on = 0;
		if (error != 0)
			return error;
		return 0;
	}

	/* Leaves the PLL or turns it off, under the PLLs' lock. */
	i915_xd_output_lock(env, I915_XD_OUTPUT_LOCK_PLL, 1);

	/* The pipe does not run on the PLL. */
	if ((out->pll_pipes & pipe_bit) == 0U) {
		i915_xd_output_lock(env, I915_XD_OUTPUT_LOCK_PLL, 0);
		i915_xd_output_error(env, "xd-output: the pipe is not on the PLL\n");
		return EINVAL;
	}

	/* The PLL of a running pipe is on. */
	if (!out->pll_on)
		i915_xd_output_error(env, "xd-output: the PLL of a running pipe is off\n");

	/* Another pipe keeps the PLL. */
	out->pll_pipes &= (uint8_t)~pipe_bit;
	if (out->pll_pipes != 0U) {
		i915_xd_output_lock(env, I915_XD_OUTPUT_LOCK_PLL, 0);
		return 0;
	}

	/* The last pipe turns it off. */
	late = 0U;
	pll_id = out->pll;
	if (out->tc)
		pll_id = out->tc_pll;
	pll = drv_i915_xd_dpll_desc(env->platform, pll_id);
	if (pll != NULL)
		late = drv_i915_xd_dpll_disable(env->io, pll);
	out->pll_on = 0;

	i915_xd_output_lock(env, I915_XD_OUTPUT_LOCK_PLL, 0);

	/* Reports the waits that ran out. */
	i915_xd_output_pll_late(env, late);
	if (late != 0U)
		return EIO;

	/* Succeeded: the pipe left the PLL. */
	return 0;
}

/*
 * Gives the AUX power of the port's link back after its PLL is off, then
 * a Type-C port's link to its PHY.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi.c
 * (intel_ddi_post_pll_disable, main_link_aux_power_domain_put), intel_tc.c
 * (intel_tc_port_put_link).
 */
int
drv_i915_xd_output_link_power_off(
	struct i915_xd_output *out,
	const struct i915_xd_output_env *env)
{
	/* The AUX channel's power. */
	i915_xd_output_give(out, env, I915_XD_OUTPUT_POWER_AUX);

	/* A Type-C port's link. */
	if (out->tc_linked) {
		env->tc->put_link(env->context);
		out->tc_linked = 0;
	}

	/* Succeeded: the link's power is given back. */
	return 0;
}

/*
 * Ends a confirmed dark: the display buffer moved to the layout without
 * the pipe (the bus, the slices of both layouts, the credits, then the
 * new slices alone) and the pipe's power given back.  The caller lets DC
 * states back (drv_i915_xd_output_dc_allow) unless something failed.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_display.c (intel_atomic_commit_tail),
 * skl_watermark.c (intel_dbuf_pre_plane_update, intel_mbus_dbox_update,
 * intel_dbuf_post_plane_update).
 */
int
drv_i915_xd_output_dark_settle(
	struct i915_xd_output *out,
	const struct i915_xd_output_env *env)
{
	/* The buffer without the pipe. */
	i915_xd_output_dbuf_before(out, env);
	i915_xd_output_dbuf_after(out, env);

	/* The pipe's power. */
	i915_xd_output_give_crtc(out, env);

	/* Succeeded: the dark is settled. */
	return 0;
}

/*
 * Lets DC states back after a light or a dark, after a delay that keeps
 * a commit following soon from switching them off and on again.
 */
void
drv_i915_xd_output_dc_allow(
	struct i915_xd_output *out,
	const struct i915_xd_output_env *env)
{
	int cookie;

	/* Nothing held. */
	if ((out->held & (1U << I915_XD_OUTPUT_POWER_DC_OFF)) == 0U)
		return;

	/* The reference goes back later. */
	cookie = out->cookie[I915_XD_OUTPUT_POWER_DC_OFF];
	out->held &= ~(1U << I915_XD_OUTPUT_POWER_DC_OFF);
	out->cookie[I915_XD_OUTPUT_POWER_DC_OFF] = 0;
	env->power_put_later(env->context, I915_XD_OUTPUT_POWER_DC_OFF, 0U, cookie, I915_XD_OUTPUT_DC_DELAY_MS);
}

/*
 * Turns the panel's backlight on: the PWM at the level (a level at or
 * below the lowest comes back at the highest, under the backlight's
 * lock), then the sequencer's enable.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dp.c
 * (intel_edp_backlight_on), intel_backlight.c (intel_backlight_enable,
 * __intel_backlight_enable, cnp_enable_backlight), intel_pps.c
 * (intel_pps_backlight_on).
 */
void
drv_i915_xd_output_backlight_on(
	struct i915_xd_output *out,
	const struct i915_xd_output_env *env)
{
	struct i915_xd_output_backlight *backlight;
	uint32_t duty;

	/* The panel's backlight. */
	backlight = &out->backlight;

	/* The PWM, when the panel has a backlight. */
	if (backlight->present) {
		i915_xd_output_lock(env, I915_XD_OUTPUT_LOCK_BACKLIGHT, 1);

		/* A level that would be dark comes back at the highest. */
		if (backlight->max == 0U)
			i915_xd_output_error(env, "xd-output: the backlight has no range\n");
		if (backlight->level <= backlight->min)
			backlight->level = backlight->max;

		/* Starts the PWM at the level. */
		duty = i915_xd_output_invert(out, backlight->level);
		(void)drv_i915_xd_backlight_enable(&backlight->pwm, env->io, duty);
		backlight->enabled = 1;

		i915_xd_output_lock(env, I915_XD_OUTPUT_LOCK_BACKLIGHT, 0);
	}

	/* The sequencer's backlight enable. */
	(void)i915_xd_output_panel(env, I915_XD_OUTPUT_PANEL_BACKLIGHT_ON);
}

/*
 * Turns the panel's backlight off: the sequencer's enable, then the PWM
 * at the lowest duty under the backlight's lock.  The level is kept for
 * the next on.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dp.c
 * (intel_edp_backlight_off), intel_pps.c (intel_pps_backlight_off),
 * intel_backlight.c (intel_backlight_disable, cnp_disable_backlight).
 */
void
drv_i915_xd_output_backlight_off(
	struct i915_xd_output *out,
	const struct i915_xd_output_env *env)
{
	struct i915_xd_output_backlight *backlight;
	uint32_t duty;

	/* The panel's backlight. */
	backlight = &out->backlight;

	/* The sequencer's backlight enable. */
	(void)i915_xd_output_panel(env, I915_XD_OUTPUT_PANEL_BACKLIGHT_OFF);

	/* A panel without a backlight has no PWM. */
	if (!backlight->present)
		return;

	/* The PWM stops at the duty of level 0. */
	i915_xd_output_lock(env, I915_XD_OUTPUT_LOCK_BACKLIGHT, 1);

	backlight->enabled = 0;
	duty = i915_xd_output_invert(out, 0U);
	drv_i915_xd_backlight_disable(&backlight->pwm, env->io, duty);

	i915_xd_output_lock(env, I915_XD_OUTPUT_LOCK_BACKLIGHT, 0);
}

/*
 * Sets the panel's brightness to user_level of user_max, scaled to the
 * backlight's range from its lowest level up; a backlight that is on
 * shows it at once, one that is off takes it at the next on.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_backlight.c (intel_panel_set_backlight,
 * scale_user_to_hw, intel_panel_actually_set_backlight).
 */
void
drv_i915_xd_output_brightness(
	struct i915_xd_output *out,
	const struct i915_xd_output_env *env,
	uint32_t user_level,
	uint32_t user_max)
{
	struct i915_xd_output_backlight *backlight;
	uint32_t level;

	/* The panel's backlight. */
	backlight = &out->backlight;

	/* A panel without a backlight has no brightness. */
	if (!backlight->present)
		return;

	/* The new level, under the backlight's lock. */
	i915_xd_output_lock(env, I915_XD_OUTPUT_LOCK_BACKLIGHT, 1);

	/* The user's range onto the backlight's. */
	if (backlight->max == 0U)
		i915_xd_output_error(env, "xd-output: the backlight has no range\n");
	(void)i915_xd_output_scale(env, user_level, 0U, user_max, backlight->min, backlight->max, &level);
	backlight->level = level;

	/* A backlight that is on shows it. */
	if (backlight->enabled)
		drv_i915_xd_backlight_set_duty(&backlight->pwm, env->io, i915_xd_output_invert(out, level));

	i915_xd_output_lock(env, I915_XD_OUTPUT_LOCK_BACKLIGHT, 0);
}

/*
 * Sets the panel's brightness the firmware asks for (an OpRegion
 * request): user_level of user_max scaled to the whole range, then
 * raised to the backlight's lowest level.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_backlight.c (intel_backlight_set_acpi,
 * clamp_user_to_hw).
 */
void
drv_i915_xd_output_brightness_acpi(
	struct i915_xd_output *out,
	const struct i915_xd_output_env *env,
	uint32_t user_level,
	uint32_t user_max)
{
	struct i915_xd_output_backlight *backlight;
	uint32_t level;

	/* The panel's backlight. */
	backlight = &out->backlight;

	/* A panel without a backlight has no brightness. */
	if (!backlight->present)
		return;

	/* The new level, under the backlight's lock. */
	i915_xd_output_lock(env, I915_XD_OUTPUT_LOCK_BACKLIGHT, 1);

	/* The user's range onto the whole range, kept within the backlight's. */
	if (backlight->max == 0U)
		i915_xd_output_error(env, "xd-output: the backlight has no range\n");
	(void)i915_xd_output_scale(env, user_level, 0U, user_max, 0U, backlight->max, &level);
	if (level < backlight->min)
		level = backlight->min;
	if (level > backlight->max)
		level = backlight->max;
	backlight->level = level;

	/* A backlight that is on shows it. */
	if (backlight->enabled)
		drv_i915_xd_backlight_set_duty(&backlight->pwm, env->io, i915_xd_output_invert(out, level));

	i915_xd_output_lock(env, I915_XD_OUTPUT_LOCK_BACKLIGHT, 0);
}

/*
 * Returns the panel's brightness as a level of user_max: the level in
 * the backlight's range scaled to the user's (0 for an empty range).
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_backlight.c (scale_hw_to_user).
 */
uint32_t
drv_i915_xd_output_user_level(
	const struct i915_xd_output *out,
	uint32_t user_max)
{
	const struct i915_xd_output_backlight *backlight;
	uint32_t level;

	/* The panel's backlight. */
	backlight = &out->backlight;

	/* An empty range has no levels to scale from. */
	if (backlight->min >= backlight->max)
		return 0U;

	/* The backlight's range onto the user's. */
	level = drv_i915_xd_backlight_scale(backlight->level, backlight->min, backlight->max, 0U, user_max);

	/* Succeeded: the user's level. */
	return level;
}

/*
 * Lights a display 9 external output on its pipe (whose transcoder has
 * the same number): the DPLL; for DisplayPort the buffer's word, the DDI
 * clock, the IO well, the translation rows at the lowest level, the
 * sink's power and the training, then the transcoder's clock and the
 * stream's attributes; for HDMI the DDI clock, the IO well and the
 * transcoder's clock.  Then the pipe and the transcoder, the DDI function
 * and the transcoder's start, an HDMI port's buffer, and the plane.
 * Returns 0, or an error with what was started stopped again.
 */
int
drv_i915_xd9_output_on(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform,
	const struct i915_xd9_output *out,
	struct i915_xd9_dp_port *port)
{
	struct i915_xd_msa msa;
	uint32_t buffer;
	int error;

	/* The DPLL; one that does not lock is stopped again. */
	error = drv_i915_xd9_dpll_enable(io, platform, out->dpll, &out->dpll_words);
	if (error != 0) {
		drv_i915_xd9_dpll_disable(io, platform, out->dpll);
		return error;
	}

	/* An HDMI port: the clock and the IO well, then the transcoder's clock. */
	if (out->hdmi) {
		drv_i915_xd9_ddi_clock(io, out->ddi, out->dpll, 1);
		error = drv_i915_xd9_power_ddi_io(io, platform, out->ddi, 1);
		if (error != 0) {
			drv_i915_xd9_ddi_clock(io, out->ddi, out->dpll, 0);
			drv_i915_xd9_dpll_disable(io, platform, out->dpll);
			return error;
		}
		drv_i915_xd_pipe_clock_on(io, platform, out->pipe, out->ddi);
	} else {
		/* A DisplayPort port: the buffer's word keeps the lane reversal and has the width. */
		buffer = drv_i915_xd_io_read(io, I915_XD9_DDI_BUF_CTL(out->ddi));
		port->buf_word = (buffer & I915_XD9_DDI_BUF_REVERSAL) | ((out->lanes - 1U) << I915_XD9_DDI_BUF_WIDTH_SHIFT);

		/* The clock, the IO well, the rows at the lowest level, the sink awake, and the training. */
		drv_i915_xd9_ddi_clock(io, out->ddi, out->dpll, 1);
		error = drv_i915_xd9_power_ddi_io(io, platform, out->ddi, 1);
		if (error == 0) {
			drv_i915_xd9_ddi_buffers(io, out->ddi, port->table, port->rows);
			drv_i915_xd9_ddi_levels(io, out->ddi, port->table, port->rows, 0U, &port->buf_word);
			(void)drv_i915_xd9_dp_sink_power(port, I915_XD_DPCD_POWER_D0);
			error = drv_i915_xd9_dp_train(port, out->rate_khz, out->lanes);
		}

		/* A link that did not come up stops what was started. */
		if (error != 0) {
			drv_i915_xd9_ddi_buffer_off(io, out->ddi);
			(void)drv_i915_xd9_power_ddi_io(io, platform, out->ddi, 0);
			drv_i915_xd9_ddi_clock(io, out->ddi, out->dpll, 0);
			drv_i915_xd9_dpll_disable(io, platform, out->dpll);
			return error;
		}

		/* The transcoder's clock from the DDI, and the stream at 8 bits a colour. */
		drv_i915_xd_pipe_clock_on(io, platform, out->pipe, out->ddi);
		kern_memset(&msa, 0, sizeof(msa));
		msa.bpp = 24U;
		drv_i915_xd_pipe_msa(io, out->pipe, &msa);
	}

	/* The pipe and the transcoder for the mode, still off. */
	if (out->hdmi) {
		drv_i915_xd9_pipe_configure(io, platform, out->pipe, &out->mode, NULL);
	} else {
		drv_i915_xd9_pipe_configure(io, platform, out->pipe, &out->mode, &out->m_n);
	}

	/* The DDI function, then the transcoder running; one that does not run darkens the output again. */
	drv_i915_xd9_pipe_func(io, out->pipe, out->ddi, out->hdmi, out->lanes, &out->mode);
	error = drv_i915_xd9_pipe_start(io, out->pipe);
	if (error != 0) {
		drv_i915_xd9_output_off(io, platform, out, port);
		return error;
	}

	/* An HDMI port's buffer starts after the transcoder. */
	if (out->hdmi)
		drv_i915_xd9_ddi_hdmi_start(io, out->ddi, out->hdmi_row);

	/* Succeeded: the plane shows the first surface. */
	drv_i915_xd9_plane_on(io, out->pipe, out->surf, out->stride_units, out->mode.hdisplay, out->mode.vdisplay, out->rgbx);
	return 0;
}

/*
 * Darkens a display 9 external output: the plane at a vertical blank,
 * the transcoder and its DDI function, a DisplayPort sink to sleep, the
 * transcoder's clock, the buffer, the IO well, the DDI clock and the DPLL.
 */
void
drv_i915_xd9_output_off(
	struct i915_xd_io *io,
	const struct i915_xd_platform *platform,
	const struct i915_xd9_output *out,
	struct i915_xd9_dp_port *port)
{
	/* The plane off at a vertical blank. */
	drv_i915_xd9_plane_off(io, out->pipe);
	(void)drv_i915_xd9_pipe_wait_vblank(io, out->pipe);

	/* The transcoder and its function. */
	drv_i915_xd9_pipe_stop(io, out->pipe);

	/* A DisplayPort sink sleeps before its port stops. */
	if (!out->hdmi && port != NULL)
		(void)drv_i915_xd9_dp_sink_power(port, I915_XD_DPCD_POWER_D3);

	/* Succeeded: the clocks, the buffer, the well and the DPLL. */
	drv_i915_xd_pipe_clock_off(io, out->pipe);
	drv_i915_xd9_ddi_buffer_off(io, out->ddi);
	(void)drv_i915_xd9_power_ddi_io(io, platform, out->ddi, 0);
	drv_i915_xd9_ddi_clock(io, out->ddi, out->dpll, 0);
	drv_i915_xd9_dpll_disable(io, platform, out->dpll);
}

/* Returns the index a power use of the output names: its pipe, transcoder, DDI or AUX channel. */
static unsigned
i915_xd_output_index(
	const struct i915_xd_output *out,
	unsigned use)
{
	/* The unit the use powers. */
	switch (use) {
	case I915_XD_OUTPUT_POWER_PIPE:
		return out->pipe;
	case I915_XD_OUTPUT_POWER_TRANS:
		return out->trans;
	case I915_XD_OUTPUT_POWER_LANES:
	case I915_XD_OUTPUT_POWER_DDI_IO:
		return out->ddi;
	case I915_XD_OUTPUT_POWER_AUX:
		return out->aux;
	default:
		break;
	}

	/* Succeeded: the display-wide uses have no index. */
	return 0U;
}

/* Takes one power use of the output unless it holds it: 0, or EIO. */
static int
i915_xd_output_take(
	struct i915_xd_output *out,
	const struct i915_xd_output_env *env,
	unsigned use)
{
	unsigned index;
	int cookie;

	/* A use held already. */
	if ((out->held & (1U << use)) != 0U)
		return 0;

	/* The caller's reference. */
	index = i915_xd_output_index(out, use);
	cookie = env->power_get(env->context, use, index);
	if (cookie == 0) {
		i915_xd_output_error(env, "xd-output: a power reference could not be taken\n");
		return EIO;
	}

	/* Succeeded: the use is held until it is given back. */
	out->held |= 1U << use;
	out->cookie[use] = cookie;
	return 0;
}

/* Gives one power use of the output back, if it holds it. */
static void
i915_xd_output_give(
	struct i915_xd_output *out,
	const struct i915_xd_output_env *env,
	unsigned use)
{
	unsigned index;
	int cookie;

	/* A use not held. */
	if ((out->held & (1U << use)) == 0U)
		return;

	/* The caller's reference goes back. */
	index = i915_xd_output_index(out, use);
	cookie = out->cookie[use];
	out->held &= ~(1U << use);
	out->cookie[use] = 0;
	env->power_put(env->context, use, index, cookie);
}

/* Hands a note that counts against the run to the caller. */
static void
i915_xd_output_error(
	const struct i915_xd_output_env *env,
	const char *text)
{
	/* The caller's error sink, when it has one. */
	if (env->error != NULL)
		env->error(env->context, text);
}

/* Hands a debug note to the caller. */
static void
i915_xd_output_debug(
	const struct i915_xd_output_env *env,
	const char *text)
{
	/* The caller's debug sink, when it has one. */
	if (env->debug != NULL)
		env->debug(env->context, text);
}

/* Takes or drops one of the output's locks. */
static void
i915_xd_output_lock(
	const struct i915_xd_output_env *env,
	unsigned lock,
	int taken)
{
	/* The caller's lock, when it has one. */
	if (env->lock != NULL)
		env->lock(env->context, lock, taken);
}

/* Runs one step of the panel's power sequencer: 0, or its error. */
static int
i915_xd_output_panel(
	const struct i915_xd_output_env *env,
	unsigned step)
{
	int error;

	/* A caller without a sequencer has nothing to run. */
	if (env->panel == NULL)
		return 0;

	/* The step. */
	error = env->panel(env->context, step);
	if (error != 0)
		return error;

	/* Succeeded: the step ran. */
	return 0;
}

/* Marks a point of the run for the caller. */
static void
i915_xd_output_observe(
	const struct i915_xd_output_env *env,
	unsigned point)
{
	/* The caller's mark, when it looks. */
	if (env->observe != NULL)
		env->observe(env->context, point);
}

/*
 * Moves the display buffer to its new layout before the planes do: the
 * bus joined or split and the slices of both layouts on, when either
 * changes; then the data box credits of the pipe, when the joining or
 * the running pipes change and the pipe runs after it.
 */
static void
i915_xd_output_dbuf_before(
	struct i915_xd_output *out,
	const struct i915_xd_output_env *env)
{
	const struct i915_xd_output_dbuf *dbuf;
	uint32_t credits;

	/* The output's display buffer. */
	dbuf = &out->dbuf;

	/* The bus and both layouts' slices. */
	if (dbuf->slices_before != dbuf->slices_after || dbuf->joined_before != dbuf->joined_after) {
		drv_i915_xd_dbuf_mbus(env->io, env->platform, dbuf->joined_after);
		env->dbuf_slices(env->context, (uint8_t)(dbuf->slices_before | dbuf->slices_after));
	}

	/* Neither the joining nor the running pipes change: the credits stay. */
	if (dbuf->joined_before == dbuf->joined_after && dbuf->pipes_before == dbuf->pipes_after)
		return;

	/* A pipe that does not run after it has no credits to set. */
	if ((dbuf->pipes_after & (1U << out->pipe)) == 0U)
		return;

	/* The pipe's credits for the bus as it is joined after it. */
	credits = drv_i915_xd_dbuf_dbox_word(env->platform, dbuf->joined_after);
	drv_i915_xd_io_write(env->io, I915_XD_PIPE_MBUS_DBOX_CTL(out->pipe), credits);
}

/* Leaves only the new layout's slices on after the planes moved, when the layout changed. */
static void
i915_xd_output_dbuf_after(
	struct i915_xd_output *out,
	const struct i915_xd_output_env *env)
{
	const struct i915_xd_output_dbuf *dbuf;

	/* The output's display buffer. */
	dbuf = &out->dbuf;

	/* Nothing changed. */
	if (dbuf->slices_before == dbuf->slices_after && dbuf->joined_before == dbuf->joined_after)
		return;

	/* The new slices alone. */
	env->dbuf_slices(env->context, dbuf->slices_after);
}

/* Gives back the pipe's power uses marked at the start of the light or the dark, in the order they were taken. */
static void
i915_xd_output_give_crtc(
	struct i915_xd_output *out,
	const struct i915_xd_output_env *env)
{
	unsigned uses;

	/* Takes the uses whose give-back is deferred. */
	uses = out->put_later;
	out->put_later = 0U;

	/* The display core, the pipe, the transcoder, the lanes. */
	if ((uses & (1U << I915_XD_OUTPUT_POWER_CORE)) != 0U)
		i915_xd_output_give(out, env, I915_XD_OUTPUT_POWER_CORE);
	if ((uses & (1U << I915_XD_OUTPUT_POWER_PIPE)) != 0U)
		i915_xd_output_give(out, env, I915_XD_OUTPUT_POWER_PIPE);
	if ((uses & (1U << I915_XD_OUTPUT_POWER_TRANS)) != 0U)
		i915_xd_output_give(out, env, I915_XD_OUTPUT_POWER_TRANS);
	if ((uses & (1U << I915_XD_OUTPUT_POWER_LANES)) != 0U)
		i915_xd_output_give(out, env, I915_XD_OUTPUT_POWER_LANES);
}

/*
 * Brings an HDMI port up before the pipe: the DDI clock from the PLL,
 * the port's IO power, the transcoder's clock and the data island
 * packets off (the sink is driven in DVI mode).  Returns 0, or EIO.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi.c
 * (intel_ddi_pre_enable_hdmi), intel_hdmi.c (hsw_set_infoframes).
 */
static int
i915_xd_output_port_hdmi(
	struct i915_xd_output *out,
	const struct i915_xd_output_env *env)
{
	int error;

	/* The DDI clock, then the port's IO power. */
	i915_xd_output_clock_on(out, env);
	error = i915_xd_output_take(out, env, I915_XD_OUTPUT_POWER_DDI_IO);
	if (error != 0)
		return error;

	/* The transcoder's clock and the packets. */
	i915_xd_output_trans_clock(out, env);
	i915_xd_output_islands(out, env, 0);

	/* Succeeded: the port is ready for its buffer. */
	return 0;
}

/*
 * Starts the port's DDI clock: a combo PHY's mapped to its PLL, a
 * Type-C port's from its own PLL (DisplayPort alternate or legacy mode)
 * or the Thunderbolt PLL, under the PLLs' lock.
 */
static void
i915_xd_output_clock_on(
	struct i915_xd_output *out,
	const struct i915_xd_output_env *env)
{
	struct i915_xd_pll_desc source;
	uint32_t select;

	/* Selects the port's clock under the PLL lock. */
	i915_xd_output_lock(env, I915_XD_OUTPUT_LOCK_PLL, 1);

	/* A Type-C port's select (its own PLL in DisplayPort alternate or legacy mode, the Thunderbolt PLL otherwise) and gate. */
	if (out->tc) {
		kern_memset(&source, 0, sizeof(source));
		source.kind = I915_XD_PLL_TBT;
		if (out->tc_mode == I915_XD_OUTPUT_TC_DP_ALT || out->tc_mode == I915_XD_OUTPUT_TC_LEGACY)
			source.kind = I915_XD_PLL_TC;
		select = drv_i915_xd_tc_clock_select(&source, out->rate_khz);
		drv_i915_xd_tc_clock_map(env->io, out->ddi, out->tc_port, select);
	} else {
		drv_i915_xd_dpll_map(env->io, env->platform, out->phy, out->pll);
	}

	i915_xd_output_lock(env, I915_XD_OUTPUT_LOCK_PLL, 0);
}

/* Gates the port's DDI clock under the PLLs' lock (a Type-C port's source is deselected too). */
static void
i915_xd_output_clock_off(
	struct i915_xd_output *out,
	const struct i915_xd_output_env *env)
{
	i915_xd_output_lock(env, I915_XD_OUTPUT_LOCK_PLL, 1);

	/* The gate. */
	if (out->tc) {
		drv_i915_xd_tc_clock_unmap(env->io, out->ddi, out->tc_port);
	} else {
		drv_i915_xd_dpll_unmap(env->io, env->platform, out->phy);
	}

	i915_xd_output_lock(env, I915_XD_OUTPUT_LOCK_PLL, 0);
}

/*
 * Directs the port's clock to the transcoder: the PHY's on display 13,
 * the DDI's before (display 11's eDP transcoder has no select).
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi.c
 * (intel_ddi_enable_transcoder_clock).
 */
static void
i915_xd_output_trans_clock(
	struct i915_xd_output *out,
	const struct i915_xd_output_env *env)
{
	unsigned which;

	/* The clock's source by the display version. */
	which = out->ddi;
	if (env->platform->display_ver >= 13U)
		which = out->phy;
	drv_i915_xd_pipe_clock_on(env->io, env->platform, out->trans, which);
}

/* Describes the transcoder's function of the output in a mode: the DDI, the depth, the sync polarities, the lanes. */
static void
i915_xd_output_func(
	const struct i915_xd_output *out,
	unsigned mode,
	struct i915_xd_trans_func *func)
{
	/* No port sync master. */
	kern_memset(func, 0, sizeof(*func));
	func->ddi = out->ddi;
	func->mode = mode;
	func->bpp = out->bpp;
	func->vsync_high = out->vsync_high;
	func->hsync_high = out->hsync_high;
	func->lanes = out->lanes;
	func->sync_master = I915_XD_NO_TRANSCODER;
}

/*
 * Programs an HDMI port's signal levels: every lane at the VBT's level
 * of the platform's HDMI table, or at the table's own.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi.c
 * (icl_combo_phy_set_signal_levels, intel_ddi_hdmi_level).
 */
static void
i915_xd_output_hdmi_levels(
	struct i915_xd_output *out,
	const struct i915_xd_output_env *env)
{
	struct i915_xd_combo_levels levels;
	unsigned lane;
	unsigned row;

	/* The port's PHY, kind, clock and table. */
	kern_memset(&levels, 0, sizeof(levels));
	levels.phy = out->phy;
	levels.kind = I915_XD_OUTPUT_HDMI;
	levels.rate_khz = out->rate_khz;
	levels.lanes = out->lanes;
	levels.table = drv_i915_xd_combo_swing_table(env->platform, I915_XD_OUTPUT_HDMI, out->rate_khz, 0U);

	/* The VBT's row, or the table's own, within the table. */
	row = levels.table->hdmi_default;
	if (out->hdmi_level >= 0)
		row = (unsigned)out->hdmi_level;
	if (levels.table->count != 0U && row >= levels.table->count) {
		i915_xd_output_error(env, "xd-output: an HDMI level past the swing table\n");
		row = levels.table->count - 1U;
	}

	/* Every lane takes the row. */
	for (lane = 0U; lane < 4U; lane++)
		levels.level[lane] = (uint8_t)row;

	/* Programs them. */
	drv_i915_xd_combo_signal_levels(env->io, env->platform, &levels);
}

/* Describes the port's link to the DDI: its DDI and transcoder, its PHY's kind, its lanes and rate, the bits it keeps, its framing. */
static void
i915_xd_output_ddi_link(
	const struct i915_xd_output *out,
	const struct i915_xd_output_env *env,
	struct i915_xd_ddi_link *link)
{
	/* The single stream link of the output, on its transport. */
	kern_memset(link, 0, sizeof(*link));
	link->ddi = out->ddi;
	link->trans = i915_xd_output_transport(out, env);
	link->tc = out->tc;
	link->lanes = out->lanes;
	link->rate_khz = out->rate_khz;
	link->kept = out->kept;
	link->enhanced = out->enhanced;
}

/*
 * Chooses a combo port's DisplayPort swing table: the platform's table
 * for the rate, for the panel with the VBT's low swing and its
 * high-efficiency table unless a link failed with it.
 */
static const struct i915_xd_combo_swing_table *
i915_xd_output_swing_table(
	const struct i915_xd_output *out,
	const struct i915_xd_output_env *env)
{
	const struct i915_xd_combo_swing_table *table;
	unsigned vbt;

	/* An external sink's table by the rate alone. */
	if (out->kind != I915_XD_SCREEN_PANEL) {
		table = drv_i915_xd_combo_swing_table(env->platform, I915_XD_OUTPUT_DP, out->rate_khz, 0U);
		return table;
	}

	/* The VBT's settings, without the high-efficiency table after a failure. */
	vbt = out->swing_vbt;
	if (out->hobl_failed)
		vbt &= ~I915_XD_SWING_HOBL;

	/* Succeeded: the platform's table. */
	table = drv_i915_xd_combo_swing_table(env->platform, I915_XD_OUTPUT_EDP, out->rate_khz, vbt);
	return table;
}

/*
 * Describes the training of the port's link: the plan's rate and lanes,
 * the source's limits (the highest swing, pre-emphasis 3, levels per
 * lane, patterns 3 and 4), repeaters behind an external sink only, the
 * sink's capabilities the output keeps, the sink through the caller and
 * the source through the output's hooks.
 */
static void
i915_xd_output_dp_link(
	struct i915_xd_output_run *run,
	struct i915_xd_dp_link *link)
{
	const struct i915_xd_combo_swing_table *table;
	const struct i915_xd_dkl_swing_table *tc_table;
	const struct i915_xd_mg_swing_table *mg_table;
	struct i915_xd_output *out;
	unsigned rows;
	int mg;

	/* The output, and the link cleared. */
	out = run->out;
	kern_memset(link, 0, sizeof(*link));

	/* The link and its rate's entry in a panel's list. */
	link->rate_khz = out->rate_khz;
	link->lanes = out->lanes;
	link->enhanced = out->enhanced;
	link->rate_index = out->rate_index;

	/* The source's limits: the swing the port's table reaches (a Type-C PHY's Dekel or MG table, or the combo table). */
	mg = i915_xd_output_mg(out, run->env);
	if (out->tc && mg) {
		rows = 1U;
		mg_table = drv_i915_xd_tc_mg_swing_table(run->env->platform, 0, out->rate_khz);
		if (mg_table != NULL)
			rows = mg_table->count;
	} else if (out->tc) {
		rows = 1U;
		tc_table = drv_i915_xd_tc_swing_table(run->env->platform, 0, out->rate_khz);
		if (tc_table != NULL)
			rows = tc_table->count;
	} else {
		table = i915_xd_output_swing_table(out, run->env);
		rows = table->count;
	}

	/* The highest swing and pre-emphasis the table offers. */
	link->swing_max = drv_i915_xd_dp_swing_max(rows);
	link->preemph_max = 3U;
	link->per_lane = 1;
	link->tps3 = 1;
	link->tps4 = 1;

	/* Repeaters are looked for behind an external sink, not a panel; a trained link is up. */
	link->find_repeaters = 0;
	if (out->kind != I915_XD_SCREEN_PANEL)
		link->find_repeaters = 1;
	link->active = out->trained;

	/* The capabilities the output keeps. */
	link->caps = out->caps;
	link->repeater_caps = out->repeater_caps;
	link->repeater_phy_caps = out->repeater_phy_caps;

	/* The two sides. */
	link->sink = run->env->sink;
	link->source.levels = i915_xd_output_levels;
	link->source.pattern = i915_xd_output_pattern;
	link->source.prepare = i915_xd_output_prepare;
	link->source.idle = i915_xd_output_idle;
	link->source.sleep = i915_xd_output_sleep;
	link->source.note = i915_xd_output_note;
	link->source.context = run;
}

/*
 * Sets the sink's power state: D3 sleeps (unless the sink is a branch
 * whose ports signal hotplug only while awake), D0 writes a panel's
 * source identifier first and gives the sink three tries to wake up.  A sink
 * without power states, or one that does not answer, is only noted.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dp.c
 * (intel_dp_set_power, downstream_hpd_needs_d0, intel_edp_init_source_oui).
 */
static void
i915_xd_output_sink_power(
	struct i915_xd_output *out,
	const struct i915_xd_output_env *env,
	uint8_t state)
{
	static const uint8_t oui[I915_XD_OUTPUT_OUI_SIZE] = { 0x00U, 0xaaU, 0x01U };
	unsigned tries;
	int written;

	/* A sink before DPCD 1.1 has no power state. */
	if (out->caps[I915_XD_DPCD_REV] < I915_XD_OUTPUT_DPCD_POWER_REV)
		return;

	/* To sleep: a branch whose ports signal hotplug only while awake stays awake. */
	if (state != I915_XD_DPCD_POWER_D0) {
		if (out->caps[I915_XD_DPCD_REV] == I915_XD_OUTPUT_DPCD_REV_1_1 &&
		    (out->caps[I915_XD_OUTPUT_DPCD_PORTS] & I915_XD_OUTPUT_DPCD_BRANCH) != 0U)
			return;
		written = env->sink.write(env->sink.context, I915_XD_DPCD_SET_POWER, &state, 1U);
		if (written != 1)
			i915_xd_output_debug(env, "xd-output: the sink did not go to sleep\n");
		return;
	}

	/* A panel's source identifier, as early as possible. */
	if (out->kind == I915_XD_SCREEN_PANEL) {
		written = env->sink.write(env->sink.context, I915_XD_OUTPUT_SOURCE_OUI, oui, I915_XD_OUTPUT_OUI_SIZE);
		if (written < 0)
			i915_xd_output_error(env, "xd-output: the source's identifier was not written\n");
	}

	/* Wakes the sink, with time to wake between the tries. */
	written = 0;
	for (tries = 0U; tries < I915_XD_OUTPUT_WAKE_TRIES; tries++) {
		written = env->sink.write(env->sink.context, I915_XD_DPCD_SET_POWER, &state, 1U);
		if (written == 1)
			break;

		/* Waits for the sink to wake. */
		(void)drv_i915_xd_io_delay(env->io, I915_XD_OUTPUT_WAKE_US);
	}

	/* Notes a sink that did not wake. */
	if (written != 1)
		i915_xd_output_debug(env, "xd-output: the sink did not wake up\n");
}

/*
 * Turns the transcoder's data island packets off: the infoframes and the
 * VSC packet (no output here uses them: the panel has no self refresh,
 * the HDMI sink is in DVI mode), and the picture parameters too when a
 * DisplayPort output stops.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dp.c
 * (intel_dp_set_infoframes), intel_hdmi.c (hsw_set_infoframes).
 */
static void
i915_xd_output_islands(
	struct i915_xd_output *out,
	const struct i915_xd_output_env *env,
	int on)
{
	uint32_t packets;
	uint32_t control;

	/* The packets cleared. */
	packets = I915_XD_DIP_INFOFRAMES | I915_XD_DIP_VSC;
	if (!on && out->kind != I915_XD_SCREEN_HDMI)
		packets |= I915_XD_DIP_PPS;

	/* The control word without them, flushed. */
	control = drv_i915_xd_io_read(env->io, I915_XD_DIP_CTL(out->trans));
	drv_i915_xd_io_write(env->io, I915_XD_DIP_CTL(out->trans), control & ~packets);
	drv_i915_xd_io_posting_read(env->io, I915_XD_DIP_CTL(out->trans));
}

/*
 * Programs the port's signal levels from the training's level bytes: a
 * Type-C PHY's from its Dekel table; a combo port's from the swing table's
 * row of each lane (the high-efficiency table is noted).
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi.c
 * (icl_combo_phy_set_signal_levels, intel_ddi_level, intel_ddi_dp_level).
 */
static void
i915_xd_output_levels(
	void *context,
	const uint8_t set[4])
{
	struct i915_xd_combo_levels levels;
	struct i915_xd_output_run *run;
	struct i915_xd_output *out;
	unsigned lane;
	unsigned row;
	int error;

	/* The run, its output, and the levels the sink asked for. */
	run = context;
	out = run->out;
	kern_memcpy(out->set, set, sizeof(out->set));

	/* A Type-C PHY's levels. */
	if (out->tc) {
		i915_xd_output_tc_levels(run, set);
		return;
	}

	/* The port's PHY, kind, link and table. */
	kern_memset(&levels, 0, sizeof(levels));
	levels.phy = out->phy;
	levels.kind = I915_XD_OUTPUT_EDP;
	if (out->kind != I915_XD_SCREEN_PANEL)
		levels.kind = I915_XD_OUTPUT_DP;
	levels.rate_khz = out->rate_khz;
	levels.lanes = out->lanes;
	levels.table = i915_xd_output_swing_table(out, run->env);

	/* The row of each lane, within the table. */
	for (lane = 0U; lane < 4U; lane++) {
		error = drv_i915_xd_combo_level_row(set[lane], &row);
		if (error != 0)
			i915_xd_output_error(run->env, "xd-output: a level the swing tables do not have\n");
		if (levels.table->count != 0U && row >= levels.table->count)
			row = levels.table->count - 1U;
		levels.level[lane] = (uint8_t)row;
	}

	/* Whether the high-efficiency table drives the panel. */
	out->hobl_active = drv_i915_xd_combo_swing_is_hobl(run->env->platform, levels.table);

	/* Programs them. */
	drv_i915_xd_combo_signal_levels(run->env->io, run->env->platform, &levels);
}

/* Selects a training pattern on the port's transport. */
static void
i915_xd_output_pattern(
	void *context,
	uint8_t pattern)
{
	struct i915_xd_output_run *run;
	unsigned select;

	/* The run the hook belongs to. */
	run = context;

	/* The transport's field of the DPCD's pattern (an unknown one is pattern 1). */
	switch (pattern & I915_XD_OUTPUT_PATTERN_MASK) {
	case I915_XD_DPCD_PATTERN_NONE:
		select = I915_XD_DP_TP_PATTERN_NORMAL;
		break;
	case I915_XD_DPCD_PATTERN_2:
		select = I915_XD_DP_TP_PATTERN_2;
		break;
	case I915_XD_DPCD_PATTERN_3:
		select = I915_XD_DP_TP_PATTERN_3;
		break;
	case I915_XD_DPCD_PATTERN_4:
		select = I915_XD_DP_TP_PATTERN_4;
		break;
	default:
		select = I915_XD_DP_TP_PATTERN_1;
		break;
	}

	/* Writes it. */
	drv_i915_xd_ddi_pattern(run->env->io, i915_xd_output_transport(run->out, run->env), select);
}

/*
 * Starts the link in training pattern 1 with the DDI buffer enabled; an
 * Alder Lake-P Type-C PHY the display owns first leaves the core reset a
 * Thunderbolt link may have left.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi.c
 * (intel_ddi_prepare_link_retrain, adlp_tbt_to_dp_alt_switch_wa).
 */
static void
i915_xd_output_prepare(
	void *context)
{
	struct i915_xd_output_run *run;
	struct i915_xd_ddi_link ddi;
	void (*before_buffer)(void *context);
	unsigned late;
	int owned;

	/* The run the hook belongs to. */
	run = context;

	/* The PHY's reset, on a Type-C port in DisplayPort alternate or legacy mode of a platform that needs it. */
	before_buffer = NULL;
	owned = 0;
	if (run->out->tc_mode == I915_XD_OUTPUT_TC_DP_ALT || run->out->tc_mode == I915_XD_OUTPUT_TC_LEGACY)
		owned = 1;
	if (run->out->tc &&
	    owned &&
	    (run->env->platform->wa & I915_XD_WA_TC_RESET_AFTER_TBT) != 0U)
		before_buffer = i915_xd_output_phy_wa;

	/* The buffer's word with its enable. */
	i915_xd_output_ddi_link(run->out, run->env, &ddi);
	run->out->buf_word |= I915_XD_DDI_BUF_ENABLE;
	late = drv_i915_xd_ddi_dp_start(run->env->io, &ddi, run->out->buf_word & ~I915_XD_DDI_BUF_ENABLE, before_buffer, run);
	i915_xd_output_ddi_late(run->env, late);
}

/*
 * Releases a Type-C PHY's core reset, between the transport and the
 * buffer.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi.c
 * (adlp_tbt_to_dp_alt_switch_wa).
 */
static void
i915_xd_output_phy_wa(
	void *context)
{
	struct i915_xd_output_run *run;

	/* The run the hook belongs to. */
	run = context;

	/* Both lanes of the port's PHY. */
	drv_i915_xd_tc_release_soft_reset(run->env->dkl, run->out->tc_port);
}

/* Tells whether a Type-C port runs on its own PLL: in DisplayPort alternate or legacy mode (not Thunderbolt). */
static int
i915_xd_output_tc_own_pll(
	const struct i915_xd_output *out)
{
	/* A combo port has no Type-C PLL. */
	if (!out->tc)
		return 0;

	/* The modes whose PHY the display drives. */
	if (out->tc_mode == I915_XD_OUTPUT_TC_DP_ALT)
		return 1;
	if (out->tc_mode == I915_XD_OUTPUT_TC_LEGACY)
		return 1;

	/* Thunderbolt mode, or none: the Thunderbolt PLL. */
	return 0;
}

/*
 * Sets a Type-C PHY's DisplayPort mode for the link's lanes and the pin
 * assignment the connection recorded (a Thunderbolt port's PHY is not
 * the display's).  A pin assignment of a fixed connection outside legacy
 * mode, or one the PHY has no mode for, is an error.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi.c
 * (icl_program_mg_dp_mode).
 */
static void
i915_xd_output_tc_dp_mode(
	struct i915_xd_output *out,
	const struct i915_xd_output_env *env)
{
	unsigned pin;
	int error;
	int mg;

	/* A Thunderbolt port's PHY is the Thunderbolt controller's. */
	if (out->tc_mode == I915_XD_OUTPUT_TC_TBT)
		return;

	/* The pin assignment; only a legacy port has a fixed connection. */
	pin = env->tc->pin(env->context);
	if (pin == I915_XD_TC_PIN_LEGACY && out->tc_mode != I915_XD_OUTPUT_TC_LEGACY)
		i915_xd_output_error(env, "xd-output: a fixed connection on a Type-C port outside legacy mode\n");

	/* Both lanes' modes, in an MG PHY's registers or through the Dekel PHY's window. */
	mg = i915_xd_output_mg(out, env);
	if (mg) {
		error = drv_i915_xd_tc_mg_dp_mode(env->io, out->tc_port, pin, out->lanes);
	} else {
		error = drv_i915_xd_tc_dp_mode(env->dkl, out->tc_port, pin, out->lanes);
	}

	/* Reports a pin assignment the PHY has no mode for. */
	if (error != 0)
		i915_xd_output_error(env, "xd-output: a pin assignment the Type-C PHY has no mode for\n");
}

/*
 * Programs a Type-C PHY's signal levels from the link's level bytes: the
 * platform's Dekel (or on display 11 MG) DisplayPort table for the rate,
 * a row per lane (a Thunderbolt port's PHY is not the display's).
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi.c
 * (tgl_dkl_phy_set_signal_levels, icl_mg_phy_set_signal_levels,
 * intel_ddi_level, intel_ddi_dp_level).
 */
static void
i915_xd_output_tc_levels(
	struct i915_xd_output_run *run,
	const uint8_t set[4])
{
	const struct i915_xd_dkl_swing_table *table;
	const struct i915_xd_mg_swing_table *mg_table;
	uint8_t rows[4];
	unsigned lane;
	unsigned row;
	int error;
	int mg;

	/* A Thunderbolt port's PHY is the Thunderbolt controller's. */
	if (run->out->tc_mode == I915_XD_OUTPUT_TC_TBT)
		return;

	/* The row of each lane. */
	for (lane = 0U; lane < 4U; lane++) {
		error = drv_i915_xd_combo_level_row(set[lane], &row);
		if (error != 0)
			i915_xd_output_error(run->env, "xd-output: a level the swing tables do not have\n");
		rows[lane] = (uint8_t)row;
	}

	/* An MG PHY's transmitters, from its table for the rate. */
	mg = i915_xd_output_mg(run->out, run->env);
	if (mg) {
		mg_table = drv_i915_xd_tc_mg_swing_table(run->env->platform, 0, run->out->rate_khz);
		if (mg_table == NULL) {
			i915_xd_output_error(run->env, "xd-output: the platform has no Type-C swing table\n");
			return;
		}

		/* Writes the MG PHY's levels. */
		drv_i915_xd_tc_mg_signal_levels(run->env->io, run->out->tc_port, mg_table, rows, run->out->rate_khz);
		return;
	}

	/* The platform's Dekel table for the rate. */
	table = drv_i915_xd_tc_swing_table(run->env->platform, 0, run->out->rate_khz);
	if (table == NULL) {
		i915_xd_output_error(run->env, "xd-output: the platform has no Type-C swing table\n");
		return;
	}

	/* Programs the PHY's transmitters. */
	drv_i915_xd_tc_signal_levels(run->env->dkl, run->env->platform, run->out->tc_port, table, rows, 0);
}

/*
 * Tells where the port's transport is: display 11's is the port's own,
 * later ones are the transcoder's.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dp.c
 * (intel_dp_init_connector: dp_tp_ctl, dp_tp_status), intel_ddi.c
 * (intel_ddi_prepare_link_retrain).
 */
static unsigned
i915_xd_output_transport(
	const struct i915_xd_output *out,
	const struct i915_xd_output_env *env)
{
	/* Display 11: the port's. */
	if (env->platform->display_ver < 12U)
		return I915_XD_DP_TP_PORT(out->ddi);

	/* Succeeded: the transcoder's. */
	return out->trans;
}

/* Tells whether the port's PHY is an MG PHY (a display 11 Type-C port). */
static int
i915_xd_output_mg(
	const struct i915_xd_output *out,
	const struct i915_xd_output_env *env)
{
	const struct i915_xd_port_desc *port;

	/* The platform's port. */
	port = drv_i915_xd_platform_port(env->platform, out->ddi);
	if (port == NULL)
		return 0;
	if (port->phy_kind != I915_XD_PHY_MG)
		return 0;

	/* Succeeded: an MG PHY. */
	return 1;
}

/* Sends the idle pattern after the training and waits for it to be sent. */
static void
i915_xd_output_idle(
	void *context)
{
	struct i915_xd_output_run *run;
	unsigned late;

	/* The run the hook belongs to. */
	run = context;

	/* The idle pattern. */
	late = drv_i915_xd_ddi_idle_patterns(run->env->io, i915_xd_output_transport(run->out, run->env));
	i915_xd_output_ddi_late(run->env, late);
}

/* Lets the time between two steps of the training pass. */
static void
i915_xd_output_sleep(
	void *context,
	unsigned min_us,
	unsigned max_us)
{
	struct i915_xd_output_run *run;

	UNUSED_PARAMETER(max_us);

	/* The run the hook belongs to. */
	run = context;

	/* The shortest wait the step allows. */
	(void)drv_i915_xd_io_delay(run->env->io, min_us);
}

/* Hands a note of the training to the caller. */
static void
i915_xd_output_note(
	void *context,
	unsigned kind,
	const char *text)
{
	struct i915_xd_output_run *run;

	/* The run the hook belongs to. */
	run = context;

	/* An error counts against the run; a debug note is only told. */
	if (kind == I915_XD_DP_NOTE_ERROR) {
		i915_xd_output_error(run->env, text);
	} else {
		i915_xd_output_debug(run->env, text);
	}
}

/* Reports the DDI buffer's and the transport's waits that ran out. */
static void
i915_xd_output_ddi_late(
	const struct i915_xd_output_env *env,
	unsigned late)
{
	/* One error per wait. */
	if ((late & I915_XD_DDI_LATE_IDLE) != 0U)
		i915_xd_output_error(env, "xd-output: the DDI buffer did not go idle\n");
	if ((late & I915_XD_DDI_LATE_ACTIVE) != 0U)
		i915_xd_output_error(env, "xd-output: the DDI buffer did not leave idle\n");
	if ((late & I915_XD_DDI_LATE_IDLE_PATTERNS) != 0U)
		i915_xd_output_error(env, "xd-output: the idle patterns were not sent\n");
}

/* Reports the PLL's waits that ran out. */
static void
i915_xd_output_pll_late(
	const struct i915_xd_output_env *env,
	unsigned late)
{
	/* One error per wait. */
	if ((late & I915_XD_DPLL_LATE_POWER_ON) != 0U)
		i915_xd_output_error(env, "xd-output: the port PLL did not power up\n");
	if ((late & I915_XD_DPLL_LATE_LOCK) != 0U)
		i915_xd_output_error(env, "xd-output: the port PLL is not locked\n");
	if ((late & I915_XD_DPLL_LATE_UNLOCK) != 0U)
		i915_xd_output_error(env, "xd-output: the port PLL did not unlock\n");
	if ((late & I915_XD_DPLL_LATE_POWER_OFF) != 0U)
		i915_xd_output_error(env, "xd-output: the port PLL did not power down\n");
}

/*
 * Turns a backlight level into the PWM's duty: for a panel whose
 * brightness runs the other way, the range read backwards.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_backlight.c
 * (intel_backlight_invert_pwm_level).
 */
static uint32_t
i915_xd_output_invert(
	const struct i915_xd_output *out,
	uint32_t level)
{
	/* A panel the right way round. */
	if (!out->backlight.inverted)
		return level;

	/* Succeeded: the range read backwards. */
	return out->backlight.max - level + out->backlight.min;
}

/*
 * Scales a value from one range into another, rounded to the closest; an
 * empty source range or an inverted target range is noted and gives the
 * target's lowest.  Returns 0, or EINVAL for those ranges.
 */
static int
i915_xd_output_scale(
	const struct i915_xd_output_env *env,
	uint32_t value,
	uint32_t from_min,
	uint32_t from_max,
	uint32_t to_min,
	uint32_t to_max,
	uint32_t *scaled)
{
	/* The ranges must be usable. */
	*scaled = to_min;
	if (from_min >= from_max) {
		i915_xd_output_error(env, "xd-output: an empty range to scale from\n");
		return EINVAL;
	}

	/* Refuses an inverted range to scale to. */
	if (to_min > to_max) {
		i915_xd_output_error(env, "xd-output: an inverted range to scale to\n");
		return EINVAL;
	}

	/* Succeeded: the scaled value. */
	*scaled = drv_i915_xd_backlight_scale(value, from_min, from_max, to_min, to_max);
	return 0;
}
