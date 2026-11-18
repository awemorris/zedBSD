/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the Gen9 backlight's arithmetic (display/xd-panel.c, the display 9 PWM): the decode of the PCH's PWM registers in both layouts,
 * and the mapping between a brightness and a duty both ways.
 *
 *   sh run-gen9-backlight-host-test.sh
 *
 * The node operations need the GPU and are not reached; the link drops
 * them.
 */

#include "host-test.h"

#include "../../display/xd-panel.h"

#include <uapi/errno.h>

#include <stdio.h>
#include <string.h>

/* The PWM's words as a test gives them, and whether they are in Cannon Point's layout. */
struct i915_host_pwm_words {
	int cnp;
	uint32_t control;
	uint32_t period;
	uint32_t duty;
};

static int i915_host_decode(const struct i915_host_pwm_words *words, struct i915_xd9_pwm *pwm);
static void i915_host_pwm_decode(void);
static void i915_host_pwm_mapping(void);

/*
 * Checks the Gen9 backlight's arithmetic.
 */
int
main(void)
{
	int status;

	/* The decode, and the mapping. */
	i915_host_pwm_decode();
	i915_host_pwm_mapping();

	/* The tally. */
	status = i915_host_report("gen9_backlight_host_test");
	if (status != 0)
		return status;

	/* Succeeded: the arithmetic is the one xd-panel.c describes. */
	return 0;
}

/*
 * Reports the decode of the PWM's words in their layout: Sunrise Point's
 * packed one, or Cannon Point's (cnp).
 */
static int
i915_host_decode(
	const struct i915_host_pwm_words *words,
	struct i915_xd9_pwm *pwm)
{
	/* Succeeded: the xd code's decode. */
	return drv_i915_xd9_pwm_decode(words->control, words->period, words->duty, !words->cnp, pwm);
}

/* Checks the decode of a Sunrise Point and a Cannon Point PWM, and the refusals. */
static void
i915_host_pwm_decode(void)
{
	struct i915_host_pwm_words registers;
	struct i915_xd9_pwm pwm;
	int error;

	/* Sunrise Point: on, period 0x1d4c (7500) and duty 0x0bb8 (3000) in CTL2. */
	memset(&registers, 0, sizeof(registers));
	registers.control = 0x80000000U;
	registers.period = (0x1d4cU << 16) | 0x0bb8U;
	error = i915_host_decode(&registers, &pwm);
	i915_host_check(error == 0 && pwm.running == 1 && pwm.active_low == 0, "spt: on");
	i915_host_check(pwm.period == 7500U && pwm.duty == 3000U, "spt: period 7500, duty 3000");
	i915_host_check(pwm.floor == 118U, "spt: lowest duty 4/255 of 7500, rounded up");

	/* Cannon Point: on and active low, period and duty in their own registers. */
	memset(&registers, 0, sizeof(registers));
	registers.cnp = 1;
	registers.control = 0xa0000000U;
	registers.period = 0x00009c40U;
	registers.duty = 0x00004e20U;
	error = i915_host_decode(&registers, &pwm);
	i915_host_check(error == 0 && pwm.running == 1 && pwm.active_low == 1, "cnp: on, active low");
	i915_host_check(pwm.period == 40000U && pwm.duty == 20000U, "cnp: period 40000, duty 20000");

	/* A PWM that is off still decodes, as off. */
	memset(&registers, 0, sizeof(registers));
	registers.period = (100U << 16) | 50U;
	error = i915_host_decode(&registers, &pwm);
	i915_host_check(error == 0 && pwm.running == 0, "spt: off decodes as off");

	/* A period of zero is refused in both layouts. */
	memset(&registers, 0, sizeof(registers));
	registers.control = 0x80000000U;
	registers.period = 0x0000ffffU;
	error = i915_host_decode(&registers, &pwm);
	i915_host_check(error == EINVAL, "spt: zero period refused");
	registers.cnp = 1;
	registers.period = 0U;
	registers.duty = 5U;
	error = i915_host_decode(&registers, &pwm);
	i915_host_check(error == EINVAL, "cnp: zero period refused");
}

/* Checks the brightness to duty mapping and back. */
static void
i915_host_pwm_mapping(void)
{
	struct i915_host_pwm_words registers;
	struct i915_xd9_pwm pwm;
	uint32_t percent;
	uint32_t duty;
	uint32_t back;
	uint32_t worst;
	int error;

	/* The Sunrise Point PWM of period 7500 (lowest duty 118). */
	memset(&registers, 0, sizeof(registers));
	registers.control = 0x80000000U;
	registers.period = (7500U << 16) | 3000U;
	error = i915_host_decode(&registers, &pwm);
	i915_host_check(error == 0, "mapping: decoded");

	/* The ends and the middle. */
	i915_host_check(drv_i915_xd9_pwm_duty(&pwm, 0U) == 118U, "0 % is the lowest duty, not dark");
	i915_host_check(drv_i915_xd9_pwm_duty(&pwm, 100U) == 7500U, "100 % is the period");
	i915_host_check(drv_i915_xd9_pwm_duty(&pwm, 150U) == 7500U, "above 100 % is full");
	i915_host_check(drv_i915_xd9_pwm_duty(&pwm, 50U) == 3809U, "50 % is halfway above the lowest duty");

	/* The inverse at the ends and beyond them. */
	i915_host_check(drv_i915_xd9_pwm_percent(&pwm, 0U) == 0U, "duty 0 is 0 %");
	i915_host_check(drv_i915_xd9_pwm_percent(&pwm, 118U) == 0U, "the lowest duty is 0 %");
	i915_host_check(drv_i915_xd9_pwm_percent(&pwm, 7500U) == 100U, "the period is 100 %");
	i915_host_check(drv_i915_xd9_pwm_percent(&pwm, 9000U) == 100U, "above the period is 100 %");
	i915_host_check(drv_i915_xd9_pwm_percent(&pwm, 3000U) == 39U, "the firmware's duty 3000 is 39 %");

	/* Every brightness comes back as itself. */
	worst = 0U;
	for (percent = 0U; percent <= 100U; percent++) {
		duty = drv_i915_xd9_pwm_duty(&pwm, percent);
		back = drv_i915_xd9_pwm_percent(&pwm, duty);
		if (back != percent)
			worst++;
	}

	/* None off. */
	i915_host_check(worst == 0U, "every brightness round-trips");

	/* A short period (100) still round-trips and keeps its lowest duty above zero. */
	registers.period = (100U << 16) | 0U;
	error = i915_host_decode(&registers, &pwm);
	i915_host_check(error == 0 && pwm.floor == 2U, "period 100: lowest duty 2");
	worst = 0U;
	for (percent = 0U; percent <= 100U; percent++) {
		duty = drv_i915_xd9_pwm_duty(&pwm, percent);
		if (duty < pwm.floor || duty > pwm.period)
			worst++;
	}

	/* None outside. */
	i915_host_check(worst == 0U, "period 100: every duty within the lowest and the period");
}
