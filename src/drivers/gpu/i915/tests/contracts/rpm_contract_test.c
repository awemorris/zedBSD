/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The device runtime-PM contract, checked on the host.
 *
 * Runs runtime-pm.c against the mock device: the early initialization is
 * not the enable, a put during probe never suspends, and get_sync and
 * resume_and_get differ in what a failed resume leaves in the usage count.
 */

#include "contract.h"
#include "mock_rpm.h"

#include "../../runtime-pm.h"
#include "../../trace.h"

#include <errno.h>

static void rpm_check_probe(struct i915_rpm *rpm, struct mock_rpm *mock);
static void rpm_check_enable(struct i915_rpm *rpm, struct mock_rpm *mock);
static void rpm_check_resume_failures(struct i915_rpm *rpm, struct mock_rpm *mock);

/*
 * Runs the runtime-PM contract checks.
 */
int
main(void)
{
	static struct i915_trace trace;
	static struct i915_rpm rpm;
	static struct mock_rpm mock;
	int status;

	contract_begin("runtime-PM contract tests (GPU-free)");

	/* Binds runtime PM to a mock device whose resumes succeed. */
	drv_i915_trace_init(&trace);
	mock_rpm_reset(&mock);
	drv_i915_rpm_init_early(&rpm, mock_rpm_ops(), &mock, &trace);

	/* Runs each contract group in the order the old suite ran them. */
	rpm_check_probe(&rpm, &mock);
	rpm_check_enable(&rpm, &mock);
	rpm_check_resume_failures(&rpm, &mock);

	/* Reports whether every check held. */
	status = contract_end();
	if (status != 0)
		return status;

	/* Succeeded: runtime PM keeps its contract. */
	return 0;
}

/* Checks the state after the early initialization and during probe. */
static void
rpm_check_probe(
	struct i915_rpm *rpm,
	struct mock_rpm *mock)
{
	int usage;
	int active;
	int enabled;
	int error;

	contract_section("init: init_early initialises struct; autosuspend NOT enabled");

	/* The device starts resumed, unused, and without autosuspend. */
	usage = drv_i915_rpm_usage(rpm);
	contract_check(usage == 0, "usage starts 0");
	active = drv_i915_rpm_active(rpm);
	contract_check(active != 0, "device active during probe");
	enabled = drv_i915_rpm_is_enabled(rpm);
	contract_check(enabled == 0, "runtime PM not enabled yet (that is driver-load end)");

	contract_section("probe: put during probe does not suspend (autosuspend off)");

	/* A get and a put move the count and nothing else. */
	drv_i915_rpm_get_noresume(rpm);
	usage = drv_i915_rpm_usage(rpm);
	contract_check(usage == 1, "usage 1 after get");
	drv_i915_rpm_put(rpm);
	usage = drv_i915_rpm_usage(rpm);
	contract_check(usage == 0, "usage 0 after put");

	/* The idle device stays resumed because autosuspend is off. */
	active = drv_i915_rpm_active(rpm);
	contract_check(active != 0, "still active (probe never suspends)");
	contract_check(mock->suspend_calls == 0, "no suspend during probe");

	contract_section("get_sync: while active only bumps the count");

	/* A resumed device is not resumed again. */
	error = drv_i915_rpm_get_sync(rpm);
	contract_check(error == 0, "get_sync ok");
	contract_check(mock->resume_calls == 0, "no resume needed (already active)");
	drv_i915_rpm_put(rpm);
}

/* Checks that the enable lets the last put suspend the device. */
static void
rpm_check_enable(
	struct i915_rpm *rpm,
	struct mock_rpm *mock)
{
	int active;
	int enabled;

	contract_section("enable: after enable, an idle put suspends");

	/* Turns autosuspend on, as the end of driver load does. */
	drv_i915_rpm_enable(rpm);
	enabled = drv_i915_rpm_is_enabled(rpm);
	contract_check(enabled != 0, "enabled");

	/* The last put of an enabled device suspends it once. */
	drv_i915_rpm_get_noresume(rpm);
	drv_i915_rpm_put(rpm);
	active = drv_i915_rpm_active(rpm);
	contract_check(active == 0, "suspended when enabled + idle");
	contract_check(mock->suspend_calls == 1, "one suspend");
}

/* Checks the usage count a failed resume leaves behind, and the recovery. */
static void
rpm_check_resume_failures(
	struct i915_rpm *rpm,
	struct mock_rpm *mock)
{
	int before;
	int usage;
	int active;
	int error;

	contract_section("get_sync: resume failure leaves usage incremented (caller must put)");

	/* A failed get_sync keeps its reference, which the caller must put. */
	before = drv_i915_rpm_usage(rpm);
	mock->resume_fail = 1;
	error = drv_i915_rpm_get_sync(rpm);
	contract_check(error == EIO, "get_sync returns the resume errno");
	usage = drv_i915_rpm_usage(rpm);
	contract_check(usage == before + 1, "usage NOT unwound on get_sync failure");
	active = drv_i915_rpm_active(rpm);
	contract_check(active == 0, "device still suspended");

	/* Balances the reference the failed get_sync left. */
	drv_i915_rpm_put(rpm);

	contract_section("resume_and_get: resume failure unwinds usage (caller must not put)");

	/* A failed resume_and_get gives its reference back itself. */
	before = drv_i915_rpm_usage(rpm);
	error = drv_i915_rpm_resume_and_get(rpm);
	contract_check(error == EIO, "resume_and_get returns the resume errno");
	usage = drv_i915_rpm_usage(rpm);
	contract_check(usage == before, "usage unwound on resume_and_get failure");

	contract_section("resume: a successful resume_and_get brings the device back");

	/* Once the device resumes it is active again. */
	mock->resume_fail = 0;
	error = drv_i915_rpm_resume_and_get(rpm);
	contract_check(error == 0, "resume_and_get ok");
	active = drv_i915_rpm_active(rpm);
	contract_check(active != 0, "device active again");
	contract_check(mock->resume_calls >= 1, "backend resume was called");
	drv_i915_rpm_put(rpm);
}
