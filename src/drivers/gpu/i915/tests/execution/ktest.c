/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The in-kernel unit test suite: the tally and the order of the parts.
 *
 * The parts live in their own files by the layer they exercise: the kernel
 * services the driver waits and queues work on, the display's data sources
 * and power, the display probe, and the GT.  This file keeps the tally they
 * share and runs them in the order the device start brings the layers up.
 */

#include "ktest.h"

#include <kern/clock.h>
#include <kern/klog.h>
#include <kern/sched.h>

#include <uapi/errno.h>
#include <stdint.h>

/*
 * Records the outcome of one check.
 *
 * A failed check is logged with what it expected; a passed one only counts.
 */
void
drv_i915_ktest_check(
	struct i915_ktest *ktest,
	int passed,
	const char *what)
{
	/* Counts the check. */
	ktest->checks++;

	/* Names a failed check in the log and counts the failure. */
	if (!passed) {
		kern_logf("i915: ktest FAIL: %s\n", what);
		ktest->failures++;
	}
}

/*
 * Records a check that was not run and why.
 *
 * A skip is neither a pass nor a failure: it is logged so that a run that
 * silently lost coverage can be told from one that did not.
 */
void
drv_i915_ktest_skip(
	struct i915_ktest *ktest,
	const char *what,
	const char *reason)
{
	/* Counts the skip and names it in the log. */
	ktest->skipped++;
	kern_logf("i915: ktest SKIP: %s (%s)\n", what, reason);
}

/*
 * Converts a timeout in milliseconds into a scheduler deadline.
 *
 * The deadline is at least one tick away, so a short timeout still waits.
 */
uint64_t
drv_i915_ktest_deadline_ms(
	unsigned milliseconds)
{
	uint64_t deadline;
	uint64_t now;

	/* Adds the timeout, rounded up to a whole tick, to the current tick. */
	deadline = 0U;
	now = sched_ticks();
	(void)kern_deadline_after(now, (uint64_t)milliseconds * KERN_CLOCK_HZ / 1000U + 1U, &deadline);

	/* Reports the deadline; zero when the addition would overflow. */
	return deadline;
}

/*
 * Runs every part of the suite on a started device.
 *
 * Fills the tally and returns 0 when no check failed, EIO otherwise.
 */
int
drv_i915_ktest_run(
	struct i915_device *device,
	struct i915_ktest *ktest)
{
	/* Starts an empty tally. */
	ktest->device = device;
	ktest->checks = 0U;
	ktest->failures = 0U;
	ktest->skipped = 0U;
	kern_logf("i915: ktest begin\n");

	/* The kernel services: completions, work queues, interrupts, timing, reset and PCODE. */
	drv_i915_ktest_sync(ktest);

	/* The display's data sources, power wells, clocks and firmware. */
	drv_i915_ktest_display(ktest);

	/* The display probe: the PCH, the interrupt masks, the outputs and the readout. */
	drv_i915_ktest_display_probe(ktest);

	/* The GT: fuses, workarounds, memory, contexts, requests and the test fixtures. */
	drv_i915_ktest_gt(ktest);

	kern_logf("i915: ktest: %u checks, %u failures, %u skipped\n",
	    ktest->checks,
	    ktest->failures,
	    ktest->skipped);

	/* Reports a failed check to the runner. */
	if (ktest->failures != 0U)
		return EIO;

	/* Succeeded: every check that ran passed. */
	return 0;
}
