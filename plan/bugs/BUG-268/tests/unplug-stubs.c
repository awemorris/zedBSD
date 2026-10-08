/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The kernel services the display sources name but the BUG-268 unplug
 * test does not reach (copied from plan/ws084/tests/native-decide-stubs.c
 * without the worker's wake, which the test counts itself).
 */

#include <stdio.h>
#include <stdlib.h>

#include <drivers/pci/pci.h>
#include <kern/boot.h>
#include <kern/sched.h>

#include "drivers/gpu/i915/worker.h"

static void stub_unreachable(const char *service) __attribute__((noreturn));

/*
 * Stands in for the scheduler's sleep.
 */
void
sched_sleep(
	uint64_t timeout_tick)
{
	(void)timeout_tick;

	/* The decision rules do not sleep. */
	stub_unreachable("sched_sleep");
}

/*
 * Stands in for the PCI device's address.
 */
void
drv_pci_device_address(
	const struct drv_pci_device *d,
	struct drv_pci_address *a)
{
	(void)d;
	(void)a;

	/* The decision rules do not name the device. */
	stub_unreachable("drv_pci_device_address");
}

/*
 * Stands in for the i915 worker's backlight call.
 */
int
drv_i915_worker_sync_backlight(
	struct i915_device *device,
	int op,
	uint32_t *value)
{
	(void)device;
	(void)op;
	(void)value;

	/* The decision rules do not touch the backlight. */
	stub_unreachable("drv_i915_worker_sync_backlight");
}

/*
 * Stands in for the boot parameters (display.mode= of the HDMI mode,
 * ws075-p012, reached since ws113-p011a's output preparation).
 */
const struct kern_boot_parameters *
kern_boot_parameters_current(
	void)
{
	/* The decision rules do not read the boot parameters. */
	stub_unreachable("kern_boot_parameters_current");
}

/*
 * Stands in for a boot parameter's value.
 */
const char *
kern_boot_parameters_value(
	const struct kern_boot_parameters *parameters,
	enum kern_boot_parameter_key key)
{
	(void)parameters;
	(void)key;

	/* The decision rules do not read the boot parameters. */
	stub_unreachable("kern_boot_parameters_value");
}

/*
 * Stands in for the display.mode= parser.
 */
int
kern_boot_display_mode_parse(
	const char *text,
	size_t length,
	uint32_t *width,
	uint32_t *height,
	uint32_t *refresh_hz)
{
	(void)text;
	(void)length;
	(void)width;
	(void)height;
	(void)refresh_hz;

	/* The decision rules do not pick an HDMI mode. */
	stub_unreachable("kern_boot_display_mode_parse");
}

/* Ends the test with the name of a service the tested path must not reach. */
static void
stub_unreachable(
	const char *service)
{
	/* The name, then the end. */
	fprintf(stderr, "BUG-268 unplug host test: %s reached\n", service);
	abort();
}
