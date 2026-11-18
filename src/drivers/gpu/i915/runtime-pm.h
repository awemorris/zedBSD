/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Device runtime power management.
 *
 * This is the device runtime-PM layer only, and only the part the device
 * start needs: the early initialization, the usage count taken and dropped
 * during probe, and the cleanup.  It is kept apart from the two other power
 * concerns -- forcewake (mmio.h) and the display power domains and GT
 * wakerefs -- so they are never merged into one "power get".
 *
 * Two distinctions are kept:
 *
 *   - the early initialization only fills the structure; it is not the
 *     enable, which turns autosuspend on at the end of driver load.  During probe runtime PM
 *     is not enabled, so the device stays resumed and a put never suspends it.
 *   - get_sync and resume_and_get differ on failure: get_sync leaves the
 *     usage count raised, so the caller must still put; resume_and_get drops
 *     it again, so the caller must not.
 *
 * The resume and suspend themselves are reached through an operations table,
 * so the same logic runs against a mock device in the host tests.
 */

#ifndef DRIVERS_GPU_I915_RUNTIME_PM_H
#define DRIVERS_GPU_I915_RUNTIME_PM_H

struct i915_trace;

/*
 * The device end of runtime power management.
 */
struct i915_rpm_ops {
	/* Brings the device to D0; returns 0 or a positive errno. */
	int (*resume)(void *context);

	/* Lets the device go to D3; called only when enabled and idle.  May be NULL. */
	void (*suspend)(void *context);
};

/*
 * The runtime power state of one device.
 *
 * It lives from the device start to the device stop and is used only by the
 * thread that runs them.
 */
struct i915_rpm {
	/* The device access and the context it is given. */
	const struct i915_rpm_ops *ops;
	void *context;

	/* Where the gets and puts are recorded; may be NULL. */
	struct i915_trace *trace;

	/*
	 * The pm_runtime usage count: how many users need the device resumed.
	 * The device may suspend only when it returns to zero while enabled.
	 */
	int usage_count;

	/* Nonzero while the device is resumed (D0). */
	int active;

	/* Nonzero once autosuspend is enabled at the end of driver load; zero during probe. */
	int enabled;

	/* How many resumes and suspends ran, for diagnostics. */
	unsigned resumes;
	unsigned suspends;
};

void drv_i915_rpm_init_early(struct i915_rpm *rpm, const struct i915_rpm_ops *ops, void *context, struct i915_trace *trace);
void drv_i915_rpm_enable(struct i915_rpm *rpm);
int drv_i915_rpm_is_enabled(const struct i915_rpm *rpm);

void drv_i915_rpm_get_noresume(struct i915_rpm *rpm);
int drv_i915_rpm_get_sync(struct i915_rpm *rpm);
int drv_i915_rpm_resume_and_get(struct i915_rpm *rpm);
void drv_i915_rpm_put(struct i915_rpm *rpm);

int drv_i915_rpm_usage(const struct i915_rpm *rpm);
int drv_i915_rpm_active(const struct i915_rpm *rpm);

const struct i915_rpm_ops *drv_i915_rpm_device_ops(void);
const struct i915_rpm_ops *drv_i915_rpm_pci_probe_ops(void);

#endif
