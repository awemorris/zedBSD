/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The built-in panel of the display 12 and 13 code (xd-panel.c): the
 * panel power sequencer and the backlight PWM of the south display.
 *
 * The panel power sequencer switches the panel's supply on and off with
 * the delays the panel needs between the steps, and can hold the panel's
 * logic supply (VDD) on by itself, so the panel answers on its AUX
 * channel while it shows nothing.  A display may have two sequencers;
 * the panel's is the one its backlight controller is (the VBT names
 * it).  The caller holds the sequencer's lock around every call and
 * keeps the times: when the panel last lost power, when it came on.
 *
 * The PWM runs from the raw clock of the south display.  Its period, in
 * raw clock ticks, sets the frequency the panel's backlight driver sees;
 * its duty, in the same ticks, sets the brightness.  A display may have
 * two controllers; the panel's is named by the VBT.  The caller holds
 * the backlight lock around every call.
 */

#ifndef DRIVERS_GPU_I915_DISPLAY_XD_PANEL_H
#define DRIVERS_GPU_I915_DISPLAY_XD_PANEL_H

#include <stdint.h>

#include "xd-io.h"

/*
 * The largest VBT minimum brightness taken as it is, of 255: a larger
 * one (some machines say 255) is cut to this quarter of the range, so the
 * lowest brightness is never the highest.
 */
#define I915_XD_BACKLIGHT_MIN_LIMIT	64U

/* The longest the sequencer takes to finish a step, in microseconds. */
#define I915_XD_PPS_STEP_US		5000000U

/* What drv_i915_xd_pps_find() looks for in a sequencer. */
#define I915_XD_PPS_FIND_POWERED	0U
#define I915_XD_PPS_FIND_VDD		1U
#define I915_XD_PPS_FIND_ANY		2U

/*
 * The delays of a panel power sequence, in the sequencer's units of
 * 100 us: power on until the panel is ready (T1+T3), the backlight's
 * start and stop (T8, T9), power off until the panel is dark (T10), and
 * the least time the panel stays off before it may come on again
 * (T11+T12; the sequencer keeps it in whole 100 ms).  A set with every
 * delay 0 holds nothing.
 */
struct i915_xd_pps_delays {
	uint32_t power_up;
	uint32_t backlight_on;
	uint32_t backlight_off;
	uint32_t power_down;
	uint32_t power_cycle;
};

/*
 * The same delays in whole milliseconds, rounded up: the waits the
 * caller times itself (the sequencer is given 100 us for the backlight's
 * delays, which the caller waits out instead).
 */
struct i915_xd_pps_waits {
	unsigned power_up_ms;
	unsigned backlight_on_ms;
	unsigned backlight_off_ms;
	unsigned power_down_ms;
	unsigned power_cycle_ms;
};

/*
 * The backlight PWM of one panel as it was found: the controller, the
 * period (0 when nobody programmed one), whether the PWM is inverted
 * (active low) and whether it runs.
 */
struct i915_xd_backlight {
	unsigned controller;
	uint32_t period;
	int active_low;
	int running;
};

/*
 * A display 9 backlight PWM as the brightness uses it: whether it runs,
 * whether its output is inverted, its period and duty, and the duty a
 * brightness of 0 % maps to.
 */
struct i915_xd9_pwm {
	int running;
	int active_low;
	uint32_t period;
	uint32_t duty;
	uint32_t floor;
};

void drv_i915_xd9_pwm_read(struct i915_xd_io *io, int packed, uint32_t *control, uint32_t *period, uint32_t *duty);
int drv_i915_xd9_pwm_decode(uint32_t control, uint32_t period, uint32_t duty, int packed, struct i915_xd9_pwm *pwm);
uint32_t drv_i915_xd9_pwm_duty(const struct i915_xd9_pwm *pwm, uint32_t percent);
uint32_t drv_i915_xd9_pwm_percent(const struct i915_xd9_pwm *pwm, uint32_t duty);
void drv_i915_xd9_pwm_set(struct i915_xd_io *io, int packed, uint32_t period_word, uint32_t duty);
int drv_i915_xd9_pwm_pause(struct i915_xd_io *io, uint32_t *control);
void drv_i915_xd9_pwm_resume(struct i915_xd_io *io, uint32_t control);
int drv_i915_xd9_panel_lit(struct i915_xd_io *io);
int drv_i915_xd_backlight_controller_usable(struct i915_xd_io *io, unsigned controller, unsigned controllers, int muxed);
void drv_i915_xd_backlight_read(struct i915_xd_backlight *backlight, struct i915_xd_io *io, unsigned controller);
uint32_t drv_i915_xd_backlight_period(unsigned rawclk_khz, unsigned hz);
uint32_t drv_i915_xd_backlight_min_duty(uint32_t period, unsigned vbt_min);
uint32_t drv_i915_xd_backlight_scale(uint32_t value, uint32_t from_min, uint32_t from_max, uint32_t to_min, uint32_t to_max);
uint32_t drv_i915_xd_backlight_duty(const struct i915_xd_backlight *backlight, struct i915_xd_io *io);
void drv_i915_xd_backlight_set_duty(const struct i915_xd_backlight *backlight, struct i915_xd_io *io, uint32_t duty);
int drv_i915_xd_backlight_enable(struct i915_xd_backlight *backlight, struct i915_xd_io *io, uint32_t duty);
void drv_i915_xd_backlight_disable(struct i915_xd_backlight *backlight, struct i915_xd_io *io, uint32_t duty);
int drv_i915_xd_pps_usable(struct i915_xd_io *io, unsigned pps, unsigned sequencers, int muxed);
int drv_i915_xd_pps_find(struct i915_xd_io *io, unsigned sequencers, unsigned what);
int drv_i915_xd_pps_powered(struct i915_xd_io *io, unsigned pps);
int drv_i915_xd_pps_vdd_forced(struct i915_xd_io *io, unsigned pps);
int drv_i915_xd_pps_delays_set(const struct i915_xd_pps_delays *delays);
void drv_i915_xd_pps_read_delays(struct i915_xd_io *io, unsigned pps, struct i915_xd_pps_delays *delays);
void drv_i915_xd_pps_spec(struct i915_xd_pps_delays *delays);
void drv_i915_xd_pps_from_vbt(struct i915_xd_pps_delays *delays);
void drv_i915_xd_pps_choose(const struct i915_xd_pps_delays *firmware, const struct i915_xd_pps_delays *vbt, struct i915_xd_pps_delays *program, struct i915_xd_pps_waits *waits);
void drv_i915_xd_pps_write_delays(struct i915_xd_io *io, unsigned pps, const struct i915_xd_pps_delays *program);
int drv_i915_xd_pps_vdd_on(struct i915_xd_io *io, unsigned pps);
uint32_t drv_i915_xd_pps_vdd_off(struct i915_xd_io *io, unsigned pps);
int drv_i915_xd_pps_wait_cycle(struct i915_xd_io *io, unsigned pps);
int drv_i915_xd_pps_power_on(struct i915_xd_io *io, unsigned pps, const struct i915_xd_pps_delays *program, int ungate_dpls);
int drv_i915_xd_pps_power_off(struct i915_xd_io *io, unsigned pps);
void drv_i915_xd_pps_backlight(struct i915_xd_io *io, unsigned pps, int on);

#endif /* DRIVERS_GPU_I915_DISPLAY_XD_PANEL_H */
