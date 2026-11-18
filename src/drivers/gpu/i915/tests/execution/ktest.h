/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The in-kernel unit tests of the driver's layers.
 *
 * The suite runs on the device's start worker, after the device has started
 * and before its node is served.  Most parts drive the code under test
 * against a register model or a recording stand-in and never touch the
 * hardware; the parts that do say so.  Each part adds its checks to one
 * shared tally, and the runner reports the tally as one PASS or FAIL line.
 */

#ifndef DRIVERS_GPU_I915_TESTS_EXECUTION_KTEST_H
#define DRIVERS_GPU_I915_TESTS_EXECUTION_KTEST_H

#include <stdint.h>

struct i915_device;

/*
 * The tally of one suite run.
 *
 * One instance lives on the runner's stack for the length of the run; every
 * part receives it and adds each check it makes.
 */
struct i915_ktest {
	/* The started device; parts that need no hardware leave it alone. */
	struct i915_device *device;

	/* How many checks ran. */
	unsigned checks;

	/* How many of them failed. */
	unsigned failures;

	/* How many checks were not run, each with a logged reason. */
	unsigned skipped;
};

void drv_i915_ktest_check(struct i915_ktest *ktest, int passed, const char *what);
void drv_i915_ktest_skip(struct i915_ktest *ktest, const char *what, const char *reason);
uint64_t drv_i915_ktest_deadline_ms(unsigned milliseconds);
int drv_i915_ktest_run(struct i915_device *device, struct i915_ktest *ktest);

void drv_i915_ktest_sync(struct i915_ktest *ktest);
void drv_i915_ktest_display(struct i915_ktest *ktest);
void drv_i915_ktest_display_probe(struct i915_ktest *ktest);
void drv_i915_ktest_gt(struct i915_ktest *ktest);

#endif
