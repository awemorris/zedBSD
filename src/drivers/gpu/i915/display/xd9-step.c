/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The step a display 9 node takes (see xd9-step.h).
 */

#include "xd9-step.h"

#include <kern/atomic.h>
#include <kern/klog.h>

#include <stddef.h>

/* The names of the steps, in their order. */
static const char *const i915_xd9_step_names[] = {
	"copy",
	"flip",
	"backlight",
	"modeset",
	"full"
};

/*
 * The highest step the node takes: full until a step fails, then the step
 * below the one that failed.  It only ever falls, by a compare and
 * exchange, since the binding, the panel's presentations and the external
 * displays may fail at the same time.  XXX: one device.
 */
static atomic_uint_t i915_xd9_step_ceiling = { I915_XD9_STEP_FULL };

/* Reports the name of a step for the log ("?" for one outside them). */
const char *
drv_i915_xd9_step_name(
	int step)
{
	/* A step outside the names. */
	if (step < 0 || (unsigned)step >= sizeof(i915_xd9_step_names) / sizeof(i915_xd9_step_names[0]))
		return "?";

	/* Succeeded: the name. */
	return i915_xd9_step_names[step];
}

/* Reports the step the node takes now: full, lowered below any step that failed. */
int
drv_i915_xd9_step(void)
{
	unsigned ceiling;

	/* The ceiling a failure may have lowered. */
	ceiling = atomic_load_acquire(&i915_xd9_step_ceiling);

	/* Succeeded: the step. */
	return (int)ceiling;
}

/*
 * Logs that a step failed and lowers the node's step below it.  Copy
 * cannot fail; a step a failure before already gave up changes nothing
 * and logs nothing.  Any context but an interrupt handler.
 */
void
drv_i915_xd9_step_fail(
	int step,
	const char *reason,
	int error)
{
	unsigned ceiling;
	unsigned below;
	int lowered;
	int used;

	/* Copy is the floor: the firmware's framebuffer is always there. */
	if (step <= I915_XD9_STEP_COPY)
		return;

	/* The ceiling below the failed step, unless it is that low already. */
	below = (unsigned)step - 1U;
	lowered = 0;
	ceiling = atomic_load_acquire(&i915_xd9_step_ceiling);
	while (below < ceiling) {
		lowered = atomic_compare_exchange(&i915_xd9_step_ceiling, &ceiling, below);
		if (lowered)
			break;
	}

	/* A step given up before. */
	if (!lowered)
		return;

	/* Succeeded: the line that says what failed, why, and what the node uses. */
	used = drv_i915_xd9_step();
	if (error != 0) {
		kern_logf("i915: xd9-display: %s failed (%s: %d); using %s\n", drv_i915_xd9_step_name(step), reason, error, drv_i915_xd9_step_name(used));
		return;
	}
	kern_logf("i915: xd9-display: %s failed (%s); using %s\n", drv_i915_xd9_step_name(step), reason, drv_i915_xd9_step_name(used));
}

/* Logs the step the node takes, once its display is bound. */
void
drv_i915_xd9_step_report(void)
{
	int step;

	/* The step taken. */
	step = drv_i915_xd9_step();

	/* Succeeded: the line. */
	kern_logf("i915: xd9-display: using %s\n", drv_i915_xd9_step_name(step));
}
