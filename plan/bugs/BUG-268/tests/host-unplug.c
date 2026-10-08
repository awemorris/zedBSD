/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * BUG-268: the host test of the window's end at an unplug.  When the
 * display the resident output was moved to is unplugged while it is lit,
 * the hotplug path marks the window to be left at once and wakes the
 * worker; the worker's looks (the hold's end, the top of its loop) see it,
 * held or not.  Another connector, the firmware's own output, or an output
 * not lit is not marked.
 */

#include "src/drivers/gpu/i915/tests/display/host-test.h"
#include "src/drivers/gpu/i915/display/internal.h"
#include "src/drivers/gpu/i915/display/present.h"
#include "src/drivers/gpu/i915/i915.h"

#include <string.h>

/* The worker's wakes. */
static unsigned wakes;

void drv_i915_worker_wake(struct i915_device *device);

/* Counts the worker's wakes (the worker is not built in). */
void
drv_i915_worker_wake(
	struct i915_device *device)
{
	(void)device;
	wakes++;
}

/*
 * Runs the checks.
 */
int
main(
	void)
{
	static struct i915_device device;
	static struct i915_display display;

	/* The firmware's output is the panel; the output was moved to the DP display of connector 3, lit. */
	memset(&device, 0, sizeof(device));
	memset(&display, 0, sizeof(display));
	device.display = &display;
	display.device = &device;
	display.gop_output.kind = I915_OUTPUT_KIND_PANEL;
	display.gop_output.has_connector = 1;
	display.gop_output.connector = 0U;
	display.output.kind = I915_OUTPUT_KIND_DP_EXT;
	display.output.has_connector = 1;
	display.output.connector = 3U;
	display.resident_up = 1;

	/* Nothing asked: the window goes on. */
	i915_host_check(drv_i915_present_unplug_pending(&device) == 0, "no unplug: the window goes on");
	i915_host_check(drv_i915_present_hold_over(&device) == 0, "no unplug, no hold: the window goes on");

	/* Another connector unplugged: nothing. */
	drv_i915_present_unplugged(&display, 2U);
	i915_host_check(display.window.unplugged == 0 && wakes == 0U, "another connector does not end the window");

	/* The moved output's connector unplugged: marked once, the worker woken. */
	drv_i915_present_unplugged(&display, 3U);
	i915_host_check(display.window.unplugged == 1 && wakes == 1U, "the moved output's unplug marks the window and wakes the worker");
	i915_host_check(drv_i915_present_unplug_pending(&device) == 1, "the worker's loop leaves the window");
	i915_host_check(drv_i915_present_hold_over(&device) == 1, "the worker's sleep leaves the window without a hold");
	display.window.holding = 1;
	display.window.hold_until = ~0ULL;
	i915_host_check(drv_i915_present_hold_over(&device) == 1, "a hold that goes on is left too");
	display.window.holding = 0;

	/* A second unplug event of the same: not woken again. */
	drv_i915_present_unplugged(&display, 3U);
	i915_host_check(wakes == 1U, "one wake for one unplug");

	/* The output not lit: not marked. */
	display.window.unplugged = 0;
	display.resident_up = 0;
	drv_i915_present_unplugged(&display, 3U);
	i915_host_check(display.window.unplugged == 0 && wakes == 1U, "an output not lit is not marked");

	/* The firmware's own output (not moved): not marked; its connector is not an external one here. */
	display.resident_up = 1;
	display.output = display.gop_output;
	drv_i915_present_unplugged(&display, 0U);
	i915_host_check(display.window.unplugged == 0 && wakes == 1U, "the firmware's output is not marked");

	/* The result. */
	return i915_host_report("BUG-268 unplug");
}
