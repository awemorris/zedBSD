/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Device runtime power management (see runtime-pm.h).
 *
 * Two sets of operations are provided.  The device operations back the
 * driver's own runtime-PM structure; during probe runtime PM is never
 * enabled, so they never have to resume or suspend anything.  The PCI-probe
 * operations back the usage reference the PCI core takes around probe;
 * their resume is a real D0 transition.
 */

#include "i915.h"
#include "pci.h"
#include "runtime-pm.h"
#include "trace.h"

#include <stddef.h>

static void i915_rpm_note(struct i915_rpm *rpm, uint16_t op, const char *what, uint64_t argument0, uint64_t argument1);
static int i915_rpm_resume(struct i915_rpm *rpm);
static int i915_rpm_device_resume(void *context);
static void i915_rpm_device_suspend(void *context);
static int i915_rpm_pci_probe_resume(void *context);
static void i915_rpm_pci_probe_suspend(void *context);

/*
 * Initializes the runtime-PM structure; autosuspend stays off.
 *
 * Autosuspend stays off until drv_i915_rpm_enable(); the device is taken to
 * be resumed, because the PCI enable has already brought it to D0.
 */
void
drv_i915_rpm_init_early(
	struct i915_rpm *rpm,
	const struct i915_rpm_ops *ops,
	void *context,
	struct i915_trace *trace)
{
	/* Binds the device access. */
	rpm->ops = ops;
	rpm->context = context;
	rpm->trace = trace;

	/* Starts with no user. */
	rpm->usage_count = 0;

	/* The device is powered during probe: the PCI enable moved it to D0. */
	rpm->active = 1;

	/* Autosuspend is off until the end of driver load, so probe never suspends. */
	rpm->enabled = 0;

	/* Starts the diagnostic counters. */
	rpm->resumes = 0U;
	rpm->suspends = 0U;
	i915_rpm_note(rpm, I915_TRACE_NOTE, "rpm_init_early", 0U, 0U);
}

/*
 * Turns autosuspend on, at the end of driver load.
 */
void
drv_i915_rpm_enable(
	struct i915_rpm *rpm)
{
	/* From now on the last put lets the device suspend. */
	rpm->enabled = 1;
	i915_rpm_note(rpm, I915_TRACE_NOTE, "rpm_enable", 0U, 0U);
}

/*
 * Reports nonzero once autosuspend is enabled.
 */
int
drv_i915_rpm_is_enabled(
	const struct i915_rpm *rpm)
{
	/* Reports the autosuspend state. */
	return rpm->enabled;
}

/*
 * Takes one usage reference without resuming the device.
 */
void
drv_i915_rpm_get_noresume(
	struct i915_rpm *rpm)
{
	/* One more user keeps the device from suspending. */
	rpm->usage_count++;
	i915_rpm_note(rpm, I915_TRACE_ACQUIRE, "rpm_get_noresume", (uint64_t)rpm->usage_count, 0U);
}

/*
 * Takes one usage reference and resumes the device.
 *
 * Returns 0 or a positive errno.  On failure the usage reference is still
 * held, as pm_runtime_get_sync() leaves it, and the caller must put it.
 */
int
drv_i915_rpm_get_sync(
	struct i915_rpm *rpm)
{
	int error;

	/*
	 * The reference is counted before the resume, and a failed resume does
	 * not take it back.
	 */
	rpm->usage_count++;
	error = i915_rpm_resume(rpm);
	if (error != 0) {
		i915_rpm_note(rpm, I915_TRACE_FAIL, "rpm_get_sync", (uint64_t)error, (uint64_t)rpm->usage_count);
		return error;
	}

	i915_rpm_note(rpm, I915_TRACE_ACQUIRE, "rpm_get_sync", (uint64_t)rpm->usage_count, 0U);

	/* Succeeded: the device is resumed and the caller holds a reference. */
	return 0;
}

/*
 * Takes one usage reference and resumes the device, giving the reference back on failure.
 *
 * Returns 0 or a positive errno.  On failure no reference is held and the
 * caller must not put, as with pm_runtime_resume_and_get().
 */
int
drv_i915_rpm_resume_and_get(
	struct i915_rpm *rpm)
{
	int error;

	/* Counts the reference before the resume and takes it back if the resume fails. */
	rpm->usage_count++;
	error = i915_rpm_resume(rpm);
	if (error != 0) {
		rpm->usage_count--;
		i915_rpm_note(rpm, I915_TRACE_FAIL, "rpm_resume_and_get", (uint64_t)error, (uint64_t)rpm->usage_count);
		return error;
	}

	i915_rpm_note(rpm, I915_TRACE_ACQUIRE, "rpm_resume_and_get", (uint64_t)rpm->usage_count, 0U);

	/* Succeeded: the device is resumed and the caller holds a reference. */
	return 0;
}

/*
 * Drops one usage reference, suspending the device after the last one once enabled.
 *
 * While autosuspend is off -- the whole of probe -- the device never suspends.
 */
void
drv_i915_rpm_put(
	struct i915_rpm *rpm)
{
	/* Drops the reference; a put without a reference leaves the count at zero. */
	if (rpm->usage_count > 0)
		rpm->usage_count--;
	i915_rpm_note(rpm, I915_TRACE_RELEASE, "rpm_put", (uint64_t)rpm->usage_count, 0U);

	/* Before the end of driver load the device stays resumed. */
	if (rpm->enabled == 0)
		return;

	/* A remaining user keeps the device resumed. */
	if (rpm->usage_count != 0)
		return;

	/* A device already suspended has nothing left to suspend. */
	if (rpm->active == 0)
		return;

	/* Lets the idle device suspend. */
	if (rpm->ops->suspend != NULL)
		rpm->ops->suspend(rpm->context);
	rpm->active = 0;
	rpm->suspends++;
}

/*
 * Reports the usage count.
 */
int
drv_i915_rpm_usage(
	const struct i915_rpm *rpm)
{
	/* Reports how many users hold the device resumed. */
	return rpm->usage_count;
}

/*
 * Reports nonzero while the device is resumed.
 */
int
drv_i915_rpm_active(
	const struct i915_rpm *rpm)
{
	/* Reports the resume state. */
	return rpm->active;
}

/*
 * Returns the operations behind the driver's own runtime-PM structure.
 *
 * Runtime PM is never enabled during the device start, so these never have
 * a transition to make: the resume succeeds and the suspend does nothing.
 */
const struct i915_rpm_ops *
drv_i915_rpm_device_ops(void)
{
	static const struct i915_rpm_ops ops = {
		i915_rpm_device_resume,
		i915_rpm_device_suspend
	};

	/* Succeeded: the operations need no context. */
	return &ops;
}

/*
 * Returns the operations behind the PCI core's probe reference.
 *
 * The context those operations receive is the device's struct i915_pci; the
 * resume moves the device to D0 and the suspend to D3hot.
 */
const struct i915_rpm_ops *
drv_i915_rpm_pci_probe_ops(void)
{
	static const struct i915_rpm_ops ops = {
		i915_rpm_pci_probe_resume,
		i915_rpm_pci_probe_suspend
	};

	/* Succeeded: the operations drive the PCI power state. */
	return &ops;
}

/* Records a runtime-PM event when the device has a trace. */
static void
i915_rpm_note(
	struct i915_rpm *rpm,
	uint16_t op,
	const char *what,
	uint64_t argument0,
	uint64_t argument1)
{
	/* A runtime-PM structure without a trace records nothing. */
	if (rpm->trace == NULL)
		return;

	drv_i915_trace_record(rpm->trace, 0U, op, what, argument0, argument1);
}

/* Resumes a suspended device and counts the resume. */
static int
i915_rpm_resume(
	struct i915_rpm *rpm)
{
	int error;

	/* A resumed device needs nothing. */
	if (rpm->active != 0)
		return 0;

	/* Asks the device to resume. */
	error = rpm->ops->resume(rpm->context);
	if (error != 0)
		return error;

	/* The device is resumed. */
	rpm->active = 1;
	rpm->resumes++;

	/* Succeeded: the device is in D0. */
	return 0;
}

/* Resumes the device for the driver's structure; nothing to do during the device start. */
static int
i915_rpm_device_resume(
	void *context)
{
	UNUSED_PARAMETER(context);

	/* Succeeded: runtime PM is not enabled, so the device never left D0. */
	return 0;
}

/* Suspends the device for the driver's structure; nothing to do during the device start. */
static void
i915_rpm_device_suspend(
	void *context)
{
	UNUSED_PARAMETER(context);
}

/* Moves the device to D0 for the PCI core's probe reference. */
static int
i915_rpm_pci_probe_resume(
	void *context)
{
	int error;

	/*
	 * A real PMCSR write when the device has a power-management capability,
	 * and a legitimate no-op when it has none; the power-state change runs
	 * either way.
	 */
	error = drv_i915_pci_set_power_state(context, I915_PCI_D0);
	if (error != 0)
		return error;

	/* Succeeded: the device is in D0. */
	return 0;
}

/* Moves the device to D3hot for the PCI core's probe reference. */
static void
i915_rpm_pci_probe_suspend(
	void *context)
{
	/* The power-state change cannot fail in a way the suspend could report. */
	(void)drv_i915_pci_set_power_state(context, I915_PCI_D3HOT);
}
