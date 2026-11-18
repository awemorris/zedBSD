/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The built-in panel of the display 12 and 13 code (see xd-panel.h).
 */

#include "xd-panel.h"
#include "xd-io.h"

#include "../intel/display-regs-xd-south.h"
#include "../intel/display-regs-xd9.h"

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>

/* The top of the VBT's minimum brightness scale. */
#define I915_XD_BACKLIGHT_VBT_SCALE	255U

/*
 * The status of a sequencer at rest with the panel on, with the panel
 * off, and with the panel off and its power cycle over: the bits looked
 * at, and what they read.
 */
#define I915_XD_PPS_ON_MASK	(I915_XD_PP_STATUS_ON | I915_XD_PP_STATUS_SEQUENCE | I915_XD_PP_STATUS_STATE)
#define I915_XD_PPS_ON_VALUE	(I915_XD_PP_STATUS_ON | I915_XD_PP_STATUS_STATE_ON_IDLE)
#define I915_XD_PPS_OFF_MASK	(I915_XD_PP_STATUS_ON | I915_XD_PP_STATUS_SEQUENCE)
#define I915_XD_PPS_OFF_VALUE	0U
#define I915_XD_PPS_CYCLE_MASK	(I915_XD_PP_STATUS_ON | I915_XD_PP_STATUS_SEQUENCE | I915_XD_PP_STATUS_CYCLE_DELAY | I915_XD_PP_STATUS_STATE)
#define I915_XD_PPS_CYCLE_VALUE	I915_XD_PP_STATUS_STATE_OFF_IDLE

/* The sequencer's units: 100 us a unit, the power cycle in whole 100 ms. */
#define I915_XD_PPS_UNITS_PER_MS	10U
#define I915_XD_PPS_CYCLE_UNIT		1000U

/*
 * The longest delays the eDP 1.3 specification allows, in the
 * sequencer's units, used where neither the firmware nor the VBT gives
 * one: T1+T3 210 ms, T8 and T9 50 ms (the specification has none and
 * T7's is taken), T10 500 ms and T11+T12 510 ms, to which the power
 * cycle adds the 100 ms the sequencer counts from.
 *
 * Values referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_pps.c (pps_init_delays_spec).
 */
#define I915_XD_PPS_SPEC_POWER_UP	2100U
#define I915_XD_PPS_SPEC_BACKLIGHT_ON	500U
#define I915_XD_PPS_SPEC_BACKLIGHT_OFF	500U
#define I915_XD_PPS_SPEC_POWER_DOWN	5000U
#define I915_XD_PPS_SPEC_POWER_CYCLE	6100U

/*
 * The Sunrise Point family's PWM keeps its period in the upper half of its
 * second register (CTL2, where the Cannon Point family has FREQ) and its
 * duty in the lower half.
 *
 * Values referenced: Intel Kaby Lake PRM Volume 2c, Command Reference:
 * Registers (BLC_PWM_PCH_CTL2).
 */
#define I915_XD9_PWM_PERIOD_SHIFT	16U
#define I915_XD9_PWM_DUTY_MASK		0xffffU

/* Display 9's lowest duty for a brightness of 0 %: 4/255 of the period, rounded up, so the panel never goes dark. */
#define I915_XD9_PWM_FLOOR_255		4U
#define I915_XD9_PWM_SCALE_255		255U
#define I915_XD9_PWM_PERCENT_MAX	100U

static uint32_t i915_xd_pps_pick(uint32_t firmware, uint32_t vbt, uint32_t spec);
static unsigned i915_xd_pps_ms(uint32_t units);

/*
 * Tells whether a backlight controller can drive the panel.
 *
 * The south display has controllers controllers.  On the south displays
 * that share the second controller's pins with other functions (muxed
 * nonzero), the second controller is usable only when those pins are
 * given to the second panel power sequencer.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_backlight.c
 * (cnp_backlight_controller_is_valid).
 */
int
drv_i915_xd_backlight_controller_usable(
	struct i915_xd_io *io,
	unsigned controller,
	unsigned controllers,
	int muxed)
{
	uint32_t chicken;

	/* A controller the south display does not have. */
	if (controller >= controllers)
		return 0;

	/* The second controller of a muxed display needs its pins. */
	if (controller == 1U && muxed != 0) {
		chicken = drv_i915_xd_io_read(io, I915_XD_SOUTH_CHICKEN1);
		if ((chicken & I915_XD_SOUTH_CHICKEN1_SECOND_PPS_IO) == 0U)
			return 0;
	}

	/* Succeeded: the controller can drive the panel. */
	return 1;
}

/*
 * Reads how the firmware left a controller: its period, its polarity and
 * whether it runs.
 */
void
drv_i915_xd_backlight_read(
	struct i915_xd_backlight *backlight,
	struct i915_xd_io *io,
	unsigned controller)
{
	uint32_t control;

	/* The polarity and the enable. */
	backlight->controller = controller;
	control = drv_i915_xd_io_read(io, I915_XD_SBLC_PWM_CTL(controller));
	backlight->active_low = 0;
	if ((control & I915_XD_SBLC_PWM_POLARITY) != 0U)
		backlight->active_low = 1;
	backlight->running = 0;
	if ((control & I915_XD_SBLC_PWM_ENABLE) != 0U)
		backlight->running = 1;

	/* The period, 0 when nobody programmed one. */
	backlight->period = drv_i915_xd_io_read(io, I915_XD_SBLC_PWM_FREQ(controller));
}

/*
 * Returns the period of a PWM frequency: the raw clock ticks of one
 * period, rounded to the closest; 0 for a frequency of 0.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_backlight.c (cnp_hz_to_pwm).
 */
uint32_t
drv_i915_xd_backlight_period(
	unsigned rawclk_khz,
	unsigned hz)
{
	uint64_t ticks;

	/* No frequency has no period. */
	if (hz == 0U)
		return 0U;

	/* The raw clock's ticks per second shared out over the periods. */
	ticks = (uint64_t)rawclk_khz * 1000U;
	ticks = (ticks + hz / 2U) / hz;

	/* Succeeded: the period. */
	return (uint32_t)ticks;
}

/*
 * Returns the lowest duty of a period: the VBT's minimum brightness, a
 * share of 255, at most 64 of 255.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_backlight.c (get_backlight_min_vbt).
 */
uint32_t
drv_i915_xd_backlight_min_duty(
	uint32_t period,
	unsigned vbt_min)
{
	uint32_t duty;

	/* A minimum above a quarter is cut to it. */
	if (vbt_min > I915_XD_BACKLIGHT_MIN_LIMIT)
		vbt_min = I915_XD_BACKLIGHT_MIN_LIMIT;

	/* The share of the period. */
	duty = drv_i915_xd_backlight_scale(vbt_min, 0U, I915_XD_BACKLIGHT_VBT_SCALE, 0U, period);

	/* Succeeded: the lowest duty. */
	return duty;
}

/*
 * Moves a value from one range to another, rounded to the closest.
 *
 * The value is first held inside its range.  An empty source range or a
 * target range upside down answers the target's bottom; the caller
 * reports those.
 */
uint32_t
drv_i915_xd_backlight_scale(
	uint32_t value,
	uint32_t from_min,
	uint32_t from_max,
	uint32_t to_min,
	uint32_t to_max)
{
	uint64_t moved;

	/* Ranges that cannot be scaled answer the bottom. */
	if (from_min >= from_max)
		return to_min;
	if (to_min > to_max)
		return to_min;

	/* Holds the value inside its range. */
	if (value < from_min)
		value = from_min;
	if (value > from_max)
		value = from_max;

	/* Scales in 64 bits, so the product does not overflow. */
	moved = (uint64_t)(value - from_min) * (to_max - to_min);
	moved = (moved + (from_max - from_min) / 2U) / (from_max - from_min);
	moved += to_min;

	/* Succeeded: the value in the target range. */
	return (uint32_t)moved;
}

/*
 * Reads the duty the controller drives.
 */
uint32_t
drv_i915_xd_backlight_duty(
	const struct i915_xd_backlight *backlight,
	struct i915_xd_io *io)
{
	uint32_t duty;

	/* Reads the duty register. */
	duty = drv_i915_xd_io_read(io, I915_XD_SBLC_PWM_DUTY(backlight->controller));

	/* Succeeded: the duty. */
	return duty;
}

/*
 * Sets the duty the controller drives; a running PWM takes it at once.
 */
void
drv_i915_xd_backlight_set_duty(
	const struct i915_xd_backlight *backlight,
	struct i915_xd_io *io,
	uint32_t duty)
{
	/* Writes the duty register. */
	drv_i915_xd_io_write(io, I915_XD_SBLC_PWM_DUTY(backlight->controller), duty);
}

/*
 * Starts the PWM with a duty: a PWM still running is stopped first, then
 * the period and the duty are set, the polarity is set and pushed out,
 * and the PWM is started.  Returns 1 when the PWM had to be stopped
 * first, 0 otherwise.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_backlight.c (cnp_enable_backlight).
 */
int
drv_i915_xd_backlight_enable(
	struct i915_xd_backlight *backlight,
	struct i915_xd_io *io,
	uint32_t duty)
{
	uint32_t control_offset;
	uint32_t control;
	int was_running;

	/* Stops a PWM that still runs, so the new period takes effect cleanly. */
	control_offset = I915_XD_SBLC_PWM_CTL(backlight->controller);
	control = drv_i915_xd_io_read(io, control_offset);
	was_running = 0;
	if ((control & I915_XD_SBLC_PWM_ENABLE) != 0U) {
		was_running = 1;
		drv_i915_xd_io_write(io, control_offset, control & ~I915_XD_SBLC_PWM_ENABLE);
	}

	/* The period, then the duty. */
	drv_i915_xd_io_write(io, I915_XD_SBLC_PWM_FREQ(backlight->controller), backlight->period);
	drv_i915_xd_io_write(io, I915_XD_SBLC_PWM_DUTY(backlight->controller), duty);

	/* The polarity alone, pushed out before the enable. */
	control = 0U;
	if (backlight->active_low != 0)
		control |= I915_XD_SBLC_PWM_POLARITY;
	drv_i915_xd_io_write(io, control_offset, control);
	drv_i915_xd_io_posting_read(io, control_offset);

	/* Starts the PWM. */
	drv_i915_xd_io_write(io, control_offset, control | I915_XD_SBLC_PWM_ENABLE);
	backlight->running = 1;

	/* Succeeded: tells whether the PWM was found running. */
	return was_running;
}

/*
 * Stops the PWM after setting its duty.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_backlight.c (cnp_disable_backlight).
 */
void
drv_i915_xd_backlight_disable(
	struct i915_xd_backlight *backlight,
	struct i915_xd_io *io,
	uint32_t duty)
{
	/* The duty, then the enable cleared. */
	drv_i915_xd_io_write(io, I915_XD_SBLC_PWM_DUTY(backlight->controller), duty);
	(void)drv_i915_xd_io_rmw(io, I915_XD_SBLC_PWM_CTL(backlight->controller), I915_XD_SBLC_PWM_ENABLE, 0U);
	backlight->running = 0;
}

/*
 * Tells whether a panel power sequencer can drive the panel.
 *
 * The south display has sequencers sequencers.  On the south displays
 * that share the second sequencer's pins with other functions (muxed
 * nonzero), the second one works only when those pins are given to it.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_pps.c (intel_pps_is_valid).
 */
int
drv_i915_xd_pps_usable(
	struct i915_xd_io *io,
	unsigned pps,
	unsigned sequencers,
	int muxed)
{
	uint32_t chicken;

	/* A sequencer the south display does not have. */
	if (pps >= sequencers)
		return 0;

	/* The second sequencer of a muxed display needs its pins. */
	if (pps == 1U && muxed != 0) {
		chicken = drv_i915_xd_io_read(io, I915_XD_SOUTH_CHICKEN1);
		if ((chicken & I915_XD_SOUTH_CHICKEN1_SECOND_PPS_IO) == 0U)
			return 0;
	}

	/* Succeeded: the sequencer can drive the panel. */
	return 1;
}

/*
 * Finds the first sequencer, in index order, that has the panel powered
 * (I915_XD_PPS_FIND_POWERED), that holds VDD on (I915_XD_PPS_FIND_VDD),
 * or any (I915_XD_PPS_FIND_ANY): the firmware's choice when the VBT names
 * none.  Returns its index, or -1 when no sequencer qualifies.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_pps.c (bxt_initial_pps_idx,
 * pps_has_pp_on, pps_has_vdd_on, pps_any).
 */
int
drv_i915_xd_pps_find(
	struct i915_xd_io *io,
	unsigned sequencers,
	unsigned what)
{
	unsigned pps;
	int qualifies;

	/* Walks the sequencers in index order. */
	for (pps = 0U; pps < sequencers; pps++) {
		/* Asks the sequencer the question the caller has. */
		if (what == I915_XD_PPS_FIND_POWERED) {
			qualifies = drv_i915_xd_pps_powered(io, pps);
		} else if (what == I915_XD_PPS_FIND_VDD) {
			qualifies = drv_i915_xd_pps_vdd_forced(io, pps);
		} else {
			qualifies = 1;
		}

		/* The first one that qualifies is the answer. */
		if (qualifies != 0)
			return (int)pps;
	}

	/* No sequencer qualifies. */
	return -1;
}

/*
 * Tells whether a sequencer has the panel powered.
 */
int
drv_i915_xd_pps_powered(
	struct i915_xd_io *io,
	unsigned pps)
{
	uint32_t status;

	/* Reads the sequencer's status. */
	status = drv_i915_xd_io_read(io, I915_XD_PP_STATUS(pps));
	if ((status & I915_XD_PP_STATUS_ON) == 0U)
		return 0;

	/* Succeeded: the panel has its power. */
	return 1;
}

/*
 * Tells whether a sequencer holds the panel's VDD on by itself.
 */
int
drv_i915_xd_pps_vdd_forced(
	struct i915_xd_io *io,
	unsigned pps)
{
	uint32_t control;

	/* Reads the sequencer's control word. */
	control = drv_i915_xd_io_read(io, I915_XD_PP_CONTROL(pps));
	if ((control & I915_XD_PP_CONTROL_FORCE_VDD) == 0U)
		return 0;

	/* Succeeded: VDD is held on. */
	return 1;
}

/*
 * Tells whether a set of delays holds any delay.
 */
int
drv_i915_xd_pps_delays_set(
	const struct i915_xd_pps_delays *delays)
{
	/* Any delay makes the set one. */
	if (delays->power_up != 0U)
		return 1;
	if (delays->backlight_on != 0U)
		return 1;
	if (delays->backlight_off != 0U)
		return 1;
	if (delays->power_down != 0U)
		return 1;
	if (delays->power_cycle != 0U)
		return 1;

	/* Every delay is 0: the set holds nothing. */
	return 0;
}

/*
 * Reads the delays a sequencer holds: the four of the delay registers,
 * and the power cycle from the control word (from the CNP south display
 * on, the only ones this code drives), turned into 100 us units.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_pps.c (intel_pps_readout_hw_state).
 */
void
drv_i915_xd_pps_read_delays(
	struct i915_xd_io *io,
	unsigned pps,
	struct i915_xd_pps_delays *delays)
{
	uint32_t control;
	uint32_t on;
	uint32_t off;

	/* Reads the control word and the two delay registers. */
	control = drv_i915_xd_io_read(io, I915_XD_PP_CONTROL(pps));
	on = drv_i915_xd_io_read(io, I915_XD_PP_ON_DELAYS(pps));
	off = drv_i915_xd_io_read(io, I915_XD_PP_OFF_DELAYS(pps));

	/* Takes the fields out; the power cycle counts whole 100 ms. */
	delays->power_up = i915_xd_field_get(I915_XD_PP_DELAY_UPPER, on);
	delays->backlight_on = i915_xd_field_get(I915_XD_PP_DELAY_LOWER, on);
	delays->backlight_off = i915_xd_field_get(I915_XD_PP_DELAY_LOWER, off);
	delays->power_down = i915_xd_field_get(I915_XD_PP_DELAY_UPPER, off);
	delays->power_cycle = i915_xd_field_get(I915_XD_PP_CONTROL_CYCLE, control) * I915_XD_PPS_CYCLE_UNIT;
}

/*
 * Gives the longest delays the eDP 1.3 specification allows, the choice
 * where neither the firmware nor the VBT gives a delay.
 */
void
drv_i915_xd_pps_spec(
	struct i915_xd_pps_delays *delays)
{
	/* The specification's limits in the sequencer's units. */
	delays->power_up = I915_XD_PPS_SPEC_POWER_UP;
	delays->backlight_on = I915_XD_PPS_SPEC_BACKLIGHT_ON;
	delays->backlight_off = I915_XD_PPS_SPEC_BACKLIGHT_OFF;
	delays->power_down = I915_XD_PPS_SPEC_POWER_DOWN;
	delays->power_cycle = I915_XD_PPS_SPEC_POWER_CYCLE;
}

/*
 * Turns the VBT's panel delays into the sequencer's: the VBT counts the
 * power cycle from 0 where the sequencer counts from 100 ms.  A VBT
 * without delays stays empty.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_pps.c (pps_init_delays_vbt).
 */
void
drv_i915_xd_pps_from_vbt(
	struct i915_xd_pps_delays *delays)
{
	int set;

	/* A VBT without delays gives none. */
	set = drv_i915_xd_pps_delays_set(delays);
	if (set == 0)
		return;

	/* Adds the 100 ms the sequencer's power cycle starts from. */
	delays->power_cycle += I915_XD_PPS_CYCLE_UNIT;
}

/*
 * Chooses the panel's delays: for each, the larger of the firmware's and
 * the VBT's, or the specification's longest when neither gives one.  The
 * waits the caller times take the chosen delays in milliseconds; the
 * sequencer is programmed with the shortest backlight delays (the caller
 * waits those out itself, so they are not waited twice) and with the
 * power cycle rounded up to its 100 ms.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_pps.c (pps_init_delays).
 */
void
drv_i915_xd_pps_choose(
	const struct i915_xd_pps_delays *firmware,
	const struct i915_xd_pps_delays *vbt,
	struct i915_xd_pps_delays *program,
	struct i915_xd_pps_waits *waits)
{
	struct i915_xd_pps_delays spec;
	struct i915_xd_pps_delays chosen;

	/* Picks each delay. */
	drv_i915_xd_pps_spec(&spec);
	chosen.power_up = i915_xd_pps_pick(firmware->power_up, vbt->power_up, spec.power_up);
	chosen.backlight_on = i915_xd_pps_pick(firmware->backlight_on, vbt->backlight_on, spec.backlight_on);
	chosen.backlight_off = i915_xd_pps_pick(firmware->backlight_off, vbt->backlight_off, spec.backlight_off);
	chosen.power_down = i915_xd_pps_pick(firmware->power_down, vbt->power_down, spec.power_down);
	chosen.power_cycle = i915_xd_pps_pick(firmware->power_cycle, vbt->power_cycle, spec.power_cycle);

	/* The waits in milliseconds. */
	waits->power_up_ms = i915_xd_pps_ms(chosen.power_up);
	waits->backlight_on_ms = i915_xd_pps_ms(chosen.backlight_on);
	waits->backlight_off_ms = i915_xd_pps_ms(chosen.backlight_off);
	waits->power_down_ms = i915_xd_pps_ms(chosen.power_down);
	waits->power_cycle_ms = i915_xd_pps_ms(chosen.power_cycle);

	/*
	 * What the sequencer is given: the backlight's delays at their least,
	 * and the power cycle in whole 100 ms.
	 */
	*program = chosen;
	program->backlight_on = 1U;
	program->backlight_off = 1U;
	program->power_cycle = (chosen.power_cycle + I915_XD_PPS_CYCLE_UNIT - 1U) / I915_XD_PPS_CYCLE_UNIT * I915_XD_PPS_CYCLE_UNIT;
}

/*
 * Programs a sequencer's delays: the power-on and power-off delay
 * registers, and the power cycle in the control word (written only when
 * it changes).
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_pps.c (pps_init_registers).
 */
void
drv_i915_xd_pps_write_delays(
	struct i915_xd_io *io,
	unsigned pps,
	const struct i915_xd_pps_delays *program)
{
	uint32_t on;
	uint32_t off;
	uint32_t old_control;
	uint32_t new_control;
	uint32_t cycle;

	/* The two delay registers. */
	on = i915_xd_field_put(I915_XD_PP_DELAY_UPPER, program->power_up) |
	     i915_xd_field_put(I915_XD_PP_DELAY_LOWER, program->backlight_on);
	off = i915_xd_field_put(I915_XD_PP_DELAY_LOWER, program->backlight_off) |
	      i915_xd_field_put(I915_XD_PP_DELAY_UPPER, program->power_down);
	drv_i915_xd_io_write(io, I915_XD_PP_ON_DELAYS(pps), on);
	drv_i915_xd_io_write(io, I915_XD_PP_OFF_DELAYS(pps), off);

	/* The power cycle in whole 100 ms, in the control word. */
	cycle = (program->power_cycle + I915_XD_PPS_CYCLE_UNIT - 1U) / I915_XD_PPS_CYCLE_UNIT;
	old_control = drv_i915_xd_io_read(io, I915_XD_PP_CONTROL(pps));
	new_control = old_control & ~I915_XD_PP_CONTROL_CYCLE;
	new_control |= i915_xd_field_put(I915_XD_PP_CONTROL_CYCLE, cycle);
	if (new_control != old_control)
		drv_i915_xd_io_write(io, I915_XD_PP_CONTROL(pps), new_control);
}

/*
 * Holds the panel's VDD on, so its AUX channel answers, and pushes the
 * write out.  Returns nonzero when the panel has its power after that;
 * a panel without power needs its power-up delay before the AUX channel
 * is used, which the caller waits out.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_pps.c (intel_pps_vdd_on_unlocked).
 */
int
drv_i915_xd_pps_vdd_on(
	struct i915_xd_io *io,
	unsigned pps)
{
	uint32_t control;
	int powered;

	/* Sets the VDD hold and pushes it out. */
	control = drv_i915_xd_io_read(io, I915_XD_PP_CONTROL(pps));
	control |= I915_XD_PP_CONTROL_FORCE_VDD;
	drv_i915_xd_io_write(io, I915_XD_PP_CONTROL(pps), control);
	drv_i915_xd_io_posting_read(io, I915_XD_PP_CONTROL(pps));

	/* Tells whether the panel has its power. */
	powered = drv_i915_xd_pps_powered(io, pps);
	if (powered == 0)
		return 0;

	/* Succeeded: the panel has its power and answers at once. */
	return 1;
}

/*
 * Lets the panel's VDD go and pushes the write out.  Returns the control
 * word written, which tells the caller whether the panel still has its
 * power (the power cycle starts now when it has not).
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_pps.c
 * (intel_pps_vdd_off_sync_unlocked).
 */
uint32_t
drv_i915_xd_pps_vdd_off(
	struct i915_xd_io *io,
	unsigned pps)
{
	uint32_t control;

	/* Clears the VDD hold and pushes it out. */
	control = drv_i915_xd_io_read(io, I915_XD_PP_CONTROL(pps));
	control &= ~I915_XD_PP_CONTROL_FORCE_VDD;
	drv_i915_xd_io_write(io, I915_XD_PP_CONTROL(pps), control);
	drv_i915_xd_io_posting_read(io, I915_XD_PP_CONTROL(pps));

	/* Succeeded: the control word as it now is. */
	return control;
}

/*
 * Waits for the sequencer to end the panel's power cycle: the panel off,
 * no sequence running and the cycle's delay over.  The caller has slept
 * the part of the cycle the sequencer does not time (after VDD alone
 * went off).  Returns 0, or ETIMEDOUT when the sequencer did not finish
 * in 5 s (the caller reports it and goes on).
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_pps.c (wait_panel_power_cycle,
 * wait_panel_status).
 */
int
drv_i915_xd_pps_wait_cycle(
	struct i915_xd_io *io,
	unsigned pps)
{
	int error;

	/* Waits for the cycle to be over. */
	error = drv_i915_xd_io_poll(io, I915_XD_PP_STATUS(pps), I915_XD_PPS_CYCLE_MASK, I915_XD_PPS_CYCLE_VALUE, I915_XD_PPS_STEP_US, NULL);
	if (error != 0)
		return error;

	/* Succeeded: the panel may be powered again. */
	return 0;
}

/*
 * Powers the panel on: programs the delays again (the firmware or a
 * suspend may have changed them), starts the power-up sequence and waits
 * for the panel to be on and at rest.  With ungate_dpls nonzero, the
 * DPLS unit's clock gating is turned off around the sequence (a display
 * 13 workaround).  The power cycle must be over already.  Returns 0, or
 * ETIMEDOUT when the panel did not come on in 5 s (the sequence is still
 * finished; the caller reports it).
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_pps.c (intel_pps_on_unlocked,
 * wait_panel_on).
 */
int
drv_i915_xd_pps_power_on(
	struct i915_xd_io *io,
	unsigned pps,
	const struct i915_xd_pps_delays *program,
	int ungate_dpls)
{
	uint32_t control;
	int error;

	/* Programs the delays the sequence runs with. */
	drv_i915_xd_pps_write_delays(io, pps, program);

	/* Ungates the DPLS unit's clock for the sequence. */
	if (ungate_dpls != 0)
		(void)drv_i915_xd_io_rmw(io, I915_XD_SOUTH_DSPCLK_GATE_D, 0U, I915_XD_SOUTH_DSPCLK_DPLS_UNGATE);

	/* Starts the power-up sequence, the reset bit with it, and pushes it out. */
	control = drv_i915_xd_io_read(io, I915_XD_PP_CONTROL(pps));
	control |= I915_XD_PP_CONTROL_POWER_ON | I915_XD_PP_CONTROL_RESET;
	drv_i915_xd_io_write(io, I915_XD_PP_CONTROL(pps), control);
	drv_i915_xd_io_posting_read(io, I915_XD_PP_CONTROL(pps));

	/* Waits for the panel to be on and at rest. */
	error = drv_i915_xd_io_poll(io, I915_XD_PP_STATUS(pps), I915_XD_PPS_ON_MASK, I915_XD_PPS_ON_VALUE, I915_XD_PPS_STEP_US, NULL);

	/* Gates the DPLS unit's clock again, whatever the wait gave. */
	if (ungate_dpls != 0)
		(void)drv_i915_xd_io_rmw(io, I915_XD_SOUTH_DSPCLK_GATE_D, I915_XD_SOUTH_DSPCLK_DPLS_UNGATE, 0U);

	/* Reports a panel that did not come on. */
	if (error != 0)
		return error;

	/* Succeeded: the panel is on. */
	return 0;
}

/*
 * Powers the panel off: the power, the reset bit, the VDD hold and the
 * backlight enable go together (some panels stop working when the power
 * and VDD go apart), and waits for the panel to be off.  Returns 0, or
 * ETIMEDOUT when the panel did not go off in 5 s.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_pps.c (intel_pps_off_unlocked,
 * wait_panel_off).
 */
int
drv_i915_xd_pps_power_off(
	struct i915_xd_io *io,
	unsigned pps)
{
	uint32_t control;
	int error;

	/* Clears the power, the reset, the VDD hold and the backlight, and pushes it out. */
	control = drv_i915_xd_io_read(io, I915_XD_PP_CONTROL(pps));
	control &= ~(I915_XD_PP_CONTROL_POWER_ON | I915_XD_PP_CONTROL_RESET |
		     I915_XD_PP_CONTROL_FORCE_VDD | I915_XD_PP_CONTROL_BACKLIGHT);
	drv_i915_xd_io_write(io, I915_XD_PP_CONTROL(pps), control);
	drv_i915_xd_io_posting_read(io, I915_XD_PP_CONTROL(pps));

	/* Waits for the panel to be off. */
	error = drv_i915_xd_io_poll(io, I915_XD_PP_STATUS(pps), I915_XD_PPS_OFF_MASK, I915_XD_PPS_OFF_VALUE, I915_XD_PPS_STEP_US, NULL);
	if (error != 0)
		return error;

	/* Succeeded: the panel is off; its power cycle starts now. */
	return 0;
}

/*
 * Sets (on nonzero) or clears the sequencer's backlight enable and pushes
 * the write out.  The caller waits out the backlight's delays.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_pps.c (intel_pps_backlight_on,
 * intel_pps_backlight_off).
 */
void
drv_i915_xd_pps_backlight(
	struct i915_xd_io *io,
	unsigned pps,
	int on)
{
	uint32_t control;

	/* Changes the enable alone and pushes it out. */
	control = drv_i915_xd_io_read(io, I915_XD_PP_CONTROL(pps));
	if (on != 0) {
		control |= I915_XD_PP_CONTROL_BACKLIGHT;
	} else {
		control &= ~I915_XD_PP_CONTROL_BACKLIGHT;
	}

	/* Writes the control. */
	drv_i915_xd_io_write(io, I915_XD_PP_CONTROL(pps), control);
	drv_i915_xd_io_posting_read(io, I915_XD_PP_CONTROL(pps));
}

/*
 * Reads a display 9 south display's backlight PWM in its layout: the
 * control, then the period and duty word (the Sunrise Point family's,
 * packed is nonzero) or the period and the duty registers (the Cannon
 * Point family's).  The duty is 0 for the packed layout.
 */
void
drv_i915_xd9_pwm_read(
	struct i915_xd_io *io,
	int packed,
	uint32_t *control,
	uint32_t *period,
	uint32_t *duty)
{
	/* The control: the enable and the polarity, at the same place in both layouts. */
	*control = drv_i915_xd_io_read(io, I915_XD_SBLC_PWM_CTL(0U));

	/* The packed layout's one word. */
	*duty = 0U;
	*period = drv_i915_xd_io_read(io, I915_XD_SBLC_PWM_FREQ(0U));
	if (packed)
		return;

	/* Succeeded: the duty register of the other layout. */
	*duty = drv_i915_xd_io_read(io, I915_XD_SBLC_PWM_DUTY(0U));
}

/*
 * Decodes a display 9 PWM from its registers (as drv_i915_xd9_pwm_read()
 * reads them): whether it runs, its polarity, its period, its duty, and
 * the floor a brightness of 0 % maps to.  Returns 0, or EINVAL for a
 * period of 0 (the firmware never set the PWM up).
 */
int
drv_i915_xd9_pwm_decode(
	uint32_t control,
	uint32_t period,
	uint32_t duty,
	int packed,
	struct i915_xd9_pwm *pwm)
{
	/* The enable and the polarity. */
	pwm->running = 0;
	if ((control & I915_XD_SBLC_PWM_ENABLE) != 0U)
		pwm->running = 1;
	pwm->active_low = 0;
	if ((control & I915_XD_SBLC_PWM_POLARITY) != 0U)
		pwm->active_low = 1;

	/* The period and the duty, from the halves of the packed word or from their registers. */
	pwm->period = period;
	pwm->duty = duty;
	if (packed) {
		pwm->period = period >> I915_XD9_PWM_PERIOD_SHIFT;
		pwm->duty = period & I915_XD9_PWM_DUTY_MASK;
	}

	/* A PWM without a period. */
	pwm->floor = 0U;
	if (pwm->period == 0U)
		return EINVAL;

	/* Succeeded: the floor is the period's 4/255, rounded up. */
	pwm->floor = (uint32_t)(((uint64_t)pwm->period * I915_XD9_PWM_FLOOR_255 + I915_XD9_PWM_SCALE_255 - 1U) / I915_XD9_PWM_SCALE_255);
	return 0;
}

/*
 * Reports the duty of a brightness (a percentage, above 100 taken as
 * 100): from the floor at 0 % to the period at 100 %, rounded to the
 * closest.
 */
uint32_t
drv_i915_xd9_pwm_duty(
	const struct i915_xd9_pwm *pwm,
	uint32_t percent)
{
	uint32_t duty;

	/* The brightness moved onto the duties. */
	duty = drv_i915_xd_backlight_scale(percent, 0U, I915_XD9_PWM_PERCENT_MAX, pwm->floor, pwm->period);

	/* Succeeded: the duty. */
	return duty;
}

/*
 * Reports the brightness of a duty, the inverse of drv_i915_xd9_pwm_duty():
 * 0 % at or below the floor, 100 % at or above the period.
 */
uint32_t
drv_i915_xd9_pwm_percent(
	const struct i915_xd9_pwm *pwm,
	uint32_t duty)
{
	uint32_t percent;

	/* At or below the floor is dark's brightness; at or above the period is full. */
	if (duty <= pwm->floor)
		return 0U;
	if (duty >= pwm->period)
		return I915_XD9_PWM_PERCENT_MAX;

	/* The duty moved onto the brightnesses. */
	percent = drv_i915_xd_backlight_scale(duty, pwm->floor, pwm->period, 0U, I915_XD9_PWM_PERCENT_MAX);

	/* Succeeded: the brightness. */
	return percent;
}

/*
 * Sets a display 9 PWM's duty: in its own register, or in the lower half
 * of the packed word with the period read before kept.
 */
void
drv_i915_xd9_pwm_set(
	struct i915_xd_io *io,
	int packed,
	uint32_t period_word,
	uint32_t duty)
{
	/* The Cannon Point family's duty register. */
	if (!packed) {
		drv_i915_xd_io_write(io, I915_XD_SBLC_PWM_DUTY(0U), duty);
		return;
	}

	/* Succeeded: the packed word with its period kept. */
	drv_i915_xd_io_write(io, I915_XD_SBLC_PWM_FREQ(0U), (period_word & ~I915_XD9_PWM_DUTY_MASK) | duty);
}

/*
 * Stops a running display 9 PWM for a sleep and keeps its control word in
 * *control.  Returns 1 when it stopped one, 0 when the PWM did not run.
 */
int
drv_i915_xd9_pwm_pause(
	struct i915_xd_io *io,
	uint32_t *control)
{
	/* A PWM that does not run is left alone. */
	*control = drv_i915_xd_io_read(io, I915_XD_SBLC_PWM_CTL(0U));
	if ((*control & I915_XD_SBLC_PWM_ENABLE) == 0U)
		return 0;

	/* Succeeded: the enable cleared and pushed out. */
	drv_i915_xd_io_write(io, I915_XD_SBLC_PWM_CTL(0U), *control & ~I915_XD_SBLC_PWM_ENABLE);
	drv_i915_xd_io_posting_read(io, I915_XD_SBLC_PWM_CTL(0U));
	return 1;
}

/*
 * Starts a display 9 PWM again with the control word a pause kept.
 */
void
drv_i915_xd9_pwm_resume(
	struct i915_xd_io *io,
	uint32_t control)
{
	/* Succeeded: the control as it was, pushed out. */
	drv_i915_xd_io_write(io, I915_XD_SBLC_PWM_CTL(0U), control);
	drv_i915_xd_io_posting_read(io, I915_XD_SBLC_PWM_CTL(0U));
}

/*
 * Tells whether the firmware left display 9's eDP panel lit: power well 1,
 * where the eDP transcoder lives, is up and the transcoder's DDI function
 * drives it.  Returns 1 or 0.
 */
int
drv_i915_xd9_panel_lit(
	struct i915_xd_io *io)
{
	uint32_t power;
	uint32_t function;

	/* Power well 1 must be up for the transcoder to run. */
	power = drv_i915_xd_io_read(io, I915_XD9_PWR_WELL_CTL2);
	if ((power & I915_XD9_PW1_STATE) == 0U)
		return 0;

	/* The transcoder's DDI function drives the panel while it is on. */
	function = drv_i915_xd_io_read(io, I915_XD9_TRANS_DDI_FUNC_CTL_EDP);
	if ((function & I915_XD9_TRANS_DDI_FUNC_ENABLE) == 0U)
		return 0;

	/* Succeeded: the panel is lit. */
	return 1;
}

/* Picks one delay: the larger given, or the specification's when none is. */
static uint32_t
i915_xd_pps_pick(
	uint32_t firmware,
	uint32_t vbt,
	uint32_t spec)
{
	uint32_t larger;

	/* The larger of the two given. */
	larger = firmware;
	if (vbt > larger)
		larger = vbt;

	/* Neither gave one: the specification's. */
	if (larger == 0U)
		return spec;

	/* Succeeded: the larger given. */
	return larger;
}

/* Turns a delay in the sequencer's units into whole milliseconds, rounded up. */
static unsigned
i915_xd_pps_ms(
	uint32_t units)
{
	/* Ten units to the millisecond. */
	return (unsigned)((units + I915_XD_PPS_UNITS_PER_MS - 1U) / I915_XD_PPS_UNITS_PER_MS);
}
