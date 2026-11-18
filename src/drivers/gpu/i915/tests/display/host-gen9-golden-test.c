/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The golden scenes of the Gen9 panel's backlight that reach the
 * registers through the MMIO BAR (display/xd9-backlight.c): the start
 * on a lit panel of either PCH layout, the brightness read and set, and
 * the sleep's light off and back on.
 *
 * The register model (gen9-fake-hw.c) stands behind the BAR's stand-ins,
 * so every write lands in the model's log; a path on the command line
 * receives the log of each scene as the golden record.
 */

#include "host-test.h"
#include "gen9-fake-hw.h"

#include "../../display/xd9-backlight.h"
#include "../../display/internal.h"
#include "../../i915.h"
#include "../../device-info.h"
#include "gen9-fake-regs.h"

#include <kern/backlight.h>

#include <stdio.h>
#include <string.h>

static void i915_golden_backlight(int cnp);
static void i915_golden_panel(int cnp);

/*
 * The device the backlight is started on.
 *
 * Filled by each scene for the Gen9 part it stands for; the test runs on
 * one thread.
 */
static struct i915_device i915_golden_device;

/* The display of the device, emptied with it by each scene. */
static struct i915_display i915_golden_display;

/*
 * Runs the golden scenes of the backlight.
 */
int
main(
	int argc,
	char **argv)
{
	int status;
	int error;

	/* A path on the command line receives the golden record of the writes. */
	if (argc > 1) {
		error = i915_gen9_fake_golden_open(argv[1]);
		if (error != 0) {
			fprintf(stderr, "%s: cannot write %s: %d\n", "gen9_golden_host_test", argv[1], error);
			return 2;
		}
	}

	/* The BAR reaches the model for the whole run. */
	i915_host_mmio = &i915_gen9_fake_mmio;

	/* Sunrise Point's layout, then Cannon Point's. */
	i915_golden_backlight(0);
	i915_golden_backlight(1);

	/* The last scene goes to the record. */
	i915_gen9_fake_golden_close();
	i915_host_mmio = NULL;

	/* The tally. */
	status = i915_host_report("gen9_golden_host_test");
	if (status != 0)
		return status;

	/* Succeeded: every scene ran. */
	return 0;
}

/* Runs the backlight's scenes on one PCH layout. */
static void
i915_golden_backlight(
	int cnp)
{
	const struct kern_backlight_ops *ops;
	void *context;
	uint32_t percent;
	int error;

	/* The start on a lit panel publishes the device. */
	i915_golden_panel(cnp);
	drv_i915_xd9_backlight_start(&i915_golden_device);
	ops = i915_host_backlight_ops;
	context = i915_host_backlight_context;
	i915_host_check(ops != NULL, "backlight: published on a lit panel");
	if (ops == NULL)
		return;

	/* The brightness as the firmware left it, then a set to each end and the middle. */
	i915_golden_panel(cnp);
	error = ops->get(context, &percent);
	i915_host_check(error == 0, "backlight: the firmware's brightness reads");
	error = ops->set(context, 0U);
	i915_host_check(error == 0, "backlight: set to 0 %");
	error = ops->set(context, 50U);
	i915_host_check(error == 0, "backlight: set to 50 %");
	error = ops->set(context, 100U);
	i915_host_check(error == 0, "backlight: set to 100 %");

	/* The sleep turns the light off and the resume back on. */
	i915_golden_panel(cnp);
	drv_i915_xd9_backlight_suspend(&i915_golden_device);
	drv_i915_xd9_backlight_resume(&i915_golden_device);
	i915_host_check(i915_gen9_fake.log_count == 2U, "backlight: the sleep's two writes");

	/* The stop withdraws the device. */
	drv_i915_xd9_backlight_stop(&i915_golden_device);
	i915_host_check(i915_host_backlight_ops == NULL, "backlight: withdrawn");
}

/*
 * Empties the model and lights the eDP panel as the firmware leaves it:
 * power well 1 up, the eDP transcoder's DDI function on, and the PWM
 * running at the PCH layout's period and duty.
 */
static void
i915_golden_panel(
	int cnp)
{
	/* A new scene on an empty model. */
	i915_gen9_fake_reset();

	/* The device: a Kaby Lake on Sunrise Point, or a Coffee Lake on Cannon Point. */
	memset(&i915_golden_device, 0, sizeof(i915_golden_device));
	memset(&i915_golden_display, 0, sizeof(i915_golden_display));
	i915_golden_device.display = &i915_golden_display;
	i915_golden_device.gt.graphics_ver = 9U;
	i915_golden_device.gt.info.platform = I915_PLATFORM_KABYLAKE;
	i915_golden_device.product = 0x5917U;
	if (cnp) {
		i915_golden_device.gt.info.platform = I915_PLATFORM_COFFEELAKE;
		i915_golden_device.product = 0x3e92U;
	}

	/* The panel is lit. */
	i915_gen9_fake_set(I915_SKL_PWR_WELL_CTL2, 0xf0000000U);
	i915_gen9_fake_set(I915_SKL_TRANS_DDI_FUNC_CTL_EDP, I915_SKL_TRANS_DDI_FUNC_ENABLE);

	/* The PWM: CTL2's period 7500 and duty 3000, or Cannon Point's own registers. */
	if (cnp) {
		i915_gen9_fake_set(I915_CNP_BLC_PWM_CTL1, 0x80000000U);
		i915_gen9_fake_set(I915_CNP_BLC_PWM_FREQ1, 40000U);
		i915_gen9_fake_set(I915_CNP_BLC_PWM_DUTY1, 20000U);
	} else {
		i915_gen9_fake_set(I915_SKL_BLC_PWM_PCH_CTL1, 0x80000000U);
		i915_gen9_fake_set(I915_SKL_BLC_PWM_PCH_CTL2, (7500U << 16) | 3000U);
	}
}
