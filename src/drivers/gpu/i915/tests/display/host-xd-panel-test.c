/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the xd panel (display/xd-panel.c) on the register
 * model (xd-fake-regs.c): the backlight PWM's period of a frequency on
 * the two raw clocks, the lowest duty of the VBT's minimum, the scaling
 * between ranges, the controllers a panel may use, the reading of the
 * firmware's PWM, and the exact writes of a start and a stop; the panel
 * power sequencer's choice of delays, its programming, the sequencers a
 * panel may use, VDD alone, power on and off against the sequencer's
 * status, and the waits that run out.
 */

#include "host-test.h"
#include "xd-fake-regs.h"

#include "../../display/xd-io.h"
#include "../../display/xd-panel.h"
#include "../../intel/display-regs-xd-south.h"

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * The period of the 200 Hz PWM of the machines the display was written
 * on, in ticks of the 19.2 MHz raw clock: 96000, what the firmware
 * leaves in the period register.
 */
#define I915_XD_PANEL_TEST_PERIOD	0x17700U

/*
 * The window the checks share, reaching a fresh model each time
 * (i915_xd_panel_test_setup()).
 */
static struct i915_xd_io i915_xd_panel_test_io;

static void i915_xd_panel_test_setup(void);
static unsigned i915_xd_panel_test_writes(uint32_t *offsets, uint32_t *values, unsigned max);
static void i915_xd_panel_test_numbers(void);
static void i915_xd_panel_test_controllers(void);
static void i915_xd_panel_test_pwm(void);
static void i915_xd_panel_test_pps_delays(void);
static void i915_xd_panel_test_pps_sequencers(void);
static void i915_xd_panel_test_pps_power(void);
static void i915_xd_panel_test_pps_status(uint32_t offset, uint32_t value, void *context);
static unsigned i915_xd_panel_test_timeouts(void);

/*
 * Runs every check of the xd panel and reports.
 */
int
main(void)
{
	int failures;

	/* Runs each group of checks. */
	i915_xd_panel_test_numbers();
	i915_xd_panel_test_controllers();
	i915_xd_panel_test_pwm();
	i915_xd_panel_test_pps_delays();
	i915_xd_panel_test_pps_sequencers();
	i915_xd_panel_test_pps_power();

	/* Reports the result. */
	failures = i915_host_report("host_xd_panel_test");
	if (failures != 0)
		return 1;

	/* Succeeded: every check passed. */
	return 0;
}

/* Starts a fresh model and the window on it. */
static void
i915_xd_panel_test_setup(void)
{
	struct i915_xd_io_hooks hooks;

	/* A fresh model and a window that reaches it. */
	i915_xd_fake_reset();
	i915_xd_fake_hooks(&hooks);
	drv_i915_xd_io_init(&i915_xd_panel_test_io, NULL, NULL);
	drv_i915_xd_io_set_hooks(&i915_xd_panel_test_io, &hooks);
}

/* Copies the writes of the window, oldest first; returns how many. */
static unsigned
i915_xd_panel_test_writes(
	uint32_t *offsets,
	uint32_t *values,
	unsigned max)
{
	struct i915_xd_io_record records[I915_XD_IO_RECORDS];
	unsigned count;
	unsigned index;
	unsigned kept;

	/* Takes the record and keeps the writes. */
	count = drv_i915_xd_io_records(&i915_xd_panel_test_io, records, I915_XD_IO_RECORDS);
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

/* The period of a frequency, the lowest duty and the scaling. */
static void
i915_xd_panel_test_numbers(void)
{
	uint32_t value;

	/* 19.2 MHz / 200 Hz is the firmware's period; 24 MHz gives 120000; 0 Hz none. */
	value = drv_i915_xd_backlight_period(19200U, 200U);
	i915_host_check(value == I915_XD_PANEL_TEST_PERIOD, "panel: 19.2 MHz / 200 Hz = 0x17700");
	value = drv_i915_xd_backlight_period(24000U, 200U);
	i915_host_check(value == 120000U, "panel: 24 MHz / 200 Hz = 120000");
	value = drv_i915_xd_backlight_period(19200U, 0U);
	i915_host_check(value == 0U, "panel: no frequency has no period");

	/* A period that does not divide evenly is rounded to the closest tick. */
	value = drv_i915_xd_backlight_period(19200U, 7U);
	i915_host_check(value == 2742857U, "panel: the period is rounded to the closest");

	/* The VBT's minimum 6 of 255 of 96000, rounded; 255 is cut to 64. */
	value = drv_i915_xd_backlight_min_duty(I915_XD_PANEL_TEST_PERIOD, 6U);
	i915_host_check(value == 2259U, "panel: the lowest duty of 6/255");
	value = drv_i915_xd_backlight_min_duty(I915_XD_PANEL_TEST_PERIOD, 255U);
	i915_host_check(value == 24094U, "panel: a minimum of 255/255 is cut to 64/255");

	/* Scaling: the ends, a value held inside its range, and the ranges that cannot scale. */
	value = drv_i915_xd_backlight_scale(100U, 0U, 100U, 2259U, 96000U);
	i915_host_check(value == 96000U, "panel: the top of the range scales to the top");
	value = drv_i915_xd_backlight_scale(50U, 0U, 100U, 0U, 96000U);
	i915_host_check(value == 48000U, "panel: the middle scales to the middle");
	value = drv_i915_xd_backlight_scale(500U, 0U, 100U, 0U, 96000U);
	i915_host_check(value == 96000U, "panel: a value above its range is held at the top");
	value = drv_i915_xd_backlight_scale(5U, 10U, 10U, 7U, 96000U);
	i915_host_check(value == 7U, "panel: an empty source range answers the target's bottom");
	value = drv_i915_xd_backlight_scale(5U, 0U, 10U, 9U, 8U);
	i915_host_check(value == 9U, "panel: a target range upside down answers its bottom");
}

/* The controllers a panel may use. */
static void
i915_xd_panel_test_controllers(void)
{
	int usable;

	/* Controller 0 of two is usable; controller 2 of two is not. */
	i915_xd_panel_test_setup();
	usable = drv_i915_xd_backlight_controller_usable(&i915_xd_panel_test_io, 0U, 2U, 1);
	i915_host_check(usable == 1 && i915_xd_fake_reads() == 0U, "panel: controller 0 is usable without a read");
	usable = drv_i915_xd_backlight_controller_usable(&i915_xd_panel_test_io, 2U, 2U, 0);
	i915_host_check(usable == 0, "panel: controller 2 of two is not");

	/* Controller 1 of a muxed display needs its pins in SOUTH_CHICKEN1. */
	usable = drv_i915_xd_backlight_controller_usable(&i915_xd_panel_test_io, 1U, 2U, 1);
	i915_host_check(usable == 0, "panel: controller 1 of a muxed display without its pins");
	i915_xd_fake_set(I915_XD_SOUTH_CHICKEN1, I915_XD_SOUTH_CHICKEN1_SECOND_PPS_IO);
	usable = drv_i915_xd_backlight_controller_usable(&i915_xd_panel_test_io, 1U, 2U, 1);
	i915_host_check(usable == 1, "panel: controller 1 of a muxed display with its pins");

	/* Controller 1 of a display that does not mux it is usable as it is. */
	i915_xd_panel_test_setup();
	usable = drv_i915_xd_backlight_controller_usable(&i915_xd_panel_test_io, 1U, 2U, 0);
	i915_host_check(usable == 1 && i915_xd_fake_reads() == 0U, "panel: controller 1 without muxing");
}

/* Reading the firmware's PWM, starting and stopping it. */
static void
i915_xd_panel_test_pwm(void)
{
	struct i915_xd_backlight pwm;
	uint32_t offsets[8];
	uint32_t values[8];
	uint32_t duty;
	unsigned count;
	int was_running;

	/* The firmware's PWM: running, inverted, with its period. */
	i915_xd_panel_test_setup();
	i915_xd_fake_set(I915_XD_SBLC_PWM_CTL(0U), I915_XD_SBLC_PWM_ENABLE | I915_XD_SBLC_PWM_POLARITY);
	i915_xd_fake_set(I915_XD_SBLC_PWM_FREQ(0U), I915_XD_PANEL_TEST_PERIOD);
	i915_xd_fake_set(I915_XD_SBLC_PWM_DUTY(0U), 0x1234U);
	drv_i915_xd_backlight_read(&pwm, &i915_xd_panel_test_io, 0U);
	duty = drv_i915_xd_backlight_duty(&pwm, &i915_xd_panel_test_io);
	i915_host_check(pwm.controller == 0U && pwm.period == I915_XD_PANEL_TEST_PERIOD &&
			pwm.active_low == 1 && pwm.running == 1 && duty == 0x1234U,
			"panel: the firmware's PWM is read: period, polarity, enable, duty");

	/* Starting a running PWM: stopped first, then the period, the duty, the polarity and the enable. */
	was_running = drv_i915_xd_backlight_enable(&pwm, &i915_xd_panel_test_io, 0x17700U);
	count = i915_xd_panel_test_writes(offsets, values, 8U);
	i915_host_check(was_running == 1 &&
			count == 5U &&
			offsets[0] == 0xc8250U && values[0] == I915_XD_SBLC_PWM_POLARITY &&
			offsets[1] == 0xc8254U && values[1] == 0x17700U &&
			offsets[2] == 0xc8258U && values[2] == 0x17700U &&
			offsets[3] == 0xc8250U && values[3] == I915_XD_SBLC_PWM_POLARITY &&
			offsets[4] == 0xc8250U && values[4] == (I915_XD_SBLC_PWM_POLARITY | I915_XD_SBLC_PWM_ENABLE),
			"panel: a running PWM is stopped, then started with the period and the duty");

	/* Starting a stopped PWM on controller 1: no stop, and the registers 0x100 up. */
	i915_xd_panel_test_setup();
	pwm.controller = 1U;
	pwm.period = I915_XD_PANEL_TEST_PERIOD;
	pwm.active_low = 0;
	pwm.running = 0;
	was_running = drv_i915_xd_backlight_enable(&pwm, &i915_xd_panel_test_io, 0x100U);
	count = i915_xd_panel_test_writes(offsets, values, 8U);
	i915_host_check(was_running == 0 &&
			count == 4U &&
			offsets[0] == 0xc8354U &&
			offsets[1] == 0xc8358U && values[1] == 0x100U &&
			offsets[2] == 0xc8350U && values[2] == 0U &&
			offsets[3] == 0xc8350U && values[3] == I915_XD_SBLC_PWM_ENABLE &&
			pwm.running == 1,
			"panel: a stopped PWM on controller 1 is started without a stop");

	/* Stopping: the duty, then the enable cleared with the polarity kept. */
	i915_xd_panel_test_setup();
	pwm.controller = 0U;
	i915_xd_fake_set(I915_XD_SBLC_PWM_CTL(0U), I915_XD_SBLC_PWM_ENABLE | I915_XD_SBLC_PWM_POLARITY);
	drv_i915_xd_backlight_disable(&pwm, &i915_xd_panel_test_io, 0U);
	count = i915_xd_panel_test_writes(offsets, values, 8U);
	i915_host_check(count == 2U &&
			offsets[0] == 0xc8258U && values[0] == 0U &&
			offsets[1] == 0xc8250U && values[1] == I915_XD_SBLC_PWM_POLARITY &&
			pwm.running == 0,
			"panel: a stop writes the duty, then clears only the enable");

	/* Setting the duty writes it alone. */
	i915_xd_panel_test_setup();
	drv_i915_xd_backlight_set_duty(&pwm, &i915_xd_panel_test_io, 48000U);
	count = i915_xd_panel_test_writes(offsets, values, 8U);
	i915_host_check(count == 1U && offsets[0] == 0xc8258U && values[0] == 48000U, "panel: the duty alone");
}

/* The choice of delays, their programming and reading back. */
static void
i915_xd_panel_test_pps_delays(void)
{
	struct i915_xd_pps_delays firmware;
	struct i915_xd_pps_delays vbt;
	struct i915_xd_pps_delays program;
	struct i915_xd_pps_delays read;
	struct i915_xd_pps_waits waits;
	uint32_t offsets[8];
	uint32_t values[8];
	unsigned count;
	int set;

	/* Neither the firmware nor the VBT: the specification's limits. */
	memset(&firmware, 0, sizeof(firmware));
	memset(&vbt, 0, sizeof(vbt));
	set = drv_i915_xd_pps_delays_set(&vbt);
	drv_i915_xd_pps_from_vbt(&vbt);
	i915_host_check(set == 0 && vbt.power_cycle == 0U, "pps: a VBT without delays stays empty");
	drv_i915_xd_pps_choose(&firmware, &vbt, &program, &waits);
	i915_host_check(waits.power_up_ms == 210U && waits.backlight_on_ms == 50U && waits.backlight_off_ms == 50U &&
			waits.power_down_ms == 500U && waits.power_cycle_ms == 610U,
			"pps: the specification's waits: 210, 50, 50, 500 and 610 ms");
	i915_host_check(program.power_up == 2100U && program.backlight_on == 1U && program.backlight_off == 1U &&
			program.power_down == 5000U && program.power_cycle == 7000U,
			"pps: the sequencer gets the least backlight delays and the cycle in whole 100 ms");

	/* The larger of the firmware's and the VBT's; the VBT's cycle counts from 100 ms. */
	firmware.power_up = 2000U;
	firmware.backlight_on = 10U;
	firmware.backlight_off = 2000U;
	firmware.power_down = 5000U;
	firmware.power_cycle = 5000U;
	vbt.power_up = 2100U;
	vbt.backlight_on = 500U;
	vbt.backlight_off = 1U;
	vbt.power_down = 100U;
	vbt.power_cycle = 4995U;
	drv_i915_xd_pps_from_vbt(&vbt);
	i915_host_check(vbt.power_cycle == 5995U, "pps: the VBT's power cycle gains the sequencer's 100 ms");
	drv_i915_xd_pps_choose(&firmware, &vbt, &program, &waits);
	i915_host_check(waits.power_up_ms == 210U && waits.backlight_on_ms == 50U && waits.backlight_off_ms == 200U &&
			waits.power_down_ms == 500U && waits.power_cycle_ms == 600U &&
			program.power_cycle == 6000U,
			"pps: each delay is the larger given, rounded up to the millisecond");

	/* Programming: the two delay registers, then the cycle field of the control word. */
	i915_xd_panel_test_setup();
	i915_xd_fake_set(I915_XD_PP_CONTROL(0U), I915_XD_PP_CONTROL_FORCE_VDD | 0x50U);
	drv_i915_xd_pps_write_delays(&i915_xd_panel_test_io, 0U, &program);
	count = i915_xd_panel_test_writes(offsets, values, 8U);
	i915_host_check(count == 3U &&
			offsets[0] == 0xc7208U && values[0] == ((2100U << 16) | 1U) &&
			offsets[1] == 0xc720cU && values[1] == ((5000U << 16) | 1U) &&
			offsets[2] == 0xc7204U && values[2] == (I915_XD_PP_CONTROL_FORCE_VDD | 0x60U),
			"pps: the delays and the 6 x 100 ms cycle are programmed, VDD kept");

	/* A cycle that is already right leaves the control word alone; sequencer 1 is 0x100 up. */
	i915_xd_panel_test_setup();
	i915_xd_fake_set(I915_XD_PP_CONTROL(1U), 0x60U);
	drv_i915_xd_pps_write_delays(&i915_xd_panel_test_io, 1U, &program);
	count = i915_xd_panel_test_writes(offsets, values, 8U);
	i915_host_check(count == 2U && offsets[0] == 0xc7308U && offsets[1] == 0xc730cU,
			"pps: an unchanged cycle is not written again");

	/* Reading back what was programmed. */
	i915_xd_panel_test_setup();
	i915_xd_fake_set(I915_XD_PP_ON_DELAYS(0U), (2100U << 16) | 1U);
	i915_xd_fake_set(I915_XD_PP_OFF_DELAYS(0U), (5000U << 16) | 1U);
	i915_xd_fake_set(I915_XD_PP_CONTROL(0U), 0x60U | I915_XD_PP_CONTROL_POWER_ON);
	drv_i915_xd_pps_read_delays(&i915_xd_panel_test_io, 0U, &read);
	i915_host_check(read.power_up == 2100U && read.backlight_on == 1U && read.backlight_off == 1U &&
			read.power_down == 5000U && read.power_cycle == 6000U,
			"pps: the delays read back as programmed");
}

/* The sequencers a panel may use, and the firmware's choice. */
static void
i915_xd_panel_test_pps_sequencers(void)
{
	int usable;
	int found;

	/* Sequencer 1 of a muxed display needs its pins; 2 of two does not exist. */
	i915_xd_panel_test_setup();
	usable = drv_i915_xd_pps_usable(&i915_xd_panel_test_io, 1U, 2U, 1);
	i915_host_check(usable == 0, "pps: sequencer 1 of a muxed display without its pins");
	i915_xd_fake_set(I915_XD_SOUTH_CHICKEN1, I915_XD_SOUTH_CHICKEN1_SECOND_PPS_IO);
	usable = drv_i915_xd_pps_usable(&i915_xd_panel_test_io, 1U, 2U, 1);
	i915_host_check(usable == 1, "pps: sequencer 1 of a muxed display with its pins");
	usable = drv_i915_xd_pps_usable(&i915_xd_panel_test_io, 2U, 2U, 0);
	i915_host_check(usable == 0, "pps: sequencer 2 of two is not");

	/* The firmware's choice: the powered one, else the one with VDD, else the first. */
	i915_xd_panel_test_setup();
	found = drv_i915_xd_pps_find(&i915_xd_panel_test_io, 2U, I915_XD_PPS_FIND_POWERED);
	i915_host_check(found == -1, "pps: no sequencer has the panel powered");
	i915_xd_fake_set(I915_XD_PP_CONTROL(1U), I915_XD_PP_CONTROL_FORCE_VDD);
	found = drv_i915_xd_pps_find(&i915_xd_panel_test_io, 2U, I915_XD_PPS_FIND_VDD);
	i915_host_check(found == 1, "pps: sequencer 1 holds VDD");
	i915_xd_fake_set(I915_XD_PP_STATUS(1U), I915_XD_PP_STATUS_ON);
	found = drv_i915_xd_pps_find(&i915_xd_panel_test_io, 2U, I915_XD_PPS_FIND_POWERED);
	i915_host_check(found == 1, "pps: sequencer 1 has the panel powered");
	found = drv_i915_xd_pps_find(&i915_xd_panel_test_io, 2U, I915_XD_PPS_FIND_ANY);
	i915_host_check(found == 0, "pps: any sequencer is the first");
}

/* VDD alone, power on and off, the backlight enable and the waits. */
static void
i915_xd_panel_test_pps_power(void)
{
	struct i915_xd_pps_delays program;
	uint32_t offsets[12];
	uint32_t values[12];
	uint32_t control;
	unsigned power_up_us;
	unsigned count;
	int powered;
	int error;

	/* The model's panel comes on 30 ms after it is asked to. */
	power_up_us = 30000U;

	/* VDD alone on an unpowered panel: the hold set and pushed out; the caller waits the power-up delay. */
	i915_xd_panel_test_setup();
	i915_xd_fake_set(I915_XD_PP_CONTROL(0U), 0x60U);
	powered = drv_i915_xd_pps_vdd_on(&i915_xd_panel_test_io, 0U);
	count = i915_xd_panel_test_writes(offsets, values, 12U);
	i915_host_check(powered == 0 && count == 1U && offsets[0] == 0xc7204U &&
			values[0] == (0x60U | I915_XD_PP_CONTROL_FORCE_VDD),
			"pps: VDD alone is set on an unpowered panel");

	/* VDD off: the hold cleared, the word handed back. */
	control = drv_i915_xd_pps_vdd_off(&i915_xd_panel_test_io, 0U);
	i915_host_check(control == 0x60U && i915_xd_fake_get(I915_XD_PP_CONTROL(0U)) == 0x60U, "pps: VDD alone goes off");

	/*
	 * Power on: the delays, the DPLS clock ungated, the power and reset
	 * bits with VDD kept, the panel on and at rest after 30 ms, the DPLS
	 * clock gated again.
	 */
	i915_xd_panel_test_setup();
	i915_xd_fake_rule_call(I915_XD_PP_CONTROL(0U), i915_xd_panel_test_pps_status, &power_up_us);
	i915_xd_fake_set(I915_XD_PP_CONTROL(0U), 0x60U | I915_XD_PP_CONTROL_FORCE_VDD);
	i915_xd_fake_set(I915_XD_SOUTH_DSPCLK_GATE_D, I915_XD_SOUTH_DSPCLK_DPMG_UNGATE);
	program.power_up = 2100U;
	program.backlight_on = 1U;
	program.backlight_off = 1U;
	program.power_down = 5000U;
	program.power_cycle = 6000U;
	error = drv_i915_xd_pps_power_on(&i915_xd_panel_test_io, 0U, &program, 1);
	count = i915_xd_panel_test_writes(offsets, values, 12U);
	i915_host_check(error == 0 && count == 5U &&
			offsets[0] == 0xc7208U && offsets[1] == 0xc720cU &&
			offsets[2] == 0xc2020U && values[2] == (I915_XD_SOUTH_DSPCLK_DPMG_UNGATE | I915_XD_SOUTH_DSPCLK_DPLS_UNGATE) &&
			offsets[3] == 0xc7204U &&
			values[3] == (0x60U | I915_XD_PP_CONTROL_FORCE_VDD | I915_XD_PP_CONTROL_RESET | I915_XD_PP_CONTROL_POWER_ON) &&
			offsets[4] == 0xc2020U && values[4] == I915_XD_SOUTH_DSPCLK_DPMG_UNGATE,
			"pps: power on writes the delays, ungates DPLS, powers the panel and gates DPLS again");
	i915_host_check(i915_xd_fake_now() >= 30000U && i915_xd_fake_now() < 31000U && i915_xd_panel_test_timeouts() == 0U,
			"pps: power on waits for the panel to be on and at rest");

	/* The backlight enable alone, set and cleared. */
	drv_i915_xd_pps_backlight(&i915_xd_panel_test_io, 0U, 1);
	control = i915_xd_fake_get(I915_XD_PP_CONTROL(0U));
	i915_host_check((control & I915_XD_PP_CONTROL_BACKLIGHT) != 0U && (control & I915_XD_PP_CONTROL_POWER_ON) != 0U,
			"pps: the backlight enable is set beside the power");
	drv_i915_xd_pps_backlight(&i915_xd_panel_test_io, 0U, 0);
	control = i915_xd_fake_get(I915_XD_PP_CONTROL(0U));
	i915_host_check((control & I915_XD_PP_CONTROL_BACKLIGHT) == 0U && (control & I915_XD_PP_CONTROL_POWER_ON) != 0U,
			"pps: the backlight enable is cleared alone");

	/* Power off: the power, reset, VDD and backlight go together; the panel is off. */
	drv_i915_xd_pps_backlight(&i915_xd_panel_test_io, 0U, 1);
	error = drv_i915_xd_pps_power_off(&i915_xd_panel_test_io, 0U);
	control = i915_xd_fake_get(I915_XD_PP_CONTROL(0U));
	i915_host_check(error == 0 && control == 0x60U, "pps: power off clears the power, reset, VDD and backlight together");

	/* The power cycle: over once the sequencer's cycle delay is. */
	error = drv_i915_xd_pps_wait_cycle(&i915_xd_panel_test_io, 0U);
	i915_host_check(error == 0 && i915_xd_panel_test_timeouts() == 0U, "pps: the power cycle ends");

	/* A panel that never comes on: the wait runs out after 5 s and DPLS is gated again. */
	i915_xd_panel_test_setup();
	error = drv_i915_xd_pps_power_on(&i915_xd_panel_test_io, 0U, &program, 1);
	i915_host_check(error == ETIMEDOUT && i915_xd_fake_now() >= 5000000U && i915_xd_panel_test_timeouts() == 1U &&
			(i915_xd_fake_get(I915_XD_SOUTH_DSPCLK_GATE_D) & I915_XD_SOUTH_DSPCLK_DPLS_UNGATE) == 0U,
			"pps: a panel that does not come on runs the wait out and gates DPLS again");

	/* Without the workaround, the clock gating word is not touched. */
	i915_xd_panel_test_setup();
	i915_xd_fake_rule_call(I915_XD_PP_CONTROL(0U), i915_xd_panel_test_pps_status, &power_up_us);
	error = drv_i915_xd_pps_power_on(&i915_xd_panel_test_io, 0U, &program, 0);
	count = i915_xd_panel_test_writes(offsets, values, 12U);
	i915_host_check(error == 0 && count == 4U && offsets[3] == 0xc7204U, "pps: power on without the DPLS workaround");

	/* A power cycle still timed by the sequencer: the wait goes on until it is over. */
	i915_xd_panel_test_setup();
	i915_xd_fake_set(I915_XD_PP_STATUS(0U), I915_XD_PP_STATUS_CYCLE_DELAY);
	i915_xd_fake_later(400000U, I915_XD_PP_STATUS(0U), I915_XD_PP_STATUS_CYCLE_DELAY, 0U);
	error = drv_i915_xd_pps_wait_cycle(&i915_xd_panel_test_io, 0U);
	i915_host_check(error == 0 && i915_xd_fake_now() >= 400000U && i915_xd_fake_now() < 401000U,
			"pps: the wait follows the sequencer's power cycle");
}

/*
 * The sequencer of the model: a power request puts the panel on and at
 * rest after the time context points to (in microseconds), a power off
 * puts it off at once.  The status is the control word's neighbour.
 */
static void
i915_xd_panel_test_pps_status(
	uint32_t offset,
	uint32_t value,
	void *context)
{
	const unsigned *power_up_us;
	uint32_t status;

	/* The status register of the same sequencer. */
	power_up_us = context;
	status = offset - (I915_XD_PP_CONTROL(0U) - I915_XD_PP_STATUS(0U));

	/* The panel follows the power bit. */
	if ((value & I915_XD_PP_CONTROL_POWER_ON) != 0U) {
		i915_xd_fake_later(*power_up_us, status, 0xffffffffU, I915_XD_PP_STATUS_ON | I915_XD_PP_STATUS_STATE_ON_IDLE);
	} else {
		i915_xd_fake_set(status, 0U);
	}
}

/* Counts the waits of the window that ran out. */
static unsigned
i915_xd_panel_test_timeouts(void)
{
	struct i915_xd_io_record records[I915_XD_IO_RECORDS];
	unsigned count;
	unsigned index;
	unsigned timeouts;

	/* Takes the record and counts its timeouts. */
	count = drv_i915_xd_io_records(&i915_xd_panel_test_io, records, I915_XD_IO_RECORDS);
	timeouts = 0U;
	for (index = 0U; index < count; index++) {
		if (records[index].kind == I915_XD_IO_RECORD_TIMEOUT)
			timeouts++;
	}

	/* Succeeded: the number of waits that ran out. */
	return timeouts;
}
